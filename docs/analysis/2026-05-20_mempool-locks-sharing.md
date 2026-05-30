# MemPool 的 `Vector<RWBitLock> locks` 与 `slock_t mutex` 跨进程共享分析

**日期**：2026-05-20
**调研对象**：`include/graph_index/graph_index_storage.h:144-312` 的 `MemPool` 类
**核心问题**：成员是值而非指针，并行构建时多 backend 之间怎么共享这些锁？

---

## 一、结论先行

**这两个锁不跨进程共享 —— 它们就是 backend-local 的。** 截图代码的"不用指针"完全合理，因为它们**根本没打算跨进程**。

跨进程的同步走的是另一组对象：`GraphIndexShared` 里的 **striped LWLock 数组（值嵌入在 DSM 段里）**。那才是"不用指针怎么共享"的范例。

---

## 二、证据链

### 2.1 `MemPool` 实例本身在 backend-local 内存

| 位置 | 证据 |
|---|---|
| `src/graph_index_build.cpp:448` | `Holder<MemStore<>> mem_store;` — 直接嵌入 `GraphIndexBuildState`，placement new |
| `include/graph_index/graph_index_storage.h:334-336` | `MemPool vector_pool / basepoint_pool / upperpoint_pool;` — 值嵌入 `MemStore` |
| `src/graph_index_build.cpp:494` | `mem_store.emplace(...)` 用 leader 当前 MemoryContext |

`MemStore` 没有 `shm_toc_allocate` 过 —— 始终 backend-local。Worker 走 `_worker_main` 时**自己重新构造一份 MemStore**（attach 模式），各自一份实例。

### 2.2 `Vector<RWBitLock> locks` 的 backing buffer 走 `palloc`

```cpp
// vtl/allocator:41,64-69
CtxAllocatorBase() : _ctx(CurrentMemoryContext) {}
pointer allocate(size_type num, ...) {
    MemoryContextSwitchTo(_ctx);
    pointer p = (pointer)palloc(num * sizeof(T));
    ...
}
```

`palloc` 永远是 backend 私有 MemoryContext。Vector 的 `_start`/`_end` 指针指向私有堆 —— 这块内存**根本没映射到其他 backend 的虚拟地址空间**。

`RWBitLock` 内部递归依赖 Vector：

```cpp
// vtl/bitlock.hpp:67-95
class RWBitLock {
    Vector<std::atomic<uint16>> _rlocks;  // 又一个 palloc buffer
    BitLock _wlock;                        // BitLock 内部还有 Vector<atomic<size_t>> _locks
};
```

每一层 Vector 都把 buffer 钉死在 backend-local heap。

### 2.3 `ParallelAttach` ctor 明确说不用 locks

```cpp
// graph_index_storage.h:192-207
MemPool(AllocatorState *state, const ParallelAttach &attach)
    : locks(pre_alloc_vec_size),   // 仍然分配, 但只是预留槽
      vec(pre_alloc_vec_size),
      ...
{
    vec.push_back(state->base + attach.pre_alloc_chunk_offset);
    /* locks left empty: parallel build uses shared striped LWLocks
     * (held in GraphIndexShared) instead of these per-chunk
     * RWBitLocks; lock_elem / unlock_elem callers must route
     * through the shared striped locks for correctness. */
    SpinLockInit(&mutex);
}
```

**注释挑明了**：worker 不调 MemPool 自带的 RWBitLock，跨 backend 同步走 `GraphIndexShared.base_point_locks[]` 这种 striped LWLock。

### 2.4 真正跨进程的锁住在 `GraphIndexShared`

`GraphIndexShared` 由 `shm_toc_allocate` 分配到 DSM 段（`graph_index_build.cpp:971`），内部成员都是**值嵌入**：

