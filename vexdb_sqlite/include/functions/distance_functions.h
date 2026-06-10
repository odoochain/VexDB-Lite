// SQLite 标量距离函数注册（M1）。
//
// 函数集（与 DuckDB 端 dialects 四件套 1:1，便于 spec 跨引擎渲染）：
//   vexdb_l2_distance(a, b)              -> REAL   sqrt(L2²)
//   vexdb_cosine_distance(a, b)          -> REAL   1 - cos_sim
//   vexdb_inner_product(a, b)            -> REAL   正内积
//   vexdb_negative_inner_product(a, b)   -> REAL   -内积（pgvector 风格 lower=closer）
//   vexdb_f32(json_text | blob)          -> BLOB   float32 小端平铺编码
//   vexdb_vector_to_json(blob)           -> TEXT   调试/对照输出
//
// 向量输入两种形态均可：float32 BLOB（长度 %4==0）或 JSON 数组文本 '[1.0, 2.0]'。
#ifndef VEXDB_SQLITE_DISTANCE_FUNCTIONS_H
#define VEXDB_SQLITE_DISTANCE_FUNCTIONS_H

#ifdef VEXDB_SQLITE_CORE
#include "sqlite3.h"
#else
#include "sqlite3ext.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

int vexdb_sqlite_register_distance_functions(sqlite3 *db);

#ifdef __cplusplus
}
#endif

#endif  // VEXDB_SQLITE_DISTANCE_FUNCTIONS_H
