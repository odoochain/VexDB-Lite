# Parallel Build Race Analysis — SIFT1M pw 数维度 recall 不稳

**日期**:2026-05-18
**触发场景**:SIFT1M m=16 efc=128 efs=200,parallel_workers 0/1/2/4/8
**症状**:recall 非单调,pw=2 暴跌到 83.57%(单线程 99.33%)

## 实测数据

### Fix 前(`main` + commit `e55309bbe0`)

| pw | build(s) | recall@10 | qps |
|---:|---:|---:|---:|
| 0(单线程) | 1220.8 | **0.9933** | 120.7 |
| 1 | 663.9 | 0.9779 | 122.5 |
| **2** | 451.5 | **0.8357 ⚠** | 126.6 |
| 4 | 289.2 | 0.9824 | 64.5 |
| 8 | 185.2 | 0.9905 | 31.4 |

### Fix v1 — `insert_new_point` 加 publish EXCLUSIVE lock(`algorithm.h:891`)

| pw | recall@10 | 备注 |
|---:|---:|---|
| 0 | 0.9933 | 同 baseline |
| 1 | 0.9480 | ↓ 3pp |
| 2 | **0.9935** | ↑ 16pp 修好 |
| 4 | **0.8513 ⚠** | ↓ 13pp 大退化 |
| 8 | 0.9910 | 持平 |

→ race 模式被 fix 推到 pw=4 上。说明还有其他 race window。

## 根因(4 agent 并行分析共识)

### 主库的 publish invariant(`openGauss-vector-main`)

`hnswbuild.cpp:1039` → `hnswbuild.cpp:852-858 AddElementInMemory`:

```
T1 (worker 私有):  element->value = ptr;            // 写 vec
T2 (worker 私有):  HnswFindElementNeighbors(...);   // 写 element->neighbors
T3 (publish):     SpinLockAcquire(&graph->lock);
                  element->next = graph->head;
                  graph->head = element;            // ← element 此时才可见
                  SpinLockRelease(&graph->lock);
T4:               UpdateNeighborsInMemory(...);     // LW_EXCLUSIVE per nbr
```

**核心 invariant**:任何能 walk 到 element 的 worker(通过 graph->head 链表),看到 element 时 `.value` 一定已经写完。spinlock release 是 memory fence。**读 vec 不需要 lock**。

### lite 的 publish 倒序(bug 源)

`graph_index_algorithm.h:879-924 insert_new_point` + `graph_index_storage.h:577-591 assign_vector_id`:

```
T1: id = atomic_fetch_add(next_base_id);  // ← id 立即在 max_id 可见!
T2: add_async_id(id);
T3: add_elem(ctx, id, tid);
T4: add_basepoint(id, ...);                // 写 basepoint_pool[id]
T5: add_vector(id, query);                 // 写 vector_pool[id]
T6: update_reverse_edges(...);             // EXCLUSIVE per nbr,set_neighbor
```

**Race window**:T1 到 T5 之间,id 已对其他 worker 可见(`max_id` 暴露),但 vec_pool[id] 没写。另一 worker 的 `search_layer` 若读 `vector_pool[id]`,拿到 bump_alloc 切的 stale 字节(DSM 不 zero-fill)→ distance 计算乱数 → graph 拓扑被污染。

主库的 publish 是"接进 graph->head"(走链才能 reach),lite 的 publish 是"atomic 增 id"(`max_id` 一变全可见)— **顺序相反**。

## 已尝试 fix(部分)

### 1. `insert_new_point` publish EXCLUSIVE lock(`algorithm.h:891-924`)

把 `add_basepoint + add_vector` 包进 `lock_point<true, false>(id)` EXCL,跟 reader 的 `lock_point<true, true>` SHARED 配对补 happens-before。

**效果**:pw=2 修好,但 pw=4 退化(race 转移)。

### 2. `get_neighbors_data` per-id SHARED lock(`algorithm.h:1495`)

每个 `point.id` 拿 SHARED lock + 读 vec + release。配对 publish EXCLUSIVE。

### 3. reorder `add_vector` 在 `add_basepoint` 之前

让 vec 先于邻居数组写,保证读者看到邻居数组时 vec 已 ready。

