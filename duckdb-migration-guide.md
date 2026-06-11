# DuckDB 侧适配指南 — pg_full 分支迁移

基于 `pg_full` 分支最近三个 commit 的变更，整理 DuckDB 侧需要做出的全部改动。

---

## 一、变更总览

三个 commit 的核心改动：

| Commit | 改动要点 |
|--------|---------|
| `69a8f0be77` | 新建 `common/platform/` 兼容层，`duck_pg_shim.hpp` 删除，合并至 `vex_simple_rwlock.hpp` + `duck_compat.h`；`common/include/graph_index/` 移至 `common/graph_index/`；删除冗余头文件；VTL allocator 统一走 `platform_compat.h` |
| `91f891e478` | PG 侧新增 `halfvec` / `int8vector` 数据类型 |
| `d32dd56cfa` | distance `core/` → `include/`，新增 half/int8 dispatch；量化器重构 `pq_alloc` / `product_quantizer` → `pq/` 子目录，新增 `rabitq` 实现；graph_index_algorithm 精简；PG 侧 distance `.cpp` 全部删除，改用 `common/distance/src/` 统一源 |

### 设计原则

**Duck 侧与 PG 侧完全共用 `common/distance/src/` 下的所有距离计算源文件**，不再维护 Duck 专属的距离分发代码（旧的 `vexdb_duckdb/distance/*.cpp` 已删除且不应恢复）。

运行时区别通过 `DispatchRunner` 的模板参数体现：
- **Duck 侧**：`DistPrecisionTypeList<DistPrecisionType::FLOAT>` — 只走 FLOAT 路径
- **PG 侧**：`DistPrecisionTypeList<DistPrecisionType::FLOAT, DistPrecisionType::HALF, DistPrecisionType::INT8>` — 走全部路径

HALF / INT8 的 SIMD 函数（来自 `template_half.cpp`）在 Duck 编译中会生成死代码，但不会参与运行时分发——可接受，不做 `#if` 隔离。

---

## 二、CMakeLists.txt 改动（最关键，当前编译直接报错）

### 2.1 删除已不存在的源文件

```cmake
# ❌ 以下文件已删除，需从 VEXDB_DUCK_EXTENSION_FILES 中移除
${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/pq_alloc.cpp        # → pq/pq.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/product_quantizer.cpp # → pq/pq.cpp + pq_distancer.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/pq_dispatcher.cpp  # 已删除，无替代
${CMAKE_CURRENT_SOURCE_DIR}/distance/general.cpp                      # 已删除
${CMAKE_CURRENT_SOURCE_DIR}/distance/sse.cpp                          # 已删除
${CMAKE_CURRENT_SOURCE_DIR}/distance/avx.cpp                          # 已删除
${CMAKE_CURRENT_SOURCE_DIR}/distance/avx512.cpp                       # 已删除
${CMAKE_CURRENT_SOURCE_DIR}/distance/neon.cpp                         # 已删除
```

### 2.2 替换为 common 统一源文件（与 PG 侧完全相同）

参考 PG 侧 `vexdb_pg/CMakeLists.txt` 的 `DISTANCE_SRCS` + `QUANTIZER_SRCS`：

```cmake
# ✅ 量化器
${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/annkmeans.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/quantizer.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/pq/pq.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/pq/pq_distancer.cpp
```

> RaBitQ 相关文件（`rabitq.cpp`、`rabitq_distancer.cpp`、`rotator.cpp`、`estimator.cpp`）视需要加入。如果 `distance_dispatcher.h` 在 Duck 侧未解锁量化器分发（见第九节），暂不加也可。

```cmake
# ✅ distance — 与 PG 侧 DISTANCE_SRCS 一致，但排除 distance.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/architecture.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/general/general_dispatcher.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/general/general.cpp
```

