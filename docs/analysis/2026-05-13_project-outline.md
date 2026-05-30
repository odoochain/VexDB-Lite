# VexDB 项目大纲

> 2026-05-13。多 agent 审查 + 深度代码勘探后的整合版。

---

## 一、一句话定位

**vexdb：把最新向量检索 SOTA（HNSW / RaBitQ / DiskANN / IVF-PQ）做成主库语义级一致、可下沉到 PG / DuckDB / SQLite 的工业级参考实现。**

三个可证伪的承诺：

| 承诺 | 可证伪指标 |
|---|---|
| **算法跟得上** | 12 个月内 SOTA 论文（RaBitQ、DiskANN、Filtered-HNSW…）主库都有工业级实现 |
| **工程打得过** | ann-benchmarks 公开 leaderboard recall/QPS 前列 |
| **生态吃得下** | PG / DuckDB / SQLite 三宿主上算法核心字节级一致（共享 libvex-core）+ 行为级 spec 一致（L2 YAML DSL 跨引擎测试矩阵） |

**同源保证的诚实表述**：不承诺"三宿主代码 100% 重合"——PG 多进程 + xlog vs DuckDB 多线程 + BLOB 持久化 vs SQLite 单线程是根本不同的模型。承诺的是 **"算法核心 30-40% 代码共享 + 行为 spec 100% 一致"**，剩余宿主集成层各自原生实现。

---

## 二、产品形态：一主一矛

```
                  ┌──────────────────────────────────┐
   主体（产品/收入）│   vexdb 主库（openGauss 改造）   │
                  │   完整向量数据库 = 内核 + 6 类索引 │
                  └──────────────────────────────────┘
                                  │
                  算法内核 → 重新打磨为跨宿主薄适配
                                  ↓
                  ┌──────────────────────────────────┐
   矛尖（品牌/渠道）│  vexdb 适配层（开源前哨）         │
                  │  pg_vexdb · vexdb_duckdb · sqlite   │
                  └──────────────────────────────────┘
```

- **主库 = 主体**：商业化主体，算法落地的标准答案。对标 Milvus / Qdrant / LanceDB
- **适配层 = 矛尖**：开源前哨，KPI 是 GitHub star + 宿主社区渗透 + 给主库带 inbound。**不卖钱**，价值是品牌与生态信号

### openGauss 起点的正面叙事

> "openGauss 是起点不是终点：改造的是存储/执行器/AM/规划器以适配向量负载；**算法核心是自研 Layer 0，并通过 pg_vexdb / vexdb_duckdb 同源外放到主流宿主，反向证明了算法层与内核解耦的工业级品质。**"

国内/信创沟通强调"openGauss 内核 + 自研向量栈"；海外材料以"vexdb core + 多宿主"为前台叙事。

---

## 三、仓库地图

| 路径 | 角色 |
|---|---|
| `openGauss-vector-main/` | **主库**：openGauss 内核 + 6 类向量索引 AM |
| `pg_vexdb/` | **pg_vexdb**：PostgreSQL 标准扩展（目前以 HNSW 为主） |
| `vexdb_lite/duckdb/vexdb_duckdb/` | **vexdb_duckdb**：DuckDB 扩展 |
| `vexdb_lite/` 外层 | 打包/分发（wheel/jar/npm/crate/iOS/Android/WASM） |
| `ann-benchmark-main/` | ANN-benchmarks fork（对外 benchmark 阵地） |

---

## 四、技术栈分层

### Layer 0 — 算法核心（主库已完整）

| 算法 | 主库路径 | 状态 |
|---|---|---|
| HNSW | `src/gausskernel/storage/access/hnsw/` | ✓ build / insert / scan / vacuum / xlog 全栈 |
| IVF-Flat | `src/gausskernel/storage/access/ivf/` | ✓ 独立 AM |
| IVF-PQ | `src/gausskernel/storage/access/annvector/pq.*` | ✓ |
| DiskANN | `src/gausskernel/storage/access/diskann/` | ✓ 含 disk_pq.cpp |
| RaBitQ | `src/gausskernel/storage/access/rabitq/` | ✓ rotator + estimator |
| HybridANN（过滤索引） | `src/gausskernel/storage/access/hybridann/` | ✓ |

