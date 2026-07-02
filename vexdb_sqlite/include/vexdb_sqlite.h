// vexdb_lite SQLite 适配层公共入口声明。
//
// 两种集成方式：
//   1. loadable：宿主 `.load ./vexdb_lite sqlite3_vexdblite_init`（或省略 entry
//      名走默认推导）。SQLite 自动调用 sqlite3_vexdblite_init。
//   2. 静态注册：宿主在 open 后调用 vexdb_sqlite_register(db)，或在 open 前
//      sqlite3_auto_extension((void(*)())sqlite3_vexdblite_init) 全局注册。
//      移动端（iOS/Android/WASM）走此路。
#ifndef VEXDB_SQLITE_H
#define VEXDB_SQLITE_H

#ifdef VEXDB_SQLITE_CORE
#include "sqlite3.h"
#else
#include "sqlite3ext.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 把 vexdb_lite 的全部对象（GRAPH_INDEX 虚拟表模块、标量函数）注册到连接 db。
// 成功返回 SQLITE_OK。loadable 入口与静态注册路径都汇聚到此函数。
int vexdb_sqlite_register(sqlite3 *db);

// loadable 扩展入口。entry point 名遵循 SQLite 约定 sqlite3_<name>_init。
int sqlite3_vexdblite_init(sqlite3 *db, char **pzErrMsg,
                           const sqlite3_api_routines *pApi);

#ifdef __cplusplus
}
#endif

#endif  // VEXDB_SQLITE_H
