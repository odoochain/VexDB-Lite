# VexDB-Lite SQLite 迁移：数据库选型与向量索引算法对比

> 类型: research / decision
> 创建日期: 2026-04-08
> 状态: final

---

## 一、数据库选型：DuckDB vs SQLite

### 1.1 为什么考虑 SQLite

VexDB-Lite 当前基于 DuckDB v1.5.0 构建。随着 On-Device AI 和端侧 RAG 需求的爆发，需要评估 SQLite 作为替代引擎的价值。

### 1.2 核心维度对比

| 维度 | DuckDB | SQLite | 结论 |
|------|--------|--------|------|
| **二进制体积** | ~11MB（最小 ~6MB） | ~500KB | SQLite 小 10-20x |
| **运行时基础内存** | ~10-20MB（空载）| ~几百 KB | SQLite 更轻量 |
| **生态渗透率** | 新兴，应用有限 | 全球部署量最大的数据库，每台手机/浏览器内置 | SQLite 压倒性优势 |
| **移动端支持** | 无官方支持，需自行适配 | iOS/Android 原生内置 | SQLite 开箱即用 |
| **嵌入集成门槛** | 需打包 DuckDB 运行时 | 应用已有 SQLite，零额外依赖 | SQLite 零成本接入 |
| **API 稳定性** | 每个版本都在变（v1.4→1.5 需适配） | 20 年向后兼容 | SQLite 维护成本极低 |
| **WASM/浏览器** | 有但体积大 | 成熟方案（wa-sqlite 等） | SQLite 更成熟 |
| **列式分析** | 原生列式引擎，分析查询极快 | 行式存储，分析能力弱 | DuckDB 优势领域 |
| **并发写入** | MVCC 多线程并行读写 | 单写多读（WAL 模式） | DuckDB 优势领域 |
| **扩展开发体验** | BoundIndex 体系，CREATE INDEX 透明语法 | Virtual Table，无透明索引语法 | DuckDB 开发体验更好 |

### 1.3 内存占用深度对比

以 10 万条 128 维 float32 向量为例：

| 场景 | DuckDB | SQLite（全量加载） | SQLite（mmap） |
|------|--------|------------------|---------------|
| 引擎基础开销 | ~15MB | ~1MB | ~1MB |
| HNSW 索引 | ~65MB | ~65MB | 按需映射 |
| 查询时峰值 | ~90MB | ~70MB | ~20MB |

注：DuckDB 的 Buffer Manager 默认按系统内存 80% 设置上限，但实际按需分配，空载时约 10-20MB。

**VexDB-Lite 当前的内存管理机制**：GraphIndexCore 使用 FixedSizeAllocator（底层是 DuckDB BlockManager），数据存储在 Buffer Manager 管理的 block 中，本身具备 Pin/Unpin 按需加载能力。但当前实现中，搜索前会通过 `BuildBufferCaches()` 将所有节点的 buffer **全量 Pin 住**缓存�� `BufferPtrCache` 中，以支持并行搜索时的 lock-free 指针访问。这意味着运行时效果上索引数据是常驻内存的，Buffer Manager 不会换出已 Pin 的 buffer。

理论上可以改为不全量 Pin、让 Buffer Manager 按需换入换出，但需要牺牲并行搜索的 lock-free 特性，每次访问节点都需要加锁调用 `FixedSizeAllocator::Get()`。

两者在内存管理上的对比：

| 维度 | DuckDB Buffer Manager | SQLite mmap |
|------|----------------------|-------------|
| 管理粒度 | Block 级（256KB） | OS 页级（4KB/16KB） |
| 换出决��� | DuckDB 内部 LRU（当前全量 Pin 未���出） | OS 内核管理，自动换入换出 |
| 内存压力响应 | 需要应用层主动 Unpin | OS 自动响应 iOS jetsam / Android LMK，直接回收 clean page |
| 并行搜索 | BufferPtrCache lock-free 访问（需全量 Pin） | mmap 天然支持多线程读（但 SQLite 本身单写多读） |

