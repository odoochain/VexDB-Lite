// 距离计算入口 —— UDF 与 GRAPH_INDEX 虚拟表共用。
//
// 语义与 DuckDB 端严格对齐，且三个 metric 的"distance"都满足 lower = closer，
// ORDER BY distance ASC 跨 metric 一致：
//   L2     -> sqrt(Σ(x-y)²)
//   COSINE -> 1 - cos_sim
//   IP     -> -Σxy（pgvector <#> 风格负内积）
#ifndef VEXDB_SQLITE_VEX_DISTANCE_ENTRY_H
#define VEXDB_SQLITE_VEX_DISTANCE_ENTRY_H

#include <cstdint>

// 与 common/distance Metric 枚举数值一致（L2=0/COSINE=1/INNER_PRODUCT=2），
// 但此头不拉 common 重型依赖，vtab/config 层可独立 include。
enum class VexMetric : uint32_t {
    L2 = 0,
    COSINE = 1,
    INNER_PRODUCT = 2,
};

namespace vexdb_sqlite {

using DistanceFn = float (*)(const float *, const float *, uint16_t);

// 取"用户语义"的距离函数（lower=closer，定义见文件头）。内部走 common
// SIMD dispatch，进程级缓存，线程安全（首调初始化）。
DistanceFn GetDistanceFn(VexMetric metric);

}  // namespace vexdb_sqlite

#endif  // VEXDB_SQLITE_VEX_DISTANCE_ENTRY_H
