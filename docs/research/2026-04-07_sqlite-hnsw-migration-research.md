# HNSW 向量搜索迁移到 SQLite：技术调研报告

> 日期：2026-04-07
> 背景：基于 VexDB-Lite（DuckDB 扩展）的 HNSW 向量搜索能力，调研迁移到 SQLite 的技术方案

---

## 一、SQLite 扩展机制技术细节

### 1.1 Loadable Extension API

SQLite 提供完整的运行时可加载扩展机制：

| API | 用途 |
|-----|------|
| `sqlite3_load_extension()` | 加载 .so/.dylib/.dll 扩展 |
| `sqlite3_auto_extension()` | 注册自动加载的扩展 |
| `sqlite3_create_function()` | 注册自定义 SQL 函数 |
| `sqlite3_create_module()` | 注册虚拟表模块 |

扩展入口点返回 `SQLITE_OK_LOAD_PERMANENTLY` 可让扩展常驻内存。扩展可以同时注册自定义函数和虚拟表模块。

**对 VexDB 迁移的意义**：距离函数（L2/Cosine/IP）可通过 `sqlite3_create_function()` 注册为标量函数；HNSW 索引通过虚拟表模块实现。

### 1.2 Virtual Table 机制

Virtual Table 是 SQLite 实现自定义索引的**唯一正式途径**。核心 `sqlite3_module` 结构包含以下关键方法：

**必须实现的方法**：
- `xCreate` / `xConnect`：创建/连接虚拟表，通过 `sqlite3_declare_vtab()` 声明列结构
- `xBestIndex`：与查询优化器通信，报告查询策略和预估代价
- `xFilter` / `xNext` / `xEof`：迭代查询结果
- `xColumn` / `xRowid`：返回列值和行ID
- `xOpen` / `xClose`：游标管理
- `xDisconnect` / `xDestroy`：清理

**可选但重要的方法**：
- `xUpdate`：处理 INSERT/UPDATE/DELETE
- `xBegin` / `xSync` / `xCommit` / `xRollback`：事务控制
- `xSavepoint` / `xRelease` / `xRollbackTo`：嵌套事务（保存点）
- `xFindFunction`：自定义操作符重载
- `xShadowName`：声明影子表（v3 模块）

**xBestIndex 的工作方式**：
SQLite 查询优化器多次调用 `xBestIndex`，传入约束条件（`aConstraint`）和排序要求（`aOrderBy`）。虚拟表实现填写：
- `aConstraintUsage[].argvIndex`：将约束映射为 xFilter 参数
- `aConstraintUsage[].omit`：告知 SQLite 跳过冗余检查
- `estimatedCost`：预估代价（影响优化器选择）
- `orderByConsumed`：如果结果已排序则设为 1
- `idxNum` / `idxStr`：传递策略标识给 xFilter

支持的约束操作符包括 EQ/GT/LT/GE/LE/MATCH/LIKE/GLOB/REGEXP/NE/LIMIT/OFFSET/FUNCTION 等。

### 1.3 SQLite 是否支持自定义索引类型？

**不支持。** SQLite 的原生索引体系假设所有索引都是 B-Tree 索引，VDBE 字节码与 B-Tree 实现紧密耦合。

SQLite 官方论坛的讨论（sqlite.org/forum/info/a6b5ec976d67c28e）明确指出："The interface between the VDBE code and the built-in index implementations assumes that indexes behave like B-tree indexes."

**替代方案**：
1. **Virtual Table**（推荐）：通过虚拟表实现完整的自定义索引逻辑，用 Shadow Tables 持久化数据
2. **Expression Index + UDF**：注册 DETERMINISTIC 函数，在函数表达式上创建 B-Tree 索引（仅适用于精确匹配场景）
3. **外部索引文件**：将索引数据存储在外部文件，虚拟表作为接口层（vectorlite 的做法）

### 1.4 R-Tree 模块实现分析（自定义索引的参考范本）

R-Tree 是 SQLite 中通过 Virtual Table 实现自定义空间索引的经典范例：

**存储架构**：使用三张 Shadow Table
- `%_node(nodeno INTEGER PRIMARY KEY, data BLOB)` - 节点数据
- `%_parent(nodeno INTEGER PRIMARY KEY, parentnode INTEGER)` - 父子关系
- `%_rowid(rowid INTEGER PRIMARY KEY, nodeno INTEGER)` - 行ID到节点的映射

**节点格式**：每个节点是一个 BLOB，前 2 字节是树深度（仅根节点），接下来 2 字节是条目数，每个条目包含一个 8 字节整数 + 若干 4 字节坐标对。

