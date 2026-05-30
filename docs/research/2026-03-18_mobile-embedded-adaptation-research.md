# VexDB-Lite 移动端/嵌入式平台适配调研报告

> 日期: 2026-03-18 | 分支: upgrade/v1.5.0

---

## 一、核心结论

**VexDB-Lite 有机会成为"移动端唯一同时提供完整 SQL 查询和高性能 ANN 向量搜索的嵌入式数据库"**——这个定位目前没有直接竞争对手。

2026 年移动端 RAG 爆发（Llama 3.2 3B 可在手机运行、Google EmbeddingGemma 专为设备端设计），行业从纯向量数据库回归"关系数据库 + 向量扩展"，Apple 尚未提供开放向量搜索框架——这些都是窗口期。

---

## 二、DuckDB 移动端支持现状

| 平台 | 官方支持 | 工具/绑定 | 状态 |
|------|---------|----------|------|
| **iOS** | ✅ 完整 | DuckDB Swift (duckdb-swift) | 生产就绪 |
| **Android** | ⚠️ 实验性 | Android NDK / dart_duckdb | 建议用 main 分支 |
| **WebAssembly** | ✅ 完整 | duckdb-wasm (Emscripten) | 生产就绪 |
| **Flutter** | ✅ 完整 | dart_duckdb (iOS/Android/Web) | 生产就绪 |
| **ARM64 Linux** | ✅ 完整 | GCC 交叉编译 | 预编译二进制可用 |
| **树莓派** | ⚠️ 部分 | 自带工具链文件 | 源码编译 |

### 关键发现
- DuckDB 已在 **iPhone 16 Pro** 和 **Samsung Galaxy S24 Ultra** 上成功运行 TPC-H 基准测试
- Swift API 原生支持 iOS，代码库中有完整的 Swift 包 (`tools/swift/duckdb-swift/`)
- WASM 支持三种构建模式：`wasm_mvp`、`wasm_eh`、`wasm_threads`
- 已有 Raspberry Pi 交叉编译工具链 (`scripts/raspberry-pi-cmake-toolchain.cmake`)

---

## 三、VEX 扩展适配分析

### 3.1 SIMD 距离计算（已有良好基础）

| 架构 | 支持情况 | 备注 |
|------|---------|------|
| SSE | ✅ | x86 基础 |
| AVX2 | ✅ | x86 高性能 |
| AVX-512 | ✅ | 服务器级 |
| **ARM NEON** | ✅ | **移动端核心**，aarch64 |
| **WASM SIMD128** | ✅ | 需 `-msimd128` 编译标志 |
| 标量回退 | ✅ | GENERIC 实现 |

运行时调度机制：静态初始化函数指针，零运行时开销。**移动端 NEON 已可直接使用。**

### 3.2 内存占用估算（M=16, dim=768）

| 组件 | 单节点 |
|------|--------|
| 节点头 + 邻接 | ~288B |
| 向量数据 | 3,072B |
| 上层邻接 | ~1,040B |
| **合计** | **~4.4KB/节点** |

- 10K 向量 ≈ 44MB（仅索引）
- 100K 向量 ≈ 440MB（需 PQ 量化或降维）

### 3.3 需要适配的关键问题

| 问题 | 严重性 | 影响 | 建议 |
|------|--------|------|------|
| 无内存预算 API | **高** | 无法限制索引大小 | 增加 `max_memory_bytes` 参数 |
| ARM NEON 仅支持 aarch64 | **高** | 不支持 ARMv7 | 增加条件编译 |
| 自旋锁浪费电池 | **中** | 功耗问题 | 移动平台降级为互斥锁 |
| 无 mmap 支持 | **高** | 大索引需全量加载 | 实现内存映射访问 |
| 无编译时特性开关 | **中** | 二进制膨胀 | 添加 `VEX_ENABLE_*` CMake 选项 |
| 并行阈值硬编码 10K | **低** | 小数据集开销 | 可配置化 |

---

## 四、竞品分析

| 方案 | 类型 | 移动支持 | ANN 索引 | SQL | 持久化 | 量化 | 优劣势 |
|------|------|---------|----------|-----|--------|------|--------|
| **sqlite-vec** | SQLite 扩展 | ✅ iOS/Android | ❌ 仅暴力 | ✅ | ✅ | ❌ | 轻量但无 ANN |
| **FAISS-Mobile** | 独立库 | ⚠️ 仅 iOS | ✅ IVF/HNSW | ❌ | ❌ | ✅ PQ/SQ | 算法丰富，体积大 |
| **Hnswlib** | Header-only | ✅ | ✅ HNSW | ❌ | ❌ | ❌ | 极简但无持久化 |
| **USearch** | 独立库 | ✅ 多语言 | ✅ HNSW | ❌ | ✅ mmap | ✅ f16/i8 | 当前最强轻量级方案 |
| **Zvec** (阿里) | 嵌入式 DB | ⚠️ 仅 Python | ✅ | ❌ | ✅ | ✅ | >8000 QPS，但绑定少 |
| **VexDB-Lite** | DuckDB 扩展 | ⚠️ 待适配 | ✅ HNSW | ✅ 完整 SQL | ✅ WAL | ✅ PQ | **唯一 SQL+ANN 一体化** |

