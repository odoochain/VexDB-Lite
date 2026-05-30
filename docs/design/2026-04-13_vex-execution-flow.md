# VEX 扩展整体执行逻辑

> 生成日期：2026-04-13

---

## 1. 扩展注册流程

```
DUCKDB_CPP_EXTENSION_ENTRY(vex)
  └─ LoadInternal()
      ├─ VexFunctions::Register()          // 注册距离函数、ANN函数等
      ├─ RegisterIndexTypes()              // 注册 GRAPH_INDEX 类型
      │   └─ create_instance = GraphIndex::Create
      ├─ VexOptimizerExtension()           // 注册查询优化器钩子
      └─ AddExtensionOption()              // vex_ef_search, vex_brute_force_threshold 等
```

---

## 2. 索引创建调用链

```
CREATE INDEX ... USING GRAPH_INDEX ON t(vec_col)
  └─ GraphIndex::Create(CreateIndexInput)
      ├─ 验证参数：列类型=FLOAT[N], M∈[2,128], ef_construction∈[1,10000]
      ├─ 初始化距离函数：distance_func_ = GetDistanceFunc(metric)
      │   └─ GetBestArch() 运行时CPU检测 → AVX512/AVX2/SSE/NEON/Generic
      └─ 构造 GraphIndexCore（延迟初始化 allocator，首次插入时检测维度）
```

---

## 3. 数据插入调用链

### 单线程（< 10K 行）

```
GraphIndex::Build(DataChunk, row_ids)
  ├─ 首次调用：初始化 4 个 FixedSizeAllocator
  └─ 对每行：
      ├─ [Cosine] NormalizeVector(vec)
      ├─ [去重] TryDedup() → 找精确匹配 → 共享节点，skip insert
      ├─ AllocateNode(row_id, vec, level)
      │   ├─ Allocator0: HNSWNodeHeader（节点头 + Level-0邻接）
      │   ├─ Allocator1: float[dim]（向量数据）
      │   ├─ Allocator2: 上层邻接（level>0）
      │   └─ Allocator3: 元数据（过滤列）
      └─ InsertNode(node_ptr)
          ├─ 上层遍历：level=max→node_level+1，贪心搜 1 个最近邻（更新 ep）
          └─ 插入层：level=node_level→0
              ├─ SearchLayer(ep, ef_construction, level)  // 找 ef 个候选
              ├─ SelectNeighbors(candidates, M)           // 启发式多样性剪枝
              ├─ new_node.neighbors[level] = selected
              └─ 双向连接：更新 selected 节点的邻接（若满则重新剪枝）
```

### 并行构建（> 10K 行）

```
GraphIndex::BuildParallel(...)
  ├─ [单线程] 预分配所有节点（AllocateNode × N）
  ├─ BuildBufferCaches()   // 预pin全部缓冲，缓存 base_ptr，消除并行阶段锁开销
  ├─ InitGraphMutex()      // 全局RWLock + 1024个SpinLock条纹
  ├─ [多线程] parallel InsertNodeParallel(node_ptr)
  │   ├─ Phase1: Lock-free SearchLayer（读，无锁，依赖BufferPtrCache）
  │   ├─ Phase2: 去重检查（SpinLock + 早退）
  │   └─ Phase3: 细粒度连接
  │       ├─ 距离计算在锁外（并行化！）
  │       └─ 写邻接时用 SpinLock 条纹锁（hash(node_ptr) % 1024）
  └─ ClearBufferCaches()
```

---

## 4. ANN 查询调用链

### 查询优化器介入

```
SELECT * FROM t ORDER BY l2_distance(vec, ?) LIMIT k
  └─ VexOptimizerExtension::OptimizeNode()
      └─ TryOptimizeLimitOrderBy()
          ├─ 模式匹配：Limit(k) → OrderBy(distance_func(col, query))
          ├─ 确认该列存在 GRAPH_INDEX
          └─ 替换为 PhysicalVexIndexScan（或直接执行常量向量查询）
```

### 运行时执行

