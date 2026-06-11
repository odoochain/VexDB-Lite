// Elkan's accelerated k-means with k-means++ initialization.
// Ported from openGauss-vector annkmeans.cpp, replacing the upstream task-pool
// framework with std::thread.
//
// Platform-neutral — uses palloc/pfree/ereport via platform_compat.h.

#include "annkmeans.h"
#include "platform/platform_compat.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Internal state (kept for backward compat with static helpers below)
// ---------------------------------------------------------------------------
struct KmeansState {
    bool skipCheckDuplicate = false;
    Metric metric = Metric::L2;
    ann_helper::distance_func kmeansprocinfo = nullptr;
    ann_helper::distance_func kmeansnormprocinfo = nullptr;
};

// ---------------------------------------------------------------------------
// Deterministic RNG (thread-local for safety with std::thread)
// ---------------------------------------------------------------------------
static thread_local std::mt19937 g_rng{42};

static double RandDouble()
{
    std::uniform_real_distribution<double> d(0.0, 1.0);
    return d(g_rng);
}

static int RandInt()
{
    std::uniform_int_distribution<int> d(0, std::numeric_limits<int>::max() - 1);
    return d(g_rng);
}

// ---------------------------------------------------------------------------
// Apply norm to vector (spherical k-means)
// ---------------------------------------------------------------------------
static void ApplyNorm(ann_helper::distance_func normprocinfo, float *vec, int dim)
{
    double norm = normprocinfo(vec, vec, static_cast<uint16>(dim));
    if (norm > 0) {
        for (int i = 0; i < dim; i++)
            vec[i] /= static_cast<float>(norm);
    }
}

// ---------------------------------------------------------------------------
// Compare vectors for dedup sorting
// ---------------------------------------------------------------------------
static int CompareVectors(const float *a, const float *b, int dim)
{
    for (int i = 0; i < dim; i++) {
        if (a[i] < b[i])
            return -1;
        if (a[i] > b[i])
            return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Parallel helper — divides [0, totalItems) among threads, calls
// fn(batchIndex, start, end).  All memory must be pre-allocated before
// calling; threads only write to non-overlapping regions.
// ---------------------------------------------------------------------------
template <typename Fn>
static void ParallelRun(int numThreads, int64_t totalItems, Fn &&fn)
{
    if (totalItems <= 0)
        return;
    if (numThreads <= 1) {
        fn(0, 0, totalItems);
        return;
    }
    int64_t batchSize = (totalItems + numThreads - 1) / numThreads;
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; t++) {
        int64_t start = t * batchSize;
        int64_t end = std::min(start + batchSize, totalItems);
        if (start >= end)
            break;
        threads.emplace_back([&fn, t, start, end]() { fn(t, start, end); });
    }
    for (auto &th : threads)
        th.join();
}

// ---------------------------------------------------------------------------
// k-means++ initialization
//
// https://theory.stanford.edu/~sergei/papers/kMeansPP-soda.pdf
// ---------------------------------------------------------------------------
static void InitCenters(KmeansState *kmeanstate, FloatVectorArray samples,
                        FloatVectorArray centers, float *weight, float *lowerBound,
                        int totalWorkers)
{
    ann_helper::distance_func procinfo = kmeanstate->kmeansprocinfo;
    int numSamples = samples->length;
    int numCenters = centers->maxlen;

    // Per-thread partial sums (pre-allocated, written by distinct threads)
    auto *parallelSum = static_cast<double *>(palloc(sizeof(double) * totalWorkers));

    // Choose first center uniformly at random
    FloatVectorArraySet(centers, 0, FloatVectorArrayGet(samples, RandInt() % numSamples));
    centers->length = 1;

    for (int i = 0; i < numSamples; i++)
        weight[i] = FLT_MAX;

    for (int i = 0; i < numCenters; i++) {
        double sum = 0;
        for (int k = 0; k < totalWorkers; k++)
            parallelSum[k] = 0;

        ParallelRun(totalWorkers, numSamples,
            [&](int batchIndex, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    float *vec = FloatVectorArrayGet(samples, j);
                    double distance = procinfo(vec, FloatVectorArrayGet(centers, i),
                                               static_cast<uint16>(centers->dim));

                    if (lowerBound)
                        lowerBound[j * numCenters + i] = static_cast<float>(distance);

                    distance *= distance;

                    if (distance < weight[j])
                        weight[j] = static_cast<float>(distance);

                    parallelSum[batchIndex] += weight[j];
                }
            });

        for (int k = 0; k < totalWorkers; k++)
            sum += parallelSum[k];

        if (i + 1 >= numCenters)
            break;

        double choice = sum * RandDouble();
        int64_t picked = 0;
        for (int64_t j = 0; j < numSamples - 1; j++) {
            choice -= weight[j];
            if (choice <= 0) {
                picked = j;
                break;
            }
        }

        FloatVectorArraySet(centers, i + 1, FloatVectorArrayGet(samples, picked));
        centers->length++;
    }

    pfree(parallelSum);
}

