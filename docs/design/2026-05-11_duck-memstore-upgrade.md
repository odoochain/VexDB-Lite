# Duck MemStore 升级到主库版本（含并行 build 落地）

**日期**：2026-05-11
**状态**：设计中
**前置上下文**：调研发现 duck 当前简化版 MemStore（`vex_graph_index_depend_duck.hpp:251`，~440 行）**完全无锁**——`lock_point` / `release_entry_lock` 是空函数，`std::vector::emplace_back` 非原子。直接接 std::thread 池调 `algo.insert` 会段错误。

替代旧文档 `2026-05-11_duck-parallel-build-wireup.md`（已删除）。

---

## 1. 决策清单（2026-05-11 锁定）

| # | 决策 | 选择 | 理由 |
|---|---|---|---|
| 1 | Duck 并行 build 路径 | **改用主库完整 MemStore** | 主库 MemStore (1428 行) 带完整 LWLock/SpinLock/RWBitLock 协议，已在 PG 路径验证 |
| 2 | LWLock shim 实现 | **SimpleRWLock + 扩展 unified `unlock()`** | atomic-based，根据 state 自动判断 shared/exclusive，无需 thread_local 记账 |
| 3 | 持久化路径 | **X.3: std::malloc + 手动 serialize**（修订）| 原计划 Y 因 CRITICAL 3 (FixedSizeAllocator segment_size 上限 256KB vs 主库 chunk MB 级) 不可行；改用 std::malloc 拿 chunk，持久化时手动 dump 整个 MemStore，顺带修 reload bug |
| 4 | 旧 wire-up 文档 | **直接删除** | 基于错误前提（"algorithm 自带锁"） |

### CRITICAL 验证结论（2026-05-11）

三个 CRITICAL 风险已在动代码前验证完毕：

| # | 风险 | 状态 | 解决方案 |
|---|---|---|---|
| 1 | LWLockRelease 不知 shared/exclusive 模式 | ✅ 解决 | SimpleRWLock 扩展统一 `unlock()`：state==-1 → store(0) writer release；state>0 → fetch_sub(1) reader release。原子状态机自动判断，无需 caller 传 mode 也无需 thread_local |
| 2 | FixedSizeAllocator buffer evict 风险 | ⏭️ 不再相关 | X.3 不用 FixedSizeAllocator，问题作废 |
| 3 | chunk_size 上限 | ✅ 通过架构调整解决 | 选 X.3：MemPool 用 std::malloc 直接拿 GB 级大块，绕开 FixedSizeAllocator 256KB segment 上限 |

---

## 2. 问题陈述

### 当前 duck 的并发死锁

| 接口 | 当前 duck 实现 | 主库 MemStore 实现 | 并发问题 |
|---|---|---|---|
| `get_entry` | 直接返回 `entry_info` | LWLockAcquire(entry_lock, SHARED) | 读到部分更新的 entry_info |
| `release_entry_lock` | 空函数 | LWLockRelease | — |
| `lock_point` | 空函数 | RWBitLock per-chunk per-element | 邻居数组写者竞争 |
| `assign_vector_id<true>` | `T id = T(elems.size()); elems.emplace_back();` | `num_vectors.fetch_add(1)` atomic | std::vector race → SIGSEGV |
| `add_async_id` | `async_list.push_back` | atomic push | std::vector race |
| `add_elem` | `std::vector::resize` | LWLock(elems_veclock, EXCLUSIVE) + chunk extend | std::vector::resize 重新分配指针失效 |
| `add_vector` | `std::vector::resize` | MemPool chunk-based + bump alloc | 同上 |

**N 个 worker 并发调 `algo.insert(ctx)` → MemStore 内部多个 std::vector 同时被 resize / emplace → segfault**。

### 为什么不能简单加 std::mutex 大锁

会变成串行（决策 1 的"短期方案"选项已排除）。HNSW 算法的好处是 search 可并发——主库设计是**细粒度锁**（per-element + chunk-level RWBitLock），这才有并行加速。

---

## 3. 设计方案：三层重构

### 3.1 Layer A: PG 锁原语 shim

duck 编译时，所有 PG 锁原语 typedef 到 duck-friendly 实现：

