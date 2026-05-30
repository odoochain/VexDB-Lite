# DuckDB 二进制体积缩减调研报告

> 日期: 2026-03-19 | 目标: 为 VexDB-Lite 移动端/嵌入式部署收集体积优化方案

---

## 一、DuckDB 当前二进制体积基线

| 构建产物 | 平台 | 大小 | 备注 |
|---------|------|------|------|
| libduckdb.dylib (release) | macOS arm64 | **45 MB** | 本地 v1.4.4 fork, 含 vex 扩展 |
| libduckdb_static.a | macOS arm64 | **59 MB** | 静态库 |
| unittest 二进制 | macOS arm64 | **51 MB** | 含测试框架 |
| DuckDB-WASM 核心 | Web | **~17 MB** (未压缩) / **~3.2 MB** (gzip) | 含内置扩展 |
| Go 静态链接 | 通用 | **~30 MB** | go-duckdb 报告 |
| sql.js (SQLite WASM) 对比 | Web | **599 KB** | 仅供参照 |

**结论**: DuckDB 完整构建在 30-50 MB 量级, 对移动端来说偏大, 需要积极优化。

---

## 二、已知的官方优化措施

### 2.1 DuckDB 1.5.0 扩展体积优化 (2026-03)

DuckDB 1.5.0 对构建系统进行了重大重构，显著缩减扩展二进制体积：

| 扩展 | 优化前 | 优化后 | 缩减比例 |
|------|-------|-------|---------|
| DuckLake | 17 MB | 12 MB | **~30%** |
| Excel | 9 MB | 3 MB | **>60%** |

技术原理：重构构建系统中扩展链接方式，减少扩展二进制中重复包含的 DuckDB 核心符号。

### 2.2 DuckDB-WASM 体积优化流水线

WASM 构建采用多阶段优化，是目前 DuckDB 最成熟的体积优化实践：

1. **`relsize` 构建模式**: `-DCMAKE_BUILD_TYPE=Release -DWASM_MIN_SIZE=1`，使用 `-Os` 优化级别
2. **最小导出列表生成**:
   - 第一次完整构建，导出所有符号
   - 用 `wasm2wat` 反汇编，`grep` 提取实际使用的导出符号
   - 生成 `exported_list.txt` 仅包含必要的 C/C++ 符号
   - 用 `-DUSE_GENERATED_EXPORTED_LIST=1` 重新构建
   - 过滤掉内部 Unwind 函数、syscall 桩、Arrow 模板实例化等
3. **扩展延迟加载**: 使用 `DONT_LINK` 标志，核心扩展 (JSON/Parquet/ICU) 不静态链接，运行时按需加载
4. **JS 胶水代码精简**: 用 awk 过滤不必要的变量声明，仅保留必要导出

### 2.3 extension_config_mobile.cmake (本项目已有)

本项目已创建移动端扩展配置，仅包含 `core_functions` 和 `vex`，跳过：
- parquet (~大依赖)
- icu (~10 MB Unicode 排序)
- httpfs, json, fts, tpch, tpcds
- jemalloc (iOS 不兼容)

---

## 三、可用的编译器/链接器优化技术

### 3.1 DuckDB CMakeLists.txt 已支持的标志

| 技术 | 当前状态 | CMake 变量 | 效果 |
|------|---------|-----------|------|
| `-ffunction-sections -fdata-sections` | 已启用 (GCC, EXTENSION_STATIC_BUILD) | 内置 | 配合 `--gc-sections` 移除未使用函数 |
| LTO (链接时优化) | 可选启用 | `-DCMAKE_LTO=thin` 或 `full` | Clang 支持 thin/full, GCC 支持 full |
| Release `-O3` | 默认 | 内置 | 性能优先，非体积优先 |

### 3.2 可额外应用的优化 (当前未启用)

