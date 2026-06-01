#ifndef GRAPH_INDEX_XLOG_H
#define GRAPH_INDEX_XLOG_H

#include "c.h"
#include "pg_compat.h"
extern "C" {
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "access/generic_xlog.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
}

#include "graph_index/graph_index_struct.h"


class GraphIndexXlog {
public:
    GraphIndexXlog() = default;

    void init(Relation index, Buffer metabuf, Page metapage)
    {
        this->index = index;
        this->metabuf = metabuf;
        this->metapage = metapage;
    }

    void update_num_vector(size_t num_vector)
    {
        GenericXLogState *state = GenericXLogStart(index);
        Page page = GenericXLogRegisterBuffer(state, metabuf, 0);
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(page);
        metap->num_vectors = num_vector;
        GenericXLogFinish(state);
    }

    /* PQ async retrain — step 1: bump code_version so the metapage enters the
     * "updating" state (centroids_version != code_version); concurrent
     * get_type() callers then spin-wait until the retrain finishes. */
    void enter_quantizer_update(uint8 code_version)
    {
        GenericXLogState *state = GenericXLogStart(index);
        Page page = GenericXLogRegisterBuffer(state, metabuf, 0);
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(page);
        metap->quantizer_metainfo.code_version = code_version;
        GenericXLogFinish(state);
    }

    /* PQ async retrain — final step: bump centroids_version to match
     * code_version (leaves "updating"), reset num_new_data, and (re)enable PQ.
     * Must run only after the re-encoded vec data and new codebook are durable. */
    void finish_quantizer_update(uint8 centroids_version, uint32 num_new_data)
    {
        GenericXLogState *state = GenericXLogStart(index);
        Page page = GenericXLogRegisterBuffer(state, metabuf, 0);
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(page);
        metap->quantizer_metainfo.centroids_version = centroids_version;
        metap->quantizer_metainfo.num_new_data = num_new_data;
        metap->quantizer_metainfo.set_enable();
        GenericXLogFinish(state);
    }

    /* Phase B: persist the running new-row counter (incremented per insert). */
    void update_num_new_data(uint32 num_new_data)
    {
        GenericXLogState *state = GenericXLogStart(index);
        Page page = GenericXLogRegisterBuffer(state, metabuf, 0);
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(page);
        metap->quantizer_metainfo.num_new_data = num_new_data;
        GenericXLogFinish(state);
    }

    void update_entry(GraphIndexEntryInfo entry)
    {
        GenericXLogState *state = GenericXLogStart(index);
        Page page = GenericXLogRegisterBuffer(state, metabuf, 0);
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(page);
        metap->entrypoint_id = entry.id;
        metap->entry_cur_layer_idx = entry.cur_layer_idx;
        metap->entry_level = entry.level;
        GenericXLogFinish(state);
    }

    void log_build_index(ForkNumber fork_num)
    {
        BlockNumber nblocks = RelationGetNumberOfBlocksInFork(index, fork_num);
        log_newpage_range(index, fork_num, 0, nblocks, true);
    }


    Relation index = NULL;
    Buffer metabuf = InvalidBuffer;
    Page metapage = NULL;
};

#endif /* GRAPH_INDEX_XLOG_H */
