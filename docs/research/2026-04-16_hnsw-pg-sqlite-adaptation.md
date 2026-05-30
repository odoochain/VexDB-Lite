# HNSW 向量搜索引擎适配 PostgreSQL 与 SQLite 技术调研

> 调研日期: 2026-04-16
> 参考项目: pgvector (GitHub: pgvector/pgvector), sqlite-vec (GitHub: asg017/sqlite-vec)

---

## 一、PostgreSQL Custom Access Method

### 1.1 pgvector 源码结构

pgvector 的 HNSW 实现分布在以下文件中（`src/` 目录）：

| 文件 | 行数 | 职责 |
|------|------|------|
| `hnsw.c` | 402 | AM handler 注册、costestimate、reloptions |
| `hnsw.h` | 521 | 数据结构定义、宏、函数声明 |
| `hnswbuild.c` | 1171 | ambuild 实现、并行构建 |
| `hnswinsert.c` | 797 | aminsert 实现、Generic WAL 写入 |
| `hnswscan.c` | 345 | ambeginscan/amgettuple/amendscan |
| `hnswutils.c` | 1428 | 搜索算法、邻居发现、类型支持 |
| `hnswvacuum.c` | 669 | ambulkdelete/amvacuumcleanup |
| **合计** | **~5333** | |

### 1.2 amhandler 与核心回调

入口函数 `hnswhandler()` 在 `hnsw.c` 中，返回 `IndexAmRoutine` 结构：

```c
FUNCTION_PREFIX PG_FUNCTION_INFO_V1(hnswhandler);
Datum hnswhandler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    // 能力标志
    amroutine->amcanorder = false;        // 不能产生排序输出
    amroutine->amcanorderbyop = true;     // 支持 ORDER BY 操作符（关键！）
    amroutine->amoptionalkey = true;      // 无需 WHERE 子句也能扫描
    amroutine->amcanbuildparallel = true;  // 支持并行构建（PG17+）
    amroutine->amsupport = 3;             // 3 个支持函数

    // 核心回调
    amroutine->ambuild = hnswbuild;
    amroutine->ambuildempty = hnswbuildempty;
    amroutine->aminsert = hnswinsert;
    amroutine->ambulkdelete = hnswbulkdelete;
    amroutine->amvacuumcleanup = hnswvacuumcleanup;
    amroutine->amcostestimate = hnswcostestimate;
    amroutine->amoptions = hnswoptions;
    amroutine->amvalidate = hnswvalidate;
    amroutine->ambeginscan = hnswbeginscan;
    amroutine->amrescan = hnswrescan;
    amroutine->amgettuple = hnswgettuple;
    amroutine->amendscan = hnswendscan;
    amroutine->ambuildphasename = hnswbuildphasename;

    PG_RETURN_POINTER(amroutine);
}
```

### 1.3 ambuild 构建流程

`hnswbuild()` 在 `hnswbuild.c` 中实现，采用**两阶段构建**：

**阶段 1 — 内存构建**：图完全在内存中，通过 `InitGraph()` 初始化，受 `maintenance_work_mem` 限制。

**阶段 2 — 磁盘写入**：当图超出内存限制或构建完成后，通过 `CreateGraphPages()` 将元素物化为磁盘页面，再通过 `WriteNeighborTuples()` 写入邻居列表。

关键函数链：
```
hnswbuild() → BuildIndex()
  → InitGraph()               // 初始化图和分配器
  → ParallelHeapScan()        // 扫描堆表
  → InsertTuple()             // 逐元素插入
    → InsertTupleInMemory()   // 内存中图更新
      → HnswFindElementNeighbors()  // HNSW 算法 1
  → CreateGraphPages()        // 物化到磁盘页面
  → WriteNeighborTuples()     // 写邻居元组
```

### 1.4 aminsert 插入流程

`hnswinsert()` 在 `hnswinsert.c` 中实现：