x86:
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/sse/sse_dispatcher.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/sse/sse.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx/avx_dispatcher.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx/avx.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx512/avx512_dispatcher.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx512/avx512.cpp
```

ARM:
```cmake
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/armv8/neon/neon_dispatcher.cpp
${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/armv8/neon/armv8neon.cpp
```

> **唯一排除**：`common/distance/src/distance.cpp` 不加入。该文件包含 `utils/lsyscache.h`（PG 专用），实现了 `get_func_metric()` 等 PG-only 函数。Duck 侧不需要。

### 2.3 新增 include 路径

```cmake
include_directories(
    # ... 原有路径 ...
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/include      # ← 新增
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/include/pq   # ← 新增
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer             # ← 新增
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/rabitq      # ← 新增（如需 rabitq）
)
```

### 2.4 更新编译 flag 中的源文件路径

`set_source_files_properties` 中的路径需同步更新（与 PG 侧完全一致）：

```cmake
# x86_64
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/sse/sse_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/sse/sse.cpp
    PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -msse3 -msse4.1 -msse4.2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx/avx_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx/avx.cpp
    PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx -mavx2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx512/avx512_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/distance/src/x86/avx512/avx512.cpp
    PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx -mavx2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
```

ARM 类似更新。

### 2.5 annkmeans.cpp 编译属性

PG 侧 CMakeLists.txt 有：
```cmake
set_source_files_properties(${REPO_ROOT}/common/quantizer/annkmeans.cpp PROPERTIES COMPILE_FLAGS "-include platform_compat.h")
```

Duck 侧也需加上：
```cmake
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/quantizer/annkmeans.cpp
    PROPERTIES COMPILE_FLAGS "-include platform_compat.h"
)
```

---

## 三、头文件 include 路径变更

### 3.1 distance 相关

| 旧路径 | 新路径 | 状态 |
|--------|--------|------|
| `distance/core/distance.h` | `distance/include/distance.h` | ✅ 已迁移 |
| `distance/core/distance_dispatcher.h` | `distance/include/distance_dispatcher.h` | ✅ 已迁移 |
| `distance/core/distance_template.h` | `distance/include/distance_template.h` | ✅ 已迁移 |
| `distance/core/distance_utils_core.h` | `distance/include/distance_utils.h` | ✅ 已迁移+重命名 |
| `distance/core/architecture_macro.h` | `distance/include/architecture_macro.h` | ✅ 已迁移 |
| `distance/core/distance_func.h` | `distance/include/distance_func.h` | ✅ 已迁移 |
| `distance/core/transform_template_core.h` | `distance/include/transform_template.h` | ✅ 已迁移+重命名 |
| `distance/core/halfutils_core.h` | `common/data_type/halfutils.h` | ✅ 已迁移 |
| `distance/core/arch_dispatch_macros.h` | *(已删除)* | 功能内线 |
| `distance/core/distance_simd_macros.h` | *(已删除)* | 功能内联 |

### 3.2 graph_index 相关

| 旧路径 | 新路径 | 说明 |
|--------|--------|------|
| `common/include/graph_index/graph_index_algorithm.h` | `common/graph_index/graph_index_algorithm.h` | 通用算法 |
| `common/include/graph_index/graph_index_depend.h` | `common/graph_index/graph_index_depend.h` | 依赖分发 |
| `common/include/graph_index/graph_index_storage.h` | `vexdb_pg/include/graph_index/graph_index_storage.h` | **PG 专用** |
| `common/include/graph_index/graph_index_struct.h` | `vexdb_pg/include/graph_index/graph_index_struct.h` | **PG 专用** |
| `common/include/graph_index/graph_index_storage_impl.h` | *(已删除)* | 内联到 PG 源文件 |

### 3.3 量化器相关

| 旧路径 | 新路径 | 说明 |
|--------|--------|------|
| `quantizer/pq_alloc.h` | `quantizer/pq/pq.h` | ProductQuantizer 类 |
| `quantizer/pq_alloc.cpp` | `quantizer/pq/pq.cpp` | |
| `quantizer/product_quantizer.h` | `quantizer/pq/pq.h` | 合并 |
| `quantizer/product_quantizer.cpp` | `quantizer/pq/pq.cpp` + `pq_distancer.cpp` | 拆分 |
| *(无)* | `quantizer/quantizer.h` / `.cpp` | 新增：QuantizerType 枚举 + 元信息 |
| *(无)* | `quantizer/pq/pq_distancer.h` / `.cpp` | 新增：PQDistancer |
| `quantizer/rabitq/*` | `quantizer/rabitq/*` | 路径不变，新增 `.cpp` 实现 |

### 3.4 DuckDB 源文件中需修改的具体 include

`vexdb_duckdb/functions/distance_functions.cpp` 第 5 行：
```cpp
// ❌ 旧
#include "distance/core/distance_utils_core.h"
// ✅ 新
#include "distance/include/distance_utils.h"
```

> `distance/include/distance.h` 已 include `platform/platform_compat.h`，`distance/include/distance_dispatcher.h` 已 include `distance.h`，所以该行其实可以直接删除（被间接包含），但保留也不出错。

---

## 四、类型冲突解决（编译通过的前提）

### 4.1 `QuantizerType` 重定义冲突

**问题**：`vex_graph_index_depend_duck.hpp` 第 96-99 行定义了 `QuantizerType` 枚举，而 `common/quantizer/quantizer.h` 也定义了同名枚举。当 `graph_index.cpp` 同时 include `graph_index_algorithm.h`（→ `vex_graph_index_depend_duck.hpp`）和 `distance_dispatcher.h`（→ `quantizer.h`）时，会触发重定义错误。

**方案**：从 `vex_graph_index_depend_duck.hpp` 中删除 `QuantizerType` 定义，改为 include `quantizer/quantizer.h`（该文件只依赖 `platform_compat.h`）：

```cpp
// vex_graph_index_depend_duck.hpp
// 删除以下代码：
// enum class QuantizerType : uint8_t {
//     NONE = 0,
//     PQ = 1,
//     RABITQ = 2
// };

// 替换为：
#include "quantizer/quantizer.h"
```

### 4.2 `Oid` / `Relation` / 类型别名冲突

**问题**：`vex_graph_index_depend_duck.hpp` 定义了 `using Oid = duckdb::Oid;`、`using Relation = void *;`、`using uint8 = uint8_t;` 等类型别名，而 `duck_compat.h`（通过 `platform_compat.h` → `distance.h` 间接 include）也定义了完全相同的别名。

**方案**：在 `vex_graph_index_depend_duck.hpp` 顶部先 include `platform/platform_compat.h`，然后删除所有重复的类型别名：

```cpp
// vex_graph_index_depend_duck.hpp
#include "platform/platform_compat.h"  // 提供 Oid, Relation, uint8, palloc, ereport 等

// 删除以下重复定义（已由 duck_compat.h 提供）：
// using Oid = duckdb::Oid;
// using Relation = void *;
// using uint8 = uint8_t;
// using uint16 = uint16_t;
// using uint32 = uint32_t;
// using uint64 = uint64_t;
// using int8 = int8_t;
// using int16 = int16_t;
// using int32 = int32_t;
// using int64 = int64_t;
// using uint = unsigned int;
// using Size = size_t;
```

`duck_compat.h` 的 `using Oid = uint32_t` 与 `duckdb::Oid = uint32_t` 底层类型一致，不会有兼容性问题。

### 4.3 `BaseObject` 重定义

`duck_compat.h` 和旧的 `duck_pg_shim.hpp`（已被删除，但旧代码中可能有残留）都定义了 `BaseObject`。当前 `vex_graph_index_depend_duck.hpp` 中可能不需要 `BaseObject`（那是 PG 的 `palloc` 风格 new/delete）。如果仍有引用，确保只从 `duck_compat.h` 引入。

---

## 五、已完成的 common/ 清理（无需 Duck 侧 stub）

### 已完成的两项修改

**修改 1**：per-ISA 文件和 `template_half.cpp` 的 `#include "halfvec.h"` 改为 `#include "data_type/half.h"`。

这些文件只需要 `half` 类型（纯 C++），不需要 PG 的 `HalfVector` / `fmgr.h`。涉及 6 个文件：

- `common/distance/src/x86/avx/avx_dispatcher.cpp`
- `common/distance/src/x86/avx512/avx512_dispatcher.cpp`
- `common/distance/src/x86/sse/sse_dispatcher.cpp`
- `common/distance/src/general/general_dispatcher.cpp`
- `common/distance/src/armv8/neon/neon_dispatcher.cpp`
- `common/distance/src/template_half.cpp`

**修改 2**：`common/data_type/halfutils.h` 用 PG 侧版本覆盖。

原 common 版本是残留的脏文件（`g_instance` inline + `knl/knl_instance.h`），PG 编译从没真正用过它（include path 优先命中 PG 版本）。PG 版本将 `HalfToFloat4` / `Float4ToHalfUnchecked` 声明为 extern（实现在 `halfvec.cpp`），没有 `g_instance` 依赖。覆盖后 common 版本变干净，Duck 编译直接通过。

**修改 3**：删除 `vexdb_pg/include/data_type/` 下与 `common/data_type/` 重复的文件（`halfutils.h`、`vec_common.h`）。

### 为什么不需要 Duck 侧 stub

Duck 运行时只走 `DistPrecisionType::FLOAT`，HALF / INT8 路径是死代码。编译期之所以需要这些头文件，是因为 per-ISA `.cpp` 文件无条件定义所有模板特化。修改后这些头文件都是 PG-free 的，Duck 直接编译通过，零额外适配。

---

## 六、duck_compat.h 行为变化

旧版 `duck_pg_shim.hpp` 的 `palloc` 失败时 `throw std::bad_alloc()`。新的 `duck_compat.h` 保持一致。

**关键差异**：`ereport` 宏的语义变了。

新版 `duck_compat.h`：
- `PANIC` / `FATAL` → `fprintf + abort()`
- `ERROR` → `throw std::runtime_error` （匹配 PG 的 `longjmp` 语义）
- `WARNING` / `NOTICE` / `LOG` → `fprintf(stderr)`
- 新增 `errmsg` / `errcode` / `errdetail` / `errhint` 宏，支持 PG 风格的 `ereport(ERROR, (errcode(...), errmsg("fmt", args...)))`

如果 Duck 侧代码之前依赖 `ereport` 的特定行为（如仅 `fprintf` 而不 `throw`），需要注意 `ERROR` 级别现在会抛异常。

---

## 七、量化器对齐：Duck 侧走 DispatchRunner 分发

### 现状问题

Duck 侧 PQ 是自建的一套独立实现，不走 `common/` 的量化器分发链：

| 对比项 | PG 侧 | Duck 侧（当前） |
|--------|--------|-----------------|
| 搜索分发 | `DispatchRunner<..., DispatcherMode::DEFAULT>` | `DispatchRunner<..., DispatcherMode::NO_QUANT>` |
| PQ 距离计算 | `DispatchRunner` → `PQDistancer::get_distance_single` | `graph_index.cpp:SearchPQ` 自建 `pq_codes_` + `dist_table` 遍历 |
| PQ 码存储 | `VecStorageType::PureCode`（PG fork 文件） | `std::vector<uint8_t> pq_codes_`（内存） |
| RaBitQ | `DispatchRunner` → `RabitqDistancer` | 完全不支持 |
| Build | `DispatcherMode::BUILD_PAIR`（plain + quantizer 双 distancer） | 无量化器参与 build |
| 头文件依赖 | `common/quantizer/pq/pq_distancer.h` | `quantizer/product_quantizer.h`（**已删除**） |

**平台无关性现状：**

PQ/RaBitQ 的 `common/quantizer/` 代码中，核心算法已经是平台无关的（通过 `platform_compat.h` 的 `palloc/pfree/ereport` 抽象），但**持久化 I/O 层**仍然是 PG 专用的：

| 层次 | PQ | RaBitQ |
|------|-----|--------|
| 训练（KMeans + 量化） | ✅ 平台无关（`pq_distancer.cpp:train`） | ✅ 平台无关（`rabitq_distancer.cpp:train`） |
| 编码/解码 | ✅ 平台无关（`pq.cpp`） | ✅ 平台无关（`rabitq.h` inline） |
| 距离计算 | ✅ 平台无关（`pq_distancer.h` inline） | ✅ 平台无关（`rabitq_distancer.h` inline） |
| 序列化（prepare/flush/process） | ⚠️ 只有声明，PG adapter 实现 | ❌ common 代码中硬编码 PG Buffer API |
| 依赖的头文件 | ✅ 无 PG 专用头 | ❌ 依赖 `graph_index/graph_index.h`、`graph_index_quantizer.h`（PG only） |

**PQ 的 `prepare/flush/process`**：声明在 `pq_distancer.h`，common 中没有实现，完全由 PG/Duck 侧各自提供 adapter。Duck 侧只需约 50 行代码。

**RaBitQ 的 `prepare/flush/process`**：部分实现在 common `rabitq_distancer.cpp`（内含 PG Buffer API），部分在 PG `rabitq_adapter.cpp`。Duck 侧接入需要先重构 common 代码中的 PG 依赖。

### 目标

Duck 侧与 PG 侧一致，通过 `DispatchRunner` + `PQDistancer` / `RabitqDistancer` 使用量化器。图索引中的向量用压缩码表示。

### 改动清单

#### 7.1 替换 `ProductQuantizer` → `PQDistancer`

`vexdb_duckdb/include/vex_graph_index.hpp` 当前依赖已删除的 `quantizer/product_quantizer.h`，使用 `::vex::quantizer::ProductQuantizer`。

```cpp
// ❌ 旧
#include "quantizer/product_quantizer.h"
::vex::quantizer::ProductQuantizer pq_quantizer_;
std::vector<uint8_t> pq_codes_;

// ✅ 新
#include "quantizer/pq/pq_distancer.h"
PQDistancer pq_distancer_;
```

#### 7.2 `DispatchRunner` 从 `NO_QUANT` 改为 `DEFAULT`

`graph_index.cpp` 中的 `RunWithDuckAlgo` 和搜索路径：

```cpp
// ❌ 旧 — 完全绕过量化器
DispatchRunner<false, DuckMetricList, DuckDTypeList, DispatcherMode::NO_QUANT>::call(
    ..., QuantizerType::NONE, ...);

// ✅ 新 — 让 DispatchRunner 根据量化类型自动分发
DispatchRunner<false, DuckMetricList, DuckDTypeList, DispatcherMode::DEFAULT>::call(
    ..., quantizer_type, ...);
```

Build 路径使用 `BUILD_PAIR`（和 PG 侧一致）：
```cpp
DispatchRunner<false, DuckMetricList, DuckDTypeList, DispatcherMode::BUILD_PAIR>::call(
    metric, DistPrecisionType::FLOAT, dim, qt_type, run_build_index);
```

#### 7.3 删除 `SearchPQ` 自建实现

`graph_index.cpp` 第 588-720 行的 `SearchPQ` 方法是自建的暴力 PQ 码遍历。改为走 `DispatchRunner::DEFAULT` → `PQDistancer::get_distance_single/batch2` 后，此方法应删除。

搜索流程统一为：
```
DispatchRunner::call(..., QuantizerType::PQ, [&](auto &distancer) {
    distancer.prepare(index, metap);
    distancer.process(query, metap);
    GraphIndexAlgorithm algo{metap, store, distancer};
    auto res = algo.search(ctx, query, ef);
});
```

#### 7.4 实现 `PQDistancer::prepare/flush/process` 的 Duck 侧适配器

**当前代码分析：**

`common/quantizer/pq/pq_distancer.h` 中声明了 `prepare/flush/process/configure_for_metric/hnsw_read_pq_center`，但 `.cpp` 中**没有实现**——这些方法的全部实现都在 `vexdb_pg/src/quantizer/pq_adapter.cpp`。`pq_distancer.cpp` 只实现了 `train()` 和 `destroy()`。

这意味着：
- `train()` / `destroy()` / `compute_code()` / `get_distance_single()` / `get_distance_batch2()` — 平台无关，Duck 直接可用
- `prepare()` / `flush()` / `process()` / `configure_for_metric()` / `hnsw_read_pq_center()` — 只有声明，没有定义，**两个平台都必须各自提供实现**

如果链接时缺少这些实现，会报 `undefined reference`。

**PQ 适配器代码量很小**——PG 侧的 `pq_adapter.cpp` 只有约 168 行，其中约 30 行是进程级缓存（Duck 不需要）。Duck 侧需要实现的是：

| 函数 | PG 侧实现 | Duck 侧实现 |
|------|-----------|------------|
| `configure_for_metric` | 设置 PQ 参数 + 距离函数 + HALF/FLOAT 分支 | 同 PG 但去掉 HALF 分支 |
| `prepare` | 从 PG Buffer 读码本 + 缓存查找 | 分配 `dist_table`，设 `prepared=true` |
| `process` | HALF→FLOAT 转换 + `compute_distance_table` | 直接调 `compute_distance_table`（Duck 只有 FLOAT） |
| `flush` | `graph_index_store_qt_centroids` 写 PG Buffer | no-op（序列化由 `ExportStorageInfo` 负责） |
| `hnsw_read_pq_center` | PG Buffer 链式读取 | 不应被调用，`vex_elog(ERROR)` |

Duck 侧创建 `vexdb_duckdb/quantizer/pq_adapter.cpp`，约 50 行：

```cpp
#include "quantizer/pq/pq_distancer.h"
#include "platform/platform_compat.h"

void PQDistancer::configure_for_metric(size_t d, size_t M, size_t nbits_,
                                        Metric metric, DistPrecisionType /*precision*/)
{
    pq.set_basic_values(d, M, nbits_);
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.set_fvec_ny_distance_func(metric);
    pq.set_dist_code_func();
    _get_distance_precise_func = get_aligned_distance_func(metric, static_cast<uint32>(d));
    flag = (metric == Metric::INNER_PRODUCT) ? -1.0f : 1.0f;
}

