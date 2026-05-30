/*
 * local_vec_cache.cpp - per-backend 向量 buffer 缓存实现
 * 见 include/local_vec_cache.h 的方案说明。
 */

#include "pg_compat.h"     /* PG 基础类型(MemoryContext/Datum/palloc 等),须先于下面 */

#include "vector_buffer/local_vec_cache.h"
#include "vector_buffer/vector_smgr.h"   /* release_vector_buffer */

thread_local LocalVecCache g_local_vec_cache;

void LocalVecCache::release_resident_pin(const VecBufferLoc &loc)
{
    /* 常驻 pin 即 tag.ref_count+1;释放就是 fetch_sub。loc 已为 valid 形式。 */
    release_vector_buffer(loc);
}
