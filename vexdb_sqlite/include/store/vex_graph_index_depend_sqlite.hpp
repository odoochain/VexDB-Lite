// SQLite 宿主依赖头 —— GraphIndexAlgorithm 的注入点（对标
// vex_graph_index_depend_duck.hpp 的角色，经 common/include/graph_index/
// graph_index_depend.h 的 PG_VEXDB_TARGET_SQLITE 分支进入）。
//
// 与 DuckDB 版的差异：
//   - 纯内存 MemStore：无 FixedSizeAllocator/BlockManager 双写路径（SQLite 的
//     持久化走 %_graph shadow 表序列化，见 M3 桥接层），数组即唯一事实源。
//   - 单线程：M3 全部锁为 no-op（SQLite 串行模型 + 构建期独占）。M3+ 并行建图
//     时在此补 striped lock 与 publish fence（接口位已保留，语义见 duck 版
//     add_upperpoint 的 publish race 注释——那是必须正面解决的唯一真险点）。
//   - ItemPointerData 用 SQLite rowid（int64）。
//
// store 命名沿用 MemStore：algorithm.h 用 std::is_same<Store, MemStore<T,pt>>
// 判定 mem_store 特化分支（need_refine/has_est 关断），同名让分支自动正确。
#pragma once

#include <atomic>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "vtl/span"
// 注意：vtl/bitvector 依赖下方的 Assert/unlikely 宏，在宏定义之后才 include
// （对齐 duck.hpp 的顺序约定）。

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint = unsigned int;
using Size = size_t;

class BaseObject {};

struct PointExtensionContext;

enum class DistPrecisionType : uint8_t;

#ifndef Assert
#define Assert(cond) ((void)0)
#endif
#ifndef CONSTEXPR_IF
#define CONSTEXPR_IF if constexpr
#endif
#ifndef likely
#define likely(x) (x)
#endif
#ifndef unlikely
#define unlikely(x) (x)
#endif
#ifndef INVALID_VECTOR_ID
#define INVALID_VECTOR_ID SIZE_MAX
#endif
#ifndef INVALID_DIST
#define INVALID_DIST FLT_MAX
#endif
#ifndef GRAPH_INDEX_NORM_PROC
#define GRAPH_INDEX_NORM_PROC 2
#endif
#ifndef CHECK_FOR_INTERRUPTS
#define CHECK_FOR_INTERRUPTS() ((void)0)
#endif

#include "vtl/bitvector"

// PG elog shim（algorithm.h 的 REPORT_PERF(NOTICE) 等需要；对齐 duck_pg_shim）。
enum {
    DEBUG5 = 10, DEBUG4 = 11, DEBUG3 = 12, DEBUG2 = 13, DEBUG1 = 14,
    LOG = 15, INFO = 17, NOTICE = 18, WARNING = 19, ERROR = 20, FATAL = 21, PANIC = 22,
};

#ifndef elog
#include <cstdio>
#include <stdexcept>
#define elog(level, ...) \
    do { \
        if ((level) >= ERROR) { \
            char _buf[512]; \
            std::snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
            throw std::runtime_error(_buf); \
        } else { \
            std::fprintf(stderr, "[vexdb-sqlite] " __VA_ARGS__); \
            std::fprintf(stderr, "\n"); \
        } \
    } while (0)
#endif

using Oid = uint32_t;

inline bool OidIsValid(Oid oid) { return oid != 0; }

inline Oid index_getprocid(void *, int, int) { return 0; }

#include "quantizer_type.h"

using Relation = void *;

// SQLite 行标识：rowid（int64）。保留 PG TID 字段形状以兼容算法层可能的
// 字段触碰，但语义只有 row_id。
struct BlockIdData {
    uint16 bi_hi = 0;
    uint16 bi_lo = 0;
};

struct ItemPointerData {
    BlockIdData ip_blkid;
    uint16 ip_posid = 0;
    int64_t row_id = 0;
};

using ItemPointer = ItemPointerData *;

inline bool ItemPointerEquals(ItemPointerData *a, ItemPointerData *b) {
    return a->row_id == b->row_id;
}

struct GraphIndexMetaPageData {
    uint16 ef_construction = 64;
    uint16 m = 16;
};
using GraphIndexMetaPage = GraphIndexMetaPageData *;

struct GraphIndexSearchRes {
    ItemPointerData tid;
    float dist;
    bool operator<(const GraphIndexSearchRes &other) const { return dist < other.dist; }
};

template <typename T>
struct GraphIndexCandidate {
    T id;
    T cur_layer_idx;
    T lower_layer_idx;
    float dist;
    const char *val;

    GraphIndexCandidate()
        : id((T)INVALID_VECTOR_ID), cur_layer_idx((T)INVALID_VECTOR_ID),
          lower_layer_idx((T)INVALID_VECTOR_ID), dist(INVALID_DIST), val(nullptr) {}
    GraphIndexCandidate(T id_val, T cur_idxx, float dist_val)
        : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx((T)INVALID_VECTOR_ID),
          dist(dist_val), val(nullptr) {}
    GraphIndexCandidate(T id_val, T cur_idxx, T lower_idx, float dist_val, const char *val_ptr)
        : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx(lower_idx), dist(dist_val),
          val(val_ptr) {}
};

struct GraphIndexEntryInfo {
    size_t id;
    size_t cur_layer_idx;
    int_fast8_t level;

    GraphIndexEntryInfo() : id(INVALID_VECTOR_ID), cur_layer_idx(INVALID_VECTOR_ID), level(-1) {}
    void set(size_t new_id, size_t new_cur_layer_idx, int_fast8_t new_level) {
        id = new_id;
        cur_layer_idx = new_cur_layer_idx;
        level = new_level;
    }
};

struct GraphIndexPoint {
    using Data = ItemPointerData;
    uint8 new_inserted = 0;
    std::vector<Data> tids;

    // 并行 BuildBulk 时 dedup 可把多个 worker 路由到同一 point 的 tids；
    // 进程级粗锁串行化（duck 同款取舍：build 一次性，正确性优先）。
    static std::shared_mutex &tid_lock() {
        static std::shared_mutex m;
        return m;
    }

    bool empty() const { return tids.empty(); }
    // 非模板重载不可删：algorithm.h 以 braced initializer {ptr, size} 调用，
    // 模板形参推导不了 initializer list。
    bool insert_tid(PointExtensionContext &, Span<const Data> data, bool &overwritten) {
        std::unique_lock<std::shared_mutex> _lk(tid_lock());
        overwritten = false;
        for (size_t i = 0; i < data.size(); ++i) {
            tids.push_back(data[i]);
            overwritten = true;
        }
        return true;
    }
    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data) {
        bool overwritten = false;
        return insert_tid(ctx, data, overwritten);
    }
    template <typename SpanLike>
    bool insert_tid(PointExtensionContext &, SpanLike data, bool &overwritten) {
        std::unique_lock<std::shared_mutex> _lk(tid_lock());
        overwritten = false;
        for (size_t i = 0; i < data.size(); ++i) {
            tids.push_back(data[i]);
            overwritten = true;
        }
        return true;
    }
    template <typename SpanLike>
    bool insert_tid(PointExtensionContext &ctx, SpanLike data) {
        bool overwritten = false;
        return insert_tid(ctx, data, overwritten);
    }
    template <typename Vec>
    uint32 get_tids(Vec &out, struct PointExtensionContext &) const {
        std::shared_lock<std::shared_mutex> _lk(tid_lock());
        for (const auto &tid : this->tids) {
            out.push_back(tid);
        }
        return uint32(this->tids.size());
    }
    template <typename Func>
    uint32 vacuum_tids(Func &&func, PointExtensionContext &, bool &dirty) {
        dirty = false;
        auto it = std::remove_if(tids.begin(), tids.end(), [&](const auto &tid) {
            if (func(tid)) {
                dirty = true;
                return true;
            }
            return false;
        });
        tids.erase(it, tids.end());
        return uint32(tids.size());
    }
};

