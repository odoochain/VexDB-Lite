#include "platform/platform_compat.h"

#include "quantizer/rabitq/rabitq_distancer.h"
#include "quantizer/annkmeans.h"

#include "graph_index/graph_index.h"
#include "data_type/halfvec.h"

namespace rabitq {

void RabitqDistancer::destroy()
{
    if (!prepared) {
        return;
    }
    if (quantizer.has_value()) {
        quantizer->destroy();
        quantizer.reset();
    }
    estimator.perf_report();
    estimator.destroy();
    prepared = false;
}

void RabitqDistancer::load_rabitq(Relation index, void *metapage)
{
    GraphIndexMetaPage metap = (GraphIndexMetaPage)metapage;
    RaBitQMeta &rabitq_meta = metap->quantizer_metainfo.get_rabitq_meta();
    load_rabitq_cache(index, rabitq_meta);
    new (&estimator) RaBitQEstimator(padded_dim, metric, rabitq_meta.query_rescaling_factor);
    estimator.set_quantizer(&*quantizer);
}

void RabitqDistancer::load_rabitq_quantizer(Relation index, RaBitQMeta &rabitq_meta, RaBitQCache &cache)
{
    quantizer.emplace(dim, padded_dim, metric);
    quantizer->set_rescaling_factor(rabitq_meta.query_rescaling_factor);

    size_t random_matrix_size = quantizer->get_random_matrix_size();
    size_t centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * dim;
    size_t rotated_centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * padded_dim;
    size_t total_fixed_size = random_matrix_size + (centroids_size + rotated_centroids_size) * sizeof(float);

    if (index) {
        cache.fixed_data = (char *)palloc(total_fixed_size);
        read_rabitq_data(index, total_fixed_size, cache.fixed_data);
    }

    char *random_matrix = cache.fixed_data;
    float *centroids = (float *)(random_matrix + random_matrix_size);
    float *rotated_centroids = centroids + centroids_size;

    quantizer->load(random_matrix, centroids, rotated_centroids);
}

void RabitqDistancer::read_rabitq_data(Relation index, size_t rabitq_data_size, char *rabitq_data)
{
    struct BufSize {
        Buffer buf;
        size_t s;
    };
    Vector<BufSize> bufs;

    Buffer buf = ReadBuffer(index, qtcode_block);
    Page page = BufferGetPage(buf);
    size_t tmp_s = (page + ((PageHeader)page)->pd_lower - PageGetContents(page));
    bufs.push_back({buf, tmp_s});
    BlockNumber next_blk = GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno;
    while (BlockNumberIsValid(next_blk)) {
        buf = ReadBuffer(index, next_blk);
        page = BufferGetPage(buf);
        tmp_s = (page + ((PageHeader)page)->pd_lower - PageGetContents(page));
        bufs.push_back({buf, tmp_s});
        next_blk = GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno;
    }

    size_t total_size = 0;
    for (const auto &bs : bufs) {
        total_size += bs.s;
    }

    Assert(total_size == rabitq_data_size);

    char *cur = rabitq_data;
    for (auto &bs : bufs) {
        memcpy(cur, PageGetContents(BufferGetPage(bs.buf)), bs.s);
        ReleaseBuffer(bs.buf);
        cur += bs.s;
    }
    ann_helper::optional_destroy(bufs);
}

void RabitqDistancer::free_rabitq()
{
    if (quantizer.has_value()) {
        quantizer->destroy();
        quantizer.reset();
    }
    estimator.perf_report();
    estimator.destroy();
}

void RabitqDistancer::train(Relation index, FloatVectorArray samples, int dimension, Metric metric,
    bool need_norm, int parallel_workers, int maintenance_work_mem)
{
    dim = dimension;
    padded_dim = RABITQ_PADDED_DIM(dim);
    cid_size = sizeof(uint16);
    bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
    size_t ext_size = RABITQ_EXT_DATA_SIZE(padded_dim);
    code_len = cid_size + bin_size + ext_size;

    quantizer.emplace(dim, padded_dim, metric);

    /* do kmeans */
    int k = GRAPH_INDEX_RABITQ_NUM_CLUSTERS;
    KMeans kmeans(samples, k, metric, false, maintenance_work_mem, parallel_workers);
    kmeans.train();
    FloatVectorArray centroids_arr = kmeans.get_centroids();

    size_t centroids_size = k * dim * sizeof(float);
    memcpy(quantizer->get_centroids(), centroids_arr->items, centroids_size);
    kmeans.destroy();

    /* do train */
    quantizer->train();

    (void)index;
}

} /* namespace rabitq */
