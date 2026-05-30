# TurboQuant / PolarQuant / QJL 调研与测试报告

## 论文来源

- **TurboQuant**: ICLR 2026, Google Research
  - 论文: https://openreview.net/pdf/6593f484501e295cdbe7efcbc46d7f20fc7e741f.pdf
  - 博客: https://research.google/blog/turboquant-redefining-ai-efficiency-with-extreme-compression/
- **PolarQuant**: AISTATS 2026, arXiv:2502.02617
- **QJL**: AAAI 2025, arXiv:2406.03482

## 核心思想

### PolarQuant

将向量从笛卡尔坐标转换为极坐标（norm + angles），然后量化角度。

关键步骤：
1. **随机预处理**: 随机符号翻转，使坐标分布正态化
2. **递归极坐标变换**: 每层将相邻坐标对转为 (norm, angle)，log2(d) 层递归
3. **角度量化**: 对每层角度用 K-means 学习最优 codebook

优势：消除传统量化中的 per-block normalization 常数（通常占 1-2 bit 额外开销）

### QJL (Quantized Johnson-Lindenstrauss)

随机高斯投影后只保留符号位（1-bit），使用非对称估计器计算内积。

关键：E[sign(dot(r,x)) * dot(r,q)] 正比于 dot(x,q)，提供无偏估计。

## VexDB-Lite 测试结果

### 10K x 128d, Recall@10

| 方法 | Code Size | bits/dim | Recall@10 | Avg MSE | Encode (ms) | Search (ms) |
|------|-----------|----------|-----------|---------|-------------|-------------|
| **PQ** (m=32) | 32B | 2.0 | 0.525 | 0.0866 | 112.9 | **10.3** |
| **PolarQuant** (4-bit) | 68B | 4.2 | **0.846** | **0.0147** | **35.6** | 3233.3 |
| **PolarQuant** (2-bit) | 36B | 2.2 | 0.447 | 0.2203 | 31.6 | 3297.3 |
| **QJL** (m=128) | 16B | 1.0 | 0.071 | - | 92.6 | 6940.8 |
| **QJL** (m=256) | 32B | 2.0 | 0.140 | - | 185.7 | 14250.6 |

### 50K x 128d, Recall@10

| 方法 | Recall@10 | Search (ms) |
|------|-----------|-------------|
| **PQ** (m=32) | 0.440 | **24.0** |
| **PolarQuant** (4-bit) | **0.830** | 8008.7 |
| **PolarQuant** (2-bit) | 0.424 | 8049.7 |
| **QJL** (m=128) | 0.054 | 17442.4 |

## 分析

### PolarQuant 4-bit

- **召回率最高** (0.83-0.85)，远超 PQ (0.44-0.53)
- **重建误差最低** (MSE 0.0147 vs PQ 的 0.0866)
- **编码速度更快** (35ms vs 113ms)，因为不需要 K-means 查最近质心
- **搜索速度是瓶颈**: 当前实现是 decode-then-distance，比 PQ 的查表法慢 300x

### PolarQuant 2-bit

- 同等 bits/dim 下 (2.2 vs 2.0)，召回率与 PQ 接近
- 更极端的压缩（16x vs 4x），但精度损失明显

### QJL

- 在向量检索场景下表现不佳（L2 检索不是其设计目标）
- 原始设计是为 KV Cache 的 attention score（内积）优化的
- 1-bit 信息量太少，单独使用不适合 ANN 检索

## 优化方向

### PolarQuant 搜索加速（关键）

当前瓶颈：每次距离计算都需要完整 decode，O(d) 三角函数调用。

可能的优化路径：
1. **预计算距离表**: 类似 PQ 的 ADC (Asymmetric Distance Computation)，对 query 也做极坐标变换，利用角度差直接计算距离
2. **SIMD 批量 decode**: 批量处理三角函数
3. **混合方案**: PolarQuant 做粗筛（利用 norm 和顶层角度），PQ 或精确计算做精排

### TurboQuant 组合方案

