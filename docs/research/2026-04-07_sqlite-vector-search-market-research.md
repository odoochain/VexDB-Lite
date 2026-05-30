# 基于 SQLite 的向量搜索市场调研报告

> 调研日期：2026-04-07
> 目的：为 VexDB-Lite 向 SQLite 平台迁移提供市场情报

---

## 一、基于 SQLite 的向量搜索产品

### 1. sqlite-vec（Alex Garcia）

| 项目 | 详情 |
|------|------|
| **URL** | https://github.com/asg017/sqlite-vec |
| **GitHub Stars** | 7.4k |
| **许可证** | MIT / Apache-2.0 双许可 |
| **最新版本** | v0.1.9（2026-03-31） |
| **语言** | 纯 C，零依赖 |
| **赞助方** | Mozilla Builders、Fly.io、Turso、SQLite Cloud、Shinkai |

**核心功能：**
- 通过 `vec0` 虚拟表存储和查询 float、int8、binary 向量
- 支持 KNN 搜索，SIMD 加速（AVX/NEON）L2 距离
- 纯暴力搜索，无 ANN 索引（适合中小数据集）
- 支持辅助列和分区键做元数据过滤
- 支持混合全文+向量搜索（Reciprocal Rank Fusion）

**平台支持：** Linux、macOS、Windows、WASM（浏览器）、Raspberry Pi、iOS/Android（理论支持）

**语言绑定：** Python、Node.js、Ruby、Go、Rust、Datasette、rqlite

**定价：** 完全免费开源

**目标用户：** 需要轻量级、零依赖向量搜索的开发者；本地优先应用；AI 原型开发

**优点：**
- 零依赖，真正跨平台
- 社区最活跃，生态最好（LangChain 已集成）
- MIT 许可，无任何商业限制
- 与 SQLite 生态天然融合

**缺点：**
- 纯暴力搜索，大数据集性能差（无 HNSW/IVF 等 ANN 算法）
- 不支持量化压缩
- 功能相对基础

---

### 2. sqlite-vector（SQLite Cloud / SQLiteAI）

| 项目 | 详情 |
|------|------|
| **URL** | https://github.com/sqliteai/sqlite-vector |
| **官网** | https://www.sqlite.ai/sqlite-vector |
| **GitHub Stars** | ~822 |
| **许可证** | Elastic License 2.0（非真正开源）+ 商业许可 |
| **语言** | C，SIMD 优化 |

**核心功能：**
- 直接在普通 SQLite 表中以 BLOB 存储向量（无需虚拟表）
- 支持 Float32、Float16、BFloat16、Int8、UInt8、1-bit 量化
- L2、L1、Cosine、Dot Product、Squared L2、Hamming 六种距离
- 零预处理，即插即查
- 默认仅 30MB 内存

**平台支持：** iOS、Android、Windows、Linux、macOS、WebAssembly

**语言绑定：** Python、Swift、Kotlin/Java、Dart/Flutter、C

**定价：**
- 开源项目（OSI 许可）：免费
- 商业生产环境：需商业许可（价格未公开）
- SQLite Cloud 托管服务：Free($0) / Dev($19/月) / Pro($39/月) / Enterprise(自定义)

**目标用户：** 移动端/边缘 AI 开发者；需要多量化格式的场景；商业产品集成

**优点：**
- 量化支持极其丰富（6种格式）
- 移动端原生支持好（Swift/Kotlin/Flutter）
- 内存效率高
- 有完整商业化公司支撑

**缺点：**
- Elastic License 2.0 引发社区争议，不是真正开源
- 也是暴力搜索，无 ANN 索引
- 社区规模远小于 sqlite-vec
- 不能嵌入其他开源项目

---

### 3. Turso / libSQL 向量搜索

| 项目 | 详情 |
|------|------|
| **URL** | https://turso.tech/vector |
| **GitHub** | https://github.com/tursodatabase/turso |
| **许可证** | MIT（libSQL） |
| **算法** | DiskANN |

