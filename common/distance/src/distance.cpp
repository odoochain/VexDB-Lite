#include <cmath>
#include <boost/preprocessor/repetition/repeat.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>
#include "platform/platform_compat.h"
#include "utils/lsyscache.h"
#include "distance/include/cblas_interface.h"
#include "distance/include/pq/pq_endecode.h"
#include "distance/include/distance_utils.h"
#include "distance/include/distance_dispatcher.h"
#include "data_type/halfutils.h"

static const Arch best_arch = ann_helper::get_best_arch();

Metric get_func_metric(Oid func_id)
{
    char *func_name = get_func_name(func_id);
    if (func_name == NULL) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("Could not get function name for OID %u", func_id)));
        return Metric::L2;
    }

    Metric result = Metric::L2;

    if (strcmp(func_name, "l2_distance") == 0 ||
        strcmp(func_name, "floatvector_l2_squared_distance") == 0 ||
        strcmp(func_name, "halfvector_l2_distance") == 0 ||
        strcmp(func_name, "halfvector_l2_squared_distance") == 0 ||
        strcmp(func_name, "int8vector_l2_distance") == 0 ||
        strcmp(func_name, "int8vector_l2_squared_distance") == 0) {
        result = Metric::L2;
    } else if (strcmp(func_name, "cosine_distance") == 0 ||
               strcmp(func_name, "halfvector_cosine_distance") == 0) {
        result = Metric::FAST_COSINE;
    } else if (strcmp(func_name, "inner_product") == 0 ||
               strcmp(func_name, "floatvector_negative_inner_product") == 0 ||
               strcmp(func_name, "halfvector_inner_product") == 0 ||
               strcmp(func_name, "halfvector_negative_inner_product") == 0 ||
               strcmp(func_name, "int8vector_inner_product") == 0 ||
               strcmp(func_name, "int8vector_negative_inner_product") == 0) {
        result = Metric::INNER_PRODUCT;
    } else if (strcmp(func_name, "floatvector_spherical_distance") == 0 ||
               strcmp(func_name, "halfvector_spherical_distance") == 0 ||
               strcmp(func_name, "int8vector_spherical_distance") == 0 ||
               strcmp(func_name, "int8vector_cosine_distance") == 0) {
        result = Metric::COSINE;
    } else {
        pfree(func_name);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("Unsupported distance function")));
    }

    pfree(func_name);
    return result;
}

#if COMPILER_SUPPORT_NEONV8
#define ISA_FUNC_CALL_NEONV8(arg, call) \
    case Arch::NEONV8:                  \
        call(NEONV8_FUNC(arg));
#else
#define ISA_FUNC_CALL_NEONV8(arg, call)
#endif

