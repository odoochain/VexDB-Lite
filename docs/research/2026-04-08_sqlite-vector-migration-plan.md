# VexDB-Lite 向量搜索迁移到 SQLite：综合调研报告

> 类型: research
> 创建日期: 2026-04-08
> 状态: final

## 背景与需求

VexDB-Lite 当前基于 DuckDB v1.5.0 构建，作为 DuckDB 扩展提供完整的向量搜索能力：FLOAT[N] 向量类型、L2/Cosine/IP 距离函数（SIMD 加速）、HNSW 图索引、Product Quantization、混合分区索引（HybridIndex）、查询优化器重写（ORDER BY dist() LIMIT k → 索引扫描）。

现需调研将这套能力迁移到 SQLite 的技术方案，以覆盖更广泛的嵌入式场景（移动端、边缘计算、轻量级应用）。

---

## 一、学术研究

### 1.1 SQLite 扩展机制

| 机制 | 说明 | 对迁移的意义 |
|------|------|-------------|
| `sqlite3_create_function()` | 注册标量/聚合 SQL 函数 | 距离函数（L2/Cosine/IP）直接注册 |
| `sqlite3_create_module()` | 注册虚拟表模块 | HNSW 索引的**唯一实现途径** |
| Shadow Table | 虚拟表在同一数据库文件中创建的辅助表 | 持久化 HNSW 图结构、向量数据、配置 |
| `xBestIndex` / `xFilter` | 虚拟表与查询优化器的通信接口 | 替代 DuckDB 的 Optimizer Hook |

**关键结论：SQLite 不支持自定义索引类型。** VDBE 字节码假设所有索引都是 B-Tree。Virtual Table 是实现 HNSW 索引的唯一正式途径。

### 1.2 R-Tree 模块（最佳参考范本）

SQLite 的 R-Tree 空间索引使用 3 张 Shadow Table（`%_node`, `%_parent`, `%_rowid`）存储索引数据，完全通过 Virtual Table API 暴露。HNSW 迁移可采用类似架构。

### 1.3 关键论文

| 论文 | 核心价值 |
|------|---------|
| HNSW (Malkov & Yashunin, 2016) | 原始算法，多层导航小世界图 |
| **P-HNSW (2025)** | 崩溃一致性 HNSW，NLog + NlistLog 双日志机制，**可直接借鉴用于 SQLite 事务集成** |
| NSG (Fu et al., VLDB 2019) | DiskANN 基础，磁盘友好的图索引 |

### 1.4 DuckDB vs SQLite 架构差异

| 维度 | DuckDB (现状) | SQLite (目标) | 迁移影响 |
|------|-------------|--------------|---------|
| 索引注册 | `CREATE INDEX ... USING HNSW` | `CREATE VIRTUAL TABLE ... USING vex0` | 用户语法变化 |
| 索引与表关系 | BoundIndex 自动同步 | 虚拟表独立，需 xUpdate 或触发器同步 | 需要额外同步设计 |
| 查询优化 | Optimizer Hook 重写查询计划 | xBestIndex 报告代价 + LIMIT 约束 (3.38+) | 无法透明重写，需表值函数 |
| 并发模型 | MVCC 多线程读写 | 单写多读 (WAL 模式) | 简化锁设计，但丧失并行构建 |
| 内存管理 | FixedSizeAllocator 3 层分配 | sqlite3_malloc 或自管理 arena | 需重新设计分配器 |
| 索引构建 | >10K 行多线程并行 | **单线程** | 大数据集构建速度下降 |

---

## 二、开源项目

### 2.1 项目全景对比

| 项目 | Stars | 索引算法 | 语言 | HNSW | PQ | 距离函数 | License | 成熟度 |
|------|-------|---------|------|------|-----|---------|---------|--------|
| **sqlite-vec** | 7.4K | 暴力搜索 (DiskANN alpha) | 纯 C | 计划中(已放弃) | 无 | L2 | MIT/Apache | 活跃 |
| **sqlite-vss** | 2K | Faiss 多种 | C++ | 支持 | 支持 | L2/Cosine/IP | MIT | **已废弃** |
| **sqlite-vector** | 822 | 暴力+SIMD | C | 无 | 标量量化 | L2/Cosine/IP/L1/Hamming | **ELv2** | 活跃 |
| **Vectorlite** | 357 | hnswlib | C++ | 支持 | 无 | L2/Cosine/IP | MIT | 小众 |
| **libSQL/Turso** | - | DiskANN | C | 类似 | 无 | L2/Cosine | BUSL | Fork |
| **Vec1** (官方) | - | IVFADC+OPQ | 纯 C | 无 | 有 | 未知 | 未发布 | 未完成 |

