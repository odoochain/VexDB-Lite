# HNSW 搜索性能优化方案

> 类型: plan
> 创建日期: 2026-04-08
> 更新日期: 2026-04-08
> 状态: draft

## 概述

VexDB-Lite 在 SIFT-1M 基准上当前 637 QPS / 1.57ms 延迟 / recall@10 0.9951。目标通过多项优化叠加尽可能提升 QPS。基于对 SearchLayer 核心搜索函数的逐行性能分析，制定以下优化方案。

## 当前性能基线

| 指标 | 值 | 数据集 |
|------|-----|--------|
| QPS | 637 | SIFT-1M (1M x 128d) |
| 平均延迟 | 1.57ms | ef_search=40, k=10 |
| Recall@10 | 0.9951 | — |
| 构建时间 | 47.4s | 单线程 |

## 性能瓶颈分解（估算占比）

| 瓶颈 | 占比 | 原因 |
|------|------|------|
| 距离计算 + 向量加载 | 40-50% | cache miss 严重（128d = 512B/向量，随机访问） |
| visited hash set | 15-25% | `unordered_set<row_t>` 查找/插入 cache 不友好 |
| 优先队列操作 | 10-15% | 两个 `priority_queue`，每次搜索 640-1280 次 push/pop |
| GetNode/GetVector 开销 | 5-10% | SegmentHandle 构造（mutex + buffer 查找） |
| 其他（排序、比较） | 5% | — |

## 优化方案（按优先级排序）

### P0: visited 集合优化 — 预期 +25%

**现状**: `unordered_set<row_t>` 使用 row_id 做 key，hash 表 cache 行为差。

**方案**: 用 flat bitset 替代，以内部 node index 为 key：
- 预分配 `vector<bool>` 或 `bitset` 大小 = node_count
- 需要从 IndexPointer 到连续整数 index 的映射（或直接用 row_id 如果范围可控）
- O(1) 查找/插入，cache line 友好

**替代方案**: `dense_hash_set`（Google sparse_hash）或 Robin Hood hash。

### P1: 软件预取（Prefetch）— 预期 +10-15%

**现状**: SearchLayer 循环中访问邻居节点时，先 GetNode 读 header，再 GetVector 读向量数据，两次 cache miss。

**方案**: 在处理当前邻居时，预取下一个邻居的 header 和 vector 数据：
```cpp
for (uint16_t i = 0; i < neighbor_count; i++) {
    // Prefetch next neighbor
    if (i + 1 < neighbor_count) {
        auto *next_h = GetNode(neighbors[i+1]);
        __builtin_prefetch(GetVector(next_h->vector_ptr), 0, 0);
    }
    // Process current neighbor
    ...
}
```

### P2: 候选队列优化 — 预期 +10-15%

**现状**: 两个 `std::priority_queue`（min-heap + max-heap），每次搜索 ~1000 次堆操作。

**方案 A**: 用单个 sorted flat array 替代（对小 ef 更快）：
- 插入用二分查找 + memmove
- 当 ef <= 64 时比堆更快（cache 连续）

**方案 B**: 自定义 tournament tree 或 d-ary heap（d=4/8，更少 cache miss）。

### P3: 减少重复 GetNode 调用 — 预期 +5-10%

**现状**: SearchLayer 中对同一节点多次调用 GetNode（检查 deleted、获取 level、获取 neighbors）。

**方案**: 一次获取 header 指针，在整个迭代中复用：
```cpp
auto *cur_header = GetNode(current.node_ptr);
// 直接从 cur_header 读取 level、neighbors，不再重复 GetNode
```

### P4: 图结构重排序（Graph Reordering）— 预期 +20-30%

**现状**: 节点在 FixedSizeAllocator 中按插入顺序存储，邻居可能分散在不同 buffer 中。

**方案**: 构建完成后按 BFS 顺序重排节点，使相邻节点在内存中相邻：
- 显著减少 cache miss
- 工程量大，需要重新分配所有节点并更新所有 IndexPointer
- 适合作为 `PRAGMA vex_optimize('index_name')` 的后处理步骤

### P5: SQ8 标量量化 — 预期 +30-50%

**现状**: 向量以 float32 存储（128d = 512B），距离计算在 float32 上。

**方案**: 添加 SQ8（Scalar Quantization 8-bit）选项：
- 向量压缩为 uint8（128d = 128B，4x 压缩）
- 距离计算用 uint8 SIMD（NEON vdotq_u32 等），吞吐量 4x
- Recall 损失 <2%（优于 PQ）
- 可作为搜索路径的默认量化（结合 rerank）

### P6: 批量搜索（Batch Query）— 预期 +2-3x 吞吐

**现状**: 每次查询独立执行 SearchLayer。

**方案**: 多个查询共享图遍历开销：
- 将多个查询向量打包，在 SearchLayer 内部同时计算多个距离
- 利用 SIMD 处理多个查询的距离计算
- 适合批量推荐/检索场景

## 预期收益叠加

| 优化组合 | 预期 QPS | 倍率 |
|----------|----------|------|
| 基线 | 637 | 1x |
| + P0 (visited bitset) | ~800 | 1.25x |
| + P1 (prefetch) | ~920 | 1.45x |
| + P2 (queue opt) | ~1050 | 1.65x |
| + P3 (减少重复访问) | ~1150 | 1.8x |
| + P5 (SQ8) | ~1700 | 2.7x |
| + P4 (graph reorder) | ~2200 | 3.5x |
| + P6 (batch query) | ~5000+ | 8x+ |

> 注：以上为乐观估算，实际收益需 benchmark 验证。优化之间可能有交互效应。

## 实施顺序

1. **第一轮（低风险快收益）**: P0 + P1 + P3 — 预期 1.5-1.8x
2. **第二轮（中等工程量）**: P2 + P5(SQ8) — 预期 2.5-3x
3. **第三轮（大工程）**: P4 + P6 — 预期 5-8x

## 验证方法

使用现有 SIFT-1M benchmark：
```bash
# 需要 ann-benchmark 数据集
python3 packaging/benchmark.py --dataset sift-1m --ef 40 --k 10
```

关注指标：QPS、Recall@10、P99 延迟。每项优化前后对比。
