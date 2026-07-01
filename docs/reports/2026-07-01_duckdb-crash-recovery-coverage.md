# DuckDB GRAPH_INDEX Crash Recovery Coverage

Date: 2026-07-01

## Scope

Added a hard-kill recovery harness for DuckDB `GRAPH_INDEX` that runs each scenario in a child process, kills it with `SIGKILL`, reopens the database, and validates exact table scan results plus the expected index-scan behavior.

Command:

```bash
bash build_duck.sh crash-recovery
```

Multi-version command:

```bash
DUCKDB_VERSIONS="v1.5.0 v1.5.1 v1.5.2 v1.5.3" bash build_duck.sh crash-recovery-matrix
```

Covered scenarios:

- Kill during `CREATE INDEX`
- Kill before transaction `COMMIT`
- Kill after transaction `COMMIT` before clean close/checkpoint
- Kill after delete-only `COMMIT` before clean close/checkpoint
- Kill after cosine `UPDATE` `COMMIT` before clean close/checkpoint
- Kill after compact/PQ `UPDATE` `COMMIT` before clean close/checkpoint
- Kill during `CHECKPOINT` / index serialization
- Repeated crash/recovery cycles

Physical torn WAL / half-write faults are not covered by this harness; that needs storage-layer fault injection.

## Result

The harness exposed a real DuckDB-side persistence gap:

- After `COMMIT`, exact scan sees the committed row, but `VEXDB_INDEX_SCAN` still returns the old nearest row.
- During interrupted `CHECKPOINT`, exact scan sees the committed row, but `VEXDB_INDEX_SCAN` still returns the old nearest row.

Observed diagnostics:

```text
kill after transaction COMMIT:
  exact scan got 6001
  index scan got 5000
  index_info: node_count=5000, row_id_map_size=5000

kill during CHECKPOINT/index serialization:
  exact scan got 65001
  index scan got 60000
  index_info: node_count=60000, row_id_map_size=60000
```

This means the table WAL recovery is ahead of the graph index state in these crash windows.

The implemented fix is conservative: before the optimizer replaces an exact plan with `VEXDB_INDEX_SCAN`, it scans live table rowids for the indexed vector column and compares that coverage with the persisted graph index:

- live row count
- live rowid upper bound
- live rowid-set checksum
- vector checksum, with table-side cosine vectors normalized before hashing
- persisted raw-vector checksum for compact/PQ indexes, because the raw vector tier is released
- PQ code checksum only for older compact/PQ indexes that do not have persisted raw-vector checksums

If coverage is stale, the optimizer keeps DuckDB's exact scan plan instead of using stale ANN rows. Cosine indexes compare normalized table vectors with the graph's normalized representation, so same-rowid cosine updates after crash are also covered. Compact/PQ indexes are counted from their PQ row order instead of the released raw-vector tier, so clean compact indexes are not disabled merely because raw vectors are absent.

This does not implement full GRAPH_INDEX WAL replay or automatic index rebuild. It prevents wrong ANN results by bypassing a stale persisted index until the index is recreated/rebuilt.

Compact/PQ note: newly written compact/PQ indexes persist per-row raw-vector coverage hashes alongside PQ codes, so same-rowid vector-content checks do not rely on lossy PQ code equality. Older compact/PQ index metadata without that field falls back to PQ code checksums and is therefore less strong; rebuilding the index writes the stronger checksum field.

## DuckDB Path Notes

In DuckDB v1.5.2, ordinary `ReplayInsert()` replays into table storage through `LocalWALAppend()` and does not update or buffer secondary index changes. The index buffering path exists for `ReplayRowGroupData()`, but not for ordinary transaction insert WAL records. This explains why the graph index can remain at the checkpointed state after a hard crash even though table data is recovered.

Cross-version source check:

- `v1.5.0`
- `v1.5.1`
- `v1.5.2`
- `v1.5.3`
- `v1.5.4`

All checked tags have the same ordinary `ReplayInsert()` shape: deserialize chunk, call `storage.LocalWALAppend(...)`, and return without updating or buffering secondary index changes. `LocalWALAppend()` likewise only initializes local append, appends to table storage, sets `index_append_mode = INSERT_DUPLICATES`, and finalizes append.

Local runtime note: the existing local `v1.5.0` build tree contains stale x86_64 static libraries, so the harness could not be linked on the current arm64 machine without a clean rebuild. The source-level finding still applies across the listed tags.

## Verification

Commands run after the fix:

```bash
bash build_duck.sh bin
bash build_duck.sh crash-recovery
bash tests/spec/_lib/docker/run_duckdb.sh test '*optimizer_extended*,*optimizer_plan*,*bf_threshold_explain*,*subquery_optimization*'
./build/duck/v1.5.2/build/test/unittest '/Users/Four/PersonalProjects/vexdb_lite/vexdb_duckdb/test/sql/vex/spec_run/index__graph_index_pq_compact.test'
./build/duck/v1.5.2/build/test/unittest '/Users/Four/PersonalProjects/vexdb_lite/vexdb_duckdb/test/sql/vex/spec_run/index__graph_index_pq_compact_reload.test'
bash tests/spec/_lib/docker/run_duckdb.sh test
```

Results:

- `crash_recovery: ok`
- hard-kill compact/PQ update scenario passed: recovery either uses a correctly replayed index or an exact fallback, and returns the recovered vector
- compact/PQ targeted specs: `graph_index_pq_compact` and `graph_index_pq_compact_reload` passed
- targeted optimizer regression: 4 test cases, 190 assertions passed
- full DuckDB spec: 113 test cases, 4056 assertions passed

Build notes: the macOS build still emits existing warnings from `common/vtl/internal/expr.hpp` using `sprintf`, C++20 structured-binding capture warnings in `common/include/graph_index/graph_index_algorithm.h`, and local missing Homebrew search-path linker warnings for zlib/bzip2. These are pre-existing warnings, not introduced by the crash-recovery fix.

## Implication

The cautious production statement is still partly justified for the DuckDB adapter: VexDB-Lite now avoids known stale-index wrong-result windows by falling back to exact scan, but it still does not have full secondary-index WAL recovery/replay. Treat this as a correctness fallback, not proof that DuckDB-side GRAPH_INDEX crash-safety is as mature as PostgreSQL-side WAL integration.
