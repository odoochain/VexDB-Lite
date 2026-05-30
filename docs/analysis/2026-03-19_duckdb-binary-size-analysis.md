# DuckDB 二进制体积分析与优化策略

日期: 2026-03-19
构建目标: iOS MinSizeRel (arm64), `-Oz -DNDEBUG`

---

## 一、当前体积概况

### 最终合并库
| 产物 | 大小 |
|------|------|
| **libvexdb.a** (最终合并) | **92 MB** |

### 各组件大小

| 组件 | 大小 | 占比 |
|------|------|------|
| libduckdb_static.a (核心) | 75 MB | 81.5% |
| core_functions_extension.a | 10.4 MB | 11.3% |
| libduckdb_fastpforlib.a | 2.0 MB | 2.2% |
| libduckdb_re2.a (正则) | 1.0 MB | 1.1% |
| vex_extension.a | 1.3 MB | 1.4% |
| libduckdb_zstd.a | 636 KB | 0.7% |
| libduckdb_pg_query.a | 552 KB | 0.6% |
| libduckdb_fmt.a | 536 KB | 0.6% |
| libduckdb_utf8proc.a | 356 KB | 0.4% |
| libduckdb_yyjson.a | 228 KB | 0.2% |
| libduckdb_mbedtls.a | 204 KB | 0.2% |
| 其他 (miniz, fsst, hyperloglog, skiplist) | ~172 KB | 0.2% |

> 注意: .a 文件包含所有 .o 对象，最终链接时 dead stripping 会移除未引用符号。实际链接后的二进制会更小。

### 核心库 (libduckdb_static.a) 模块分解 (75 MB)

| 模块 | 大小 | 说明 |
|------|------|------|
| functions (函数系统) | 8.0 MB | 类型转换、标量/表函数、窗口函数 |
| common (通用代码) | 7.7 MB | 类型系统、HTTP、Arrow、多文件、运算符 |
| storage (存储引擎) | 6.3 MB | 表存储、压缩、序列化 |
| main (主模块) | 4.5 MB | **含 CAPI 921KB、HTTP 503KB、Secret 386KB、Relation 508KB** |
| operator (算子) | 3.8 MB | join、aggregate、persistent 等物理算子 |
| optimizer (优化器) | 2.9 MB | 查询优化、join order |
| binder (绑定器) | 2.4 MB | SQL 绑定 |
| parser (解析器) | 2.1 MB | SQL 解析、transformer |
| planner (计划器) | 2.0 MB | 查询计划 |
| sort (排序) | 1.6 MB | 排序算法 |
| catalog (目录) | 1.6 MB | 系统目录 |
| csv (CSV处理) | 1.2 MB | CSV scanner、sniffer |
| compression (压缩) | 1.3 MB | bitpacking 独占 |
| execution (执行引擎) | 1.2 MB | 含 ART 索引 |
| table_functions (系统表函数) | 1.0 MB | system info 表函数 |
| 其他 | ~6 MB | Arrow、logging、verifier、re2 等 |

### core_functions_extension (10.4 MB) 分解

| 子模块 | 大小 | 说明 |
|--------|------|------|
| aggregate/distributive | 2.1 MB | count, sum, min, max 等 |
| aggregate/holistic | 2.0 MB | median, quantile, mode 等 |
| scalar/date | 1.0 MB | 日期函数 |
| scalar/string | 0.7 MB | 字符串函数 |
| aggregate/nested | 0.6 MB | list_agg 等 |
| scalar/math | 0.5 MB | 数学函数 |
| scalar/list | 0.4 MB | 列表函数 |
| 其他 (operators, generic, map, struct, array, bit, blob, random, union, regression) | ~2.1 MB | |

---

## 二、当前构建已启用的优化

- [x] `-Oz` 最小体积优化
- [x] `-DNDEBUG` 禁用断言
- [x] `BUILD_SHELL=OFF`
- [x] `BUILD_UNITTESTS=OFF`
- [x] `DISABLE_THREADS=ON`
- [x] `SKIP_EXTENSIONS="parquet;jemalloc"`
- [x] `EXTENSION_STATIC_BUILD=ON`
- [x] 仅加载 core_functions + vex 两个扩展

