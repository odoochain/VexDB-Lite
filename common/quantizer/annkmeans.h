// K-means clustering with k-means++ initialization and Elkan's accelerated
// algorithm.  Platform-agnostic — uses palloc/pfree/ereport via
// platform_compat.h.  Parallelism via std::thread.

#pragma once

#include "data_type/floatvector.h"
#include "distance/include/distance.h"
#include "distance/include/distance_func.h"

/// Lightweight k-means clusterer with k-means++ initialization and Elkan's
/// accelerated algorithm (triangle inequality pruning).
///
/// Usage:
///   KMeans kmeans(samples, K, metric, /*ispq=*/false, work_mem, nthreads);
///   kmeans.train();
///   FloatVectorArray centers = kmeans.get_centroids();
///   kmeans.destroy();
class KMeans {
public:
    /// @param samples     Input vectors (borrowed; must outlive this object)
    /// @param cluster_num Number of clusters to produce (K)
    /// @param metric      Distance metric (used when ispq=false)
    /// @param ispq        true = PQ sub-vector training (always L2_SQRT, no norm)
    /// @param work_mem    Per-call memory limit in KB (0 = no limit)
    /// @param nthreads    Parallel threads for Elkan's algorithm (1 = sequential)
    KMeans(FloatVectorArray samples, int cluster_num, Metric metric, bool ispq,
           int work_mem = 0, int nthreads = 1);

    /// Run k-means.  Allocates and computes `cluster_num` centroids.
    /// Uses Elkan's algorithm when samples > K, QuickCenters otherwise.
    void train();

    /// Result centroids (FloatVectorArray, maxlen == cluster_num).
    /// Valid only after train().
    FloatVectorArray get_centroids() { return centers; }

    /// Free internally allocated resources (centroids buffer).
    void destroy();

private:
    FloatVectorArray samples;
    int cluster_num;
    int work_mem;
    int nthreads;
    Metric metric;
    ann_helper::distance_func dist_func{nullptr};
    ann_helper::distance_func norm_func{nullptr};
    FloatVectorArray centers{nullptr};
};
