#include "pg_compat.h"
#include "pq.h"
#include "annkmeans.h"
#include "pq_pg_adapter.h"
#include "quantizer/annkmeans.h"
#include "quantizer/pq_endecode.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_param.h"

#include <algorithm>
#include <cstring>
#include <vector>

void ProductQuantizer::set_basic_values(size_t dim, size_t m, size_t nbits_)
{
    d = dim;
    M = m;
    nbits = nbits_;
    set_derived_values();
}

void ProductQuantizer::set_derived_values()
{
    if (d == 0 || M == 0 || d % M != 0) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQ: dimension %zu must be a positive multiple of M=%zu", d, M)));
    }
    dsub = d / M;
    code_size = (nbits * M + 7) / 8;
    ksub = 1ULL << nbits;
    // Allow >1GB centroid tables (large d/M/ksub combos). palloc_extended +
    // memset gives the same zero-init as palloc0 without the 1GB cap.
    size_t bytes = d * ksub * sizeof(float);
    centroids = (float *)palloc_extended(bytes, MCXT_ALLOC_HUGE);
    std::memset(centroids, 0, bytes);
}

void ProductQuantizer::set_fvec_L2sqr_ny_nearest_func()
{
    _fvec_L2sqr_ny_nearest_func = ann_helper::get_fvec_L2sqr_ny_nearest_func();
}

void ProductQuantizer::set_fvec_ny_distance_func(Metric metric)
{
    _fvec_ny_distance_func = ann_helper::get_fvec_ny_distance_func(metric);
}

void ProductQuantizer::set_dist_code_func()
{
    _distance_single_code_func = ann_helper::get_distance_single_code_func((uint32)nbits);
    _distance_four_codes_func  = ann_helper::get_distance_four_codes_func((uint32)nbits);
}

void ProductQuantizer::free_resourses()
{
    if (centroids != nullptr) {
        pfree(centroids);
        centroids = nullptr;
    }
}

void ProductQuantizer::set_params(FloatVectorArray subcenters, int m)
{
    for (size_t i = 0; i < ksub; i++) {
        std::memcpy(get_centroids((size_t)m, i),
                    FloatVectorArrayGet(subcenters, i),
                    dsub * sizeof(float));
    }
}

void ProductQuantizer::train(AnnKmeansState *kmeansState, FloatVectorArray samples,
                             int parallelWorkers, int maintenanceWorkMem)
{
    (void)parallelWorkers;  // shared K-means runs serially per subquantizer; M-way parallelism is added later

    using ::vex::quantizer::KMeansState;
    using ::vex::quantizer::PQFloatArray;
    using ::vex::quantizer::PQContext;

    KMeansState shared_state;
    shared_state.skip_check_duplicate = kmeansState ? kmeansState->skipCheckDuplicate : false;
    // ann_helper::distance_func and KMeansDistanceFn are unrelated typedefs
    // that share the (const void*, const void*, uint16) ABI by construction.
    shared_state.distance_fn = kmeansState ? kmeansState->kmeansprocinfo : nullptr;
    shared_state.norm_fn     = kmeansState ? kmeansState->kmeansnormprocinfo : nullptr;
    if (shared_state.distance_fn == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQ: train called without a distance function")));
    }

    PQContext ctx;
    ctx.allocator = vex_pg::PgPQAllocator();
    // TODO(stage5): wire ctx.parallel for per-subquantizer parallelism.
    // Forward shared-layer progress events to PG's ereport(NOTICE). M is
    // small (typically <= 64) so one notice per subquantizer is fine.
    ctx.progress.fn = [M_total = M](size_t done, size_t total, const char *stage) {
        (void)M_total;
        ereport(NOTICE, (errmsg("vex PQ: %s %zu/%zu",
                                stage ? stage : "progress", done, total)));
    };

    const size_t n = (size_t)samples->length;
    if (n == 0) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQ: train called with empty sample set")));
    }

    // Scratch buffers reused across the M subquantizers. Sizes are
    // M-invariant so a single palloc pair amortises the allocation cost.
    PQFloatArray subvecs;
    subvecs.maxlen = n;
    subvecs.length = n;
    subvecs.dim    = dsub;
    subvecs.data   = (float *)palloc(n * dsub * sizeof(float));

    PQFloatArray subcenters;
    subcenters.maxlen = ksub;
    subcenters.dim    = dsub;
    subcenters.data   = (float *)palloc(ksub * dsub * sizeof(float));

    ctx.progress.Report(0, M, "kmeans subq");
    for (size_t m = 0; m < M; m++) {
        for (size_t j = 0; j < n; j++) {
            const float *src = (const float *)FloatVectorArrayGet(samples, j);
            std::memcpy(subvecs.Get(j), src + m * dsub, dsub * sizeof(float));
        }
        subcenters.length = 0;  // AnnKmeans treats input length as "already computed centers"

        vex_pg::PgQuantizerCall([&]() {
            ::vex::quantizer::AnnKmeans(shared_state, subvecs, subcenters,
                                         maintenanceWorkMem, ctx);
        });

        for (size_t i = 0; i < ksub; i++) {
            std::memcpy(get_centroids(m, i), subcenters.Get(i),
                        dsub * sizeof(float));
        }
        ctx.progress.Report(m + 1, M, "kmeans subq");
    }

    pfree(subvecs.data);
    pfree(subcenters.data);
}