### 2.2 关键发现

1. **sqlite-vec** 是生态主流（7.4K stars），但作者**明确放弃 HNSW**，认为不适合 SQLite 的 shadow table 模型，转向 DiskANN
2. **sqlite-vss** 因 Faiss 依赖导致平台限制、编译困难、内存问题而被废弃
3. **Vectorlite** 是唯一有 HNSW 的 SQLite 扩展，但社区小、无 PQ、不支持移动端
4. **sqlite-vector** 距离函数最丰富，SIMD dispatch 设计值得借鉴，但 ELv2 许可阻碍商业使用

### 2.3 市场空白

**没有任何项目同时具备：HNSW ANN 索引 + PQ 量化 + 完整距离函数(L2/Cosine/IP) + 移动端支持 + MIT/Apache 开源**。

---

## 三、商业产品

### 3.1 嵌入式向量数据库竞品

| 产品 | 定位 | 存储 | 索引 | 移动端 | 特点 |
|------|------|------|------|--------|------|
| **Turso/libSQL** | SQLite 分支 | 内置 | DiskANN | 部分 | 需要用 libSQL 替换 SQLite |
| **Zvec (阿里)** | "向量数据库的SQLite" | 自有格式 | 多种 | 待确认 | 9.3K stars，增长极快 |
| **Chroma** | 嵌入式向量DB | 自有 | HNSW | 无 | mindshare 下降 (13.4%→7.6%) |
| **LanceDB** | 嵌入式向量DB | Lance 格式 | IVF-PQ/DiskANN | 无 | mindshare 下降 (9.5%→6.6%) |
| **USearch** | 纯搜索库 | 自有 | HNSW | 有 | 比 FAISS 快 10x，语言绑定最广 |

### 3.2 市场趋势

1. **"回归现有数据库"趋势**：独立向量数据库热潮降温，pgvector/sqlite-vec 等"扩展模式"受追捧
2. **On-Device AI 爆发**：移动端 + 边缘计算场景下 SQLite 向量搜索成为基础设施刚需
3. **开源许可敏感**：社区对 ELv2 等限制性许可极度反感，MIT/Apache 是推广前提

---

## 四、综合分析

### 现状总结

| 维度 | 成熟度 | 说明 |
|------|--------|------|
| SQLite 向量扩展 | 中等 | sqlite-vec 生态好但无 ANN；其余项目各有短板 |
| HNSW on SQLite | **低** | 仅 Vectorlite 一个小项目，无 PQ、无移动端 |
| 移动端向量搜索 | **空白** | 无成熟的 HNSW + SQLite + iOS/Android 方案 |
| PQ 量化 on SQLite | **空白** | Vec1 未发布，其余无实现 |

### VexDB-Lite 的差异化优势

我们已有的技术积累恰好填补市场空白：

| 能力 | 竞品状态 | 我们的优势 |
|------|---------|-----------|
| HNSW 索引 | sqlite-vec 放弃，Vectorlite 不成熟 | GraphIndexCore 已验证 |
| PQ 量化 | 几乎空白 | 已实现完整 PQ 编解码 |
| L2/Cosine/IP + SIMD | sqlite-vector 有但 ELv2 | 已有 SSE/AVX2/NEON/WASM dispatch |
| 混合分区索引 | **无竞品** | HybridIndex 独有 |
| iOS/Android 构建 | 仅 sqlite-vec 理论支持 | 已有完整构建流程和 C API |

---

## 五、推荐方案

### 方案一：纯 C Virtual Table 扩展（推荐）

**技术路线：**
- 纯 C 实现，零外部依赖（参考 sqlite-vec 验证过的方案）
- Virtual Table 模块 `vex0`，Shadow Table 持久化
- 复用现有 `distance/` SIMD 代码和 GraphIndexCore 算法

**架构设计：**
```
┌─────────────────────────────────────────┐
│  SQLite Extension (vex0)                │
├──────────────┬──────────────────────────┤
│ 标量函数      │ 虚拟表模块               │
│ vex_l2()     │ xBestIndex (LIMIT感知)   │
│ vex_cosine() │ xFilter (KNN搜索)        │
│ vex_ip()     │ xUpdate (INSERT/DELETE)   │
├──────────────┴──────────────────────────┤
│ HNSW Core (从 GraphIndexCore 移植)      │
│ PQ Encoder/Decoder                      │
│ SIMD Distance (SSE/AVX2/NEON/WASM)     │
├─────────────────────────────────────────┤
│ Shadow Tables                           │
│ %_graph | %_vectors | %_config | %_pq   │
└─────────────────────────────────────────┘
```

**SQL 语法设计：**
```sql
-- 创建向量索引
CREATE VIRTUAL TABLE vec_idx USING vex0(
  embedding float[128],
  metric = 'cosine',
  m = 16,
  ef_construction = 200
);

-- 插入向量（关联外部表的 rowid）
INSERT INTO vec_idx(rowid, embedding) VALUES (1, X'...');

-- KNN 搜索
SELECT rowid, distance
FROM vec_idx
WHERE embedding MATCH vex_param(?, 10)  -- query_vec, k=10
ORDER BY distance;

-- 或表值函数风格
SELECT * FROM vex_search('vec_idx', ?, 10);
```

**实施路径：**
1. **Phase 1（2 周）**：搭建 SQLite 扩展骨架 + 标量距离函数注册
2. **Phase 2（3 周）**：Virtual Table 框架 + Shadow Table 存储 + 暴力搜索
3. **Phase 3（3 周）**：移植 GraphIndexCore（HNSW）到 Virtual Table
4. **Phase 4（2 周）**：PQ 量化集成 + SIMD dispatch
5. **Phase 5（2 周）**：iOS/Android/WASM 交叉编译 + C API

**预期效果：**
- 二进制体积：~500KB（远小于 DuckDB 的 ~11MB）
- 搜索性能：与 DuckDB 版本持平（核心算法不变）
- 平台覆盖：Linux/macOS/Windows/iOS/Android/WASM
- 许可证：MIT/Apache

**风险：**
- 无 `CREATE INDEX ... USING HNSW` 透明语法，用户体验有差距
- 单线程构建，大数据集(>100K)构建速度不如 DuckDB 版
- 虚拟表与原表同步需额外设计

### 方案二：基于现有项目二次开发

**技术路线：**
- Fork sqlite-vec 或 Vectorlite，在其基础上添加 HNSW/PQ
- 复用其 Virtual Table 骨架和构建系统

**优点：** 起步更快，可借力社区
**缺点：** 受限于上游架构设计，sqlite-vec 作者已放弃 HNSW 路线，Vectorlite 依赖 C++ 和 hnswlib

**评估：不推荐。** 我们的 HNSW/PQ 实现已成熟，自建扩展反而更干净。

### 方案三：双引擎策略（DuckDB + SQLite 并行维护）

**技术路线：**
- 将核心算法（HNSW/PQ/Distance）抽取为独立 C 库 `libvex-core`
- DuckDB 扩展和 SQLite 扩展分别作为上层适配

**优点：** 代码复用最大化，两个生态都能覆盖
**缺点：** 维护成本高，需要抽象层设计

**评估：长期最优，但短期建议先完成方案一，再逐步重构为双引擎。**

---

## 六、参考资源

### 论文
- [HNSW Original Paper (arXiv:1603.09320)](https://arxiv.org/abs/1603.09320)
- [P-HNSW: Crash-Consistent HNSW on Persistent Memory (MDPI 2025)](https://www.mdpi.com/2076-3417/15/19/10554)
- [NSG: Fast ANN Search (VLDB 2019)](http://www.vldb.org/pvldb/vol12/p461-fu.pdf)

### 开源项目
- [sqlite-vec](https://github.com/asg017/sqlite-vec) — 7.4K stars, 纯 C, 暴力搜索
- [sqlite-vector](https://github.com/sqliteai/sqlite-vector) — 822 stars, SIMD dispatch 设计
- [Vectorlite](https://github.com/1yefuwang1/vectorlite) — 357 stars, hnswlib + SQLite
- [sqlite-vss](https://github.com/asg017/sqlite-vss) — 2K stars, 已废弃
- [USearch](https://github.com/unum-cloud/usearch) — 纯搜索库, 语言绑定最广

### 商业产品
- [Turso/libSQL](https://turso.tech/) — SQLite 分支，DiskANN 内置
- [Zvec (阿里)](https://github.com/alipay/zvec) — "向量数据库的 SQLite"
- [Chroma](https://www.trychroma.com/) — 嵌入式向量数据库
- [LanceDB](https://lancedb.com/) — 嵌入式向量数据库

### 技术文档
- [SQLite Virtual Table API](https://www.sqlite.org/vtab.html)
- [SQLite R-Tree Module](https://www.sqlite.org/rtree.html)
- [SQLite Loadable Extensions](https://sqlite.org/loadext.html)
- [Alex Garcia: Building New Vector Search for SQLite](https://alexgarcia.xyz/blog/2024/building-new-vector-search-sqlite/index.html)
- [The State of Vector Search in SQLite](https://marcobambini.substack.com/p/the-state-of-vector-search-in-sqlite)