struct PointExtensionContext {
    void destroy() {}
};

struct RepairGraphSharedState {
    void *deleted = nullptr;
    std::atomic<size_t> *base_counter = nullptr;
    std::atomic<size_t> *upper_counter = nullptr;
    size_t basepoint_num = 0;
    size_t upperpoint_num = 0;
    size_t base_batch_size = 0;
    size_t upper_batch_size = 0;
};

constexpr int GRAPH_INDEX_MAX_LEVEL = 32;

inline void vacuum_delay_point(bool) {}

template <typename T>
inline T Min(const T &a, const T &b) {
    return a < b ? a : b;
}

template <typename IdType = uint32, typename elem_type = GraphIndexPoint>
class MemStore;

template <typename IdType, typename elem_type>
class MemStore {
public:
    using T = IdType;
    using point_type = elem_type;

    static constexpr bool use_dist_cache = false;
    static constexpr bool has_occlusion_cache = true;
    static constexpr bool clustered = false;

    struct BasePointRec {
        std::vector<T> neighbors;
        std::vector<float> dists;
        std::vector<uint32> stat_words;
    };
    struct UpperPointRec {
        T lower_layer_idx = T(INVALID_VECTOR_ID);
        T id = T(INVALID_VECTOR_ID);
        std::vector<T> neighbors_info;  // [m 个 ID | m 个 cur_layer_idx]
        std::vector<float> dists;
        std::vector<uint32> stat_words;
    };

    uint_fast16_t dim = 0;
    uint_fast16_t m = 0;
    uint_fast32_t vec_size = 0;

    GraphIndexEntryInfo entry_info;

    std::vector<point_type> elems;
    std::vector<std::vector<char>> vectors;
    std::vector<BasePointRec> base_points;
    std::vector<UpperPointRec> upper_points;
    std::vector<T> async_ids;

    bool normalize_vectors_ = false;

    // ---- 并发原语（M3+ 并行 BuildBulk）。语义对照 duck 版：----
    //   entry_mutex_      : 保护 entry_info（升级协议见 get_entry）
    //   entry_wait_mutex_ : 升级前抢 wait gate 阻拦后续 reader（防写者饥饿）
    //   elems_mutex_      : 保护 elems/vectors/base_points/upper_points 外层扩容
    //   striped locks     : 64 路按 idx&MASK 寻址的 per-node 锁（base/upper 两组）
    //   parallel_build_active_ : true=并行建图中（get_data 走 SHARED 锁）；
    //                            false=单线程/查询期（免锁快路径）
    // 单线程路径上这些锁全部 uncontended（~ns 级），不设跳锁开关。
    mutable std::shared_mutex entry_mutex_;
    mutable std::mutex entry_wait_mutex_;
    mutable std::shared_mutex elems_mutex_;
    static constexpr size_t STRIPE_COUNT = 64;
    static constexpr size_t STRIPE_MASK = STRIPE_COUNT - 1;
    mutable std::shared_mutex base_point_locks_[STRIPE_COUNT];
    mutable std::shared_mutex upper_point_locks_[STRIPE_COUNT];
    std::atomic<bool> parallel_build_active_{false};

    // 原子层 size：assign_vector_id fast-path 不持锁读（acquire），slow-path 在
    // EXCLUSIVE elems_mutex_ 下扩容后 release 写——建立 happens-before。
    std::atomic<size_t> base_size_{0};
    std::atomic<size_t> upper_size_{0};

    std::atomic<T> next_base_id_{0};
    std::atomic<T> next_upper_id_{0};

    MemStore() = default;
    MemStore(uint_fast16_t dim_in, uint_fast16_t m_in, uint_fast32_t vec_size_in)
        : dim(dim_in), m(m_in), vec_size(vec_size_in) {
        entry_info.set(INVALID_VECTOR_ID, INVALID_VECTOR_ID, -1);
    }

