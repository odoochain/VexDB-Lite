# iOS 静态库体积优化研究报告

**日期**: 2026-03-19
**当前基线**: 构建目录 `duckdb/build/ios_min/`, 链接后 stripped 二进制 **17MB**

---

## 一、当前二进制段(Segment)分析

| 段/节 | 大小 | 占比 | 说明 |
|--------|------|------|------|
| `__TEXT.__text` | **11.5 MB** | 76.2% | 机器码（主要优化目标） |
| `__TEXT.__const` | 1.09 MB | 7.3% | 常量数据（Unicode表、解析器表等） |
| `__TEXT.__gcc_except_tab` | 1.04 MB | 7.0% | C++ 异常处理表 |
| `__TEXT.__cstring` | 388 KB | 2.6% | 字符串字面量 |
| `__TEXT.__unwind_info` | 378 KB | 2.5% | 栈回溯信息 |
| `__DATA_CONST.__const` | 351 KB | - | 只读数据 |
| `__DATA_CONST.__got` | 17 KB | - | 全局偏移表 |
| `__LINKEDIT` | 1.9 MB | - | 符号表（strip 后基本消除） |
| **__TEXT 总计** | **15.1 MB** | 100% | |

---

## 二、编译器/链接器优化技术评估

### 2.1 `-fvisibility=hidden -fvisibility-inlines-hidden`

**影响**: 对预编译的 `.a` 文件无效，需要在 CMake 编译阶段加入。

- 主要减少 `__LINKEDIT` 中的符号导出表大小
- 对 stripped 二进制影响很小（strip 已经移除大部分符号）
- **预估节省**: ~50-100KB（stripped 后）
- **风险**: 低，DuckDB 作为静态库内部使用不需要导出符号

**建议**: 加入 CMake 编译参数，效果有限但无副作用。

### 2.2 `-fno-unwind-tables -fno-asynchronous-unwind-tables`

**影响**: 可减少 `__unwind_info`（378KB）和 `__eh_frame`（3KB）。

- 当前测试：只在链接阶段加此标志对预编译 `.a` **无效**（已验证，大小不变）
- 需要在 CMake **编译阶段**对所有源文件启用
- **注意**: 如果同时使用 C++ 异常（DuckDB 大量使用），去掉 unwind tables 可能导致异常处理失败
- `__gcc_except_tab`（1.04MB）依赖 unwind 信息，不能独立去掉

**预估节省**: ~378KB（仅 unwind_info，前提是不破坏异常）
**风险**: **高** — DuckDB 内部重度依赖 C++ 异常，去掉可能导致崩溃
**建议**: **不推荐**，除非确认所有异常路径都改为错误码

### 2.3 `-Wl,-dead_strip` 效果验证

**当前已启用，效果显著**:

| 来源 | .a 文件总量 | 链接后大小 | 剥离率 |
|------|------------|-----------|--------|
| `libduckdb_static.a` | 75 MB | ~14 MB | **81%** |
| 第三方库 .a 合计 | 5.7 MB | 737 KB | **87%** |

Dead strip 已经在高效工作。注意：第三方的独立 `.a` 文件实际**不需要**单独链接（已验证），它们的目标文件已全部打包进 `libduckdb_static.a`。

### 2.4 `strip` 变体对比

| Strip 方式 | 大小 | 说明 |
|-----------|------|------|
| 不 strip | 32 MB | 包含所有调试符号 |
| `strip -S` | 32 MB | 仅去调试符号（.debug sections），效果不明显 |
| `strip -x` | 18 MB | 去本地符号，保留全局 |
| `strip`（完整） | **17 MB** | 去所有非必要符号 |

**结论**: 完整 `strip` 是最优选择，已在使用。

### 2.5 `-flto=full` vs `-flto=thin`

**关键发现**: 当前 `.a` 文件 **未包含 LTO bitcode**。仅在链接阶段加 `-flto` 无效（已验证：大小完全相同）。