```cpp
// graph_index_build.cpp:51-137
struct GraphIndexShared {
    AllocatorState alloc_state;            // 值
    LWLock allocator_lock;                 // 值
    LWLock flush_lock;                     // 值
    LWLockPadded base_point_locks[64];     // 定长数组值嵌入  ← striped LWLocks
    LWLockPadded upper_point_locks[64];    // 同上
    std::atomic<uint32_t> num_vectors;     // 值
    std::atomic<uint64_t> tuples_done;     // 值
    GraphIndexEntryInfo entry_info;        // POD 值嵌入
    ConditionVariable workersdonecv;       // PG 原语, 值
    slock_t mutex;                         // 值
    ...
};
```

注释里有一句关键的（line 86-87）：

> *"Embedded inline so workers see them at the same physical offset within the DSM region as the leader."*

这才是"不用指针怎么共享"的答案 —— 但**它在 `GraphIndexShared` 里，不在 `MemPool` 里**。

---

## 三、"值嵌入" 跨进程共享的本质

PG DSM 跨进程共享对象需满足**三条件**（缺一不可）：

1. **对象本身被分到 DSM 段**
   `shm_toc_allocate` / `dsa_allocate`；worker 用 `shm_toc_attach + lookup` 拿 base 指针。

2. **成员用相对 offset 访问，不能藏指针**
   值嵌入（`T member;`）或定长数组（`T arr[N];`）— offset 编译期常量。
   worker 看到的 `&shared->base_point_locks[0]` = `attach_base + offsetof(base_point_locks)`，offset 跨进程一致，物理位置同一份。

3. **成员类型不依赖虚拟地址**
   不藏 backend-local heap 指针；不依赖 vtable（vtable 指向 .text 段，跨 backend 若 ASLR 启用则不同地址）；不持有 backend-local handle（Relation、MemoryContext）。

满足这三条 → 不需要指针，值嵌入即跨进程。
任一不满足 → 即使改成指针也救不了，指针指的还是私有堆。

### 3.1 类型 DSM-portability 速查

| 类型 | DSM-safe? | 原因 |
|---|---|---|
| `slock_t` | ✅ | 一个 `volatile uint8`/`pthread_spinlock_t`，纯值 |
| `std::atomic<T>` (T = POD) | ✅ | atomic 内部就是 T，无指针 |
| `LWLock` / `LWLockPadded` | ✅ | PG 设计本就为 DSM/shmem 而生 |
| `ConditionVariable` | ✅ | PG shmem 原语，间接寻址走 proc array |
| `LWLockPadded arr[64]` | ✅ | 定长数组值嵌入 |
| POD struct（无指针成员） | ✅ | 整体平铺 |
| `pthread_mutex_t` / `pthread_rwlock_t` | ⚠️ | 必须用 `PSHARED` 属性初始化；vexdb_lite 没用 |
| `vtl::Vector<T>` (默认 `CtxAllocator`) | ❌ | `_start` 指针指 palloc heap |
| `std::vector<T>` | ❌ | 同上 |
| `MemoryContext` 字段 | ❌ | 指向 backend-local linked list |
| `Relation` 字段 | ❌ | RelationData backend-local cache |
| 含虚函数的类 | ⚠️ | vptr 指向 .text；通常 OK（同二进制 .text 同地址），ASLR 极端情况下 break |

### 3.2 截图里的 `vec[i].buf` 是个干净的中间形态

```cpp
// MemPool::ParallelAttach ctor
vec.push_back(state->base + attach.pre_alloc_chunk_offset);
```

`vec` 自己 backend-local（不跨进程，没必要跨），但 `vec[0].buf` 是 `worker 自己看到的 attach_base + offset` —— 每个 backend 自己重算 buf 地址，但物理 chunk 是同一份。这是 "**容器 local + 容器指向 DSM 数据**" 的混合模式，比把 Vector 整个塞进 DSM 干净得多。

---

## 四、当前设计的实际分层

