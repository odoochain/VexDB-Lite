#include "pg_compat.h"
#include "utils/relcache.h"
#include "storage/itemptr.h"

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_algorithm.h"
#include "graph_index/graph_index_xlog.h"
#include "graph_index/graph_index_qtupdate_worker.h"
#include "ann_utils.h"
#include "distance/core/distance_dispatcher.h"
#include "annkmeans.h"


bool graph_index_insert_internal(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno)
{
    if (isnull[0]) {
        return false;
    }

    MemoryContext insert_ctx = AllocSetContextCreate(CurrentMemoryContext,
        "graph_index insert temporary context", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old_ctx = MemoryContextSwitchTo(insert_ctx);

    Buffer metabuf = ReadBuffer(index, metablkno);
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));

    // PQ incremental DML is supported: the new vector is PQ-encoded on write
    // (DiskStore::add_vector -> compute_code into the code_size slot) and the
    // prune heuristic reads original vectors back from the heap for raw-vs-raw
    // distance (need_refine path in GraphIndexAlgorithm), matching openGauss.
    // The codebook is fixed (trained at build); large incremental inserts may
    // drift recall until REINDEX.

    Pointer vec_p;
    char *v = DatumGetVector(values[0], metap->precision_type, &vec_p);
    char *query = v;
    bool is_alloc = false;
    FmgrInfo *normprocinfo = graph_index_optional_proc_info(index, GRAPH_INDEX_NORM_PROC);
    bool need_norm = normprocinfo != NULL;
    if (!is_aligned(query) || need_norm) {
        uint_fast32_t vec_size = metap->dimension * VEC_ELEM_SIZE(metap->precision_type);
        query = alloc_vector(vec_size);
        memcpy(query, v, vec_size);
        is_alloc = true;
    }
    if (need_norm) {
        auto func = ann_helper::get_vector_preprocess_func(Metric::FAST_COSINE, metap->precision_type, metap->dimension);
        func(query, metap->dimension, query);
    }

    DiskStoreVariant disk_store;
    create_disk_store(disk_store, index, heap, metabuf, true);
    PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, true);
    auto visitor = [&](auto &store) -> void {
        constexpr DispatcherMode mode = DispatcherMode::DEFAULT;
        return DispatchRunner<true,
            MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
            DistPrecisionTypeList<
                DistPrecisionType::FLOAT
            >, mode>::call(metap, [&](auto &distancer) {
                distancer.prepare(index, metap);
                distancer.process(query);
                GraphIndexAlgorithm algo{metap, store, distancer};
                typename decltype(algo)::InsertContext ictx{ctx, query, heap_tid};
                algo.insert(ictx);
                ictx.destroy();
            }
        );
    };
    visit(visitor, disk_store);
    disk_store.destroy();
    ctx.destroy();

    /* Phase B: track post-train drift for PQ indexes and nudge the user to
     * retrain once enough new rows accumulate. Mirrors openGauss which bumps
     * num_new_data per insert; lite emits a one-shot NOTICE at the threshold
     * (automatic bgworker-driven retrain is phase C). */
    if (metap->quantizer_metainfo.get_setting_type() == QuantizerType::PQ &&
        metap->quantizer_metainfo.get_pq_metainfo().graph_pq) {
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        GraphIndexMetaPage mp = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
        uint32 nnd = mp->quantizer_metainfo.num_new_data + 1;
        size_t nv = mp->num_vectors;
        GraphIndexXlog xlog;
        xlog.init(index, metabuf, BufferGetPage(metabuf));
        xlog.update_num_new_data(nnd);
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
        /* Fire when new rows first cross ~20% of the vector count. Detect the
         * crossing edge (prev below, now at/above) rather than exact equality:
         * num_vectors keeps growing as we insert, so nnd*5 == nv almost never
         * lands exactly. Edge-detect keeps it one-shot per 20% band. */
        size_t thresh = nv / 5;
        if (nv >= 256 && nnd >= thresh && (nnd - 1) < thresh) {
            /* Phase C: enqueue an async retrain (launcher spawns a DB-connected
             * worker). Falls back to a NOTICE so the user can also retrain
             * manually if the queue is full / the launcher is disabled. */
            if (!qtupdate_submit(MyDatabaseId, RelationGetRelid(index))) {
                ereport(NOTICE,
                    (errmsg("vexdb_graph: %u rows inserted since last PQ codebook train "
                            "(~%d%% of %zu vectors); run SELECT index_qtupdate(<index>) to "
                            "retrain and refresh recall",
                            nnd, (int)(100 * nnd / nv), nv)));
            }
        }
    }

    if (vec_p != DatumGetPointer(values[0])) {
        pfree(vec_p);
    }
    if (is_alloc) {
        free_vector(query);
    }
    ReleaseBuffer(metabuf);
    
    MemoryContextSwitchTo(old_ctx);
    MemoryContextDelete(insert_ctx);
    return true;
}
