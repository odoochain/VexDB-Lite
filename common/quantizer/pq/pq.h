#ifndef PQ_H
#define PQ_H

#include "platform/platform_compat.h"
#include "quantizer/annkmeans.h"


inline void pq_set_param(uint32 dim, uint16 &m, uint16 &k)
{
    if (dim % 4u == 0) {
        m = dim / 4u;
    } else if (dim % 3u == 0) {
        m = dim / 3u;
    } else if (dim % 5u == 0) {
        m = dim / 5u;
    } else if (dim % 2u == 0) {
        m = dim / 2u;
    } else {
        m = dim;
    }
    k = 256u;
}

/** Product Quantizer. Implemented only for METRIC_L2 */
struct ProductQuantizer {
    size_t d;         ///< dimension size of the input vectors
    size_t code_size; ///< bytes per indexed vector
    size_t M;     ///< number of subquantizers
    size_t nbits; ///< number of bits per quantization index

    // values derived from the above
    size_t dsub;  ///< dimensionality of each subvector
    size_t ksub;  ///< number of centroids for each subquantizer

    ann_helper::fvec_L2sqr_ny_nearest_func _fvec_L2sqr_ny_nearest_func;
    ann_helper::fvec_ny_distance_func _fvec_ny_distance_func;
    ann_helper::distance_single_code_func _distance_single_code_func;
    ann_helper::distance_four_codes_func _distance_four_codes_func;

    /// Centroid table, size M * ksub * dsub.
    /// Layout: (M, ksub, dsub)
    float* centroids;

    size_t get_centroids_size() const { return d * ksub; }
    /// return the centroids associated with subvector m
    float* get_centroids(size_t m, size_t i) { return &centroids[(m * ksub + i) * dsub]; }
    const float* get_centroids(size_t m, size_t i) const { return &centroids[(m * ksub + i) * dsub]; }

    // Train each sub-quantizer via k-means.
    void train(FloatVectorArray samples, Metric metric, int parallelWorkers, int maintenanceWorkMem);
    void free_resourses();
    void set_basic_values(size_t dim, size_t m , size_t nbits_);
    void set_fvec_L2sqr_ny_nearest_func();
    void set_fvec_ny_distance_func(Metric metric);
    void set_dist_code_func();
    /// compute derived values when d, M and nbits have been set
    void set_derived_values();
    /// Define the centroids for subquantizer m
    void set_params(FloatVectorArray subcenters, int m);
    /// Quantize one vector with the product quantizer
    void compute_code(const float* x, uint8_t* code) const;
    float distance_to_code(const uint8_t* code, const float *distTable);
    void distance_to_four_code(const float* distTable,
                            // codes
                            const uint8_t* code0,
                            const uint8_t* code1,
                            const uint8_t* code2,
                            const uint8_t* code3,
                            // computed distances
                            float& result0,
                            float& result1,
                            float& result2,
                            float& result3);

    /** Compute distance table for one vector.
     *
     * The distance table for x = [x_0 x_1 .. x_(M-1)] is a M * ksub
     * matrix that contains
     *
     *   dist_table (m, j) = || x_m - c_(m, j)||^2
     *   for m = 0..M-1 and j = 0 .. ksub - 1
     *
     * where c_(m, j) is the centroid no j of sub-quantizer m.
     *
     * @param x         input vector size d
     * @param dist_table output table, size M * ksub
     */
    void compute_distance_table(const float* x, float* dist_table) const;
    // void compute_inner_prod_table(const float* x, float* dist_table) const;
};



#endif
