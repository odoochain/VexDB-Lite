# VexDB-Lite 新测试文件第二轮代码审查

**日期**: 2026-03-19
**审查范围**: 5 个新增测试文件（已删除 config_advanced.test）

---

## 一、逐文件审查

### 1. graph_index_small_dataset.test

**[Warning] Test 5 (dim=2) 与 lowdim.test Test 1 (dim=2) 高度重复**

- `small_dataset.test` Test 5: 4 行 dim=2 向量，建索引，搜索 [0,0] 和 [1,1]
- `lowdim.test` Test 1: 6 行 dim=2 向量（含上面 4 个 + 2 个额外），建索引，搜索 [0,0]、[1,1]、[0.5,0.5]

两者共享完全相同的测试逻辑和数据子集。建议**删除 small_dataset.test 的 Test 5**，因为 lowdim.test Test 1 是其严格超集。

**[Warning] Test 6-7 (brute force threshold 64/65) 与 graph_index_brute_force_threshold.test 可能重复**

已有独立文件 `graph_index_brute_force_threshold.test` 专门测试此边界。需要确认两者是否测试了不同方面。如果已有文件已覆盖 64/65 行边界，建议删除 small_dataset.test 的 Test 6-7。

**[Suggestion] Test 3 (5 rows) 只验证了 count，未验证排序正确性**

```sql
SELECT count(*) FROM (SELECT id FROM t5 ORDER BY l2_distance(...) LIMIT 5);
-- 只验证返回 5 行，未验证具体排序
```

5 行全部返回，顺序正确性无法验证。建议改为验证至少 top-1 是正确的。

**[Suggestion] Test 8 (k > table size) 与 edge_cases.test Test 8 完全重复**

`edge_cases.test` Test 8 已经测试了 k=100 和 k=1000 超过行数的场景。`small_dataset.test` Test 8 测试 k=10 和 k=100，是重复覆盖。

**正确性**: 所有预期值数学正确。数据沿线性分布，ANN 搜索结果确定性高。

---

### 2. graph_index_lowdim.test

**[Critical] Test 4 (IP metric) — `ORDER BY inner_product(...) DESC LIMIT 1` 可能不触发索引优化**

从 optimizer 代码看，`inner_product` 函数在 `RequiresDescending` 中返回 true，optimizer 的 `TryResolveDistanceOrder` 会检查 `order_type == OrderType::DESCENDING`。查询使用了 `ORDER BY ... DESC`，所以优化器**可以**匹配。

但问题是：这个测试只验证了 "不报错 + top-1 正确"，而 4 行数据量极小（远低于 brute_force_threshold=64），实际走的是暴力搜索路径。**索引几乎没有被真正使用**。

建议：要么增加到 >64 行以测试真正的图搜索路径，要么在注释中明确说明此测试只验证 IP 索引创建不报错。

**[Warning] Test 7 (dim=32, PQ, 500 rows) — PQ 搜索结果可能 flaky**

PQ 是有损压缩。对于 `pq_m=8`（每个子空间 32/8=4 维），搜索 row 250 的精确向量能否保证 top-1 就是 row 250？在 500 行线性分布的数据上，相邻行的距离非常接近，PQ 量化误差可能导致 top-1 不是精确匹配。

验证：row 250 向量为各维度 (x+250)/200，row 249 和 251 的差异每维仅 1/200=0.005。PQ 量化误差量级可能超过这个差异。

**建议**：改用更宽松的验证方式，如 `SELECT id BETWEEN 248 AND 252` 或使用 recall 方式验证。

**[Suggestion] Test 2 (dim=4, L2, 100 rows) 与 minimal_params.test Test 1 数据完全相同**

两者都生成 `range(100)` + `[(i*0.1), (i*0.2), (i*0.3), (i*0.4)]`，只是索引参数不同（默认 vs m=2）。数据重复不影响正确性，但增加了测试执行时间。

---

### 3. graph_index_minimal_params.test

**[Warning] Test 4 (ef_search=1) — SET 后未验证实际搜索行为差异**

测试设置 `vex_ef_search=1` 后只验证返回了结果，未验证结果的正确性或与默认 ef_search 的行为差异。ef_search=1 理论上 recall 会很差，但线性分布数据可能仍然正确。

此外，`ef_search=1` 且 `LIMIT 5` 的组合行为不明确——ef_search < k 时，HNSW 是否能返回足够多的结果？代码里会如何处理？这个测试实际上是在验证一个重要的边界条件，但只用 count 验证太弱了。