总结：DuckDB 的 Buffer Manager 架构上支持按需加载，但 VexDB-Lite 当前为了并行搜索性能选择了全量 Pin。SQLite mmap 方案由 OS 内核管理页面换入换出，粒度更细、对移动端内存压力响应更及时，但不存在量级上的差异。

### 1.4 生态整合优势

SQLite 版本可以实现 **三合一轻量级 AI 基础设施**：

| 能力 | 实现方式 | 说明 |
|------|---------|------|
| 向量搜索 | vex0 扩展（我们做） | 语义检索 |
| 全文检索 | FTS5 BM25（SQLite 内置） | 关键词搜索 |
| Hybrid Search | RRF 融合两者 | 混合检索 |
| 知识图谱 | 邻接表 + WITH RECURSIVE / graphqlite | GraphRAG |

一个 SQLite 文件搞定向量+全文+图谱，无需额外服务。DuckDB 做不到这个生态整合度。

### 1.5 迁移代价

| 维度 | 代价 |
|------|------|
| 用户体验 | 丢失 `CREATE INDEX ... USING HNSW` 透明语法，变成虚拟表 |
| 查询优化 | 丢失 `ORDER BY dist() LIMIT k` 自动重写，需要表值函数 |
| 并行构建 | SQLite 单线程写，大数据集构建慢 |
| 分析能力 | 丢失 DuckDB 的列式存储和分析查询优势 |
| 开发成本 | 约 12 周重新适配 |

### 1.6 选型结论

| 目标场景 | 推荐 | 理由 |
|---------|------|------|
| 移动端 / 边缘 / 轻量嵌入 | **SQLite** | 体积小、内存低、原生集成、生态好 |
| 服务端 / 分析场景 | **DuckDB** | 列式引擎、并行查询、透明索引语法 |
| 两个都要 | 抽取 libvex-core | 核心算法独立，上层分别适配 |

---

## 二、端侧向量索引算法对比

### 2.1 端侧的真实数据规模

| 场景 | 典型向量数 | 维度 |
|------|----------|------|
| 本地相册搜索 | 1K - 10K | 512 |
| 本地文档 RAG | 500 - 5K | 768/1536 |
| 聊天记忆检索 | 1K - 50K | 384 |
| 离线商品推荐 | 10K - 100K | 128 |
| 端侧极限场景 | ~100K | 128-768 |

**关键事实：90% 的端侧场景数据量 < 1 万条。**

### 2.2 候选算法

#### 暴力搜索 + SIMD

逐一计算 query 与所有向量的距离，取 top-k。通过 SIMD（SSE/AVX2/NEON/WASM SIMD）加速距离计算。

- 优点：零额外内存、无需构建、实现简单、精度 100%
- 缺点：O(n) 复杂度，大数据集慢
- 适用：< 10K 条

#### HNSW（Hierarchical Navigable Small World）

多层跳表式图索引，上层稀疏导航快速定位区域，下层密集图精确搜索。

- 优点：搜索极快（O(log n)）、增量插入友好、不需要训练
- 缺点：内存占用大（图结构 ~30% 额外开销）、构建慢、删除复杂
- 适用：内存充足时的 > 50K 场景

#### IVF-PQ（Inverted File + Product Quantization）

先将向量空间聚类为 nlist 个桶（IVF），再用乘积量化（PQ）压缩每个向量。搜索时只扫描最近的 nprobe 个桶。

- 优点：内存极低（压缩比 10-30x）、搜索快
- 缺点：**需要训练阶段**（K-means 聚类 + PQ 码本学习）、数据分布变化需重训
- 适用：内存受限的大数据集（> 100K）

#### DiskANN

微软提出的磁盘友好图索引（Vamana 图），节点存储在磁盘上，搜索时按需加载。

- 优点：磁盘友好、增量插入、不需要训练、内存占用极低
- 缺点：依赖随机 IO 性能、实现复杂度高
- 适用：大数据集 + 存储充足场景

### 2.3 端侧性能实测估算

128 维向量，top-10 搜索，移动端 ARM 单线程 + NEON：

