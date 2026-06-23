# Graph Index 迁移指南

任意数据库要接入 graph_index 算法库，需按以下步骤完成。

## 已验证平台

| 平台 | 状态 | 备注 |
|------|------|------|
| PostgreSQL (`vexdb_pg`) | ✅ 功能完整 | 标准参考实现 |
| DuckDB (`vexdb_duckdb`) | ⚠️ 部分工作 | 未按本指南完整接入，存在已知缺陷（详见 `duckdb-migration-guide.md`） |
| 其他 | ❌ 未验证 | 需按本指南完整执行 |

> **重要**：目前**只有 PG 是功能完整的**。DuckDB 早期接入跳过了若干关键步骤，
> 存在 buffer pool 集成缺失、内存统计缺失、PQ DML 不完整等问题。完整分析与修复
> 路线见 [`duckdb-migration-guide.md`](./duckdb-migration-guide.md)。

---

## 第一阶段：阅读理解（不改代码）

### 步骤 1：阅读算法内核

`common/graph_index/graph_index_algorithm.h` — graph_index 的 HNSW 算法实现。
**完全平台无关**，不依赖任何 DB API。

它通过模板参数 `store` 调用存储层。算法对 `store` 的 API 合同见
`common/graph_index/graph_index_depend.h`，迁移目标必须逐方法实现。

算法实际调用的 store API（约 25 个方法，必须全部实现）：

```
# 元素 / id 管理
add_elem, add_vector, add_async_id, assign_vector_id, set_entrypoint,
get_entry, release_entry_lock, max_id, get_vector_num

# 邻居 / 距离
get_data, read_data, get_neighbors, set_neighbor, get_point_info,
get_distance, get_distance_batch, get_distance_precise, get_distance_est,
get_neighbor_stats, fetch_vec_from_heap, reset_neighbors_val_pool

# 并发
lock_point, unlock_point

# 元信息
get_dim, get_vecsize, get_elemsize, get_index, get_precision,
get_norm_func

# 静态常量
use_dist_cache, has_occlusion_cache, clustered
```

### 步骤 2：阅读 PG 参考实现

`vexdb_pg/include/graph_index/graph_index_storage.h` — PG 侧 store 模块实现。

> **注意：PG 实现 deep-coupled 到 PG Buffer / Relation / metapage API，
> 不能直接 `#include` 复用**。`vexdb_pg/include/disk_container/` 下的 10 个文件
> （blockmgr / buffer_cache / diskarray / diskvector / plain_store / freespace / ...）
> 同样 PG-coupled。它们是**参考样板**，不是可移植代码。

### 步骤 3：阅读距离分发模块

`common/distance/` — SIMD 距离分发。**完全平台无关**，不需做任何改动，
可以直接接入任意代码中。

### 步骤 4：阅读 vtl 内存容器

`common/vtl/` — VexDB Template Library，纯内存容器集合：

| 容器 | 用途 |
|------|------|
| `vtl/vector` | 动态数组（HNSW 邻居列表） |
| `vtl/hashtable` | 哈希表（visited set / id 映射） |
| `vtl/priority_queue` | 优先队列（搜索候选集 / top-k） |
| `vtl/bitlock` / `vtl/bitvector` | 位锁 / 位图（并发控制 / 有效位） |
| `vtl/span` / `vtl/string` / `vtl/list` | 基础工具类型 |
| `vtl/allocator` | 内存分配器抽象 |

**这些容器是平台无关的**，通过 `platform_compat.h` 间接调用平台内存服务。
迁移目标只需做两件事即可直接使用：

1. **在 `<platform>_compat.h` 里提供底层内存 API**（详见步骤 6）：
   - `palloc` / `pfree` / `repalloc`（或等价的 `vex_alloc` / `vex_free`）
   - `MemoryContext` / `CurrentMemoryContext` / `MemoryContextAlloc`
     （vtl 的 `CtxAllocator` 依赖这套；非 PG 平台可定义为 no-op / 单一
     全局 context）
