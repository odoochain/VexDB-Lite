# VexDB

**English** | **[中文](README.zh.md)**

`VexDB` currently contains two vector-index integrations that share the same core graph algorithm and distance stack:

- `vexdb_pg`: PostgreSQL extension `vexdb_vector`
- `vexdb_duckdb`: DuckDB extension `vex`

Shared core directories:

- `include/graph_index/`: graph index headers and shared HNSW logic
- `distance/`, `src/distance/`: distance functions, ISA dispatch, transform templates
- `vtl/`: shared template/container layer
- `vexdb_duckdb/`: DuckDB integration layer
- `src/`, `include/`, `sql/`: PostgreSQL integration layer

---

## 1. Components

### 1.1 PostgreSQL: `vexdb_vector`

Current functionality:

- `floatvector(N)` type
- Distance functions and operators:
  - `l2_distance` (`<->`)
  - `cosine_distance` (`<=>`)
  - `inner_product` (use `<~>` for negative inner product / MIPS)
- Scalar helpers: `vector_dims()`, `vector_norm()`, `l2_normalize()`, `vex_index_info()`
- `CREATE INDEX ... USING vexdb_graph`
- Index options: `m`, `ef_construction`, `parallel_workers` (parallel build), `quantizer` / `pq_m` (PQ)
- Product quantization (PQ) with `compact` mode
- Optimizer rewrite into an ANN Index Scan
- Shared-memory vector buffer cache and parallel index build
- Runtime settings: `vexdb_vector.ef_search`, `vexdb_vector.vec_architecture`

### 1.2 DuckDB: `vexdb_vector`

Current functionality:

- `GRAPH_INDEX` on `FLOAT[N]` vector columns
- Distance functions and operators:
  - `l2_distance` (`<->`)
  - `cosine_distance` (`<=>`)
  - `inner_product` (use `<~>` for negative inner product / MIPS)
  > Note: `<#>` is unavailable in DuckDB (`#` clashes with its comment syntax); use `<~>` for negative inner product — same meaning as in PG.
- Scalar helpers: `vector_dims()`, `l2_normalize()`, `vex_version()`, `vex_index_info()`
- `CREATE INDEX ... USING GRAPH_INDEX (vec [, metadata...])` with metadata filtering
- Index options: `m`, `ef_construction`, `parallel_workers` (parallel build), `quantizer` / `pq_m` (PQ)
- Product quantization (PQ) with `compact` mode
- Optimizer rewrite into `VEX_INDEX_SCAN`
- Vector buffer cache and parallel index build
- Runtime settings: `vex_ef_search`, `vex_brute_force_threshold`, `vex_pq_search_mode`, `vex_pq_refine_k_factor`

---

## 2. Capability Matrix

| Category | Feature | Description | vexdb-lite (open-source) | VexDB (commercial) |
|---|---|---|:---:|:---:|
| Graph Index | GRAPH_INDEX | In-house high-performance graph index, universal | ✅ | ✅ |
| Distance | Distance function dispatch | Inlined distance functions, compile-time optimized | ✅ | ✅ |
| Cache | Vector buffer | General vector cache, all scenarios | ✅ | ✅ |
| Cache | Bulk buffer | Full in-memory cache for max throughput | ❌ | ✅ |
| Cache | Async I/O cache | Accelerated disk-to-cache reads under memory pressure | ❌ | ✅ |
| Data types | floatvector | Standard float32 vector type | ✅ | ✅ |
| Data types | halfvector | Float16 vector type | 🟡 | ✅ |
| Data types | int8vector | Int8 vector type | 🟡 | ✅ |
| Quantization | PQ quantization | Maximum compression, QPS close to raw vectors | ✅ | ✅ |
| Quantization | RaBitQ quantization | Moderate compression, QPS better than raw vectors | 🟡 | ✅ |
| Quantization | Auto quantization | Background auto-enable, supports empty-table index build | ❌ | ✅ |
| Index | Async insert | Fast ingestion for write-heavy workloads | ❌ | ✅ |
| Index | Graph sharding | Large-scale vectors on small-memory machines | ❌ | ✅ |
| HA | Primary-replica HA | Synchronous replication and backup restore | ❌ | ✅ |
| Maintenance | Parallel vacuum | Parallel index cleanup and reclaim | ❌ | ✅ |