void PQDistancer::prepare(Relation /*index*/, void * /*metap*/)
{
    if (prepared) return;
    dist_table = (float *)palloc(pq.M * pq.ksub * sizeof(float));
    prepared = true;
}

void PQDistancer::process(const char *query, void * /*metap*/)
{
    pq.compute_distance_table((const float *)query, dist_table);
}

void PQDistancer::flush(Relation /*index*/, BlockNumber /*qtcode_block*/, bool /*enabling*/)
{
    // Duck 侧持久化由 GraphIndex::ExportStorageInfo 负责
}

void PQDistancer::hnsw_read_pq_center(Relation /*index*/, ProductQuantizer & /*target*/,
                                       BlockNumber /*qtcode_block*/)
{
    vex_elog(ERROR, "hnsw_read_pq_center should not be called in DuckDB");
}
```

#### 7.4.1 训练采样数据：Duck 侧与 PG 侧的关键差异

**PG 侧的采样流程：**

PG 侧的 PQ 训练数据来自**堆表采样**，而非图索引构建过程中的全量向量：

```
graph_index_build.cpp::init_quantizer()
  → graph_index_quantizer_sample_data(heap, index, dim, ...)
    → ann_sample_rows(samples, heap, index, dim, max_samples=50000, need_norm, precision)
      // 顺序扫描堆表，取前 50000 行向量（PG 专用，涉及 heap scan / tuple / slot）
      // 支持 HALF→FLOAT 转换、cosine 归一化
  → PQDistancer::train(index, samples, dimension, metric, ...)    // 平台无关
    → ProductQuantizer::train(samples, metric, ...)                // 平台无关
      → KMeans(samples, ksub, ...)                                 // 平台无关
