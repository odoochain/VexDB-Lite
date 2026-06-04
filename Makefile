# DuckDB Community Extension 构建入口 —— 仅供 duckdb/community-extensions CI 使用。
#
# CI 流程(_extension_distribution.yml)会把本仓库 checkout 到根，并把 duckdb/ 与
# extension-ci-tools/ 单独 checkout 到同级目录，然后从这里跑 `make`。
#
# ⚠️ 本地开发/发版请用 ./build_duck.sh，不要用这个 Makefile —— 它依赖 CI 注入的
#    extension-ci-tools/ 与 duckdb/，本地默认不存在。
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# 扩展构建逻辑隔离在 community/ 子目录，根目录只留这个薄入口 + vcpkg.json。
EXT_NAME=vexdb_lite
EXT_CONFIG=${PROJ_DIR}community/extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile
