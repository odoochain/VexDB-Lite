/*
 * local_vec_cache.h - per-backend 进程私有向量 buffer 缓存
 *
 * 目的:消除高并发(reads=16)下 VecBufferManager::get_buffer 对唯一共享 locmap
 * (boost concurrent_flat_map)的 reader 自旋锁争用。火焰图实测 get_buffer self
 * 占 74% CPU,其中 boost rw_spinlock::lock_shared 的 cmpxchg 自旋 ≈55% 全局 CPU
 * (16 backend 重访 HNSW hub 节点,cacheline 在 state_ 锁字上乒乓)。
 *
 * 方案(纯 scan 作用域 + borrow-no-recount):
 *  - 每 backend 一个 thread_local 直接映射缓存 sig→VecBufferLoc。
 *  - 缓存条目持有一份"常驻 pin"(tag.ref_count+1)。因 do_evict / invalidate 都仅在
 *    ref_count==0 时回收 slot(vector_smgr.cpp:393/895/931),常驻 pin 保证该 slot 的
 *    tag.sig 在缓存存活期内不变 → 命中无需校验、不碰 locmap 锁。
 *  - 命中:纯 hash lookup,返回哨兵 VecBuffer(release 为 no-op),零原子零锁。
 *  - 未命中:走原 get_buffer(其 pin 即作为常驻 pin),插入缓存;冲突桶覆盖时 release
 *    被挤出条目的常驻 pin。
 *  - 作用域绑定单次 HNSW search(graph_index_scan.cpp 的 algo.search):入口 activate,
 *    出口 flush(release 全部常驻 pin)。flush 边界极小,规避跨 backend 失效/旧数据问题
 *    (search 内只读、buf 不跨另一次 get_buffer 持有,已确认安全)。
 *  - longjmp(ereport)兜底:调用点用 PG_TRY/PG_CATCH 保证异常路径也 flush。
 *  - crash 兜底:PG crash-restart 重新 init vecbuf 共享内存 + backend 进程重启,
 *    thread_local 缓存随进程消失,泄漏的 ref_count 随共享内存重置清零。
 */

#ifndef LOCAL_VEC_CACHE_H
#define LOCAL_VEC_CACHE_H

#include "vector_buffer/vector_buffer_manager.h"   /* BufferSignature, VecBufferLoc */

/* VecBuffer.pool_offset 哨兵:本地缓存借用,release() 为 no-op(pin 归缓存所有) */
constexpr int16 VECBUF_LOCAL_BORROW = -2;

struct LocalVecCacheEntry {
    BufferSignature sig;
    VecBufferLoc loc;       /* 常驻 pin 持有的 slot (valid bit 已清,纯 buf_offset/offset) */
    uint32 dim;             /* 用于 get_vector 复算 buf 指针 */
    int16 pool_offset;
    bool valid;
};

class LocalVecCache {
public:
    static constexpr uint32 NBUCKET = 512u;          /* 2 的幂;常驻 pin 上限 = NBUCKET */
    static constexpr uint32 MASK = NBUCKET - 1u;

    /* 进入一次 search:开启缓存。HNSW search 内 visited 去重,单次 search 内每 id 只访问
     * 一次 → 命中来自 hub 节点跨查询复用,故缓存内容跨 search 常驻(deactivate 不 flush)。
     * 持久 pin 由直接映射缓存的冲突淘汰 bound 在 NBUCKET 个;失效由 invalidate hook 处理
     * (见 vec_invalidate_buffer_cache)。 */
    void activate() { ++depth_; }
    void deactivate()
    {
        Assert(depth_ > 0);
        --depth_;
        /* 诊断:周期性打印命中率(每 backend) */
        if (depth_ == 0 && (hits_ + misses_) >= 200000ull) {
            elog(LOG, "[lvc] pid=%d hits=%llu misses=%llu hit_rate=%.1f%%",
                 (int)getpid(), (unsigned long long)hits_, (unsigned long long)misses_,
                 100.0 * (double)hits_ / (double)(hits_ + misses_ + 1ull));
            hits_ = 0;
            misses_ = 0;
        }
    }
    bool active() const { return depth_ > 0; }
    void note_hit() { ++hits_; }
    void note_miss() { ++misses_; }

    /* 命中返回 true 并填 out_loc/out_po/out_dim;否则 false。命中即有效(常驻 pin 保 sig 不变)。 */
    bool lookup(const BufferSignature &sig, VecBufferLoc &out_loc, int16 &out_po, uint32 &out_dim) const
    {
        const LocalVecCacheEntry &e = table_[bucket(sig)];
        if (e.valid && e.sig == sig) {
            out_loc = e.loc;
            out_po = e.pool_offset;
            out_dim = e.dim;
            return true;
        }
        return false;
    }

    /* 插入新条目;条目持有传入 loc 对应 slot 的常驻 pin(由调用方刚 pin 得到)。
     * 桶被占用则先 release 旧条目的常驻 pin(冲突淘汰)。 */
    void insert(const BufferSignature &sig, const VecBufferLoc &loc, int16 po, uint32 dim)
    {
        LocalVecCacheEntry &e = table_[bucket(sig)];
        if (e.valid) {
            release_resident_pin(e.loc);
        }
        e.sig = sig;
        e.loc = loc;
        e.dim = dim;
        e.pool_offset = po;
        e.valid = true;
    }

    /* release 全部常驻 pin 并清空。幂等(空表 flush 为 no-op)。 */
    void flush()
    {
        for (uint32 i = 0; i < NBUCKET; ++i) {
            if (table_[i].valid) {
                release_resident_pin(table_[i].loc);
                table_[i].valid = false;
            }
        }
    }

private:
    static uint32 bucket(const BufferSignature &sig)
    {
        return (uint32)BufferSignature::Hasher()(sig) & MASK;
    }
    /* 定义在 .cpp(依赖 release_vector_buffer) */
    static void release_resident_pin(const VecBufferLoc &loc);

    LocalVecCacheEntry table_[NBUCKET] = {};
    uint32 depth_ = 0;
    unsigned long long hits_ = 0;
    unsigned long long misses_ = 0;
};

extern thread_local LocalVecCache g_local_vec_cache;

#endif /* LOCAL_VEC_CACHE_H */