namespace {

template <class Encoder>
void compute_code_generic(const ProductQuantizer &pq, const float *x, uint8_t *code,
                          float *distances_scratch)
{
    Encoder encoder(code, (int)pq.nbits);
    for (size_t m = 0; m < pq.M; m++) {
        const float *xsub = x + m * pq.dsub;
        uint64_t idxm = pq._fvec_L2sqr_ny_nearest_func(
            distances_scratch, xsub, pq.get_centroids(m, 0),
            (uint32)pq.dsub, (uint32)pq.ksub);
        encoder.encode(idxm);
    }
    encoder.restore_code();
}

} // namespace

void ProductQuantizer::compute_code(const float *x, uint8_t *code) const
{
    using ::vex::quantizer::PQEncoder8;
    using ::vex::quantizer::PQEncoder16;
    using ::vex::quantizer::PQEncoderGeneric;
    if (ksub <= 4096) {
        float distances[4096];
        switch (nbits) {
            case 8:  compute_code_generic<PQEncoder8>(*this, x, code, distances); break;
            case 16: compute_code_generic<PQEncoder16>(*this, x, code, distances); break;
            default: compute_code_generic<PQEncoderGeneric>(*this, x, code, distances); break;
        }
        return;
    }
    std::vector<float> distances(ksub);
    switch (nbits) {
        case 8:  compute_code_generic<PQEncoder8>(*this, x, code, distances.data()); break;
        case 16: compute_code_generic<PQEncoder16>(*this, x, code, distances.data()); break;
        default: compute_code_generic<PQEncoderGeneric>(*this, x, code, distances.data()); break;
    }
}

float ProductQuantizer::distance_to_code(const uint8_t *code, const float *distTable)
{
    return _distance_single_code_func((uint32)M, (uint32)nbits, distTable, code);
}

void ProductQuantizer::distance_to_four_code(const float *distTable,
    const uint8_t *code0, const uint8_t *code1, const uint8_t *code2, const uint8_t *code3,
    float &result0, float &result1, float &result2, float &result3)
{
    _distance_four_codes_func((uint32)M, (uint32)nbits, distTable,
                               code0, code1, code2, code3,
                               result0, result1, result2, result3);
}

void ProductQuantizer::compute_distance_table(const float *x, float *dist_table) const
{
    for (size_t m = 0; m < M; m++) {
        _fvec_ny_distance_func(dist_table + m * ksub, x + m * dsub,
                               get_centroids(m, 0),
                               (uint32)dsub, (uint32)ksub);
    }
}

