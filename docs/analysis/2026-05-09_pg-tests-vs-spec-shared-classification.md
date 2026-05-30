# pg_tests/ ↔ tests/spec/ 分类与迁移建议

**日期**: 2026-05-09
**目的**: 把根目录下手写的 `pg_tests/`（59 文件）跟 spec DSL 对齐 —— 该删的删、该并入 `shared/` 的并入、真正 PG 专属的迁到 `tests/spec/pg/`。

## 数据基线

| 来源 | 数量 |
|---|---|
| `tests/spec/shared/` yaml | 25 |
| `tests/spec/duckdb/` yaml（DuckDB 专属） | 80 |
| `tests/spec/pg/` yaml | 0 |
| `pg_tests/` 手写 .sql | 58 个独立用例 |

- `pg_tests/` 中 8 个文件标了 `KNOWN GAP / duck-only`，5 个标了 `pg-test-status: skipped`。
- `pg_tests/opengauss_adapted/ivfpq_basic_hnsw_adapted.sql` 是唯一一个**两边 yaml 都没有**的纯新增用例。

## 三类处置

### Group A — 删除：shared/ 已经覆盖（共 16 个）

`shared/` 已有同名 yaml，渲染产物会落到 `build/spec/pg/`。`pg_tests/` 里的手写副本是历史遗留的重复劳动，确认渲染产物对齐后直接删除。

| pg_tests 路径 | 对应 shared yaml |
|---|---|
| `functions/distance_cosine.sql` | `shared/functions/distance_cosine.yaml` |
| `functions/distance_ip.sql` | `shared/functions/distance_ip.yaml` |
| `functions/distance_l2.sql` | `shared/functions/distance_l2.yaml` |
| `index_basic/float_array_basic.sql` | `shared/types/float_array_basic.yaml` |
| `index_basic/float_array_dims.sql` | `shared/types/float_array_dims.yaml` |
| `index_basic/graph_index_ann.sql` | `shared/index/graph_index_ann.yaml` |
| `index_basic/graph_index_ddl.sql` | `shared/index/graph_index_ddl.yaml` |
| `index_basic/graph_index_delete.sql` | `shared/index/graph_index_delete.yaml` |
| `index_basic/graph_index_delete_freenode.sql` | `shared/index/graph_index_delete_freenode.yaml` |
| `index_basic/graph_index_distance.sql` | `shared/index/graph_index_distance.yaml` |
| `index_basic/graph_index_null_vectors.sql` | `shared/index/graph_index_null_vectors.yaml` |
| `index_basic/graph_index_query_patterns.sql` | `shared/index/graph_index_query_patterns.yaml` |
| `index_basic/graph_index_search.sql` | `shared/index/graph_index_search.yaml` |
| `index_basic/graph_index_simple.sql` | `shared/index/graph_index_simple.yaml` |
| `index_basic/graph_index_type_safety.sql` | `shared/index/graph_index_type_safety.yaml` |
| `index_basic/graph_index_verify.sql` | `shared/index/graph_index_verify.yaml` |

**操作**：用 `diff build/spec/pg/sql/<name>.sql pg_tests/.../<name>.sql` 逐个核对；若手写版本断言更严，把缺失的 step 补回 shared yaml；然后删除 .sql。

### Group B — 提升：从 duckdb/ 上提到 shared/（共 28 个）

这些用例 `duckdb/` 有 yaml、`shared/` 没有，但 `pg_tests/` 里能跑通 → 行为本就跨引擎，原作者只是把它们误放进 `duckdb/`。把 yaml 从 `duckdb/<sub>/` 搬到 `shared/<sub>/`，加上 `engines: [duckdb, pg, opengauss]`，再删掉 `pg_tests/` 对应 .sql。

**类型/函数（4）**
- `distance_defensive` · `distance_edge_cases` · `distance_null` · `vector_ops`

**基础索引行为（17）**
- `graph_index_basic` · `graph_index_boundary` · `graph_index_cosine` · `graph_index_cosine_recall`
- `graph_index_ddl_dup_cols` · `graph_index_highdim` · `graph_index_invariant` · `graph_index_lowdim`
- `graph_index_metric` · `graph_index_metric_cosine` · `graph_index_metric_ip` · `graph_index_minimal_params`
- `graph_index_param_validation` · `graph_index_params` · `graph_index_recall` · `graph_index_recall_after_delete` · `graph_index_recall_vs_brute`
- `graph_index_small_dataset` · `graph_index_update`

**PQ（6）** —— PG 端 PQ 大多 `pg-test-status: skipped`，先把 yaml 提上来，PG 在 yaml 里 `skip: pg: "..."`，等 PG PQ 落地再放开。
- `graph_index_pq` · `graph_index_pq_compact` · `graph_index_pq_delete` · `graph_index_pq_search`
- `graph_index_pq_simd_correctness` · `graph_index_review_fixes`