```c
bool hnswinsert(Relation index, Datum *values, bool *isnull,
                ItemPointer heap_tid, Relation heap,
                IndexUniqueCheck checkUnique, IndexInfo *indexInfo)
```

核心流程：
1. `HnswInsertTupleOnDisk()` — 获取页面锁，读取元页面
2. `HnswFindElementNeighbors()` — 分层搜索找邻居
3. `UpdateGraphOnDisk()` — 磁盘持久化
4. `AddElementOnDisk()` — 分配页面空间，支持复用已删除空间
5. `HnswUpdateNeighborsOnDisk()` — 更新邻居连接

### 1.5 amgettuple 扫描流程

`hnswgettuple()` 在 `hnswscan.c` 中实现：

```c
bool hnswgettuple(IndexScanDesc scan, ScanDirection dir)
```

- 首次调用：执行 `GetScanItems()`（论文 Algorithm 5），从上层遍历到底层
- 后续调用：从结果列表取下一个候选
- 支持迭代搜索：通过 `ResumeScanItems()` 恢复扫描
- MVCC 合规：验证快照可见性

### 1.6 amcostestimate 代价估算

`hnswcostestimate()` 在 `hnsw.c` 中实现：

```c
static void hnswcostestimate(PlannerInfo *root, IndexPath *path,
    double loop_count, Cost *indexStartupCost,
    Cost *indexTotalCost, Selectivity *indexSelectivity,
    double *indexCorrelation, double *indexPages)
```

关键逻辑：
- **无 ORDER BY 时返回无穷代价**：确保优化器不会选择没有排序的 HNSW 扫描
- 代价公式：`numIndexTuples = entryLevel * m + layer0TuplesMax * layer0Selectivity`
- 其中 `entryLevel = log(tuples) * HnswGetMl(m)`
- `layer0TuplesMax = HnswGetLayerM(m, 0) * ef_search`
- `layer0Selectivity = 0.55 * log(tuples) / (log(m) * (1 + log(ef_search)))`

### 1.7 amcanorderbyop 的实现方式

`amcanorderbyop = true` 是 HNSW 索引的核心标志，表示索引支持 **ORDER BY operator** 扫描模式。

实现要点：
1. 在 `hnswhandler()` 中设置 `amcanorderbyop = true`
2. 通过 Operator Class 定义 `FOR ORDER BY float_ops` 关联排序操作符
3. 在 `hnswcostestimate()` 中，若 `path->indexorderbys == NIL` 则返回无穷代价
4. 在 `hnswgettuple()` 中根据 `scan->orderByData` 获取查询向量
5. 优化器识别到 `ORDER BY <-> (vec1, vec2) LIMIT k` 模式时，自动选择索引扫描

### 1.8 Generic WAL API 的使用

pgvector 在 `hnswinsert.c` 中大量使用 Generic WAL：

```c
#include "access/generic_xlog.h"

// 典型使用模式
GenericXLogState *state = GenericXLogStart(index);
Page page = GenericXLogRegisterBuffer(state, buf, 0);

// 对 page 进行修改...
HnswSetElement(page, ...);

GenericXLogFinish(state);  // 提交 WAL 记录

// 或在错误时
GenericXLogAbort(state);   // 放弃修改
```

API 关键点：
- `GenericXLogStart(relation)` — 开始构建 WAL 记录
- `GenericXLogRegisterBuffer(state, buffer, flags)` — 注册要修改的 buffer，返回页面副本
- `GenericXLogFinish(state)` — 应用修改，写入 WAL，自动标记 dirty 和设置 LSN
- `GenericXLogAbort(state)` — 取消修改
- 标志 `GENERIC_XLOG_FULL_IMAGE` — 记录完整页面镜像（新页面用）
- 修改必须在副本上进行，不能直接修改原始 buffer
- 每条记录最多 `MAX_GENERIC_XLOG_PAGES` 个 buffer
- 不启动临界区，可安全分配内存和抛出错误
- **注意**：Generic WAL 在逻辑解码时被忽略

