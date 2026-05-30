/**
 * SharedAllocSet - MemoryContext backed by main shared memory
 */

#include "pg_compat.h"

#include <string.h>

extern "C" {
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/memutils_internal.h"
}

#include "vector_buffer/shared_alloc_set.h"

/* Function declarations matching PostgreSQL's MemoryContextMethods signature */
#if PG_VERSION_NUM >= 170000
static void *SharedAllocSetAlloc(MemoryContext context, Size size, int flags);
#else
static void *SharedAllocSetAlloc(MemoryContext context, Size size);
#endif
static void SharedAllocSetFree(void *pointer);
#if PG_VERSION_NUM >= 170000
static void *SharedAllocSetRealloc(void *pointer, Size size, int flags);
#else
static void *SharedAllocSetRealloc(void *pointer, Size size);
#endif
static void SharedAllocSetReset(MemoryContext context);
static void SharedAllocSetDelete(MemoryContext context);
static void SharedAllocSetStats(MemoryContext context,
                                MemoryStatsPrintFunc printfunc, void *passthru,
                                MemoryContextCounters *totals,
                                bool print_to_stderr);

/* Method table - must match PostgreSQL's MemoryContextMethods signature */
static MemoryContextMethods SharedAllocSetMethods = {
    .alloc = SharedAllocSetAlloc,
    .free_p = SharedAllocSetFree,
    .realloc = SharedAllocSetRealloc,
    .reset = SharedAllocSetReset,
    .delete_context = SharedAllocSetDelete,
    .get_chunk_context = NULL,
    .get_chunk_space = NULL,
    .is_empty = NULL,
    .stats = SharedAllocSetStats,
};

/* Allocate a new block from ShmemAlloc */
static SharedAllocBlock shared_alloc_new_block(SharedAllocSet set, Size block_size)
{
    SharedAllocBlock block;
    
    block = (SharedAllocBlock)ShmemAlloc(block_size);
    if (!block)
        return NULL;
    
    block->next = set->blocks;
    block->size = block_size;
    block->used = SHARED_BLOCKHDRSZ;
    
    set->blocks = block;
    set->totalAllocated += block_size;
    
    return block;
}

/* Try to allocate from global freelist first */
static void *shared_alloc_from_freelist(SharedAllocSet set, int fidx, Size chunk_size)
{
    if (fidx < 0 || fidx >= SHARED_ALLOC_NUM_FREELISTS)
        return NULL;
    
    SharedAllocChunk chunk = set->freelists[fidx];
    if (!chunk)
        return NULL;
    
    set->freelists[fidx] = SharedGetFreeListLink(chunk)->next;
    set->totalFree -= chunk->size;
    
    return (char *)chunk + SHARED_CHUNKHDRSZ;
}

/* Add chunk to freelist */
static void shared_add_to_freelist(SharedAllocSet set, SharedAllocChunk chunk, int fidx)
{
    if (fidx < 0 || fidx >= SHARED_ALLOC_NUM_FREELISTS)
        return;
    
    SharedGetFreeListLink(chunk)->next = set->freelists[fidx];
    set->freelists[fidx] = chunk;
    set->totalFree += chunk->size;
}

/* Allocate from a block's free area */
static void *shared_alloc_from_block(SharedAllocSet set, SharedAllocBlock block, Size chunk_size)
{
    Size aligned_size = MAXALIGN(chunk_size);
    
    if (block->used + aligned_size > block->size)
        return NULL;
    
    SharedAllocChunk chunk = (SharedAllocChunk)((char *)block + block->used);
    chunk->size = aligned_size;
    chunk->next = NULL;
    
    block->used += aligned_size;
    set->numAllocations++;
    
    return (char *)chunk + SHARED_CHUNKHDRSZ;
}

/* Create a new SharedAllocSet */
MemoryContext SharedAllocSetCreate(const char *name, Size initBlockSize, Size maxBlockSize)
{
    SharedAllocSet set;
    Size contextSize = MAXALIGN(sizeof(SharedAllocSetData));
    
    set = (SharedAllocSet)ShmemAlloc(contextSize);
    if (!set)
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of shared memory for SharedAllocSet")));
    
    memset(set, 0, sizeof(SharedAllocSetData));
    set->header.type = T_AllocSetContext;
    set->header.methods = &SharedAllocSetMethods;
    set->header.parent = NULL;
    set->header.name = name;
    set->header.mem_allocated = 0;
    
    set->initBlockSize = initBlockSize;
    set->maxBlockSize = maxBlockSize;
    set->nextBlockSize = initBlockSize;
    set->blocks = NULL;
    set->totalAllocated = 0;
    set->totalFree = 0;
    set->numAllocations = 0;
    
    for (int i = 0; i < SHARED_ALLOC_NUM_FREELISTS; i++)
        set->freelists[i] = NULL;
    
    SpinLockInit(&set->lock);
    
    return (MemoryContext)set;
}