```
┌────────────────────── DSM segment (shm_toc_allocate) ──────────────────────┐
│                                                                             │
│  GraphIndexShared {                                                         │
│      AllocatorState alloc_state;        ← 值, offset 跨进程一致             │
│      LWLockPadded base_point_locks[64]; ← 值, striped per-id 锁             │
│      LWLockPadded upper_point_locks[64];                                    │
│      atomic<uint32_t> num_vectors;      ← 值, assign_vector_id fetch_add    │
│      atomic<uint32_t> num_uppers;                                           │
│      GraphIndexEntryInfo entry_info;    ← 值, POD                           │
│      LWLock entry_lock;                 ← 值                                │
│      ...                                                                    │
│  }                                                                          │
│                                                                             │
│  [Chunk buffers @ alloc_state.base + offset]  ← 真共享数据                  │
│  [id_to_tid array]                            ← worker 写, leader 读        │
│  [ready_base / ready_upper bitmaps]           ← release/acquire 屏障        │
└─────────────────────────────────────────────────────────────────────────────┘
         ▲                  ▲                  ▲                  ▲
         │ shm_toc_attach   │ shm_toc_attach   │ shm_toc_attach   │
         │                  │                  │                  │
┌────────┴───────┐ ┌────────┴───────┐ ┌────────┴───────┐ ┌────────┴───────┐
│ Leader backend │ │ Worker1 backend│ │ Worker2 backend│ │ Worker3 backend│
│                │ │                │ │                │ │                │
│ MemStore {     │ │ MemStore {     │ │ MemStore {     │ │ MemStore {     │
│   MemPool {    │ │   MemPool {    │ │   MemPool {    │ │   MemPool {    │
│     locks      │ │     locks      │ │     locks      │ │     locks      │ ← backend-local
│      (palloc'd)│ │      (palloc'd)│ │      (palloc'd)│ │      (palloc'd)│   互不可见
│     mutex      │ │     mutex      │ │     mutex      │ │     mutex      │
│     vec[0].buf │ │     vec[0].buf │ │     vec[0].buf │ │     vec[0].buf │ ← 各自算出
│       ↓        │ │       ↓        │ │       ↓        │ │       ↓        │   的 DSM 地址
│     (指向 DSM) │ │     (指向 DSM) │ │     (指向 DSM) │ │     (指向 DSM) │   (物理同一份)
│   }            │ │   }            │ │   }            │ │   }            │
│   shared_*     │ │   shared_*     │ │   shared_*     │ │   shared_*     │ ← 各自存的
│   (pointers    │ │   (pointers    │ │   (pointers    │ │   (pointers    │   指向 DSM 的
│    into DSM)   │ │    into DSM)   │ │    into DSM)   │ │    into DSM)   │   裸指针
│ }              │ │ }              │ │ }              │ │ }              │
└────────────────┘ └────────────────┘ └────────────────┘ └────────────────┘
```

**总结**：
- 控制流和元数据外壳 → backend-local（palloc，便宜）
- 真共享数据（chunk、id_to_tid、ready bitmap）→ DSM
- 跨 backend 同步原语（striped LWLock、atomic counter、ConditionVariable）→ DSM 值嵌入

---

## 五、潜在风险点

### 5.1 `extend()` / `lock_elem()` 在 worker 路径必须不被调到

`graph_index_storage.h:251-271` `lock_elem` 走 `locks[chunk_no].rlock()`，但 worker 的 `locks` 内部 RWBitLock 槽是空构造的（line 192-207 ctor 调 `locks(pre_alloc_vec_size)` 仅分配 Vector 容量，未 `emplace_back` 任何 RWBitLock）—— 一旦 worker reach 到这一行 `locks[chunk_no]` 立刻 OOB / SEGV。

注释提到 "this path is single-thread-only"，但 **runtime 没 `Assert(!is_parallel_worker)` 兜底**。任何对 lock_elem 调用路径的重构都得手动核查不要把 worker 拖进来。这是历史上栽过跟头的位置（见 `project_parallel_build_race_fix.md`）。

### 5.2 `pre_alloc_vec_size = 20000` 是个硬上限

