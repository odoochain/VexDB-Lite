// 集中处理 loadable vs 静态注册（core）两形态的 SQLite 头切换。
//
// 双形态分发是架构前置决策（见 docs/plans/2026-06-10_sqlite-adapter-v1-plan.md）：
//   - loadable (.so/.dylib)：桌面/服务端，运行时 .load。用 sqlite3ext.h，
//     所有 sqlite3_* 调用经 sqlite3_api 间接表。
//   - 静态注册 (VEXDB_SQLITE_CORE)：移动端唯一可行形态（iOS 系统 libsqlite3
//     禁扩展加载、WASM 不支持运行时 .load）。直接链接宿主 sqlite3 符号。
//
// 每个 .cpp 只需 include 本头：loadable 形态下 sqlite3_api 的 extern 声明
// （即 SQLITE_EXTENSION_INIT3 的展开）已在此给出，新增源文件无需记忆任何
// INIT 宏约定。唯一例外是入口 vexdb_sqlite_init.cpp，由它用
// SQLITE_EXTENSION_INIT1 定义 sqlite3_api 变量本体（全库仅此一处）。
#ifndef VEXDB_SQLITE_INTERNAL_H
#define VEXDB_SQLITE_INTERNAL_H

#ifdef VEXDB_SQLITE_CORE
#include "sqlite3.h"
#else
#include "sqlite3ext.h"
extern const sqlite3_api_routines *sqlite3_api;
#endif

#endif  // VEXDB_SQLITE_INTERNAL_H
