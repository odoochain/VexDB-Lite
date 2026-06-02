/*
 * graph_index_qtupdate_worker.cpp - PQ codebook async retrain bgworker (phase C).
 * See graph_index_qtupdate_worker.h for the design overview.
 */
#include "pg_compat.h"

extern "C" {
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "access/xact.h"
#include "utils/snapmgr.h"
#include "utils/guc.h"
#include "tcop/tcopprot.h"
#include "pgstat.h"
}

#include "graph_index/graph_index_qtupdate_worker.h"

/* slot state values (extends the 0/1/2 in the header with a transient reserve) */
#define QT_EMPTY     0u
#define QT_PENDING   1u
#define QT_RUNNING   2u
#define QT_RESERVING 3u

QtUpdateSharedState *qtupdate_shared_state = NULL;

Size qtupdate_shmem_size(void)
{
    return MAXALIGN(sizeof(QtUpdateSharedState));
}

void qtupdate_shmem_init(void)
{
    bool found;
    qtupdate_shared_state = (QtUpdateSharedState *)ShmemInitStruct(
        "QtUpdateSharedState", sizeof(QtUpdateSharedState), &found);
    if (!found) {
        qtupdate_shared_state->launcher_latch = NULL;
        for (int i = 0; i < QT_UPDATE_QUEUE_SIZE; ++i) {
            qtupdate_shared_state->tasks[i].dbid = InvalidOid;
            qtupdate_shared_state->tasks[i].index_oid = InvalidOid;
            pg_atomic_init_u32(&qtupdate_shared_state->tasks[i].state, QT_EMPTY);
        }
    }
}

void qtupdate_register_launcher(void)
{
    BackgroundWorker bw;
    memset(&bw, 0, sizeof(bw));
    bw.bgw_flags = BGWORKER_SHMEM_ACCESS;            /* launcher: no DB connection */
    bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
    bw.bgw_restart_time = 5;                          /* relaunch 5s after a crash */
    strncpy(bw.bgw_library_name, "vexdb_vector", BGW_MAXLEN);
    strncpy(bw.bgw_function_name, "qtupdate_launcher_main", BGW_MAXLEN);
    snprintf(bw.bgw_name, BGW_MAXLEN, "vexdb qtupdate launcher");
    strncpy(bw.bgw_type, "vexdb qtupdate launcher", BGW_MAXLEN);
    bw.bgw_main_arg = Int32GetDatum(0);
    bw.bgw_notify_pid = 0;
    RegisterBackgroundWorker(&bw);
}

bool qtupdate_submit(Oid dbid, Oid index_oid)
{
    if (qtupdate_shared_state == NULL) {
        elog(LOG, "qtupdate_submit: shared_state is NULL (shmem not attached in backend)");
        return false;
    }

    /* dedup: skip if the same index already has a queued/running retrain */
    for (int i = 0; i < QT_UPDATE_QUEUE_SIZE; ++i) {
        QtUpdateTask *t = &qtupdate_shared_state->tasks[i];
        if (pg_atomic_read_u32(&t->state) != QT_EMPTY &&
            t->dbid == dbid && t->index_oid == index_oid) {
            return false;
        }
    }
    /* claim a free slot: EMPTY -> RESERVING, fill data, publish as PENDING */
    for (int i = 0; i < QT_UPDATE_QUEUE_SIZE; ++i) {
        QtUpdateTask *t = &qtupdate_shared_state->tasks[i];
        uint32 expected = QT_EMPTY;
        if (pg_atomic_compare_exchange_u32(&t->state, &expected, QT_RESERVING)) {
            t->dbid = dbid;
            t->index_oid = index_oid;
            pg_write_barrier();
            pg_atomic_write_u32(&t->state, QT_PENDING);
            if (qtupdate_shared_state->launcher_latch)
                SetLatch(qtupdate_shared_state->launcher_latch);
            return true;
        }
    }
    return false;  /* queue full — drop; next insert past threshold retries */
}

void qtupdate_launcher_main(Datum main_arg)
{
    (void)main_arg;
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    BackgroundWorkerUnblockSignals();
#if PG_VERSION_NUM >= 180000
    InitializeWaitEventSupport();
#endif

    if (qtupdate_shared_state == NULL)
        return;
    qtupdate_shared_state->launcher_latch = MyLatch;

    for (;;) {
        if (ShutdownRequestPending)
            break;
        if (ConfigReloadPending) {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        for (int i = 0; i < QT_UPDATE_QUEUE_SIZE; ++i) {
            QtUpdateTask *t = &qtupdate_shared_state->tasks[i];
            uint32 expected = QT_PENDING;
            if (pg_atomic_compare_exchange_u32(&t->state, &expected, QT_RUNNING)) {
                BackgroundWorker bw;
                memset(&bw, 0, sizeof(bw));
                bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
                bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
                bw.bgw_restart_time = BGW_NEVER_RESTART;
                strncpy(bw.bgw_library_name, "vexdb_vector", BGW_MAXLEN);
                strncpy(bw.bgw_function_name, "qtupdate_worker_main", BGW_MAXLEN);
                snprintf(bw.bgw_name, BGW_MAXLEN, "vexdb qtupdate worker %d", i);
                strncpy(bw.bgw_type, "vexdb qtupdate worker", BGW_MAXLEN);
                bw.bgw_main_arg = Int32GetDatum(i);
                bw.bgw_notify_pid = 0;
                BackgroundWorkerHandle *handle = NULL;
                if (!RegisterDynamicBackgroundWorker(&bw, &handle)) {
                    /* out of worker slots (max_worker_processes) — revert to
                     * PENDING and retry on the next poll. */
                    ereport(LOG, (errmsg("vexdb_graph: qtupdate launcher could not "
                        "register a worker (slot %d) — raise max_worker_processes", i)));
                    pg_atomic_write_u32(&t->state, QT_PENDING);
                }
            }
        }

        ResetLatch(MyLatch);
        (void)WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                        5000L, PG_WAIT_EXTENSION);
    }
}

void qtupdate_worker_main(Datum main_arg)
{
    int slot = DatumGetInt32(main_arg);
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();

    QtUpdateTask *t = &qtupdate_shared_state->tasks[slot];
    Oid dbid = t->dbid;
    Oid index_oid = t->index_oid;

    BackgroundWorkerInitializeConnectionByOid(dbid, InvalidOid, 0);

    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());
    PG_TRY();
    {
        graph_index_qtupdate_internal(index_oid);
        PopActiveSnapshot();
        CommitTransactionCommand();
    }
    PG_CATCH();
    {
        EmitErrorReport();
        FlushErrorState();
        AbortCurrentTransaction();
    }
    PG_END_TRY();

    /* free the slot regardless of success (a failed retrain leaves the old
     * codebook intact; the next drift threshold will re-enqueue). */
    pg_atomic_write_u32(&t->state, QT_EMPTY);
}