```
PhysicalVexIndexScan::Execute()
  ├─ 评估 query_vec_expr → float[]
  ├─ ANNSearch(graph, query_vec, k, ef_search)
  │   ├─ 上层贪心：level=max→1，SearchLayer(ef=1) → 更新 ep
  │   └─ Level-0：SearchLayer(ep, ef_search, level=0)
  │       ├─ 优先队列 candidates（最大堆，按距离）
  │       ├─ visited bitset（避免重复）
  │       └─ 展开邻接，过滤 visited，计算距离，更新 candidates
  ├─ 排序取 top-k（剔除 deleted 节点）
  ├─ [去重] 展开 dedup_map[node_ptr] 里的额外 row_ids
  └─ FetchRows(row_ids) → 输出结果
```

---

## 5. 距离计算 SIMD Dispatch

```
GetDistanceFunc(metric)
  └─ GetBestArch()（运行时 CPU 检测，调用一次缓存结果）
      ├─ AVX-512 → 每周期 16 浮点
      ├─ AVX2    → 每周期  8 浮点（4个并行累加器消除延迟链）
      ├─ SSE     → 每周期  4 浮点
      ├─ NEON    → ARM，每周期  4 浮点
      └─ Generic → 标量

支持度量：L2（欧几里得）、Cosine（余弦，插入时归一化）、IP（内积，取负）
```

---

## 6. 产品量化（PQ）

```
训练：ProductQuantizer::Train(vectors)
  └─ 每个子量化器（M个）：对子向量做 K-means(k=256)，存 256 个质心

编码：Encode(vec) → M 字节
  └─ 每个子向量 → 找最近质心 → 输出索引（0-255）

查询加速：
  ├─ ComputeDistanceTable(query) → 预计算 M×256 个距离
  └─ DistanceFromTable(code)     → M 次查表相加（代替 D 次 FMA）
     加速比 ≈ D/M，例如 D=768, M=96 → 8x 加速
```

---

## 7. 线程安全层次

| 层次 | 机制 | 保护目标 |
|------|------|----------|
| 图级 | `SimpleRWLock`（原子CAS或condvar） | 搜索(共享)/插入(独占) |
| 节点级 | `SpinLock` 条纹（1024条） | 邻接表写操作 |
| 缓冲级 | `FixedSizeAllocator` 内置 mutex | New/Get/Free 操作 |
| 并行优化 | `BufferPtrCache` | 预pin缓冲，消除并行阶段锁 |

---

## 8. 内存布局（节点存储）

```
Allocator0: HNSWNodeHeader (40B) + Level-0邻接数组
  [row_id | level | deleted | counts | vector_ptr→A1 | upper_ptr→A2 | meta_ptr→A3]
  [neighbor0 | neighbor1 | ... | neighbor_{2M-1}]  ← 直接内联，避免指针跳转

Allocator1: float[dim]（向量数据）
Allocator2: 上层邻接（level1~levelMax，共享一块内存）
Allocator3: 元数据（可选，用于过滤谓词）
```

---

## 9. 整体数据流

```
INSERT → Build/BuildParallel
           ↓
     AllocateNode (4个Allocator)
           ↓
     InsertNode → SearchLayer → SelectNeighbors → 双向连接
           ↓
     持久化到 WAL / 序列化

SELECT ... ORDER BY distance LIMIT k
           ↓
     VexOptimizer 识别模式
           ↓
     PhysicalVexIndexScan
           ↓
     ANNSearch → SearchLayer(ef_search) → top-k 候选
           ↓
     FetchRows → 输出
```

---

## 关键源文件

| 文件 | 职责 |
|------|------|
| `vex_extension.cpp` | 扩展入口、注册所有函数和索引类型 |
| `include/vex_graph_index.hpp` | 索引公共 API |
| `include/vex_graph_index_core.hpp` | 核心图算法 + 存储结构 |
| `include/vex_hnsw_node.hpp` | 节点内存布局定义 |
| `distance/distance.cpp` | SIMD 距离计算 dispatch |
| `index/graph_index_core.cpp` | HNSW 算法详细实现（~1500行） |
| `index/graph_index.cpp` | DuckDB BoundIndex 包装、WAL 集成 |
| `optimizer/vex_optimizer.cpp` | 查询重写：ORDER BY distance LIMIT k |
| `quantizer/product_quantizer.cpp` | PQ 训练、编码、距离表 |
