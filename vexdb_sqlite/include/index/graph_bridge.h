// GraphBridge —— vtab 与 GraphIndexAlgorithm 之间的桥（M3）。
//
// 轻头：算法重模板全部藏在 graph_bridge.cpp（pimpl），vtab 只见这个接口。
// 单线程（SQLite 串行模型）；M3+ 并行建图在 cpp 内扩展。
//
// 距离输出为"用户语义"（与 M2 暴力路径/标量函数严格一致，lower=closer）：
//   L2 = sqrt(raw)，COSINE = 1 + raw（query 已归一化），IP = raw（即负内积）。
#ifndef VEXDB_SQLITE_GRAPH_BRIDGE_H
#define VEXDB_SQLITE_GRAPH_BRIDGE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vex_distance_entry.h"

namespace vexdb_sqlite {

class GraphBridge {
public:
    GraphBridge(uint16_t dim, int m, int ef_construction, VexMetric metric);
    ~GraphBridge();
    GraphBridge(const GraphBridge &) = delete;
    GraphBridge &operator=(const GraphBridge &) = delete;

    // 增量插入一条向量（HNSW algo.insert，真增量非重建）。
    void Insert(const float *vec, int64_t rowid);

    // 批量建图（M3+）：vecs 平铺 n×dim，rowids 长度 n。n_threads<=1 退化串行；
    // 多线程时三段式（首点串行 → 连续切片 spawn worker），调用方保证构建期独占
    // 且 vecs/rowids 在返回前有效。线程为纯计算，绝不触碰 sqlite3 句柄。
    void BuildBulk(const float *vecs, const int64_t *rowids, size_t n, int n_threads);

    // KNN：返回 (用户语义 distance, rowid) 升序，最多 k 条。
    // cosine 时内部归一化 query 副本。
    void Search(const float *query, size_t k, uint32_t ef_search,
                std::vector<std::pair<double, int64_t>> &out);

    // 带谓词 KNN（M7' 图内过滤）：filter 返回 false 的 rowid 不进结果，
    // 但其节点仍参与图遍历导航（filtered-HNSW 标准语义，连通性不受谓词影响）。
    // ef_search 由调用方按选择性补偿后传入；filter 为空等价无过滤。
    void Search(const float *query, size_t k, uint32_t ef_search,
                const std::function<bool(int64_t)> &filter,
                std::vector<std::pair<double, int64_t>> &out);

    size_t Count() const;

    // ---- 格式 v2：段式（M9'，%_graph(kind, seg, data)）----
    // kind：0=meta 1=elems 2=upper 3=base 段 4=vec 段（base/vec 按 4096 条定长
    // 记录切段）。read 返回 false=段不存在；write 必须在宿主写事务内调用。
    using SegReadFn = std::function<bool(int kind, uint32_t seg, std::vector<char> &out)>;
    using SegWriteFn = std::function<bool(int kind, uint32_t seg, const std::vector<char> &data)>;

    // 是否 DiskStore 模式（OpenV2Disk 打开；内存有界，段 LRU 懒加载）。
    bool IsDiskMode() const;
    // DiskStore 模式专用：有未落盘的 dirty 段/常驻改动（全内存模式恒 false，
    // dirty 由 vtab 的 graph_dirty 管）。
    bool HasDirty() const;

    // 序列化写出 v2 段。全内存模式=全量切段；DiskStore 模式=dirty 段 +
    // meta/elems/upper（常驻部分体量小，全量重写）。返回 false=write 回调失败。
    // 非 const：DiskStore 模式写出后清 dirty 标记。
    bool SerializeV2(const SegWriteFn &write);

    // 从 v2 段全量载入（全内存模式，性能形态）。无 meta 段/校验失败返回
    // nullptr 并填 err。
    static std::unique_ptr<GraphBridge> OpenV2(const SegReadFn &read, uint16_t dim, int m,
                                               int ef_construction, VexMetric metric,
                                               std::string &err);

    // DiskStore 懒加载打开（内存有界形态）：meta/elems/upper 常驻，base/vec
    // 段按需 LRU（cache_budget 字节）。write 可为空（只读打开：查询期段永不
    // dirty，evict 纯丢弃）。
    static std::unique_ptr<GraphBridge> OpenV2Disk(const SegReadFn &read, const SegWriteFn &write,
                                                   uint16_t dim, int m, int ef_construction,
                                                   VexMetric metric, size_t cache_budget,
                                                   std::string &err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vexdb_sqlite

#endif  // VEXDB_SQLITE_GRAPH_BRIDGE_H