// ---------------------------------------------------------------------------
// QuickCenters — when N <= K, copy unique samples, fill remaining with
// random data.
// ---------------------------------------------------------------------------
static void QuickCenters(KmeansState *kmeanstate, FloatVectorArray samples,
                         FloatVectorArray centers)
{
    ann_helper::distance_func normprocinfo = kmeanstate->kmeansnormprocinfo;
    int dimensions = centers->dim;

    if (samples->length > 0) {
        int N = samples->length;
        std::vector<int> idx(N);
        for (int i = 0; i < N; i++)
            idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](int a, int b) {
            return CompareVectors(FloatVectorArrayGet(samples, a),
                                  FloatVectorArrayGet(samples, b), dimensions) < 0;
        });

        for (int i = 0; i < N; i++) {
            float *vec = FloatVectorArrayGet(samples, idx[i]);
            if (i == 0 || CompareVectors(vec, FloatVectorArrayGet(samples, idx[i - 1]),
                                         dimensions) != 0) {
                FloatVectorArraySet(centers, centers->length, vec);
                centers->length++;
            }
        }
    }

    while (centers->length < centers->maxlen) {
        float *vec = FloatVectorArrayGet(centers, centers->length);
        for (int j = 0; j < dimensions; j++)
            vec[j] = static_cast<float>(RandDouble());

        if (normprocinfo)
            ApplyNorm(normprocinfo, vec, dimensions);

        centers->length++;
    }
}

// ---------------------------------------------------------------------------
// CheckCenters — validate center quality
// ---------------------------------------------------------------------------
static void CheckCenters(KmeansState *kmeanstate, FloatVectorArray centers)
{
    int dim = centers->dim;

    if (centers->length != centers->maxlen)
        ereport(ERROR, (errmsg("Not enough centers. Please report a bug.")));

    // Ensure no NaN or infinite values
    for (int i = 0; i < centers->length; i++) {
        float *vec = FloatVectorArrayGet(centers, i);
        for (int j = 0; j < dim; j++) {
            if (std::isnan(vec[j]))
                ereport(ERROR, (errmsg("NaN detected. Please report a bug.")));
            if (std::isinf(vec[j]))
                ereport(ERROR, (errmsg("Infinite value detected. Please report a bug.")));
        }
    }

    // Ensure no duplicate centers (sort by index to keep original order)
    if (!kmeanstate->skipCheckDuplicate) {
        int N = centers->length;
        std::vector<int> idx(N);
        for (int i = 0; i < N; i++)
            idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](int a, int b) {
            return CompareVectors(FloatVectorArrayGet(centers, a),
                                  FloatVectorArrayGet(centers, b), dim) < 0;
        });
        for (int i = 1; i < N; i++) {
            if (CompareVectors(FloatVectorArrayGet(centers, idx[i]),
                               FloatVectorArrayGet(centers, idx[i - 1]), dim) == 0)
                ereport(ERROR,
                        (errmsg("Duplicate centers detected. Please report a bug.")));
        }
    }

    // Ensure no zero vectors for cosine / spherical distance
    if (kmeanstate->metric == Metric::COSINE || kmeanstate->metric == Metric::FAST_COSINE) {
        ann_helper::distance_func norm_dist_func =
            ann_helper::get_general_distance_func(Metric::L2_NORM, dim);
        for (int i = 0; i < centers->length; i++) {
            double norm = norm_dist_func(FloatVectorArrayGet(centers, i),
                                         FloatVectorArrayGet(centers, i),
                                         static_cast<uint16>(centers->dim));
            if (norm == 0)
                ereport(ERROR,
                        (errmsg("Zero norm detected. For PQ, please specify parameter "
                                "by_residual to false and retry, other case please report "
                                "a bug.")));
        }
    }
}

