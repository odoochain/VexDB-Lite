// GraphBridge 实现：GraphIndexAlgorithm × SQLite 单线程 MemStore。
//
// include 顺序约定（duck 同款）：宿主依赖头必须先于 algorithm.h。
#include "vex_graph_index_depend_sqlite.hpp"
#include "graph_index/graph_index_algorithm.h"

#include "distance/core/distance.h"
#include "distance/core/distance_dispatcher.h"
#include "distance/core/distance_utils_core.h"

#include "index/graph_bridge.h"

#include <cmath>
#include <cstring>
#include <exception>
#include <thread>
#include <unordered_map>

namespace vexdb_sqlite {

namespace {

using SqliteStore = MemStore<uint32, GraphIndexPoint>;
using SqliteDiskStore = DiskStore<uint32, GraphIndexPoint>;
using SqMetricList = MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>;
using SqDTypeList = DistPrecisionTypeList<DistPrecisionType::FLOAT>;

Metric ToCoreMetric(VexMetric m) {
    switch (m) {
    case VexMetric::L2: return Metric::L2;
    case VexMetric::COSINE: return Metric::COSINE;
    case VexMetric::INNER_PRODUCT: return Metric::INNER_PRODUCT;
    }
    return Metric::L2;
}

// 照抄 duck RunWithDuckAlgo（graph_index.cpp:53）：dispatcher 选出 distancer，
// 就地实例化算法对象执行 fn。Store 参数化：MemStore（全内存）/ DiskStore（段式）。
template <typename Store, typename Fn>
auto RunWithAlgo(Metric metric, uint16_t dim, int ef_construction, int m, Store &store,
                 Fn &&fn) {
    return DispatchRunner<false, SqMetricList, SqDTypeList, DispatcherMode::NO_QUANT>::call(
        metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
        [&](auto &distancer) -> decltype(auto) {
            using DistT = std::decay_t<decltype(distancer)>;
            using AlgoT = GraphIndexAlgorithm<Store, DistT>;
            AlgoT algo(uint_fast16_t(ef_construction), uint_fast16_t(m), store, distancer);
            return fn(algo);
        });
}

// ---- 序列化格式 v3（段式，M9'：%_graph(kind, seg, data)；全部小端定长记录）----
// v1（全量镜像分块）已废弃：EnsureGraph 读不出 meta 段 → fall through 重建。
// v3 = elems 支持空壳节点（tid 摘除：DELETE/UPDATE 增量化，对齐 MySQL 二级
// 索引 delete-mark+insert 范式）。空壳节点 v2 解析器也兼容（cnt=0 正常读，
// 查询零输出），读侧接受 v2；写侧统一 v3。
constexpr uint32_t kGraphBlobMagic = 0x47535856;  // 'VXSG'
constexpr uint32_t kGraphBlobVersion = 3;
constexpr uint32_t kGraphBlobMinVersion = 2;

struct BlobHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t dim;
    uint32_t m;
    uint32_t ef_construction;
    uint32_t metric;
    uint64_t base_count;
    uint64_t upper_count;
    uint64_t entry_id;
    uint64_t entry_cur_layer_idx;
    int32_t entry_level;
    uint32_t seg_records;  // v2：base/vec 每段记录数（写入时固定，读侧以此为准）
};

template <typename T>
void AppendPod(std::vector<char> &out, const T &v) {
    const char *p = reinterpret_cast<const char *>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool ReadPod(const char *&p, const char *end, T &v) {
    if (p + sizeof(T) > end) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

}  // namespace

struct GraphBridge::Impl {
    uint16_t dim;
    int m;
    int ef_construction;
    VexMetric metric;
    Metric core_metric;
    SqliteStore store;                          // 全内存模式（disk 为空时生效）
    std::unique_ptr<SqliteDiskStore> disk;      // DiskStore 模式（非空=段式懒加载）
    // tid 摘除（DELETE/UPDATE 增量化）：rowid→节点 id 反查 map 懒构建（首次
    // RemoveTid 时从 elems 扫描，之后随 Insert/RemoveTid 维护）；dead_nodes=
    // tids 摘空的空壳节点数（占 ef 槽位，Search 补偿+阈值重建判据）。
    std::unordered_map<int64_t, uint32_t> rowid_to_node;
    bool rowid_map_built = false;
    size_t dead_nodes = 0;
    bool elems_dirty = false;  // tid 摘除待落盘（disk 模式 HasDirty 用）
    // mem 模式落盘形态：true=段与持久化不一致需清段+全量重写（重建/BuildBulk
    // 后首次落盘）；false=可按 MemStore 段级 dirty 集增量 UPSERT（增量
    // Insert/RemoveTid 的 commit 免每次重写全图——1M 驻留图单行 insert 原要
    // 清段+重写 ~0.8GB）。
    bool full_dirty = false;

    template <typename ElemsVec>
    void build_rowid_map(const ElemsVec &elems) {
        rowid_to_node.clear();
        rowid_to_node.reserve(elems.size() * 2);
        dead_nodes = 0;
        for (size_t i = 0; i < elems.size(); i++) {
            if (elems[i].tids.empty()) {
                dead_nodes++;
                continue;
            }
            for (const auto &t : elems[i].tids) rowid_to_node[t.row_id] = uint32_t(i);
        }
        rowid_map_built = true;
    }

    Impl(uint16_t dim_in, int m_in, int efc_in, VexMetric metric_in)
        : dim(dim_in), m(m_in), ef_construction(efc_in), metric(metric_in),
          core_metric(ToCoreMetric(metric_in)),
          store(dim_in, uint_fast16_t(m_in), uint_fast32_t(dim_in) * sizeof(float)) {
        store.normalize_vectors_ = (metric == VexMetric::COSINE);
    }
};

GraphBridge::GraphBridge(uint16_t dim, int m, int ef_construction, VexMetric metric)
    : impl_(new Impl(dim, m, ef_construction, metric)) {}

GraphBridge::~GraphBridge() = default;

void GraphBridge::Insert(const float *vec, int64_t rowid) {
    auto &im = *impl_;
    const size_t before = im.disk ? im.disk->elems.size() : im.store.elems.size();
    auto run = [&](auto &store) {
        RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, store, [&](auto &algo) {
            using AlgoT = std::decay_t<decltype(algo)>;
            PointExtensionContext pctx;
            ItemPointerData tid{};
            tid.row_id = rowid;
            typename AlgoT::InsertContextBase ctx(pctx, reinterpret_cast<const char *>(vec), &tid);
            algo.insert(ctx);
            return 0;
        });
    };
    if (im.disk) {
        run(*im.disk);
    } else {
        run(im.store);
    }
    // rowid→node 懒 map 增量维护：节点数 +1 = 新节点（assign_vector_id 顺序
    // 分配，必在尾部）→ 追加一条；节点数不变 = dedup 把 tid 挂到既有节点
    //（节点 id 不可知）→ 才保守作废。原先无条件作废让交替 INSERT/DELETE
    // 工作负载每次删除付 O(N) 全 elems 重扫。
    if (im.rowid_map_built) {
        const size_t after = im.disk ? im.disk->elems.size() : im.store.elems.size();
        if (after == before + 1) {
            im.rowid_to_node[rowid] = uint32_t(after - 1);
        } else {
            im.rowid_map_built = false;
        }
    }
}

// 批量建图（duck BuildBulk 三段式）：首点串行立 entry → 单线程回退 → 连续
// 切片 spawn std::thread。worker 是纯计算（只碰 store/算法），异常聚合后抛。
// 仅全内存模式（DiskStore 模式的两阶段构建见 M9'c）。
void GraphBridge::BuildBulk(const float *vecs, const int64_t *rowids, size_t n, int n_threads) {
    auto &im = *impl_;
    if (n == 0) return;
    // 构建期关闭段级 dirty 追踪（8 线程标记会 race 且无意义——全新图首次
    // 落盘必然全量）；构建后标 full_dirty 走全量重写协议。
    im.store.track_dirty_ = false;

    RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, im.store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        auto insert_one = [&](size_t i) {
            PointExtensionContext pctx;
            ItemPointerData tid{};
            tid.row_id = rowids[i];
            typename AlgoT::InsertContextBase ctx(
                pctx, reinterpret_cast<const char *>(vecs + i * im.dim), &tid);
            algo.insert(ctx);
        };

        // Phase A：首点串行（建 entry，避免空图竞争升级协议）
        insert_one(0);
        if (n == 1) return 0;

        size_t rest = n - 1;
        int workers = n_threads;
        if (workers > int(rest)) workers = int(rest);
        // Phase B：单线程回退
        if (workers <= 1) {
            for (size_t i = 1; i < n; i++) insert_one(i);
            return 0;
        }

        // Phase C：并行。预留容量（内层向量 buffer 预 resize=publish 安全的根基；
        // upper 预留按 1/(m-1) 几何级数的期望上界放大）。
        im.store.ReserveCapacity(n, n / size_t(im.m > 2 ? im.m - 1 : 1) + 64);
        im.store.parallel_build_active_.store(true, std::memory_order_release);

        std::atomic<size_t> next{1};
        std::atomic<bool> failed{false};
        std::exception_ptr first_error = nullptr;
        std::mutex err_mu;
        auto worker = [&]() {
            constexpr size_t kBatch = 64;
            while (!failed.load(std::memory_order_relaxed)) {
                size_t begin = next.fetch_add(kBatch, std::memory_order_relaxed);
                if (begin >= n) break;
                size_t end = begin + kBatch < n ? begin + kBatch : n;
                try {
                    for (size_t i = begin; i < end; i++) insert_one(i);
                } catch (...) {
                    std::lock_guard<std::mutex> g(err_mu);
                    if (!first_error) first_error = std::current_exception();
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        };
        std::vector<std::thread> threads;
        threads.reserve(size_t(workers));
        for (int t = 0; t < workers; t++) threads.emplace_back(worker);
        for (auto &th : threads) th.join();
        im.store.parallel_build_active_.store(false, std::memory_order_release);
        if (first_error) std::rethrow_exception(first_error);
        return 0;
    });
    // 恢复增量追踪 + 全新图首次落盘走全量；rowid map 已不可信
    im.store.track_dirty_ = true;
    im.store.dirty_base_segs_.clear();
    im.store.dirty_vec_segs_.clear();
    im.full_dirty = true;
    im.rowid_map_built = false;
}

void GraphBridge::Search(const float *query, size_t k, uint32_t ef_search,
                         std::vector<std::pair<double, int64_t>> &out) {
    Search(query, k, ef_search, nullptr, out);
}

void GraphBridge::Search(const float *query, size_t k, uint32_t ef_search,
                         const std::function<bool(int64_t)> &filter,
                         std::vector<std::pair<double, int64_t>> &out) {
    auto &im = *impl_;
    out.clear();
    if (Count() == 0 || k == 0) return;

    // cosine：归一化 query 副本，使 raw=-cos_sim、1+raw 与 M2 暴力路径数值一致
    //（不只排序一致）。L2/IP 直接用原始 query。
    const float *q = query;
    std::vector<float> normalized;
    if (im.metric == VexMetric::COSINE) {
        normalized.assign(query, query + im.dim);
        float norm2 = 0.0f;
        for (uint16_t i = 0; i < im.dim; i++) norm2 += normalized[i] * normalized[i];
        if (norm2 > 0.0f) {
            float inv = 1.0f / std::sqrt(norm2);
            for (uint16_t i = 0; i < im.dim; i++) normalized[i] *= inv;
        }
        q = normalized.data();
    }

    uint32_t ef = std::max<uint32_t>(uint32_t(k), ef_search);
    // 空壳（tid 摘空）节点占据遍历与结果槽位但产不出行，按存活比放大 ef
    //（上限 2×）。空壳节点的零输出由 tids 展开天然保证，无需显式过滤。
    if (im.dead_nodes > 0) {
        size_t n = Count();
        size_t live = n > im.dead_nodes ? n - im.dead_nodes : 1;
        ef = uint32_t(std::min<double>(double(ef) * double(n) / double(live), double(ef) * 2.0));
    }
    auto run = [&](auto &store) {
        RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, store, [&](auto &algo) {
            PointExtensionContext pctx;
            // 节点级 filter：dedup 后一个 point 可挂多个 rowid，任一通过即保留节点
            // （结果输出端再按 rowid 精确过滤掉混进来的不满足行）。空壳节点
            // 无 tid 可通过，天然不进结果。
            auto node_filter = [&](uint32 id) -> bool {
                for (const auto &t : store.elems[id].tids) {
                    if (filter(t.row_id)) return true;
                }
                return false;
            };
            auto emit = [&](const auto &res) {
                for (size_t i = 0; i < res.size() && out.size() < k; i++) {
                    if (filter && !filter(res[i].tid.row_id)) continue;
                    double d = res[i].dist;
                    switch (im.metric) {
                    case VexMetric::L2: d = std::sqrt(d); break;          // raw=平方距离
                    case VexMetric::COSINE: d = 1.0 + d; break;           // raw=-cos_sim
                    case VexMetric::INNER_PRODUCT: break;                 // raw=-ip 即 neg_ip
                    }
                    out.emplace_back(d, res[i].tid.row_id);
                }
            };
            if (filter) {
                emit(algo.search(pctx, reinterpret_cast<const char *>(q), ef, node_filter));
            } else {
                emit(algo.search(pctx, reinterpret_cast<const char *>(q), ef));
            }
            return 0;
        });
    };
    if (im.disk) {
        run(*im.disk);
    } else {
        run(im.store);
    }
}

