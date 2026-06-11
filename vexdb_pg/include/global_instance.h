#ifndef KNL_VARIABLE_H
#define KNL_VARIABLE_H

#include "distance/include/distance_func.h"
#include "utils/memutils.h"

typedef struct knl_g_annvec_context {
    ann_helper::distance_func l2_squared_distance;
    ann_helper::distance_func negative_inner_product;
    ann_helper::distance_func cosine_distance;
    ann_helper::distance_func half_l2_squared_distance;
    ann_helper::distance_func half_negative_inner_product;
    ann_helper::distance_func half_cosine_distance;
    ann_helper::distance_func int8_l2_squared_distance;
    ann_helper::distance_func int8_negative_inner_product;
    ann_helper::distance_func int8_cosine_distance;
    ann_helper::float_to_half_func float_to_half;
    ann_helper::half_to_float_func half_to_float;
    void *ann_cxt;
} knl_g_annvec_context;

/* Vector buffer manager context */
typedef struct knl_g_diskann_context {
    MemoryContext vec_buf_ctx;
    void *vec_buffer_mgr;
    size_t vector_buffers;
    bool enable_buffer_manager;
    int vector_buffer_workers;
    int vec_writer_nproc;
    int16 pool_offset_to_write;
    void **vec_writer_latch;
} knl_g_diskann_context;

struct GlobalInstance {
    knl_g_annvec_context annvec_cxt;
    knl_g_diskann_context diskann_cxt;
};

extern GlobalInstance g_instance;

#endif
