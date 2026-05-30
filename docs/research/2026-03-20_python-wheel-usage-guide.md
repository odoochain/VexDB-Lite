# VexDB-Lite Python Wheel 使用教程

## 安装

```bash
pip install dist/duckdb-1.5.0-cp312-cp312-macosx_11_0_arm64.whl --force-reinstall --no-deps
```

> 注意：此 wheel 包基于 DuckDB 1.5.0，内置了 VEX 向量搜索扩展，安装后包名仍为 `duckdb`。

## 快速开始

```python
import duckdb

# 创建连接（内存模式或持久化模式）
con = duckdb.connect()          # 内存模式
# con = duckdb.connect('my.db') # 持久化到文件

print(f'DuckDB version: {duckdb.__version__}')
```

## 核心功能

### 1. 向量类型

VexDB-Lite 使用 DuckDB 原生的 `FLOAT[N]` 类型存储向量：

```python
con.execute('''
    CREATE TABLE documents (
        id INTEGER,
        title VARCHAR,
        embedding FLOAT[768]    -- 768维向量
    )
''')
```

### 2. 距离函数

支持三种距离度量：

```python
# L2 距离（欧氏距离）
con.execute("SELECT l2_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3])").fetchone()
# => (1.4142135623730951,)

# 余弦距离（1 - cosine_similarity）
con.execute("SELECT cosine_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3])").fetchone()
# => (1.0,)

# 内积距离（用于 MaxMIPS）
# 使用 array_inner_product 或通过 metric='ip' 的索引
```

### 3. 创建 HNSW 索引

```python
# L2 度量索引
con.execute('''
    CREATE INDEX embedding_idx ON documents
    USING GRAPH_INDEX (embedding)
    WITH (metric = 'l2')
''')

# 余弦度量索引
con.execute('''
    CREATE INDEX embedding_idx ON documents
    USING GRAPH_INDEX (embedding)
    WITH (metric = 'cosine')
''')

# 内积度量索引
con.execute('''
    CREATE INDEX embedding_idx ON documents
    USING GRAPH_INDEX (embedding)
    WITH (metric = 'ip')
''')
```

### 4. ANN 近似最近邻搜索

通过 `ORDER BY distance_func(...) LIMIT k` 自动走索引：

```python
query_vec = [0.1, 0.2, ...]  # 查询向量

results = con.execute('''
    SELECT id, title, cosine_distance(embedding, $1::FLOAT[768]) as dist
    FROM documents
    ORDER BY cosine_distance(embedding, $1::FLOAT[768])
    LIMIT 10
''', [query_vec]).fetchall()

for row in results:
    print(f'id={row[0]}, title={row[1]}, distance={row[2]:.4f}')
```

### 5. 查看索引信息

```python
info = con.execute("SELECT * FROM vex_index_info()").fetchall()
for row in info:
    print(row)
# (index_name, index_type, table_name, partition_col, node_count, max_level, dimension, row_count)
```

### 6. 调整搜索参数

```python
# ef_search: 搜索扩展因子，越大召回率越高但越慢（默认40）
con.execute("SET vex_ef_search = 100")

# 注：vex_brute_force_threshold 仅适用于 DuckDB 侧。
```

## 完整示例：语义搜索

```python
import duckdb
import numpy as np

con = duckdb.connect('semantic_search.db')

# 建表
con.execute('''
    CREATE TABLE IF NOT EXISTS articles (
        id INTEGER PRIMARY KEY,
        title VARCHAR,
        content VARCHAR,
        embedding FLOAT[384]
    )
''')

# 插入数据（假设已有 embedding）
embeddings = np.random.randn(1000, 384).astype(np.float32)
for i in range(1000):
    con.execute(
        "INSERT INTO articles VALUES (?, ?, ?, ?)",
        [i, f"Article {i}", f"Content of article {i}", embeddings[i].tolist()]
    )

# 建索引
con.execute('''
    CREATE INDEX IF NOT EXISTS article_idx ON articles
    USING GRAPH_INDEX (embedding)
    WITH (metric = 'cosine')
''')

# 搜索
query = np.random.randn(384).astype(np.float32).tolist()
con.execute("SET vex_ef_search = 64")
results = con.execute('''
    SELECT id, title, cosine_distance(embedding, $1::FLOAT[384]) as dist
    FROM articles
    ORDER BY cosine_distance(embedding, $1::FLOAT[384])
    LIMIT 5
''', [query]).fetchall()

for row in results:
    print(f'[{row[2]:.4f}] {row[1]}')
```

## 构建 Wheel 包

```bash
cd /path/to/vexdb_lite
bash packaging/build_wheel.sh          # 输出到 dist/
bash packaging/build_wheel.sh /tmp/out # 指定输出目录
```

前置依赖：
```bash
pip install scikit-build-core pybind11 setuptools_scm ninja
```

## 注意事项

- 索引类型名为 `GRAPH_INDEX`（不是 `GRAPH` 或 `HNSW`）
- 距离函数名：`l2_distance`、`cosine_distance`（不带 `vex_` 前缀）
- 索引创建的 `WITH` 选项中 metric 值用**单引号**：`metric = 'l2'`
- 此 wheel 会替换系统已安装的 `duckdb` 包
