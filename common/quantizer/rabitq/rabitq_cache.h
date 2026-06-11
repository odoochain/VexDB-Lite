/**
 * Copyright (c) 2026 VexDB-THU
 * RaBitQ Cache — process-local cache entry type.
 *
 * The actual cache is implemented in vexdb_pg/src/quantizer/rabitq_adapter.cpp
 * using a per-process std::unordered_map.  This header only provides the
 * RaBitQCache struct used by rabitq_distancer.h and the aligned-allocation
 * macros used by rabitq.h.
 */

#ifndef RABITQ_CACHE_H
#define RABITQ_CACHE_H

#include "platform/platform_compat.h"

#define RABITQ_CACHE_ALLOC_ALIGNED(size) mem_align_alloc((ann_helper::vector_aligned_size), (size))
#define RABITQ_CACHE_FREE_ALIGNED(ptr) mem_align_free(ptr)
#define RABITQ_CACHE_FREE_ALIGNED_EXT(ptr)  \
    do {                                    \
        if (ptr) {                          \
            RABITQ_CACHE_FREE_ALIGNED(ptr); \
            ptr = NULL;                     \
        }                                   \
    } while (0)

namespace rabitq {

typedef struct RaBitQCache
{
    Oid oid{InvalidOid}; /* index oid */
    char *fixed_data{NULL}; /* random matrix + centroids + rotated_centroids */

    RaBitQCache() = default;
}   RaBitQCache;

} /* namespace rabitq */

#endif /* RABITQ_CACHE_H */
