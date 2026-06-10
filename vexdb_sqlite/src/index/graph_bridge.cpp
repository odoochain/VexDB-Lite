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

namespace vexdb_sqlite {

namespace {

using SqliteStore = MemStore<uint32, GraphIndexPoint>;
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
// 就地实例化算法对象执行 fn。
template <typename Fn>
auto RunWithAlgo(Metric metric, uint16_t dim, int ef_construction, int m, SqliteStore &store,
                 Fn &&fn) {
    return DispatchRunner<false, SqMetricList, SqDTypeList, DispatcherMode::NO_QUANT>::call(
        metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
        [&](auto &distancer) -> decltype(auto) {
            using DistT = std::decay_t<decltype(distancer)>;
            using AlgoT = GraphIndexAlgorithm<SqliteStore, DistT>;
            AlgoT algo(uint_fast16_t(ef_construction), uint_fast16_t(m), store, distancer);
            return fn(algo);
        });
}

// ---- 序列化格式 v1（全部小端、紧凑定长记录；详见 docs/design M3 契约 §6） ----
constexpr uint32_t kGraphBlobMagic = 0x47535856;  // 'VXSG'
constexpr uint32_t kGraphBlobVersion = 1;

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
    uint32_t pad = 0;
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
    SqliteStore store;

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
    RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, im.store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        PointExtensionContext pctx;
        ItemPointerData tid{};
        tid.row_id = rowid;
        typename AlgoT::InsertContextBase ctx(pctx, reinterpret_cast<const char *>(vec), &tid);
        algo.insert(ctx);
        return 0;
    });
}

void GraphBridge::Search(const float *query, size_t k, uint32_t ef_search,
                         std::vector<std::pair<double, int64_t>> &out) {
    auto &im = *impl_;
    out.clear();
    if (im.store.get_vector_num() == 0 || k == 0) return;

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
    RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, im.store, [&](auto &algo) {
        PointExtensionContext pctx;
        auto res = algo.search(pctx, reinterpret_cast<const char *>(q), ef);
        for (size_t i = 0; i < res.size() && out.size() < k; i++) {
            double d = res[i].dist;
            switch (im.metric) {
            case VexMetric::L2: d = std::sqrt(d); break;          // raw=平方距离
            case VexMetric::COSINE: d = 1.0 + d; break;           // raw=-cos_sim
            case VexMetric::INNER_PRODUCT: break;                 // raw=-ip 即 neg_ip
            }
            out.emplace_back(d, res[i].tid.row_id);
        }
        return 0;
    });
}

size_t GraphBridge::Count() const {
    return impl_->store.get_vector_num();
}

void GraphBridge::SerializeToBlob(std::vector<char> &out) const {
    const auto &im = *impl_;
    const auto &st = im.store;
    const size_t base_n = st.base_points.size();
    const size_t upper_n = st.upper_points.size();
    const size_t nb = size_t(im.m) * 2;

    out.clear();
    BlobHeader h{};
    h.magic = kGraphBlobMagic;
    h.version = kGraphBlobVersion;
    h.dim = im.dim;
    h.m = uint32_t(im.m);
    h.ef_construction = uint32_t(im.ef_construction);
    h.metric = uint32_t(im.metric);
    h.base_count = base_n;
    h.upper_count = upper_n;
    h.entry_id = st.entry_info.id;
    h.entry_cur_layer_idx = st.entry_info.cur_layer_idx;
    h.entry_level = st.entry_info.level;
    AppendPod(out, h);

    // base 段：neighbors[m*2] + dists[m*2]（定长记录）
    for (size_t i = 0; i < base_n; i++) {
        const auto &bp = st.base_points[i];
        out.insert(out.end(), reinterpret_cast<const char *>(bp.neighbors.data()),
                   reinterpret_cast<const char *>(bp.neighbors.data() + nb));
        out.insert(out.end(), reinterpret_cast<const char *>(bp.dists.data()),
                   reinterpret_cast<const char *>(bp.dists.data() + nb));
    }
    // upper 段：lower_layer_idx + id + neighbors_info[m*2] + dists[m]
    for (size_t i = 0; i < upper_n; i++) {
        const auto &up = st.upper_points[i];
        AppendPod(out, up.lower_layer_idx);
        AppendPod(out, up.id);
        out.insert(out.end(), reinterpret_cast<const char *>(up.neighbors_info.data()),
                   reinterpret_cast<const char *>(up.neighbors_info.data() + nb));
        out.insert(out.end(), reinterpret_cast<const char *>(up.dists.data()),
                   reinterpret_cast<const char *>(up.dists.data() + im.m));
    }
    // vec 段：向量平铺（cosine 下为归一化后的存储形态，与图一致）
    for (size_t i = 0; i < base_n; i++) {
        out.insert(out.end(), st.vectors[i].data(), st.vectors[i].data() + st.vec_size);
    }
    // rowid 段：tid_count + row_id 列表
    for (size_t i = 0; i < base_n; i++) {
        const auto &tids = st.elems[i].tids;
        AppendPod(out, uint32_t(tids.size()));
        for (const auto &t : tids) {
            AppendPod(out, int64_t(t.row_id));
        }
    }
}

