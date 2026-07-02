# VexDB 文档

**English** | 中文

VexDB 是一款高性能向量检索插件，提供 PostgreSQL（`vexdb_lite`）和 DuckDB（`vexdb_lite`）两种适配形式，共享同一套自研图索引算法内核。

---

## 文档目录

| 文档 | 内容 |
|------|------|
| [编译构建](build-guide.md) | 从源码编译 PG/DuckDB 插件、发版打包流程 |
| [功能文档](features.md) | 向量类型、距离函数、图索引参数、PQ 量化、运行配置 |
| [数据库适配版本](compatibility.md) | 支持的数据库版本、操作系统、CPU 架构矩阵 |
| [README 性能对比](../README.md#5-测试结果) | SIFT1M 基准、与 pgvector / VSS 对比结果 |

---

## 快速导航

### 我想用 PostgreSQL 插件
1. 查看 [兼容性表](compatibility.md) 确认 PG 版本
2. 下载 Release 中的 `vexdb_lite.so` 并参考 [功能文档](features.md#postgresql-插件) 建表

### 我想用 DuckDB 扩展
1. 查看 [兼容性表](compatibility.md) 确认 DuckDB 版本
2. 下载 `vexdb_lite.duckdb_extension` 并参考 [功能文档](features.md#duckdb-扩展)

### 我想从源码编译
参考 [编译构建指南](build-guide.md)

### 我想了解性能
参考 [README 性能对比](../README.md#5-测试结果)
