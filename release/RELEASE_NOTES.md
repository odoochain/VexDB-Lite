# VexDB-Lite v0.0.4

发版日期：2026-05-14

## 重要：v0.0.3 用户必须升级

v0.0.3 release 中的 `vex.duckdb_extension` **完全无法被 DuckDB 加载**——`LOAD '...'` 会报 `IO Error: The file is not a DuckDB extension. The metadata at the end of the file is invalid`。同时 `vexdb_vector.so` 被剥得太彻底，客户 gdb 抓不到 `file:line` 信息。

v0.0.4 解决了这两件事，并包含一批问题修复。任何在用 v0.0.3 的环境都需要替换。

---

## DuckDB 扩展 — footer 被 strip 截断（根因修复）

`vex.duckdb_extension` 在 ELF section 表之后追加了 534 字节的 metadata footer（由 DuckDB CMake 的 `append_metadata.cmake` 写入，含 `duckdb_signature` magic、平台 tag、版本号、签名占位）。release 流水线里 `strip --strip-unneeded` 会重写 ELF，**截掉所有 ELF 表之后的裸 bytes**，footer 就这么丢了。DuckDB loader 在加载时先读末尾 metadata 校验，footer 不存在直接拒收。

v0.0.4 把 strip 步骤改成 **strip → 重新调用 `append_metadata.cmake` 写回 footer**，新增 `bash build_duck.sh strip` 子命令统一处理。release.sh 的 `validate_duck` 现在硬性检查 `duckdb_signature` magic + 用官方 DuckDB v1.5.2 CLI 真 `LOAD` 一次作 smoke。下次再出破包会直接拦在发布前。

## PG 扩展 — split-debug，可独立分发的 debug symbols

`vexdb_vector.so` 改走标准 GNU binutils 三步流程（Fedora/Debian 的 -debuginfo / -dbgsym 包同样工作流）：

1. `objcopy --only-keep-debug` 抽出 `.debug_info` 到独立的 `vexdb_vector.so.debug` 文件
2. `strip --strip-unneeded` 主 .so 移除 debug 但保留 dynamic symbols
3. `objcopy --add-gnu-debuglink` 把 `.debug` 文件名 + CRC32 嵌进主 .so 的 `.gnu_debuglink` section

主 .so 跟 .debug 文件共享 `.note.gnu.build-id`，gdb 自动按 build-id 找配套 debug 文件。客户碰到 crash 时，下载对应 release 的 `vexdb_vector-debugsymbols-...tar.gz`，把 `.debug` 文件放到 `.so` 同目录或 `/usr/lib/debug/.build-id/` 下，gdb attach 立刻能拿到完整 file:line backtrace。

## 问题修复

| 问题 | 说明 |
|---|---|
| 二进制 `-f` 跑含语法错误的 SQL 产生 core | v1.5.2 移植 + checkpoint 序列化路径重构后不再复现；远程 CentOS 8 + macOS 验证通过 |
| 退出连接重进后 PQ 索引查询召回率从 ~98% 掉到 ~10% | compact-mode PQ reload 路径修复 + full reload 正确性修复 + search_layer 锁泄漏修复 |
| `CREATE INDEX ... USING GRAPH_INDEX(v, c1, c1)` 重复列建索引成功 | CREATE INDEX 时拒绝重复列和多余向量列 |
| 范围 / BETWEEN / 组合 AND / CTE+CROSS_PRODUCT 形态的 hybrid ANN 不走索引 | 优化器提升 LogicalGet.table_filters 到 VEX_INDEX_SCAN 上层，覆盖多种过滤组合形态 |

新增回归 spec：`tests/spec/duckdb/index/graph_index_hybrid_filter.yaml`（21 assertions）。全 spec 套件 106 用例 / 3912 assertions 通过。

## 兼容性

| 产物 | GLIBCXX 需求 | 兼容平台 |
|---|---|---|
| `vex.duckdb_extension` | ≤ 3.4.22（manylinux_2_28 toolchain 编译） | CentOS 8 / Kylin V10 SP1 / 任何 glibc ≥ 2.28 Linux |
| `vexdb_vector.so` | ≤ 3.4.21（host gcc 10.3） | 同上 |

CentOS 7 默认 libstdc++（GLIBCXX 3.4.19）不在兼容范围内，需要手动升级 libstdc++ 或使用 conda 环境。

## 安装

### DuckDB 扩展

```bash
tar -xzf vex-duckdb-linux-x86_64.tar.gz   # 或 aarch64
duckdb -unsigned
> LOAD '/path/to/vex.duckdb_extension';
> SELECT vex_version();
```

### PG 扩展

```bash
tar -xzf vexdb_vector-linux-x86_64-pg19.tar.gz
sudo install -m 755 vexdb_vector.so          $(pg_config --pkglibdir)/
sudo install -m 644 vexdb_vector.control     $(pg_config --sharedir)/extension/
sudo install -m 644 vexdb_vector--1.0.sql    $(pg_config --sharedir)/extension/

# postgresql.conf 加 shared_preload_libraries = 'vexdb_vector' 后重启
psql -c "CREATE EXTENSION vexdb_vector;"
```

### 客户调试 PG 扩展时拉 debug 配套

```bash
tar -xzf vexdb_vector-debugsymbols-linux-x86_64-pg19.tar.gz
sudo install -m 644 vexdb_vector.so.debug $(pg_config --pkglibdir)/
# gdb 在 attach 时按 build-id 自动加载，不需要别的配置
```

## 产物清单

| 文件 | 大小 | 内容 |
|---|---|---|
| `vex-duckdb-linux-x86_64.tar.gz` | 12 MB | `vex.duckdb_extension` |
| `vex-duckdb-linux-aarch64.tar.gz` | 9.9 MB | `vex.duckdb_extension` |
| `vexdb_vector-linux-x86_64-pg19.tar.gz` | 1.4 MB | `vexdb_vector.so` + `.control` + `.sql` |
| `vexdb_vector-linux-aarch64-pg19.tar.gz` | 531 KB | 同上 |
| `vexdb_vector-debugsymbols-linux-x86_64-pg19.tar.gz` | 297 KB | `vexdb_vector.so.debug` |
| `vexdb_vector-debugsymbols-linux-aarch64-pg19.tar.gz` | 63 KB | 同上 |
| `SHA256SUMS.txt` | — | 上述 6 个文件的 SHA256 |
