#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index.h"
#include "quantizer/quantizer.h"
#include "quantizer/pq/pq_distancer.h"
#include "data_type/halfvec.h"

#include <unordered_map>
#include <vector>

using namespace ann_helper;

// ---------------------------------------------------------------------------
// Process-local PQ codebook cache.  Each backend caches centroids after the
// first disk read; subsequent prepare() calls within the same process hit
// this map instead of re-reading from shared buffers.
// ---------------------------------------------------------------------------
namespace {
struct PQCachedCodebook {
    size_t d, M, nbits, dsub, ksub;
    Metric metric;
    DistPrecisionType precision;
    std::vector<float> centroids; // d * ksub floats
};
static std::unordered_map<Oid, PQCachedCodebook> g_pq_cache;
} // namespace

static void stash_to_cache(Relation index, const ProductQuantizer &pq,
                           Metric metric, DistPrecisionType precision)
{
    if (index == NULL) return;
    PQCachedCodebook entry;
    entry.d         = pq.d;
    entry.M         = pq.M;
    entry.nbits     = pq.nbits;
    entry.dsub      = pq.dsub;
    entry.ksub      = pq.ksub;
    entry.metric    = metric;
    entry.precision = precision;
    entry.centroids.assign(pq.centroids,
                           pq.centroids + pq.d * pq.ksub);
    g_pq_cache[RelationGetRelid(index)] = std::move(entry);
}

static bool load_from_cache(Relation index, Metric metric,
                            DistPrecisionType precision,
                            ProductQuantizer &pq,
                            ann_helper::distance_func &precise_func,
                            float &flag)
{
    if (index == NULL) return false;
    auto it = g_pq_cache.find(RelationGetRelid(index));
    if (it == g_pq_cache.end()) return false;
    const auto &entry = it->second;

    // Reconfigure PQ with cached parameters
    pq.set_basic_values(entry.d, entry.M, entry.nbits);
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.set_fvec_ny_distance_func(metric);
    pq.set_dist_code_func();
    if (precision == DistPrecisionType::HALF) {
        precise_func = get_aligned_half_distance_func(metric, static_cast<uint32>(entry.d));
    } else {
        precise_func = get_aligned_distance_func(metric, static_cast<uint32>(entry.d));
    }
    flag = (metric == Metric::INNER_PRODUCT) ? -1.0f : 1.0f;

    std::memcpy(pq.centroids, entry.centroids.data(),
                entry.centroids.size() * sizeof(float));
    return true;
}

void PQDistancer::configure_for_metric(size_t d, size_t M, size_t nbits_,
                                       Metric metric, DistPrecisionType precision)
{
    pq.set_basic_values(d, M, nbits_);
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.set_fvec_ny_distance_func(metric);
    pq.set_dist_code_func();
    if (precision == DistPrecisionType::HALF) {
        _get_distance_precise_func = get_aligned_half_distance_func(metric, static_cast<uint32>(d));
    } else {
        _get_distance_precise_func = get_aligned_distance_func(metric, static_cast<uint32>(d));
    }
    flag = (metric == Metric::INNER_PRODUCT) ? -1.0f : 1.0f;
}

void PQDistancer::prepare(Relation index, void *metapage)
{
    if (prepared) {
        return;
    }
    GraphIndexMetaPage metap = (GraphIndexMetaPage)metapage;
    Metric metric = metap ? metap->metric : Metric::L2;
    DistPrecisionType precision = metap ? metap->precision_type : DistPrecisionType::FLOAT;

    // Try process-local cache first
    if (load_from_cache(index, metric, precision, pq, _get_distance_precise_func, flag)) {
        dist_table = alloc_floatvector(pq.M * pq.ksub);
        prepared = true;
        return;
    }

    // Cache miss — read from meta page and load from disk
    if (metap == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("PQ: cannot prepare with NULL metapage and no cache entry")));
    }
    PQMetaInfo &pq_metainfo = metap->quantizer_metainfo.get_pq_metainfo();

    configure_for_metric(metap->dimension, pq_metainfo.m, pq_metainfo.nbits(),
                         metric, precision);
    hnsw_read_pq_center(index, pq, metap->qtcode_block);
    stash_to_cache(index, pq, metric, precision);

    dist_table = alloc_floatvector(pq_metainfo.m * pq_metainfo.k);
    prepared = true;
}

void PQDistancer::process(const char *query, void *metapage)
{
    GraphIndexMetaPage metap = (GraphIndexMetaPage)metapage;
    if (metap->precision_type == DistPrecisionType::HALF) {
        float *half2float = alloc_floatvector(metap->dimension);
        halfs_to_floats((half *)query, half2float, metap->dimension);
        pq.compute_distance_table(half2float, dist_table);
        free_vector(half2float);
    } else {
        pq.compute_distance_table((float *)query, dist_table);
    }
}

void PQDistancer::hnsw_read_pq_center(Relation index, ProductQuantizer &target,
                                       BlockNumber qtcode_block)
{
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

void PQDistancer::flush(Relation index, BlockNumber qtcode_block, bool enabling)
{
    graph_index_store_qt_centroids(index, qtcode_block, pq.centroids, pq.d * pq.ksub * sizeof(float));
    stash_to_cache(index, pq, flag < 0 ? Metric::INNER_PRODUCT : Metric::L2,
                   DistPrecisionType::FLOAT);
}
