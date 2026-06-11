#include "quantizer/pq/pq.h"
#include "quantizer/annkmeans.h"
#include "distance/include/pq/pq_endecode.h"
#include <thread>
#include <vector>

using namespace ann_helper;

/*********************************************
 * PQ implementation
 *********************************************/

void ProductQuantizer::set_basic_values(size_t dim, size_t m , size_t nbits_) {
    d = dim;
    M = m;
    nbits = nbits_;

    set_derived_values();
}

void ProductQuantizer::set_fvec_L2sqr_ny_nearest_func()
{
   _fvec_L2sqr_ny_nearest_func = ann_helper::get_fvec_L2sqr_ny_nearest_func();
}

void ProductQuantizer::set_fvec_ny_distance_func(Metric metric)
{
    _fvec_ny_distance_func = ann_helper::get_fvec_ny_distance_func(metric);
}

void ProductQuantizer::set_dist_code_func()
{
    _distance_single_code_func = ann_helper::get_distance_single_code_func(nbits);
    _distance_four_codes_func = ann_helper::get_distance_four_codes_func(nbits);
}

void ProductQuantizer::free_resourses() {
    pfree(centroids);
}

void ProductQuantizer::set_derived_values() {
    // quite a few derived values
    if (d % M != 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("The dimension of the vector (%ld) should be a multiple of the number subquantizers (%ld)", d, M))); 
    }
    dsub = d / M;
    code_size = (nbits * M + 7) / 8;
    ksub = 1 << nbits;
    centroids = (float *)palloc(d * ksub * sizeof(float));
}

void ProductQuantizer::set_params(FloatVectorArray subcenters, int m) {

    for (size_t i = 0; i < ksub; ++i) {
        float* vector = FloatVectorArrayGet(subcenters, i);
        memcpy(get_centroids(m, i), vector, dsub * sizeof(float));
    }
}

void ProductQuantizer::train(FloatVectorArray samples, Metric metric, int parallelWorkers, int maintenanceWorkMem)
{
    size_t n = samples->length;
    int avgWorkMem = maintenanceWorkMem / std::min(parallelWorkers + 1, (int)M);

    auto task = [this, n, samples, avgWorkMem, metric](size_t m) {
        FloatVectorArray subvectors = FloatVectorArrayInit(n, dsub);
        float *subvector = (float *)palloc0(sizeof(float) * dsub);
        for (size_t j = 0; j < n; j++) {
            float *vector = FloatVectorArrayGet(samples, j);
            memcpy(subvector, &(vector[m * dsub]), sizeof(float) * dsub);
            FloatVectorArraySet(subvectors, j, subvector);
            subvectors->length++;
        }
        KMeans kmeans(subvectors, ksub, metric, true, avgWorkMem, 0);
        kmeans.train();
        set_params(kmeans.get_centroids(), m);
        kmeans.destroy();
        pfree(subvector);
        FloatVectorArrayFree(subvectors);
    };

    // Run sub-quantizer training in parallel with std::thread.
    // Each thread writes to its own subquantizer (index m), no shared state.
    // Note: safe in Duck path (palloc = malloc). In PG path, palloc is not
    // thread-safe, so callers should pass parallelWorkers = 0 for PG.
    if (parallelWorkers > 1) {
        std::vector<std::thread> threads;
        for (size_t m = 0; m < M; m++) {
            threads.emplace_back(task, m);
        }
        for (auto &th : threads)
            th.join();
    } else {
        for (size_t m = 0; m < M; m++)
            task(m);
    }
}

template <class PQEncoder>
void compute_code_generic(const ProductQuantizer& pq, const float* x, uint8_t* code) {
    float* distances = (float*)palloc(pq.ksub * sizeof(float));

    PQEncoder encoder(code, pq.nbits);
    for (size_t m = 0; m < pq.M; m++) {
        const float* xsub = x + m * pq.dsub;
        uint64_t idxm = 0;

        // the regular version
        idxm = pq._fvec_L2sqr_ny_nearest_func(
                distances,
                xsub,
                pq.get_centroids(m, 0),
                pq.dsub,
                pq.ksub);
        encoder.encode(idxm);
    }
    encoder.restore_code();
    pfree(distances);
}

void ProductQuantizer::compute_code(const float* x, uint8_t* code) const {
    switch (nbits) {
        case 8:
            compute_code_generic<PQEncoder8>(*this, x, code);
            break;

        case 16:
            compute_code_generic<PQEncoder16>(*this, x, code);
            break;

        default:
            compute_code_generic<PQEncoderGeneric>(*this, x, code);
            break;
    }
}

void ProductQuantizer::compute_distance_table(const float* x, float* dis_table) const {
    for (size_t m = 0; m < M; m++) {
        _fvec_ny_distance_func(dis_table + m * ksub,
                                x + m * dsub,
                                get_centroids(m, 0),
                                dsub,
                                ksub);
    }
}

float ProductQuantizer::distance_to_code(const uint8_t* code, const float *distTable) {
    return _distance_single_code_func(M, nbits, distTable, code);
}

void ProductQuantizer::distance_to_four_code(const float* distTable,
                            // codes
                            const uint8_t* code0,
                            const uint8_t* code1,
                            const uint8_t* code2,
                            const uint8_t* code3,
                            // computed distances
                            float& result0,
                            float& result1,
                            float& result2,
                            float& result3) {
    _distance_four_codes_func(M, nbits, distTable, code0, code1, code2, code3, result0, result1, result2, result3);
}


