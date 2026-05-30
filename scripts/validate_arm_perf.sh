#!/usr/bin/env bash
# validate_arm_perf.sh
# -----------------------------------------------------------------------------
# ARM qps validation runner (Marina Petrova protocol).
# Drives R independent runs on a remote ARM host. Each run:
#   1. systemctl/pg_ctl restart Postgres (cold cache)
#   2. drop OS page cache (echo 3 > /proc/sys/vm/drop_caches)
#   3. warmup W queries (results discarded)
#   4. time Q queries -> emit JSON
# Results land in results/<label>_<ts>/run_NN.json and are aggregated by
# validate_arm_perf.py into summary.md + summary.csv.
#
# Usage:
#   ./validate_arm_perf.sh <label> <arch> [runs=10] [queries=1000] [warmup=100]
#
# Environment:
#   ARM_SSH         ssh target (default: vexdb@<ARM_HOST> or ssh alias)
#   ARM_PG_HOME     remote PG install dir         (default: /path/to/pg19)
#   ARM_PGDATA      remote PGDATA                 (default: /path/to/pgdata-vexdb)
#   ARM_BENCH_DIR   remote validation root        (default: /path/to/vexdb-validation)
#   ARM_PGPORT      remote PG port                (default: 5433)
#   ARM_DATASET     dataset name                  (default: sift1m)
#   BASELINE_DIR    optional baseline results dir for A/B comparison
# -----------------------------------------------------------------------------
set -euo pipefail

LABEL="${1:?label required (e.g. baseline-neon, fix1-only)}"
ARCH="${2:?arch required (e.g. neon, general, sve2)}"
RUNS="${3:-10}"
QUERIES="${4:-1000}"
WARMUP="${5:-100}"

ARM_SSH="${ARM_SSH:-vexdb@<ARM_HOST>}"
ARM_PG_HOME="${ARM_PG_HOME:-/path/to/pg19}"
ARM_PGDATA="${ARM_PGDATA:-/path/to/pgdata-vexdb}"
ARM_BENCH_DIR="${ARM_BENCH_DIR:-/path/to/vexdb-validation}"
ARM_PGPORT="${ARM_PGPORT:-5433}"
ARM_DATASET="${ARM_DATASET:-sift1m}"
BASELINE_DIR="${BASELINE_DIR:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="$REPO_ROOT/results/${LABEL}_${TS}"
mkdir -p "$OUT_DIR"

echo "[validate_arm_perf] label=$LABEL arch=$ARCH runs=$RUNS queries=$QUERIES warmup=$WARMUP"
echo "[validate_arm_perf] ssh=$ARM_SSH dataset=$ARM_DATASET out=$OUT_DIR"

# Sanity-probe ssh.
if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$ARM_SSH" 'uname -a' >/dev/null 2>&1; then
    echo "[validate_arm_perf] ERROR: cannot ssh $ARM_SSH (BatchMode=yes). Set ARM_SSH alias or push ssh key." >&2
    exit 2
fi

REMOTE_BENCH="$ARM_BENCH_DIR/scripts/pg_sift1m_bench.py"
REMOTE_DATA="$ARM_BENCH_DIR/data/$ARM_DATASET"

# Preflight: the remote bench script must actually exist + be executable by
# python3. The sunji-era script that this harness was originally written
# against has been retired, so without this check the loop below silently
# produces empty run_*.json on every iteration.
if ! ssh -o BatchMode=yes "$ARM_SSH" "test -x $REMOTE_BENCH || test -r $REMOTE_BENCH" 2>/dev/null; then
    cat >&2 <<EOF
[validate_arm_perf] ERROR: remote bench script not found at $REMOTE_BENCH on $ARM_SSH
This harness relies on a sunji-era script that has been retired.
Either:
  (a) write a bench script matching the JSON contract in
      scripts/README_validate_arm.md §"Output layout"
  (b) point ARM_BENCH_DIR to an existing ann-benchmark/psycopg2 driver
See docs/review/2026-05-15_code-review-aggregate.md for context.
EOF
    exit 4
fi

for i in $(seq 1 "$RUNS"); do
    RUN_TAG="$(printf 'run_%02d' "$i")"
    echo "[validate_arm_perf] === $RUN_TAG / $RUNS ==="

    # 1. restart PG (clean shared_buffers).
    # 2. drop OS page cache (needs sudo NOPASSWD on remote).
    # 3. run bench, emit JSON to stdout.
    ssh "$ARM_SSH" "
        set -e
        $ARM_PG_HOME/bin/pg_ctl -D $ARM_PGDATA -m fast -w restart >/dev/null
        sync
        if sudo -n true 2>/dev/null; then
            sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' || true
        fi
        sleep 2
        python3 $REMOTE_BENCH \\
            --pg-port $ARM_PGPORT \\
            --dataset-dir $REMOTE_DATA \\
            --queries $QUERIES \\
            --warmup $WARMUP \\
            --arch $ARCH \\
            --label $LABEL \\
            --run-index $i \\
            --json-out -
    " > "$OUT_DIR/$RUN_TAG.json"

    if [ ! -s "$OUT_DIR/$RUN_TAG.json" ]; then
        echo "[validate_arm_perf] ERROR: $RUN_TAG produced empty JSON" >&2
        exit 3
    fi
done

# Aggregate.
AGG_ARGS=(--results-dir "$OUT_DIR" --label "$LABEL" --arch "$ARCH")
if [ -n "$BASELINE_DIR" ]; then
    AGG_ARGS+=(--baseline-dir "$BASELINE_DIR")
fi
python3 "$SCRIPT_DIR/validate_arm_perf.py" "${AGG_ARGS[@]}"

echo "[validate_arm_perf] done. summary at $OUT_DIR/summary.md"
echo "$OUT_DIR"