std::unique_ptr<GraphBridge> GraphBridge::LoadFromBlob(const char *data, size_t len,
                                                       uint16_t dim, int m, int ef_construction,
                                                       VexMetric metric, std::string &err) {
    const char *p = data;
    const char *end = data + len;
    BlobHeader h{};
    if (!ReadPod(p, end, h)) {
        err = "graph blob too short";
        return nullptr;
    }
    if (h.magic != kGraphBlobMagic) {
        err = "graph blob bad magic";
        return nullptr;
    }
    if (h.version > kGraphBlobVersion) {
        err = "graph blob format v" + std::to_string(h.version) + " newer than supported";
        return nullptr;
    }
    if (h.dim != dim || int(h.m) != m || VexMetric(h.metric) != metric) {
        err = "graph blob params mismatch index config";
        return nullptr;
    }

    auto bridge = std::make_unique<GraphBridge>(dim, m, ef_construction, metric);
    auto &st = bridge->impl_->store;
    const size_t base_n = size_t(h.base_count);
    const size_t upper_n = size_t(h.upper_count);
    const size_t nb = size_t(m) * 2;

    // 防御：段总长校验（定长段可精确预算，rowid 段变长逐条查）
    st.ResizeForReload(base_n, upper_n);
    for (size_t i = 0; i < base_n; i++) {
        auto &bp = st.base_points[i];
        if (p + nb * sizeof(uint32_t) + nb * sizeof(float) > end) {
            err = "graph blob truncated in base section";
            return nullptr;
        }
        std::memcpy(bp.neighbors.data(), p, nb * sizeof(uint32_t));
        p += nb * sizeof(uint32_t);
        std::memcpy(bp.dists.data(), p, nb * sizeof(float));
        p += nb * sizeof(float);
    }
    for (size_t i = 0; i < upper_n; i++) {
        auto &up = st.upper_points[i];
        if (!ReadPod(p, end, up.lower_layer_idx) || !ReadPod(p, end, up.id) ||
            p + nb * sizeof(uint32_t) + size_t(m) * sizeof(float) > end) {
            err = "graph blob truncated in upper section";
            return nullptr;
        }
        std::memcpy(up.neighbors_info.data(), p, nb * sizeof(uint32_t));
        p += nb * sizeof(uint32_t);
        std::memcpy(up.dists.data(), p, size_t(m) * sizeof(float));
        p += size_t(m) * sizeof(float);
    }
    for (size_t i = 0; i < base_n; i++) {
        if (p + st.vec_size > end) {
            err = "graph blob truncated in vec section";
            return nullptr;
        }
        st.vectors[i].assign(p, p + st.vec_size);
        p += st.vec_size;
    }
    for (size_t i = 0; i < base_n; i++) {
        uint32_t cnt = 0;
        if (!ReadPod(p, end, cnt)) {
            err = "graph blob truncated in rowid section";
            return nullptr;
        }
        auto &tids = st.elems[i].tids;
        tids.resize(cnt);
        for (uint32_t j = 0; j < cnt; j++) {
            int64_t rid = 0;
            if (!ReadPod(p, end, rid)) {
                err = "graph blob truncated in rowid entries";
                return nullptr;
            }
            tids[j] = ItemPointerData{};
            tids[j].row_id = rid;
        }
    }
    st.entry_info.set(size_t(h.entry_id), size_t(h.entry_cur_layer_idx),
                      int_fast8_t(h.entry_level));
    return bridge;
}

}  // namespace vexdb_sqlite
