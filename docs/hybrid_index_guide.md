# VexDB HybridIndex 使用指南

HybridIndex（混合索引）是 VexDB 扩展提供的**向量 + 标量分区联合索引**。它将数据按标量列的值自动分区，每个分区内部维护一个独立的 HNSW 图索引，从而在向量近邻搜索时可以高效地按标量条件过滤，避免全量扫描。

---

## 1. 适用场景

| 场景 | 说明 |
|------|------|
| 分类过滤搜索 | 商品按类目搜索、文档按标签搜索 |
| 多租户隔离 | 每个租户数据独立建图，互不干扰 |
| 枚举值过滤 | 颜色、状态、地区等离散值过滤 |

> **注意**：HybridIndex 的标量列适合**低基数**（分区数有限）的等值过滤场景。如果标量列基数极高（如用户 ID），每个分区数据量过少，图索引效果会下降。

---

## 2. 快速开始

### 2.1 加载扩展

```sql
LOAD vex;
```

### 2.2 创建表

```sql
CREATE TABLE products (
    id       INTEGER,
    category VARCHAR,
    vec      FLOAT[4]
);
```

- 向量列使用 `FLOAT[N]` 类型（DuckDB 原生固定长度数组类型）
- 标量过滤列支持 `VARCHAR`、`INTEGER` 等类型

### 2.3 插入数据

```sql
INSERT INTO products VALUES
  (1, 'electronics', [1.0, 0.0, 0.0, 0.0]::FLOAT[4]),
  (2, 'electronics', [0.9, 0.1, 0.0, 0.0]::FLOAT[4]),
  (3, 'electronics', [0.8, 0.2, 0.0, 0.0]::FLOAT[4]),
  (4, 'clothing',    [0.0, 1.0, 0.0, 0.0]::FLOAT[4]),
  (5, 'clothing',    [0.0, 0.9, 0.1, 0.0]::FLOAT[4]),
  (6, 'clothing',    [0.0, 0.8, 0.2, 0.0]::FLOAT[4]),
  (7, 'food',        [0.0, 0.0, 1.0, 0.0]::FLOAT[4]),
  (8, 'food',        [0.0, 0.0, 0.9, 0.1]::FLOAT[4]),
  (9, 'food',        [0.0, 0.0, 0.8, 0.2]::FLOAT[4]);
```

### 2.4 创建混合索引

```sql
CREATE INDEX idx_hybrid ON products USING GRAPH_INDEX (vec, category);
```

**语法要点**：
- 第一个列必须是 `FLOAT[N]` 向量列
- 第二个列是标量分区列（用于等值过滤）
- 系统会自动按标量列的不同值创建独立的 HNSW 分区图

---

## 3. 查询方式

### 3.1 分区过滤搜索（推荐）

在 `WHERE` 子句中指定标量列的等值条件，优化器会自动将查询路由到对应分区的 HNSW 图：

```sql
-- 在 electronics 分区内搜索最近邻
SELECT id, category FROM products
WHERE category = 'electronics'
ORDER BY l2_distance(vec, [1.0, 0.0, 0.0, 0.0]::FLOAT[4])
LIMIT 2;
```

结果：
```
1  electronics
2  electronics
```

```sql
-- 在 food 分区内搜索
SELECT id, category FROM products
WHERE category = 'food'
ORDER BY l2_distance(vec, [0.0, 0.0, 1.0, 0.0]::FLOAT[4])
LIMIT 2;
```

结果：
```
7  food
8  food
```

### 3.2 全局搜索（不带过滤）

不指定标量过滤条件时，系统会在**所有分区**中搜索，合并结果返回全局 Top-K：

```sql
SELECT id FROM products
ORDER BY l2_distance(vec, [0.0, 0.0, 0.0, 1.0]::FLOAT[4])
LIMIT 3;
```

结果：
```
9
8
6
```

### 3.3 支持的距离函数

| 函数 | 说明 |
|------|------|
| `l2_distance(a, b)` | 欧氏距离（L2） |
| `cosine_distance(a, b)` | 余弦距离 |
| `<->` | L2 距离运算符 |
| `<=>` | 余弦距离运算符 |
| `<#>` | 负内积运算符 |

---

## 4. 索引参数

通过 `WITH` 子句调整 HNSW 图的构建参数：

```sql
CREATE INDEX idx ON products USING GRAPH_INDEX (vec, category)
WITH (m = 16, ef_construction = 128);
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `m` | 16 | 每个节点的最大邻居数。越大召回率越高，内存越大 |
| `ef_construction` | 64 | 构建时的搜索宽度。越大索引质量越高，构建越慢 |

> `m` 的有效范围为 2~128。

---

## 5. 数据变更操作

### 5.1 插入新数据

直接 `INSERT`，新行会自动路由到对应分区。如果分区不存在则自动创建：

```sql
-- 向已有分区插入
INSERT INTO products VALUES (10, 'electronics', [0.7, 0.3, 0.0, 0.0]::FLOAT[4]);

-- 向新分区插入（自动创建 'toys' 分区）
INSERT INTO products VALUES (11, 'toys', [0.5, 0.5, 0.0, 0.0]::FLOAT[4]);
```

### 5.2 删除数据

支持删除单行、按条件删除、删除整个分区：

```sql
-- 删除单行
DELETE FROM products WHERE id = 1;