2. **选择 allocator 类型注入容器模板**：vtl 默认用 `DEFAULT_ALLOCATOR<T>`
   （= `CtxAllocator<T>`，走 `MemoryContextAlloc`）。如果本平台用
   `malloc` 直接管内存，可改为 `MallocAlloc<T>`；或自定义 allocator
   实现 `allocate / deallocate / reallocate / construct / destroy` 接口。

参考 `common/vtl/allocator` 末尾的三个别名：
```cpp
template <typename T> using DEFAULT_ALLOCATOR = vtl::CurCtxAllocator<T>;
template <typename T> using HUGE_ALLOCATOR    = vtl::HugeCtxAllocator<T>;
template <typename T> using CONTEXT_ALLOCATOR = vtl::CtxAllocator<T>;
```

对接完成后，HNSW 算法在 build/search 期间用到的所有临时容器
（`Vec<Cand>` / `USet<T>` / `fpq` / `cpq` 等）都由 vtl 承担，无需迁移目标
自己重新实现。

> **注意（PG 特有）**：PG 因 `setjmp` 与 RAII 不兼容，vtl 容器需要手动调
> `.destroy()` 释放，不走析构。其他平台（如 Duck）走 RAII 也可，但沿用
> `.destroy()` 模式更省心。

### 步骤 5（可选）：阅读量化器模块

`common/quantizer/pq/` 和 `common/quantizer/rabitq/` — PQ / RaBitQ 量化算法。

> **量化器是可选的优化**，不是接入必需。最小可工作接入只需支持 Plain
> （原始 float）路径。如果首版接入不追求内存压缩 / 大规模数据集，可以
> 跳过本步和步骤 10，后续按需补齐。

如果要接入 PQ/RaBitQ，大部分核心算法（train / encode / decode）平台无关
可直接复用，但 `pq_distancer.h` 声明的 5 个方法（`prepare/process/flush/
configure_for_metric/hnsw_read_pq_center`）需要每平台自己实现，因为它们
涉及与本平台存储层的交互。

---

## 第二阶段：适配实现（按依赖顺序）

### 步骤 6：平台 compat 层

在 `common/platform/` 下新建 `<platform>_compat.h`（参考 `pg_compat.h` /
`duck_compat.h`），提供以下抽象的最小集合：

- **内存**：`palloc` / `pfree` / `repalloc` / `MemoryContextAlloc` /
  `MemoryContext` / `CurrentMemoryContext`（vtl `CtxAllocator` 需要）
- **错误**：`vex_elog` / `ereport` / `errmsg` / `elog`
- **断言**：`Assert` / `Assume`
- **类型别名**：`uint8` / `uint16` / `uint32` / `Oid` / `BlockNumber` /
  `Relation` / ...
- **PG 兼容宏**：`FORCE_INLINE` / `PG_VEXDB_TARGET_*` 等

然后在 `common/platform/platform_compat.h` 加入分支：

```c
#if defined(PG_VEXDB_TARGET_<PLATFORM>)
#include "<platform>_compat.h"
#endif
```

并在本平台 build 系统 `-DPG_VEXDB_TARGET_<PLATFORM>`。

> **vtl 联动**：本步完成后，步骤 4 列出的 vtl 容器立即生效 —— 算法层用到的
> `Vec` / `USet` / `fpq` / `cpq` 全部走 vtl，不需要迁移目标再写一份。

### 步骤 7：磁盘容器（核心组件）

基于本平台 buffer pool 实现两个容器（对标 PG `disk_container::DiskVector`
和 `PlainStore`）：

- **`DiskVector<T>`** — 变长记录的磁盘数组，承载 HNSW 节点（邻居 IDs）
- **`PlainStore`** — 定长元素的无序集合，承载向量字节（float / PQ code / RaBitQ）

容器必须对接：
- 本平台 buffer pool 的 Pin/Unpin（或等价机制）
- WAL 日志（如果支持增量写）
- FreeSpace 空闲槽位管理（支持 vacuum 回收）

**这是工作量最大的部分**。PG 的实现（`vexdb_pg/include/disk_container/`）可以作为
参考，但不能直接复用。

### 步骤 8：MemStore + DiskStore 模板实例

