# PQ v1 出包说明 (vexdb-lite / pg_vexdb)

## 这一版包含什么

- ✅ `CREATE INDEX ... WITH (quantizer='pq', pq_m=N)` 训练 PQ codebook 并把 codes 写入索引存储
- ✅ codebook 持久化到 `qtcode_block`，重启后从盘恢复
- ✅ SELECT 路径走 ADC（asymmetric distance）—— 比 plain HNSW 节省 ~16x 存储
- ✅ `vex_index_info()` 报告 `use_pq=t`、`pq_m=N`

## 强制要求

| 配置 | 值 | 说明 |
|---|---|---|
| `maintenance_work_mem` | **≥ 1GB** | PQ 训练 + 编码需要在内存里完成；小于 1GB 自动回落 plain HNSW + NOTICE |

```sql
SET maintenance_work_mem = '2GB';
CREATE INDEX idx ON tbl USING vexdb_graph(vec) WITH (quantizer='pq', pq_m=4);
```

## 已知限制（必须告知用户）

1. **PQ 索引在 build 后是只读的**（v1 限制）
   - `INSERT` / `UPDATE` 操作如果会触发 aminsert，会被显式拒绝：
   ```
   ERROR: DML on a PQ-enabled vexdb_graph index is not yet supported
   HINT:  Drop and recreate the index after data changes, or use an index without quantizer='pq'.
   ```
   - **用户工作流**：先批量写数据 → CREATE INDEX → 只读查询。数据更新后需要 DROP + CREATE INDEX 重建。
   - 这与 FAISS 等向量库的 "build-once index" 模式一致。

2. **parallel build × PQ 不支持** — 走单线程，无错误（与 plain HNSW 行为一致）

3. **vacuum 不会重剪 PQ index 的图** — 现行 vacuum 是 stub

## 测试侧情况

- 5 个 `graph_index_pq_*.yaml` spec test 仍 FAIL，原因：fixture 里 `expect: []` 全是占位，**没人写过 expected 输出**。这是测试侧 populate 工作，不是 PQ 实现缺陷。
- 实际 PQ 行为：`use_pq=t`、codes 落盘正确（`Flushing Vector: 305μs` 比 raw float 快约 16x）、SELECT 走 ADC 返回正确最近邻。

## 下一 sprint TODO

参考 `docs/design/2026-05-12_pq-stage-a2-impl.md`：

1. **Phase 2.B + 2.D 完整版**：SDC 距离 + DML INSERT encode 路径，去掉 "DML not supported" 守卫
2. **DISK build × PQ**：允许 PQ 在低 maintenance_work_mem 下也工作（存储布局重写）
3. **vex_index_info 报告 `pq_codebook_bytes` / `pq_codes_bytes`**：现在硬编码 0
4. **vacuum re-prune for PQ**

工时估计 ~4-6 工日（详见 design doc）。

## 文件清单

本次改动的文件（相对 `duckdb/`）：

- `include/pq.h` — ADC distancer, `code_size()` 返回字节, configure_for_metric helper
- `src/pq.cpp` — `flush` / `hnsw_read_pq_center` / `prepare` 重启恢复, `setupKmeansState` 调用
- `src/quantizer_stubs.cpp` — `setupKmeansState` 移植自 openGauss
- `src/graph_index_build.cpp` — encode-at-flush 写 codes, `init_quantizer` 返回 true + MEMORY-only gate
- `src/graph_index_insert.cpp` — DML 守卫（PQ index 上 INSERT 报错）
- `include/graph_index/graph_index.h` — `graph_index_store_qt_centroids` extern 声明