bool GraphBridge::RemoveTid(int64_t rowid) {
    auto &im = *impl_;
    auto with_elems = [&](auto &elems) -> bool {
        if (!im.rowid_map_built) im.build_rowid_map(elems);
        auto it = im.rowid_to_node.find(rowid);
        if (it == im.rowid_to_node.end()) return false;
        auto &tids = elems[it->second].tids;
        for (size_t i = 0; i < tids.size(); i++) {
            if (tids[i].row_id == rowid) {
                tids.erase(tids.begin() + i);
                break;
            }
        }
        if (tids.empty()) im.dead_nodes++;
        im.rowid_to_node.erase(it);
        im.elems_dirty = true;
        return true;
    };
    return im.disk ? with_elems(im.disk->elems) : with_elems(im.store.elems);
}

size_t GraphBridge::DeadNodeCount() const {
    return impl_->dead_nodes;
}

size_t GraphBridge::Count() const {
    return impl_->disk ? impl_->disk->get_vector_num() : impl_->store.get_vector_num();
}

bool GraphBridge::IsDiskMode() const {
    return impl_->disk != nullptr;
}

bool GraphBridge::HasDirty() const {
    return impl_->elems_dirty || (impl_->disk && impl_->disk->has_dirty());
}

namespace {

// ---- v2 段式序列化辅助（MemStore 全量 / DiskStore 增量共用） ----

constexpr int kKindMeta = SqliteDiskStore::KIND_META;
constexpr int kKindElems = SqliteDiskStore::KIND_ELEMS;
constexpr int kKindUpper = SqliteDiskStore::KIND_UPPER;
constexpr int kKindBase = SqliteDiskStore::KIND_BASE;
constexpr int kKindVec = SqliteDiskStore::KIND_VEC;
constexpr size_t kSegRecords = SqliteDiskStore::SEG_RECORDS;

// 载入后初始化空壳节点计数（tid 摘除产生的 dead 节点，序列化随 elems 走）。
template <typename ElemsVec>
size_t CountDeadNodes(const ElemsVec &elems) {
    size_t dead = 0;
    for (const auto &e : elems) {
        if (e.tids.empty()) dead++;
    }
    return dead;
}

void BuildMetaBlob(uint16_t dim, int m, int efc, VexMetric metric, size_t base_n,
                   size_t upper_n, const GraphIndexEntryInfo &entry, std::vector<char> &out) {
    out.clear();
    BlobHeader h{};
    h.magic = kGraphBlobMagic;
    h.version = kGraphBlobVersion;
    h.dim = dim;
    h.m = uint32_t(m);
    h.ef_construction = uint32_t(efc);
    h.metric = uint32_t(metric);
    h.base_count = base_n;
    h.upper_count = upper_n;
    h.entry_id = entry.id;
    h.entry_cur_layer_idx = entry.cur_layer_idx;
    h.entry_level = entry.level;
    h.seg_records = uint32_t(kSegRecords);
    AppendPod(out, h);
}

template <typename ElemsVec>
void BuildElemsBlob(const ElemsVec &elems, size_t base_n, std::vector<char> &out) {
    out.clear();
    for (size_t i = 0; i < base_n; i++) {
        const auto &tids = elems[i].tids;
        AppendPod(out, uint32_t(tids.size()));
        for (const auto &t : tids) AppendPod(out, int64_t(t.row_id));
    }
}

bool ParseElemsBlob(const std::vector<char> &blob, size_t base_n,
                    std::vector<GraphIndexPoint> &elems, std::string &err) {
    const char *p = blob.data();
    const char *end = p + blob.size();
    elems.resize(base_n);
    for (size_t i = 0; i < base_n; i++) {
        uint32_t cnt = 0;
        if (!ReadPod(p, end, cnt)) {
            err = "graph v2 elems blob truncated";
            return false;
        }
        auto &tids = elems[i].tids;
        tids.resize(cnt);
        for (uint32_t j = 0; j < cnt; j++) {
            int64_t rid = 0;
            if (!ReadPod(p, end, rid)) {
                err = "graph v2 elems blob truncated in rowids";
                return false;
            }
            tids[j] = ItemPointerData{};
            tids[j].row_id = rid;
        }
    }
    return true;
}

template <typename UpperVec>
void BuildUpperBlob(const UpperVec &ups, size_t upper_n, int m, std::vector<char> &out) {
    const size_t nb = size_t(m) * 2;
    out.clear();
    for (size_t i = 0; i < upper_n; i++) {
        const auto &up = ups[i];
        AppendPod(out, up.lower_layer_idx);
        AppendPod(out, up.id);
        out.insert(out.end(), reinterpret_cast<const char *>(up.neighbors_info.data()),
                   reinterpret_cast<const char *>(up.neighbors_info.data() + nb));
        out.insert(out.end(), reinterpret_cast<const char *>(up.dists.data()),
                   reinterpret_cast<const char *>(up.dists.data() + size_t(m)));
    }
}

using UpperRec = SqliteStore::UpperPointRec;

bool ParseUpperBlob(const std::vector<char> &blob, size_t upper_n, int m,
                    std::vector<UpperRec> &ups, std::string &err) {
    const size_t nb = size_t(m) * 2;
    const char *p = blob.data();
    const char *end = p + blob.size();
    ups.resize(upper_n);
    for (size_t i = 0; i < upper_n; i++) {
        auto &up = ups[i];
        up.neighbors_info.resize(nb);
        up.dists.resize(size_t(m));
        up.stat_words.assign((size_t(m) + 31) / 32, 0);
        if (!ReadPod(p, end, up.lower_layer_idx) || !ReadPod(p, end, up.id) ||
            p + nb * sizeof(uint32) + size_t(m) * sizeof(float) > end) {
            err = "graph v2 upper blob truncated";
            return false;
        }
        std::memcpy(up.neighbors_info.data(), p, nb * sizeof(uint32));
        p += nb * sizeof(uint32);
        std::memcpy(up.dists.data(), p, size_t(m) * sizeof(float));
        p += size_t(m) * sizeof(float);
    }
    return true;
}

// 读 meta 段并校验参数。失败填 err 返回 false。
bool ReadMetaV2(const GraphBridge::SegReadFn &read, uint16_t dim, int m, VexMetric metric,
                BlobHeader &h, std::string &err) {
    std::vector<char> buf;
    if (!read(kKindMeta, 0, buf)) {
        err = "graph v2 meta segment missing";
        return false;
    }
    const char *p = buf.data();
    const char *end = p + buf.size();
    if (!ReadPod(p, end, h)) {
        err = "graph v2 meta too short";
        return false;
    }
    if (h.magic != kGraphBlobMagic) {
        err = "graph v2 bad magic";
        return false;
    }
    if (h.version < kGraphBlobMinVersion || h.version > kGraphBlobVersion) {
        err = "graph format v" + std::to_string(h.version) + " unsupported";
        return false;
    }
    if (h.dim != dim || int(h.m) != m || VexMetric(h.metric) != metric) {
        err = "graph v2 params mismatch index config";
        return false;
    }
    if (h.seg_records != kSegRecords) {
        err = "graph v2 seg_records mismatch";
        return false;
    }
    return true;
}

}  // namespace