### 1.9 PG 多进程模型下的索引数据共享

PG 使用**多进程**架构，索引数据共享有两种机制：

**1. Shared Memory（固定共享内存）**
- 在服务器启动时分配，所有后端进程映射
- 适用于 Buffer Pool：索引页面通过 Buffer Manager 自动共享
- 索引的磁盘页面通过 `ReadBuffer()` / `LockBuffer()` 访问，天然跨进程共享

**2. DSM（Dynamic Shared Memory）**
- pgvector 并行构建使用 DSM 共享 HNSW 图
- `CreateParallelContext()` → `InitializeParallelDSM()` → `shm_toc_allocate()` 分配
- 图中所有指针使用**相对指针**（relptr），基于基地址偏移
- `HnswPtrDeclare` 宏定义双模式指针（绝对/相对），通过 `HnswPtrStore` / `HnswPtrAccess` 访问
- DSM 生命周期：并行构建开始到结束

**正常运行时**，HNSW 索引数据完全存储在磁盘页面中，通过 PG 的 Buffer Pool（共享内存）自动缓存和跨进程共享。不需要额外的共享内存机制。

### 1.10 CREATE TYPE + Operator Class 定义

在 `sql/vector.sql` 中：

```sql
-- 1. 类型定义
CREATE TYPE vector (
    INPUT     = vector_in,
    OUTPUT    = vector_out,
    TYPMOD_IN = vector_typmod_in,
    RECEIVE   = vector_recv,
    SEND      = vector_send,
    STORAGE   = external
);

-- 2. 距离操作符
CREATE OPERATOR <-> (
    LEFTARG = vector, RIGHTARG = vector,
    PROCEDURE = l2_distance
);

-- 3. Access Method 注册
CREATE FUNCTION hnswhandler(internal) RETURNS index_am_handler
    AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE ACCESS METHOD hnsw TYPE INDEX HANDLER hnswhandler;

-- 4. Operator Class（关键：FOR ORDER BY float_ops）
CREATE OPERATOR CLASS vector_l2_ops
    FOR TYPE vector USING hnsw AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
    FOR TYPE vector USING hnsw AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
    FOR TYPE vector USING hnsw AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector),
    FUNCTION 2 vector_norm(vector);    -- 额外的规范化函数
```

**关键设计**：`FOR ORDER BY float_ops` 告诉优化器这是一个排序操作符，而非布尔比较操作符。

### 1.11 并行索引构建接口

在 `hnswbuild.c` 中的 `HnswBeginParallel()`：

```c
static void HnswBeginParallel(HnswBuildState *buildstate, bool isconcurrent, int request)
{
    // 1. 进入并行模式
    EnterParallelMode();
    ParallelContext *pcxt = CreateParallelContext("vector", "HnswParallelBuildMain", request);

    // 2. 估算共享内存
    Size esthnswshared = ParallelEstimateShared(heap, snapshot);
    Size esthnswarea = maintenance_work_mem * 1024L;  // 图的共享内存大小

    // 3. 创建 DSM 段
    InitializeParallelDSM(pcxt);

    // 4. 分配共享状态
    HnswShared *hnswshared = shm_toc_allocate(pcxt->toc, esthnswshared);
    char *hnswarea = shm_toc_allocate(pcxt->toc, esthnswarea);
    InitGraph(&hnswshared->graphData, hnswarea, esthnswarea);

    // 5. 启动 Worker
    LaunchParallelWorkers(pcxt);
}
```

Worker 入口：`HnswParallelBuildMain(dsm_segment *seg, shm_toc *toc)`
Worker 工作：`HnswParallelScanAndInsert()` — 并行扫描堆表并插入图

配置参数：
- `max_parallel_maintenance_workers` — Worker 数量（默认 2）
- `maintenance_work_mem` — 图内存上限，直接影响 DSM 大小

