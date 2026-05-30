# Hybrid Search & Filtered ANN: Industry Approaches Research

**Date:** 2026-03-21
**Scope:** Industry approaches for combining scalar/metadata filtering with vector similarity search

---

## Table of Contents

1. [The Core Problem](#1-the-core-problem)
2. [Pre-Filtering](#2-pre-filtering)
3. [Post-Filtering](#3-post-filtering)
4. [Single-Stage / Integrated Approaches](#4-single-stage--integrated-approaches)
5. [IVF-Based Hybrid Approaches](#5-ivf-based-hybrid-approaches)
6. [Geometric Transformation / Filter Fusion](#6-geometric-transformation--filter-fusion)
7. [Graph-Based Approaches with Attribute-Aware Navigation](#7-graph-based-approaches)
8. [Relaxed Monotonicity (VBASE)](#8-relaxed-monotonicity-vbase)
9. [Multi-Index Collection Approaches (SIEVE)](#9-multi-index-collection-sieve)
10. [Range-Filtered Search (UNIFY)](#10-range-filtered-search-unify)
11. [Summary Comparison](#11-summary-comparison)
12. [Relevance to VexDB-Lite](#12-relevance-to-vexdb-lite)

---

## 1. The Core Problem

Unlike traditional RDBMS where adding a WHERE clause speeds up queries by pruning the search space, adding a filter to a vector search often **slows it down**. This fundamental paradox drives all the research in this area.

### Why Filtering is Hard for Vector Search

1. **Index mismatch**: ANN indexes optimize for proximity in high-dimensional space; they have no native boolean predicate structures.
2. **Graph fragmentation**: Removing nodes from HNSW breaks connectivity. Unlike B-tree invariants, HNSW has no self-repair mechanism for arbitrary node removal.
3. **No composite indexing**: There is no clean way to jointly index (vector, metadata) as elegantly as relational composite indexes.
4. **Approximation vs. exactness**: ANN trades recall for speed; filters demand exact matching. These are conflicting objectives.
5. **Join-like complexity**: Combining similarity with predicates mirrors expensive relational joins.

### The Two Failure Modes

- **Narrow filter problem** (low selectivity, few matches): Post-filtering fails because top-k ANN results may contain zero or very few items matching the filter. Even aggressive oversampling (increasing k) may not help.
- **Wide filter problem** (high selectivity, many matches): Pre-filtering is wasteful because filtering passes most items, making it essentially equivalent to unfiltered search but with overhead of the filtering step.

The ideal system must handle the full selectivity spectrum gracefully.

---

## 2. Pre-Filtering (Filter-Then-Search)

### How It Works

1. Evaluate scalar predicates first using a metadata/payload index (inverted index, B-tree, bitmap, etc.)
2. Identify the subset of vectors matching the filter
3. Run ANN search (or brute-force kNN) only on the filtered subset

### Pros

- **Guaranteed correctness**: All results match the filter; no false negatives from filtering
- **Simple to implement**: Reuses existing metadata indexes + vector indexes
- **Optimal for very narrow filters**: When the filtered set is tiny, brute-force on the subset is fast

### Cons

- **Requires brute-force or index rebuild**: The main ANN index (e.g., HNSW) is built over the full dataset. The filtered subset does not have a prebuilt index, so search degrades to brute-force O(n_filtered)
- **Wasted computation for wide filters**: When the filter passes 80%+ of data, you still pay the full filtering cost before running essentially the same search
- **Cannot leverage ANN index structure**: The full HNSW graph is useless once you restrict to a subset

### Performance Characteristics

- **Recall**: Perfect (for the filtered subset)
- **Latency**: O(n_filtered) for brute-force on subset; acceptable when selectivity < 1%, unacceptable for > 10%
- **Best case**: Very selective filters (< 1% of data matches)
- **Worst case**: Broad filters or no filter at all

---

## 3. Post-Filtering (Search-Then-Filter)

### How It Works

1. Run standard ANN search on the full index to retrieve top-k' candidates (where k' >> k)
2. Apply scalar predicates to filter the k' candidates
3. Return the top-k that pass the filter

### Pros

- **Uses existing ANN index as-is**: No index modification needed
- **Fast for broad filters**: When most items pass the filter, the first k results likely all match
- **Simple implementation**: Standard ANN + filter predicate evaluation

### Cons

- **Narrow filter catastrophe**: If only 0.1% of data matches the filter, you need k' = k/0.001 = 1000k candidates to expect k results. For k=10 this means retrieving 10,000 candidates, negating all ANN benefits
- **No recall guarantee**: Even with large oversampling, there is no guarantee of finding k qualifying results
- **Unpredictable latency**: Depends entirely on filter selectivity, which may vary per query

### Performance Characteristics

- **Recall**: Degrades severely with narrow filters; often returns < k results
- **Latency**: O(ANN + k' * filter_eval). For narrow filters, k' explodes
- **Best case**: Broad filters (> 50% selectivity)
- **Worst case**: Narrow filters (< 5% selectivity)

---

## 4. Single-Stage / Integrated Approaches

These approaches modify the index structure itself to incorporate filtering into the search algorithm.

### 4.1 Weaviate's ACORN Implementation

**Paper**: ACORN: Performant and Predicate-Agnostic Search Over Vector Embeddings and Structured Data (SIGMOD 2024, originally on arXiv March 2024)

#### How It Works

ACORN modifies HNSW to support **predicate subgraph traversal** — the search traverses only the subgraph of nodes that pass the query predicate, while maintaining good graph connectivity.

**Construction (ACORN-gamma):**
- Expands candidate neighbors from M to M*gamma per node, where gamma = 1/s_min (s_min = minimum expected selectivity)
- Applies **predicate-agnostic pruning** instead of HNSW's standard RNG pruning (which fails when predicate-violating nodes break triangles)
- At level 0: retains nearest M_beta candidates directly; for remaining candidates, prunes candidate c if reachable via two-hop path, otherwise keeps c

**Search:**
- **Two-hop expansion**: Instead of evaluating single-hop connections, ACORN evaluates nodes two hops away, allowing it to bypass filtered-out intermediate nodes
- **Adaptive activation**: If the first hop passes the filter, traversal proceeds normally (single-hop); two-hop activates only when intermediate nodes fail filter conditions
- **Entry point seeding**: Additional entry points at layer 0 prevent queries from landing in filter-sparse regions

**Complexity**: O((d + gamma) * log(s*n) + log(1/s)), compared to oracle partition complexity O(d * log(s*n))

#### Weaviate-Specific Adaptations

1. **No reindexing required**: Works on existing HNSW indexes without rebuild
2. **Conditional two-hop**: Dynamically switches based on neighborhood filter density
3. **Automatic fallback**: Switches to sweeping (standard HNSW with skip) for high-selectivity filters
4. **Flat search cutoff**: Falls back to brute-force for extremely small filtered subsets (configurable via `flatSearchCutOff`)

#### Performance

- 2-1000x higher throughput than prior methods at fixed recall
- At 20% selectivity with low filter-query correlation: ~2x throughput improvement
- At very low correlation: order-of-magnitude advantage
- LAION-25M: >1000x higher QPS than post-filtering
- Construction cost: <=11x HNSW build time; memory <=1.3x HNSW

#### When It Works Best/Worst

- **Best**: Low-correlation filters (filter attribute is independent of the query vector), moderate to narrow selectivity
- **Worst**: Very high selectivity where standard HNSW traversal (sweeping) is simpler and faster

---

### 4.2 Qdrant's Filterable HNSW + Payload Indexing

#### How It Works

Qdrant combines three mechanisms:

1. **Payload Index**: Organizes metadata using specialized structures per field type:
   - Numeric indices: interval trees for range queries
   - Keyword indices: exact matching
   - Full-text indices
   - Tenant indices for multi-tenant isolation
   - Both in-memory and on-disk variants

2. **Filterable HNSW**: Constructs additional edges in the HNSW graph based on payload values. For each distinct payload value (e.g., `brand=Apple`), Qdrant pre-builds a connected subgraph. These per-value subgraphs are merged into the full graph. During search, the algorithm traverses the HNSW graph but skips nodes that don't match the filter, using the additional edges to maintain connectivity.

3. **Query Planner**: At query time, estimates filter cardinality and selects strategy:
   - **High cardinality** (broad filter): Use filterable HNSW, skip non-matching nodes during traversal
   - **Low cardinality** (narrow filter, < threshold): Switch to payload index alone, retrieve matching IDs, then brute-force or direct lookup
   - Threshold is configurable (default ~10K points)

#### Performance Characteristics

- **Recall**: High for both narrow and broad filters due to adaptive strategy selection
- **Latency**: Consistent due to query planner routing
- **Best**: Multi-tenant workloads where tenant_id partitions data cleanly
- **Worst**: High-cardinality categorical filters where subgraph construction is expensive

---

### 4.3 Pinecone's Single-Stage Filtering

#### How It Works

Pinecone merges the vector index and metadata index into a unified structure:

1. **Slab Architecture**: Vectors are partitioned into non-overlapping "slabs" organized in an LSM-tree structure in object storage
2. Each slab contains: vector data, an internal ANN index, and a metadata index (bitmap-based)
3. **Bitmap indices** per metadata field within each slab:
   - Low-cardinality bitmaps: cached in memory if frequently used
   - High-cardinality bitmaps: streamed from disk, intersected with vector index during scan
4. During search, the vector index and metadata index are traversed simultaneously — the bitmap intersection happens within the ANN search loop, not before or after

#### Key Properties

- Produces pre-filtering accuracy at post-filtering speed (or faster)
- Searches with metadata filters can actually be **faster** than unfiltered searches because filtering prunes the work
- Scales consistently from 1M to 100B vectors
- Supports complex multi-condition filters

#### Performance (2025 benchmarks)

- 79ms unfiltered -> 51.6ms with 1% filter = ~35% latency improvement
- Maintains exact filter recall (no false negatives)

#### When It Works Best/Worst

- **Best**: Production workloads with varied filter selectivity; multi-tenant access control
- **Worst**: Requires Pinecone's proprietary infrastructure; not reproducible in open-source

---

### 4.4 Milvus's Partition-Based Approach

#### How It Works

1. Vectors are physically partitioned based on frequently-queried scalar attributes (e.g., category, tenant_id)
2. Each partition maintains its own vector index (HNSW, IVF, etc.)
3. At query time, the system routes to the relevant partition(s) based on filter predicates and searches within them
4. A cost-based optimizer selects the execution strategy per partition

Additionally, Milvus supports **partition keys** that automatically route data and queries, and **multi-vector hybrid search** combining dense and sparse vectors with RRF fusion.

#### Performance

- Partition-based approach outperforms cost-based strategies by up to 13.7x
- Natural fit for categorical filters with moderate cardinality

#### When It Works Best/Worst

- **Best**: Workloads with a clear partitioning key (tenant_id, category) with moderate cardinality (10s to 1000s of values)
- **Worst**: High-cardinality filters (millions of unique values), range filters, or multi-column filter combinations that don't align with partitioning

---

## 5. IVF-Based Hybrid Approaches

### How It Works

IVF (Inverted File Index) partitions the vector space into clusters via k-means. Hybrid IVF extends this by:

1. Storing metadata alongside each vector within each cluster
2. During search: probe the nearest nprobe clusters, but within each cluster apply the filter predicate to skip non-matching vectors
3. **FAISS Big-ANN approach**: Encodes filter logic directly in unused bits of 64-bit vector IDs. During the IVF search loop, filter checks are done via bit operations on the ID without any metadata callback — rules out ~80% of negatives with zero overhead

### Pros

- Natural integration point: IVF already scans lists, adding filter checks is trivial
- Works well with product quantization (IVF-PQ)
- Memory-efficient: no separate filter index needed if using ID-encoded filters

### Cons

- Cluster imbalance: filtered subsets may concentrate in few clusters, causing load imbalance
- For narrow filters, most probed clusters may contain zero matching vectors
- k-means clustering is oblivious to metadata — no guarantee that same-metadata vectors are co-located

### Performance

- Filter-aware hybrid IVF achieves high recall with 1.4s latency on billion-scale data in RAM-constrained (<=64GB) environments
- FAISS optimized: 30-40K QPS at equal recall vs. 3K QPS unfiltered baseline

---

## 6. Geometric Transformation / Filter Fusion (FCVI)

**Paper**: Filter-Centric Vector Indexing: Geometric Transformation for Efficient Filtered Vector Search (2025)

### How It Works

FCVI incorporates filter information directly into the vector space through geometric transformation, allowing any standard ANN index to handle filtered queries without modification.

**Transformation**: Given a vector v with filter value f and scaling parameter alpha:

```
psi(v, f, alpha) = [v(1) - alpha*f, v(2) - alpha*f, ..., v(d/m) - alpha*f]
```

Where the original d-dimensional vector is partitioned into d/m segments, each subtracted by alpha * filter_vector.

**Effect**: Vectors with identical filter values maintain original distances; vectors with different filter values are pushed apart proportionally to alpha^2. This creates natural clustering by filter value in the transformed space.

**Search**:
1. Transform query vector with query filter values
2. Run standard ANN search on transformed index
3. Retrieve k' = O(k / (lambda * alpha^2)) candidates
4. Re-score with combined similarity: score = lambda * sim(v, q) + (1-lambda) * sim(f, Fq)

### Theoretical Guarantees

- **Distance preservation**: When filter values match, original distances are perfectly preserved
- **Uniqueness**: The transformation is the unique linear transformation with these properties
- **Separation**: Complete filter cluster separation when alpha >= alpha* (derived analytically)

### Performance

- 1.7-1.8x speed over post-filtering, 2.6-3.0x over pre-filtering
- 94.8-95.3% recall@100
- Index sizes comparable to baseline (within 1-2%), 40-50% smaller than UNIFY
- Under distribution shift: only 19-31% latency increase vs. 128-172% for pre-filtering

### Limitations

- **Dimensional explosion**: High-cardinality categorical fields (1000s of values) bloat vector size linearly via one-hot encoding
- **Multi-column difficulty**: Balancing weights across multiple filter attributes remains an unsolved problem
- **Range queries**: Ordinal/continuous filters don't map cleanly to one-hot encoding
- **Dynamic updates**: Metadata changes force re-indexing of affected vectors
- **Alpha tuning**: Trade-off between strict filtering and semantic relevance requires empirical optimization

### When It Works Best/Worst

- **Best**: Low-to-moderate cardinality categorical filters; single filter column; static data
- **Worst**: High-cardinality filters, range queries, multi-column filters, frequently changing metadata

---

## 7. Graph-Based Approaches with Attribute-Aware Navigation

### 7.1 Filtered-DiskANN (Microsoft, WWW 2023)

#### How It Works

Two algorithms that modify graph construction based on label sets:

1. **FilteredVamana**: Builds a single graph where edges consider both vector proximity and label overlap. During construction, the greedy search for neighbors is augmented with label-aware routing. During search, only nodes matching the query label are considered as candidates.

2. **StitchedVamana**: Builds a **separate Vamana index per label** (one graph for "electronics", one for "clothing", etc.), then overlays them into a single graph whose edges are the union of all per-label edges.

#### Performance

- 10x+ more efficient than prior state-of-the-art for filtered queries
- Supports SSD-based search with thousands of QPS at >90% recall@10
- FilteredVamana: faster to build, easier to update incrementally
- StitchedVamana: better query performance, but O(L) construction cost where L is number of labels

### 7.2 NHQ (Navigable Hybrid Query, arXiv 2022)

#### How It Works

NHQ builds a composite proximity graph index that considers both vector proximity and attribute constraints:

1. Edge selection considers attribute overlap (not just distance)
2. Routing strategy uses attributes to guide greedy search
3. Joint pruning removes edges that are suboptimal for both distance and attribute matching

#### Performance

- Up to 315x speedup over baseline methods at same accuracy
- Framework-style: can deploy various proximity graphs (HNSW, NSG, etc.) on NHQ

### 7.3 UNG (Unified Navigating Graph, SIGMOD 2024)

A framework that encodes **containment relationships of label sets** for all vectors into the graph structure:

- Supports any query label size and specificity
- Only searches filtered vectors (no post-filtering needed)
- 10x speedups over baselines at same accuracy

---

## 8. Relaxed Monotonicity: VBASE (OSDI 2023, Microsoft)

### The Key Insight

Traditional database indices exhibit **monotonicity**: scanning an index in order guarantees results come in sorted order (e.g., B-tree scan produces sorted keys). This enables early termination — stop scanning once you have k results.

Vector indices **do not** have this property. An HNSW traversal does not visit nodes in strict distance order. This forces existing systems to use the TopK interface: "give me exactly k nearest neighbors" as a black box, making it impossible to interleave filtering with index traversal.

### How VBASE Works

1. **Relaxed monotonicity observation**: While HNSW traversal is not strictly monotonic, it exhibits a **statistical trend** — later-visited nodes are generally (but not always) farther away. This "relaxed" monotonicity can still support early termination with high probability.

2. **Iterator-based interface**: Instead of TopK (batch), VBASE exposes the vector index as an iterator with Open/Next/Close operations (Volcano model). Each Next() call returns the next candidate in approximate distance order.

3. **Integration with relational operators**: The iterator interface allows the vector index to participate in standard query plans — join with filter predicates, merge with other index scans, etc. Filtering happens **during** traversal, not before or after.

4. **Termination condition**: The query terminates when:
   - K qualifying vectors have been found, AND
   - The relaxed monotonicity check indicates further traversal is unlikely to find closer qualifying vectors

### Performance

- 10-1000x improvement over traditional pre/post-filtering approaches for complex hybrid queries
- Enables arbitrary SQL predicates combined with vector similarity
- Integrated into PostgreSQL (MSVBASE)

### When It Works Best/Worst

- **Best**: Complex queries combining vector similarity with multiple relational predicates (joins, aggregations, range filters)
- **Worst**: Simple single-predicate filters where specialized approaches (ACORN, filterable HNSW) are more optimized

---

## 9. Multi-Index Collection: SIEVE (VLDB 2025)

### How It Works

Instead of building one index and adapting it for all filters, SIEVE builds a **collection of specialized indexes**, each optimized for a specific predicate subgroup.

1. **Workload-driven index selection**: Analyzes observed query workload to identify common filter patterns
2. **Three-dimensional cost model**: Evaluates each index candidate on (search speed, recall, memory cost)
3. **Budget-constrained optimization**: Selects the set of indexes that maximizes expected throughput within a memory budget
4. **Query-time routing**: For each incoming query, the cost model routes to the index with the best expected latency-recall trade-off

### Performance

- Up to 8.06x speedup over single-index approaches
- Index build overhead as low as 1% of standard HNSW per additional index
- Adapts to actual query distribution rather than worst-case

### When It Works Best/Worst

- **Best**: Workloads with predictable, recurring filter patterns; sufficient memory budget
- **Worst**: Unpredictable/uniform filter distribution; tight memory constraints; cold-start with no workload history

---

## 10. Range-Filtered Search: UNIFY (VLDB 2025)

### The Problem

Range filters (e.g., `price BETWEEN 100 AND 500`, `date >= 2024-01-01`) are fundamentally different from categorical/label filters. Existing methods suffer from performance degradation as the query range shifts.

### How It Works

1. **SIG (Segmented Inclusive Graph)**: Segments the dataset by attribute values. The proximity graph of objects from any segment combination is guaranteed to be a subgraph of SIG. This enables efficient hybrid filtering by reconstructing and searching a proximity graph from relevant segments.

2. **HSIG (Hierarchical SIG)**: Adds HNSW-like hierarchical structure to SIG, achieving logarithmic hybrid filtering complexity. Fuses skip-list connections and compressed HNSW edges into the hierarchical graph.

3. **Adaptive execution**: Implements pre-filtering, post-filtering, and hybrid filtering for HSIG, selecting the best strategy based on the query range width.

### Performance

- Outperforms state-of-the-art across varying query ranges
- Handles both narrow and wide range filters with consistent performance
- Single unified index (avoids the index proliferation problem)

---

## 11. Summary Comparison

| Approach | Narrow Filter | Wide Filter | Build Cost | Memory | Recall | Implementation Complexity |
|---|---|---|---|---|---|---|
| Pre-filtering | Good (small subset) | Bad (wasteful) | Low | Low | Perfect | Low |
| Post-filtering | Bad (misses results) | Good | Low | Low | Degrades | Low |
| ACORN | Excellent | Good (falls back) | Medium (<=11x HNSW) | Low (<=1.3x) | High | Medium |
| Qdrant Filterable HNSW | Good (payload fallback) | Good (skip traversal) | Medium | Medium | High | Medium |
| Pinecone Single-Stage | Excellent | Excellent | Medium | Medium | Perfect | High (proprietary) |
| Milvus Partitions | Good (per-partition) | Good | Low per partition | High (duplication) | High | Low |
| IVF Hybrid | Medium | Good | Low | Low | Medium | Low |
| FCVI (Geometric) | Good | Good | Low (standard index) | Low | ~95% | Medium |
| Filtered-DiskANN | Excellent | Good | High (per-label) | High | High | Medium |
| VBASE | Good (iterator) | Good (early term) | Low | Low | Approximate | High |
| SIEVE | Excellent (routed) | Good | High (many indexes) | High | High | High |
| UNIFY (ranges) | Good | Good | Medium | Medium | High | High |

### 2025 State of the Art

Based on benchmarks across six vector engines (2025):
- **Integrated filtering systems** (Pinecone, Zilliz/Milvus): 1.2-1.5x throughput gains under 10% selectivity filters
- **Brute-force fallback systems** (LanceDB, OpenSearch, PGVector): QPS drops under filtering pressure
- **P95 latency**: Integrated systems achieve 30-50ms; post-filter systems spike to 200-300ms

The consensus is that **in-algorithm filtering** is the way forward. Systems with integrated filtering not only preserve recall but actually get faster when filters prune the workload.

---

## 12. Relevance to VexDB-Lite

VexDB-Lite currently implements a HybridIndex that maintains per-partition HNSW graphs keyed by scalar column values (similar to Milvus's partition approach and StitchedVamana). This is a strong foundation. Key observations:

### Current Approach Strengths
- Per-partition HNSW avoids the graph fragmentation problem entirely
- Good for moderate-cardinality categorical filters (the partition key)
- Simple, correct, and well-integrated with DuckDB

### Potential Improvements to Research Further

1. **ACORN-style two-hop expansion**: Could be added to GraphIndexCore's search algorithm without index rebuild. This would handle filters on attributes *other than* the partition key.

2. **Query planner with selectivity-based routing** (Qdrant-style): When filter selectivity is extremely narrow, fall back to brute-force on the filtered subset. When broad, use standard HNSW with skip.

3. **FCVI geometric transformation**: Interesting for single additional filter columns with low cardinality. Could be a lightweight alternative to full partition-based indexing.

4. **VBASE iterator model**: DuckDB's Volcano-model execution engine is already iterator-based. Exposing GraphIndexCore search as an iterator that yields results one-at-a-time would enable natural integration with DuckDB's filter pushdown.

5. **UNIFY for range filters**: The current HybridIndex only supports equality filters on the partition key. Range filters (price ranges, date ranges) would require a different approach like UNIFY's segmented inclusive graph.

---

## Sources

- [ACORN Paper (arXiv)](https://arxiv.org/abs/2403.04871)
- [Weaviate ACORN Blog](https://weaviate.io/blog/speed-up-filtered-vector-search)
- [Qdrant Filtering Guide](https://qdrant.tech/articles/vector-search-filtering/)
- [Pinecone: The Missing WHERE Clause](https://www.pinecone.io/learn/vector-search-filtering/)
- [Pinecone Metadata Filtering Research](https://www.pinecone.io/research/accurate-and-efficient-metadata-filtering-in-pinecones-serverless-vector-database/)
- [Filtered-DiskANN (WWW 2023)](https://harsha-simhadri.org/pubs/Filtered-DiskANN23.pdf)
- [VBASE (OSDI 2023)](https://www.usenix.org/conference/osdi23/presentation/zhang-qianxi)
- [FCVI: Geometric Transformation for Filtered Vector Search](https://arxiv.org/pdf/2506.15987)
- [SIEVE (VLDB 2025)](https://dl.acm.org/doi/10.14778/3749646.3749725)
- [UNIFY (VLDB 2025)](https://dl.acm.org/doi/10.14778/3717755.3717770)
- [Filtered Vector Search Survey (VLDB 2025)](https://www.vldb.org/pvldb/vol18/p5488-caminal.pdf)
- [The Achilles Heel of Vector Search: Filters (2025)](https://yudhiesh.github.io/2025/05/09/the-achilles-heel-of-vector-search-filters/)
- [NHQ: Navigable Proximity Graph for Hybrid Queries](https://arxiv.org/abs/2203.13601)
- [Weaviate Filtering Concepts](https://docs.weaviate.io/weaviate/concepts/filtering)
- [Milvus Hybrid Search](https://milvus.io/docs/hybrid_search_with_milvus.md)
- [Benchmarking Filtered ANN Search (ETH 2025)](http://htor.inf.ethz.ch/publications/img/2025_iff_finns_benchmark.pdf)
- [Experimental Evaluation of Hybrid Querying (VLDB 2025)](https://www.vldb.org/pvldb/vol19/p183-zheng.pdf)
- [Elasticsearch ACORN Implementation](https://www.elastic.co/search-labs/blog/elasticsearch-9-1-bbq-acorn-vector-search)