| 数据量 | 暴力+SIMD | HNSW | IVF-PQ | DiskANN |
|--------|----------|------|--------|---------|
| 1K | <0.5ms | ~0.1ms | N/A (不值得) | N/A |
| 10K | ~3-5ms | ~0.3ms | ~1ms | ~1-2ms |
| 100K | ~30-50ms | ~1ms | ~5-15ms | ~5-10ms |
| 1M | ~300-500ms | ~2ms (内存不可行) | ~10-20ms | ~10-15ms |

### 2.4 端侧内存占用对比

100K 条 128 维向量（原始数据 ~49MB）：

| 方案 | 索引额外内存 | 总内存 | 移动端可行？ |
|------|------------|--------|------------|
| 暴力搜索 | 0 | 49MB | 可行 |
| HNSW (m=16) | ~15MB | 64MB | 可行但偏大 |
| IVF-PQ (pq_m=32) | ~3MB（码本） | ~6MB（压缩后） | 非常好 |
| DiskANN | ~5MB 内存 + 磁盘 | ~5MB | 非常好 |

1M 条 128 维向量（原始数据 ~488MB）：

| 方案 | 索引额外内存 | 总内存 | 移动端可行？ |
|------|------------|--------|------------|
| 暴力搜索 | 0 | 488MB | 不可行 |
| HNSW (m=16) | ~150MB | 638MB | 不可行 |
| IVF-PQ (pq_m=32) | ~3MB | ~35MB | 非常好 |
| DiskANN | ~10MB + 磁盘 | ~10MB | 非常好 |

### 2.5 关键决策因素：训练问题

IVF-PQ 需要"训练"（K-means 聚类 + PQ 码本学习），而端侧数据是逐条插入的。这跟 SQLite 的使用心智模型存在根本冲突：

```
SQLite 用户的预期：
  INSERT → 数据进去了
  SELECT → 数据查出来了
  没有第三步

IVF-PQ 的要求：
  INSERT × N → 数据进去了
  TRAIN       → 训练码本（用户困惑：这是什么？）
  SELECT      → 数据查出来了
```

SQLite 内置的 FTS5（全文搜索）和 R-Tree（空间索引）都不需要训练，插一条就能查。**用户不期望"索引需要训练"。**

如果要隐藏训练步骤（自动阈值触发），会引入不可预测的延迟 — 用户不知道哪次操作会突然慢一下。

DiskANN 和 HNSW 没有这个问题 — 每次 INSERT 独立处理，不依赖全局信息。

### 2.6 IVF-PQ 重训练成本

当数据分布变化较大时需要重训练。以 128 维、nlist=256、pq_m=32 为例：

**桌面 CPU（x86，单线程 + SIMD）：**

| 数据量 | IVF 聚类 | PQ 训练 | 重编码 | 总计 |
|--------|---------|---------|--------|------|
| 10K | ~50ms | ~50ms | ~10ms | ~0.1s |
| 100K | ~0.5s | ~0.5s | ~0.1s | ~1s |
| 1M | ~5s | ~5s | ~1s | ~10s |

**移动端 ARM（单线程 + NEON，约 3-5x 慢）：**

| 数据量 | 总计 | 用户感受 |
|--------|------|---------|
| 10K | ~0.3s | 无感 |
| 100K | ~3-5s | 可接受 |
| 1M | ~30-60s | 偏慢，需后台执行 |

重训练频率通常很低：只有换 embedding 模型或数据量增长 5-10 倍时才需要。大部分端侧场景下码本训练一次就够了。

### 2.7 SQLite 生态的选择

现有项目的算法选择验证了上述分析：

| 项目 | 选择 | 理由 |
|------|------|------|
| sqlite-vec | 暴力 → DiskANN | 无需训练、磁盘友好、增量友好 |
| Vectorlite | HNSW | 搜索最快，但内存大 |
| Turso/libSQL | DiskANN | 无需训练、B-tree 友好 |
| sqlite-vector | 暴力+SIMD | 简单可靠 |

**没有一个项目选择 IVF-PQ**，原因正是训练步骤与 SQLite 心智模型的冲突。

---

## 三、推荐方案

### 3.1 分层索引策略

根据数据规模自动选择最优算法，对用户透明：

