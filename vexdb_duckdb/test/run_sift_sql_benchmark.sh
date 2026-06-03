#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DUCKDB_BUILD_DIR="${1:-/tmp/vexdb_duckdb-build}"
DATASET="${2:-both}"
# VEXDB_ROOT: root of the vexdb_lite checkout (defaults to two levels up from this script).
VEXDB_ROOT="${VEXDB_ROOT:-$ROOT_DIR}"
DATA_DIR="${3:-$VEXDB_ROOT/vexdb_duckdb/test/benchmark/data}"
EXTENSION_PATH="${4:-$DUCKDB_BUILD_DIR/extension/vexdb_lite/vexdb_lite.duckdb_extension}"
BENCH_SRC="$ROOT_DIR/vexdb_duckdb/test/benchmark/vex_sift_sql_benchmark.cpp"
BENCH_BIN="${5:-$DUCKDB_BUILD_DIR/vex_sift_sql_benchmark}"
CXX_BIN="${CXX:-c++}"

if [[ ! -f "$BENCH_SRC" ]]; then
  echo "missing benchmark source: $BENCH_SRC" >&2
  exit 2
fi
if [[ ! -f "$DUCKDB_BUILD_DIR/src/libduckdb_static.a" ]]; then
  echo "missing duckdb static lib: $DUCKDB_BUILD_DIR/src/libduckdb_static.a" >&2
  exit 2
fi
if [[ ! -f "$DUCKDB_BUILD_DIR/extension/libdummy_static_extension_loader.a" ]]; then
  echo "missing dummy loader: $DUCKDB_BUILD_DIR/extension/libdummy_static_extension_loader.a" >&2
  exit 2
fi
if [[ ! -f "$DUCKDB_BUILD_DIR/extension/core_functions/libcore_functions_extension.a" ]]; then
  echo "missing core_functions static lib: $DUCKDB_BUILD_DIR/extension/core_functions/libcore_functions_extension.a" >&2
  exit 2
fi
if [[ ! -d "$DATA_DIR" ]]; then
  echo "missing benchmark data dir: $DATA_DIR" >&2
  exit 2
fi
if [[ ! -f "$EXTENSION_PATH" ]]; then
  echo "missing extension: $EXTENSION_PATH" >&2
  exit 2
fi

mkdir -p "$(dirname "$BENCH_BIN")"

# DUCKDB_SRC: path to the DuckDB source tree (contains src/include).
# Example: export DUCKDB_SRC=/path/to/duckdb
DUCKDB_SRC="${DUCKDB_SRC:-}"

"$CXX_BIN" -std=c++17 -O2 \
  "$BENCH_SRC" \
  ${DUCKDB_SRC:+-I"$DUCKDB_SRC/src/include"} \
  ${DUCKDB_SRC:+-I"$DUCKDB_SRC/extension/core_functions/include"} \
  "$DUCKDB_BUILD_DIR/src/libduckdb_static.a" \
  "$DUCKDB_BUILD_DIR/extension/libdummy_static_extension_loader.a" \
  "$DUCKDB_BUILD_DIR/extension/core_functions/libcore_functions_extension.a" \
  -o "$BENCH_BIN"

"$BENCH_BIN" "$DATA_DIR" "$DATASET" "$EXTENSION_PATH"