参照 PG `graph_index_storage.h`（line 70 `MemStore` / line 670 `DiskStore`）
实现两个模板类：

- **`MemStore<IdType, elem_type>`** — 建图期容器，原始 float 在内存
  - HNSW 图结构（base_points / upper_points）
  - 建图期并发控制（striped locks）
  - 建图完成后可释放（compact 模式）

- **`DiskStore<IdType, elem_type>`** — 运行期容器，动态 `elem_size`
  - `Plain`：`elem_size = dim * sizeof(float)`
  - `PQCode`（可选）：`elem_size = pq.code_size`
  - `RabitQ`（可选）：`elem_size = rabitq.quant_size`

> **Plain-only 接入**：首版可以只实现 `Plain`，`DiskStore` 简化为单一
> `elem_size = dim * sizeof(float)` 的容器。PQ/RaBitQ 加入后再切动态 kind。

两个类必须**完整覆盖**步骤 1 列出的 store API。建议从 PG 实现逐方法对照。

### 步骤 9：GraphIndexPoint / elem_type

参照 `vexdb_duckdb/include/vex/vex_duck_point.hpp` 或 PG 对应实现，
提供 `elem_type`（即 `GraphIndexPoint`）：

- `tids` 列表（行 ID 集合，支持单节点对应多行）
- 锁支持（如果算法并发路径需要）
- 序列化 / 反序列化钩子

### 步骤 10（可选）：量化器 adapter

> **首版接入可跳过**。Plain `Distancer` 是平台无关的，不做量化也能完整
> 工作，只是索引内存占用 = `dim * sizeof(float) * N`。等基本功能稳定、
> 有内存压力后再补 PQ/RaBitQ 支持。

如果要接入，实现 `PQDistancer` 的 5 个方法（签名见
`common/quantizer/pq/pq_distancer.h`）：

```cpp
void configure_for_metric(size_t d, size_t M, size_t nbits,
                         Metric metric, DistPrecisionType precision);
void prepare(Relation index, void *metap);
void process(const char *query, void *metap);
void flush(Relation index, BlockNumber qtcode_block, bool enabling);
void hnsw_read_pq_center(Relation index, ProductQuantizer &target,
                        BlockNumber qtcode_block);
```

RaBitQ 类似（如果接入）。

参考实现：
- PG：`vexdb_pg/src/quantizer/pq_distancer.cpp`
- Duck：`vexdb_duckdb/quantizer/pq_adapter.cpp`

### 步骤 11（可选）：Metric enum 映射

如果新平台直接使用 `common::Metric` 作为索引成员类型，**可跳过此步**。

只有在以下情况才需要平台专属 enum + 映射函数：
- 平台类型系统要求 enum 必须在该平台 namespace 里
- 平台已有一个等价 metric enum 想复用（如该平台已有距离操作符）
- 想在平台层扩展 `common::Metric` 不支持的度量

PG/Duck 都有自己的 enum（`VexMetric`）+ `ToCommonMetric` 桥，**主要因历史原因**
（早期 `common` 还没统一 `Metric`）。新平台不需要重复这个错误。

如果确实需要，参考 `vexdb_duckdb/include/vex_distance.hpp::ToCommonMetric`：

```cpp
inline Metric ToCommonMetric(PlatformMetric m) {
    switch (m) {
        case PlatformMetric::L2:          return Metric::L2;
        case PlatformMetric::INNER_PRODUCT: return Metric::INNER_PRODUCT;
        case PlatformMetric::COSINE:      return Metric::COSINE;
    }
}
```

---

## 第三阶段：DB 集成（每平台特定）

### 步骤 12：vector SQL 类型

注册 `FLOATVECTOR(N)` / `HALFVECTOR(N)` / `INT8VECTOR(N)`（按需），包括：

- 类型定义与存储格式
- 输入 / 输出函数（text ↔ binary）
- 距离操作符：`<->`（L2）/ `<#>`（IP）/ `<=>`（Cosine）
- 类型转换函数（如 float[] → floatvector）

参考：
- PG：`vexdb_pg/include/data_type/floatvector.h` 等
- Duck：用内置 `FLOAT[N]` 类型，跳过此步