void PQDistancer::train(Relation index, FloatVectorArray samples, size_t dimension,
    Metric metric, bool need_norm, int parallel_workers, int maintenance_work_mem,
    uint32 requested_m)
{
    uint16 m = 0;
    uint16 k = 0;
    pq_set_param((uint32)dimension, m, k, requested_m);
    configure_for_metric(dimension, (size_t)m, (size_t)(31 - __builtin_clz((uint32)k)), metric);

    AnnKmeansState *kstate = (AnnKmeansState *)palloc0(sizeof(AnnKmeansState));
    setupKmeansState(metric == Metric::INNER_PRODUCT && !need_norm
                         ? Metric::INNER_PRODUCT : Metric::L2,
                     index, kstate, (int)pq.dsub, /*ispq*/ true, /*pqtrain*/ true);
    pq.train(kstate, samples, parallel_workers, maintenance_work_mem);
    FREE_ANNKEMANSTATE(kstate);

    prepared = false;
    dist_table = nullptr;
}

// Configure ProductQuantizer state (dims + dispatch funcs + metric-derived
// flag) without touching centroids data.
void PQDistancer::configure_for_metric(size_t d, size_t M, size_t nbits_, Metric metric)
{
    pq.set_basic_values(d, M, nbits_);
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.set_fvec_ny_distance_func(metric);
    pq.set_dist_code_func();
    _get_distance_precise_func = ann_helper::get_general_distance_func(metric);
    flag = (metric == Metric::INNER_PRODUCT) ? -1.0f : 1.0f;
}

void PQDistancer::prepare(Relation index, void *metap)
{
    if (prepared && dist_table != nullptr) {
        return;
    }
    GraphIndexMetaPage mp = (GraphIndexMetaPage)metap;
    if (mp == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("PQ centroids unavailable; rebuild the index")));
    }
    Metric m = mp->metric;
    const PQMetaInfo &pqi = mp->quantizer_metainfo.get_pq_metainfo();
    if (!BlockNumberIsValid(mp->qtcode_block) ||
        !pqi.graph_pq || pqi.m == 0 || pqi.k == 0) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("PQ centroids unavailable (qtcode_block=%u, pq_m=%u, "
                   "pq_k=%u); rebuild the index",
                   mp->qtcode_block, (unsigned)pqi.m, (unsigned)pqi.k)));
    }
    configure_for_metric(mp->dimension, pqi.m, pqi.nbits(), m);
    hnsw_read_pq_center(index, pq, mp->qtcode_block);
    dist_table = (float *)palloc(pq.M * pq.ksub * sizeof(float));
    prepared = true;
}

void PQDistancer::process(const char *query)
{
    if (!prepared || dist_table == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQDistancer::process called before prepare")));
    }
    pq.compute_distance_table((const float *)query, dist_table);
}

void PQDistancer::destroy()
{
    if (dist_table != nullptr) {
        pfree(dist_table);
        dist_table = nullptr;
    }
    pq.free_resourses();
    prepared = false;
}

void PQDistancer::flush(Relation index, BlockNumber qtcode_block, bool enabling)
{
    if (index == NULL || !BlockNumberIsValid(qtcode_block) || pq.centroids == nullptr) {
        return;
    }
    const size_t bytes = pq.get_centroids_size() * sizeof(float);
    graph_index_store_qt_centroids(index, qtcode_block, pq.centroids, bytes);
}

// Symmetric to graph_index_store_qt_centroids; `target.centroids` must
// already be allocated (set_basic_values does this) so the read length is
// well-defined.
void PQDistancer::hnsw_read_pq_center(Relation index, ProductQuantizer &target,
                                      BlockNumber qtcode_block)
{
    if (index == NULL || !BlockNumberIsValid(qtcode_block) || target.centroids == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("vex PQ: hnsw_read_pq_center called with invalid state")));
    }
    const size_t expected = target.get_centroids_size() * sizeof(float);
    char *cur = (char *)target.centroids;
    size_t remaining = expected;
    BlockNumber blk = qtcode_block;
    while (BlockNumberIsValid(blk) && remaining > 0) {
        Buffer buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        PageHeader phdr = (PageHeader)page;
        char *contents = (char *)PageGetContents(page);
        size_t avail = (size_t)(page + phdr->pd_lower - contents);
        size_t take = std::min(avail, remaining);
        if (take > 0) {
            memcpy(cur, contents, take);
            cur += take;
            remaining -= take;
        }
        blk = GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno;
        UnlockReleaseBuffer(buf);
    }
    if (remaining != 0) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
            errmsg("vex PQ: short read on qtcode_block (missing %zu of %zu bytes)",
                   remaining, expected)));
    }
}