---

## 二、SQLite Virtual Table

### 2.1 sqlite-vec 源码结构

| 文件 | 行数 | 职责 |
|------|------|------|
| `sqlite-vec.c` | 10718 | 主扩展：vec0 虚拟表、距离函数、向量类型 |
| `sqlite-vec-diskann.c` | 1889 | DiskANN 图索引实现 |
| `sqlite-vec-ivf.c` | ~1800 | IVF 索引实现 |
| `sqlite-vec-rescore.c` | ~700 | 重排序功能 |
| **合计** | **~15000+** | |

注意：sqlite-vec 的向量搜索**不使用 HNSW**，而是使用 **DiskANN** 算法（Vamana 图）。

### 2.2 Virtual Table 完整生命周期

在 `sqlite-vec.c` 中定义的 `vec0Module`：

```c
static sqlite3_module vec0Module = {
    /* iVersion      */ 3,
    /* xCreate       */ vec0Create,      // CREATE VIRTUAL TABLE 时调用
    /* xConnect      */ vec0Connect,     // 每次连接时调用
    /* xBestIndex    */ vec0BestIndex,   // 查询优化
    /* xDisconnect   */ vec0Disconnect,  // 连接断开
    /* xDestroy      */ vec0Destroy,     // DROP TABLE 时调用
    /* xOpen         */ vec0Open,        // 打开游标
    /* xClose        */ vec0Close,       // 关闭游标
    /* xFilter       */ vec0Filter,      // 开始搜索
    /* xNext         */ vec0Next,        // 下一行
    /* xEof          */ vec0Eof,         // 是否结束
    /* xColumn       */ vec0Column,      // 获取列值
    /* xRowid        */ vec0Rowid,       // 获取 rowid
    /* xUpdate       */ vec0Update,      // INSERT/DELETE/UPDATE
    /* xBegin        */ vec0Begin,       // 事务开始
    /* xSync         */ vec0Sync,        // 事务同步
    /* xCommit       */ vec0Commit,      // 事务提交
    /* xRollback     */ vec0Rollback,    // 事务回滚
    /* xFindFunction */ 0,
    /* xRename       */ vec0Rename,      // 重命名
    /* xSavepoint    */ 0,
    /* xRelease      */ 0,
    /* xRollbackTo   */ 0,
    /* xShadowName   */ vec0ShadowName,  // Shadow Table 名称验证
};
```

**xCreate vs xConnect 的区别**：
- `xCreate`：CREATE VIRTUAL TABLE 时调用，负责**创建** shadow 表
- `xConnect`：每次数据库连接时调用，负责**连接**已有的 shadow 表
- 如果 xCreate == xConnect，则为"同名表"（eponymous），如 `vec_each`

**xBestIndex**：
```c
int (*xBestIndex)(sqlite3_vtab *pVTab, sqlite3_index_info *pIdxInfo);
```
- 输入：`aConstraint[]`（WHERE 约束）、`aOrderBy[]`（ORDER BY）
- 输出：`aConstraintUsage[]`（参数传递）、`idxNum/idxStr`（策略标识）、`estimatedCost`
- `orderByConsumed = 1` 表示虚拟表已排序输出

**xFilter**：
```c
int (*xFilter)(sqlite3_vtab_cursor*, int idxNum, const char *idxStr,
               int argc, sqlite3_value **argv);
```
- `idxNum` 和 `idxStr` 与 xBestIndex 的输出对应
- `argv[]` 包含约束表达式的值

**xUpdate 三种操作**：
```c
int (*xUpdate)(sqlite3_vtab *pVTab, int argc, sqlite3_value **argv,
               sqlite_int64 *pRowid);
```
- **DELETE**: argc=1, argv[0] 为要删除的 rowid
- **INSERT**: argc>1, argv[0]=NULL
- **UPDATE**: argc>1, argv[0] 为旧 rowid, argv[1] 为新 rowid