```

`ann_sample_rows` 是 **PG 专用**的（涉及 `TableScanDesc`/`TupleTableSlot`/`FormIndexDatum`），在 `vexdb_pg/src/quantizer/annkmeans.cpp` 中实现。Duck 侧不需要也不应该调用它。

关键参数：
- 最大采样数：`MAX_SAMPLE_VECTOR_NUM = 50000`
- 最小采样数：`GRAPH_INDEX_MIN_QT_SAMPLES_SIZE = 10000`（不足则跳过 PQ）
- 采样方式：顺序扫描堆表前 N 行（非随机采样）
- 数据格式：`FloatVectorArray`（`{length, maxlen, dim, *items}`）

**Duck 侧当前的训练流程：**

Duck 侧没有堆表采样步骤——`BuildBulk` 在构建 HNSW 图之后，将**全量向量**直接传给 `TrainAndEncodePQ`：

```cpp
// graph_index.cpp: BuildBulk 末尾
if (pq_m_ > 0 && !row_ids.empty()) {
    TrainAndEncodePQ(src, row_ids);  // src = 全量向量数据
}

// TrainAndEncodePQ 内部（当前的自建路径）
PQFloatArray samples;
samples.data   = const_cast<float *>(vec_data);  // 全量，不采样
samples.length = row_ids.size();
// → pq_quantizer_.train(kmeans_state, samples, 0, ctx)  // 自建的 PQContext 线程池
```

**迁移到 PQDistancer 后的适配：**

`PQDistancer::train()` 已经是平台无关的（`pq_distancer.cpp:train` → `pq.cpp:train` → `annkmeans.cpp KMeans`），Duck 侧只需将数据包装为 `FloatVectorArray` 后直接调用：

```cpp
void GraphIndex::TrainAndEncodePQ(const float *vec_data, const std::vector<row_t> &row_ids)
{
    const size_t n = row_ids.size();
    const size_t sample_count = std::min(n, static_cast<size_t>(50000));

    if (sample_count < 10000) {
        // 数据量不足，跳过 PQ（与 PG 侧 GRAPH_INDEX_MIN_QT_SAMPLES_SIZE 一致）
        return;
    }

    // 将全量或前 sample_count 条向量包装为 FloatVectorArray
    FloatVectorArray samples = FloatVectorArrayInit(sample_count, dimension_);
    for (size_t i = 0; i < sample_count; ++i) {
        FloatVectorArraySet(samples, i, vec_data + i * dimension_);
    }
    samples->length = sample_count;

    // 调 PQDistancer::train — 平台无关，内部走 ProductQuantizer::train → KMeans
    pq_distancer_.train(NULL, samples, dimension_, ToDuckMetric(metric_),
                        /*need_norm=*/false, build_threads_, /*maintenance_work_mem=*/0);

    FloatVectorArrayFree(samples);

    // 为全量向量生成 PQ 码
    pq_distancer_.flush(NULL, InvalidBlockNumber);

    auto code_size = pq_distancer_.code_size();
    pq_codes_.assign(row_ids.size() * code_size, 0);
    // ... 编码全量向量到 pq_codes_（按 row_id 排序）...
    pq_use_ = true;
}
```

**注意事项：**

1. **采样 vs 全量**：PG 侧因为需要扫描堆表，所以限制 50000 条。Duck 侧数据已在内存中，可以：
   - **方案 A（推荐）**：全量训练。数据已在内存，不增加 I/O 开销，只增加 K-means 迭代时间。
   - **方案 B**：采样到 50000 条，与 PG 完全对齐。好处是训练速度有上限。

2. **`need_norm` 参数**：PG 侧在采样时做 cosine 归一化（`ann_sample_rows` 内 `norm_func`）。Duck 侧的 `BuildBulk` 已经在传入前做了归一化（`if (metric_ == VexMetric::COSINE) normalize`），所以传 `false`。

3. **`parallel_workers` 参数**：`ProductQuantizer::train` 内部用 `std::thread` 并行跑 M 个子量化器的 K-means。Duck 侧传入 `build_threads_`，删除自建的 `PQContext` 线程池——不能叠加。

4. **`maintenance_work_mem`**：PG 侧传入 `maintenance_work_mem` GUC 值限制 K-means 内存。Duck 侧传 `0` 即可。

**GraphIndex 侧配套改动：**

在 `vex_graph_index.hpp` 中将 `ProductQuantizer pq_quantizer_` 替换为 `PQDistancer pq_distancer_` 后，训练和加载路径需要适配：

```cpp
// --- BuildBulk 训练路径 (原 TrainAndEncodePQ) ---
// 具体实现见 7.4.1 节的代码示例