```cpp
// vexdb_duckdb/include/vex/duck_pg_shim.hpp（新增）
#pragma once

#include "vex_simple_rwlock.hpp"   // 从 main 分支搬来的 SimpleRWLock

namespace duck_shim {

// SimpleRWLock 接口：lock() / unlock() / lock_shared() / unlock_shared()

using LWLock = SimpleRWLock;

struct LWLockPadded {
    SimpleRWLock lock;
    char padding[64 - sizeof(SimpleRWLock) % 64];  // cache line align
};

enum LWLockMode {
    LW_SHARED,
    LW_EXCLUSIVE,
};

inline void LWLockAcquire(LWLock* l, LWLockMode mode) noexcept {
    if (mode == LW_SHARED) l->lock_shared();
    else                   l->lock();
}

inline void LWLockRelease(LWLock* l) noexcept {
    // 这里要根据持有的类型释放——主库代码不区分，因为 PG LWLock 内部记
    // SimpleRWLock 也不区分 release_shared vs release_exclusive？需检查
    // 方案：l->unlock_dispatch() 内部判断
    l->unlock_either();
}

inline void LWLockInitialize(LWLock* l, int /*tranche*/) noexcept {
    new (l) SimpleRWLock();
}

// SpinLock shim
using slock_t = std::atomic_flag;
inline void SpinLockInit(slock_t* s)    noexcept { s->clear(); }
inline void SpinLockAcquire(slock_t* s) noexcept {
    while (s->test_and_set(std::memory_order_acquire)) {
        // backoff
    }
}
inline void SpinLockRelease(slock_t* s) noexcept {
    s->clear(std::memory_order_release);
}

}  // namespace duck_shim

// 主库代码看到的全局符号
#if defined(PG_VEXDB_TARGET_DUCK)
using LWLock         = duck_shim::LWLock;
using LWLockPadded   = duck_shim::LWLockPadded;
using slock_t        = duck_shim::slock_t;
constexpr auto LW_SHARED    = duck_shim::LW_SHARED;
constexpr auto LW_EXCLUSIVE = duck_shim::LW_EXCLUSIVE;
using duck_shim::LWLockAcquire;
using duck_shim::LWLockRelease;
using duck_shim::LWLockInitialize;
using duck_shim::SpinLockInit;
using duck_shim::SpinLockAcquire;
using duck_shim::SpinLockRelease;
#endif
```

**关键问题**：`LWLockRelease` 在 PG 内部不区分 shared/exclusive（PG LWLock 内部记账）。SimpleRWLock 需要分两个方法：`unlock()` 和 `unlock_shared()`。**调用方必须知道自己持有什么模式**。

主库代码 `release_entry_lock(bool shared)` 已经传了 shared 标志——shim 里要从 caller 拿到这个信息。可能需要在 shim 层加一个 RAII handle 或者在 caller 端记录持有模式。

**待原型阶段验证**：是否所有 `LWLockRelease` 调用点都能拿到 shared/exclusive 信息？如果不能，SimpleRWLock 内部要加状态记账（违反纯 atomic 设计）。

### 3.2 Layer B: MemPool 直接 std::malloc（X.3 方案）

主库 MemPool 用 chunk-based bump allocator，单 chunk 设计为 MB 级别。由于 DuckDB FixedSizeAllocator segment_size 上限 256KB，无法承载 MB 级 chunk（CRITICAL 3）。

**X.3 选择**：MemPool 改用 std::malloc 直接拿大块内存，**完全绕开 FixedSizeAllocator**。

```cpp
// duck 编译路径：bump_alloc → std::aligned_alloc
#if defined(PG_VEXDB_TARGET_DUCK)
class DuckBumpAllocator {
    // 维护 chunk 列表，析构时统一 free
    std::vector<void*> chunks_;
    size_t total_allocated_ = 0;
    size_t budget_;
public:
    void* alloc(size_t bytes, size_t alignment) {
        void* p = std::aligned_alloc(alignment,
                                     (bytes + alignment - 1) & ~(alignment - 1));
        if (!p) throw std::bad_alloc();
        chunks_.push_back(p);
        total_allocated_ += bytes;
        return p;
    }
    ~DuckBumpAllocator() {
        for (auto* p : chunks_) std::free(p);
    }
    size_t total_size() const { return total_allocated_; }
};
#endif
```

`AllocatorState` 在 duck 侧 typedef 到 `DuckBumpAllocator`；`bump_alloc(size, state)` shim 调 `state->alloc`。

**优势**：
1. 主库 MemPool 代码原样可用，零改动
2. 没有 chunk_size 上限，主库默认 MB 级 chunk 直接工作
3. raw ptr 永远稳定（std::malloc 不 evict）
4. 实现最简单

**代价（持久化）**：std::malloc 内存不通过 DuckDB BufferManager 管理 → 失去 WAL / checkpoint / 增量 flush 集成。所有持久化必须**手动 serialize 整个 MemStore 到磁盘文件**（见 Layer C-bis）。

### 3.2-bis Layer B': 持久化层（X.3 配套）

由于 MemStore 内存与 BufferManager 解耦，需要新的持久化机制：