bool GraphBridge::SerializeV2(const SegWriteFn &write) {
    auto &im = *impl_;
    std::vector<char> buf;

    if (im.disk) {
        // DiskStore：常驻部分（meta/elems/upper/deleted）全量重写，base/vec
        // 只写 dirty 段
        auto &ds = *im.disk;
        const size_t base_n = ds.elems.size();
        const size_t upper_n = ds.upper_points.size();
        BuildMetaBlob(im.dim, im.m, im.ef_construction, im.metric, base_n, upper_n,
                      ds.entry_info, buf);
        if (!write(kKindMeta, 0, buf)) return false;
        BuildElemsBlob(ds.elems, base_n, buf);
        if (!write(kKindElems, 0, buf)) return false;
        BuildUpperBlob(ds.upper_points, upper_n, im.m, buf);
        if (!write(kKindUpper, 0, buf)) return false;
        bool ok = true;
        ds.flush_dirty_segs([&](int kind, uint32 seg, const std::vector<char> &data) -> bool {
            bool w = write(kind, seg, data);
            if (!w) ok = false;
            return w;  // 失败段保留 dirty，重试 flush 时重写
        });
        if (ok) {
            ds.upper_dirty = false;
            im.elems_dirty = false;
        }
        return ok;
    }

    // 全内存：常驻三件套全量重写 + base/vec 段（full=全部段，调用方已清旧段；
    // 增量=仅 MemStore 段级 dirty 集，UPSERT 覆盖——单行 insert 的 commit 从
    // 重写全图降为 ~每邻居触碰段，1M 驻留图 ~0.8GB → 几百 KB）。
    auto &st = im.store;
    const size_t base_n = st.base_points.size();
    const size_t upper_n = st.upper_points.size();
    const size_t nb = size_t(im.m) * 2;
    const size_t base_rec = nb * (sizeof(uint32) + sizeof(float));
    const size_t vec_size = st.vec_size;

    BuildMetaBlob(im.dim, im.m, im.ef_construction, im.metric, base_n, upper_n,
                  st.entry_info, buf);
    if (!write(kKindMeta, 0, buf)) return false;
    BuildElemsBlob(st.elems, base_n, buf);
    if (!write(kKindElems, 0, buf)) return false;
    BuildUpperBlob(st.upper_points, upper_n, im.m, buf);
    if (!write(kKindUpper, 0, buf)) return false;
    im.elems_dirty = false;

    auto write_base_seg = [&](size_t seg) -> bool {
        buf.assign(kSegRecords * base_rec, 0);
        for (size_t r = 0; r < kSegRecords; r++) {
            char *rec = buf.data() + r * base_rec;
            size_t idx = seg * kSegRecords + r;
            if (idx < base_n) {
                const auto &bp = st.base_points[idx];
                std::memcpy(rec, bp.neighbors.data(), nb * sizeof(uint32));
                std::memcpy(rec + nb * sizeof(uint32), bp.dists.data(), nb * sizeof(float));
            } else {
                auto *neighbors = reinterpret_cast<uint32 *>(rec);
                auto *dists = reinterpret_cast<float *>(rec + nb * sizeof(uint32));
                for (size_t i = 0; i < nb; i++) {
                    neighbors[i] = uint32(INVALID_VECTOR_ID);
                    dists[i] = INVALID_DIST;
                }
            }
        }
        return write(kKindBase, uint32(seg), buf);
    };
    auto write_vec_seg = [&](size_t seg) -> bool {
        buf.assign(kSegRecords * vec_size, 0);
        for (size_t r = 0; r < kSegRecords; r++) {
            size_t idx = seg * kSegRecords + r;
            if (idx < base_n) {
                std::memcpy(buf.data() + r * vec_size, st.vectors[idx].data(), vec_size);
            }
        }
        return write(kKindVec, uint32(seg), buf);
    };

    if (im.full_dirty) {
        const size_t n_segs = (base_n + kSegRecords - 1) / kSegRecords;
        for (size_t seg = 0; seg < n_segs; seg++) {
            if (!write_base_seg(seg)) return false;
        }
        for (size_t seg = 0; seg < n_segs; seg++) {
            if (!write_vec_seg(seg)) return false;
        }
        im.full_dirty = false;
        st.dirty_base_segs_.clear();
        st.dirty_vec_segs_.clear();
        return true;
    }
    // 增量：失败段保留在 dirty 集供重试
    for (auto it = st.dirty_base_segs_.begin(); it != st.dirty_base_segs_.end();) {
        if (!write_base_seg(*it)) return false;
        it = st.dirty_base_segs_.erase(it);
    }
    for (auto it = st.dirty_vec_segs_.begin(); it != st.dirty_vec_segs_.end();) {
        if (!write_vec_seg(*it)) return false;
        it = st.dirty_vec_segs_.erase(it);
    }
    return true;
}

