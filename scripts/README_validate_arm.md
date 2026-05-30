# ARM qps validation (Marina Petrova protocol)

Infrastructure for **statistically rigorous** ARM qps A/B comparison. Each
configuration runs **R independent runs**, every run does a full PG restart +
OS page cache drop + warmup + timed query loop. Aggregator uses Welch's t-test
+ 95% CI overlap + Cohen's d to gate significance — pure stdlib, no scipy.

## Files

| File | Purpose |
|---|---|
| `validate_arm_perf.sh` | Run R repetitions of one configuration on the remote ARM host. |
| `validate_arm_perf.py` | Aggregate `run_*.json` → `summary.md` + `summary.csv`; optional A/B vs baseline. |
| `validate_arm_topfixes.sh` | Drive the full Top-3 fix matrix (baseline-general / baseline-neon / fixA / fixB / fixC / fixD / fixE / all-fixes). |

## Quickstart

```bash
# Single configuration, 10 runs:
./scripts/validate_arm_perf.sh baseline-neon neon 10

# A/B vs an earlier baseline:
BASELINE_DIR=results/baseline-general_20260515T012345Z \
    ./scripts/validate_arm_perf.sh fix1-only neon 10

# Full Top-3 matrix end-to-end:
./scripts/validate_arm_topfixes.sh 10 1000 100
```

## Output layout

```
results/
  <label>_<UTC_TS>/
    run_01.json … run_10.json          # raw bench output (one per run)
    summary.md                         # human-readable; includes A/B gates
    summary.csv                        # machine-readable; raw + comparison
  topfixes_<UTC_TS>/
    REPORT.md                          # matrix-level rollup
```

Each `run_NN.json` carries at minimum:

```json
{
  "label": "fix1-only",
  "arch": "neon",
  "run_index": 3,
  "queries": 1000,
  "warmup": 100,
  "elapsed_sec": 1.72,
  "qps": 581.40,
  "recall_at_10": 0.9924,
  "pg_port": 5433,
  "dataset_dir": "/path/to/vexdb-validation/data/sift1m",
  "hostname": "kylin-arm",
  "ts_utc": "2026-05-15T08:12:31Z"
}
```

## Marina decision gates

To declare a configuration **significantly faster** than baseline, **all four**
gates must pass:

1. **Welch's t-test** p < 0.05 (two-sided)
2. **95% CIs do not overlap**
3. **Effect size** `|Δqps| / mean_baseline ≥ 5%`
4. **Recall@10 regression ≤ 0.5 pp**

If any gate fails, the summary verdict is `INCONCLUSIVE / NO-OP`. This is the
Marina rule: small wins that fail any gate get parked, never shipped as headline
numbers.

## Configuration (env vars)

| Variable | Default | Notes |
|---|---|---|
| `ARM_SSH` | `vexdb@<ARM_HOST>` | Prefer an SSH alias (`Host arm-test` in `~/.ssh/config`); credentials live in `~/.claude/projects/.../servers.md` (out-of-repo). |
| `ARM_PG_HOME` | `/path/to/pg19` | Remote PG install prefix. |
| `ARM_PGDATA` | `/path/to/pgdata-vexdb` | Remote PGDATA. |
| `ARM_BENCH_DIR` | `/path/to/vexdb-validation` | Houses `scripts/pg_sift1m_bench.py` + `data/sift1m`. |
| `ARM_PGPORT` | `5433` | Forwarded to bench script. |
| `ARM_DATASET` | `sift1m` | Selects subdir under `data/`. |
| `ARM_REPO_DIR` | `/path/to/vexdb_lite` | Used by `validate_arm_topfixes.sh` to checkout refs. |
| `BASELINE_DIR` | _(unset)_ | Optional baseline `results/...` dir to compare against. |

## Each run does

1. `pg_ctl -m fast -w restart` → drops shared_buffers.
2. `sync` + `echo 3 > /proc/sys/vm/drop_caches` (needs passwordless sudo on remote).
3. Warmup `W` queries (default 100) — discarded.
4. Time `Q` queries (default 1000), emit JSON to stdout.

## Verifying syntax locally (macOS)

```bash
bash -n scripts/validate_arm_perf.sh
bash -n scripts/validate_arm_topfixes.sh
python3 -m py_compile scripts/validate_arm_perf.py
```

## Remote bench script — caller must provide

**The remote `$ARM_BENCH_DIR/scripts/pg_sift1m_bench.py` does not exist in any
repo today.** The original sunji-era script (on `aaa@…66`) has been retired,
and no replacement has been checked in. `validate_arm_perf.sh` performs a
preflight `test -x` against `$REMOTE_BENCH` and aborts with exit 4 if the
script is missing.

You have two options:

**(a) Write a minimal bench script** matching the JSON contract in §Output
layout. The validation pipeline only reads four fields per run:

- `qps`
- `recall_at_10`
- `label`
- `arch`

The script must accept `--pg-port --dataset-dir --queries --warmup --arch
--label --run-index --json-out -` and emit a single JSON object on stdout.

**(b) Point `ARM_BENCH_DIR`** at an existing driver (e.g. an
ann-benchmark/psycopg2 wrapper) and adapt that wrapper's stdout to the JSON
contract above (a tiny shim is usually enough).

## Provenance

Protocol prescribed by jury member **Marina Petrova — 微基准压测工程师**. Target effect size 1.5–2.0×
qps lift (580 → 870–1160) for the Top-3 ARM fixes (Fix C compile flags / Fix A
NEONv8 restart / Fix B HNSW visited bitmap pool).
