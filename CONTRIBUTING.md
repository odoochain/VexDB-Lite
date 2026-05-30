# Contributing to VexDB-Lite

## 开发环境

- C++17，GCC 9+ 或 Clang 10+
- CMake 3.14+
- Python 3.8+（运行 spec 测试）
- Docker（运行完整测试套件）

## 构建

```bash
# DuckDB 扩展
bash build_duck.sh setup   # 首次：拉取 DuckDB 源码
bash build_duck.sh build   # 编译扩展

# PostgreSQL 插件
export PG_CONFIG=/path/to/pg_config
cmake -B build/pg vexdb_pg/ && make -C build/pg -j$(nproc)
```

## 运行测试

```bash
bash tests/spec/_lib/docker/run_duckdb.sh test   # DuckDB spec（需 Docker）
bash tests/spec/_lib/docker/run_pg.sh test        # PG spec（需 Docker + PG19）
```

## 代码规范

- 算法层（`common/`）使用 snake_case
- DuckDB/PG 适配层使用 PascalCase
- 新功能必须附带对应的 spec 测试（`tests/spec/`）
- Commit message 使用约定式提交（`feat:`、`fix:`、`chore:` 等前缀）

## 提交 PR

1. Fork 本仓库，基于 `dev` 分支创建特性分支
2. 确保 `bash tests/spec/_lib/docker/run_duckdb.sh test` 全部通过
3. 提交 PR 到 `dev` 分支，描述中说明改动目的

## 报告 Bug

请在 GitHub Issues 中提交，注明：
- 操作系统和架构（x86_64 / aarch64）
- 数据库版本（DuckDB 1.5.2 / PG 16-19）
- 复现步骤和期望行为
