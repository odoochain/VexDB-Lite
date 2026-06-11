/**
 * Copyright ...
 * Distance calculation utilities.
 */

#ifndef DISKANN_UTILS_DISTANCE_H
#define DISKANN_UTILS_DISTANCE_H

#include <boost/preprocessor/seq.hpp>
#include <vtl/expr_helper>

#include "platform/platform_compat.h"
static_assert(__cplusplus >= 201402L, "ehei");
#include "distance/include/architecture_macro.h"
#include "distance/include/distance_func.h"
// #include "access/annvector/quantizer.h"

enum class Arch : uint16 {
    BOOST_PP_SEQ_ENUM(DISTANCER_ISAS)
};

#define METRIC_SEQ \
    ((L2, 0)) \
    ((COSINE, 1)) \
    ((INNER_PRODUCT, 2)) \
    ((FAST_COSINE, 4)) \
    ((SPHERICAL, 8)) \
    ((L2_SQRT, 9)) \
    ((L2_NORM, 10))

#define ENUM_ITEM(r, data, elem) \
    BOOST_PP_TUPLE_ELEM(0, elem) = BOOST_PP_TUPLE_ELEM(1, elem),
enum class Metric : uint32 {
    BOOST_PP_SEQ_FOR_EACH(ENUM_ITEM, _, METRIC_SEQ)
    CUSTOM = 11
};
#undef ENUM_ITEM

#define TRANSFORM_OP_SEQ (ADD)(SUB)(MUL_SCALAR)(NORMALIZE)

enum class TransformOp : uint8 {
    BOOST_PP_SEQ_ENUM(TRANSFORM_OP_SEQ)
};

#define DIST_PRECISION_TYPE_SEQ \
    ((FLOAT, 4)) \
    ((HALF, 2)) \
    ((INT8, 1))
#define ENUM_ITEM(r, data, elem) BOOST_PP_TUPLE_ELEM(0, elem),
enum class DistPrecisionType : uint8 {
    BOOST_PP_SEQ_FOR_EACH(ENUM_ITEM, _, DIST_PRECISION_TYPE_SEQ)
    CUSTOM
};
#undef ENUM_ITEM

#define ENUM_ITEM(r, data, elem) \
    case DistPrecisionType::BOOST_PP_TUPLE_ELEM(0, elem): \
        return BOOST_PP_TUPLE_ELEM(1, elem);
constexpr inline size_t get_dtype_size(DistPrecisionType dt)
{
    switch (dt) {
        BOOST_PP_SEQ_FOR_EACH(ENUM_ITEM, _, DIST_PRECISION_TYPE_SEQ)
        default:
            __builtin_unreachable();
    }
}
#undef ENUM_ITEM

/**
 * NoPartial: dim % (k * k_per_iter) == 0
 * NoTail:    dim % k_per_iter == 0
 */
#define REMAINDER_SITUATION_SEQ (Unknown)(NoPartial)(NoTail)
enum class RemainderSituation {
    BOOST_PP_SEQ_ENUM(REMAINDER_SITUATION_SEQ)
};

template <RemainderSituation... Rs>
struct RemainderSituationList {};

template <Metric... Ms>
struct MetricList {};

template <TransformOp... Ops>
struct TransformOpList {};

template <DistPrecisionType... Ds>
struct DistPrecisionTypeList {};

template <Arch arch, DistPrecisionType dt>
struct RemainderPatcher {
    using res_cats = std::conditional_t<
#if COMPILER_TARGET_X86_64
        arch == Arch::GENERAL || (arch == Arch::SSE && dt == DistPrecisionType::HALF),
#else
        arch == Arch::GENERAL,
#endif
        RemainderSituationList<RemainderSituation::Unknown>,
        RemainderSituationList<RemainderSituation::NoPartial, RemainderSituation::NoTail, RemainderSituation::Unknown>>;
    static RemainderSituation get_remainder_situation(uint16 dim);
};

extern Arch get_best_arch(Metric m, DistPrecisionType dt, uint16 dim);

template <Arch ar, TransformOp op, DistPrecisionType d, RemainderSituation r, bool a>
struct Transformer {
    static void transform_single(const void *x, const void *y, void *out, uint16 dim);
    static constexpr void destroy() {}

    static constexpr Arch arch = ar;
    static constexpr TransformOp operation = op;
    static constexpr DistPrecisionType dpt = d;
    static constexpr RemainderSituation rs = r;
    static constexpr bool aligned = a;
};

template <Arch ar, Metric m, DistPrecisionType d, RemainderSituation r, bool a>
struct Distancer {
    static constexpr bool has_estimation_func = false;
    static constexpr bool need_refine = false;
    static constexpr bool is_quantizer = false;
    static constexpr void prepare(Relation index, void *meta) {}
    static constexpr void process(const char *query, void *metap) {}
    static constexpr void process(const char *query) {}
    static constexpr void compute_code(float *query, char *code) {}
    static float get_distance_single(const void *x, const void *y, uint16 dim);
    static void get_distance_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out);
    static constexpr void destroy() {}

    static constexpr Arch arch = ar;
    static constexpr Metric metric = m;
    static constexpr DistPrecisionType dpt = d;
    static constexpr RemainderSituation rs = r;
    static constexpr bool aligned = a;

    template <TransformOp op>
    using transform_type = Transformer<
        arch,
        op,
        dpt == DistPrecisionType::INT8 ? DistPrecisionType::FLOAT : dpt,
        rs,
        aligned>;
};

