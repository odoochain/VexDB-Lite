# VexDB-Lite 移动端适配 Phase 1 完成总结

> 日期: 2026-03-19 | 分支: upgrade/v1.5.0 | 状态: Code Review 通过

## 完成内容

### 1. CMake 工具链文件（新增 3 个文件）

| 文件 | 用途 |
|------|------|
| `duckdb/scripts/ios-toolchain.cmake` | iOS 交叉编译（arm64 设备 / x86_64 & arm64 模拟器） |
| `duckdb/scripts/android-toolchain.cmake` | Android NDK 编译封装（arm64-v8a / armeabi-v7a / x86_64） |
| `duckdb/extension/extension_config_mobile.cmake` | 移动端最小扩展配置（仅 core_functions + vex） |

### 2. build.sh 移动端构建目标（修改 1 个文件）

新增命令：
- `bash build.sh ios` — iOS arm64 设备编译
- `bash build.sh ios --sim` — iOS 模拟器编译（x86_64 + arm64 通用二进制）
- `bash build.sh android` — Android arm64-v8a 编译
- `bash build.sh android --abi armeabi-v7a` — 指定 ABI
- `bash build.sh wasm` — WebAssembly 编译（需 Emscripten）

所有移动构建自动启用：
- `-Os` 体积优化
- `VEX_MOBILE_MODE=ON`
- `EXTENSION_STATIC_BUILD=ON`
- 禁用 shell / unittests / 动态加载 / parquet / jemalloc

### 3. VEX 编译时特性开关（修改 CMakeLists.txt）

| 选项 | 默认 | 说明 |
|------|------|------|
| `VEX_ENABLE_PQ` | ON | 产品量化支持 |
| `VEX_ENABLE_HYBRID_INDEX` | ON | 分区索引支持 |
| `VEX_ENABLE_OPTIMIZER` | ON | 查询优化器改写 |
| `VEX_MOBILE_MODE` | OFF | 移动端友好模式 |

移动端可通过 `-DVEX_ENABLE_PQ=OFF` 等进一步裁剪二进制。

### 4. 移动端自旋锁降级（修改 vex_graph_index_core.hpp）

`VEX_MOBILE_MODE` 启用时：
- `SimpleRWLock` → `std::mutex` + `std::condition_variable`（不自旋，节省电池）
- `SpinLock` → `std::mutex`（条带锁同理）
- 接口完全兼容，`SharedLockGuard`/`SpinLockGuard` 无需修改

### 5. SIMD 架构正确报告（修改 vex_distance.hpp + distance.cpp）

- `SimdArch` 枚举新增 `NEON = 4` 和 `WASM_SIMD = 5`
- `GetBestArch()` 在 ARM 上返回 `NEON`，WASM 上返回 `WASM_SIMD`（之前均返回 `GENERIC`）
- `ArchName()` 正确输出 "NEON" / "WASM-SIMD"
- `vex_index_info()` 现在能在移动端准确显示 SIMD 架构

### 6. 运行时配置（修改 vex_extension.cpp）

新增 `vex_parallel_threshold` 参数：
- 桌面端默认 10000
- 移动端（VEX_MOBILE_MODE）默认 1000

### 7. 测试用例补充（新增 6 个测试文件）

| 测试文件 | 覆盖场景 |
|----------|---------|
| `graph_index_small_dataset.test` | 1/2/5/10 行极小数据集、k > 表大小、暴力搜索阈值边界 |
| `graph_index_lowdim.test` | dim=2/4/8/16/32 低维向量、三种度量、PQ 量化 |
| `graph_index_config_advanced.test` | ef_search 极值、brute_force_threshold=0/极大值、低 M/ef |
| `graph_index_minimal_params.test` | m=2/4 最小参数、ef_search=1、所有度量类型 |
| `index_info.test` | vex_index_info() 函数完整测试 |
| `distance_edge_cases.test` | 零向量、自距离、极大/极小值、正交向量、单维度 |

### 测试结果

```
All tests passed (4393 assertions in 92 test cases)
```

- 之前：86 个测试，4147 个断言
- 现在：92 个测试，4393 个断言
- 新增：6 个测试，246 个断言

## 变更文件清单

### 新增文件（9 个）
- `duckdb/scripts/ios-toolchain.cmake`
- `duckdb/scripts/android-toolchain.cmake`
- `duckdb/extension/extension_config_mobile.cmake`
- `duckdb/test/sql/vex/functions/distance_edge_cases.test`
- `duckdb/test/sql/vex/functions/index_info.test`
- `duckdb/test/sql/vex/index/graph_index_config_advanced.test`
- `duckdb/test/sql/vex/index/graph_index_lowdim.test`
- `duckdb/test/sql/vex/index/graph_index_minimal_params.test`
- `duckdb/test/sql/vex/index/graph_index_small_dataset.test`

### 修改文件（6 个）
- `build.sh` — 新增 ios/android/wasm 命令
- `duckdb/extension/vex/CMakeLists.txt` — 特性开关
- `duckdb/extension/vex/distance/distance.cpp` — SIMD 架构报告
- `duckdb/extension/vex/include/vex_distance.hpp` — SimdArch 枚举扩展
- `duckdb/extension/vex/include/vex_graph_index_core.hpp` — 移动端锁降级
- `duckdb/extension/vex/vex_extension.cpp` — 并行阈值配置

## Code Review 修复记录

Review 发现并修复了以下问题：

### Critical 修复
1. **build.sh 路径和变量名错误** — `MOBILE_EXT_CONFIG` 路径修正，`EXTENSION_CONFIG_FILE` → `DUCKDB_EXTENSION_CONFIGS`，移除不存在的 `EXTERNAL_EXTENSION_DIRECTORIES`
2. **build.sh 未使用工具链文件** — ios/android 命令改为使用 `-DCMAKE_TOOLCHAIN_FILE` 引用已创建的工具链
3. **条件编译缺失** — `vex_extension.cpp` 中 HybridIndex/Optimizer 引用加上 `#ifdef` 守卫，防止 `VEX_ENABLE_HYBRID_INDEX=OFF` 时链接失败
4. **Mobile RWLock 写者饥饿** — 添加 `writer_waiting_` 标志，防止持续读者永久阻塞写者

### Warning 修复
5. **android-toolchain.cmake** — 移除被 NDK 覆盖的 `CMAKE_SYSTEM_NAME/VERSION`，`add_compile_definitions` → `CMAKE_CXX_FLAGS`
6. **ios-toolchain.cmake** — 移除无意义的 `CMAKE_SYSTEM_VERSION=1` 和已废弃的 bitcode 支持
7. **optimizer 条件编译** — `optimizer/vex_optimizer.cpp` 移入 `VEX_ENABLE_OPTIMIZER` 条件块
8. **重复测试删除** — 删除 `graph_index_config_advanced.test`（与现有测试高度重复）

### 仍存在的已知问题（Warning 级别，后续处理）
- `vex_parallel_threshold` 注册了但 `graph_index.cpp` 中未读取（需在 Phase 2 集成）
- `SimdArch` 枚举算术比较依赖数值顺序（当前被 `#ifdef` 保护，不影响运行）

## 最终测试结果

```
All tests passed (4347 assertions in 91 test cases)
```

## 后续 Phase 2 建议

1. 内存预算参数 `vex_max_index_memory`
2. Int8 标量量化
3. 并行阈值与 `vex_parallel_threshold` 集成到 graph_index.cpp（替代硬编码 PARALLEL_THRESHOLD）
4. 实际在 iOS/Android 设备上验证编译和运行
5. `SimdArch` 比较改为显式 `==` 检查