// ---------------------------------------------------------------------------
// Elkan's accelerated k-means
//
// Uses triangle inequality to skip distance calculations.
// We use L2 distance for L2 (not L2 squared) and angular distance for
// inner product and cosine.
//
// https://www.aaai.org/Papers/ICML/2003/ICML03-022.pdf
// ---------------------------------------------------------------------------
static void ElkanKmeans(KmeansState *kmeanstate, FloatVectorArray samples,
                        FloatVectorArray centers, int avgMaintenanceWorkMem,
                        int totalWorkers)
{
    ann_helper::distance_func procinfo = kmeanstate->kmeansprocinfo;
    ann_helper::distance_func normprocinfo = kmeanstate->kmeansnormprocinfo;
    int dimensions = centers->dim;
    int numCenters = centers->maxlen;
    int numSamples = samples->length;

    // Calculate allocation sizes
    Size lowerBoundSize = sizeof(float) * static_cast<size_t>(numSamples) * numCenters;
    Size upperBoundSize = sizeof(float) * numSamples;
    Size closestCentersSize = sizeof(int) * numSamples;
    Size centerCountsSize = sizeof(int) * numCenters;
    Size sSize = sizeof(float) * numCenters;
    Size halfcdistSize = sizeof(float) * static_cast<size_t>(numCenters) * numCenters;
    Size newcdistSize = sizeof(float) * numCenters;
    Size newCentersBufSize = sizeof(float) * static_cast<size_t>(numCenters) * dimensions;

    Size totalSize = lowerBoundSize + upperBoundSize + closestCentersSize +
                     centerCountsSize + sSize + halfcdistSize + newcdistSize +
                     newCentersBufSize;

    if (avgMaintenanceWorkMem > 0 &&
        totalSize > static_cast<Size>(avgMaintenanceWorkMem) * 1024) {
        ereport(ERROR,
                (errmsg("memory required is %zu MB, average maintenance work memory is %d MB",
                        totalSize / (1024 * 1024) + 1,
                        avgMaintenanceWorkMem / 1024)));
    }

    if (static_cast<int64_t>(numCenters) * numCenters > INT_MAX)
        ereport(ERROR, (errmsg("Indexing overflow detected. Please report a bug.")));

    // Allocate all buffers upfront — safe for multi-threaded access
    auto *lowerBound = static_cast<float *>(palloc(lowerBoundSize));
    auto *upperBound = static_cast<float *>(palloc(upperBoundSize));
    auto *closestCenters = static_cast<int *>(palloc(closestCentersSize));
    auto *centerCounts = static_cast<int *>(palloc(centerCountsSize));
    auto *s = static_cast<float *>(palloc(sSize));
    auto *halfcdist = static_cast<float *>(palloc(halfcdistSize));
    auto *newcdist = static_cast<float *>(palloc(newcdistSize));
    auto *newCenters = static_cast<float *>(palloc(newCentersBufSize));

    // Per-thread accumulators for step 4b (no shared writes)
    auto *parCounts = static_cast<int *>(
        palloc(sizeof(int) * static_cast<size_t>(totalWorkers) * numCenters));
    auto *parCenters = static_cast<float *>(
        palloc(sizeof(float) * static_cast<size_t>(totalWorkers) * numCenters * dimensions));
    // Per-thread change flags for step 2-3
    auto *threadChanged = static_cast<bool *>(palloc(sizeof(bool) * totalWorkers));

    // k-means++ initialization
    auto *weight = static_cast<float *>(palloc(sizeof(float) * numSamples));
    InitCenters(kmeanstate, samples, centers, weight, lowerBound, totalWorkers);
    pfree(weight);

    // Assign each sample to its closest initial center (using lowerBound from
    // InitCenters)
    ParallelRun(totalWorkers, numSamples,
        [&](int /*batchIndex*/, int64_t start, int64_t end) {
            for (int64_t j = start; j < end; j++) {
                double minDistance = DBL_MAX;
                int closest = 0;
                for (int k = 0; k < numCenters; k++) {
                    double distance = lowerBound[j * numCenters + k];
                    if (distance < minDistance) {
                        minDistance = distance;
                        closest = k;
                    }
                }
                upperBound[j] = static_cast<float>(minDistance);
                closestCenters[j] = closest;
            }
        });

    // Main Elkan loop — give 500 iterations to converge
    for (int iteration = 0; iteration < 500; iteration++) {
        // Step 1a: For all centers, compute half inter-center distances
        ParallelRun(totalWorkers, numCenters,
            [&](int /*batchIndex*/, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    float *vec = FloatVectorArrayGet(centers, j);
                    for (int64_t k = j + 1; k < numCenters; k++) {
                        double distance = 0.5 * procinfo(vec, FloatVectorArrayGet(centers, k),
                                                         static_cast<uint16>(centers->dim));
                        halfcdist[j * numCenters + k] = static_cast<float>(distance);
                        halfcdist[k * numCenters + j] = static_cast<float>(distance);
                    }
                }
            });

        // Step 1b: For all centers c, compute s(c) = 0.5 * min distance to
        // other center
        ParallelRun(totalWorkers, numCenters,
            [&](int /*batchIndex*/, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    double minDistance = DBL_MAX;
                    for (int64_t k = 0; k < numCenters; k++) {
                        if (j == k)
                            continue;
                        double distance = halfcdist[j * numCenters + k];
                        if (distance < minDistance)
                            minDistance = distance;
                    }
                    s[j] = static_cast<float>(minDistance);
                }
            });

        // Step 2-3: Assign points using bounds
        bool rjreset = iteration != 0;
        memset(threadChanged, 0, sizeof(bool) * totalWorkers);

        ParallelRun(totalWorkers, numSamples,
            [&](int batchIndex, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    // Step 2: Skip if upper bound is tight enough
                    if (upperBound[j] <= s[closestCenters[j]])
                        continue;

                    bool rj = rjreset;

                    for (int k = 0; k < numCenters; k++) {
                        // Step 3: For all remaining points x and centers c
                        if (k == closestCenters[j])
                            continue;

                        if (upperBound[j] <= lowerBound[j * numCenters + k])
                            continue;

                        if (upperBound[j] <= halfcdist[closestCenters[j] * numCenters + k])
                            continue;

                        float *vec = FloatVectorArrayGet(samples, j);

                        // Step 3a: Tighten upper bound
                        double dxcx;
                        if (rj) {
                            dxcx = procinfo(vec, FloatVectorArrayGet(centers, closestCenters[j]),
                                            static_cast<uint16>(centers->dim));
                            lowerBound[j * numCenters + closestCenters[j]] =
                                static_cast<float>(dxcx);
                            upperBound[j] = static_cast<float>(dxcx);
                            rj = false;
                        } else {
                            dxcx = upperBound[j];
                        }

                        // Step 3b: Check if closer center exists
                        if (dxcx > lowerBound[j * numCenters + k] ||
                            dxcx > halfcdist[closestCenters[j] * numCenters + k]) {
                            double dxc = procinfo(vec, FloatVectorArrayGet(centers, k),
                                                  static_cast<uint16>(centers->dim));
                            lowerBound[j * numCenters + k] = static_cast<float>(dxc);

                            if (dxc < dxcx) {
                                closestCenters[j] = k;
                                upperBound[j] = static_cast<float>(dxc);
                                threadChanged[batchIndex] = true;
                            }
                        }
                    }
                }
            });

        // Step 4a: Clear per-thread accumulators
        memset(parCenters, 0,
               sizeof(float) * static_cast<size_t>(totalWorkers) * numCenters * dimensions);
        memset(parCounts, 0,
               sizeof(int) * static_cast<size_t>(totalWorkers) * numCenters);

        // Step 4b: Accumulate points to new centers (per-thread, no
        // contention)
        ParallelRun(totalWorkers, numSamples,
            [&](int batchIndex, int64_t start, int64_t end) {
                float *myCenters =
                    parCenters + static_cast<size_t>(batchIndex) * numCenters * dimensions;
                int *myCounts = parCounts + batchIndex * numCenters;

                for (int64_t j = start; j < end; j++) {
                    float *vec = FloatVectorArrayGet(samples, j);
                    int c = closestCenters[j];
                    float *acc = myCenters + static_cast<size_t>(c) * dimensions;
                    for (int d2 = 0; d2 < dimensions; d2++)
                        acc[d2] += vec[d2];
                    myCounts[c]++;
                }
            });

        // Reduce per-thread results into global buffers
        memset(newCenters, 0, newCentersBufSize);
        memset(centerCounts, 0, centerCountsSize);
        for (int t = 0; t < totalWorkers; t++) {
            float *myCenters =
                parCenters + static_cast<size_t>(t) * numCenters * dimensions;
            int *myCounts = parCounts + t * numCenters;
            for (int c = 0; c < numCenters; c++) {
                centerCounts[c] += myCounts[c];
                float *acc = newCenters + static_cast<size_t>(c) * dimensions;
                float *myAcc = myCenters + static_cast<size_t>(c) * dimensions;
                for (int d2 = 0; d2 < dimensions; d2++)
                    acc[d2] += myAcc[d2];
            }
        }

        // Step 4c: Compute new center positions
        ParallelRun(totalWorkers, numCenters,
            [&](int /*batchIndex*/, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    float *acc = newCenters + static_cast<size_t>(j) * dimensions;
                    if (centerCounts[j] > 0) {
                        for (int64_t k = 0; k < dimensions; k++) {
                            if (std::isinf(acc[k]))
                                acc[k] = acc[k] > 0 ? FLT_MAX : -FLT_MAX;
                            acc[k] /= centerCounts[j];
                        }
                    } else {
                        // Handle empty cluster — random reinit
                        for (int64_t k = 0; k < dimensions; k++)
                            acc[k] = static_cast<float>(RandDouble());
                    }

                    if (normprocinfo)
                        ApplyNorm(normprocinfo, acc, dimensions);
                }
            });

        // Step 5a: Compute distance between old and new centers
        ParallelRun(totalWorkers, numCenters,
            [&](int /*batchIndex*/, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    newcdist[j] = procinfo(FloatVectorArrayGet(centers, j),
                                           newCenters + static_cast<size_t>(j) * dimensions,
                                           static_cast<uint16>(centers->dim));
                }
            });

        // Step 5b: Update lower bounds
        ParallelRun(totalWorkers, numSamples,
            [&](int /*batchIndex*/, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    for (int k = 0; k < numCenters; k++) {
                        double distance = lowerBound[j * numCenters + k] - newcdist[k];
                        if (distance < 0)
                            distance = 0;
                        lowerBound[j * numCenters + k] = static_cast<float>(distance);
                    }
                }
            });

        // Step 6: Update upper bounds
        ParallelRun(totalWorkers, numSamples,
            [&](int /*batchIndex*/, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++)
                    upperBound[j] += newcdist[closestCenters[j]];
            });

        // Step 7: Move new centers to centers
        ParallelRun(totalWorkers, numCenters,
            [&](int /*batchIndex*/, int64_t start, int64_t end) {
                for (int64_t j = start; j < end; j++) {
                    memcpy(FloatVectorArrayGet(centers, j),
                           newCenters + static_cast<size_t>(j) * dimensions,
                           dimensions * sizeof(float));
                }
            });

        // Convergence check
        bool anyChanges = false;
        for (int t = 0; t < totalWorkers; t++) {
            if (threadChanged[t]) {
                anyChanges = true;
                break;
            }
        }
        if (!anyChanges && iteration != 0)
            break;
    }

    centers->length = numCenters;

    pfree(lowerBound);
    pfree(upperBound);
    pfree(closestCenters);
    pfree(centerCounts);
    pfree(s);
    pfree(halfcdist);
    pfree(newcdist);
    pfree(newCenters);
    pfree(parCounts);
    pfree(parCenters);
    pfree(threadChanged);
}

