#ifndef PQ_DISTANCER_H
#define PQ_DISTANCER_H

#include "quantizer/pq/pq.h"

struct PQDistancer {
    static constexpr bool has_estimation_func = false;
    static constexpr bool need_refine = true;
    static constexpr bool is_quantizer = true;

    PQDistancer() : dist_table(NULL), prepared(false) {}
    void train(Relation index, FloatVectorArray samples, size_t dimension, Metric metric,
               bool need_norm, int parallel_workers, int maintenance_work_mem);
    void destroy();
    size_t code_size() { return pq.M; }
    void compute_code(float *vec, char *code) { pq.compute_code(vec, (uint8 *)code); }
    float get_distance_precise(const void *x, const void *y, uint16 dim) const
        { return _get_distance_precise_func(x, y, dim); }
    float get_distance_single(const void *x, const void *y, uint16 dim) const
        { return flag * pq.distance_to_code((const uint8 *)y, dist_table); }
    void get_distance_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out) const
    {
        const uint8 *const *code = (const uint8 * const *)y;
        uint16 i = 0;
        for (; i + 4 < y_size; i += 4) {
            pq.distance_to_four_code(dist_table, code[i], code[i + 1], code[i + 2], code[i + 3],
                                      out[i], out[i + 1], out[i + 2], out[i + 3]);
        }
        for (; i < y_size; ++i) {
            out[i] = pq.distance_to_code(code[i], dist_table);
        }
    }

    /* 以下函数需要再pg侧和duck测做出不同实现 
     * PG: plugin-workspace/VexDB-Lite/vexdb_pg/src/quantizer/pq_adapter.cpp
     * Duck:
     */
    void prepare(Relation index, void *metap);
    void process(const char *query, void *metap);
    void flush(Relation index, BlockNumber qtcode_block, bool enabling = false);
private:
    void hnsw_read_pq_center(Relation index, ProductQuantizer &pq, BlockNumber qtcode_block);
    void configure_for_metric(size_t d, size_t M, size_t nbits_, Metric metric,
                              DistPrecisionType precision = DistPrecisionType::FLOAT);
private:
    mutable ProductQuantizer pq;
    ann_helper::distance_func _get_distance_precise_func;
    float *dist_table;
    float flag;
    bool prepared;
};

#endif