距离层全栈 SIMD 分发：SSE / AVX2 / AVX512 / NEON / WASM SIMD128（`architecture.cpp` + `distances_simd_template.cpp` 共 9029 行，含 3.4× 模板展开）。

### Layer 1 — 主库（openGauss-vector）

**openGauss 内核 + 自研向量索引 AM 与算法栈**。利用 openGauss 已有：transam / buffer / lmgr / executor / 规划器；自研叠加：向量类型、6 类索引 AM、混合查询规划与代价模型、SIMD 距离栈、量化栈。

### Layer 2 — 适配层

**`pg_vexdb`（PostgreSQL）**
- 模块：`distance/`（SIMD macro 框架，2663 行）、`rabitq/`（头文件为主，实现尚未完整下沉）、`quantizer/`（存根）、`vtl/`（HNSW 图容器：list / btree / disk_container / 缓存）、`knl/`（内存分配兼容层）、`module/`（性能监测）、`graph_index*.cpp`（PG 扩展 AM handler，HNSW）
- **主库与 pg_vexdb 当前未真正共享代码**——是"概念同源、各自实现"，要"语义级一致"还需把核心算法抽到 libvex-core

**`vexdb_duckdb`（DuckDB，fork v1.5.0）**
- 入口 `vex_extension.cpp` 注册 `vex_ef_search` / `vex_brute_force_threshold` / `vex_pq_search_mode` / `vex_pq_refine_k_factor` 等配置
- 核心 HNSW：`graph_index.cpp`（1336 行）+ `vex_graph_index_depend_duck.hpp` 的 `MemStore<>` 类（1018 行，**真正的耦合中心**：3 个 FixedSizeAllocator + 4 LWLock + 8 striped lock）
- 磁盘层：`vex_disk_block_store`；优化器：`vex_optimizer.cpp` 通过 `DistanceFuncEntry kDistanceFuncs[]` 识别 KNN 模式改写为 `PhysicalIndexScan`；并发：`SimpleRWLock`；持久化已修复 reload gap（`f41c023246`）
- 量化：`quantizer/product_quantizer.h`，PQ 是可选层
- **当前只注册了 `GRAPH_INDEX`**，HYBRID_INDEX 在 DuckDB 端尚未落地
- 跨宿主 shim：`duck_pg_shim.hpp`（LWLock → std::atomic 等）；`compat/shared_algo_probe.cpp` 是 9 行空 stub（NodeStore 桥接预留位）

**`vexdb-sqlite`（规划中）**

### Layer 3 — 分发

多语言绑定 / 多平台打包 / 包名统一 `vexdb-lite`。端侧形态（iOS 11MB、Android 11MB、WASM 8MB）作为独立卖点——**没人在认真做端侧向量库**。

### Layer 4 — 质量与基准

**L2 spec DSL（YAML → 多引擎渲染）已落地**：
- `duckdb/tests/spec/` 含 119 个 yaml（shared 25 / duckdb 80 / opengauss 1 / pg 0）
- `_lib/render.py` 完整渲染器，按引擎产出 DuckDB `.test` / PG/openGauss `.sql`
- `_lib/dialects.yaml` 覆盖 5 个 GUC × 3 引擎 + 类型/距离/字面量函数

**当前缺口**：openGauss 主库未接入（仍用传统 `.sql`）、CI 未自动化（手动跑 `run_duckdb.sh` / `run_pg.sh`）、PG 专属用例数 = 0。

ANN-benchmarks fork 作对外 benchmark 阵地。

---

## 五、开源对标

### 主库赛道（独立向量数据库）—— 主战场

| 项目 | 现状 | 与 vexdb 关系 |
|---|---|---|
| **Milvus** ~40k★ | 2.6 GA（hot/cold tiering）、Zilliz Cloud | 头号对手；Milvus Lite 是功能子集（无分布式/GPU/高级索引） |
| **Qdrant** ~26k★ | v1.30 实时索引，Rust 生态首选 | 单机 + 过滤查询主要对手 |
| **LanceDB** 上升中 | Lance 列存 + DuckDB SQL retrieval | **嵌入式向量赛道直接竞品** |
| Weaviate ~14k★ | 活跃 | 中等强度 |
| Chroma | 1.0 GA（2026.1） | dev/SDK 路线，错位 |
| Turbopuffer | 闭源 SaaS（Cursor/Notion/Linear） | 不开源，抢心智 |
| Vespa / Vearch / Marqo / DeepLake / MyScale | 老牌但停滞或转向 | 弱对标 |

### 适配层赛道

| 宿主 | 对手 |
|---|---|
| **PG** | pgvector ★事实标准 / VectorChord（继承自**已停维**的 pgvecto.rs，主打磁盘+RaBitQ）/ pgai（embedding 流水线）/ ~~pg_embedding 2025.1 已删除~~ |
| **DuckDB** | duckdb-vss（官方 experimental，**只有内存 HNSW、未持久化**）/ duckdb-faiss-ext（几乎不活跃） |
| **SQLite** | sqlite-vec（Alex Garcia，Mozilla Builders 支持）/ ~~sqlite-vss 作者本人弃用~~ |

### 算法底座（非产品）

faiss / hnswlib / DiskANN / usearch / ScaNN / RaBitQ 官方实现。

### 差异化论点

> **"主库语义级一致的跨宿主向量索引族——OLTP / OLAP / Edge 三态同源。"**

四个可辩护点：

1. **主库语义等价 vs 功能子集**：Milvus Lite 是 Milvus 的子集；vexdb 三宿主与主库**调参面 / 量化 / 过滤语义 1:1**（spec/测试矩阵保障）
2. **宿主原生 vs 寄生**：FAISS 被各宿主"封装抄一部分"导致语义漂移；vexdb 通过 libvex-core + 宿主原生 AM/事务/WAL/类型系统集成
3. **主库厂商身份 vs 算法库厂商身份**：usearch/hnswlib 没事务、没 SQL planner；vexdb 带着关系型主库的优化器/并行/持久化语义外放
4. **三态同源**：没人同时把同一份索引核心交付到 OLTP（PG）+ OLAP（DuckDB）+ Edge（SQLite/iOS/Android/WASM）

### "同源"的诚实定义

- **字节级共享（约 30-40% 代码）**：距离 + SIMD dispatch、RaBitQ、PQ encode/decode、HNSW 拓扑算子。通过 libvex-core 静态库 + 严格 C-ABI 提供
- **行为级一致（100%）**：L2 spec DSL（YAML → 三引擎渲染）保证调参面、量化语义、过滤语义、recall 数字跨宿主一致

剩余 60%（持久化 / 锁 / 事务 / IO）必须分宿主原生实现——这恰恰是 vexdb 优于 FAISS 寄生模式的地方：**每个宿主用自己最自然的 buffer manager / WAL / planner**。

### pg_vexdb 杀手论点（三选一 all-in）

候选：
- (A) "pgvector 的 API，DiskANN/RaBitQ 的性能，零迁移成本" — 性能向
- **(B) "过滤查询比 pgvector 快 N 倍"** — 抓 pgvector 真实痛点（HybridANN 是主库已有强项），**推荐**
- (C) "PG 里的向量，能无缝下沉到主库做十亿级" — 升级路径向，配合商业化

---

## 六、合流（libvex-core）：现状 + 路径

### 模块级判定

🟢 **完全可共享（~30%）**：距离 + SIMD、RaBitQ、PQ encode/decode、HNSW 拓扑算子