#if COMPILER_SUPPORT_SVEV8
#define ISA_FUNC_CALL_SVEV8(arg, call) \
    case Arch::SVEV8:                  \
        call(SVEV8_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVEV8(arg, call)
#endif

#if COMPILER_SUPPORT_SVE2V8
#define ISA_FUNC_CALL_SVE2V8(arg, call) \
    case Arch::SVE2V8:                  \
        call(SVE2V8_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVE2V8(arg, call)
#endif

#if COMPILER_SUPPORT_SMEV9
#define ISA_FUNC_CALL_SMEV9(arg, call) \
    case Arch::SMEV9:                  \
        call(SMEV9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SMEV9(arg, call)
#endif

#if COMPILER_SUPPORT_SME2V9
#define ISA_FUNC_CALL_SME2V9(arg, call) \
    case Arch::SME2V9:                  \
        call(SME2V9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SME2V9(arg, call)
#endif

#if COMPILER_SUPPORT_SSE
#define ISA_FUNC_CALL_SSE(arg, call) \
    case Arch::SSE:                  \
        call(SSE_FUNC(arg));
#else
#define ISA_FUNC_CALL_SSE(arg, call)
#endif

#if COMPILER_SUPPORT_AVX
#define ISA_FUNC_CALL_AVX(arg, call) \
    case Arch::AVX:                  \
        call(AVX_FUNC(arg));
#else
#define ISA_FUNC_CALL_AVX(arg, call)
#endif

#if COMPILER_SUPPORT_AVX512_EXTEND
#define ISA_FUNC_CALL_AVX512(arg, call) \
    case Arch::AVX512:                 \
        call(AVX512_FUNC(arg));
#else
#define ISA_FUNC_CALL_AVX512(arg, call)
#endif

#define ARCH_FUNC_CALL(arch, arg, call) \
    switch (arch) {                     \
        ISA_FUNC_CALL_NEONV8(arg, call) \
        ISA_FUNC_CALL_SVEV8(arg, call)  \
        ISA_FUNC_CALL_SVE2V8(arg, call) \
        ISA_FUNC_CALL_SMEV9(arg, call)  \
        ISA_FUNC_CALL_SME2V9(arg, call) \
        ISA_FUNC_CALL_SSE(arg, call)    \
        ISA_FUNC_CALL_AVX(arg, call)    \
        ISA_FUNC_CALL_AVX512(arg, call) \
        case Arch::GENERAL:             \
        default:                        \
            call(GENERAL_FUNC(arg));    \
    }

static void pairwise_distance_l2(const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out)
{
    uint32 one_size = std::max(x_size, y_size);
    float *ones = (float *)palloc(one_size * sizeof(float));
    std::fill_n(ones, one_size, 1.0f);
    cblas_sgemm_rnt(x_size, y_size, 1, 1.0f, x_norm, 1, ones, 1, 0.0f, out, y_size);
    cblas_sgemm_rnt(x_size, y_size, 1, 1.0f, ones, 1, y_norm, 1, 1.0f, out, y_size);
    cblas_sgemm_rnt(x_size, y_size, dim, -2.0f, x, dim, y, dim, 1.0f, out, y_size);
    pfree(ones);
}

static void pairwise_distance_dot(const float *x, const float *y, uint32 dim,
    uint32 x_size, uint32 y_size, float *out)
{
    cblas_sgemm_rnt(x_size, y_size, dim, -1.0f, x, dim, y, dim, 0.0f, out, y_size);
}

namespace ann_helper {
distance_func get_general_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE,
                   Metric::SPHERICAL, Metric::L2_SQRT, Metric::L2_NORM>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func get_aligned_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func_batch get_general_distance_batch_func2(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE,
                   Metric::SPHERICAL, Metric::L2_SQRT, Metric::L2_NORM>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func_batch {
                return std::decay_t<decltype(d)>::get_distance_batch2;
            });
}

distance_func_batch get_aligned_distance_batch_func2(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func_batch {
                return std::decay_t<decltype(d)>::get_distance_batch2;
            });
}

distance_func get_general_half_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::HALF>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::HALF, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}
distance_func get_aligned_half_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::HALF>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::HALF, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}
distance_func_batch get_general_half_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}
distance_func_batch get_aligned_half_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}

distance_func get_general_int8_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::INT8>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::INT8, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func get_aligned_int8_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::INT8>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::INT8, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func_batch get_general_int8_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}

distance_func_batch get_aligned_int8_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}

distance_func get_general_distance_func(Metric metric)
    { return get_general_distance_func(metric, 1); }
distance_func get_general_half_distance_func(Metric metric)
    { return get_general_half_distance_func(metric, 1); }
distance_func get_general_int8_distance_func(Metric metric)
    { return get_general_int8_distance_func(metric, 1); }

vector_preprocess_func get_vector_preprocess_func(Metric metric, DistPrecisionType type, uint16 dim)
{
    if (metric != Metric::FAST_COSINE) {
        return NULL;
    }
    return DispatchRunner<false,
        MetricList<Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
        DistPrecisionTypeList<
                DistPrecisionType::FLOAT,
                DistPrecisionType::HALF>,
            DispatcherMode::NO_QUANT>::call(Metric::FAST_COSINE, type, dim, QuantizerType::NONE,
            [](auto &d) -> vector_preprocess_func {
                using tr = typename std::decay_t<decltype(d)>::template transform_type<TransformOp::NORMALIZE>;
                return [](const void *x, uint16 dim, void *out) {
                    tr::transform_single(x, x, out, dim);
                };
            });
}

