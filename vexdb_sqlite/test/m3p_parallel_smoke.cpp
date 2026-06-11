// M3+ 并行建图冒烟（直接调 GraphBridge，绕过 SQL 层专测并发正确性）。
//
// 验收（计划 M3+）：并行 recall 必须对齐单线程 baseline（防 duck 端 pw=2
// recall 暴跌 83.57% 的前科）；N=40000 > 32768（吃透多次外层扩容/快慢路径
// 切换）；并行多轮重跑（概率性 race 需重复采样）；并行图序列化 round-trip。
// 线程合法性由结构保证：BuildBulk 输入是纯内存数组，worker 不碰 sqlite3。
#include "index/graph_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using vexdb_sqlite::GraphBridge;

namespace {

constexpr int kDim = 16;
#ifdef M3P_SMALL_N
constexpr size_t kN = M3P_SMALL_N;  // TSan 等慢速 instrument 下缩规模
#else
constexpr size_t kN = 40000;
#endif
constexpr size_t kQueries = 10;
constexpr size_t kK = 10;
constexpr int kParallelRounds = 3;

int g_fail = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "M3+ FAIL: %s\n", what);
        g_fail = 1;
    }
}

unsigned int g_seed = 20260610;
float frand() {
    g_seed = g_seed * 1103515245u + 12345u;
    return float((g_seed >> 8) & 0xFFFF) / 65536.0f * 2.0f - 1.0f;
}

float L2(const float *a, const float *b) {
    float s = 0;
    for (int d = 0; d < kDim; d++) {
        float diff = a[d] - b[d];
        s += diff * diff;
    }
    return std::sqrt(s);
}

// 暴力 ground truth top-k（纯内存）。
void BruteTopK(const std::vector<float> &data, const float *q, std::vector<int64_t> &ids) {
    std::vector<std::pair<float, int64_t>> all(kN);
    for (size_t i = 0; i < kN; i++) {
        all[i] = {L2(data.data() + i * kDim, q), int64_t(i + 1)};
    }
    std::partial_sort(all.begin(), all.begin() + kK, all.end());
    ids.clear();
    for (size_t i = 0; i < kK; i++) ids.push_back(all[i].second);
}

double RecallOf(GraphBridge &g, const std::vector<float> &data,
                const std::vector<std::vector<int64_t>> &truth) {
    size_t hit = 0;
    for (size_t qi = 0; qi < kQueries; qi++) {
        std::vector<std::pair<double, int64_t>> res;
        g.Search(data.data() + (qi * 977 % kN) * kDim, kK, /*ef_search=*/120, res);
        for (const auto &r : res) {
            for (int64_t t : truth[qi]) {
                if (r.second == t) { hit++; break; }
            }
        }
    }
    return double(hit) / double(kQueries * kK);
}

}  // namespace

int main() {
    std::vector<float> data(kN * kDim);
    for (auto &v : data) v = frand();
    std::vector<int64_t> rowids(kN);
    for (size_t i = 0; i < kN; i++) rowids[i] = int64_t(i + 1);

    std::vector<std::vector<int64_t>> truth(kQueries);
    for (size_t qi = 0; qi < kQueries; qi++) {
        BruteTopK(data, data.data() + (qi * 977 % kN) * kDim, truth[qi]);
    }

    // ── 单线程 baseline ──
    double recall_serial;
    {
        GraphBridge g(kDim, /*m=*/16, /*efc=*/100, VexMetric::L2);
        g.BuildBulk(data.data(), rowids.data(), kN, /*n_threads=*/1);
        check(g.Count() == kN, "serial count == N");
        recall_serial = RecallOf(g, data, truth);
        printf("serial   recall@%zu = %.4f (N=%zu)\n", kK, recall_serial, kN);
        check(recall_serial >= 0.95, "serial recall >= 0.95");
    }

    // ── 并行 ×3 轮：recall 必须对齐 baseline（容差 0.02，防暴跌型 race） ──
    for (int round = 0; round < kParallelRounds; round++) {
        GraphBridge g(kDim, 16, 100, VexMetric::L2);
        g.BuildBulk(data.data(), rowids.data(), kN, /*n_threads=*/8);
        check(g.Count() == kN, "parallel count == N");
        double r = RecallOf(g, data, truth);
        printf("parallel recall@%zu = %.4f (round %d, 8 threads)\n", kK, r, round + 1);
        check(r >= recall_serial - 0.02, "parallel recall aligns serial baseline");

        if (round == 0) {
            // 并行图 v2 段式序列化 round-trip（内存 map 模拟 %_graph 段存储）：
            // 全内存 load 与 DiskStore 懒加载两条路径 recall 都必须不变。
            std::map<std::pair<int, uint32_t>, std::vector<char>> segs;
            bool ser_ok = g.SerializeV2([&](int kind, uint32_t seg, const std::vector<char> &d) {
                segs[{kind, seg}] = d;
                return true;
            });
            check(ser_ok, "v2 serialize ok");
            auto reader = [&](int kind, uint32_t seg, std::vector<char> &out) -> bool {
                auto it = segs.find({kind, seg});
                if (it == segs.end()) return false;
                out = it->second;
                return true;
            };
            std::string err;
            auto g2 = GraphBridge::OpenV2(reader, kDim, 16, 100, VexMetric::L2, err);
            check(g2 != nullptr, "v2 full load ok");
            if (g2) {
                double r2 = RecallOf(*g2, data, truth);
                check(std::fabs(r2 - r) < 1e-9, "v2 round-trip recall identical");
            }
            // DiskStore：预算压到最小（强制段抖动），recall 仍须逐位一致
            auto g3 = GraphBridge::OpenV2Disk(reader, nullptr, kDim, 16, 100, VexMetric::L2,
                                              /*cache_budget=*/1, err);
            check(g3 != nullptr, "v2 disk open ok");
            if (g3) {
                double r3 = RecallOf(*g3, data, truth);
                check(std::fabs(r3 - r) < 1e-9, "disk-mode recall identical under min budget");
            }
        }
    }

    if (g_fail) {
        printf("M3+ PARALLEL SMOKE: FAIL\n");
        return 1;
    }
    printf("M3+ PARALLEL SMOKE: PASS\n");
    return 0;
}