bool GraphBridge::NeedsFullRewrite() const {
    return !impl_->disk && impl_->full_dirty;
}

int64_t GraphBridge::PeekPersistedNodeCount(const SegReadFn &read) {
    std::vector<char> buf;
    if (!read || !read(kKindMeta, 0, buf)) return -1;
    const char *p = buf.data();
    const char *end = p + buf.size();
    BlobHeader h{};
    if (!ReadPod(p, end, h) || h.magic != kGraphBlobMagic) return -1;
    return int64_t(h.base_count);
}

std::unique_ptr<GraphBridge> GraphBridge::OpenV2(const SegReadFn &read, uint16_t dim, int m,
                                                 int ef_construction, VexMetric metric,
                                                 std::string &err) {
    BlobHeader h{};
    if (!ReadMetaV2(read, dim, m, metric, h, err)) return nullptr;

    auto bridge = std::make_unique<GraphBridge>(dim, m, ef_construction, metric);
    auto &st = bridge->impl_->store;
    const size_t base_n = size_t(h.base_count);
    const size_t upper_n = size_t(h.upper_count);
    const size_t nb = size_t(m) * 2;
    const size_t base_rec = nb * (sizeof(uint32) + sizeof(float));

    st.ResizeForReload(base_n, upper_n);

    std::vector<char> buf;
    if (!read(kKindElems, 0, buf) || !ParseElemsBlob(buf, base_n, st.elems, err))
        return nullptr;
    if (!read(kKindUpper, 0, buf) || !ParseUpperBlob(buf, upper_n, m, st.upper_points, err))
        return nullptr;
    bridge->impl_->dead_nodes = CountDeadNodes(st.elems);

    const size_t n_segs = (base_n + kSegRecords - 1) / kSegRecords;
    for (size_t seg = 0; seg < n_segs; seg++) {
        if (!read(kKindBase, uint32(seg), buf) || buf.size() != kSegRecords * base_rec) {
            err = "graph v2 base segment missing/short";
            return nullptr;
        }
        size_t lo = seg * kSegRecords;
        size_t hi = std::min(base_n, lo + kSegRecords);
        for (size_t idx = lo; idx < hi; idx++) {
            const char *rec = buf.data() + (idx - lo) * base_rec;
            auto &bp = st.base_points[idx];
            std::memcpy(bp.neighbors.data(), rec, nb * sizeof(uint32));
            std::memcpy(bp.dists.data(), rec + nb * sizeof(uint32), nb * sizeof(float));
        }
    }
    for (size_t seg = 0; seg < n_segs; seg++) {
        if (!read(kKindVec, uint32(seg), buf) || buf.size() != kSegRecords * st.vec_size) {
            err = "graph v2 vec segment missing/short";
            return nullptr;
        }
        size_t lo = seg * kSegRecords;
        size_t hi = std::min(base_n, lo + kSegRecords);
        for (size_t idx = lo; idx < hi; idx++) {
            st.vectors[idx].assign(buf.data() + (idx - lo) * st.vec_size,
                                   buf.data() + (idx - lo + 1) * st.vec_size);
        }
    }
    st.entry_info.set(size_t(h.entry_id), size_t(h.entry_cur_layer_idx),
                      int_fast8_t(h.entry_level));
    return bridge;
}