float_to_half_func get_float_to_half_func()
{
#define DISTANCER_ARCH_ARG float_to_half
#define DISTANCER_ARCH_CALL(n) return n
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

half_to_float_func get_half_to_float_func()
{
#define DISTANCER_ARCH_ARG half_to_float
#define DISTANCER_ARCH_CALL(n) return n
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

fvec_ny_distance_func get_fvec_ny_distance_func(Metric Metric)
{
    switch (Metric) {
        case Metric::L2:
#define DISTANCER_ARCH_ARG fvec_L2sqr_ny
#define DISTANCER_ARCH_CALL(fvec_ny) return fvec_ny
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case Metric::INNER_PRODUCT:
#define DISTANCER_ARCH_ARG  fvec_inner_products_ny
#define DISTANCER_ARCH_CALL(fvec_ny) return fvec_ny
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
        default:
            __builtin_unreachable();
    }
}

fvec_L2sqr_ny_nearest_func get_fvec_L2sqr_ny_nearest_func()
{
#define DISTANCER_ARCH_ARG fvec_L2sqr_ny_nearest
#define DISTANCER_ARCH_CALL(nearest) return nearest
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
        ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

distance_single_code_func get_distance_single_code_func(uint32 nbits)
{
    switch (nbits) {
        case 8:
#define DISTANCER_ARCH_ARG distance_single_code_8
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case 16:
#define DISTANCER_ARCH_ARG distance_single_code_16
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
        default:
#define DISTANCER_ARCH_ARG distance_single_code_g
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
    }
}

distance_four_codes_func get_distance_four_codes_func(uint32 nbits)
{
    switch (nbits) {
        case 8:
#define DISTANCER_ARCH_ARG distance_four_codes_8
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case 16:
#define DISTANCER_ARCH_ARG distance_four_codes_16
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
        default:
#define DISTANCER_ARCH_ARG distance_four_codes_g
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL)
            ;
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL  
            break;
    }
}

fht_func get_fht_func(uint32 bottom_log_dim)
{
#define FHT_HELPER(z, i, func) case i: return func##i;
#define DISTANCER_ARCH_ARG fht_helper_
#define DISTANCER_ARCH_CALL(fht)    \
    switch (bottom_log_dim) { \
        BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(max_vector_bottom_dim, 1), FHT_HELPER, fht)   \
    }   \
    return NULL

    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
#undef FHT_HELPER
}

ann_helper::flip_sign_func get_flip_sign()
{
#define DISTANCER_ARCH_ARG flip_sign
#define DISTANCER_ARCH_CALL(fs) return fs;
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

ann_helper::kacs_walk_func get_kacs_walk()
{
#define DISTANCER_ARCH_ARG kacs_walk
#define DISTANCER_ARCH_CALL(kw) return kw;
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

ann_helper::warmup_ip_x0_q_func get_warmup_ip_x0_q()
{
#define DISTANCER_ARCH_ARG warmup_ip_x0_q
#define DISTANCER_ARCH_CALL(wiq) return wiq;
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

ann_helper::ip_fxi_func get_ip_fxi()
{
#define DISTANCER_ARCH_ARG ip_fxi
#define DISTANCER_ARCH_CALL(ipf) return ipf;
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

ann_helper::mask_ip_x0_q_func get_mask_ip_x0_q()
{
#define DISTANCER_ARCH_ARG mask_ip_x0_q
#define DISTANCER_ARCH_CALL(miq) return miq;
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

void pairwise_distance(const Metric metric, const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out)
{
    if (metric == Metric::L2) {
        pairwise_distance_l2(x, y, x_norm, y_norm, dim, x_size, y_size, out);
    } else {
        pairwise_distance_dot(x, y, dim, x_size, y_size, out);
        if (metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
            for (uint32 i = 0; i != x_size * y_size; ++i) {
                out[i] /= x_norm[i / y_size] * y_norm[i % y_size];
            }
        }
    }
}
} /* namespace ann_helper */

uint32 get_aligned_dim(uint32 dim)
    { return (dim + vector_step_size - 1) / vector_step_size * vector_step_size; }

size_t get_aligned_vec_size(size_t vec_size)
{
    return (vec_size + ann_helper::vector_aligned_size - 1) /
        ann_helper::vector_aligned_size * ann_helper::vector_aligned_size;
}

float *alloc_floatvector(uint32 dim, size_t n)
{
    void *res = mem_align_alloc(ann_helper::vector_aligned_size, sizeof(float) * dim * n);
    return (float *)__builtin_assume_aligned(res, ann_helper::vector_aligned_size);
}

char *alloc_vector(size_t vec_size, size_t n)
{
    void *res = mem_align_alloc(ann_helper::vector_aligned_size, vec_size * n);
    return (char *)__builtin_assume_aligned(res, ann_helper::vector_aligned_size);
}

bool is_aligned(const void *ptr)
{
    return reinterpret_cast<uintptr_t>(ptr) % ann_helper::vector_aligned_size == 0;
}
