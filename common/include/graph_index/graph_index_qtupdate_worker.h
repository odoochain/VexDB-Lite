/*
 * graph_index_qtupdate_worker.h - PQ codebook async retrain via bgworker (phase C).
 *
 * Design (mirrors openGauss launcher + dynamic worker, adapted to lite's shmem
 * hooks): a fixed-size shared-memory task queue holds (dbid, index_oid) retrain
 * requests. A resident launcher bgworker (SHMEM only, no DB) polls the queue and
 * spawns a dynamic per-task worker that DOES connect to the task's database, runs
 * graph_index_qtupdate_internal() in its own transaction (which takes
 * AccessExclusiveLock), then frees the slot. Inserts enqueue via qtupdate_submit()
 * once num_new_data crosses the drift threshold.
 */
#ifndef GRAPH_INDEX_QTUPDATE_WORKER_H
#define GRAPH_INDEX_QTUPDATE_WORKER_H

#include "pg_compat.h"

extern "C" {
#include "storage/latch.h"
#include "port/atomics.h"
}

#define QT_UPDATE_QUEUE_SIZE 64

/* slot state: 0=empty, 1=pending (enqueued), 2=running (worker launched) */
struct QtUpdateTask {
    Oid dbid;
    Oid index_oid;
    pg_atomic_uint32 state;
};

struct QtUpdateSharedState {
    Latch *launcher_latch;            /* set by launcher; backends SetLatch to wake it */
    QtUpdateTask tasks[QT_UPDATE_QUEUE_SIZE];
};

extern QtUpdateSharedState *qtupdate_shared_state;

/* shmem integration (called from pg_init's hooks) */
Size qtupdate_shmem_size(void);
void qtupdate_shmem_init(void);          /* ShmemInitStruct + init slots */
void qtupdate_register_launcher(void);   /* RegisterBackgroundWorker in _PG_init */

/* enqueue a retrain request; returns true if newly enqueued (dedups + finds a
 * free slot), false if already queued/running or the queue is full. */
bool qtupdate_submit(Oid dbid, Oid index_oid);

/* the actual synchronous retrain (defined in graph_index_qtupdate.cpp) */
bool graph_index_qtupdate_internal(Oid index_oid);

extern "C" {
PGDLLEXPORT void qtupdate_launcher_main(Datum main_arg);
PGDLLEXPORT void qtupdate_worker_main(Datum main_arg);
}

#endif /* GRAPH_INDEX_QTUPDATE_WORKER_H */