要获得 LTO 优化，**必须在 CMake 编译阶段**添加:
```cmake
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
# 或
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto=full")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=full")
```

**预估节省**:
- `-flto=full`: 预计减少 **10-20%** 代码段（约 1.2-2.3 MB），因为可以跨模块内联、消除重复代码
- `-flto=thin`: 预计减少 **5-15%**，编译更快但优化不如 full
- LTO 对模板重度使用的 C++ 代码库（如 DuckDB）效果尤其明显

**风险**: 中 — 编译时间大幅增加（可能 3-5x），内存消耗高
**建议**: **强烈推荐** `-flto=full`，这是最大的潜在收益来源

### 2.6 `-Wl,-why_load` 分析

链接映射(link map)分析显示仅 **272 个目标文件**被链接（从 `.a` 中抽取），证实 dead strip 工作良好。

---

## 三、最大代码贡献者分析

### 3.1 按目标文件排名（Top 15）

| 目标文件 | 大小 | 功能 |
|----------|------|------|
| `ub_duckdb_func_cast.cpp.o` | **855 KB** | 类型转换函数 |
| `ub_duckdb_core_functions_distributive.cpp.o` | 539 KB | 聚合函数（SUM/COUNT/AVG等） |
| `ub_duckdb_func_ops_main.cpp.o` | 491 KB | 运算符函数 |
| `ub_duckdb_sort.cpp.o` | 484 KB | 排序实现 |
| `ub_duckdb_expression_executor.cpp.o` | 465 KB | 表达式执行器 |
| `ub_duckdb_core_functions_holistic.cpp.o` | 453 KB | 完整聚合函数 |
| `ub_duckdb_common_types.cpp.o` | 446 KB | 类型系统 |
| `src_backend_parser_gram.cpp.o` | 382 KB | SQL 解析器（Bison 生成） |
| `ub_duckdb_common.cpp.o` | 373 KB | 通用工具 |
| `utf8proc.cpp.o` | 329 KB | Unicode 处理 |
| `ub_duckdb_main.cpp.o` | 325 KB | 主入口 |
| `ub_duckdb_core_functions_date.cpp.o` | 320 KB | 日期函数 |
| `ub_duckdb_storage_table.cpp.o` | 308 KB | 存储引擎 |
| `ub_duckdb_optimizer.cpp.o` | 274 KB | 查询优化器 |
| `bitpacking.cpp.o` | 264 KB | 压缩 (bitpacking) |

### 3.2 第三方库实际贡献

| 库 | .a 文件大小 | 链接后实际大小 | 功能 | VEX 是否需要? |
|----|-----------|-------------|------|------------|
| **utf8proc** | 352 KB | **333 KB** | Unicode 处理 | **需要** — SQL 解析、字符串处理核心依赖 |
| **zstd** | 636 KB | **211 KB** | 压缩 | **需要** — DuckDB 存储引擎默认压缩格式 |
| **yyjson** | 225 KB | **132 KB** | JSON 解析 | 可能可裁 — 用于 profiling、variant 类型 |
| **fsst** | 52 KB | **25 KB** | 字符串压缩 | **需要** — 存储引擎字符串列压缩 |
| **miniz** | 82 KB | **20 KB** | gzip 压缩 | 部分需要 — gzip 文件系统支持 |
| **re2** | 1.0 MB | **8.5 KB** | 正则引擎 | 几乎全被 strip — 仅少量引用 |
| **mbedtls** | 201 KB | **7.4 KB** | TLS/加密 | 几乎全被 strip — 存储加密、SHA 哈希 |
| **hyperloglog** | 27 KB | **1.8 KB** | 基数估计 | 极小开销 |
| **skiplist** | 5 KB | **0.2 KB** | 跳表 | 极小开销 |
| **fastpforlib** | 2.0 MB | **0 KB** | 整数压缩 | 已被完全 strip（通过 bitpacking.cpp 间接引用） |
| **总计** | **5.7 MB** | **737 KB** | | |

