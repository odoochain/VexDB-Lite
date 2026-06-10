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

    // KNN：返回 (用户语义 distance, rowid) 升序，最多 k 条。
    // cosine 时内部归一化 query 副本。
    void Search(const float *query, size_t k, uint32_t ef_search,
                std::vector<std::pair<double, int64_t>> &out);

    size_t Count() const;

    // 序列化整图为单 blob（格式版本见 cpp 内 kGraphBlobVersion）。
    void SerializeToBlob(std::vector<char> &out) const;

    // 从 blob 还原。参数（dim/m/metric）不匹配或格式不识别返回 nullptr 并填 err。
    static std::unique_ptr<GraphBridge> LoadFromBlob(const char *data, size_t len,
                                                     uint16_t dim, int m, int ef_construction,
                                                     VexMetric metric, std::string &err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vexdb_sqlite

#endif  // VEXDB_SQLITE_GRAPH_BRIDGE_H
