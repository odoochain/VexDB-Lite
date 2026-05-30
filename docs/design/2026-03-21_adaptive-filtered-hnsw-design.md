# Adaptive Filtered HNSW 设计方案

**日期:** 2026-03-21
**分支:** memme-db
**状态:** 设计中

---

## 1. 背景

### 当前方案（分区 HNSW / HybridIndex）

按标量列值分区，每个分区独立 HNSW 图：

```
HybridIndex
├── partition["electronics"] → GraphIndexCore (独立 HNSW)
├── partition["clothing"]    → GraphIndexCore (独立 HNSW)
└── partition["food"]        → GraphIndexCore (独立 HNSW)
```

### 当前方案局限

| 问题 | 说明 |
|------|------|
| 只支持单列等值过滤 | 不支持 `price BETWEEN 100 AND 500`、`date >= 2024-01` |
| 高基数爆炸 | 100 万个 user_id → 100 万个独立 HNSW 图 |
| 跨分区搜索次优 | GlobalSearch 独立搜每个分区再合并 |
| 多列过滤不支持 | 不能同时按 category + brand 过滤 |
| 无选择性自适应 | 不区分宽/窄过滤场景 |

---

## 2. 业内方案调研总结

详见 `docs/research/2026-03-21_hybrid-search-filtered-ann-industry-approaches.md`

关键结论：
- **ACORN**（Weaviate/Elasticsearch）：两跳展开，谓词无关，2-1000x 提升
- **Qdrant**：选择性感知查询规划器，自适应切换策略
- **VBASE**（Microsoft）：迭代器接口 + 松弛单调性，10-1000x 提升
- **Pinecone**：bitmap 元数据索引与向量索引融合，单阶段过滤
- **FCVI**：几何变换将过滤信息融入向量空间

**共识：in-algorithm filtering（算法内过滤）是未来方向。**

---

## 3. 新方案：Adaptive Filtered HNSW

### 3.1 整体架构

```
┌─────────────────────────────────────────────┐
│              Query Planner                   │
│  (估算 filter selectivity → 选择执行策略)     │
└──────────┬──────────┬──────────┬────────────┘
           │          │          │
     selectivity     10%-90%    selectivity
       < 10%           │         > 90%
           │          │          │
    ┌──────▼───┐  ┌───▼────┐  ┌─▼──────────┐
    │ Pre-filter│  │In-graph│  │ Standard   │
    │ + Brute  │  │Filtered│  │ HNSW +     │
    │ Force    │  │Traversal│  │ Post-filter│
    └──────────┘  └────────┘  └────────────┘
```

### 3.2 核心变化

#### 3.2.1 单图 + 过滤遍历（取代分区方案）

不再为每个标量值建独立图，而是一个统一 HNSW 图。搜索时在遍历过程中跳过不匹配的节点。

```cpp
// ACORN 风格过滤搜索
SearchResult FilteredSearch(query_vec, filter_predicate, k, ef) {
    priority_queue candidates;
    candidates.push(entry_point);

    while (!candidates.empty() && results.size() < k) {
        node = candidates.pop();

        if (filter_predicate(node)) {
            results.add(node);
        }

        // 关键：即使节点不匹配过滤条件，也展开邻居
        // 维持图遍历的连通性
        for (neighbor : node.neighbors) {
            if (!visited[neighbor]) {
                visited[neighbor] = true;
                candidates.push(neighbor);
            }
        }
    }
}
```

#### 3.2.2 选择性感知查询规划器

```cpp
float selectivity = EstimateSelectivity(filter, table_stats);

if (selectivity < 0.01) {
    // 极窄过滤：先用辅助索引找匹配行，再暴力搜索
    return BruteForceOnSubset(filtered_row_ids, query_vec, k);
} else if (selectivity > 0.90) {
    // 极宽过滤：标准 HNSW + 后过滤
    return PostFilteredHNSW(query_vec, filter, k, oversample=1.5);
} else {
    // 中间区域：图内过滤遍历
    return FilteredGraphTraversal(query_vec, filter, k, ef);
}
```

#### 3.2.3 迭代器接口（Volcano 模型集成）

```cpp
// 暴露 Next() 接口，让 DuckDB 自然 pipeline 过滤
class HnswIndexIterator {
    optional<RowId> Next() {
        // 返回下一个最近邻候选
        // DuckDB 的 Filter 算子在外层做谓词过滤
    }
    void Reset();
    void Close();
};
```

#### 3.2.4 可选 Payload Index

```cpp
// 为常用过滤列建立轻量级辅助索引
PayloadIndex {
    bitmap_index   → 低基数列 (category, status)
    interval_tree  → 数值范围列 (price, date)
}
```

### 3.3 与当前方案的关键差异

| 维度 | 当前（分区 HNSW） | 新方案（自适应过滤 HNSW） |
|------|-----------------|----------------------|
| 图结构 | N 个独立图 | 1 个统一图 |
| 过滤类型 | 仅单列等值 | 等值/范围/多列/任意 SQL |
| 执行策略 | 固定路由 | 选择性自适应（3 种策略） |
| DuckDB 集成 | 批量 TopK | 迭代器接口（Volcano） |
| 内存 | 高基数时爆炸 | 恒定（一个图） |
| 构建时间 | 按分区叠加 | 一次构建 |
| 索引类型名 | GRAPH_INDEX / HYBRID_INDEX | HNSW |

---

## 4. 实施计划

### Phase 1：概念重命名 + 过滤搜索基础

- [ ] GRAPH_INDEX → HNSW 全局重命名
- [ ] GraphIndex → HnswIndex 类名重命名
- [ ] 在 HnswIndex 的 Search 方法中增加 filter_predicate 参数
- [ ] 实现基础的图内过滤遍历（跳过不匹配节点，但继续展开邻居）
- [ ] 添加 SQL 测试用例

### Phase 2：选择性自适应

- [ ] 实现 EstimateSelectivity（基于 DuckDB 统计信息）
- [ ] 实现三策略切换（pre-filter / in-graph / post-filter）
- [ ] 优化器适配：识别 WHERE 子句中的过滤条件并下推
- [ ] 性能基准测试

### Phase 3：迭代器接口

- [ ] 实现 HnswIndexIterator（Open/Next/Close）
- [ ] 集成到 DuckDB 的 Volcano 执行引擎
- [ ] 支持任意 SQL WHERE 条件的自然过滤

### Phase 4：Payload Index

- [ ] Bitmap index 用于低基数列
- [ ] Interval tree 用于范围过滤
- [ ] 查询规划器集成

---

## 5. 性能目标

| 指标 | 当前 | 目标 |
|------|------|------|
| 等值过滤 (10% selectivity) | 基线 | 持平或更快 |
| 窄过滤 (< 1%) | 需全分区扫描 | 2-5x 提升 |
| 范围过滤 | 不支持 | 支持，延迟 < 50ms (100K 数据) |
| 多列过滤 | 不支持 | 支持 |
| 内存（高基数） | O(N_partitions) | O(1) |