// --- DeserializeFromStorage 恢复路径 ---

// --- DeserializeFromStorage 恢复路径 ---
// 从 IndexStorageInfo.options 读取 pq_m / codebook 后：
pq_distancer_.pq.set_basic_values(dim, M, 8);          // 设置维度参数
pq_distancer_.pq.set_fvec_L2sqr_ny_nearest_func();     // 设置距离函数
pq_distancer_.pq.set_fvec_ny_distance_func(metric);
pq_distancer_.pq.set_dist_code_func();
memcpy(pq_distancer_.pq.centroids, blob_data, blob_size); // 恢复码本
pq_distancer_.pq.trained = true;
pq_distancer_.flag = (metric == Metric::INNER_PRODUCT) ? -1.0f : 1.0f;
pq_distancer_._get_distance_precise_func = get_aligned_distance_func(metric, dim);
pq_distancer_.prepared = false;  // 搜索前会调 prepare 分配 dist_table

// --- 搜索路径 ---
// 走 DispatchRunner::DEFAULT → PQDistancer 自动分发:
//   distancer.prepare(index, metap);    // 分配 dist_table
//   distancer.process(query, metap);    // 构建距离表
//   distancer.get_distance_single(...); // ADC 查表
```

> **注意**：`PQDistancer::configure_for_metric` 和 `hnsw_read_pq_center` 是 private 方法。如果 Duck 侧需要从 `GraphIndex` 直接设置 `pq` 成员的状态（方案 A），有两种途径：
> 1. 将 `GraphIndex` 声明为 `PQDistancer` 的 friend
> 2. 在 `PQDistancer` 中新增 public 的 `setup(float *centroids, size_t d, size_t M, Metric metric)` 方法

#### 7.5 `distance_dispatcher.h` 解锁量化器 include

`distance_dispatcher.h` 当前 include 了 `quantizer/pq/pq_distancer.h` 和 `quantizer/rabitq/rabitq_distancer.h`。

- **PQ 路径**：`pq_distancer.h` 只 include `pq.h`，没有 PG 专用头文件依赖。Duck 侧直接可用。
- **RaBitQ 路径**：`rabitq_distancer.h` include 了 `graph_index/graph_index_quantizer.h`（只在 `vexdb_pg/include/` 中存在）和 `rabitq/rabitq_cache.h`（依赖 `platform_compat.h`）。如果不接入 RaBitQ，需要对 RaBitQ include 加条件守卫：

```cpp
#include "quantizer/pq/pq.h"
#include "quantizer/pq/pq_distancer.h"
#if defined(PG_VEXDB_TARGET_PG) || defined(PG_VEXDB_TARGET_DUCK_RABITQ)
#include "quantizer/rabitq/rabitq_distancer.h"
#endif
```

`graph_index_quantizer.h` 内容很简单（只有 `GRAPH_INDEX_RABITQ_NUM_CLUSTERS` 和 `GRAPH_INDEX_MIN_QT_SAMPLES_SIZE` 两个常量），后续可以移到 `common/quantizer/` 下消除平台差异。

#### 7.6 RaBitQ（后续）

**当前代码分析：**

RaBitQ 的情况比 PQ 复杂。`common/quantizer/rabitq/rabitq_distancer.cpp` 不是平台无关的——它包含大量 PG Buffer API 调用：

```cpp
// rabitq_distancer.cpp 中的 PG 依赖：
#include "graph_index/graph_index.h"          // PG graph_index 头文件
GraphIndexMetaPage metap = ...                 // PG 元页类型
ReadBuffer(index, qtcode_block)                // PG Buffer 管理
BufferGetPage(buf) / PageHeader / PageGetContents  // PG 页面操作
GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno   // PG 页面链表
ReleaseBuffer(buf)                             // PG Buffer 释放
```

方法拆分：

| 方法 | 位置 | 平台无关？ |
|------|------|-----------|
| `train()` | common `rabitq_distancer.cpp` | ✅ 纯 KMeans + 量化器训练 |
| `destroy()` | common `rabitq_distancer.cpp` | ✅ |
| `compute_code()` | common `rabitq_distancer.h` (inline) | ✅ |
| `get_distance_single()` | common `rabitq_distancer.h` (inline) | ✅ |
| `get_distance_batch2()` | common `rabitq_distancer.h` (inline) | ✅ |
| `read_rabitq_data()` | common `rabitq_distancer.cpp` | ❌ PG Buffer 链式读取 |
| `load_rabitq()` | common `rabitq_distancer.cpp` | ❌ 依赖 `GraphIndexMetaPage` |
| `load_rabitq_quantizer()` | common `rabitq_distancer.cpp` | ❌ 调 `read_rabitq_data` |
| `prepare()` | PG `rabitq_adapter.cpp` | ❌ 依赖 `GraphIndexMetaPage` |
| `process()` | PG `rabitq_adapter.cpp` | ❌ HALF 转换 |
| `flush()` | PG `rabitq_adapter.cpp` | ❌ `graph_index_store_qt_centroids` |
| `load_rabitq_cache()` | PG `rabitq_adapter.cpp` | ❌ PG 进程缓存 |

**Duck 侧接入 RaBitQ 需要：**

1. 加入 `common/quantizer/rabitq/*.cpp` 到 CMakeLists.txt
2. 将 `common/quantizer/rabitq/rabitq_distancer.cpp` 中的 PG 依赖方法（`read_rabitq_data`/`load_rabitq`/`load_rabitq_quantizer`）改为声明式——移到 PG adapter，或通过 `platform_compat.h` 抽象存储层
3. 实现 Duck 侧 `rabitq_adapter.cpp`：`prepare/flush/process/load_rabitq_cache` + 上述被移出的方法
4. 处理 `#include "graph_index/graph_index_quantizer.h"`（目前只在 `vexdb_pg/include/` 中）— 需要移到 `common/` 或在 Duck 侧提供等价定义
5. 解锁 `distance_dispatcher.h` 中 RaBitQ include

当前阶段可暂不接入。`distance_dispatcher.h` 中 `qt == QuantizerType::RABITQ` 的分支在 Duck 侧不会走到，`rabitq_distancer.cpp` 可不加入 Duck 的 CMakeLists.txt。

---

## 八、vex_simple_rwlock.hpp 变更

`duck_pg_shim.hpp` 已删除，其中 PG 并发原语 mock 已合并到 `vex_simple_rwlock.hpp`。当前 `vex_simple_rwlock.hpp` 提供：

- `LWLock` / `LWLockAcquire` / `LWLockRelease`（基于 `std::shared_mutex`）
- `SpinLockInit` / `SpinLockAcquire` / `SpinLockRelease`（基于 `std::atomic_flag`）
- `pg_memory_barrier` / `pg_read_barrier` / `pg_write_barrier`
- `LWLockPadded`（64-byte 对齐的 LWLock）
- `LW_EXCLUSIVE` / `LW_SHARED` 常量
- `LWLockInitialize` / `LWTRANCHE_EXTEND`

如果 Duck 侧代码中有 `#include "duck_pg_shim.hpp"` 的残留引用，需替换为 `#include "vex_simple_rwlock.hpp"` + `#include "platform/platform_compat.h"`。

---

## 九、distance_dispatcher.h 中量化器分发

`distance_dispatcher.h` 直接 include 了 `quantizer/pq/pq.h`、`quantizer/pq/pq_distancer.h`、`quantizer/rabitq/rabitq_distancer.h`。

Duck 侧 `DispatchRunner` 使用 `DispatcherMode::NO_QUANT`（见 `distance_functions.cpp`），运行时不会进入 PQ / RaBitQ 分支。但头文件的 include 会拉入 `PQDistancer` / `RabitqDistancer` 的声明——只要它们的 `prepare/flush/process` 有链接实现（哪怕是 stub），就不会报错。

**当前建议**（二选一）：

1. **条件编译守卫**：在 `distance_dispatcher.h` 的量化器 include 前加 `#if` 守卫，Duck 侧跳过，彻底避免拉入量化器符号。`DispatchRunner::call` 中 `NO_QUANT` 模式已有 `Assert(qt == QuantizerType::NONE)` 保护。

2. **Duck 侧 stub 实现**：为 `PQDistancer::prepare/flush/process` 和 `RabitqDistancer::prepare/flush/process` 提供 Duck 侧空实现（见第七节），这样 Duck 编译不需要改 `distance_dispatcher.h`。

---

## 十、改动检查清单

按优先级排序：

- [ ] **P0** 更新 `vexdb_duckdb/CMakeLists.txt`：删除已不存在的源文件引用，加入 `common/distance/src/` 和 `common/quantizer/` 新源文件，更新 include 路径和编译 flag
- [ ] **P0** 修复 `vex_graph_index_depend_duck.hpp` 类型冲突：删除 `QuantizerType` 定义（改 include `quantizer/quantizer.h`），删除与 `duck_compat.h` 重复的类型别名（`Oid`、`Relation`、`uint8` 等）
- [ ] **P0** 修复 `vexdb_duckdb/functions/distance_functions.cpp` include 路径：`distance/core/distance_utils_core.h` → `distance/include/distance_utils.h`
- [x] **P0** 清理 `common/data_type/halfutils.h` + per-ISA 文件 include + 删除 PG 侧冗余文件（已完成）
- [ ] **P1** 为 `annkmeans.cpp` 添加 `-include platform_compat.h` 编译属性
- [ ] **P1** 量化器对齐：`vex_graph_index.hpp` 中 `ProductQuantizer` → `PQDistancer`，删除自建 `SearchPQ`，`DispatchRunner` 从 `NO_QUANT` 改为 `DEFAULT`/`BUILD_PAIR`（见第七节）
- [ ] **P1** 实现 `PQDistancer::prepare/flush/process/configure_for_metric/hnsw_read_pq_center` 的 Duck 侧适配器（约 50 行，见第七节 7.4）
- [ ] **P1** 训练采样适配：`TrainAndEncodePQ` 改用 `FloatVectorArray` + `PQDistancer::train`（已平台无关），删除 `PQContext` 线程池，决定全量 vs 采样 50000 条（见第七节 7.4.1）
- [ ] **P2** 验证 `ereport(ERROR)` 抛异常行为不会破坏 Duck 侧异常处理链
- [ ] **P2** RaBitQ Duck 侧接入：重构 `rabitq_distancer.cpp` 中的 PG Buffer 依赖，实现 Duck 侧 adapter，移 `graph_index_quantizer.h` 到 `common/`（见第七节 7.6）