std::unique_ptr<GraphBridge> GraphBridge::OpenV2Disk(const SegReadFn &read,
                                                     const SegWriteFn &write,
                                                     const SegRecReadFn &read_rec, uint16_t dim,
                                                     int m, int ef_construction,
                                                     VexMetric metric, size_t cache_budget,
                                                     std::string &err) {
    BlobHeader h{};
    if (!ReadMetaV2(read, dim, m, metric, h, err)) return nullptr;

    auto bridge = std::make_unique<GraphBridge>(dim, m, ef_construction, metric);
    auto &im = *bridge->impl_;
    SqliteDiskStore::PageIO io;
    io.read = read;
    io.write = write;
    io.read_rec = read_rec;
    im.disk = std::make_unique<SqliteDiskStore>(dim, uint_fast16_t(m),
                                                uint_fast32_t(dim) * sizeof(float),
                                                std::move(io), cache_budget);
    auto &ds = *im.disk;
    ds.normalize_vectors_ = (metric == VexMetric::COSINE);

    const size_t base_n = size_t(h.base_count);
    const size_t upper_n = size_t(h.upper_count);
    std::vector<char> buf;
    if (!read(kKindElems, 0, buf) || !ParseElemsBlob(buf, base_n, ds.elems, err))
        return nullptr;
    if (!read(kKindUpper, 0, buf) || !ParseUpperBlob(buf, upper_n, m, ds.upper_points, err))
        return nullptr;
    im.dead_nodes = CountDeadNodes(ds.elems);
    ds.reset_capacity(base_n, upper_n);
    ds.entry_info.set(size_t(h.entry_id), size_t(h.entry_cur_layer_idx),
                      int_fast8_t(h.entry_level));
    ds.upper_dirty = false;
    return bridge;
}

}  // namespace vexdb_sqlite