    // 并行 build 前调用：预留外层容量 + 预 resize 内层向量 buffer，使并行期
    // add_vector 只 memcpy 进稳定指针（防 get_data 读侧 dangling——duck 的
    // parallel-build publish race 教训）。
    void ReserveCapacity(size_t base_n, size_t upper_n) {
        std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
        elems.reserve(base_n);
        vectors.reserve(base_n);
        base_points.reserve(base_n);
        upper_points.reserve(upper_n);
        if (base_n > elems.size()) {
            size_t cur = vectors.size();
            elems.resize(base_n);
            vectors.resize(base_n);
            for (size_t i = cur; i < base_n; i++) {
                vectors[i].resize(vec_size);
            }
            base_points.resize(base_n, MakeBasePoint());
            base_size_.store(base_n, std::memory_order_release);
        }
        if (upper_n > upper_points.size()) {
            upper_points.resize(upper_n, MakeUpperPoint());
            upper_size_.store(upper_n, std::memory_order_release);
        }
    }

    // 双锁升级协议（照 duck/主库）：SHARED 读 entry；需要更高 level 或图空时
    // 先抢 wait gate 再升 EXCLUSIVE，防写者饥饿。
    template <bool exclusive = false, bool bottom_only = false>
    std::pair<GraphIndexEntryInfo, bool> get_entry(int_fast8_t insert_level = 0) {
        (void)bottom_only;
        entry_wait_mutex_.lock();
        entry_wait_mutex_.unlock();

        entry_mutex_.lock_shared();
        GraphIndexEntryInfo entry = entry_info;
        bool shared = true;
        if ((!exclusive && insert_level > entry.level) || entry.level < 0) {
            entry_mutex_.unlock_shared();
            entry_wait_mutex_.lock();
            entry_mutex_.lock();
            entry_wait_mutex_.unlock();
            entry = entry_info;
            shared = false;
        }
        return {entry, shared};
    }

    void release_entry_lock(bool shared) {
        if (shared) {
            entry_mutex_.unlock_shared();
        } else {
            entry_mutex_.unlock();
        }
    }

    // fast path：atomic fetch_add 拿独占 id，已在预留容量内则零锁返回；
    // slow path：EXCLUSIVE elems_mutex_ 下扩容（增量 Append 或未预留时）。
    template <bool is_base_layer>
    T assign_vector_id() {
        if constexpr (is_base_layer) {
            T id = next_base_id_.fetch_add(1, std::memory_order_acq_rel);
            if (size_t(id) < base_size_.load(std::memory_order_acquire)) {
                return id;
            }
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (size_t(id) + 1 > elems.size()) {
                elems.resize(id + 1);
                vectors.resize(id + 1);
                base_points.resize(id + 1, MakeBasePoint());
            }
            if (vectors[id].size() != (size_t)vec_size) {
                vectors[id].resize(vec_size);
            }
            base_size_.store(elems.size(), std::memory_order_release);
            return id;
        } else {
            T idx = next_upper_id_.fetch_add(1, std::memory_order_acq_rel);
            if (size_t(idx) < upper_size_.load(std::memory_order_acquire)) {
                return idx;
            }
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (size_t(idx) + 1 > upper_points.size()) {
                upper_points.resize(idx + 1, MakeUpperPoint());
            }
            upper_size_.store(upper_points.size(), std::memory_order_release);
            return idx;
        }
    }

    void add_async_id(T id) {
        std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
        async_ids.push_back(id);
    }

    // elems[id]/vectors[id] 的 id 维度由 assign_vector_id 独占分配不竞争；
    // SHARED elems_mutex_ 防外层 realloc 让引用失效；tid_lock 串行化 dedup
    // 路由到同一 id 的并发 push_back（duck 同款双锁）。
    void add_elem(PointExtensionContext &, T id, const ItemPointerData &tid) {
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        std::unique_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
        elems[id].tids.push_back(tid);
    }
    void add_elem(PointExtensionContext &, T id, Span<const ItemPointerData> tids) {
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        std::unique_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
        for (const auto &tid : tids) {
            elems[id].tids.push_back(tid);
        }
    }