**对 HNSW 迁移的启示**：
- Shadow Table 模式完全适用于存储 HNSW 图结构
- 可以设计类似的节点存储方案：`%_nodes`（HNSW 层级节点）、`%_vectors`（向量数据）、`%_config`（索引配置）
- R-Tree 的 xBestIndex 实现展示了如何与查询优化器协商空间查询策略

---

## 二、现有 SQLite 向量搜索项目分析

### 2.1 项目对比

| 项目 | 索引算法 | 语言 | 存储方式 | HNSW | 状态 |
|------|---------|------|---------|------|------|
| **sqlite-vss** | Faiss（多种） | C++ | 外部 Faiss 索引 | 支持 | 已废弃 |
| **sqlite-vec** | 暴力搜索 | 纯 C | Shadow Table chunks | 计划中 | 活跃 |
| **vectorlite** | hnswlib | C++ | 外部文件序列化 | 支持 | 活跃 |
| **sqlite-vector** | 优化暴力搜索 | C | 普通表 + Shadow Table | 不支持 | 活跃 |
| **hnsqlite** | hnswlib | Python | 外部 hnswlib 文件 | 支持 | 小众 |
| **libsql (Turso)** | DiskANN | C | 内置 | 类似 | Fork |

### 2.2 各项目技术细节

**sqlite-vec**（Alex Garcia）：
- 纯 C 无依赖，vec0 虚拟表模块
- 向量按 chunk 存储在 Shadow Table 中，KNN 搜索时逐 chunk 读取
- 不需要全部载入内存，可通过 `PRAGMA mmap_size` 优化
- 暂无 ANN 支持，计划未来加入 IVF + HNSW
- 来源：alexgarcia.xyz/blog/2024/building-new-vector-search-sqlite/

**vectorlite**：
- hnswlib 的薄封装，通过 Virtual Table API 暴露
- SIMD 加速使用 Google Highway 库，比 hnswlib 快 1.5-3x（维度 >= 256 时）
- 索引可序列化到文件，支持加载 hnswlib 原生索引文件
- 支持 predicate pushdown（需 SQLite >= 3.38）
- 插入比 sqlite-vss 快 10x，搜索快 2-40x
- 来源：github.com/1yefuwang1/vectorlite

**sqlite-vector**（SQLite Cloud）：
- 2D dispatch table 实现距离函数分派（5 种度量 x 5 种数据类型 = 25 个函数指针）
- 运行时 CPU 特性检测：AVX2/SSE2/NEON，逐步覆盖 dispatch table
- 量化通过 Shadow Table 实现：三遍扫描（统计 → 参数计算 → 压缩序列化）
- 向量存储在普通表中，不要求使用虚拟表
- 来源：deepwiki.com/sqliteai/sqlite-vector

### 2.3 相关学术论文

| 论文 | 关键贡献 | 来源 |
|------|---------|------|
| Malkov & Yashunin (2016), "Efficient and robust approximate nearest neighbor search using HNSW graphs" | HNSW 原始算法，多层导航小世界图 | arXiv:1603.09320 |
| P-HNSW (2025), "Crash-Consistent HNSW for Vector Databases on Persistent Memory" | 持久内存上的崩溃一致性 HNSW，NLog + NlistLog 日志机制 | MDPI Applied Sciences 15(19):10554 |
| Fu et al., "Fast Approximate Nearest Neighbor Search With Navigating Spreading-out Graphs" | NSG 图索引，DiskANN 的基础 | VLDB 2019 |
| Zhao et al., "Towards Efficient Index Construction and ANN Search" | 高效索引构建综述 | VLDB 2023 |

**P-HNSW 论文的特殊价值**：这是首个解决 HNSW 持久化崩溃一致性的学术工作，其 NLog（记录节点插入）和 NlistLog（记录邻居列表修改）的双日志机制可直接借鉴用于 SQLite 事务集成。

---

## 三、SQLite 与 DuckDB 扩展架构关键差异

### 3.1 索引体系对比

| 维度 | DuckDB | SQLite |
|------|--------|--------|
| **索引注册** | `CreateIndex` + `BoundIndex` 继承体系 | `sqlite3_create_module` + Virtual Table |
| **索引声明** | `CREATE INDEX ... USING HNSW` | `CREATE VIRTUAL TABLE ... USING module` |
| **与原表关系** | 索引附属于原表，自动同步 | 虚拟表独立存在，需手动同步或设计联合查询 |
| **查询优化集成** | Optimizer Hook（重写查询计划） | xBestIndex（报告代价，优化器选择） |
| **持久化** | Checkpoint 时全量序列化到数据库文件 | Shadow Table 存储在同一数据库文件中 |