// ---------------------------------------------------------------------------
// KMeans class implementation
// ---------------------------------------------------------------------------

KMeans::KMeans(FloatVectorArray samples_, int cluster_num_, Metric metric,
               bool ispq, int work_mem_, int nthreads_)
    : samples(samples_),
      cluster_num(cluster_num_),
      work_mem(work_mem_),
      nthreads(nthreads_ > 0 ? nthreads_ + 1 : 1),
      metric(ispq ? Metric::L2 : metric)
{
    using ann_helper::get_general_distance_func;
    int dim = samples->dim;

    if (ispq) {
        // PQ codebook training — always L2_SQRT, no normalization.
        dist_func  = get_general_distance_func(Metric::L2_SQRT, dim);
        norm_func  = nullptr;
    } else {
        // Full-vector k-means (graph partition, RaBitQ clustering).
        if (metric == Metric::L2) {
            dist_func = get_general_distance_func(Metric::L2_SQRT, dim);
            norm_func = nullptr;
        } else if (metric == Metric::COSINE || metric == Metric::FAST_COSINE ||
                   metric == Metric::INNER_PRODUCT) {
            dist_func = get_general_distance_func(Metric::SPHERICAL, dim);
            norm_func = get_general_distance_func(Metric::L2_NORM, dim);
        } else {
            ereport(ERROR,
                    (errmsg("Distance Metric type(%d) is not handled during kmeans setup",
                            static_cast<int>(metric))));
        }
    }
}

void KMeans::train()
{
    int dim = samples->dim;

    // Allocate output centers
    centers = FloatVectorArrayInit(cluster_num, dim);

    if (samples->length == 0) {
        centers->length = 0;
        return;
    }

    if (samples->length <= cluster_num) {
        // Not enough samples — copy unique ones, fill rest with random data
        KmeansState st;
        st.kmeansnormprocinfo = norm_func;
        st.skipCheckDuplicate = true;
        QuickCenters(&st, samples, centers);
    } else {
        // Proper k-means with Elkan's algorithm
        KmeansState st;
        st.kmeansprocinfo     = dist_func;
        st.kmeansnormprocinfo = norm_func;
        st.metric             = static_cast<Metric>(0); // unused by Elkan
        st.skipCheckDuplicate = true;                   // Elkan doesn't check

        int avgWorkMem = work_mem / std::max(1, nthreads);
        ElkanKmeans(&st, samples, centers, avgWorkMem, nthreads);
    }

    // Validate
    KmeansState st;
    st.skipCheckDuplicate = (norm_func == nullptr);
    st.metric             = metric;
    CheckCenters(&st, centers);
}

void KMeans::destroy()
{
    if (centers) {
        FloatVectorArrayFree(centers);
        centers = nullptr;
    }
}