    void add_vector(T id, const char *query) {
        const char *store_data = query;
        std::vector<char> normalized;
        if (normalize_vectors_) {
            normalized.resize(vec_size);
            auto *dst = reinterpret_cast<float *>(normalized.data());
            auto *src = reinterpret_cast<const float *>(query);
            float norm2 = 0.0f;
            for (uint_fast16_t i = 0; i < dim; i++) {
                norm2 += src[i] * src[i];
            }
            if (norm2 > 0.0f) {
                float inv_norm = 1.0f / std::sqrt(norm2);
                for (uint_fast16_t i = 0; i < dim; i++) {
                    dst[i] = src[i] * inv_norm;
                }
            } else {
                std::memcpy(dst, src, vec_size);
            }
            store_data = normalized.data();
        }
        // SHARED：防外层 realloc。ReserveCapacity 已预 resize 内层 buffer →
        // 热路径 memcpy 进稳定指针（不 realloc，不挪 data() 指针）。
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        auto &slot = vectors[id];
        if (slot.size() == (size_t)vec_size) {
            std::memcpy(slot.data(), store_data, vec_size);
        } else {
            slot.assign(store_data, store_data + vec_size);
        }
    }

    template <typename Distancer>
    void add_vector(Distancer &, T id, const char *query) {
        add_vector(id, query);
    }

    void set_entrypoint(T id, T cur_layer_idx, int_fast8_t level) {
        entry_info.set(id, cur_layer_idx, level);
    }

    void *get_index() const { return nullptr; }
    DistPrecisionType get_precision() const { return static_cast<DistPrecisionType>(0); }
    uint16 get_dim() const { return uint16(dim); }
    uint32 get_vecsize() const { return uint32(vec_size); }
    uint32 get_elemsize() const { return uint32(vec_size); }
    size_t get_vector_num() const { return elems.size(); }

    template <bool is_base_layer>
    size_t max_id() const {
        if constexpr (is_base_layer) {
            return std::max<size_t>(next_base_id_.load(std::memory_order_relaxed),
                                    base_points.size()) + 16;
        } else {
            return std::max<size_t>(next_upper_id_.load(std::memory_order_relaxed),
                                    upper_points.size()) + 16;
        }
    }

    void ResizeForReload(size_t base_n, size_t upper_n) {
        elems.resize(base_n);
        vectors.resize(base_n);
        base_points.resize(base_n, MakeBasePoint());
        upper_points.resize(upper_n, MakeUpperPoint());
        next_base_id_.store(T(base_n), std::memory_order_relaxed);
        next_upper_id_.store(T(upper_n), std::memory_order_relaxed);
    }

    char *get_data_unlocked(T id) {
        if (id >= vectors.size()) return nullptr;
        return vectors[id].data();
    }
    const char *get_data_unlocked(T id) const {
        if (id >= vectors.size()) return nullptr;
        return vectors[id].data();
    }
    // 并行 build 期取 SHARED（防外层 realloc 撕裂读）；其余时间免锁快路径
    //（SQLite 查询是单线程 xFilter，无并发写）。
    char *get_data(T id) {
        if (!parallel_build_active_.load(std::memory_order_acquire)) {
            return get_data_unlocked(id);
        }
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        return get_data_unlocked(id);
    }
    const char *get_data(T id) const {
        if (!parallel_build_active_.load(std::memory_order_acquire)) {
            return get_data_unlocked(id);
        }
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        return get_data_unlocked(id);
    }

    struct my_buf {
        const char *c;
        explicit my_buf(const char *c) : c(c) {}
        char *get_vecbuf() const { return (char *)c; }
        static constexpr void release() {}
    };
    my_buf read_data(T id) { return my_buf{get_data(id)}; }
    my_buf read_data(T id) const { return my_buf{get_data(id)}; }