### 步骤 13：Index Access Method

把 GraphIndex 类接到本平台的 index AM 框架：

- 实现 create / drop / insert / delete / scan / vacuum / serialize /
  deserialize 钩子
- 注册 `CREATE INDEX ... USING vex_index` 语法
- 配置参数（`WITH (m=16, ef_construction=200, ...)`）

参考：
- PG：`vexdb_pg/src/graph_index/graph_index_am.cpp`（注册 `IndexAmRoutine`）
- Duck：`vexdb_duckdb/index/graph_index.cpp`（继承 `BoundIndex`）

### 步骤 14：查询优化

让 `ORDER BY dist(vec, q) LIMIT k` 走索引而不是顺序扫描：

- 解析器 / 重写器识别距离表达式
- 优化器生成 IndexScan 计划（key 是 query vector + LIMIT）
- IndexScan 节点把 top-k 结果按距离排序输出

参考：
- PG：`vexdb_pg/src/graph_index/graph_index_scan.cpp`
- Duck：`vexdb_duckdb/optimizer/vex_optimizer.cpp`

### 步骤 15：持久化

实现 serialize / deserialize：

- 元数据（dim / m / ef_construction / metric / entry_point）
- 邻居表 allocator 的 buffers
- 向量 allocator 的 buffers
- 量化器 codebook + codes（PQ/RaBitQ）
- `deleted_rids`（已删除行集合）
- id → ptr 映射

PG 用 metapage buffer chain，Duck 用 `IndexStorageInfo.options` 的 BLOB map，
新平台按本平台持久化惯例。

参考：
- PG：`vexdb_pg/src/graph_index/graph_index_xlog.cpp` + metapage
- Duck：`vexdb_duckdb/index/graph_index_disk.cpp`（`ExportStorageInfo` /
  `DeserializeFromStorage`）

---

## 第四阶段：测试

### 步骤 16：测试套件

参照 `tests/spec/pg/` 和 `tests/spec/duckdb/` 编写：

- **Smoke**：建索引、搜索、基本 DML
- **Recall**：对比 brute-force 基线，recall@k ≥ 阈值
- **Persistence**：build → checkpoint → restart → search，验证数据完整
  - **关键**：必须验证返回的 ID 是否正确，不能只验证 COUNT
- **Concurrency**：多线程 build + 并发 search
- **Quantizer（可选）**：PQ / RaBitQ 各路径（build/insert/delete/reload），
  仅在接入量化器后执行
- **边界**：dim=1、dim=high、m=1、ef=1、空表、单行表

---

## 工作量估算

| 模块 | 工作量（人天） |
|------|------------|
| compat 层（含 vtl allocator 对接） | 1-3 |
| 磁盘容器（DiskVector + PlainStore） | 5-10 |
| MemStore + DiskStore | 3-5 |
| 量化器 adapter（可选） | 0-2 |
| Metric enum 映射（可选） | 0-1 |
| vector 类型 + 操作符 | 3-5 |
| Index AM | 5-10 |
| 优化器重写 | 3-5 |
| 持久化 | 2-3 |
| 测试 | 3-5 |
| **合计** | **25-50** |

实际工作量与本平台 index AM 复杂度强相关。PG 因为有成熟的 index AM 框架，
整体偏低；自研 DB 可能偏高。

---

---

## 迁移自检清单

接入完成前，逐项确认：

- [ ] 步骤 1 列出的 25+ 个 store API 全部实现
- [ ] Smoke / Recall / Persistence / Concurrency / Quantizer / 边界测试通过
- [ ] Persistence 测试**断言返回的 ID 正确**（不只是 COUNT）
- [ ] `memory_limit` 能限制索引内存（buffer pool 集成验证）
- [ ] 索引内存占用被 DB 统计视图反映（内存跟踪验证）
- [ ] PQ 索引支持 build + INSERT + DELETE + VACUUM + reload 完整生命周期（**仅当接入量化器**）
- [ ] reload 后 recall 与 reload 前基本一致（图结构无损）
- [ ] 并发 build + 并发 search 不出错、不死锁