**关键发现**: re2 (1MB .a) 和 fastpforlib (2MB .a) 虽然 archive 很大，但 dead strip 后几乎完全消除。真正占空间的是 utf8proc 和 zstd，但这两个是核心依赖无法去除。

---

## 四、可裁减模块分析

以下模块对 VEX 向量搜索场景**不是必需的**：

| 模块 | 链接后大小 | 说明 |
|------|-----------|------|
| CSV Scanner | 233 KB | CSV 导入/导出 |
| Variant/JSON | 283 KB | VARIANT 类型、JSON 操作 |
| System Table Functions | 188 KB | `duckdb_tables()` 等系统表 |
| C API | 181 KB | C 语言接口（iOS 用 C++ 接口即可） |
| Window Functions | 141 KB | 窗口函数（VEX 不需要） |
| Arrow | 98 KB | Apache Arrow 格式支持 |
| **合计** | **~1.1 MB** | 约占代码段 9.5% |

**但注意**: 这些模块通过 unity build 打包在一起，无法通过简单的链接参数去除。需要修改 DuckDB 的 `src/CMakeLists.txt` 添加条件编译开关，工作量较大。

---

## 五、优化建议优先级

### 第一优先级：LTO 全量优化（预估节省 1.2-2.3 MB）

```cmake
# 在 iOS build 配置中加入
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -flto=full")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto=full")
set(CMAKE_AR "${CMAKE_C_COMPILER_AR}")
set(CMAKE_RANLIB "${CMAKE_C_COMPILER_RANLIB}")
```

这是**投入产出比最高**的优化。LTO 可以：
- 跨模块消除重复模板实例化（DuckDB 大量使用模板）
- 内联跨翻译单元的小函数
- 更精确的 dead code elimination
- 减少 `__gcc_except_tab` 和 `__unwind_info`

### 第二优先级：Visibility + 编译标志（预估节省 100-200 KB）

在 CMake 中为所有目标添加：
```cmake
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)
```

### 第三优先级：裁减非必要 DuckDB 子系统（预估节省 0.5-1 MB）

需要 fork 修改 DuckDB 的 `src/CMakeLists.txt`，添加条件编译：
- `DUCKDB_NO_CSV` — 去除 CSV scanner（233 KB）
- `DUCKDB_NO_ARROW` — 去除 Arrow 支持（98 KB）
- `DUCKDB_NO_CAPI` — 去除 C API（181 KB）
- `DUCKDB_NO_VARIANT` — 去除 Variant 类型（283 KB）

**工作量大**，需要处理各种编译依赖关系。

### 第四优先级：`-fno-exceptions` 实验（预估节省 1.4 MB，风险极高）

去掉 C++ 异常可减少 `__gcc_except_tab`(1.04 MB) + `__unwind_info`(378 KB)。但 DuckDB 内核重度依赖异常，需要将所有 `throw` 改为返回错误码，工作量极大。**不推荐**。

---

## 六、总结

| 优化措施 | 预估节省 | 难度 | 风险 | 推荐度 |
|---------|---------|------|------|--------|
| **LTO full** | 1.2-2.3 MB | 低（改 CMake） | 低 | ★★★★★ |
| Visibility hidden | 100-200 KB | 低 | 低 | ★★★★ |
| 裁减子系统(CSV/Arrow/CAPI) | 0.5-1 MB | 高（改源码） | 中 | ★★★ |
| 去除 re2/mbedtls | ~16 KB | 不值得 | - | ★ |
| 去除 zstd | 不可行 | - | 高 | ✗ |
| `-fno-exceptions` | 1.4 MB | 极高 | 极高 | ✗ |
| Dead strip | 已生效 | - | - | 已用 |
| Full strip | 已生效 | - | - | 已用 |

**当前 17MB，通过 LTO + visibility 有望降至 ~14-15MB。如果再裁减子系统可降至 ~13-14MB。**

最低理论极限（仅保留核心引擎 + VEX 扩展 + 必要第三方库）估计在 **10-12MB** 左右。