🟡 **算法可共享但要适配层（~10%）**：HNSW 主流程 search/insert（主库代码大量 `LockBuffer`/`palloc`/`PageGetItem`，靠模板 + 注入分配器隔离）；NodeStore（DuckDB 平铺数组 vs PG buffer manager，接口可统一，两套实现各 800-1200 行）

🔴 **必须分宿主（~60%）**：WAL/持久化、锁与并发、内存分配、DiskANN、HybridANN、ItemPointer 行 ID

### 四个致命断裂点（无法靠抽象消除）

1. **C++ 异常 vs PG longjmp**：`ereport(ERROR)` 跨 C++ 栈不调析构。core 边界必须 `noexcept` + 返回 `vex_status_t`
2. **xlog 在 DuckDB 不存在**：持久化完全推给 host
3. **ItemPointer 在 DuckDB 是占位符**：`GraphIndexPoint.tids` 字段需重设计
4. **进程模型**：PG 多进程 vs DuckDB 多线程 vs SQLite 单线程，并行构建调度必须分宿主

### libvex-core 推荐形态

**静态 `.a` + 严格 C-ABI**（不是 header-only 模板，不是 C++ 虚基类）。距离层例外可做 header-only inline（SIMD 性能）。

8 条边界硬性原则：
1. 内存分配走 `vex_allocator_t` host 注入
2. IO 走 `vex_blockstore_t` 抽象
3. 错误用 `vex_status_t` 返回码，禁止跨边界异常
4. 日志走 `vex_logger_t` callback
5. 线程由 host 决定，core 不 spawn 线程
6. row_id 统一 `uint64_t`，host 负责双向映射
7. SIMD intrinsics 不暴露在 public header
8. 配置用 key-value 字符串，避免每加参数破 ABI

### 工程量

总工程量 ~14 人月（2 人并行 ~7 个月 / 3 人 ~5 个月）

| Workstream | 人月 | 关键路径 |
|---|---|---|
| 距离层（SIMD dispatch + half/int8） | 1.5 | 否 |
| 量化层（PQ + RaBitQ） | 2 | 否 |
| **HNSW 核心** | **4** | **是** |
| **持久化适配（DuckDB/PG/oG 各 1 PM）** | **3** | **是** |
| 测试基础设施（spec DSL → 三引擎） | 2 | 部分 |
| 打包 / CI / 多平台 | 1.5 | 否 |

### MVP（3 周，距离层 + PQ）

| Week | 任务 | 难点 |
|---|---|---|
| 1 | 抽出主库 `distances_simd_template.cpp` 核心 L2/Cos/IP，把 `palloc` 替换为注入 allocator | 主库 `annvector/distance/distance.cpp:101` 用 `palloc()`，要么改主库要么 core 注入 allocator |
| 2 | 统一 `uint32` vs `uint16` dim 参数；补 ARM SVE/SME 在 vexdb_duckdb 侧验证 | ARM SVE/SME 在 PG_VEXDB / vexdb_duckdb 尚未适配 |
| 3 | 三方各自切薄包装层 + 跨 ISA 回归（x86 SSE/AVX/AVX512 + ARM NEON/SVE） | 跨 ISA 隐藏不兼容 |

**压缩到 2 周**：只做 float32 + SSE/AVX + x86（砍掉 fp16/int8/ARM）

**HNSW 留到 PoC 之后**——它同时触碰内存/IO/并发/持久化四个接口，且真正耦合中心是 `MemStore<>` 类（1018 行），不是 HNSW 主算法本身。

### vexdb_duckdb HNSW 改造工作量

`graph_index.cpp` 1336 行按耦合密度分层：