inline void *transform_scalar_to_ptr(float v)
{
    union {
        float f;
        uint32 u;
    } cvt;
    cvt.f = v;
    return reinterpret_cast<void *>((uintptr_t)cvt.u);
}

inline void transform_int8_to_float(const int8 *src, float *dst, uint16 dim)
{
    for (uint16 i = dim; i > 0; --i) {
        uint16 j = i - 1;
        dst[j] = (float)src[j];
    }
}

inline void transform_float_to_int8(const float *src, int8 *dst, uint16 dim)
{
    auto transform_round_float_to_int8 = [](float v) -> int8 {
        int32 iv = (v >= 0.0f) ? (int32)(v + 0.5f) : (int32)(v - 0.5f);
        if (iv > 127) {
            iv = 127;
        } else if (iv < -128) {
            iv = -128;
        }
        return (int8)iv;
    };
    for (uint16 i = 0; i < dim; ++i) {
        dst[i] = transform_round_float_to_int8(src[i]);
    }
}

Metric get_func_metric(Oid func_id);

namespace ann_helper {
#ifdef __x86_64__
constexpr size_t vector_aligned_size = 64ul;
#define vector_step_size 16
#elif defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
/* NEON & SVE only require 16-aligned to reach best performance, but some SME case may beed 32 */
constexpr size_t vector_aligned_size = 32ul;
#define vector_step_size 8
#else
constexpr size_t vector_aligned_size = 64ul;
#define vector_step_size 16
#endif

/* use dim as input can help improve dist calculation efficiency, I guess... */
distance_func get_general_distance_func(Metric metric, uint32 dim);
distance_func get_aligned_distance_func(Metric metric, uint32 dim);
distance_func get_general_distance_func(Metric metric);
distance_func_batch get_general_distance_batch_func2(Metric metric, uint32 dim);
distance_func_batch get_aligned_distance_batch_func2(Metric metric, uint32 dim);

/* half vector related */
distance_func get_general_half_distance_func(Metric metric, uint32 dim);
distance_func get_aligned_half_distance_func(Metric metric, uint32 dim);
distance_func get_general_half_distance_func(Metric metric);
distance_func_batch get_general_half_distance_batch_func2(Metric metric, uint32 dim);
distance_func_batch get_aligned_half_distance_batch_func2(Metric metric, uint32 dim);
float_to_half_func get_float_to_half_func();
half_to_float_func get_half_to_float_func();

/* int8 vector related */
distance_func get_general_int8_distance_func(Metric metric, uint32 dim);
distance_func get_aligned_int8_distance_func(Metric metric, uint32 dim);
distance_func get_general_int8_distance_func(Metric metric);
distance_func_batch get_general_int8_distance_batch_func2(Metric metric, uint32 dim);
distance_func_batch get_aligned_int8_distance_batch_func2(Metric metric, uint32 dim);

vector_preprocess_func get_vector_preprocess_func(Metric metric, DistPrecisionType type, uint16 dim);

fvec_ny_distance_func get_fvec_ny_distance_func(Metric metric);
fvec_L2sqr_ny_nearest_func get_fvec_L2sqr_ny_nearest_func();
distance_single_code_func get_distance_single_code_func(uint32 nbits);
distance_four_codes_func get_distance_four_codes_func(uint32 nbits);
fht_func get_fht_func(uint32 bottom_log_dim);
flip_sign_func get_flip_sign();
kacs_walk_func get_kacs_walk();
warmup_ip_x0_q_func get_warmup_ip_x0_q();
ip_fxi_func get_ip_fxi();
mask_ip_x0_q_func get_mask_ip_x0_q();

/* we don't use func pointer here since it is not trivial */
void pairwise_distance(const Metric metric, const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out);

struct VecArchConfigResolved {
    uint16 float_l2_arch;
    uint16 float_ip_arch;
    uint16 float_cos_arch;
    uint16 half_l2_arch;
    uint16 half_ip_arch;
    uint16 half_cos_arch;
    uint16 int8_l2_arch;
    uint16 int8_ip_arch;
    uint16 int8_cos_arch;
};
void store_vec_arch_config(VecArchConfigResolved &cfg);
void apply_vec_arch_config(const VecArchConfigResolved &cfg);
} /* namespace ann_helper */

uint32 get_aligned_dim(uint32 dim);
size_t get_aligned_vec_size(size_t vec_size);
float *alloc_floatvector(uint32 dim, size_t n = 1);
char *alloc_vector(size_t vec_size, size_t n = 1);
inline void free_vector(void *vec) { mem_align_free(vec); }
bool is_aligned(const void *ptr);


#endif /* DISKANN_UTILS_DISTANCE_H */