✅ Supported · 🟡 Coming soon · ❌ Not included in open-source edition

---

## 3. PostgreSQL Syntax Examples

### 3.1 Install and Create Table

```sql
CREATE EXTENSION vexdb_vector;

CREATE TABLE items (
    id  BIGSERIAL PRIMARY KEY,
    vec floatvector(128)
);

INSERT INTO items (vec) VALUES
    ('[0.10, 0.20, 0.30]'),
    ('[0.40, 0.50, 0.60]');
```

### 3.2 Build Index

```sql
CREATE INDEX idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (
    m = 16,
    ef_construction = 64
);
```

### 3.3 ANN Query

```sql
SET vexdb_vector.ef_search = 100;
SET enable_seqscan = off;

SELECT id, vec <-> '[0.15, 0.25, 0.35]' AS dist
FROM items
ORDER BY vec <-> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

### 3.4 Other Metrics

```sql
SELECT id
FROM items
ORDER BY vec <#> '[0.15, 0.25, 0.35]'
LIMIT 10;

SELECT id
FROM items
ORDER BY vec <=> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

---

## 4. DuckDB Syntax Examples

### 4.1 Load Extension

```sql
LOAD '/path/to/vex.duckdb_extension';
SELECT vex_version();
```

Typical Python usage:

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vex.duckdb_extension'")
```

### 4.2 Create Table and Index

```sql
CREATE TABLE items (
    id       INTEGER,
    category VARCHAR,
    vec      FLOAT[128]
);

CREATE INDEX idx_items_vec
ON items
USING GRAPH_INDEX (vec)
WITH (
    metric = 'l2',
    m = 16,
    ef_construction = 64
);
```

### 4.3 ANN Query

```sql
SET vexdb_vector.ef_search = 100;

SELECT id
FROM items
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 4.4 Filtered Index Example