| 类别 | 行数 | 改写成本 | 例子 |
|---|---|---|---|
| 纯算法（可零改搬走） | ~300 | 0 | `SearchPQ`（160 行，90% 纯算法）、PQ encode/decode |
| 半算法半 allocator（需抽象 Store） | ~350 | 中 | `BuildBulk`（L319-435）、`Append`（L796-903） |
| 纯 host 集成（应留 adapter） | ~500 | 高 | `DeserializeFromStorage`（L1057-1334）、`Create`（184 行） |
| Merge/Vacuum/CommitDrop/Delete | ~8 | — | 均 stub，无耦合压力 |

**真正的工作是把 `MemStore` 参数化为 `MemStore<StorePolicy>`**，DuckDB allocator 作为一个 Policy，PG/SQLite 各自一个 Policy。

### 真实合流基础设施现状

✓ **L2 spec DSL**：`duckdb/tests/spec/` 119 yaml + `_lib/render.py` 完整渲染器 + `_lib/dialects.yaml`（5 GUC × 3 引擎）。**这是已落地的行为级一致兜底。**
✓ `compat/shared_algo_probe.cpp` 9 行空 stub（NodeStore 桥接预留位）
✗ `libvex-core/` 目录尚未创建
✗ `adapters/pg/` subtree 尚未发生
✗ L2 spec DSL 在 openGauss 主库尚未接入、CI 未自动化、PG 专属用例数 = 0

---

## 七、v1 兼容矩阵（已锁定）

| 维度 | v1 范围 |
|---|---|
| **DuckDB** | **1.5.2 only**（dev 分支 15 文件已对齐 1.5.2 API，未提交） |
| **PostgreSQL** | **PG 16 ~ 19**（已适配 PG 16/17/18/19；主验证平台 PG 19，release.sh 默认针对 PG 19 boost include） |
| **平台** | x86_64-linux、aarch64-linux、macOS arm64/x86_64、iOS、Android、WASM |
| **算法** | HNSW + PQ（vexdb_duckdb 已就绪；pg_vexdb HNSW 已就绪、PQ 从主库下沉中） |
| **PG_VEXDB 仓库** | 2026-05-13 已并入 `vexdb_lite/duckdb/`，外部 checkout 废弃 |

不追求宽兼容，先把单一版本打透；扩展性留 v1.x。

---

## 八、对外材料硬料缺口

| 类别 | 状态 |
|---|---|
| 一句话定位 | ✓ |
| 性能数据点（sift-1M recall@10 vs pgvector @ PG 19） | **TODO** |
| 12 个月路线图（Q 粒度 3-5 里程碑） | **TODO** |
| 目标用户画像（RAG / 信创金融 / 端侧 AI） | **TODO** |
| 商业模式暗示（开源 → 商业版 → 信创授权 漏斗） | **TODO** |
| pg_vexdb 杀手论点 | (B) 过滤查询 N× pgvector，待性能数据落地 |

---

## 九、待办与未决问题

1. **dev 分支 15 文件未提交**：A 仓库统一（2 文件）+ B DuckDB 1.5.2 API 对齐（12 文件）+ C NEONV8 临时关闭（2 文件，留 TODO 追根因）
2. **pg_vexdb PQ 从零下沉**：主库 `annvector/pq.*` 335 行 + AM handler，估 2-3 周
3. **L2 spec 覆盖 HNSW + PQ**：补 ~30 用例 shared spec，CI 自动化
4. **README × 2**：pg_vexdb / vexdb-lite quickstart + 兼容矩阵 + pgvector 迁移指南
5. **License / NOTICE / Third-party**：DuckDB MIT + openGauss MulanPSL2 兼容性核对
6. **libvex-core MVP**：3 周距离层 → PQ；HNSW 留到 PoC 之后
7. **HybridIndex 在 vexdb_duckdb 端落地**：v1 是否包含？还是 v1.1？
8. **vexdb-sqlite 启动节奏**：当前只有 research，无代码
9. **RaBitQ 位精度范围（1-8 bit）、并发写入锁机制**：白皮书必须说清
10. **NEONV8 SIMD 路径根因**：哪个平台 build 失败？v1.x 补回
