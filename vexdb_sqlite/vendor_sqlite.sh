#!/bin/bash
# 拉取官方 SQLite amalgamation 到 third_party/sqlite/。
# amalgamation（sqlite3.c/.h + sqlite3ext.h）是双形态构建的依赖：
#   - loadable 扩展只需 sqlite3ext.h（也可用系统头）
#   - 移动端/静态注册形态必须 sqlite3.c（编进宿主，无运行时 .load）
# 体积大不入库，CI/开发者各自跑本脚本重现。
set -e

# 版本基线 ≥3.38.0（xBestIndex 感知 LIMIT/IN 约束的最低版本，HybridIndex 依赖）。
# 默认锁 3.45.3 (2024) — 含全部所需 vtab API。
SQLITE_YEAR="${SQLITE_YEAR:-2024}"
SQLITE_AMALG_VERSION="${SQLITE_AMALG_VERSION:-3450300}"

DIR="$(cd "$(dirname "$0")" && pwd)/third_party/sqlite"
URL="https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_AMALG_VERSION}.zip"

mkdir -p "$DIR"
echo "[vendor] 下载 $URL"
curl -sSL --max-time 60 -o /tmp/vexdb-sqlite-amalg.zip "$URL"
unzip -o -j /tmp/vexdb-sqlite-amalg.zip -d "$DIR"
rm -f /tmp/vexdb-sqlite-amalg.zip

echo "[vendor] 完成 → $DIR"
ls -la "$DIR"
grep -m1 "SQLITE_VERSION " "$DIR/sqlite3.h" || true