```
数据量 < 10K   →  暴力搜索 + SIMD（零开销，毫秒级）
数据量 10K-1M  →  DiskANN（磁盘友好，无需训练，增量插入）
可选增强       →  PQ 量化层（压缩向量存储，降低内存/磁盘占用）
```

### 3.2 为什么是这个组合

| 决策 | 理由 |
|------|------|
| 暴力搜索作为基础 | 覆盖 90% 端侧场景，零复杂度 |
| DiskANN 而非 HNSW | 内存低、磁盘友好、无需训练、增量插入友好 |
| DiskANN 而非 IVF-PQ | 无需训练步骤，符合 SQLite 使用直觉 |
| PQ 作为可选压缩层 | 不改变索引类型，只压缩存储，降低 IO |

### 3.3 PQ 的定位调整

在此方案中，PQ 不再是索引算法的核心（IVF-PQ），而是作为**存储压缩层**：

- DiskANN 图结构不变
- 向量数据用 PQ 压缩存储（原始 512B → 压缩后 32B）
- 搜索时用压缩向量做粗排，必要时回查原始向量精排
- 类似于 DiskANN 论文中的 compressed in-memory + full-precision on-disk 策略

这样既保留了 PQ 的内存/存储优势，又避免了训练步骤的体验问题（PQ 码本可以在数据量足够时后台静默训练，不阻塞任何操作）。

### 3.4 与 VexDB-Lite 现有能力的映射

| VexDB-Lite 现有能力 | SQLite 版本对应 | 复用程度 |
|---------------------|----------------|---------|
| SIMD 距离函数 (L2/Cosine/IP) | sqlite3_create_function 注册 | **直接复用** distance/ 代码 |
| GraphIndexCore (HNSW) | 替换为 DiskANN | 需新实现，但图搜索逻辑类似 |
| Product Quantization | 作为存储压缩层 | **直接复用** quantizer/ 代码 |
| HybridIndex (分区) | DiskANN + 分区键 | 需适配但概念相同 |
| 查询优化器重写 | xBestIndex + 表值函数 | 需重新实现 |

### 3.5 实施优先级

| 优先级 | 功能 | 工作量 | 价值 |
|--------|------|--------|------|
| P0 | 暴力搜索 + SIMD + Virtual Table 骨架 | 3 周 | 覆盖多数端侧场景 |
| P1 | DiskANN 索引 | 4 周 | 大数据集性能 |
| P2 | PQ 压缩层 | 2 周 | 内存/存储优化 |
| P3 | FTS5 Hybrid Search (RRF) | 1 周 | 混合检索 |
| P4 | iOS/Android/WASM 交叉编译 | 2 周 | 平台覆盖 |

---

## 四、参考资源

### 算法论文
- [HNSW (Malkov & Yashunin, 2016)](https://arxiv.org/abs/1603.09320) — 多层导航小世界图
- [DiskANN (Subramanya et al., NeurIPS 2019)](https://papers.nips.cc/paper/9527-rand-nsg) — 磁盘友好图索引
- [Product Quantization (Jegou et al., TPAMI 2011)](https://hal.inria.fr/inria-00514462v2) — 向量压缩经典论文
- [P-HNSW (2025)](https://www.mdpi.com/2076-3417/15/19/10554) — 崩溃一致性 HNSW

### SQLite 技术文档
- [SQLite Virtual Table API](https://www.sqlite.org/vtab.html)
- [SQLite R-Tree Module](https://www.sqlite.org/rtree.html)（自定义索引参考范本）
- [SQLite FTS5](https://www.sqlite.org/fts5.html)（BM25 全文检索）

### 开源项目
- [sqlite-vec](https://github.com/asg017/sqlite-vec) — 7.4K stars, 暴力+DiskANN(alpha)
- [Vectorlite](https://github.com/1yefuwang1/vectorlite) — 357 stars, HNSW
- [sqlite-vector](https://github.com/sqliteai/sqlite-vector) — 822 stars, SIMD dispatch 设计
- [Alex Garcia: Building New Vector Search for SQLite](https://alexgarcia.xyz/blog/2024/building-new-vector-search-sqlite/index.html)

### 市场分析
- [The State of Vector Search in SQLite](https://marcobambini.substack.com/p/the-state-of-vector-search-in-sqlite)
