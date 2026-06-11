#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index.h"
#include "rabitq/rabitq_distancer.h"
#include "data_type/halfvec.h"

#include <unordered_map>
#include <vector>

/* 以下函数需要在 pg 侧和 duck 侧作出不同实现
 * PG: vexdb_pg/src/quantizer/rabitq_adapter.cpp
 * Duck:
 */

namespace rabitq {

// ---------------------------------------------------------------------------
// Process-local RaBitQ cache.  Stores the concatenated fixed data
// (random matrix + centroids + rotated centroids) per index OID.
// ---------------------------------------------------------------------------
namespace {
struct CachedRaBitQData {
    std::vector<char> fixed_data;
};
static std::unordered_map<Oid, CachedRaBitQData> g_rabitq_cache;
} // namespace

static bool load_from_cache(Oid index_oid, RaBitQCache &cache)
{
    auto it = g_rabitq_cache.find(index_oid);
    if (it == g_rabitq_cache.end())
        return false;
    cache.oid = index_oid;
    cache.fixed_data = it->second.fixed_data.data();
    return true;
}

static void stash_to_cache(Oid index_oid, const char *data, size_t size)
{
    CachedRaBitQData entry;
    entry.fixed_data.assign(data, data + size);
    g_rabitq_cache[index_oid] = std::move(entry);
}

void RabitqDistancer::load_rabitq_cache(Relation index, RaBitQMeta &rabitq_meta)
{
    Oid index_oid = RelationGetRelid(index);

    RaBitQCache cache;
    if (load_from_cache(index_oid, cache)) {
        load_rabitq_quantizer(NULL, rabitq_meta, cache);
        return;
    }

    RaBitQCache new_cache;
    new_cache.oid = index_oid;
    load_rabitq_quantizer(index, rabitq_meta, new_cache);

    size_t random_matrix_size = quantizer->get_random_matrix_size();
    size_t centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * dim;
    size_t rotated_centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * padded_dim;
    size_t total_fixed_size = random_matrix_size + (centroids_size + rotated_centroids_size) * sizeof(float);

    stash_to_cache(index_oid, new_cache.fixed_data, total_fixed_size);
    pfree(new_cache.fixed_data);
}

void RabitqDistancer::prepare(Relation index, void *metapage)
{
    if (prepared) {
        return;
    }

    GraphIndexMetaPage metap = (GraphIndexMetaPage)metapage;
    dim = metap->dimension;
    padded_dim = RABITQ_PADDED_DIM(dim);
    metric = metap->metric;
    cid_size = sizeof(uint16);
    bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
    qtcode_block = metap->qtcode_block;
    load_rabitq(index, metapage);
    prepared = true;
}

void RabitqDistancer::process(const char *query, void *metapage)
{
    GraphIndexMetaPage metap = (GraphIndexMetaPage)metapage;
    if (metap->precision_type == DistPrecisionType::HALF) {
        float *half2float = alloc_floatvector(metap->dimension);
        halfs_to_floats((half *)query, half2float, metap->dimension);
        estimator.preprocess(half2float);
        free_vector(half2float);
    } else {
        estimator.preprocess((float *)query);
    }
}

void RabitqDistancer::flush(Relation index, BlockNumber qtcode_block, bool enabling)
{
    size_t centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * dim * sizeof(float);
    size_t random_matrix_size = quantizer->get_random_matrix_size();
    size_t rotated_centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * padded_dim * sizeof(float);
    size_t total_size = random_matrix_size + centroids_size + rotated_centroids_size;
    char *rabitq_data = (char *)palloc(total_size);
    char *centroids = rabitq_data + random_matrix_size;
    char *rotated_centroids = centroids + centroids_size;
    memcpy(rabitq_data, quantizer->get_random_matrix(), random_matrix_size);
    memcpy(centroids, quantizer->get_centroids(), centroids_size);
    memcpy(rotated_centroids, quantizer->get_rotated_centroids(), rotated_centroids_size);
    graph_index_store_qt_centroids(index, qtcode_block, (float *)rabitq_data, total_size);
    pfree(rabitq_data);
}

} /* namespace rabitq */