论文的真正创新是 PolarQuant + QJL 的组合：
- PolarQuant 做主量化（3-4 bit）
- QJL 做 1-bit 残余误差校正
- 组合后达到 ~3 bit 总开销，精度接近无损

这个组合在当前实现中尚未测试，是下一步工作。

## SIFT-128 真实数据测试（2026-03-26 更新）

### SIFT 10K (10000 x 128d, 200 queries)

| 方法 | B/vec | R@1 | R@10 | R@100 | QPS | Mem(MB) |
|------|-------|-----|------|-------|-----|---------|
| PQ (m=32, dsub=4) | 32 | **0.745** | **0.821** | **0.870** | 7815 | **0.43** |
| PQ (m=16, dsub=8) | 16 | 0.595 | 0.728 | 0.800 | **12305** | **0.28** |
| PolarQuant 4-bit (batch+SIMD) | 68 | **0.765** | 0.815 | 0.869 | 7727 | 5.53 |
| PolarQuant 2-bit (batch+SIMD) | 36 | 0.400 | 0.468 | 0.599 | 7753 | 5.23 |
| PolarQuant 4-bit (fast_decode, no cache) | 68 | 0.765 | 0.815 | 0.869 | 512 | **0.65** |

### SIFT 100K (100000 x 128d, 200 queries)

| 方法 | B/vec | R@1 | R@10 | R@100 | QPS | Mem(MB) |
|------|-------|-----|------|-------|-----|---------|
| PQ (m=32, dsub=4) | 32 | **0.730** | **0.755** | **0.817** | **1014** | **3.18** |
| PQ (m=16, dsub=8) | 16 | 0.570 | 0.637 | 0.715 | **1904** | **1.65** |
| PolarQuant 4-bit (batch+SIMD) | 68 | 0.665 | 0.746 | 0.808 | 867 | 55.31 |
| PolarQuant 2-bit (batch+SIMD) | 36 | 0.280 | 0.365 | 0.458 | 891 | 52.26 |

### 分析

1. **在真实 SIFT 数据上，PQ 32 略优于 PolarQuant 4-bit**（R@10: 0.755 vs 0.746），与随机数据测试结论相反
2. **原因**：SIFT 数据有明显的聚类结构，PQ 的 K-means codebook 能很好地捕获这种结构；PolarQuant 的随机预处理反而打散了这种结构
3. **PolarQuant 2-bit 在真实数据上表现很差**（R@10: 0.365），角度分辨率不足以捕获 SIFT 的分布特征
4. **内存是 PolarQuant batch 模式的致命问题**：100K 时需要 55MB（包含 48MB 解码缓存），而 PQ 只要 3MB
5. **PolarQuant 无缓存模式 QPS 太低**（52 QPS vs PQ 1014 QPS），差 20 倍
6. **编码速度 PolarQuant 更快**（278K vec/s vs 88K vec/s），因为不需要 K-means 最近邻查找

### 关键结论

PolarQuant 在**向量检索**场景中**不优于 PQ**，原因：
- 论文设计目标是 LLM KV Cache 量化（均匀分布、关注内积），不是 ANN 检索（聚类分布、关注 L2）
- PQ 的子空间 K-means 天然适配聚类数据；PolarQuant 的极坐标变换假设角度均匀分布
- batch_decode 模式虽快但内存开销等于存原始数据，失去了压缩意义

## 结论

PolarQuant 在**向量检索**场景中不适合替代 PQ。它的真正优势在论文原始目标——LLM KV Cache 量化，那里数据分布更均匀、以内积为主、且压缩比要求极高（3-bit）。

对于 VexDB-Lite 的端侧向量索引，**PQ 仍然是最佳选择**。

## 文件

- 实现: `duckdb/extension/vex/quantizer/polar_quantizer.cpp`
- 实现: `duckdb/extension/vex/quantizer/qjl_quantizer.cpp`
- 头文件: `duckdb/extension/vex/include/vex_quantizer.hpp`
- 测试: `duckdb/extension/vex/test/benchmark/quantizer_benchmark.cpp`
- 分支: `feature/polar-quant`