/* Allocate memory */
#if PG_VERSION_NUM >= 170000
static void *SharedAllocSetAlloc(MemoryContext context, Size size, int flags)
#else
static void *SharedAllocSetAlloc(MemoryContext context, Size size)
#endif
{
    SharedAllocSet set = (SharedAllocSet)context;
    void *result;
    Size total_size;
    int fidx;
    
    if (size == 0)
        return NULL;
    
    total_size = MAXALIGN(SHARED_CHUNKHDRSZ + size);
    fidx = SharedGetFreeListIdx(total_size);
    
    SpinLockAcquire(&set->lock);
    
    result = shared_alloc_from_freelist(set, fidx, total_size);
    if (result) {
        SpinLockRelease(&set->lock);
        return result;
    }
    
    for (SharedAllocBlock block = set->blocks; block; block = block->next) {
        result = shared_alloc_from_block(set, block, total_size);
        if (result) {
            SpinLockRelease(&set->lock);
            return result;
        }
    }
    
    Size block_size = set->nextBlockSize;
    if (total_size > block_size - SHARED_BLOCKHDRSZ)
        block_size = total_size + SHARED_BLOCKHDRSZ;
    
    SharedAllocBlock new_block = shared_alloc_new_block(set, block_size);
    if (!new_block) {
        SpinLockRelease(&set->lock);
#if PG_VERSION_NUM >= 170000
        if ((flags & MCXT_ALLOC_NO_OOM) == 0)
#endif
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("out of shared memory in SharedAllocSet \"%s\"",
                            context->name)));
        return NULL;
    }
    
    result = shared_alloc_from_block(set, new_block, total_size);
    
    set->nextBlockSize = Min(set->nextBlockSize * 2, set->maxBlockSize);
    
    SpinLockRelease(&set->lock);
    
    return result;
}

/* Free memory — internal implementation that returns chunk to the per-size
 * freelist. Called by both the MemoryContext methods table (which PG kernel
 * itself never reaches for SharedAllocSet — see SharedAllocSet_Free below)
 * and by the public SharedAllocSet_Free wrapper.
 *
 * Caller must hold set->lock (or guarantee single-process access). The public
 * SharedAllocSet_Free wrapper acquires the lock itself.
 */
static void SharedAllocSetFreeInternal(SharedAllocSet set, void *pointer)
{
    if (!pointer)
        return;
    SharedAllocChunk chunk = (SharedAllocChunk)((char *)pointer - SHARED_CHUNKHDRSZ);
    int fidx = SharedGetFreeListIdx(chunk->size);
    shared_add_to_freelist(set, chunk, fidx);
}

/* MemoryContext method-table free_p. PG kernel never actually invokes this
 * for SharedAllocSet (PG 18+ pfree() looks up method via chunk hdrmask, and
 * SharedAllocSet cannot register a method ID), but keep a no-op definition
 * so the methods table stays valid. */
static void SharedAllocSetFree(void *pointer)
{
    /* unreachable in practice — see header comment */
}

/* Public free entry point — bypasses PG pfree's MemoryChunk-header path.
 * Use this on any pointer returned by MemoryContextAlloc{,Zero,Extended}
 * on a SharedAllocSet context. Calling pfree() on such a pointer triggers
 * "invalid pointer (header 0x0)". */
extern "C" void SharedAllocSet_Free(MemoryContext context, void *pointer)
{
    if (!context || !pointer)
        return;
    SharedAllocSet set = (SharedAllocSet)context;
    SpinLockAcquire(&set->lock);
    SharedAllocSetFreeInternal(set, pointer);
    SpinLockRelease(&set->lock);
}

/* Reallocate memory */
#if PG_VERSION_NUM >= 170000
static void *SharedAllocSetRealloc(void *pointer, Size size, int flags)
#else
static void *SharedAllocSetRealloc(void *pointer, Size size)
#endif
{
    (void)pointer;
    (void)size;
#if PG_VERSION_NUM >= 170000
    (void)flags;
#endif
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("repalloc not supported for SharedAllocSet")));
    return NULL;
}

/* Reset */
static void SharedAllocSetReset(MemoryContext context)
{
    SharedAllocSet set = (SharedAllocSet)context;
    
    SpinLockAcquire(&set->lock);
    
    for (int i = 0; i < SHARED_ALLOC_NUM_FREELISTS; i++)
        set->freelists[i] = NULL;
    
    for (SharedAllocBlock block = set->blocks; block; block = block->next) {
        block->used = SHARED_BLOCKHDRSZ;
    }
    
    set->totalFree = 0;
    set->numAllocations = 0;
    
    SpinLockRelease(&set->lock);
}

/* Delete context */
static void SharedAllocSetDelete(MemoryContext context)
{
    SharedAllocSetReset(context);
}

/* Stats */
static void SharedAllocSetStats(MemoryContext context,
                                MemoryStatsPrintFunc printfunc, void *passthru,
                                MemoryContextCounters *totals,
                                bool print_to_stderr)
{
    SharedAllocSet set = (SharedAllocSet)context;
    
    SpinLockAcquire(&set->lock);
    
    Size total_allocated = set->totalAllocated;
    Size total_free = set->totalFree;
    Size num_allocs = set->numAllocations;
    
    SpinLockRelease(&set->lock);
    
    if (totals) {
        totals->nblocks += 1;
        totals->freechunks += num_allocs;
        totals->totalspace += total_allocated;
        totals->freespace += total_free;
    }
}