**操作**：`git mv duckdb/tests/spec/duckdb/{functions,index}/<name>.yaml duckdb/tests/spec/shared/<sub>/`，在 yaml 里加 `engines:` 字段（默认全部，无法在 PG 跑的写 `skip:`）。

### Group C — 迁移：真正 PG 专属或方言差异（共 13 个）

这部分必须在 `tests/spec/pg/` 下落 yaml，不能合并到 shared。

| pg_tests 文件 | 原因 | 目标位置 |
|---|---|---|
| `functions/distance_operators.sql` | PG 中缀运算符 `<-> <=> <~>` 的 EXPLAIN/操作符路径，shared 用 `${L2()}` 宏渲染只能验函数式 | `tests/spec/pg/functions/distance_operators.yaml` |
| `functions/distance_simd.sql` | PG 路径 SIMD 验证（dialect 不同） | `tests/spec/pg/functions/distance_simd.yaml` |
| `functions/distance_literal_bug.sql` | 字面量解析 bug，PG/DuckDB 解析器不同，必须分开 | `tests/spec/pg/functions/distance_literal_bug.yaml` |
| `functions/index_info.sql` | `vex_index_info()` 两边都有但列名/语义不完全对齐 | `tests/spec/pg/functions/index_info.yaml` |
| `functions/simd_arch.sql` | 当前 PG 标 SKIPPED，保留为 placeholder | `tests/spec/pg/functions/simd_arch.yaml`（`skip:`） |
| `index_basic/graph_index_brute_force_threshold.sql` | 历史项：`vex_brute_force_threshold` 为 DuckDB 侧参数，PG 已移除该 GUC | `tests/spec/pg/index/graph_index_brute_force_threshold.yaml` |
| `index_basic/graph_index_runtime_param_validation.sql` | 同上，runtime GUC 校验 PG 错误码不同 | `tests/spec/pg/index/graph_index_runtime_param_validation.yaml` |
| `index_basic/graph_index_memory_mode.sql` | 标 `KNOWN GAP: memory_mode is duck-only` —— PG 不该有该用例，删除即可 | **删除** |
| `index_pq/graph_index_pq_compact_reload.sql` | PG `pg_restart` 语义跟 DuckDB attach/detach 完全不同，restart 类用例两边逻辑不同源 | `tests/spec/pg/index/graph_index_pq_compact_reload.yaml` |
| `index_pq/graph_index_pq_restart_recall.sql` | 同上 | `tests/spec/pg/index/graph_index_pq_restart_recall.yaml` |
| `index_pq/graph_index_pq_persistence.sql` | 同上 | `tests/spec/pg/index/graph_index_pq_persistence.yaml` |
| `index_pq/graph_index_pq_real.sql` | PG 端真实数据 PQ 流程（含 `CREATE EXTENSION` / 卸载副作用） | `tests/spec/pg/index/graph_index_pq_real.yaml` |
| `index_pq/graph_index_pq_incremental.sql` | PG 增量 PQ 写入路径 | `tests/spec/pg/index/graph_index_pq_incremental.yaml` |
| `opengauss_adapted/ivfpq_basic_hnsw_adapted.sql` | openGauss 的 IVFPQ 改写，纯 og 专属 | `tests/spec/opengauss/index/ivfpq_basic_hnsw_adapted.yaml` |

> 实际是 14 项（含 1 项删除）。

## 执行顺序建议

1. **先做 Group B**（最大收益，一次性把 28 个用例变成跨引擎 spec）。
2. **再做 Group A**（diff + 删除，机械工作）。
3. **最后 Group C**（真要写新 yaml，工作量最大）—— 优先 `distance_operators` 和 `*_pq_*`，其余可以分批。
4. 收尾：`pg_tests/skipped/` 留空目录可以删；`pg_tests/` 整个目录最终应清空，PG 测试统一从 `build/spec/pg/` 跑。

## 留待确认

- **dialect 字典是否要扩**：Group C 里 `graph_index_brute_force_threshold` / `_runtime_param_validation` 之所以无法走 shared，是因为 GUC 名字没进 `dialects.yaml`。补一条 `${GUC_BRUTE_FORCE_THRESHOLD}` 之后，这两个就能下放到 shared，Group C 减到 12 项。
- **`distance_operators`**：如果在 `dialects.yaml` 把 `${L2_OP_EXPLAIN}` 这类宏补全，部分用例也能合到 shared，但对 DuckDB 侧噪音较大，留 PG 专属更干净。
- **DuckDB 80 个 yaml 中的清单**：未在本表出现的 ~52 个 DuckDB 专属 yaml（`graph_index_attach` / `_concurrent` / `_filtered_*` / `_memory_leak` / `_optimizer_*` / `_persistence*` / `_restart_*` / `_stress` / `_vacuum_serialize` / `bf_threshold_explain` 等）确实是 DuckDB 内部机制，**保留在 `tests/spec/duckdb/`** 不动。
