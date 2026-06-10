#ifndef GRAPH_INDEX_DEPEND_H
#define GRAPH_INDEX_DEPEND_H

#if defined(PG_VEXDB_TARGET_DUCK)
#include "vex_graph_index_depend_duck.hpp"
#elif defined(PG_VEXDB_TARGET_SQLITE)
// SQLite store policy 头在 M3 接入（vexdb_sqlite/include/store/）。
#error "SQLite graph_index store dependency lands in M3; do not include this header yet"
#elif defined(PG_VEXDB_TARGET_PG)
#include "postgres.h"
#include "global_instance.h"
#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_storage.h"
#include "vector_buffer/vector_smgr.h"
#else
#error "One of PG_VEXDB_TARGET_DUCK or PG_VEXDB_TARGET_PG must be defined"
#endif

#endif
