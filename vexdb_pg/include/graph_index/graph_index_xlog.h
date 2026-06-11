#ifndef GRAPH_INDEX_XLOG_H
#define GRAPH_INDEX_XLOG_H

#include "c.h"
#include "platform/platform_compat.h"
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