**核心功能：**
- 原生集成在 libSQL 中，无需额外扩展
- `F32_BLOB(N)` 向量列类型，像普通列一样使用
- 自动维护 ANN 索引（DiskANN 算法）
- 精确搜索和近似搜索都支持
- 支持嵌入式副本（embedded replicas）实现离线向量搜索

**平台支持：** 所有 libSQL 支持的平台，含 iOS、Android SDK

**定价：**
- Free: $0（500M 行读/月，5GB 存储）
- Developer: $4.99/月
- Scaler: $24.92/月
- Pro: $416.58/月
- Enterprise: 自定义

**目标用户：** 全栈开发者；需要云+边缘同步的应用；移动端 RAG 场景

**优点：**
- 唯一内置 ANN 索引（DiskANN）的 SQLite 方案
- 零配置，向量搜索即开即用
- 云到边缘数据同步能力
- 有移动端 React Native 实际案例（Kin AI）

**缺点：**
- 是 SQLite 分支而非扩展，兼容性有取舍
- DiskANN 而非 HNSW，调优参数不同
- 云服务绑定较深
- 社区小于原生 SQLite 生态

---

### 4. Vectorlite

| 项目 | 详情 |
|------|------|
| **URL** | https://github.com/1yefuwang1/vectorlite |
| **GitHub Stars** | ~357 |
| **许可证** | 仓库中有 LICENSE 文件（未明确说明类型） |
| **算法** | HNSW（基于 hnswlib） |
| **语言** | C++，依赖 hnswlib + Google Highway |

**核心功能：**
- 基于 hnswlib 的快速 ANN 搜索
- HNSW 参数完全可控（ef_construction, M 等）
- Google Highway 库实现 SIMD 加速，距离计算比 hnswlib 快 1.5-3x
- 支持 L2、Cosine、Inner Product
- 支持元数据过滤（rowid filter, 需 SQLite >= 3.38）

**平台支持：** Windows x64、Linux x64、macOS x64/ARM64

**定价：** 免费开源

**目标用户：** 需要高性能 ANN 搜索且使用 SQLite 的开发者

**优点：**
- 向量查询速度比 sqlite-vec 快 8-100x
- 比 sqlite-vss 快 10x 插入、2-40x 搜索
- HNSW 参数可调
- 支持元数据过滤

**缺点：**
- 依赖 hnswlib 和 Highway，非零依赖
- 不支持移动端（iOS/Android）
- 社区规模小
- 不支持量化

---

### 5. sqlite-vss（已停止开发）

| 项目 | 详情 |
|------|------|
| **URL** | https://github.com/asg017/sqlite-vss |
| **状态** | 不再活跃开发，已被 sqlite-vec 取代 |
| **算法** | 基于 Faiss |

sqlite-vss 是 Alex Garcia 的早期作品，基于 Facebook Faiss 库。由于 Faiss 依赖复杂，安装困难，作者已全面转向零依赖的 sqlite-vec。

---

## 二、嵌入式向量数据库竞品（非 SQLite）

### 1. Chroma

| 项目 | 详情 |
|------|------|
| **URL** | https://www.trychroma.com |
| **类型** | 嵌入式向量数据库 |
| **语言** | Python 优先 |
| **许可证** | Apache-2.0 |
| **定价** | 开源免费；Cloud 版新账号 $5 免费额度，后按用量计费 |

**核心特点：**
- Python API 极其简洁，5 行代码即可开始
- 支持向量+全文+混合搜索
- 嵌入式模式零网络延迟
- 写入性能优化好，适合频繁更新数据集
- 10M 向量以下自托管零成本

**与 SQLite 方案对比：**
- 优势：API 最简单，Python 生态最好，社区活跃
- 劣势：仅 Python/JS，不支持移动端，非 SQL 接口，无法复用已有 SQLite 数据

---

### 2. LanceDB

| 项目 | 详情 |
|------|------|
| **URL** | https://lancedb.com |
| **类型** | 嵌入式多模态向量数据库 |
| **语言** | Rust 核心 |
| **许可证** | Apache-2.0 |
| **定价** | 开源免费；Cloud $16.03/月起；Enterprise 自定义 |
| **融资** | $8M（2024） |