### VexDB-Lite 的独特优势
1. **SQL + 向量搜索一体化** — 竞品要么只有向量搜索，要么只有 SQL
2. **HybridIndex 分区索引** — 独特功能，支持按标量列分区的向量检索
3. **已有 PQ 量化** — 可大幅减少移动端内存占用
4. **DuckDB 生态** — 自带 Parquet/CSV/JSON 读写、分析函数

---

## 五、构建系统适配方案

### 5.1 静态链接（推荐）

VEX 已支持静态编译：
```cmake
build_static_extension(vex ${VEX_EXTENSION_FILES})
```

最小化移动编译命令：
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DDISABLE_BUILTIN_EXTENSIONS=1 \
      -DEXTENSION_STATIC_BUILD=1 \
      -DCORE_EXTENSIONS="core_functions;vex" \
      -DBUILD_SHELL=0 \
      -DBUILD_UNITTESTS=0 \
      -DDISABLE_EXTENSION_LOAD=1 \
      -DCMAKE_CXX_FLAGS_RELEASE="-Os -ffunction-sections -fdata-sections" \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,--gc-sections"
```

### 5.2 各平台构建路径

**iOS:**
```bash
# 通过 DuckDB Swift 包集成，VEX 静态链接
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/ios-toolchain.cmake \
      -DIOS_PLATFORM=OS -DIOS_ARCH=arm64 \
      # ... 同上最小化选项
```

**Android:**
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_PLATFORM=android-21 \
      -DANDROID_ABI=arm64-v8a \
      # ... 同上最小化选项
```

**WASM:**
```bash
emcmake cmake -DWASM_LOADABLE_EXTENSIONS=1 \
              -DUSE_WASM_THREADS=1 \
              -DCMAKE_CXX_FLAGS="-fwasm-exceptions -DWITH_WASM_SIMD=1"
```

### 5.3 二进制大小预估

| 配置 | 预估大小 |
|------|---------|
| 默认编译（全扩展） | 200-500 MB |
| 核心 + VEX（静态） | 50-100 MB |
| 核心 + VEX（-Os + gc-sections） | 20-50 MB |
| WASM MVP | 5-15 MB |
| WASM with threads | 10-25 MB |

---

## 六、推荐实施路线图

### Phase 1: 基础适配（2-3 周）
1. **创建 iOS/Android CMake 工具链文件** — 参考已有的 raspberry-pi 工具链
2. **验证 ARM NEON 在移动端的编译和运行** — 在真机上跑基础向量搜索
3. **最小化编译配置** — 裁剪不需要的扩展，目标 < 30MB
4. **编写 build.sh 的 mobile 目标** — `bash build.sh ios` / `bash build.sh android`

### Phase 2: 性能优化（2-3 周）
5. **添加内存预算参数** — `SET vex_max_index_memory = '256MB'`
6. **自旋锁降级** — 移动平台检测，自动使用 mutex
7. **Int8 标量量化** — 补充 PQ 之外的更轻量化量化选项
8. **并行阈值可配置** — 移动端默认更小的阈值

### Phase 3: 生态集成（3-4 周）
9. **Swift 封装** — 提供 iOS SDK，集成 DuckDB Swift
10. **Kotlin/JNI 封装** — Android SDK
11. **Flutter 插件** — 基于 dart_duckdb 扩展
12. **WASM 发布** — npm 包，浏览器端向量搜索

### Phase 4: 进阶能力（按需）
13. **mmap 索引加载** — 减少启动时间和内存占用
14. **增量索引更新** — 移动端场景常见的小批量更新
15. **模型集成示例** — 端侧 embedding + VexDB 的完整 RAG demo

---

## 七、风险与注意事项

1. **DuckDB 本身体积较大** — 即使裁剪后仍可能 20MB+，需评估是否可接受
2. **Android 官方支持仍为实验性** — 可能遇到未知兼容性问题
3. **v1.5.0 升级进行中** — 移动适配工作建议在升级完成后开展
4. **Apple App Store 审核** — 需确保静态链接不违反审核规则
5. **竞品 USearch 非常轻量** — 如果用户只需向量搜索不需 SQL，USearch 是更简单的选择

---

## 附录：关键文件路径

- 主构建配置: `duckdb/CMakeLists.txt`
- VEX 扩展配置: `duckdb/extension/vex/CMakeLists.txt`
- SIMD 距离计算: `duckdb/extension/vex/distance/distance.cpp`
- 索引核心: `duckdb/extension/vex/index/graph_index_core.cpp`
- 扩展编译框架: `duckdb/extension/extension_build_tools.cmake`
- RPI 工具链: `duckdb/scripts/raspberry-pi-cmake-toolchain.cmake`
- Swift 包: `duckdb/tools/swift/duckdb-swift/`
- Wheel 构建: `packaging/build_wheel.sh`
