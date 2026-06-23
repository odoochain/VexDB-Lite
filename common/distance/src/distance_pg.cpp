/**
 * Copyright ...
 *
 * PG-only 距离工具函数：依赖 utils/lsyscache.h 的 get_func_name()。
 * 拆出 distance.cpp 以便 Duck 侧编译时只链接平台无关部分。
 */

#include "platform/platform_compat.h"
#include "utils/lsyscache.h"
#include "distance/include/distance.h"

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