**核心特点：**
- 基于自研 Lance 数据格式（比 Parquet 快 100x）
- 支持多模态数据（图片、文本、视频）
- 内存映射文件访问，无需全量加载到 RAM
- 无服务器架构，嵌入应用代码
- 10 亿向量在笔记本上可在 100ms 内搜索

**与 SQLite 方案对比：**
- 优势：性能极强，多模态原生支持，Rust 实现安全高效
- 劣势：自有数据格式，无法复用 SQLite 生态，移动端支持有限

---

### 3. Qdrant

| 项目 | 详情 |
|------|------|
| **URL** | https://qdrant.tech |
| **类型** | 高性能向量数据库（含嵌入模式） |
| **语言** | Rust |
| **许可证** | Apache-2.0 |
| **嵌入模式** | Local Mode（Python）+ Qdrant Edge（私有测试） |

**核心特点：**
- Local Mode：进程内运行，支持内存或 SQLite 持久化
- 适合开发测试和 <20K 向量的小数据集
- API 与云端完全一致，可无缝迁移
- Qdrant Edge：面向 IoT/移动端的轻量版，目前私有测试

**与 SQLite 方案对比：**
- 优势：生产级功能完整，有成熟的云服务
- 劣势：Local Mode 数据量限制大，Edge 版未公开发布，Python 限定

---

### 4. USearch

| 项目 | 详情 |
|------|------|
| **URL** | https://github.com/unum-cloud/usearch |
| **类型** | 嵌入式向量搜索库 |
| **语言** | C++ 核心 |
| **许可证** | Apache-2.0 |
| **绑定** | C++、C、Python、JS、Rust、Java、ObjC、Swift、C#、Go、Wolfram |

**核心特点：**
- HNSW 实现，比 FAISS 快 10x
- 支持自定义距离度量
- 内存映射文件，无需全量加载 RAM
- 单文件头文件库
- 被 ScyllaDB、YugabyteDB 等集成为向量索引后端

**与 SQLite 方案对比：**
- 优势：性能极致，语言绑定最广，可嵌入任何系统
- 劣势：纯搜索库非数据库，无 SQL 接口，无 CRUD 管理

---

### 5. Zvec（阿里巴巴）

| 项目 | 详情 |
|------|------|
| **URL** | https://github.com/alibaba/zvec |
| **GitHub Stars** | 9.3k |
| **许可证** | Apache-2.0 |
| **最新版本** | v0.3.0（2026-04-03） |
| **语言** | 基于 Proxima 引擎 |

**核心特点：**
- 定位"向量数据库的 SQLite"
- 基于阿里巴巴 Proxima 生产级搜索引擎
- 支持密集+稀疏向量、多向量查询
- 混合搜索（语义+标量过滤）
- 内置重排序器（加权融合 + RRF）
- ACID 合规，WAL 日志
- RabitQ 量化，CPU 自动派发优化
- 二进制 <5MB
- v0.3.0 新增 Windows、Android 支持和 C-API

**与 SQLite 方案对比：**
- 优势：功能最完整（量化、混合搜索、重排序），阿里巴巴背书
- 劣势：自有存储格式（.zvdb），非 SQLite 扩展，生态刚起步

---

## 三、移动端/边缘计算场景分析

### 当前市场现状

移动端/边缘场景是 SQLite 向量搜索的**核心差异化战场**。主要玩家布局：

| 产品 | iOS | Android | WASM | 移动端 SDK |
|------|-----|---------|------|-----------|
| sqlite-vec | 理论支持 | 理论支持 | 是 | 无原生 SDK |
| sqlite-vector | 是 | 是 | 是 | Swift/Kotlin/Flutter |
| Turso/libSQL | 是 | 是 | - | 官方 iOS/Android SDK |
| Zvec | - | v0.3.0 新增 | - | C-API |
| Chroma | 否 | 否 | - | 无 |
| LanceDB | 有限 | 有限 | - | 无原生 |
| USearch | 是 | 是 | 是 | ObjC/Swift/Java 绑定 |

### 实际应用案例