| 技术 | 命令/标志 | 预期效果 | 风险 |
|------|----------|---------|------|
| **`-Os` 替代 `-O3`** | `CMAKE_CXX_FLAGS_RELEASE` 中替换 | 体积减少 10-20%，性能略降 | 向量距离计算可能变慢 |
| **`-Oz` (Clang)** | 同上 | 比 `-Os` 更激进，体积再减 5-10% | 性能影响更大 |
| **LTO thin** | `-DCMAKE_LTO=thin` | 跨编译单元死代码消除，减少 10-30% | 编译时间增加 |
| **`strip -x`** | 构建后执行 | 移除本地符号，减少 5-15% | 注意：DuckDB 扩展有签名机制，strip 会破坏扩展加载 |
| **`-fvisibility=hidden`** | 添加到 CXX_FLAGS | 隐藏非必要符号，配合链接器优化 | 需确保 API 正确导出 |
| **`-fno-rtti`** | 添加到 CXX_FLAGS | 减少 RTTI 元数据 | DuckDB 使用 dynamic_cast，需验证 |
| **`-fno-exceptions`** | 添加到 CXX_FLAGS | 移除异常处理表 | DuckDB 内部使用 try/catch，**不可行** |
| **`--gc-sections` (链接器)** | `-Wl,--gc-sections` (Linux) / `-Wl,-dead_strip` (macOS) | 移除未引用的 section | 配合 `-ffunction-sections` 使用 |
| **`-Wl,-x`** | 链接器标志 | 丢弃本地符号 | 低风险 |

### 3.3 平台特定优化

**iOS (Clang/Apple Linker)**:
- `-dead_strip` 默认启用
- Bitcode 已废弃 (Xcode 14+)，不需要 `-fembed-bitcode`
- `-flto=thin` 与 Xcode 良好集成

**Android (NDK Clang)**:
- `-ffunction-sections -fdata-sections -Wl,--gc-sections` 组合
- NDK r28+ 默认支持 ThinLTO
- `-DANDROID_STL=c++_static` 静态链接 libc++

---

## 四、架构层面的体积优化策略

### 4.1 Amalgamation (合并编译)

DuckDB 官方发布采用 amalgamation 模式——将整个源码树合并为一个 `.cpp` + 一个 `.hpp`。

**优点**: 编译器在单一编译单元内有更好的内联和死代码消除能力
**当前状态**: VexDB-Lite 未使用 amalgamation 模式
**建议**: 移动端发布版本考虑使用 amalgamation

### 4.2 选择性禁用 DuckDB 子系统

可通过条件编译移除不需要的功能模块：

| 模块 | 估计体积 | 移动端是否需要 |
|------|---------|--------------|
| SQL Parser | ~3-5 MB | 需要 |
| Optimizer | ~5-8 MB | 需要 (ANN 重写规则) |
| 存储引擎 | ~5-8 MB | 需要 |
| Catalog/Schema | ~2-3 MB | 需要 |
| Arrow IPC | ~2-3 MB | **可移除** |
| CSV Reader/Writer | ~1-2 MB | 可选 |
| Aggregate Functions (完整集) | ~3-5 MB | 可精简 |
| Window Functions | ~2-3 MB | 可选 |
| 正则表达式引擎 (RE2) | ~1-2 MB | 可选 |

### 4.3 DONT_LINK + 运行时加载

参照 WASM 模式，核心 DuckDB 只包含最小功能，扩展按需加载：
```cmake
duckdb_extension_load(parquet DONT_LINK)  # 编译但不链接
duckdb_extension_load(json DONT_LINK)
```
移动端仅静态链接 `core_functions` + `vex`，其他扩展以 `.duckdb_extension` 形式按需下载。

---

## 五、社区实践与讨论

### 5.1 WASM 社区反馈 (GitHub Discussion #1469)