`graph_index_storage.h:221` `Assert(chunk_no < pre_alloc_vec_size)`。Vector 预 reserve 20000 容量后不会再 reallocate（避免 `_start` 移位破坏 worker 看到的旧地址 —— 虽然 worker 根本不读 leader 的 vec，但这是个隐含不变量）。

20000 × min chunk size (~MB 级) = 几十 GB 上限。超出会断 Assert 而不是降级，建议改 `ereport(ERROR)`。

### 5.3 `PoolSharedState` (storage.h:62-87) 是死代码

整个 struct 定义了但**全仓零引用**。它是早期"per-pool 一把 LWLock"的方案，被后来的"striped 64 LWLocks per id type"取代。建议直接删，避免未来读代码的人误以为它是活路径。

### 5.4 worker 的 `MemPool.locks(pre_alloc_vec_size)` 仍走了 palloc

ParallelAttach ctor 里 `locks(pre_alloc_vec_size)` 实际上**白浪费了一份 backend-local Vector 容量分配** —— 20000 个 RWBitLock 槽的容器外壳 + 0 个实例。对一个并行 worker 来说 `sizeof(Vector<RWBitLock>) × 1 + sizeof(RWBitLock *) × 20000` 容量 = 几百 KB private heap 浪费 × N worker。

建议在 ParallelAttach 路径下 `locks(0)` 不预分配，或者把 `auto_init=false` 加上（Vector 模板支持，看 `vtl/vector:25`）。

---

## 六、如果将来要让 `MemPool.locks` 真跨进程

不是"把字段从值改指针"的事，是 allocator 重选 + 容器策略调整：

| 路径 | 做法 | 代价 |
|---|---|---|
| **A. 定长数组** | `RWBitLock locks[MAX_CHUNKS]` + RWBitLock 内部 BitLock 也改成定长 `atomic<size_t> _locks[N]` | 内存浪费大（按上限留），所有 chunk 一刀切；递归改造 BitLock |
| **B. 用 SharedCtxAllocator** | `vtl/shared_allocator` 已存在，让 Vector buffer 走 DSM | reallocate 路径要 DSM-aware；DSM 段大小估算更复杂；多线程 reallocate 还得加 DSM allocator 锁 |
| **C. 保持现状 + 文档化** | 把 5.1 的不变量写进 `MemPool` doc comment + 加 Assert | 0 改动，最安全 |

当前选 backend-local + striped LWLock 模式是合理折中。主库（openGauss-vector-main）用 `palloc_huge(maintenance_work_mem)` 一次性给死、不估算，lite 用 DSM 必须预估 —— 那条路径长期想学主库 refactor，到时再统一调 allocator 策略（参 `project_parallel_build_design.md`）。

---

## 七、一句话回答

> "`Vector<RWBitLock> locks; slock_t mutex;` 不用指针怎么共享" —— **不共享，单进程语义**。跨进程同步走 `GraphIndexShared.base_point_locks[64]` 这种 LWLockPadded 定长数组**值嵌入 DSM 段**，靠"DSM 分配 + 编译期 offset + 类型无虚拟地址依赖"三条件实现。

---

## 附录 A：建议行动清单

| 编号 | 行动 | 优先级 |
|---|---|---|
| A1 | 删 `PoolSharedState` 死代码（5.3） | low |
| A2 | `MemPool::lock_elem` / `extend` 加 `Assert(!is_parallel_worker_mode)`（5.1） | medium |
| A3 | ParallelAttach ctor 里 `locks(0)` 省 palloc（5.4） | low |
| A4 | `Assert(chunk_no < pre_alloc_vec_size)` 改 `ereport(ERROR)` + clear errmsg（5.2） | medium |
| A5 | 给 `MemPool` 写 doc comment 把"locks/mutex 是 single-thread-only"不变量写明 | medium |

## 附录 B：相关 memory 锚点

- [[project_parallel_build_design]] PG 并行构建内存模型 vs 主库
- [[project_parallel_build_race_fix]] parallel build publish race stopgap
- [[feedback_check_main_repo]] 异常/bug 必看主库