-- 删除整个分区
DELETE FROM products WHERE category = 'food';
```

删除后：
- 对应分区的 HNSW 图中节点被标记删除
- 搜索结果自动排除已删除的行
- 其他分区不受影响

### 5.3 重新插入已删除分区

已被完全删除的分区可以重新插入数据，系统会自动重建该分区的 HNSW 图：

```sql
-- 先删除整个分区
DELETE FROM products WHERE category = 'food';

-- 重新插入数据，分区自动重建
INSERT INTO products VALUES
  (20, 'food', [0.0, 0.0, 0.0, 0.8]::FLOAT[4]),
  (21, 'food', [0.0, 0.0, 0.2, 0.8]::FLOAT[4]);
```

### 5.4 更新向量

`UPDATE` 在 DuckDB 内部实现为 DELETE + INSERT，HybridIndex 能正确处理：

```sql
UPDATE products SET vec = [0.5, 0.5, 0.0, 0.0]::FLOAT[4] WHERE id = 1;
```

---

## 6. 持久化与重启

HybridIndex 支持持久化。执行 `CHECKPOINT` 后，索引数据会序列化到磁盘，重启后自动恢复：

```sql
-- 创建索引并写入数据
CREATE INDEX idx ON items USING GRAPH_INDEX (vec, category);

-- 持久化到磁盘
CHECKPOINT;

-- 重启后索引自动恢复，分区过滤搜索立即可用
SELECT id FROM items
WHERE category = 'A'
ORDER BY l2_distance(vec, [1.0, 0.0, 0.0, 0.0]::FLOAT[3])
LIMIT 2;
```

---

## 7. 优化器如何工作

VexDB 的查询优化器会自动识别以下查询模式并使用 HybridIndex 加速：

### 模式一：TopN + Filter + Get（推荐）

```sql
SELECT ... FROM table
WHERE scalar_col = 'value'
ORDER BY distance_func(vec_col, query_vec)
LIMIT k;
```

优化器识别到 `WHERE` 中的等值过滤条件后，直接调用 `FilteredSearch()`，仅在对应分区的 HNSW 图中搜索。

### 模式二：TopN + Get（无过滤）

```sql
SELECT ... FROM table
ORDER BY distance_func(vec_col, query_vec)
LIMIT k;
```

如果没有标量过滤条件，但表上存在 HybridIndex，优化器会调用 `GlobalSearch()`，在所有分区中搜索并合并 Top-K 结果。

### 模式三：带 pushed-down filter 的 TopN

当 DuckDB 将 `WHERE` 条件下推为 `TableFilter` 时，优化器同样能识别并使用分区过滤搜索。

---

## 8. 与 GraphIndex 的对比

| 特性 | GraphIndex | HybridIndex |
|------|-----------|-------------|
| 索引列 | 仅向量列 | 向量列 + 标量列 |
| 标量过滤 | 不支持（需后过滤） | 原生支持（分区内搜索） |
| 建索引语法 | `USING GRAPH_INDEX (vec)` | `USING GRAPH_INDEX (vec, scalar)` |
| PQ 量化 | 支持 | 不支持 |
| 适用场景 | 纯向量搜索 | 向量 + 标量联合过滤 |

---

## 9. 批量数据示例

使用 `range()` 函数批量生成数据并建索引：

```sql
CREATE TABLE products (id INTEGER, category VARCHAR, vec FLOAT[4]);

-- 批量插入 1000 条数据，分 5 个类目
INSERT INTO products
SELECT i, 'cat_a',
  [(i * 0.01)::FLOAT, (0.5 + i * 0.001)::FLOAT, 0.0::FLOAT, 0.0::FLOAT]::FLOAT[4]
FROM range(200) t(i);

INSERT INTO products
SELECT 200 + i, 'cat_b',
  [0.0::FLOAT, (i * 0.01)::FLOAT, (0.5 + i * 0.001)::FLOAT, 0.0::FLOAT]::FLOAT[4]
FROM range(200) t(i);

-- ... 更多类目 ...

CREATE INDEX idx_hybrid ON products USING GRAPH_INDEX (vec, category);

-- 分区搜索
SELECT id FROM products
WHERE category = 'cat_a'
ORDER BY l2_distance(vec, [0.0, 0.0, 0.0, 0.0]::FLOAT[4])
LIMIT 5;

-- 全局搜索
SELECT id, category FROM products
ORDER BY l2_distance(vec, [0.5, 0.5, 0.5, 0.5]::FLOAT[4])
LIMIT 10;
```

---

## 10. 注意事项

1. **列顺序**：`CREATE INDEX ... USING GRAPH_INDEX (vec_col, scalar_col)` 第一列必须是向量列，第二列是标量分区列，顺序不可颠倒。
2. **等值过滤**：优化器仅识别 `=` 等值过滤条件（如 `WHERE category = 'electronics'`），不支持 `LIKE`、`IN`、范围查询等。
3. **分区数量**：每个分区独立维护一个 HNSW 图。分区数过多（如上万个）会增加内存开销和全局搜索延迟。建议分区数控制在合理范围内。
4. **全局搜索代价**：不带过滤的全局搜索需要在所有分区中搜索并合并结果，代价与分区数成正比。
5. **ORDER BY + LIMIT**：优化器只在 `ORDER BY distance(...) LIMIT k` 模式下触发索引加速。没有 `LIMIT` 的查询不会使用索引。
