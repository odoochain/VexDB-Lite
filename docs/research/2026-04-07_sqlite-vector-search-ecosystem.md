# SQLite 生态向量搜索开源项目调研

> 调研日期: 2026-04-07
> 背景: VexDB-Lite 基于 DuckDB，功能包括 HNSW 索引、PQ 量化、L2/Cosine/IP 距离、混合分区索引。评估将这套能力迁移到 SQLite 的可行性。

---

## 一、核心项目详细调研

### 1. sqlite-vec (Alex Garcia)

| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/asg017/sqlite-vec |
| **Stars** | ~7,400 |
| **最新版本** | v0.1.9 (2026-03-31) |
| **License** | Apache 2.0 / MIT 双协议 |
| **技术栈** | 纯 C，零依赖 |
| **成熟度** | 中等 (pre-1.0，ANN 索引仍在开发中) |
| **赞助** | Mozilla Builders, Fly.io, Turso, SQLite Cloud |

**技术架构:**
- 基于 SQLite **Virtual Table** 机制实现，核心虚拟表名为 `vec0`
- 向量存储在 shadow tables 中，按 chunk 读取，不需要全部加载到内存
- 纯 C 实现，无外部依赖，二进制体积仅几百 KB

**向量类型:**
- Float32 (32-bit 浮点)
- Int8 (8-bit 有符号整数，标量量化)
- Binary (bit-packed，二进制量化)
- 支持 Matryoshka embeddings (可变长度向量)