```cpp
// vexdb_duckdb/index/duck_memstore_persistence.hpp
class DuckMemStorePersistence {
public:
    // Build 完成时调用：把 MemStore 整个序列化到磁盘
    void Serialize(const MemStore<>& store, const std::string& file_path);

    // 打开索引时调用：从磁盘反序列化重建 MemStore
    void Deserialize(MemStore<>& store, const std::string& file_path);
};

// 文件格式（自描述、可演进）：
// [header magic + version + dim + m + ef_construction]
// [entry_info: id + cur_layer_idx + level]
// [vector_pool: num_chunks + each chunk size + data...]
// [basepoint_pool: ...]
// [upperpoint_pool: ...]
// [elems vector: count + each elem...]
// [footer checksum]
```

**优势**：
- memory 里 `vexdb_duck_reload_gap.md` 描述 5 个 restart 测试失败（DeserializeFromStorage 缺 elems / 邻居数据）—— 现有部分持久化已经 broken，X.3 等于**重写**一个完整的，**顺带修了 reload bug**
- 文件格式自定义，无需兼容 DuckDB 原生格式
- 单文件 dump，简单可靠

**代价**：
- 失去增量持久化（INSERT 后必须重 dump 整个 MemStore？需要决策）
- 失去 DuckDB 自动 checkpoint，要 hook 进 DuckDB extension 的 commit/checkpoint callback

**针对增量持久化**：HNSW 索引通常一次性 build（CREATE INDEX），之后基本只读，少量 INSERT。可以接受"每 N 次 INSERT 或 checkpoint 时整体 re-dump"——HNSW 索引大小通常 GB 内，写盘耗时秒级，对偶发增量可以接受。Phase 2 优化为增量序列化。

### 3.3 Layer C: 并行 build wire-up

跟之前的 wire-up 设计一致（80 行 thread 池），但**前提条件**：Layer A + Layer B 已经让 MemStore 线程安全。

```cpp
void GraphIndex::Build(const float* src, const std::vector<row_t>& row_ids) {
    if (row_ids.empty()) return;

    int requested = build_threads_;           // 默认 1
    const idx_t n = row_ids.size();
    int n_workers = std::min<int>(requested, n);
    if (n_workers < 1) n_workers = 1;

    RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, runtime_->store,
                    [&](auto& algo) {
        using AlgoT = std::decay_t<decltype(algo)>;

        // Phase A: 串行插入第 0 点
        idx_t start_index = 0;
        if (runtime_->store.get_vector_num() == 0) {
            PointExtensionContext ctx0;
            ItemPointerData tid0{};
            tid0.row_id = row_ids[0];
            const char* q0 = reinterpret_cast<const char*>(src);
            typename AlgoT::InsertContextBase ictx(ctx0, q0, &tid0);
            algo.insert(ictx);
            start_index = 1;
        }

        if (n_workers <= 1 || start_index >= n) {
            for (idx_t i = start_index; i < n; i++) {
                PointExtensionContext ctx;
                ItemPointerData tid{};
                tid.row_id = row_ids[i];
                const char* q = reinterpret_cast<const char*>(src + i * dimension_);
                typename AlgoT::InsertContextBase ictx(ctx, q, &tid);
                algo.insert(ictx);
            }
            return;
        }

        // Phase B: 并发插入剩余
        std::vector<std::thread> workers;
        std::vector<std::exception_ptr> errors(n_workers);
        const idx_t remaining = n - start_index;
        const idx_t per = remaining / n_workers;
        const idx_t rem = remaining % n_workers;
        idx_t offset = start_index;
        for (int t = 0; t < n_workers; t++) {
            idx_t count = per + (t < (int)rem ? 1 : 0);
            idx_t s = offset, e = offset + count;
            offset = e;
            workers.emplace_back([&, t, s, e]() {
                try {
                    for (idx_t i = s; i < e; i++) {
                        PointExtensionContext ctx;
                        ItemPointerData tid{};
                        tid.row_id = row_ids[i];
                        const char* q = reinterpret_cast<const char*>(src + i * dimension_);
                        typename AlgoT::InsertContextBase ictx(ctx, q, &tid);
                        algo.insert(ictx);
                    }
                } catch (...) { errors[t] = std::current_exception(); }
            });
        }
        for (auto& w : workers) w.join();
        for (auto& ep : errors) if (ep) std::rethrow_exception(ep);
    });

    if (pq_m_ > 0 && !row_ids.empty()) TrainAndEncodePQ(src, row_ids);
    if (compact_mode_ && pq_use_)      ReleaseRawVectors();
}
```

---

## 4. 迁移 Phase

