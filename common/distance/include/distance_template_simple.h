/**
 * Copyright ...
 */

#ifndef DISTANCE_TEMPLATE_SIMPLE_H
#define DISTANCE_TEMPLATE_SIMPLE_H

#include <math.h>
#include <vtl/array>

#include "distance/include/distance.h"

#define ASSUME_ALIGNED(v) v = (const plaint *)__builtin_assume_aligned(v, ann_helper::vector_aligned_size)

template <typename Policy>
struct DistanceHelper {
    using plaint = typename Policy::PlainT;
    using vect = typename Policy::VecT;
    using intm = typename Policy::IntmT;
    using acc = typename Policy::AccT;

    static constexpr uint16 k = Policy::k;
    static constexpr uint16 k_per_iter = Policy::k_per_iter;
    static constexpr uint16 k_step = k_per_iter * k;
    static constexpr uint16 k_bytes = sizeof(plaint) * k_step;
    static constexpr uint16 skip_step = 4;
    static constexpr uint16 cache_size = 64;
    static constexpr bool has_partial_tail = Policy::RS != RemainderSituation::NoPartial;
    static constexpr bool has_rem_tail = Policy::RS == RemainderSituation::Unknown;

    static_assert(k == 4 || !Policy::use_custom_code,
        "simple version template only supports k = 4 as it requires manual unrolling");

    static INLINE_PROP float l2_distance(const void *xx, const void *yy, uint16 dim)
    {
        const plaint *x = (const plaint *)xx;
        const plaint *y = (const plaint *)yy;

        CONSTEXPR_IF (Policy::use_asm_code) {
            return Policy::asm_code(x, y, dim);
        } else CONSTEXPR_IF (!Policy::use_custom_code) {
            intm sum = 0;
            CONSTEXPR_IF (Policy::is_aligned) {
                ASSUME_ALIGNED(x);
                ASSUME_ALIGNED(y);
            }
            for (uint16 i = 0; i != dim; ++i) {
                const intm diff = Policy::transform(x[i]) - Policy::transform(y[i]);
                sum += diff * diff;
            }
            return sum;
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;
            Array<acc, k> accs;
            accs[0] = Policy::get_zero_vectors();
            accs[1] = Policy::get_zero_vectors();
            accs[2] = Policy::get_zero_vectors();
            accs[3] = Policy::get_zero_vectors();

            for (uint16 i = 0; i < n_full; ++i) {
                if (i >= skip_step && (!has_partial_tail || i + skip_step < n_full)) {
                    constexpr uint16 pf = k_bytes / cache_size;
                    for (uint16 j = 0; j < pf; ++j) {
                        unpolluted_prefetch((char *)y + j * cache_size + skip_step * k_bytes);
                    }
                }
                Array<vect, k> xs = Policy::template loadk<Policy::is_aligned>(x);
                Array<vect, k> ys = Policy::template loadk<Policy::is_aligned>(y);
                vect diff0 = Policy::sub_vectors(xs[0], ys[0]);
                vect diff1 = Policy::sub_vectors(xs[1], ys[1]);
                vect diff2 = Policy::sub_vectors(xs[2], ys[2]);
                vect diff3 = Policy::sub_vectors(xs[3], ys[3]);
                accs[0] = Policy::madd_vectors(diff0, diff0, accs[0]);
                accs[1] = Policy::madd_vectors(diff1, diff1, accs[1]);
                accs[2] = Policy::madd_vectors(diff2, diff2, accs[2]);
                accs[3] = Policy::madd_vectors(diff3, diff3, accs[3]);
                x += k_step;
                y += k_step;
            }

            CONSTEXPR_IF (has_partial_tail) {
                for (uint16 j = 0; j < n_tail; ++j) {
                    vect x0 = Policy::template load<Policy::is_aligned>(x + k_per_iter * j);
                    vect y0 = Policy::template load<Policy::is_aligned>(y + k_per_iter * j);
                    vect diff = Policy::sub_vectors(x0, y0);
                    accs[j] = Policy::madd_vectors(diff, diff, accs[j]);
                }
            }

            CONSTEXPR_IF (has_rem_tail && Policy::rem_before_reduce) {
                accs[k - 1] = Policy::deal_remainder(x, y, dim, n_tail, accs[k - 1]);
            }
            float res = Policy::reduce(accs);
            CONSTEXPR_IF (has_rem_tail && !Policy::rem_before_reduce) {
                res += Policy::deal_remainder(x, y, dim, n_tail);
            }
            return res;
        }
        __builtin_unreachable();
    }