### 2.3 Shadow Table 机制

sqlite-vec 定义了多种 shadow 表：

```c
// 核心 shadow 表
#define VEC0_SHADOW_INFO_NAME     "\"%w\".\"%w_info\""       // 元信息
#define VEC0_SHADOW_CHUNKS_NAME   "\"%w\".\"%w_chunks\""     // 分块数据
#define VEC0_SHADOW_ROWIDS_NAME   "\"%w\".\"%w_rowids\""     // rowid 映射

// 向量存储 shadow 表（按列编号）
#define VEC0_SHADOW_VECTOR_N_NAME "\"%w\".\"%w_vector_chunks%02d\""

// DiskANN 索引 shadow 表
#define VEC0_SHADOW_DISKANN_NODES_N_NAME  "\"%w\".\"%w_diskann_nodes%02d\""
#define VEC0_SHADOW_DISKANN_BUFFER_N_NAME "\"%w\".\"%w_diskann_buffer%02d\""

// 元数据 shadow 表
#define VEC0_SHADOW_METADATA_N_NAME       "\"%w\".\"%w_metadatachunks%02d\""
#define VEC0_SHADOW_AUXILIARY_NAME        "\"%w\".\"%w_auxiliary\""
```

**_chunks 表**（核心存储结构）：
```sql
CREATE TABLE "schema"."name_chunks" (
    chunk_id INTEGER PRIMARY KEY AUTOINCREMENT,
    size INTEGER NOT NULL,
    validity BLOB NOT NULL,   -- 位图，标记每个槽位是否有效
    rowids BLOB NOT NULL      -- 固定长度的 rowid 数组
);
```

**_rowids 表**（rowid 到 chunk 位置的映射）：
```sql
CREATE TABLE "schema"."name_rowids" (
    rowid INTEGER PRIMARY KEY AUTOINCREMENT,
    id,                        -- 可选的 TEXT 主键
    chunk_id INTEGER,          -- 所在 chunk
    chunk_offset INTEGER       -- chunk 内偏移
);
```

**_vector_chunksNN 表**（向量数据）：
```sql
CREATE TABLE "schema"."name_vector_chunks00" (
    rowid PRIMARY KEY,         -- 非 INTEGER alias，需手动同步
    vectors BLOB NOT NULL      -- 向量数据块
);
```

**Shadow Table 识别**：通过 `xShadowName` 回调，当 `SQLITE_DBCONFIG_DEFENSIVE` 启用时，shadow 表变为只读，防止直接修改导致损坏。

### 2.4 Table-Valued Function 实现

sqlite-vec 中的 `vec_each` 是一个 TVF 示例：

```c
static sqlite3_module vec_eachModule = {
    /* xCreate  */ 0,                // NULL = eponymous-only 表
    /* xConnect */ vec_eachConnect,
    /* xUpdate  */ 0,                // 只读
    /* xShadowName */ 0,
    ...
};
```

TVF 通过 hidden 列接收参数：
```c
// 声明 schema，vector 是隐藏列
sqlite3_declare_vtab(db, "CREATE TABLE x(value, vector hidden)");
```

调用方式等价：
```sql
SELECT value FROM vec_each(my_vector);
-- 等价于
SELECT value FROM vec_each WHERE vector = my_vector;
```

xBestIndex 通过检查 hidden 列的等值约束来获取参数。

### 2.5 sqlite3_auto_extension 使用模式

sqlite-vec 的入口函数：

