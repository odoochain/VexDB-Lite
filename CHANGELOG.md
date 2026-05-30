# Changelog

## [v0.0.11] - 2026-05-29
### Fixed
- 修复 DuckDB 并行 build 时 MemStore 并发崩溃；build-only 加锁，保查询路径 QPS 不受影响
- 修复 xlog dead lock 及 vacuum 崩溃问题（diskvector 路径）
- 修复 insert dead lock，清理 bulkbuffer 冗余代码
- 修复 PG 并行 memory build worker 中 `PG_TRY/PG_CATCH` 跨线程 longjmp 崩溃
- 禁用 build 过程中的 `ps` 系统调用（避免部分 Linux 环境权限问题）
### Changed
- 代码清理：清除修复过程中的临时注释，整理目录结构
- PG 支持版本范围更新为 16–19（新增 PG 16/17/18 兼容）
- 统一插件命名：`vexdb_vector`（PG）/ `vexdb_duckdb`（DuckDB）/ `vexdb_pg`

## [v0.0.10] - 2026-05-25
### Fixed
- 修复 PG `PointExtensionContext::ps()` 多线程路径 ps_mutex 未真正加锁
- 修复 PG 并行 build 的多处 race condition：`get_neighbors_data`/`select_neighbors`/`mark order` pw=8 收敛
- 修复 `backward select_neighbors` 跨 stripe 读邻居值时缺失 `is_ready` 屏障
- 修复 parallel build ready atomic bitmap 缺失导致 pw=4 recall 偏低
- 修复 parallel build 漏 publish 屏障（pw=2 暴跌 83% 修到 ~98%）
- 修复 `search_layer` base 层漏邻居：evaluate 即入 result set，SIFT1M recall 提升约 0.9pp
- 修复 `mwm < 1GB` 时错误提前退出 disk 路径
- 修复 single-thread build `lock_elem` 数组未随 chunk 扩展导致越界
- 修复 `SharedAllocSet` 出的 chunk 走专用 free 路径，绕开 pfree hdrmask 解析问题
- 修复 `vex_index_info().max_level` 返回值不准
- 修复 `backward select_neighbors` dist_cache key 碰撞（disk path 主因）
- 修复 PG INT reloption 拒绝浮点/科学计数字面量
- 修复 `quantizer` reloption 格式校验
- 修复 DuckDB `WITH threads` 缺省走 TaskScheduler 线程数
### Added
- vendored patched boost 1.91（`thirdparties/boost/`），修复 fork 多进程下 `concurrent_flat_map` 锁聚集问题
- PQ centroids backup/restore 回归 spec
- PG 索引构建进度追踪
### Changed
- `PointExtensionContext::ps()` 只在 build 多线程路径加锁，降低 cluster 路径锁开销
- 移除异步读取路径

## [v0.0.9] - 2026-05-24
### Fixed
- 修复 `PointExtensionContext::ps()` 真正 engage ps_mutex
### Added
- 补齐 plugin-pg / plugin-duck 多条回归 spec

## [v0.0.8] - 2026-05-24
### Fixed
- 修复 `PointExtensionContext::ps()` ps_mutex 初版修复

## [v0.0.7] - 2026-05-22
### Fixed
- 修复 PG 并行 build 多处独立 race：publish 屏障、ready bitmap、邻居读 is_ready 屏障
- 修复 `search_layer` 漏邻居导致 recall 偏低
- 修复 PG `mwm < 1GB` 提前退出 disk 的判断错误
- 修复 `out_of_memory` safety margin 对 parallel 路径误计
- 修复打包排除 macOS `._*` 和 `.DS_Store`
### Added
- 对应回归 spec 补全
- DuckDB `vex_index_info()` 跨维度与字段正确性回归 spec
### Changed
- 仓库重构为三层结构：`common/` + `vexdb_pg/` + `vexdb_duckdb/`（共享算法与适配层解耦）

## [v0.0.6] - 2026-05-18
### Fixed
- 修复 DuckDB GRAPH_INDEX 持久化跨进程 reload SIGSEGV（`m != 16` 必踩）
  - `SerializeToWAL` 写顺序与 ART 对齐，补写 `info.buffers`，修复 INVALID_BLOCK reload 路径
  - 覆盖 m = {4, 8, 12, 32} 的 build → CHECKPOINT → restart → ANN 完整路径
### Added
- `graph_index_persistence_m_grid` spec：覆盖 m != 16 的持久化 reload 回归

## [v0.0.5] - 2026-05-16
### Fixed
- ARM aarch64：修复 `linux_arm644` footer padding bug，修复 NEONV8 undefined symbol，DuckDB extension 可正常 LOAD
- PG 并行构建 4 处独立 hazard：reltuples 负值越界、DSM fallback 未初始化 MemStore、0 worker 双重 emplace、VecBufferLoc quota 计算错误
- DuckDB 优化器：`ORDER BY dist LIMIT k`（无 OFFSET）错走 SEQ_SCAN 而非 VEX_INDEX_SCAN
### Added
- `vex-duckdb-debugsymbols-linux-*.tar.gz` debug symbol 产物
- spec 测试通过 `release.sh test` 集成到发版流程
### Changed
- ARM Kirin 9000C 向量搜索性能提升约 27000 倍（SIMD 路径修复 + visited pool + distance prefetch）
- `release.sh` 改为 rsync 增量同步，首次约 5 min，增量变更秒级

---

## 早期版本

### [v0.0.4] - 2026-05-14
DuckDB extension footer 修复 + PG split-debug + 多项 bug 修复

### [v0.0.3] - 2026-05-14
修复 GLIBCXX 兼容性问题

### [v0.0.2] - 2026-05-14
初期功能完善

### [v0.0.1] - 2026-03-24
VexDB-Lite 预览版首次发布