**关键差异**：DuckDB 的 BoundIndex 让索引透明地附属于原始表，INSERT/UPDATE/DELETE 自动触发索引更新。SQLite 中没有等价机制——虚拟表是独立实体，需要通过触发器或应用层逻辑保持同步。

**迁移方案**：
- **方案 A（独立虚拟表）**：HNSW 索引作为独立虚拟表，用户需同时维护数据表和索引表
- **方案 B（Shadow Table + 触发器）**：在原始表上创建触发器，自动将向量数据同步到 HNSW 虚拟表
- **方案 C（类 sqlite-vector 模式）**：向量存普通表，索引信息存 Shadow Table，通过自定义函数查询

### 3.2 内存管理对比

| 维度 | DuckDB | SQLite |
|------|--------|--------|
| **分配器** | FixedSizeAllocator（3 层：节点头/向量/邻居） | sqlite3_malloc / sqlite3_mem_methods |
| **自定义分配** | 扩展直接使用内部分配器 | 可通过 SQLITE_CONFIG_MALLOC 替换全局分配器 |
| **内存映射** | Buffer Manager + Pin/Unpin | PRAGMA mmap_size |
| **内存池** | BufferPool 分页管理 | memsys5 零碎片分配器（可选） |

**迁移策略**：
- VexDB 的 FixedSizeAllocator 不能直接移植
- SQLite 扩展应使用 `sqlite3_malloc()` / `sqlite3_free()`
- 对于 HNSW 图的大块内存需求，可以：
  1. 使用自管理的 arena/pool 分配器
  2. 利用 mmap 将索引数据映射到内存
  3. 参考 hnswlib 的内存管理策略（vectorlite 的做法）

### 3.3 查询优化器对比

| 维度 | DuckDB | SQLite |
|------|--------|--------|
| **机制** | Optimizer Hook，可重写整个查询计划 | xBestIndex，只能报告代价和约束处理能力 |
| **能力** | 检测 `ORDER BY dist() LIMIT k` 并重写为索引扫描 | 通过 estimatedCost 影响选择，通过 LIMIT/OFFSET 约束感知 k 值 |
| **灵活度** | 可以完全替换物理算子 | 只能在虚拟表查询内部优化 |
| **LIMIT 感知** | 优化器直接传递 LIMIT 值 | SQLite 3.38+ 支持 SQLITE_INDEX_CONSTRAINT_LIMIT |

**迁移方案**：
DuckDB 的 `ORDER BY distance_func(...) LIMIT k` 优化器重写在 SQLite 中无法直接实现。替代方案：
1. 使用表值函数（Table-Valued Function）：`SELECT * FROM knn_search(table, query_vec, k)` 
2. 利用 xBestIndex 的 LIMIT 约束：SQLite 3.38+ 传递 LIMIT 值，可在 xFilter 中直接执行 top-k 搜索
3. 虚拟表 + `knn_param()` 函数模式（vectorlite 的做法）

### 3.4 事务与并发模型对比

| 维度 | DuckDB | SQLite |
|------|--------|--------|
| **并发模型** | MVCC（HyPer 变体），支持并行读写 | 单写多读（WAL 模式下） |
| **写并发** | 多线程并行写入 | 同一时间只有一个写入者 |
| **事务隔离** | Serializable（通过 undo buffer） | Serializable（通过锁） |
| **索引更新** | 可多线程并行构建索引 | 单线程索引构建 |

**对 HNSW 的影响**：
- VexDB 的 SimpleRWLock（共享搜索/排他插入）在 SQLite 中可简化——SQLite 天然保证写入互斥
- 并行构建（>10K 行多线程）在 SQLite 中不可行，需要改为单线程构建
- WAL 模式可确保搜索操作不被写入阻塞

---

## 四、推荐迁移架构

### 4.1 总体方案

```
+------------------+     +-------------------+
|  用户 SQL 查询    |     |  sqlite3_module   |
|                  |     |  (vex0 模块)       |
+--------+---------+     +--------+----------+
         |                        |
         v                        v
+--------+---------+     +--------+----------+
| 标量函数注册      |     | Virtual Table     |
| - vex_l2()       |     | - xBestIndex      |
| - vex_cosine()   |     | - xFilter (KNN)   |
| - vex_ip()       |     | - xUpdate (插入)   |
+------------------+     +--------+----------+
                                  |
                    +-------------+-------------+
                    |             |             |
             +------+--+  +------+--+  +------+--+
             |%_graph  |  |%_vectors|  |%_config |
             |Shadow   |  |Shadow   |  |Shadow   |
             |Table    |  |Table    |  |Table    |
             +---------+  +---------+  +---------+
```