- 默认 WASM 二进制 **~17 MB**，社区认为过大
- Cloudflare Workers 有 **25 MB** 资产限制，接近上限
- 社区建议：对于单一功能 (如仅 Parquet) 使用专用小库 (parquet-wasm) 而非完整 DuckDB
- **尚无官方"特性标志"构建系统**可让用户自选 DuckDB 核心模块

### 5.2 strip 命令与 DuckDB 扩展 (GitHub Issue #15975)

- `strip` 命令会破坏 DuckDB 扩展的签名验证
- 扩展文件尾部有元数据，strip 会改变文件布局
- **对核心二进制 (libduckdb.so/dylib) strip 是安全的**，只对 `.duckdb_extension` 文件有风险

### 5.3 边缘计算 / 树莓派部署

- MotherDuck 博客展示了在 Raspberry Pi 5 上运行 DuckDB
- 采用混合模式：本地 DuckDB 实例 + 云端 MotherDuck 查询
- 未提供具体的体积优化细节

---

## 六、量化估算：VexDB-Lite 移动端目标体积

基于当前 45 MB (libduckdb.dylib with vex) 基线的优化路径：

| 优化步骤 | 预期缩减 | 累计体积 |
|---------|---------|---------|
| 基线 (当前 release build) | - | ~45 MB |
| 移除非必要扩展 (已做: mobile config) | -10~15 MB | ~30-35 MB |
| `-Os` 替代 `-O3` | -3~7 MB (10-20%) | ~25-30 MB |
| LTO thin | -3~8 MB (10-25%) | ~20-25 MB |
| `strip -x` (仅核心库) | -1~3 MB | ~18-23 MB |
| `-fvisibility=hidden` + `--gc-sections` | -1~3 MB | ~17-22 MB |
| 移除 Arrow IPC / RE2 / CSV 等可选模块 | -3~6 MB | ~13-18 MB |
| Amalgamation 构建 | -1~3 MB | ~12-16 MB |

**保守目标**: 20 MB 以下 (仅需编译器/链接器优化)
**激进目标**: 12-15 MB (需要裁剪 DuckDB 核心模块)
**极限目标**: < 10 MB (需要深度重构，移除大量 SQL 功能)

---

## 七、建议的实施优先级

1. **P0 (立即可做)**: `-Os` + LTO thin + strip + `--gc-sections`/`-dead_strip` — 无代码改动，仅 CMake 配置
2. **P1 (短期)**: 验证 `-fvisibility=hidden` 兼容性，应用到移动端构建
3. **P2 (中期)**: 基于 WASM 的最小导出列表技术，为移动端生成最小符号表
4. **P3 (长期)**: 条件编译移除 Arrow IPC、RE2、CSV、Window Functions 等非必要模块
5. **P4 (可选)**: Amalgamation 构建模式适配

---

## 参考来源

- [DuckDB 1.5.0 发布公告](https://duckdb.org/2026/03/09/announcing-duckdb-150) — 扩展体积优化 30-60%
- [DuckDB 构建配置文档](https://duckdb.org/docs/stable/dev/building/build_configuration)
- [DuckDB Android 构建文档](https://duckdb.org/docs/stable/dev/building/android)
- [DuckDB WASM 编译与分发 (DeepWiki)](https://deepwiki.com/duckdb/duckdb-wasm/5.2-bundling-and-distribution)
- [DuckDB-WASM 体积讨论 (GitHub #1469)](https://github.com/duckdb/duckdb-wasm/discussions/1469)
- [DuckDB 扩展 README](https://github.com/duckdb/duckdb/blob/main/extension/README.md)
- [DuckDB 移动端 TPC-H 博文](https://duckdb.org/2024/12/06/duckdb-tpch-sf100-on-mobile)
- [strip 破坏扩展问题 (GitHub #15975)](https://github.com/duckdb/duckdb/issues/15975)
- [DuckDB on Raspberry Pi (MotherDuck)](https://motherduck.com/blog/duckdb-on-edge-raspberry-pi/)