**索引类型:**
- 当前主要是 **暴力搜索** (brute-force KNN)
- ANN 索引开发中 (Issue #25):
  - **DiskANN**: 已有 alpha 实现，被认为最适合 SQLite 的 B-tree 架构
  - **IVF**: 实验性支持，尚未默认启用
  - **HNSW**: 作者认为过于依赖内存结构，不适合 SQLite 的 shadow table 模型
- 作者明确表示 DiskANN 是未来主方向

**距离函数:**
- Euclidean (L2) 距离
- (Cosine 和 IP 支持情况不完全明确，文档主要提及 L2)

**查询语法:**
```sql
-- 创建虚拟表
CREATE VIRTUAL TABLE vec_items USING vec0(
  sample_embedding float[384]
);

-- 插入向量
INSERT INTO vec_items(rowid, sample_embedding)
  VALUES (1, '[0.1, 0.2, ...]');

-- KNN 查询
SELECT rowid, distance
FROM vec_items
WHERE sample_embedding MATCH '[0.890, 0.544, ...]'
ORDER BY distance
LIMIT 10;
```

**优势:**
- 极高的可移植性 (Linux/macOS/Windows/WASM/移动端/Raspberry Pi)
- 社区活跃，生态完善 (sqlite-lembed/sqlite-rembed 配套)
- 纯 C 零依赖，编译简单
- 内存效率高，chunk-based 读取

**劣势:**
- ANN 索引不成熟，大规模数据搜索性能有限
- 不支持 HNSW (作者主动放弃此方向)
- 距离函数种类有限

---

### 2. sqlite-vss (Alex Garcia, 已废弃)

| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/asg017/sqlite-vss |
| **Stars** | ~2,000 |
| **最新版本** | v0.1.2 (2023-08-06) |
| **状态** | **已废弃**，不再维护 |
| **技术栈** | C++，基于 Facebook Faiss |

**废弃原因:**
1. **平台限制**: 只能在 Linux + macOS 上运行，不支持 Windows/WASM/移动端
2. **编译困难**: Faiss 的 C++ 依赖极难编译，耗时长
3. **内存问题**: 所有向量必须加载到内存
4. **稳定性差**: 存在事务相关的 bug
5. **二进制体积大**: 3-5MB
6. **缺少功能**: 不支持量化等高级功能

**与 sqlite-vec 的关键区别:**
| 对比项 | sqlite-vss | sqlite-vec |
|--------|-----------|-----------|
| 语言 | C++ | 纯 C |
| 依赖 | Faiss (重量级) | 无 |
| 数据存储 | 外部索引文件 | SQLite shadow tables |
| 内存模式 | 全部加载到内存 | chunk-by-chunk 读取 |
| 平台支持 | Linux/macOS | 全平台 + WASM |
| 二进制体积 | 3-5MB | 几百 KB |
| ANN 算法 | Faiss (IVF/HNSW) | DiskANN (开发中) |

---

### 3. sqlite-vector (SQLite AI / Marco Bambini)

| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/sqliteai/sqlite-vector |
| **Stars** | ~822 |
| **License** | Elastic License 2.0 |
| **技术栈** | C + SIMD 优化 |
| **成熟度** | 中等 |

**技术架构:**
- **不使用 Virtual Table**，向量直接存储在标准 SQLite 表中作为 BLOB
- **无需预索引**: 不依赖 DiskANN/HNSW/IVF，开箱即用
- 默认 30MB 内存占用
- 硬件特定的 SIMD 加速距离计算

**向量类型:** float32, float16, bfloat16, int8, uint8, 1bit

**距离函数 (最丰富):**
- L2 Distance (Euclidean)
- Squared L2
- L1 Distance (Manhattan)
- Cosine Distance
- Dot Product (内积)
- Hamming Distance (仅 1bit 向量)

**性能基准 (100K 向量, 384维 FLOAT32, M1 Pro):**
| 指标 | sqlite-vec | sqlite-vector |
|------|-----------|--------------|
| 插入时间 | 1,179 ms | 563 ms |
| 全扫描查询 | 67.84 ms | 56.65 ms |
| 量化查询 | - | 17.44 ms |
| 量化+预加载 | - | 3.97 ms |

**重要注意:** Elastic License 2.0 不是纯开源协议，对商业使用有限制。

---

### 4. Vec1 (SQLite 官方/社区)

| 属性 | 详情 |
|------|------|
| **文档** | https://sqlite.org/vec1/doc/trunk/doc/vec1.md |
| **Stars** | N/A (Fossil 仓库) |
| **状态** | 开发中，尚未正式发布 |
| **技术栈** | 纯 C，无依赖 |

**技术特点:**
- 使用 **IVFADC** (Inverted File with Asymmetric Distance Computation) + **OPQ** (Optimized Product Quantization)
- SIMD 加速: AVX2 (x86) + NEON (ARM)
- Virtual Table 接口
- 支持 Euclidean (L2) 和 Cosine 距离

**意义:** 这可能成为 SQLite 官方的向量搜索方案。一旦发布，可能对第三方扩展产生较大影响。目前"测试严重不足"，无具体发布日期。

---

### 5. Vectorlite

| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/1yefuwang1/vectorlite |
| **Stars** | ~357 |
| **技术栈** | C++，基于 hnswlib + Google Highway (SIMD) |
| **成熟度** | 早期 |

**技术特点:**
- **HNSW 索引** (基于 hnswlib)
- SIMD 加速通过 Google Highway 库 (与我们 VexDB 的 SIMD dispatch 理念类似)
- 支持 L2, Cosine, Inner Product 距离
- 可配置 HNSW 参数 (max_elements, ef_construction, M)
- 支持索引序列化/反序列化到文件
- Python 和 Node.js 绑定

**查询语法:**
```sql
-- 使用专用函数
SELECT * FROM knn_search(my_table, vector_from_json('[1,2,3]'), 10);
SELECT vector_distance(v1, v2, 'l2') FROM ...;
```

**与 VexDB-Lite 的相关性:** 这是 SQLite 生态中最接近我们 HNSW 实现的项目，但成熟度较低，且依赖 hnswlib 而非自研。

---

### 6. libSQL / Turso (SQLite Fork)

| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/tursodatabase/libsql |
| **Stars** | 很高 (大型项目) |
| **技术栈** | C (SQLite fork) + Rust |

**技术特点:**
- **不是扩展，而是 SQLite Fork** (libSQL)
- 向量是原生列类型，零配置
- 使用 **DiskANN** 算法，集成到 SQLite 索引系统中
- 自动更新向量索引
- 支持 1-bit 量化压缩
- 通过 `vector_top_k` 虚拟表查询

**意义:** 如果接受 fork SQLite 的方案，Turso/libSQL 是最成熟的实现。但这意味着放弃标准 SQLite 兼容性。

---

### 7. USearch (SQLite 绑定)

| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/unum-cloud/usearch |
| **Stars** | ~4,000 |
| **技术栈** | C++ (主库)，多语言绑定 |

**SQLite 集成:**
- 提供 SIMD 加速的距离函数扩展
- 支持: Cosine, Euclidean (float), Jaccard, Hamming (binary), Levenshtein (string), Haversine (geo)
- 使用 HNSW 算法，比 Faiss 快约 10x
- SQLite 扩展作为 Python wheel 的一部分分发
- 向量以 BLOB 存储，也支持 JSON 格式

**注意:** USearch 的 SQLite 集成主要提供距离函数，而非完整的索引管理方案。

---

### 8. sqlite-vec 社区 Fork (vlasky)

| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/vlasky/sqlite-vec |
| **Stars** | ~65 |

**新增功能:**
- **距离约束**: 按距离阈值过滤 KNN 结果 (`<`, `<=`, `>`, `>=`)
- **游标分页**: 基于距离值的高效分页，避免 OFFSET
- **空间优化**: `optimize` 命令压缩 shadow tables，回收删除后的空间

---

## 二、配套生态

### Alex Garcia 的 SQLite 向量生态

| 项目 | 功能 | GitHub |
|------|------|--------|
| sqlite-vec | 向量存储与搜索 | asg017/sqlite-vec |
| sqlite-lembed | 本地生成 embedding (GGUF/llama.cpp) | asg017/sqlite-lembed |
| sqlite-rembed | 远程 API 生成 embedding (OpenAI/Ollama) | asg017/sqlite-rembed |

这三个项目构成了一个完整的 "embedding 生成 + 向量搜索" 端到端方案。

---

## 三、SQLite 扩展开发框架

### 1. C/C++ 方案

**官方文档:** https://sqlite.org/loadext.html

**核心机制:**
- **Loadable Extension**: 运行时加载 (.so/.dylib/.dll)
- **Virtual Table**: 实现自定义表引擎的标准接口
- **Shadow Tables**: 虚拟表使用的底层存储表 (如 FTS5 使用 `%_content`, `%_segdir` 等)
- **Scalar/Aggregate Functions**: 自定义 SQL 函数

**模板项目:**
- [sqlite-extension-template](https://github.com/timlrx/sqlite-extension-template) (12 stars)
  - 支持: loadable module, static library, Python wheel, WASM
  - 语言: C/Makefile/Python/CMake
  - 包含示例函数和虚拟表实现

**最佳实践:**
- Shadow tables 在 SQLITE_DBCONFIG_DEFENSIVE 模式下自动变为只读
- 虚拟表实现应能检测和处理损坏的 shadow table 数据
- 使用 `sqlite3_auto_extension()` 实现持久化加载

### 2. Rust 方案

**sqlite-loadable-rs** (Alex Garcia)
| 属性 | 详情 |
|------|------|
| **GitHub** | https://github.com/asg017/sqlite-loadable-rs |
| **Stars** | ~400 |
| **状态** | Beta，代码不稳定 |

**特点:**
- 支持 Scalar Functions, Table Functions, Virtual Tables (含读写)
- 性能接近 C (仅慢 10-15%)
- 比 Go 实现快 20-30x
- 二进制体积较大 (~469KB vs C 的 17KB)
- WASM 编译支持有限
- 灵感来自 rusqlite, pgx

### 3. 跨平台编译方案

| 方案 | 适用场景 | 成熟度 |
|------|---------|--------|
| CMake + C | 最通用，全平台支持 | 非常成熟 |
| Makefile + C | 简单项目 | 成熟 |
| cargo + Rust | Rust 生态 | Beta |
| Emscripten (WASM) | 浏览器 | 成熟 |

sqlite-vec 的纯 C + 零依赖方案证明了这条路线的可行性，能覆盖所有主要平台。

---

## 四、对比总结

| 项目 | Stars | 索引类型 | 距离函数 | 语言 | 依赖 | License | 可集成性 |
|------|-------|---------|---------|------|------|---------|---------|
| **sqlite-vec** | 7.4K | Brute-force + DiskANN(alpha) | L2 | C | 无 | Apache 2.0/MIT | 高 |
| **sqlite-vector** | 822 | 无索引(暴力+优化) | L2/Cosine/IP/L1/Hamming | C | 无 | Elastic 2.0 | 中(协议限制) |
| **Vec1** | N/A | IVFADC + OPQ | L2/Cosine | C | 无 | 未知 | 低(未发布) |
| **Vectorlite** | 357 | HNSW | L2/Cosine/IP | C++ | hnswlib, Highway | 未明确 | 中 |
| **libSQL** | 高 | DiskANN | 多种 | C/Rust | SQLite fork | 开源 | 低(需fork) |
| **USearch** | 4K | HNSW | 多种 | C++ | 多 | 开源 | 中(仅距离函数) |
| **sqlite-vss** | 2K | IVF/HNSW (Faiss) | 多种 | C++ | Faiss | MIT | 废弃 |

---

## 五、对 VexDB-Lite SQLite 迁移的建议

### 竞争格局分析

1. **sqlite-vec 是当前的主流选择**，但其 ANN 索引仍在 alpha 阶段，且明确放弃 HNSW
2. **没有成熟的 SQLite HNSW 实现** -- 这是一个明确的市场空白
3. **PQ 量化在 SQLite 生态几乎空白** -- Vec1 的 OPQ 尚未发布，sqlite-vec 仅有标量量化
4. **混合分区索引在 SQLite 生态中完全没有** -- 这是 VexDB-Lite 的独特优势

### VexDB-Lite 的差异化优势

如果迁移到 SQLite，我们的核心差异化点：
- **HNSW 索引**: SQLite 生态中无成熟实现 (Vectorlite 太弱，USearch 不完整)
- **PQ 量化**: 唯一的 PQ 实现 (非标量量化)
- **混合分区索引**: 独有功能
- **完整的距离函数集**: L2 + Cosine + IP
- **多线程构建**: 大数据集并行索引构建

### 技术路线建议

1. **推荐: 纯 C 实现 + Virtual Table + Shadow Tables**
   - 参考 sqlite-vec 的架构 (零依赖，chunk-based 存储)
   - 但使用我们自己的 HNSW + PQ 算法
   - 这是被验证过的最佳可移植性方案

2. **存储层设计:**
   - 向量数据存 shadow tables (BLOB 格式)
   - HNSW 图结构存 shadow tables (类似 DuckDB 的 FixedSizeAllocator，但基于 SQLite pages)
   - PQ codebook 存 shadow tables

3. **SIMD 加速:**
   - 参考 sqlite-vector 和 USearch 的运行时 SIMD 检测
   - 复用 VexDB-Lite 现有的 distance/ 目录代码 (SSE/AVX2/NEON/WASM)

4. **查询接口:**
   - `WHERE vec_column MATCH query_vector ORDER BY distance LIMIT k` (sqlite-vec 风格)
   - 或自定义函数 `vex_search(table, query, k)` (vectorlite 风格)

---

## Sources

- [sqlite-vec GitHub](https://github.com/asg017/sqlite-vec)
- [sqlite-vss GitHub](https://github.com/asg017/sqlite-vss)
- [sqlite-vector GitHub](https://github.com/sqliteai/sqlite-vector)
- [Vectorlite GitHub](https://github.com/1yefuwang1/vectorlite)
- [USearch GitHub](https://github.com/unum-cloud/usearch)
- [libSQL GitHub](https://github.com/tursodatabase/libsql)
- [Vec1 文档](https://sqlite.org/vec1/doc/trunk/doc/vec1.md)
- [sqlite-loadable-rs](https://github.com/asg017/sqlite-loadable-rs)
- [sqlite-extension-template](https://github.com/timlrx/sqlite-extension-template)
- [sqlite-vec 社区 Fork](https://github.com/vlasky/sqlite-vec)
- [sqlite-lembed](https://github.com/asg017/sqlite-lembed)
- [sqlite-rembed](https://github.com/asg017/sqlite-rembed)
- [Alex Garcia: Building new vector search SQLite](https://alexgarcia.xyz/blog/2024/building-new-vector-search-sqlite/index.html)
- [sqlite-vec ANN Tracking Issue #25](https://github.com/asg017/sqlite-vec/issues/25)
- [The State of Vector Search in SQLite](https://marcobambini.substack.com/p/the-state-of-vector-search-in-sqlite)
- [HN: I'm writing a new vector search SQLite Extension](https://news.ycombinator.com/item?id=40243168)
- [HN: Ultra efficient vector extension for SQLite](https://news.ycombinator.com/item?id=45347619)
- [Turso: Native Vector Search to SQLite](https://turso.tech/blog/turso-brings-native-vector-search-to-sqlite)
- [SQLite Loadable Extensions 官方文档](https://sqlite.org/loadext.html)
- [SQLite Virtual Table 机制](https://www.sqlite.org/vtab.html)