### 4.2 Shadow Table 设计

```sql
-- HNSW 图结构
CREATE TABLE vex_idx_graph(
  layer INTEGER,
  node_id INTEGER,
  neighbors BLOB,  -- 紧凑编码的邻居列表
  PRIMARY KEY(layer, node_id)
);

-- 向量数据（chunk 存储）
CREATE TABLE vex_idx_vectors(
  chunk_id INTEGER PRIMARY KEY,
  data BLOB  -- N 个向量的紧凑存储
);

-- 索引配置
CREATE TABLE vex_idx_config(
  key TEXT PRIMARY KEY,
  value TEXT
);
```

### 4.3 关键映射关系

| VexDB (DuckDB) 组件 | SQLite 对应实现 |
|---------------------|----------------|
| GraphIndexCore | 独立 C 模块，数据存 Shadow Table |
| FixedSizeAllocator | 自管理 arena allocator + sqlite3_malloc |
| BoundIndex 自动同步 | xUpdate 方法 + 可选触发器 |
| Optimizer Hook (ORDER BY...LIMIT) | xBestIndex LIMIT 约束 + 表值函数 |
| SimpleRWLock | SQLite 内置写互斥（无需额外锁） |
| 并行构建（>10K） | 单线程构建（SQLite 限制） |
| Product Quantization | Shadow Table 存储量化码本和编码 |
| HybridIndex 分区 | 虚拟表参数指定分区键 + 多图管理 |

---

## 五、风险与建议

### 高风险项
1. **无透明索引集成**：SQLite 没有 `CREATE INDEX ... USING HNSW` 语法，用户体验会有差距
2. **单线程构建**：大数据集索引构建速度受限
3. **查询优化限制**：无法像 DuckDB 那样透明重写 `ORDER BY dist() LIMIT k`

### 建议
1. **优先参考 vectorlite 架构**：hnswlib 封装 + Virtual Table 是最成熟的模式
2. **采用 sqlite-vector 的 SIMD dispatch 模式**：2D dispatch table + 运行时特性检测
3. **Shadow Table 存储**：比外部文件更安全，支持事务，但需注意性能
4. **P-HNSW 的日志机制**：可借鉴其 NLog/NlistLog 设计保证崩溃一致性
5. **API 设计参考**：`CREATE VIRTUAL TABLE idx USING vex0(embedding float[128], metric='cosine', m=16, ef_construction=200)`

---

## 参考来源

- [SQLite Run-Time Loadable Extensions](https://sqlite.org/loadext.html)
- [SQLite Virtual Table Mechanism](https://www.sqlite.org/vtab.html)
- [SQLite R-Tree Module](https://www.sqlite.org/rtree.html)
- [SQLite Dynamic Memory Allocation](https://sqlite.org/malloc.html)
- [SQLite Forum: Custom Index Support Discussion](https://sqlite.org/forum/info/a6b5ec976d67c28e)
- [sqlite-vec (Alex Garcia)](https://github.com/asg017/sqlite-vec)
- [vectorlite](https://github.com/1yefuwang1/vectorlite)
- [sqlite-vector (SQLite Cloud)](https://github.com/sqliteai/sqlite-vector)
- [Alex Garcia: Building New Vector Search for SQLite](https://alexgarcia.xyz/blog/2024/building-new-vector-search-sqlite/index.html)
- [The State of Vector Search in SQLite (Marco Bambini)](https://marcobambini.substack.com/p/the-state-of-vector-search-in-sqlite)
- [vectorlite introduction (Dev.to)](https://dev.to/yefuwang/introducing-vectorlite-a-fast-and-tunable-vector-search-extension-for-sqlite-4dcl)
- [HNSW Original Paper (arXiv:1603.09320)](https://arxiv.org/abs/1603.09320)
- [P-HNSW: Crash-Consistent HNSW on Persistent Memory](https://www.mdpi.com/2076-3417/15/19/10554)
- [DuckDB vs SQLite Comparison (BetterStack)](https://betterstack.com/community/guides/scaling-python/duckdb-vs-sqlite/)
- [SQLite WAL Documentation](https://www.sqlite.org/wal.html)
- [DuckDB Concurrent Transactions](https://duckdb.org/2024/10/30/analytics-optimized-concurrent-transactions)
