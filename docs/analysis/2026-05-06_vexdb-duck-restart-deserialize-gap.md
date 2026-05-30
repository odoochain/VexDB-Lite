# vexdb_duckdb 持久化 reload 不完整 — 5 个 restart 测试失败的根因

## 现象

测试 `graph_index_restart_delete/insert/update`、`graph_index_multi_restart`、`graph_index_persistence_full` 均在 restart 后做修改，再做 ANN 搜索时返回空结果。

## 复现最小路径

```sql
CREATE TABLE t1 (id INTEGER, vec FLOAT[4]);
INSERT INTO t1 SELECT i, ... FROM range(1, 11) t(i);
CREATE INDEX idx1 ON t1 USING GRAPH_INDEX (vec);
CHECKPOINT;
restart
DELETE FROM t1 WHERE id = 1;
SELECT count(*) FROM (SELECT id FROM t1 ORDER BY l2_distance(vec, [...]) LIMIT 5);
-- 期望 5，实际 0
```

观察：restart 后的纯只读 ANN 查询走 SEQ_SCAN（优化器没绑定到 index），返回正确结果；任何修改操作（DELETE/INSERT/UPDATE）触发 index 绑定后，下一个 ANN 查询走 VEX_INDEX_SCAN，`algo.search` 返回 0。

## 根因

`GraphIndex::DeserializeFromStorage`（`duckdb/vexdb_duckdb/index/graph_index.cpp:493`）只恢复了：

- `entry_info`（id、cur_layer_idx、level）
- `id_to_node_ptr_` / `upper_idx_to_ptr_`（IndexPointer 映射）
- 通过 `ResizeForReload` 把 `elems` / `base_points` / `upper_points` 重置为默认构造

**缺失**：实际的 `point_type` 数据（包括每个节点的 `tids`）和邻居连接（`base_points[i].dists`、`upper_points[i].dists` 中的真实邻居 id）。

`MakeBasePoint()` / `MakeUpperPoint()` 把 `dists` 全填为 `INVALID_DIST`，邻居 id 都是 sentinel。

后果：`algo.search` 从 entry_point 出发，调用 `search_layer`：

1. 初始候选只含 entry id=0
2. 在 `search_layer` 里读 entry 节点的邻居 — 全部 invalid，`is_valid(id)` 失败 → 跳出邻居循环
3. 没有新候选加入 `closest`，循环结束
4. `furthest` 中只有 entry 节点（如果它过了 filter）
5. 返回 ep 后，对每个 cand 调 `store.get_itempointer(cand.id, callback)` 读 `tids` — 但 elems[id].tids 是空（默认构造）
6. `res.first` 空 → 用户看到 0 行

## 修复方向

需要在 `DeserializeFromStorage` 中遍历 `id_to_node_ptr_`，对每个 `IndexPointer` 通过 `node_alloc_->Get(ptr)` 读取磁盘块上的 HNSW 节点头，把数据填入 `elems[id]`、`base_points[id]`、`vectors[id]`。同理对 `upper_idx_to_ptr_` → `upper_points[]`。

涉及解析 `vex::HNSWNodeHeader<T>` / `vex::HNSWUpperLevel<T>` 的二进制布局，以及把 `tids`、`vectors` 指针解引用并复制到内存结构。

工作量估计：1-2 天，需对 HNSW 节点段格式有清晰认识；建议参考 `BuildBulk` 写入路径反向实现读取。

## 当前状态（vexdb-unify-fixes 分支）

- 72/78 passing (92.3%)
- 6 failing：上述 5 个 restart 测试 + `graph_index_stress`（独立的 HNSW entry-point 偏向新插入节点的算法 bug，与 reload 无关）

## 相关代码位置

- `duckdb/vexdb_duckdb/index/graph_index.cpp:493` — DeserializeFromStorage
- `duckdb/vexdb_duckdb/include/vex_graph_index_depend_duck.hpp:444` — ResizeForReload
- `duckdb/include/graph_index/graph_index_algorithm.h:85` — search 入口
- `duckdb/include/graph_index/graph_index_algorithm.h:943` — search_layer
