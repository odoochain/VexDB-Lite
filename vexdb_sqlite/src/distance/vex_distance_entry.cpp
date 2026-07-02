// 距离入口实现：把 common DispatchRunner（重模板，编译慢）隔离在本翻译单元，
// vtab/UDF 只 include 轻头 vex_distance_entry.h。
#include "vex_distance_entry.h"

#include "distance/core/distance.h"
#include "distance/core/distance_dispatcher.h"
#include "distance/core/distance_utils_core.h"

#include <cmath>

namespace {

// 与 DuckDB 端 GetRawDistanceFunc 完全一致的 dispatcher 实例化（NO_QUANT、
// FLOAT、unaligned——SQLite BLOB 指针不保证对齐）。
ann_helper::distance_func GetRawDistanceFunc(Metric metric) {
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(
            metric, DistPrecisionType::FLOAT, 1, QuantizerType::NONE,
            [](auto &d) -> ann_helper::distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

float ComputeL2(const float *x, const float *y, uint16_t dim) {
    static const auto raw = GetRawDistanceFunc(Metric::L2);  // 返回平方距离
    return std::sqrt(raw(x, y, dim));
}

float ComputeCosine(const float *x, const float *y, uint16_t dim) {
    static const auto raw = GetRawDistanceFunc(Metric::COSINE);  // 返回 -cos_sim
    return 1.0f + raw(x, y, dim);
}

float ComputeNegIP(const float *x, const float *y, uint16_t dim) {
    static const auto raw = GetRawDistanceFunc(Metric::INNER_PRODUCT);  // 返回 -ip
    return raw(x, y, dim);
}

}  // namespace

namespace vexdb_sqlite {

DistanceFn GetDistanceFn(VexMetric metric) {
    switch (metric) {
    case VexMetric::L2: return ComputeL2;
    case VexMetric::COSINE: return ComputeCosine;
    case VexMetric::INNER_PRODUCT: return ComputeNegIP;
    }
    return ComputeL2;
}

}  // namespace vexdb_sqlite
