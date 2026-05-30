/**
 * SharedAllocSet - MemoryContext backed by main shared memory
 * 
 * All allocations are at the same address in every process, enabling
 * raw pointers to be shared across processes. Uses ShmemAlloc for
 * block allocation with internal freelists for deallocation support.
 */

#ifndef SHARED_ALLOC_SET_H
#define SHARED_ALLOC_SET_H

#include "utils/memutils.h"

/* Same as AllocSet - power of 2 sizes from 8 to 8192 bytes */
#define SHARED_ALLOC_MINBITS 3
#define SHARED_ALLOC_NUM_FREELISTS 11
#define SHARED_ALLOC_CHUNK_LIMIT (1 << (SHARED_ALLOC_NUM_FREELISTS-1+SHARED_ALLOC_MINBITS))

/* Block and chunk header sizes */
#define SHARED_BLOCKHDRSZ MAXALIGN(sizeof(struct SharedAllocBlockData))
#define SHARED_CHUNKHDRSZ sizeof(struct SharedAllocChunkData)

/* Forward declarations */
typedef struct SharedAllocBlockData *SharedAllocBlock;
typedef struct SharedAllocChunkData *SharedAllocChunk;
typedef struct SharedAllocSetData *SharedAllocSet;

/* Chunk header - stored before each allocation */
struct SharedAllocChunkData {
    Size size;              /* Size of this chunk (including header) */
    SharedAllocChunk next;  /* Next chunk in freelist (when free) */
};

/* Block header - stored at start of each ShmemAlloc'd block */
struct SharedAllocBlockData {
    SharedAllocBlock next;  /* Next block in list */
    Size size;              /* Total size of this block */
    Size used;              /* Bytes used in this block */
};

/* Free list link - stored in chunk data area when chunk is free */
typedef struct SharedFreeListLink {
    SharedAllocChunk next;
} SharedFreeListLink;

/* Get freelist link from chunk pointer */
#define SharedGetFreeListLink(chkptr) \
    ((SharedFreeListLink *)((char *)(chkptr) + SHARED_CHUNKHDRSZ))

/* Determine chunk size from freelist index */
#define SharedGetChunkSizeFromIdx(fidx) \
    (((Size)1) << (SHARED_ALLOC_MINBITS + (fidx)))

/* Get freelist index for a given size (0-indexed) */
static inline int SharedGetFreeListIdx(Size size) {
    if (size == 0)
        return -1;
    int idx = 0;
    Size chunk_size = 8;
    while (chunk_size < size && idx < SHARED_ALLOC_NUM_FREELISTS - 1) {
        chunk_size <<= 1;
        idx++;
    }
    return idx;
}

/* Main context structure */
struct SharedAllocSetData {
    MemoryContextData header;
    
    /* Allocation parameters */
    Size initBlockSize;
    Size maxBlockSize;
    Size nextBlockSize;
    
    /* Blocks allocated from ShmemAlloc */
    SharedAllocBlock blocks;
    
    /* Free lists - one for each power-of-2 size class */
    SharedAllocChunk freelists[SHARED_ALLOC_NUM_FREELISTS];
    
    /* Global free list for returned memory (cross-pool reuse) */
    SharedAllocChunk global_freelist;
    
    /* Statistics */
    Size totalAllocated;
    Size totalFree;
    Size numAllocations;
    
    /* Thread safety */
    slock_t lock;
};

/* Public API */
extern MemoryContext SharedAllocSetCreate(const char *name,
                                          Size initBlockSize,
                                          Size maxBlockSize);

/* Direct alloc/free API that bypasses PG pfree.
 *
 * Why: PG 18+ pfree expects a packed MemoryChunk header (hdrmask) immediately
 * before the user pointer, encoding the MemoryContextMethodID. SharedAllocSet
 * cannot register a method ID in the PG kernel's MemoryContextMethodID enum
 * (it's an internal PG enum), so chunks allocated via MemoryContextAlloc on
 * a SharedAllocSet have a zeroed hdrmask. Calling pfree() on these pointers
 * triggers "pfree called with invalid pointer ... (header 0x0000000000000000)"
 * and PANIC.
 *
 * Use these direct APIs from any code that allocated on vecbuf_shared_ctx
 * (or any other SharedAllocSet context) when it needs to free the memory.
 */
#ifdef __cplusplus
extern "C" {
#endif
extern void SharedAllocSet_Free(MemoryContext context, void *pointer);
#ifdef __cplusplus
}
#endif

#endif /* SHARED_ALLOC_SET_H */
