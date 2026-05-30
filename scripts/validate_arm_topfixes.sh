#!/usr/bin/env bash
# validate_arm_topfixes.sh
# -----------------------------------------------------------------------------
# Drive the full Top 3 ROI matrix (Marina protocol) across multiple git refs.
# Each matrix row = (label, arch, git_ref). For every row:
#   1. ssh to ARM host, fetch + checkout ref
#   2. rebuild vexdb_vector extension
#   3. invoke validate_arm_perf.sh
# After all rows complete, aggregate per-label summaries into results/REPORT.md
# with A/B comparisons referenced back to baseline-general.
#
# Usage:
#   ./validate_arm_topfixes.sh [runs=10] [queries=1000] [warmup=100]
#
# Env (same as validate_arm_perf.sh):
#   ARM_SSH, ARM_PG_HOME, ARM_PGDATA, ARM_BENCH_DIR, ARM_PGPORT, ARM_DATASET,
#   ARM_REPO_DIR    remote vexdb_lite checkout (default: /home/vexdb/vexdb_lite)
# -----------------------------------------------------------------------------
set -euo pipefail

RUNS="${1:-10}"
QUERIES="${2:-1000}"
WARMUP="${3:-100}"

ARM_SSH="${ARM_SSH:-vexdb@<ARM_HOST>}"
ARM_REPO_DIR="${ARM_REPO_DIR:-/home/vexdb/vexdb_lite}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
REPORT_DIR="$REPO_ROOT/results/topfixes_${TS}"
mkdir -p "$REPORT_DIR"

# (label, arch, git_ref) — git_ref is what we'd `git checkout` on the remote.
# Keep order: baselines first, then incremental fixes.
MATRIX=(
    "baseline-general|general|28206e2744"
    "baseline-neon|neon|f5c7640bc5"
    "fixC-only|neon|943d5b99ad"
    "fixA-only|neon|14ffb50c07"
    "fixD-only|neon|e27c10ba60"
    "fixB-only|neon|e4b1cc80e8"
    "fixE-only|neon|021afeac40"
    "all-fixes|neon|f5c7640bc5"
)

declare -A RESULT_DIRS

for row in "${MATRIX[@]}"; do
    IFS='|' read -r LABEL ARCH REF <<< "$row"
    echo "================================================================"
    echo "[topfixes] $LABEL  arch=$ARCH  ref=$REF"
    echo "================================================================"

    ssh "$ARM_SSH" "
        set -e
        cd $ARM_REPO_DIR
        git fetch --all --quiet
        git checkout --quiet $REF || { echo 'ref $REF missing — skipping'; exit 42; }
        git log -1 --oneline
        make -s clean >/dev/null
        make -s -j USE_PGXS=1 >/dev/null
        make -s install USE_PGXS=1 >/dev/null
    " || {
        rc=$?
        if [ "$rc" -eq 42 ]; then
            echo "[topfixes] SKIP $LABEL (ref absent)"
            continue
        fi
        echo "[topfixes] FAILED remote build for $LABEL (rc=$rc)" >&2
        exit "$rc"
    }

    BASELINE_ARG=""
    if [ "${LABEL}" != "baseline-general" ] && [ -n "${RESULT_DIRS[baseline-general]:-}" ]; then
        BASELINE_ARG="${RESULT_DIRS[baseline-general]}"
    fi

    out_dir="$(BASELINE_DIR="$BASELINE_ARG" "$SCRIPT_DIR/validate_arm_perf.sh" \
        "$LABEL" "$ARCH" "$RUNS" "$QUERIES" "$WARMUP" | tail -n 1)"
    RESULT_DIRS["$LABEL"]="$out_dir"
    echo "[topfixes] $LABEL -> $out_dir"
done

# Final report.
{
    echo "# ARM Top-3 Fix Matrix Report — $TS"
    echo ""
    echo "| label | arch | result dir | mean qps | 95% CI | vs baseline-general |"
    echo "|---|---|---|---:|---|---|"
    for row in "${MATRIX[@]}"; do
        IFS='|' read -r LABEL ARCH _ <<< "$row"
        dir="${RESULT_DIRS[$LABEL]:-}"
        if [ -z "$dir" ] || [ ! -f "$dir/summary.md" ]; then
            echo "| $LABEL | $ARCH | (skipped) | - | - | - |"
            continue
        fi
        mean=$(grep -E '^- mean qps' "$dir/summary.md" | head -n1 | sed -E 's/.*\*\*([0-9.]+)\*\*.*/\1/')
        ci=$(grep -E '^- 95% CI' "$dir/summary.md" | head -n1 | sed -E 's/^- 95% CI: //')
        delta=$(grep -E '^- delta qps' "$dir/summary.md" | head -n1 | sed -E 's/^- delta qps: //' || true)
        echo "| $LABEL | $ARCH | $(basename "$dir") | $mean | $ci | ${delta:--} |"
    done
    echo ""
    echo "_Each row uses Marina Petrova's protocol: R=$RUNS independent runs, PG restart + drop_caches + $WARMUP warmup + $QUERIES timed queries per run._"
} > "$REPORT_DIR/REPORT.md"

echo "[topfixes] aggregated report: $REPORT_DIR/REPORT.md"
