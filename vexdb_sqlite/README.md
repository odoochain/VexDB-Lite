# vexdb_sqlite — VexDB-Lite 的 SQLite 适配层

把 VexDB 自研图索引（GRAPH_INDEX / HNSW）以 **SQLite 虚拟表 + Shadow Table** 形态外放，补齐三态同源的 **Edge** 一极。对标 sqlite-vec（有过滤、无 ANN），目标是 SQLite 生态第一个「持久化 ANN 索引 + 过滤查询 + 端侧 + 宽松许可」的向量扩展。

> 完整计划：`docs/plans/2026-06-10_sqlite-adapter-v1-plan.md`
> 范围调研：`docs/research/2026-06-10_sqlite-v1-scope-reinvestigation.md`

## 当前进度：M0 骨架 ✅ → M1 距离层 ✅ → M2 虚拟表+暴力 KNN ✅

```sql
.load ./vexdb_lite              -- 桌面；移动端走静态注册

SELECT vexdb_l2_distance('[1,2,3]', '[4,5,6]');      -- 4 个距离函数 + vexdb_f32/vector_to_json

CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(embedding FLOAT[128], metric=cosine, m=16);
INSERT INTO idx(rowid, embedding) VALUES (1, :blob_or_json);
SELECT rowid, distance FROM idx WHERE embedding MATCH :query AND k = 10;  -- 暴力 KNN（M3 切 HNSW）
```

| 里程碑 | 验证 | 状态 |
|---|---|---|
| M0 双形态注册链路 | `m0_static_smoke` + CLI `.load` | ✅ arm64 + x86_64 |
| M1 距离层（common SIMD dispatch） | `m1_distance_smoke` + 跨引擎 800 组对照（`m1_cross_engine_check.py`，float64 真值仲裁）+ DuckDB 回归 111 cases | ✅ |
| M2 虚拟表（shadow table 持久化 + 暴力 KNN） | `m2_vtab_smoke`（KNN 正确性/事务回滚/关库重开/错误路径） | ✅ arm64 + x86_64 |
| M3 HNSW（GraphIndexCore）+ M3+ 并行建图 | — | 下一步 |

距离语义三 metric 统一 **lower = closer**（L2=sqrt、cosine=1-sim、ip=负内积），`ORDER BY distance ASC` 即最近优先。跨 ISA（NEON/SSE）允许 ~1e-6 级 float32 重排序分歧。

## 双形态分发（架构前置决策）

| 形态 | 适用 | 机制 |
|---|---|---|
| **静态注册**（默认） | 移动端 iOS/Android/WASM、可嵌入宿主 | amalgamation 静态链 + `vexdb_sqlite_register(db)` 或 `sqlite3_auto_extension`。`-DVEXDB_SQLITE_CORE=1` 直链真实 sqlite3 符号 |
| **loadable** `.so`/`.dylib` | 桌面/服务端 | 运行时 `.load ./vexdb_lite sqlite3_vexdblite_init`，经 `sqlite3ext.h` 间接表 |

> iOS 系统 libsqlite3 禁扩展加载、WASM 不支持运行时 `.load` → 移动端**只能**走静态注册。故默认形态是静态注册，loadable 仅桌面附加。

## 目录

```
vexdb_sqlite/
├── CMakeLists.txt           # 双形态产出：vexdb_lite(.so/.dylib) + vexdb_lite_static(.a)
├── vendor_sqlite.sh         # 拉取官方 SQLite amalgamation（≥3.38，默认 3.45.3）
├── include/
│   ├── vexdb_sqlite.h        # 公共入口：register / loadable init
│   ├── vexdb_sqlite_internal.h  # loadable vs core 头切换
│   └── vtab/graph_index_vtab.h
├── src/
│   ├── vexdb_sqlite_init.cpp  # 入口（两形态汇聚）+ vexdb_version()
│   └── vtab/graph_index_vtab.cpp  # GRAPH_INDEX 虚拟表模块（M0 只读骨架）
├── test/m0_static_smoke.c    # 静态注册冒烟
└── third_party/sqlite/       # vendored amalgamation（gitignore，跑 vendor_sqlite.sh 获取）
```

> 模块名 `GRAPH_INDEX` = 三端共享的索引类型名（DuckDB `TYPE_NAME` / DuckDB+PG `index_info` 报告名均为 `GRAPH_INDEX`）。

## 构建

```bash
# 推荐（需 cmake）：
bash build_sqlite.sh test     # vendor + 配置 + 编双形态 + 跑 M0 冒烟
bash build_sqlite.sh vendor   # 仅拉取 amalgamation
bash build_sqlite.sh clean

# 无 cmake 时的手动 fallback（M0 时点快照，源文件清单以 CMakeLists.txt 为准）：
cd vexdb_sqlite && bash vendor_sqlite.sh
INC="-Iinclude -Ithird_party/sqlite"
clang -O1 -c third_party/sqlite/sqlite3.c -Ithird_party/sqlite -DSQLITE_ENABLE_LOAD_EXTENSION=1 -o build/sqlite3.o
clang++ -std=c++17 -DVEXDB_SQLITE_CORE=1 $INC -c src/vexdb_sqlite_init.cpp -o build/init_core.o
clang++ -std=c++17 -DVEXDB_SQLITE_CORE=1 $INC -c src/vtab/graph_index_vtab.cpp -o build/vtab_core.o
clang -DVEXDB_SQLITE_CORE=1 $INC -c test/m0_static_smoke.c -o build/smoke.o
clang++ build/smoke.o build/init_core.o build/vtab_core.o build/sqlite3.o -lpthread -o build/m0_static_smoke
./build/m0_static_smoke
```

> macOS 注意：本机 anaconda clang 默认 target 是 x86_64，与系统 arm64 sqlite3 不匹配。
> CMake 已自动对齐 host 架构；手动编 loadable 时按需加 `-arch arm64`。

## 路线（详见计划文档）

- **Stage A 核心**（M0✅ → M1 距离层 → M2 暴力搜索 → M3 HNSW + M3+ 并行 → M4 spec → M5 桌面发版）
- **Stage B** HybridIndex（过滤查询）
- **Stage C** 移动端（iOS/Android，WASM 可选）
