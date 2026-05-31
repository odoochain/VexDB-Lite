# 数据库适配版本

本文档列出 VexDB 各产物所支持的数据库版本、操作系统和 CPU 架构。

---

## 支持矩阵

### PostgreSQL 插件（`vexdb_vector.so`）

| 数据库 | 支持版本 | 说明 |
|--------|----------|------|
| PostgreSQL | **16 / 17 / 18 / 19** | 已适配，主验证平台为 PG 19 |
| openGauss | — | 参见 VexDB 主库 |

### DuckDB 扩展（`vex.duckdb_extension`）

| 数据库 | 支持版本 | 说明 |
|--------|----------|------|
| DuckDB | **v1.5.2** | 当前唯一支持版本 |
| DuckDB | 其他版本 | 不兼容（扩展 API 与 metadata footer 格式随版本变化） |

> DuckDB 扩展与运行时版本严格绑定。加载前请通过 `SELECT version()` 确认版本为 v1.5.2。

---

## 操作系统兼容性

### Linux

| 发行版 | 架构 | 状态 | 说明 |
|--------|------|------|------|
| Kylin V10 SP1 / SP3 | aarch64 | ✅ 已验证 | 主要测试平台（ARM） |
| Ubuntu 22.04 | aarch64 | ✅ 已验证 | ARM 性能基准测试平台 |
| Ubuntu 22.04 | x86_64 | ✅ 已验证 | x86 性能基准测试平台 |
| CentOS 8 | x86_64 | ✅ 已验证 | manylinux_2_28 兼容 |
| CentOS 7 | x86_64 | ⚠️ 需升级 libstdc++ | 默认 GLIBCXX 3.4.19，需升级 |
| 其他 glibc ≥ 2.28 | x86_64 / aarch64 | ✅ 应兼容 | 未逐一验证 |

### macOS

| 版本 | 架构 | 状态 | 说明 |
|------|------|------|------|
| macOS 14 / 15 | arm64 (Apple Silicon) | 🔧 源码构建 | 开发者本地构建可用，不在 Release 中发布 |
| macOS 13 | x86_64 | 🔧 源码构建 | 同上 |

> macOS 产物不在预编译 Release 中发布，仅供本地开发使用。

---

## CPU 架构

| 架构 | SIMD 优化 | 最低 CPU 要求 | 说明 |
|------|-----------|--------------|------|
| x86_64 | SSE4.1 / AVX2 / AVX-512 | SSE4.1 | 运行时动态检测最优实现 |
| AArch64 | NEON（ARMv8.2 dotprod） | **ARMv8.2+** | 运行时动态检测；ARMv8.0（如 Cortex-A53/A57）因 LSE 指令不兼容，会触发 SIGILL |
| AArch64 SVE | 代码已就位，默认构建未启用 | — | 默认 `-march=armv8.2-a+lse+fp16+dotprod+crc`，无 `+sve` |

> 距离计算采用运行时 SIMD dispatch 机制，无需重新编译即可利用当前 CPU 的最高 SIMD 级别。

---

## 系统库要求

### glibc

| 产物 | 最低 glibc |
|------|-----------|
| DuckDB 扩展（Linux） | **2.28** |
| PG 插件（Linux） | **2.28** |

两者均通过 manylinux_2_28 工具链或等效 GCC 10.3 / 9.3 编译，不支持 glibc < 2.28 的旧系统（如 CentOS 6）。

### libstdc++ / GLIBCXX

| 产物 | GLIBCXX 依赖上限 | 编译工具链 |
|------|-----------------|------------|
| `vex.duckdb_extension` | `GLIBCXX_3.4.22` | manylinux_2_28 容器 GCC；硬校验，超出即拦截 |
| `vexdb_vector.so`（x86） | `GLIBCXX_3.4.26` | x86 host GCC 10.3 |
| `vexdb_vector.so`（ARM） | `GLIBCXX_3.4.26`（或更低） | ARM host GCC 9.3（Kylin V10 SP1 系统） |

> DuckDB 扩展通过 manylinux_2_28 容器构建，GLIBCXX 上限有硬校验（`GLIBCXX_MAX=3.4.22`）；PG 插件走 host GCC，当前未做同等校验，实际依赖 GLIBCXX_3.4.26。

CentOS 7 默认 libstdc++ 为 GLIBCXX_3.4.19，需手动升级：

```bash
conda install -c conda-forge libstdcxx-ng
```

---

## 已知不兼容场景

| 场景 | 状态 | 原因 |
|------|------|------|
| DuckDB ≠ v1.5.2 | ❌ 不支持 | 扩展 API 与 metadata footer 格式随版本变化 |
| PostgreSQL < 16 或 > 19 | ❌ 不支持 | 仅适配 PG 16-19 |
| ARMv8.0 及更早（如 Cortex-A53/A57） | ❌ 不支持 | 构建使用 `+lse` 扩展指令，旧核触发 SIGILL |
| Windows | ❌ 暂不支持 | 未适配 Windows 构建系统 |
| CentOS 7 默认环境 | ⚠️ 需手动升级 libstdc++ | GLIBCXX 版本低 |
| CMake 4.x 构建 DuckDB 扩展 | ❌ 不兼容 | CMake 4.x 与 DuckDB v1.5.2 `link_threads` 检查冲突；请用 3.28.x |