    static INLINE_PROP float cosine_distance(const void *xx, const void *yy, uint16 dim)
    {
        const plaint *x = (const plaint *)xx;
        const plaint *y = (const plaint *)yy;

        CONSTEXPR_IF (Policy::use_asm_code) {
            return Policy::asm_code(x, y, dim);
        } else CONSTEXPR_IF (!Policy::use_custom_code) {
            intm dot = 0;
            intm norm_x = 0;
            intm norm_y = 0;
            CONSTEXPR_IF (Policy::is_aligned) {
                ASSUME_ALIGNED(x);
                ASSUME_ALIGNED(y);
            }
            for (uint16 i = 0; i < dim; i++) {
                intm xf = Policy::transform(x[i]);
                intm yf = Policy::transform(y[i]);
                dot += xf * yf;
                norm_x += xf * xf;
                norm_y += yf * yf;
            }
            return -dot / (sqrtf(norm_x * norm_y) + __FLT_EPSILON__);
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;

            Array<acc, k> accs;
            Array<acc, k> nxs;
            Array<acc, k> nys;
            for (uint16 j = 0; j < k; ++j) {
                accs[j] = Policy::get_zero_vectors();
                nxs[j] = Policy::get_zero_vectors();
                nys[j] = Policy::get_zero_vectors();
            }

            for (uint16 i = 0; i < n_full; ++i) {
                if (i >= skip_step && (!has_partial_tail || i + skip_step < n_full)) {
                    constexpr uint16 pf = k_bytes / cache_size;
                    for (uint16 j = 0; j < pf; ++j) {
                        unpolluted_prefetch((char *)y + j * cache_size + skip_step * k_bytes);
                    }
                }
                Array<vect, k> xs = Policy::template loadk<Policy::is_aligned>(x);
                Array<vect, k> ys = Policy::template loadk<Policy::is_aligned>(y);
                accs[0] = Policy::madd_vectors(xs[0], ys[0], accs[0]);
                nxs[0] = Policy::madd_square(xs[0], nxs[0]);
                nys[0] = Policy::madd_square(ys[0], nys[0]);
                accs[1] = Policy::madd_vectors(xs[1], ys[1], accs[1]);
                nxs[1] = Policy::madd_square(xs[1], nxs[1]);
                nys[1] = Policy::madd_square(ys[1], nys[1]);
                accs[2] = Policy::madd_vectors(xs[2], ys[2], accs[2]);
                nxs[2] = Policy::madd_square(xs[2], nxs[2]);
                nys[2] = Policy::madd_square(ys[2], nys[2]);
                accs[3] = Policy::madd_vectors(xs[3], ys[3], accs[3]);
                nxs[3] = Policy::madd_square(xs[3], nxs[3]);
                nys[3] = Policy::madd_square(ys[3], nys[3]);
                x += k_step;
                y += k_step;
            }

            CONSTEXPR_IF (has_partial_tail) {
                for (uint16 j = 0; j < n_tail; ++j) {
                    vect x0 = Policy::template load<Policy::is_aligned>(x + k_per_iter * j);
                    vect y0 = Policy::template load<Policy::is_aligned>(y + k_per_iter * j);
                    accs[j] = Policy::madd_vectors(x0, y0, accs[j]);
                    nxs[j] = Policy::madd_square(x0, nxs[j]);
                    nys[j] = Policy::madd_square(y0, nys[j]);
                }
            }

            CONSTEXPR_IF (has_rem_tail && Policy::rem_before_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    accs[k - 1] = Policy::deal_remainder(x, y, dim, n_tail, accs[k - 1]);
                    nxs[k - 1] = Policy::deal_remainder(x, x, dim, n_tail, nxs[k - 1]);
                    nys[k - 1] = Policy::deal_remainder(y, y, dim, n_tail, nys[k - 1]);
                }
            }
            float res = Policy::reduce(accs);
            float norm_x = Policy::reduce(nxs);
            float norm_y = Policy::reduce(nys);
            CONSTEXPR_IF (has_rem_tail && Policy::rem_after_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    res += Policy::deal_remainder(x, y, dim, n_tail);
                    norm_x += Policy::deal_remainder(x, x, dim, n_tail);
                    norm_y += Policy::deal_remainder(y, y, dim, n_tail);
                }
            }
            return -res / (sqrtf(norm_x * norm_y) + __FLT_EPSILON__);
        }
        __builtin_unreachable();
    }

    static INLINE_PROP float ip_distance(const void *xx, const void *yy, uint16 dim)
    {
        const plaint *x = (const plaint *)xx;
        const plaint *y = (const plaint *)yy;

        CONSTEXPR_IF (Policy::use_asm_code) {
            return Policy::asm_code(x, y, dim);
        } else CONSTEXPR_IF (!Policy::use_custom_code) {
            intm distance = 0;
            CONSTEXPR_IF (Policy::is_aligned) {
                ASSUME_ALIGNED(x);
                ASSUME_ALIGNED(y);
            }
            for (uint16 i = 0; i < dim; ++i) {
                intm xi = Policy::transform(x[i]);
                intm yi = Policy::transform(y[i]);
                distance -= xi * yi;
            }
            return distance;
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;
            Array<acc, k> accs;
            accs[0] = Policy::get_zero_vectors();
            accs[1] = Policy::get_zero_vectors();
            accs[2] = Policy::get_zero_vectors();
            accs[3] = Policy::get_zero_vectors();

            for (uint16 i = 0; i < n_full; ++i) {
                if (i >= skip_step && (!has_partial_tail || i + skip_step < n_full)) {
                    constexpr uint16 pf = k_bytes / cache_size;
                    for (uint16 j = 0; j < pf; ++j) {
                        unpolluted_prefetch((char *)y + j * cache_size + skip_step * k_bytes);
                    }
                }
                Array<vect, k> xs = Policy::template loadk<Policy::is_aligned>(x);
                Array<vect, k> ys = Policy::template loadk<Policy::is_aligned>(y);
                accs[0] = Policy::madd_vectors(xs[0], ys[0], accs[0]);
                accs[1] = Policy::madd_vectors(xs[1], ys[1], accs[1]);
                accs[2] = Policy::madd_vectors(xs[2], ys[2], accs[2]);
                accs[3] = Policy::madd_vectors(xs[3], ys[3], accs[3]);
                x += k_step;
                y += k_step;
            }

            CONSTEXPR_IF (has_partial_tail) {
                for (uint16 j = 0; j < n_tail; ++j) {
                    vect x0 = Policy::template load<Policy::is_aligned>(x + k_per_iter * j);
                    vect y0 = Policy::template load<Policy::is_aligned>(y + k_per_iter * j);
                    accs[j] = Policy::madd_vectors(x0, y0, accs[j]);
                }
            }

            CONSTEXPR_IF (has_rem_tail && Policy::rem_before_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    accs[k - 1] = Policy::deal_remainder(x, y, dim, n_tail, accs[k - 1]);
                }
            }
            float res = Policy::reduce(accs);
            CONSTEXPR_IF (has_rem_tail && !Policy::rem_before_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    res += Policy::deal_remainder(x, y, dim, n_tail);
                }
            }
            return -res;
        }
        __builtin_unreachable();
    }

    static INLINE_PROP float sphere_distance(const void *xx, const void *yy, uint16 dim)
    {
        float dist = -ip_distance(xx, yy, dim);
        if (dist > 1) {
            dist = 1;
        } else if (dist < -1) {
            dist = -1;
        }
        return acos(dist) / M_PI;
    }

    static INLINE_PROP float l2_sqrt_distance(const void *xx, const void *yy, uint16 dim)
    {
        float dist = l2_distance(xx, yy, dim);
        return sqrtf(dist);
    }

    static INLINE_PROP float l2_norm_distance(const void *xx, const void *yy, uint16 dim)
    {
        float dist = -ip_distance(xx, yy, dim);
        return sqrtf(dist);
    }

    template <Metric m>
    static constexpr auto get_function()
    {
        switch (m) {
            case Metric::L2:
                return l2_distance;
            case Metric::INNER_PRODUCT:
            case Metric::FAST_COSINE:
                return ip_distance;
            case Metric::COSINE:
                return cosine_distance;
            case Metric::SPHERICAL:
                return sphere_distance;
            case Metric::L2_SQRT:
                return l2_sqrt_distance;
            case Metric::L2_NORM:
                return l2_norm_distance;
            default:
                Assert(false);
                __builtin_unreachable();
        }
    }

    static FORCE_INLINE void unpolluted_prefetch(void *ptr)
    {
        CONSTEXPR_IF(false) {
            __builtin_prefetch(ptr, 0, 0);
        }
    }

    static FORCE_INLINE void favored_prefetch(void *ptr)
    {
        CONSTEXPR_IF(false) {
#if defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
            __builtin_prefetch(ptr, 0, 2);
#else
            __builtin_prefetch(ptr, 0, 1);
#endif
        }
    }
};

#endif /* DISTANCE_TEMPLATE_SIMPLE_H */