1. **Kin AI**：本地优先 AI 助手，使用 Turso libSQL 在 iOS/Android 上实现设备端向量搜索和个人知识图谱
2. **On-Device RAG**：Google 开发者专家在 2026 年初发布移动端 RAG 开发指南，使用 SQLite 向量搜索
3. **SQLiteAI 生态**：sqlite-vector + sqlite-ai 组合，支持在设备端运行 GGUF 模型做 embedding 生成和推理

### 边缘计算场景需求

- **隐私保护**：数据不出设备，敏感场景刚需
- **离线可用**：无网络环境下的语义搜索
- **低延迟**：消除网络往返，毫秒级响应
- **资源受限**：内存 <100MB，存储有限，CPU 受限
- **On-Device RAG**：2025-2026 最热趋势，LLM + 本地知识库

### 结论

移动端/边缘计算**确实是 SQLite 向量搜索最重要的应用场景**。这也是 SQLite 方案相对于 Chroma/Qdrant 等服务端方案最大的差异化优势所在。

---

## 四、用户反馈与市场分析

### HackerNews / 社区热门讨论

1. **"Vector databases are the wrong abstraction"（2024.11）**
   - 核心观点：将向量放在独立数据库中增加同步成本，不如在现有数据库中添加向量能力
   - 这一观点有利于 SQLite 扩展路线

2. **sqlite-vec 发布讨论（2024.05 & 2024.08）**
   - 社区对零依赖、跨平台的方案高度认可
   - 最大关切：暴力搜索的性能天花板

3. **sqlite-vector 许可证争议（2026.03）**
   - Elastic License 2.0 引发强烈反对
   - 社区明确偏好真正的开源许可（MIT/Apache-2.0）
   - 技术上认可其量化和移动端支持

4. **"Are we at peak vector database?"（2024.01）**
   - 反思独立向量数据库是否是过度设计
   - 越来越多人倾向于在现有数据库中集成向量搜索

### 用户最关心的功能（优先级排序）

1. **易用性** - SQL 接口、零配置、即插即用
2. **跨平台兼容性** - 尤其是移动端和 WASM
3. **许可证自由** - MIT/Apache-2.0 强烈偏好
4. **ANN 搜索性能** - 大数据集场景的核心需求
5. **与现有 SQLite 生态集成** - 不想维护两套系统
6. **量化/压缩** - 边缘设备内存受限
7. **混合搜索** - 向量 + 全文 + 标量过滤

### 市场发展趋势

1. **"回归现有数据库"趋势**：独立向量数据库热潮降温，集成在 PostgreSQL/SQLite/DuckDB 中成为主流
2. **On-Device AI 爆发**：2025-2026 边缘 AI 从概念走向产品，SQLite 向量搜索是基础设施
3. **Agentic RAG**：AI Agent 动态检索模式成为主流，需要轻量级本地向量存储
4. **大厂入局**：阿里巴巴（Zvec）、Google（On-Device RAG 指南）等巨头推动市场
5. **Chroma/LanceDB 市场份额下降**：2026 年 mindshare 分别从 13.4%->7.6% 和 9.5%->6.6%
6. **许可证成为竞争要素**：社区对非开源许可反感强烈

---

## 五、市场格局总览

### 竞争象限

```
        高性能 ANN
            |
   Zvec     |    Turso/libSQL
   USearch   |    Vectorlite
            |
  ──────────┼──────────
            |
   LanceDB  |    sqlite-vec
   Chroma   |    sqlite-vector
            |
        暴力搜索/简单 ANN

  独立方案 ←──────→ SQLite 生态
```

### 关键空白与机会

| 空白点 | 说明 |
|--------|------|
| **SQLite 扩展 + HNSW + 移动端** | 目前无产品完美覆盖。Vectorlite 有 HNSW 但不支持移动端；sqlite-vec 跨平台但无 ANN；sqlite-vector 跨平台但无 ANN；Turso 有 ANN 但是 SQLite 分支 |
| **SQLite 扩展 + 量化 + ANN** | sqlite-vector 有量化但无 ANN；Vectorlite 有 ANN 但无量化 |
| **真正开源 + 高性能 + 移动端** | sqlite-vector 有移动端但非开源；sqlite-vec 开源但性能受限 |

