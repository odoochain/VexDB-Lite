// GRAPH_INDEX 虚拟表模块声明。
//
// 模块名 GRAPH_INDEX 是三端共享的索引类型名（DuckDB TYPE_NAME /
// DuckDB+PG 的 index_info 报告名均为 GRAPH_INDEX）。用户语法：
//   CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(embedding FLOAT[128], metric=cosine, ...);
//
// M0：最小只读骨架（建表成功 + SELECT 返回 0 行），验证 vtab 注册链路。
// M2 起填充 shadow table / xUpdate / 暴力 KNN；M3 接 GraphIndexCore。
#ifndef VEXDB_SQLITE_GRAPH_INDEX_VTAB_H
#define VEXDB_SQLITE_GRAPH_INDEX_VTAB_H

#ifdef VEXDB_SQLITE_CORE
#include "sqlite3.h"
#else
#include "sqlite3ext.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern const sqlite3_module vexdb_graph_index_module;

#ifdef __cplusplus
}
#endif

#endif  // VEXDB_SQLITE_GRAPH_INDEX_VTAB_H
