# PQ Stage-A.2 实施进度

## 已实施（pg_vexdb / vexdb-lite）

### Phase 1 — codebook + ADC distancer 基础
- `PQDistancer::flush` / `hnsw_read_pq_center` / `prepare` — codebook 训练后落 qtcode_block，重启后从盘重读
- `PQDistancer::get_distance_single/batch2` — 走 `pq.distance_to_code` (ADC)
- `setupKmeansState` 从 openGauss 移植
- `code_size()` 返回 byte 数（原来错返 M）
- 删除 dead `pq_distancer.h` stub

### Phase 2.A — encode-at-flush
- `flush_graph`（src/graph_index_build.cpp:1029-1075）：检测 `qt_type==PQ`，加载 codebook，把 mem_store 中每个 raw float vector 经 `compute_code` 转 codes，按 `code_size * id` offset 写入 vec storage（替代 raw float）。
- `init_quantizer` 返回 `true`（不再 fallback 到 plain），但加保护 `if (build_state != MEMORY) return false`：因为 disk-build 路径与 PQ 不兼容（会写 raw float 字节进 code_size 槽位）。

### Phase 2.C — set_enable() + init_quantizer 返回 true
- flush_graph 在 metabuf 锁内调 `metap->quantizer_metainfo.set_enable()` 翻 `graph_pq=true`。后续任何打开此索引的 DiskStore 在 ctor 看到 `get_type()==PQ` → `elem_size = code_size`、`st = PureCode`。

### Phase 2.B — code-vs-code 距离（SDC via 地址分发）
- `PQDistancer::process` 记录 `_processed_query = query` 指针。
- `get_distance_single(x, y)` 地址分发：
  - `x == _processed_query` → ADC (`pq.distance_to_code(y, dist_table) * flag`)
  - 否则 → SDC：`sdc_distance(x_code, y_code)` 把两个 code 都 decode 到 centroid concat 再算 L2。
- 算法无需改：select_neighbors 调 `get_distance(stored_a, stored_b)` 自动走 SDC，搜索阶段调 `get_distance(query, stored)` 走 ADC。

### Phase 2.D — DML INSERT encode
- `graph_index_insert.cpp` 在 `distancer.process(query)` 之后用 `if constexpr (is_same_v<D, PQDistancer>)` 检测，PQ 路径上 `compute_code(query, code_buf)` 然后 `ictx.query = code_buf`。
- 这样 add_vector 写 code、memcmp dedup 比 code、prune 走 SDC、insert 期间 search_layer 也走 SDC（query=code != _processed_query=raw）—— 后者是 ADC 转 SDC 的微小性能损失，可接受。

## 已知限制

1. **PQ 必须 MEMORY build**：`maintenance_work_mem ≥ 1GB`。否则自动回落 plain HNSW + NOTICE。
2. **Insert 期间 search 用 SDC 而不是 ADC**：精度等价，吞吐略低。
3. **parallel build × PQ**：现已 gate 在通用的 parallel_workers=0 fallback 里，PQ 走单线程。
4. **vacuum re-prune**：vacuum 现是 stub，未来实现时需 SDC 路径（与现 PQDistancer 一致）。

## 仍 TODO

- vex_index_info 的 `pq_codebook_bytes` / `pq_codes_bytes` 仍硬编码 0（src/vex_index_info.cpp:158-159）—— reporting only，不影响功能
- spec test `expect: []` 占位 —— 测试侧 populate 工作
- DISK build + PQ：需要重写存储布局或 insert-during-build 编码
