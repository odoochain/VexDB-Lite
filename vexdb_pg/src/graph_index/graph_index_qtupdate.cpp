/*
 * graph_index_qtupdate.cpp - PQ codebook async retrain (phase A: synchronous).
 *
 * SQL entry: index_qtupdate(regclass) RETURNS bool. Holds AccessExclusiveLock
 * on the index, retrains the PQ codebook from a fresh heap sample, re-encodes
 * every vector with the new codebook, and atomically flips the metapage
 * version (code_version++ -> re-encode + store new codebook -> centroids_version++).
 * Concurrent readers spin-wait in QuantizerMetaInfo::get_type() while
 * centroids_version != code_version. Mirrors openGauss index_qtupdate.
 */
#include "pg_compat.h"

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "storage/smgr.h"
#include "utils/rel.h"
#include "fmgr.h"
}

#include <algorithm>

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_storage.h"
#include "vector_buffer/vector_smgr.h"
#include "graph_index/graph_index_algorithm.h"
#include "graph_index/graph_index_xlog.h"
#include "ann_utils.h"
#include "distance/core/distance_dispatcher.h"
#include "annkmeans.h"
#include "floatvector.h"
#include "pq.h"

extern int maintenance_work_mem;

/* Minimum live vectors before a retrain is worthwhile (also the PQ ksub floor). */
static const size_t QTUPDATE_MIN_VECTORS = 256;

static bool graph_index_qtupdate_internal(Oid index_oid)
{
    Relation index = index_open(index_oid, AccessExclusiveLock);
    if (index->rd_index == NULL || !OidIsValid(index->rd_index->indrelid)) {
        index_close(index, AccessExclusiveLock);
        ereport(ERROR, (errmsg("vexdb_graph: not a valid index relation")));
    }
    Relation heap = table_open(index->rd_index->indrelid, AccessShareLock);

    Buffer metabuf = ReadBuffer(index, GRAPH_INDEX_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
    QuantizerType qt = metap->quantizer_metainfo.get_setting_type();
    size_t num_vectors = metap->num_vectors;
    uint16 dimension = metap->dimension;
    Metric metric = metap->metric;
    BlockNumber qtcode_block = metap->qtcode_block;
    uint8 old_cv = metap->quantizer_metainfo.code_version;
    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

    if (qt != QuantizerType::PQ) {
        ReleaseBuffer(metabuf);
        table_close(heap, AccessShareLock);
        index_close(index, AccessExclusiveLock);
        ereport(NOTICE, (errmsg("vexdb_graph: index has no PQ quantizer, nothing to retrain")));
        return false;
    }
    if (num_vectors < QTUPDATE_MIN_VECTORS) {
        ReleaseBuffer(metabuf);
        table_close(heap, AccessShareLock);
        index_close(index, AccessExclusiveLock);
        ereport(NOTICE, (errmsg("vexdb_graph: only %zu vectors (< %zu), skipping PQ retrain",
                                num_vectors, QTUPDATE_MIN_VECTORS)));
        return false;
    }

    /* 1. fresh sample + train a new codebook */
    int target = (int)std::min<int64>((int64)num_vectors, (int64)MAX_SAMPLE_VECTOR_NUM);
    FloatVectorArray samples = FloatVectorArrayInit(target, dimension);
    ann_sample_rows(samples, heap, index, dimension, target, false, DistPrecisionType::FLOAT);
    if (samples->length < (int)QTUPDATE_MIN_VECTORS) {
        FloatVectorArrayFree(samples);
        ReleaseBuffer(metabuf);
        table_close(heap, AccessShareLock);
        index_close(index, AccessExclusiveLock);
        ereport(NOTICE, (errmsg("vexdb_graph: sampled too few rows, skipping PQ retrain")));
        return false;
    }
    PQDistancer encoder;
    encoder.train(index, samples, dimension, metric, false, 0, maintenance_work_mem);
    FloatVectorArrayFree(samples);

    GraphIndexXlog xlog;

    /* Build the DiskStore + ctx BEFORE entering "updating": create_disk_store /
     * the DiskStore ctor call QuantizerMetaInfo::get_type(), which spin-waits
     * while centroids_version != code_version. It must run while the versions
     * still match, otherwise the retraining backend would deadlock on its own
     * version bump. The store caches its construction-time elem_size/code_size,
     * so the later code_version++ does not affect it. */
    const size_t vec_size = (size_t)dimension * sizeof(float);
    const size_t code_size = encoder.code_size();
    PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, true);
    DiskStoreVariant disk_store;
    create_disk_store(disk_store, index, heap, metabuf, true);

    /* code_version++ -> enter "updating": new get_type() callers spin-wait */
    uint8 new_cv = (uint8)(old_cv + 1);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    xlog.init(index, metabuf, BufferGetPage(metabuf));
    xlog.enter_quantizer_update(new_cv);
    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

    /* re-encode every live vector with the new codebook (raw read back from heap;
     *    PureCode stores codes which are lossy/irreversible). */
    char *raw = alloc_vector(vec_size);
    char *code = (char *)palloc(code_size);
    visit([&](auto &store) -> void {
        using StoreT = typename std::decay<decltype(store)>::type::T;
        StoreT n = (StoreT)store.get_vector_num();
        for (StoreT id = 0; id < n; ++id) {
            CHECK_FOR_INTERRUPTS();
            if (store.fetch_vec_from_heap(ctx, id, raw)) {
                encoder.compute_code((float *)raw, code);
                vec_write(index->rd_smgr, (off_t)code_size * id, code_size, code,
                          false, VecStorageType::PureCode);
            }
        }
    }, disk_store);
    pfree(code);
    free_vector(raw);

    /* 4. persist the new codebook (overwrites qtcode_block) + refresh this
     *    backend's process-local cache. */
    encoder.flush(index, qtcode_block, false);

    /* 5. durability barrier for the re-encoded vec data + new codebook */
    smgrimmedsync(index->rd_smgr, (ForkNumber)VECTOR_FORKNUM);

    /* 6. centroids_version++ -> matches code_version: leave "updating",
     *    reset num_new_data, (re)enable PQ. */
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    xlog.init(index, metabuf, BufferGetPage(metabuf));
    xlog.finish_quantizer_update(new_cv, 0);
    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

    encoder.destroy();
    ctx.destroy();
    disk_store.destroy();
    ReleaseBuffer(metabuf);
    table_close(heap, AccessShareLock);
    /* keep AccessExclusiveLock until transaction end */
    index_close(index, NoLock);
    ereport(NOTICE, (errmsg("vexdb_graph: PQ codebook retrained over %zu vectors", num_vectors)));
    return true;
}

extern "C" {
PG_FUNCTION_INFO_V1(index_qtupdate);
Datum index_qtupdate(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);
    bool ok = graph_index_qtupdate_internal(index_oid);
    PG_RETURN_BOOL(ok);
}
}