```sql
CREATE INDEX idx_items_vec_meta
ON items
USING GRAPH_INDEX (vec, category);

SELECT id
FROM items
WHERE category = 'book'
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 4.5 Other Functions

```sql
SELECT inner_product([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT cosine_distance([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);
SELECT * FROM vex_index_info();
```

---

## 5. Build

### 5.1 Build the PostgreSQL Variant

### Dependencies

- PostgreSQL 16 ~ 19 (PG 16/17/18/19 supported; primary validation target is `19devel`)
- CMake
- C++17 compiler
- Boost headers

### Build PostgreSQL (release example)

```bash
cd /path/to/postgresql-19-source
./configure \
  --prefix=/opt/postgresql-19rel-install \
  --without-icu \
  --without-readline \
  --without-zlib \
  CFLAGS="-O3 -DNDEBUG"
make -j$(nproc)
make install
```

### Build `vexdb_vector`

```bash
cd /path/to/VexDB
mkdir -p build-pg19rel-release
cd build-pg19rel-release

export PG_CONFIG=/opt/postgresql-19rel-install/bin/pg_config
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
make install
```

### PostgreSQL Configuration

At minimum:

```conf
shared_preload_libraries = 'vexdb_vector'
```

Then restart PostgreSQL and run:

```sql
CREATE EXTENSION vexdb_vector;
```

---

### 5.2 Build the DuckDB Variant

**Recommended: use `build_duck.sh`** — it handles DuckDB clone, cmake configuration, compilation, and metadata processing in one command.

```bash
bash build_duck.sh setup   # First time: clone DuckDB v1.5.2 and cmake configure
bash build_duck.sh build   # Compile the extension (incremental)
```

Output: `build/duck/build/extension/vex/vex.duckdb_extension`

### Dependencies

- CMake 3.14+
- C++17 compiler (GCC 9+ or Clang 10+)
- Git

### Why a shell script and not plain cmake?

DuckDB extensions must be compiled inside DuckDB's source tree — you cannot run `cmake -B build vexdb_duckdb/` standalone. `build_duck.sh` automates:
1. Cloning DuckDB v1.5.2
2. Writing `extension_config_local.cmake` to register the vex extension
3. Running `cmake` + `cmake --build`
4. Appending the extension metadata footer (required by DuckDB's release format)

---

## 6. Running Tests

### DuckDB Extension Tests

```bash
bash build_duck.sh build          # Build the extension
bash tests/spec/_lib/docker/run_duckdb.sh test  # Run full spec tests (requires Docker)
```

### PostgreSQL Plugin Tests

```bash
bash tests/spec/_lib/docker/run_pg.sh test      # Run PG spec tests (requires Docker + PG19)
```

Tests are driven by a YAML spec DSL; test files live under `tests/spec/`.

---

## 7. Benchmark Results

Dataset: SIFT-1M 128-dim, `m=16`, `ef_construction=128`. Columns: `QPS (reads=1)` / `QPS (reads=16)` / `Recall@10`.

Test environment: Intel Core Ultra 7-265K (20c/20t, 3.9 GHz) / 16 GB DDR5 / x86_64 Linux

### 7.1 Comparison with pgvector / VSS (x86_64)

**ef_search = 50**

| System | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 507.9 | 7153.5 | 96.22% |
| **vexdb_vector (PostgreSQL)** | **994.7** | **12084.6** | 95.97% |
| **vexdb_vector (DuckDB)** | **717.5** | **8667.8** | 95.06% |
| duckdb-vss | 496.1 | 5360.9 | 94.07% |

**ef_search = 100**

| System | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 313.4 | 4272.5 | 98.82% |
| **vexdb_vector (PostgreSQL)** | **618.5** | **7883.1** | 98.62% |
| **vexdb_vector (DuckDB)** | **547.2** | **5379.1** | 98.40% |
| duckdb-vss | 405.2 | 4433.3 | 98.04% |

**ef_search = 200**

| System | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 193.1 | 2694.1 | 99.66% |
| **vexdb_vector (PostgreSQL)** | **421.3** | **5038.0** | 99.58% |
| **vexdb_vector (DuckDB)** | **383.6** | **4298.8** | 99.53% |
| duckdb-vss | 321.9 | 3809.3 | 99.42% |

---

## 8. Known Limitations

See [docs/known-limitations/](docs/known-limitations/) for the full list.

### PostgreSQL

- Supports PostgreSQL 16 ~ 19; primary validation target is PostgreSQL 19
- ARM PG SIMD is not fully wired back yet; current state prioritizes correctness/buildability
- WAL/quantizer work is still incomplete compared to the full roadmap

### DuckDB

- Current focus is `GRAPH_INDEX`, optimizer integration, and shared-algorithm alignment
- Some accepted options such as `threads` and `pq_m` are currently compatibility placeholders on parts of the path
- ARM Duck builds also currently rely on `GENERAL` distance dispatch

---

## 9. Where To Look Next

- PostgreSQL implementation: `src/`, `include/`, `sql/`
- DuckDB implementation: [vexdb_duckdb/README.md](vexdb_duckdb/README.md) and `vexdb_duckdb/`

---

## Community

| Channel | Description |
|---|---|
| [GitHub Issues](https://github.com/VexDB-THU/VexDB-Lite/issues) | Bug reports and feature requests |
| [GitHub Discussions](https://github.com/VexDB-THU/VexDB-Lite/discussions) | Questions, proposals and general discussion |
| [Discord](https://discord.gg/Ge4kaFak) | Real-time chat and Q&A |
| WeChat Group | Scan QR code at [vexdb.com/community](https://vexdb.com/community) · Chinese community |

---

## License

MIT License. See [LICENSE](LICENSE).