**建议**：增加对 top-1 结果正确性的验证。

**[Suggestion] Test 5 (m=2 + IP metric) — 只验证了索引创建成功，无搜索验证**

```sql
SELECT count(*) FROM duckdb_indexes() WHERE index_name = 'idx_m2_ip';
```

只检查索引存在，没有执行任何搜索查询。这是一个典型的 "只验证不报错" 的测试。

**建议**：至少添加一个 `ORDER BY inner_product(...) DESC LIMIT 1` 搜索验证。

**[Suggestion] Test 2 (m=4, 500 rows) — 搜索 [25,50,75,100] 期望返回 id=250**

验证：row 250 的向量为 `[25.0, 50.0, 75.0, 100.0]`，精确匹配查询向量。数据沿线性分布且间隔大，ANN 搜索确定性高。结果正确。

---

### 4. functions/index_info.test

**[Critical] Test 3 — HybridIndex 预期 4 行结果需要验证**

```sql
SELECT count(*) FROM vex_index_info();
----
4
```

此时有 idx1 (GraphIndex, 1行)、idx2 (GraphIndex, 1行)、idx3 (HybridIndex with A,B 两个分区, 2行)。总计 1+1+2=4 行。数值正确。

但依赖于 `vex_index_info()` 对 HybridIndex 的具体行为——每个分区返回一行。如果实现变更，此测试会失败。**建议添加注释说明预期值的计算逻辑**。

**[Warning] 缺少对 metric 列的验证**

`vex_index_info()` 应该返回 metric 信息（L2/cosine/ip），但测试没有验证此列。如果该函数支持 metric 列，建议添加验证。

**[Suggestion] 缺少 vex_index_info() 的 schema 验证**

建议添加一个测试验证 `SELECT * FROM vex_index_info() LIMIT 0` 的列名和类型，确保 API 稳定性。

---

### 5. functions/distance_edge_cases.test

**[Critical] Test 7 — cosine_distance 零向量预期值 1.0 需要注意**

代码实现中，当 `denom < 1e-30f` 时返回 `1.0f`。但数学上，cosine_distance(zero, nonzero) 是未定义的。测试预期 1.0 是基于当前实现，**不是数学定义**。

- `cosine_distance([0,0,0], [1,0,0]) = 1.0` — 实现返回 1.0（兜底值）
- `cosine_distance([0,0], [0,0]) = 1.0` — 实现返回 1.0（两个零向量）

这个行为合理但需要在测试注释中明确说明是 "实现定义行为"，而非数学正确性验证。当前注释 "should return max distance" 暗示 max cosine distance 是 2.0（反方向向量），而非 1.0，**注释有误导性**。

**[Warning] Test 5 — l2_distance([-1,-2,-3], [1,2,3]) = 7.483314773547883 精度问题**

l2_distance 返回 DOUBLE 类型（代码中 `std::sqrt(static_cast<double>(l2sqr))`），但内部计算 l2sqr 是 float 精度。sqrt(56.0f) 的 float 精度为 7.4833149，转为 double 后精度可能不是 7.483314773547883。

DuckDB test framework 对 `query R` 的容差约为 1e-6 到 1e-3（取决于版本）。实际值：
- `sqrt(56.0)` (double) = 7.483314773547883
- `sqrt(56.0f)` (float→double) = 7.483314990997314（约）

预期值 `7.483314773547883` 是 double 精度的 sqrt(56)。由于实现是 `std::sqrt(static_cast<double>(l2sqr))`——l2sqr 是 float，cast 到 double 后取 sqrt，实际结果取决于 l2sqr 的 float 精度。如果 56.0 能精确表示为 float（可以，56 = 7*8），则 `static_cast<double>(56.0f) = 56.0`，sqrt 结果与 double 精度一致。**此处数值正确**。

但如果被测向量的分量不能精确表示为 float（如 3.14, 2.71），平方差的累加可能有 float 精度损失。Test 2 中 `l2_distance([3.14, 2.71, 1.41], [3.14, 2.71, 1.41])` 预期 0.0 是安全的（完全相同的 float 值相减）。

**[Suggestion] 缺少维度不匹配的负面测试**

`distance_edge_cases.test` 专注于数值边界，但没有测试：
- `l2_distance([1,0]::FLOATVECTOR(2), [1,0,0]::FLOATVECTOR(3))` 应报错