    void reset_neighbors_val_pool() {}

    template <typename Distancer, typename IdVec>
    void get_distance_batch(const Distancer &distancer, const char *query, const IdVec &ids,
                            float *dists) {
        std::vector<void *> vals;
        vals.reserve(ids.size());
        for (auto id : ids) {
            vals.push_back(get_data(id));
        }
        distancer.get_distance_batch2(query, vals.data(), uint16(dim), uint16(vals.size()), dists);
    }

    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, T id) {
        return distancer.get_distance_single(query, get_data(id), uint16(dim));
    }
    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, const char *val) {
        return distancer.get_distance_single(query, val, uint16(dim));
    }
    template <typename Distancer>
    float get_distance_precise(const Distancer &distancer, const char *query, const char *val) {
        return distancer.get_distance_single(query, val, uint16(dim));
    }
    template <typename Distancer>
    float get_distance_est(const Distancer &distancer, const char *query, T id) {
        return get_distance(distancer, query, id);
    }

    // striped per-node 锁：idx & MASK 命中 64 路之一。查询期（非并行 build）
    // 无并发写，锁 uncontended，不设跳锁开关（duck 的 search_lock_free_ 是
    // 高并发查询优化，SQLite 串行查询不需要）。
    template <bool is_base_layer, bool shared_lock>
    void lock_point(T idx) {
        auto &lk = (is_base_layer ? base_point_locks_ : upper_point_locks_)[idx & STRIPE_MASK];
        if constexpr (shared_lock) {
            lk.lock_shared();
        } else {
            lk.lock();
        }
    }
    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T idx) {
        auto &lk = (is_base_layer ? base_point_locks_ : upper_point_locks_)[idx & STRIPE_MASK];
        if constexpr (shared_lock) {
            lk.unlock_shared();
        } else {
            lk.unlock();
        }
    }

    template <bool is_base_layer>
    auto get_point_info(T idx) {
        if constexpr (is_base_layer) {
            auto &bp = base_points[idx];
            return std::make_tuple(bp.neighbors.data(), idx, idx);
        } else {
            auto &up = upper_points[idx];
            return std::make_tuple(up.neighbors_info.data(), up.lower_layer_idx, up.id);
        }
    }

    template <bool is_base_layer, typename CandVec, typename CandType>
    void get_neighbors(CandVec &out, const CandType &cand) {
        if constexpr (is_base_layer) {
            auto &bp = base_points[cand.cur_layer_idx];
            const T *neighbors = bp.neighbors.data();
            const float *dists = bp.dists.data();
            out.reserve(m * 2);
            for (size_t i = 0; i < size_t(m) * 2; ++i) {
                if (neighbors[i] == T(INVALID_VECTOR_ID)) break;
                out.emplace_back(neighbors[i], neighbors[i], dists[i]);
            }
        } else {
            auto &up = upper_points[cand.cur_layer_idx];
            const T *neighbors = up.neighbors_info.data();
            const T *cur_layer_idxs = neighbors + m;
            const float *dists = up.dists.data();
            out.reserve(m);
            for (size_t i = 0; i < size_t(m); ++i) {
                if (neighbors[i] == T(INVALID_VECTOR_ID)) break;
                out.emplace_back(neighbors[i], cur_layer_idxs[i], dists[i]);
            }
        }
    }

    template <bool is_base_layer>
    auto get_neighbor_stats(T idx) {
        if constexpr (is_base_layer) {
            auto &bp = base_points[idx];
            return std::make_pair(bp.dists.data(),
                                  BitSpan<uint32>(bp.stat_words.data(), bp.neighbors.size()));
        } else {
            auto &up = upper_points[idx];
            return std::make_pair(up.dists.data(), BitSpan<uint32>(up.stat_words.data(), size_t(m)));
        }
    }

    bool has_stat(BitSpan<uint32>) const { return false; }
    void set_stat(BitSpan<uint32>) {}

    template <bool is_base_layer>
    void set_neighbor(T cur_layer_idx, int16 pruned, T newpoint_id, T newpoint_cur_layer_idx) {
        if (pruned < 0) return;
        if constexpr (is_base_layer) {
            auto &bp = base_points[cur_layer_idx];
            if (size_t(pruned) < bp.neighbors.size()) {
                bp.neighbors[pruned] = newpoint_id;
            }
        } else {
            auto &up = upper_points[cur_layer_idx];
            if (size_t(pruned) < size_t(m)) {
                up.neighbors_info[pruned] = newpoint_id;
                up.neighbors_info[size_t(m) + size_t(pruned)] = newpoint_cur_layer_idx;
            }
        }
    }

    void set_base_neighbors(T id, const T *neighbors_id) {
        base_points[id].neighbors.assign(neighbors_id, neighbors_id + m * 2);
    }
    void set_upper_neighbors(T idx, const T *neighbors_info) {
        upper_points[idx].neighbors_info.assign(neighbors_info, neighbors_info + m * 2);
    }

    void add_basepoint(T id, const T *neighbors_id) {
        if (id >= base_points.size()) {
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (id >= base_points.size()) {
                base_points.resize(id + 1, MakeBasePoint());
                base_size_.store(base_points.size(), std::memory_order_release);
            }
        }
        base_points[id].neighbors.assign(neighbors_id, neighbors_id + m * 2);
    }

    // publish fence（duck #26 forward-publish race 的根治同款）：另一 worker 可
    // 经反向边在本函数返回前到达 cur_layer_idx，读 up.lower_layer_idx/id；不持
    // EXCLUSIVE stripe 锁写则无 happens-before，读侧可能看到 MakeUpperPoint 的
    // INVALID 默认值并当裸 base 索引使用 → 越界。base 层免疫（lower=identity）。
    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, const T *neighbors_info) {
        if (cur_layer_idx >= upper_points.size()) {
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (cur_layer_idx >= upper_points.size()) {
                upper_points.resize(cur_layer_idx + 1, MakeUpperPoint());
                upper_size_.store(upper_points.size(), std::memory_order_release);
            }
        }
        lock_point<false, false>(cur_layer_idx);
        auto &up = upper_points[cur_layer_idx];
        up.lower_layer_idx = lower_layer_idx;
        up.id = id;
        up.neighbors_info.assign(neighbors_info, neighbors_info + m * 2);
        unlock_point<false, false>(cur_layer_idx);
    }

    template <typename Func>
    bool apply_elem(T id, Func &&func) {
        return func(elems[id]);
    }

    template <typename Func>
    void get_itempointer(T id, Func &&func) {
        func(&elems[id]);
    }

    template <typename Func>
    void for_each_async_id(Func &&func) {
        for (auto id : async_ids) {
            func(id);
        }
    }

    bool fetch_vec_from_heap(ItemPointerData tid, char *dest) {
        auto id = static_cast<size_t>(tid.row_id);
        if (id >= vectors.size()) return false;
        std::memcpy(dest, get_data(T(id)), vec_size);
        return true;
    }
    bool fetch_vec_from_heap(PointExtensionContext &, T id, char *dest) {
        if (id >= vectors.size()) return false;
        std::memcpy(dest, get_data(id), vec_size);
        return true;
    }

private:
    BasePointRec MakeBasePoint() const {
        BasePointRec bp;
        bp.neighbors.assign(m * 2, T(INVALID_VECTOR_ID));
        bp.dists.assign(m * 2, INVALID_DIST);
        bp.stat_words.assign((m * 2 + 31) / 32, 0);
        return bp;
    }
    UpperPointRec MakeUpperPoint() const {
        UpperPointRec up;
        up.neighbors_info.assign(m * 2, T(INVALID_VECTOR_ID));
        up.dists.assign(m, INVALID_DIST);
        up.stat_words.assign((m + 31) / 32, 0);
        return up;
    }
};