---

## 三、可操作的优化建议

### 优先级 1：高收益、低风险

#### 1.1 启用 `SMALLER_BINARY=ON` (预计节省 2-4 MB)
```cmake
-DSMALLER_BINARY=ON
```
- 影响 8 个源文件，43 处条件编译
- 移除 type-specialized 的代码路径 (scatter/gather、is_distinct_from、row_matcher、between、executor)
- 使用通用路径代替，有轻微性能损失但对向量搜索场景影响极小
- **VEX 不依赖这些 specialized 路径**

#### 1.2 启用 `-ffunction-sections -fdata-sections` + 链接时 dead strip (预计节省 10-20 MB)
当前 iOS 构建 **未启用** 此优化（仅在 EXTENSION_STATIC_BUILD + GNU 编译器时启用）。

在 `build.sh` 的 iOS 配置中添加:
```cmake
-DCMAKE_CXX_FLAGS_MINSIZEREL="-Oz -DNDEBUG -ffunction-sections -fdata-sections"
-DCMAKE_C_FLAGS_MINSIZEREL="-Oz -DNDEBUG -ffunction-sections -fdata-sections"
```
链接时 Apple Clang 默认已有 `-dead_strip`，但需要配合 `-ffunction-sections` 才能精细剥离。

**这是收益最大的单项优化。** 静态库中大量未被 VEX 调用的函数（CSV parser、窗口函数、join 算子等）会被链接器自动移除。

#### 1.3 启用 `DISABLE_EXTENSION_LOAD=ON` (预计节省 ~200 KB)
```cmake
-DDISABLE_EXTENSION_LOAD=ON
```
- 移除动态扩展加载/安装代码
- iOS 不支持 dlopen 加载扩展，此功能完全无用
- 注意：之前注释说会导致链接错误，需要验证当前版本

### 优先级 2：中等收益

#### 2.1 排除 C API 模块 (~921 KB)
`ub_duckdb_main_capi.cpp.o` 占 921 KB。如果只通过 C++ API 使用 DuckDB：
- 在 `src/main/CMakeLists.txt` 中条件编译 capi：
```cmake
if(NOT DISABLE_CAPI)
  add_subdirectory(capi)
endif()
```
- 需要添加 `-DDISABLE_CAPI=ON` CMake 选项
- **需要确认 VEX 和 core_functions 不依赖 C API**

#### 2.2 排除 Arrow 相关代码 (~655 KB)
Arrow 转换模块:
- `ub_duckdb_common_arrow.cpp` (457 KB)
- `ub_duckdb_arrow_conversion.cpp` (146 KB)
- `ub_duckdb_common_arrow_appender.cpp` (52 KB)
- iOS 场景不需要 Arrow IPC，可以考虑条件编译

#### 2.3 排除 ADBC 模块 (~175 KB)
- `ub_duckdb_adbc.cpp.o` (163 KB) + nanoarrow (12 KB)
- ADBC 是数据库连接标准，iOS 嵌入式不需要

#### 2.4 排除 HTTP + Secret 模块 (~889 KB)
- `ub_duckdb_common_http.cpp.o` (503 KB)
- `ub_duckdb_main_secret.cpp.o` (386 KB)
- iOS 不通过 DuckDB 做 HTTP 请求
- Secret manager 是用于 httpfs 等扩展的凭证管理

#### 2.5 减少 CSV 模块 (~1.2 MB)
- 如果不需要 CSV 导入，可以条件编译移除 csv scanner/sniffer
- 需要注意 DuckDB 核心可能有对 CSV 的硬依赖

### 优先级 3：深度裁剪（需要改源码）

#### 3.1 精简 core_functions (~4 MB 潜在节省)
core_functions 有 10.4 MB，但 VEX 只需要基础功能:
- **必需**: 基础数学、比较、类型转换
- **可移除**: holistic aggregates (2 MB)、date 函数 (1 MB)、string 函数 (0.7 MB)、list 函数 (0.4 MB)
- 方法: Fork core_functions，移除不需要的函数注册
- 风险：DuckDB 内部可能隐式依赖某些函数