```c
SQLITE_VEC_API int sqlite3_vec_init(sqlite3 *db, char **pzErrMsg,
                                     const sqlite3_api_routines *pApi)
{
#ifndef SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);  // 可加载扩展模式
#endif

    // 注册标量函数
    sqlite3_create_function_v2(db, "vec_distance_l2", 2, ...);
    sqlite3_create_function_v2(db, "vec_distance_cosine", 2, ...);
    sqlite3_create_function_v2(db, "vec_f32", 1, ...);
    // ... 共 17 个函数

    // 注册虚拟表模块
    sqlite3_create_module_v2(db, "vec0", &vec0Module, NULL, NULL);
    sqlite3_create_module_v2(db, "vec_each", &vec_eachModule, NULL, NULL);

    return SQLITE_OK;
}
```

使用 `sqlite3_auto_extension` 的模式：
```c
// 应用程序启动时注册，所有新连接自动加载
sqlite3_auto_extension((void(*)(void))sqlite3_vec_init);

// 或运行时加载
.load ./vec0
SELECT load_extension('./vec0');
```

### 2.6 SQLite mmap 使用

配置方式：
```sql
PRAGMA mmap_size = 268435456;  -- 映射前 256MB
```

工作原理：
- SQLite 通过 VFS 的 `xFetch()` / `xUnfetch()` 方法请求 mmap 页面
- 如果页面在 mmap 范围内，直接返回指针，**零拷贝**读取
- 写操作仍复制到堆内存修改（只读 mmap，防止跨进程可见性问题）
- 超出 mmap 范围的部分，回退到传统 `xRead()`

限制：
- 默认关闭（mmap_size = 0）
- `SQLITE_MAX_MMAP_SIZE` 编译时上限（默认 2GB）
- I/O 错误会导致 SIGSEGV 信号，无法被 SQLite 捕获
- 主要加速读操作，对写性能无显著影响
- 与 WAL 模式配合效果最佳

---

## 三、共同问题

### 3.1 HNSW 代码量对比

| 项目 | 图算法代码行数 | 说明 |
|------|---------------|------|
| pgvector (HNSW) | ~5333 行 | 7 个 C 文件 |
| sqlite-vec (DiskANN + 基础设施) | ~12600 行 | sqlite-vec.c (10718) + diskann.c (1889) |
| VexDB-Lite (HNSW) | 参考自身 | GraphIndexCore + GraphIndex |

注意：sqlite-vec 的 10718 行包含大量非图算法代码（向量类型、距离函数、Virtual Table 框架等），纯 DiskANN 图代码约 1889 行。

### 3.2 存储方案对比

**pgvector 存储方案**（PG 页面存储）：

```
Meta Page (Block 0)
├── dimensions, m, ef_construction
├── entry point (BlkNo + OffsetNumber)
└── insert page 指针

Element Pages
├── HnswElementTuple: heaptids[] + level + neighbors_blkno/offno
└── 每个元素占一个或多个 tuple

Neighbor Pages
├── HnswNeighborTuple: 邻居列表 (element_blkno/offno + distance)
└── 每层一个 HnswNeighborArray
```

- 完全使用 PG 的**页面存储**（8KB 页）
- 通过 Buffer Manager 管理缓存和 I/O
- 每次修改通过 Generic WAL 记录
- 元素和邻居分开存储在不同的 tuple 中

**sqlite-vec 存储方案**（Shadow Table + Chunk 存储）：

```
_info 表: 元信息 (key-value)
_chunks 表: 分块管理 (chunk_id, size, validity_bitmap, rowids_blob)
_rowids 表: rowid → (chunk_id, chunk_offset) 映射
_vector_chunks00 表: 向量数据 BLOB（按 chunk 打包）
_diskann_nodes00 表: DiskANN 图节点（邻居 ID + 量化向量）
_diskann_buffer00 表: DiskANN 插入缓冲
```

- 使用 SQLite 的**普通表**作为存储后端（shadow tables）
- 向量按 chunk 打包存储为 BLOB
- 通过 `sqlite3_blob_open()` 直接读写 BLOB，避免全行拷贝
- DiskANN 节点存储邻居 ID 列表 + 量化后的邻居向量

### 3.3 DELETE/UPDATE 同步处理