| Phase | 内容 | 估时 | 验证 |
|---|---|---|---|
| **P1** | SimpleRWLock 从 main 分支搬到 `vexdb_duckdb/include/vex_simple_rwlock.hpp`，**扩展统一 `unlock()`** 自动判断 shared/exclusive | 0.5 天 | unit test：多线程 reader + writer 混合 |
| **P2** | 写 `duck_pg_shim.hpp` shim 层（LWLock + SpinLock + slock_t） | 1 天 | 编译过 + 简单互斥测试 |
| **P3** | 写 `DuckBumpAllocator`（std::aligned_alloc + chunks vector）取代 PG palloc | 1 天 | MemStore 单线程跑通 |
| **P4** | 替换 duck 简化 MemStore → 主库 `MemStore<uint32, GraphIndexPoint>` | 3-5 天 | 现有 105 个 spec 单线程通过 |
| **P5** | 写持久化层 `DuckMemStorePersistence`（serialize/deserialize MemStore 到单文件） | 4-6 天 | 5 个 restart 测试从 fail → pass |
| **P6** | wire-up 并行 build（Layer C，按上面骨架） | 1 天 | threads=4 不崩 |
| **P7** | Stress test：100 次空图 build × threads=4 × 50k vectors | 1 天 | 无 SIGSEGV，召回率稳定 |
| **P8** | 新增 spec `graph_index_parallel_build.yaml` | 0.5 天 | 跨引擎一致性 |
| **P9** | 性能 baseline + bug fix | 2-3 天 | threads=4 加速 ≥ 2.5x |
| **总计** | | **14-19 天** |

每个 Phase 可以独立 commit，P3 改简单了（std::malloc 直接，没有 FixedSizeAllocator 集成复杂度）；P5 增加了（要写完整持久化）。总工作量持平。

P4-P5 之间是最大风险点（替换 MemStore + 重写持久化）。

---

## 5. 风险

### CRITICAL
1. ~~**LWLockRelease 不知道 shared/exclusive 模式**~~ ✅ 已通过 SimpleRWLock 扩展 unified `unlock()` 解决
2. ~~**FixedSizeAllocator buffer evict 风险**~~ ⏭️ X.3 不用 FixedSizeAllocator，问题作废
3. ~~**FixedSizeAllocator segment_size 上限**~~ ✅ 改用 X.3 std::malloc 直接绕开

### MAJOR
4. **持久化兼容性**：现有 duck 索引文件（FixedSizeAllocator 序列化）跟新 MemStore 格式不兼容。需要：
   - 选项 A: 升级时强制 reindex（用户接受）
   - 选项 B: deserialize 时旧格式 → 新格式自动转换（工作量大）
5. **Build 期内存占用**：FixedSizeAllocator 持续 pin 所有 chunk → 大索引可能挤爆 BufferManager。需要观察 `vex_memory_budget` 配置是否够。
6. **跨 backend 测试**：duck 改用主库 MemStore 后，**两边走完全相同的代码路径**，spec 测试一致性应当变好。但 PG 侧目前 parallel_workers gated off，要小心 PG 这边不要顺手开了。
7. **PointExtensionContext**：duck 当前是空 struct，主库版本承载什么？需 P4 阶段确认。

### MINOR
8. **SimpleRWLock 写者优先 vs PG LWLock**：PG LWLock 有更复杂的 priority 协议。SimpleRWLock 简单的 writer_waiters 计数应当够用，但极端竞争场景下可能行为差异——可以用 stress test 验证。

---

## 6. 不在本设计范围内

- **PG 侧并行 build**：仍 gated off，本设计不动；PG 那边需要单独把 MemStore 搬到 DSM 段
- **Append() 并行**：Phase 2 处理（涉及 DuckDB Sink pipeline）
- **算法层改进**（SelectNeighbors / SIMD search）
- **跨 backend 索引文件兼容**（除非 P5 顺便解决）

---

## 7. 验证计划

| 阶段 | 验证 |
|---|---|
| P1-P2 | shim 单元测试，主要看 LWLockRelease 模式问题 |
| P3 | 跑现有所有 spec（105 个），单线程下零回归 |
| P4 | spec 一致性：duck spec 输出 = PG spec 输出（之前 DuckDB 100%、PG 72%，预期 P4 后 duck 仍 100%，PG 不动） |
| P5 | 5 个 restart 测试从 fail → pass |
| P6 | threads=1 跑通；threads=4 跑通无崩溃 |
| P7 | 100 次 stress test 全过 |
| P9 | sift100k baseline：threads=4 加速 ≥ 2.5x；threads=8 加速 ≥ 3.5x |

---

## 8. 参考

- 主库 MemStore: `common/graph_index/graph_index_storage.h:65-540`
- Duck 当前简化 MemStore: `vexdb_duckdb/include/vex_graph_index_depend_duck.hpp:251-440`
- SimpleRWLock: `vexdb_duckdb/include/vex_simple_rwlock.hpp`
- Duck FixedSizeAllocator: DuckDB 内置 `src/include/duckdb/execution/index/fixed_size_allocator.hpp`
- Algorithm.insert 协议: `common/graph_index/graph_index_algorithm.h:273-435`
- 现有 Build 调用点（要改）: `vexdb_duckdb/index/graph_index.cpp:~340`