这已在 `graph_index_type_safety.test` 中覆盖，但既然本文件命名为 "edge_cases"，建议至少引用已有覆盖的位置。

---

## 二、与已有测试的重复度总结

| 新测试场景 | 已有覆盖 | 重复程度 |
|---|---|---|
| small_dataset Test 1 (单行) | edge_cases.test Test 2 | **高度重复** |
| small_dataset Test 5 (dim=2) | lowdim.test Test 1 | **高度重复** |
| small_dataset Test 8 (k>rows) | edge_cases.test Test 8, boundary.test Test 8 | **完全重复** |
| small_dataset Test 6-7 (BF边界) | graph_index_brute_force_threshold.test | **需确认** |
| lowdim Test 2 (dim=4, 100行) | minimal_params Test 1 (相同数据) | **数据重复** |
| minimal_params Test 5 (m=2 IP) | review_fixes.test Test 5-7 (m=2各metric) | 部分重复 |

**建议**: `small_dataset.test` 的独特价值主要在 Test 2 (两行)、Test 3-4 (少量行验证)、Test 6-7 (BF边界)。其中 Test 1、5、8 建议删除以减少重复。

---

## 三、第一轮 Review 问题修复状态

| 第一轮问题 | 状态 | 备注 |
|---|---|---|
| 维度不匹配错误测试 | **已有覆盖** | graph_index_type_safety.test 覆盖 |
| NULL 输入测试 | **已有覆盖** | graph_index_null_vectors.test + distance_l2.test |
| 增量插入（先建索引再插入） | **已有覆盖** | edge_cases.test Test 1, null_vectors.test |
| PQ + cosine/IP 组合 | **已有覆盖** | review_fixes.test Test 1-2 |
| HybridIndex 跨分区搜索 | **已有覆盖** | review_fixes.test Test 3-4 |
| 小 M 值导致的层级溢出 | **已有覆盖** | review_fixes.test Test 5 |

所有第一轮发现的关键问题已在 `graph_index_review_fixes.test` 和其他已有测试中解决。

---

## 四、覆盖缺口分析

现有 ~80 个测试文件 + 5 个新增后，仍有以下场景未覆盖或覆盖较弱：

### [Warning] 高优先级缺口

1. **并发写入 + 搜索的正确性验证** — `graph_index_concurrent_rw.test` 存在但不确定是否验证了搜索结果正确性（而非仅"不崩溃"）

2. **大批量 DELETE 后重建图的连通性** — `graph_index_delete.test` 测试了删除，但没有验证删除超过 50% 节点后图是否仍保持连通、recall 是否可接受

3. **UPDATE 向量值后的索引一致性** — `graph_index_update.test` 存在，但新测试中未涉及 "UPDATE 后搜索能找到新值" 的验证

### [Suggestion] 低优先级缺口

4. **dim=1 索引搜索** — distance_edge_cases.test 测试了 dim=1 的距离函数，但没有测试 dim=1 的索引创建和搜索

5. **非法参数值的负面测试** — 如 `m=0`、`m=-1`、`ef_construction=0`、`metric='invalid'` 等是否报错

6. **FLOATVECTOR(0) 或超大维度** — 如 FLOATVECTOR(0) 是否报错、FLOATVECTOR(10000) 是否能正常工作

7. **事务回滚后的索引状态** — 已有 transaction 测试但不确定是否覆盖了 ROLLBACK 后索引回退

---

## 五、总结

### 必须修复 (Critical)

1. **lowdim.test Test 7 (PQ dim=32)** — PQ 量化误差可能导致 top-1 不稳定，改用 recall 验证或放宽断言
2. **distance_edge_cases.test Test 7** — 注释 "should return max distance" 具有误导性，cosine_distance max=2.0 而非 1.0，应改为 "implementation-defined fallback for zero vector"

### 建议修复 (Warning)

3. 删除 `small_dataset.test` 的 Test 1、5、8（与已有测试重复）
4. `minimal_params.test` Test 5 应添加搜索查询验证
5. `lowdim.test` Test 4 应注明小数据量实际走暴力搜索
6. `index_info.test` 添加对 metric 列和 partition_key 值的验证

### 锦上添花 (Suggestion)

7. 添加 dim=1 的索引搜索测试
8. 添加非法参数值的 `statement error` 测试
9. `index_info.test` 添加 schema 稳定性验证