**pgvector 的 DELETE 处理**（`hnswvacuum.c`）：

三遍扫描策略：
1. **Pass 1 - RemoveHeapTids**: 扫描所有索引页，移除无效的 heap TID，标记完全无效的元素到 deleted hash table
2. **Pass 2 - RepairGraph**: 获取排他锁，为每个受影响元素重新计算邻居（跳过已删除节点），修复图连通性
3. **Pass 3 - MarkDeleted**: 等待并发扫描完成，标记删除标志，清零数据，递增版本号（1-15 循环，使缓存失效）

UPDATE 处理：PG 中 UPDATE = DELETE + INSERT（HOT 优化不适用于索引列变更）

**sqlite-vec 的 DELETE 处理**（`vec0Update_Delete()`）：

```c
int vec0Update_Delete(sqlite3_vtab *pVTab, sqlite3_value *idValue) {
    // 1. DiskANN 图删除（如果启用）
    for (每个 DiskANN 索引列) {
        diskann_delete(p, i, rowid);  // 修剪邻居列表
    }

    // 2. 获取 chunk 位置
    vec0_get_chunk_position(p, rowid, &chunk_id, &chunk_offset);

    // 3. 清除有效性位
    vec0Update_Delete_ClearValidity(p, chunk_id, chunk_offset);

    // 4. 清零 chunk 中的 rowid
    vec0Update_Delete_ClearRowid(p, chunk_id, chunk_offset);

    // 5. 清零向量数据
    vec0Update_Delete_ClearVectors(p, chunk_id, chunk_offset);

    // 6. 删除 _rowids 表记录
    vec0Update_Delete_DeleteRowids(p, rowid);

    // 7. 删除辅助列数据
    vec0Update_Delete_DeleteAux(p, rowid);

    // 8. 清除元数据
    vec0Update_Delete_ClearMetadata(p, i, rowid, chunk_id, chunk_offset);

    // 9. 如果 chunk 完全为空，回收 chunk
    vec0Update_Delete_DeleteChunkIfEmpty(p, chunk_id, &chunkDeleted);
}
```

UPDATE 处理：sqlite-vec 中 UPDATE = DELETE + INSERT（在 `vec0Update_Insert` 中检测到已存在的 rowid 时，先调用 `vec0Update_Delete`）

---

## 四、适配 VexDB-Lite 的关键差异分析

### 4.1 PostgreSQL 适配要点

| VexDB-Lite 当前机制 | PG 对应机制 | 适配难度 |
|---------------------|------------|---------|
| FixedSizeAllocator 3 层分配 | PG 8KB 页面 + Buffer Manager | 高 — 需重写存储层 |
| SimpleRWLock 线程安全 | LWLock + Buffer 锁 + 进程模型 | 高 — 多进程 vs 多线程 |
| 查询优化器重写 | amcanorderbyop + costestimate | 中 — PG 原生支持 |
| WAL 集成 | Generic WAL API | 中 — 接口清晰 |
| 并行构建 | DSM + ParallelContext | 高 — 需相对指针 |
| FLOAT[N] 类型 | CREATE TYPE + opclass | 低 — 参考 pgvector |

### 4.2 SQLite 适配要点

| VexDB-Lite 当前机制 | SQLite 对应机制 | 适配难度 |
|---------------------|----------------|---------|
| BoundIndex 封装 | Virtual Table xCreate/xConnect | 中 — 接口较多 |
| 查询优化器重写 | xBestIndex + orderByConsumed | 中 — 需自行实现 |
| FixedSizeAllocator | Shadow Table BLOB 存储 | 高 — 需重写存储 |
| 多线程构建 | SQLite 单线程限制 | N/A — 无法直接并行 |
| WAL 集成 | SQLite 自带 WAL | 低 — 事务自动管理 |
| FLOAT[N] 类型 | BLOB + sqlite3_create_function | 低 |