#### 3.2 精简压缩算法 (~2 MB)
- bitpacking.cpp 单独占 1.3 MB
- 加上其他压缩算法 (ALP, chimp, patas, dict_fsst, roaring 等) 约 2 MB
- VEX 的向量数据使用自己的 PQ 压缩，但 DuckDB 表数据存储需要这些
- 可以只保留 uncompressed + 1-2 个基础压缩

#### 3.3 精简第三方库 (~3 MB)
- **fastpforlib** (2 MB): 用于 bitpacking 压缩，如果精简压缩算法可一起移除
- **re2** (1 MB): 正则表达式引擎，VEX 不需要正则匹配
- **mbedtls** (204 KB): TLS 加密库，嵌入式场景不需要

#### 3.4 移除 Logging 模块 (~410 KB)
- `ub_duckdb_logging.cpp.o` 占 410 KB
- iOS 嵌入可以禁用 DuckDB 内部日志

#### 3.5 移除 Verifier 代码 (~180 KB)
- Release 构建中 verifier 不应该被调用
- 但节省量较小，优先级低

---

## 四、预期效果汇总

| 优化项 | 预计节省 (.a) | 实施难度 |
|--------|--------------|----------|
| -ffunction-sections + dead strip | 10-20 MB | 低 (改 build.sh) |
| SMALLER_BINARY=ON | 2-4 MB | 低 (加 CMake 参数) |
| DISABLE_EXTENSION_LOAD=ON | ~200 KB | 低 (加 CMake 参数) |
| 移除 C API | ~921 KB | 中 (改 CMakeLists) |
| 移除 Arrow | ~655 KB | 中 (改 CMakeLists) |
| 移除 HTTP + Secret | ~889 KB | 中 (改 CMakeLists) |
| 移除 ADBC | ~175 KB | 中 (改 CMakeLists) |
| 精简 core_functions | ~4 MB | 高 (改扩展源码) |
| 精简压缩 + fastpforlib | ~3 MB | 高 (改核心源码) |
| 移除 re2 | ~1 MB | 高 (需移除正则函数) |
| **合计** | **~24-35 MB** | |

最终 libvexdb.a 预计可从 **92 MB → 57-68 MB** (.a 文件大小)。

链接到最终二进制后（配合 dead strip），实际代码段大小预计可从约 30-40 MB 降至 **15-25 MB**。

---

## 五、推荐的立即实施方案

在 `build.sh` 的 iOS 配置中修改:
```bash
cmake -S "$DUCKDB_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_TOOLCHAIN_FILE="$DUCKDB_DIR/scripts/ios-toolchain.cmake" \
    -DIOS_PLATFORM="$IOS_PLATFORM" \
    -DCMAKE_CXX_FLAGS_MINSIZEREL="-Oz -DNDEBUG -ffunction-sections -fdata-sections" \
    -DCMAKE_C_FLAGS_MINSIZEREL="-Oz -DNDEBUG -ffunction-sections -fdata-sections" \
    -DBUILD_SHELL=OFF \
    -DBUILD_UNITTESTS=OFF \
    -DENABLE_EXTENSION_AUTOLOADING=OFF \
    -DENABLE_EXTENSION_AUTOINSTALL=OFF \
    -DEXTENSION_STATIC_BUILD=ON \
    -DVEX_MOBILE_MODE=ON \
    -DDISABLE_THREADS=ON \
    -DSMALLER_BINARY=ON \
    -DDISABLE_EXTENSION_LOAD=ON \
    -DSKIP_EXTENSIONS="parquet;jemalloc" \
    "${MOBILE_EXT_CONFIG:+-DDUCKDB_EXTENSION_CONFIGS=$MOBILE_EXT_CONFIG}"
```

这三个改动（`-ffunction-sections -fdata-sections`、`SMALLER_BINARY=ON`、`DISABLE_EXTENSION_LOAD=ON`）零风险，不需要修改源码，预计可节省 12-24 MB。
