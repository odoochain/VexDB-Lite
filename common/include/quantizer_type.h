// 宿主无关的量化器类型枚举。
//
// 历史上 PG（vexdb_pg/include/quantizer.h）与 DuckDB
// （vexdb_duckdb/include/vex_graph_index_depend_duck.hpp）各复制了一份等值定义；
// SQLite 适配层引入时上提到 common 作为单一来源。两宿主头暂保留各自定义
// （编译单元不会同时见到，无 ODR 冲突），libvex-core 合流时统一收口到此。
#ifndef VEX_COMMON_QUANTIZER_TYPE_H
#define VEX_COMMON_QUANTIZER_TYPE_H

#include <cstdint>

enum class QuantizerType : uint8_t {
    NONE = 0,
    PQ = 1,
    RABITQ = 2
};

#endif  // VEX_COMMON_QUANTIZER_TYPE_H