### Lock 覆盖矩阵(Agent A finding)

| 行为 | 位置 | lock 状态 |
|---|---|---|
| 写 vector_pool[id] | algorithm.h:911 | EXCL on id(✓) |
| 读 vector_pool[id] | algorithm.h:1515 get_neighbors_data | SHARED on id(✓ 我加的)|
| **读 vector_pool[nbr.id]** | **search_layer:1028 `get_distance_batch`** | **无 lock ⚠ 关键漏洞** |
| 写 basepoint_pool[id] | algorithm.h:912 | EXCL on id(✓)|
| 读 basepoint_pool[id] | storage.h:630/661 get_neighbors / get_point_info | SHARED on cur_layer_idx(✓ caller)|
| 读 vector_pool[nbr.id] | algorithm.h:1268 elem_closer | 在 nbr.cur_layer_idx EXCL 内,**nbr.id stripe 不一定相同** ⚠ |
| 写 first_basepoint / first_upperpoint | algorithm.h:311/357 | 在 entry_lock SHARED 下,**无 publish lock** ⚠ |

### 为什么 fix 后 pw=4 反而坏(双峰转移)

race rate ≈ `P(并发 writer) × P(在 window 内)`。

- **fix 前**:race window = T2..T_finalize 约 30µs(整个 select_neighbors)
- **fix 后**:window 缩到 T1..T4 约 2µs(只剩 FAA → EXCL acquire 的间隙) + T2..T3 vec write

window 缩短但**没消除**。pw=2 这点 window 小到刚好不撞;pw=4 collision pressure 4×,撞概率重新升高,且 fix 把 reader EXCL hold 时间拉长(reader 现在拿 EXCL 等 publisher),lock-contention 反而扩大窗口期间的 reader 暴露。

## 真根因(待修)

**未 cover 的 race path**:`algorithm.h:1028 search_layer` 主循环里的 `get_distance_batch(distancer, query, nbr_id, dists)` — batch 读 `vector_pool[nbr_id[i]]` **无任何 lock**。reader 通过 graph edge 拿到 nbr id N(set_neighbor 已写),立即 read vec_pool[N]。如果 N 的 vec 还在 T2..T5 publish window 内 → 读 garbage。

我加的 `get_neighbors_data` SHARED 只 cover `update_reverse_edges` 内 select_neighbors 的 distance 计算,不 cover **search_layer hot path 的 batch distance**。

## Fix 收敛方向

### 方案 A — `ready` atomic bitmap(agent 推荐,minimal cost)

`storage.h` 加 `std::atomic<uint64_t> ready_bits[]` per id chunk。

writer:
```
id = reserve_id();           // 不暴露 max_id
add_vector(id, query);
add_basepoint(id, ...);
ready_bits[id].store(true, release);  // ← release fence,publish 边界
max_id_advance(id);          // 现在 reader 才看见
```

reader:
```
if (!ready_bits[id].load(acquire)) continue;  // skip 未 publish
distance(query, vec_pool[id]);                // safe
```

`acquire-load on ready` 提供 happens-before,reader 读 vec 看到 writer 所有 prior writes。**reader 路径无需 lock 等待**,hot path 性能保持。

### 方案 B — reorder publish 进 `update_reverse_edges`

让 `assign_vector_id` 不立刻 expose;只在 `update_reverse_edges` 完成后 expose。但 lite 用 atomic counter,改不动。

### 方案 C — search_layer 也加 per-nbr SHARED lock

把 `get_distance_batch` 之前对每个 nbr_id[i] 加 `lock_point<is_base, true>(id)` SHARED + read + unlock。简单但 hot path 性能掉。

## 决策

短期 stopgap 走方案 C(已部分实施),验证算法层面 race 真在这一点;**真正 fix 走方案 A** —— 跟主库的 happens-before invariant 一致,且对 hot path 零开销。

## 关联

- search_layer base 漏邻居 fix:`e55309bbe0`(独立 bug,跟 parallel 无关)
- memory:[[project-search-layer-recall-fix]]
- 主库 parallel build 参考:`openGauss-vector-main/src/gausskernel/storage/access/hnsw/hnswbuild.cpp:852-880`