### VexDB-Lite 的机会分析

VexDB-Lite 如果迁移到 SQLite，其核心竞争力在于：

1. **HNSW + SQLite 扩展 + 移动端原生**：目前市场上这三者的交集为空
2. **量化（PQ）+ ANN 索引**：sqlite-vector 有量化无索引，Vectorlite 有索引无量化，VexDB 两者兼备
3. **MIT/Apache-2.0 许可**：社区明确偏好，可获得开发者信任
4. **已有 iOS/Android 构建经验**：从 DuckDB 迁移到 SQLite 后二进制更小、集成更简单

**最大威胁：**
- Zvec（阿里巴巴）：功能全面，9.3k stars，增长迅猛，但非 SQLite 扩展
- Turso/libSQL：内置 ANN，但是 SQLite 分支而非扩展
- sqlite-vec：社区最大（7.4k stars），LangChain 等已集成

**建议定位：** "SQLite 的高性能向量搜索扩展"——填补 HNSW ANN + 量化 + 移动端原生 + 真正开源的市场空白。

---

## 附录：产品对比速查表

| 产品 | 类型 | ANN 算法 | 量化 | 移动端 | 许可证 | Stars | 定价 |
|------|------|---------|------|--------|--------|-------|------|
| sqlite-vec | SQLite 扩展 | 无（暴力） | 无 | 理论支持 | MIT/Apache-2.0 | 7.4k | 免费 |
| sqlite-vector | SQLite 扩展 | 无（暴力） | 6种格式 | iOS/Android/Flutter | ELv2 | 822 | 商业许可 |
| Turso/libSQL | SQLite 分支 | DiskANN | 无 | iOS/Android SDK | MIT | - | $0-$416/月 |
| Vectorlite | SQLite 扩展 | HNSW | 无 | 不支持 | 未明确 | 357 | 免费 |
| Chroma | 独立数据库 | HNSW | 无 | 不支持 | Apache-2.0 | - | 按用量 |
| LanceDB | 独立数据库 | IVF-PQ 等 | 支持 | 有限 | Apache-2.0 | - | $0-$16/月起 |
| USearch | 搜索库 | HNSW | 支持 | Swift/Java | Apache-2.0 | - | 免费 |
| Zvec | 独立数据库 | Proxima | RabitQ | Android(新增) | Apache-2.0 | 9.3k | 免费 |

Sources:
- [sqlite-vec GitHub](https://github.com/asg017/sqlite-vec)
- [sqlite-vector GitHub](https://github.com/sqliteai/sqlite-vector)
- [SQLiteAI 官网](https://www.sqlite.ai)
- [SQLiteAI 定价](https://www.sqlite.ai/pricing)
- [Turso 向量搜索](https://turso.tech/vector)
- [Turso 定价](https://turso.tech/pricing)
- [Turso 移动端案例](https://turso.tech/blog/building-vector-search-and-personal-knowledge-graphs-on-mobile-with-libsql-and-react-native)
- [Vectorlite GitHub](https://github.com/1yefuwang1/vectorlite)
- [Chroma 官网](https://www.trychroma.com)
- [LanceDB 官网](https://lancedb.com)
- [USearch GitHub](https://github.com/unum-cloud/usearch)
- [Zvec GitHub](https://github.com/alibaba/zvec)
- [HN: Vector databases are the wrong abstraction](https://news.ycombinator.com/item?id=41985176)
- [HN: Ultra efficient vector extension for SQLite](https://news.ycombinator.com/item?id=45347619)
- [HN: sqlite-vec 讨论](https://news.ycombinator.com/item?id=40243168)
- [On-Device RAG 开发指南](https://medium.com/google-developer-experts/on-device-rag-for-app-developers-embeddings-vector-search-and-beyond-47127e954c24)
- [Zvec 介绍（MarkTechPost）](https://www.marktechpost.com/2026/02/10/alibaba-open-sources-zvec-an-embedded-vector-database-bringing-sqlite-like-simplicity-and-high-performance-on-device-rag-to-edge-applications/)
