// SIFT1M QPS/RSS-vs-graph_memory_limit sweep.
//
// Loads SIFT1M once, then rebuilds the GRAPH_INDEX under a sweep of
// vexdb_graph_memory_limit budgets (ASCENDING so resident RSS grows monotonically
// and each measurement is clean of a prior larger build's malloc retention).
// Per budget reports: build time, resident RSS, vexdb_graph_mirror_used(), QPS,
// recall@10/@100. Recall should stay ~constant; RSS should fall with the budget
// (the mirror is a redundant copy of the buffer-manager tier); QPS degrades
// gracefully as more vectors are served from vector_alloc_ (per-buffer mutex).
#include "duckdb.hpp"
#include "core_functions_extension.hpp"
#include "vexdb_lite_extension.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <mach/mach.h>

using namespace duckdb;

namespace {

struct Timer {
    std::chrono::high_resolution_clock::time_point t;
    void Start() { t = std::chrono::high_resolution_clock::now(); }
    double Ms() const {
        return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
    }
};

// Current resident set size (bytes) of this process, via mach. Unlike getrusage
// ru_maxrss (peak), this is the live footprint, so ascending-budget measurements
// each reflect that build's resident memory.
static size_t ResidentBytes() {
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return static_cast<size_t>(info.resident_size);
}

static unique_ptr<QueryResult> ExecOrThrow(Connection &con, const string &sql) {
    auto result = con.Query(sql);
    if (!result) {
        throw std::runtime_error("query returned null result: " + sql);
    }
    if (result->HasError()) {
        result->ThrowError("query failed: " + sql + "\n");
    }
    return result;
}

static std::vector<float> LoadFloatBin(const string &path, idx_t dim) {
    std::ifstream fs(path, std::ios::binary | std::ios::ate);
    if (!fs) {
        throw std::runtime_error("cannot open: " + path);
    }
    auto size = static_cast<size_t>(fs.tellg());
    fs.seekg(0);
    if (size % (static_cast<size_t>(dim) * sizeof(float)) != 0) {
        throw std::runtime_error("unexpected float bin size: " + path);
    }
    std::vector<float> data(size / sizeof(float));
    fs.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

static std::vector<int32_t> LoadIntBin(const string &path, idx_t cols) {
    std::ifstream fs(path, std::ios::binary | std::ios::ate);
    if (!fs) {
        throw std::runtime_error("cannot open: " + path);
    }
    auto size = static_cast<size_t>(fs.tellg());
    fs.seekg(0);
    if (size % (static_cast<size_t>(cols) * sizeof(int32_t)) != 0) {
        throw std::runtime_error("unexpected int bin size: " + path);
    }
    std::vector<int32_t> data(size / sizeof(int32_t));
    fs.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

static Value MakeArrayValue(const float *vec, idx_t dim) {
    std::vector<Value> values;
    values.reserve(dim);
    for (idx_t i = 0; i < dim; i++) {
        values.emplace_back(Value::FLOAT(vec[i]));
    }
    return Value::ARRAY(LogicalType::FLOAT, std::move(values));
}

static string MakeQueryArrayLiteral(const float *vec, idx_t dim) {
    std::ostringstream oss;
    oss << '[' << std::fixed << std::setprecision(6);
    for (idx_t i = 0; i < dim; i++) {
        if (i) {
            oss << ", ";
        }
        oss << vec[i];
    }
    oss << "]::FLOAT[" << dim << ']';
    return oss.str();
}

static std::vector<int32_t> ReadTopIds(Connection &con, const string &sql, idx_t k) {
    auto result = ExecOrThrow(con, sql);
    auto &mat = result->Cast<MaterializedQueryResult>();
    std::vector<int32_t> ids;
    ids.reserve(k);
    for (idx_t row = 0; row < mat.RowCount() && row < k; row++) {
        ids.push_back(IntegerValue::Get(mat.GetValue(0, row).DefaultCastAs(LogicalType::INTEGER)));
    }
    return ids;
}

static uint64_t ReadU64(Connection &con, const string &sql) {
    auto result = ExecOrThrow(con, sql);
    auto &mat = result->Cast<MaterializedQueryResult>();
    if (mat.RowCount() == 0) {
        return 0;
    }
    return mat.GetValue(0, 0).GetValue<uint64_t>();
}

static double RecallAtK(const std::vector<int32_t> &ids, const int32_t *gt, idx_t k) {
    idx_t hits = 0;
    for (idx_t i = 0; i < ids.size() && i < k; i++) {
        for (idx_t j = 0; j < k; j++) {
            if (ids[i] == gt[j]) {
                hits++;
                break;
            }
        }
    }
    return static_cast<double>(hits) / static_cast<double>(k);
}

static double MB(size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

} // namespace

int main(int argc, char **argv) {
    static constexpr idx_t D = 128;
    static constexpr idx_t GT_K = 100;

    string data_dir = "vexdb_duckdb/test/benchmark/data";
    if (argc > 1) {
        data_dir = argv[1];
    }

    const string train_file = data_dir + "/sift_train_1m.fbin";
    const string query_file = data_dir + "/sift_query_200.fbin";
    const string gt_file = data_dir + "/sift_gt_1m_200q.ibin";

    auto train = LoadFloatBin(train_file, D);
    auto queries = LoadFloatBin(query_file, D);
    auto gt = LoadIntBin(gt_file, GT_K);
    const idx_t n = train.size() / D;
    const idx_t nq = queries.size() / D;
    std::cout << "loaded train n=" << n << " queries nq=" << nq << std::endl;

    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", true);
    DuckDB db(nullptr, &config);
    db.LoadStaticExtension<CoreFunctionsExtension>();
    db.LoadStaticExtension<VexdbLiteExtension>();
    Connection con(db);
    ExecOrThrow(con, "SET vexdb_ef_search=40");
    ExecOrThrow(con, "SET vexdb_brute_force_threshold=64");
    ExecOrThrow(con, "CREATE TABLE sift (id INTEGER, vec FLOAT[128])");

    Timer timer;
    timer.Start();
    {
        Appender app(con, "sift");
        for (idx_t i = 0; i < n; i++) {
            app.BeginRow();
            app.Append(static_cast<int32_t>(i));
            app.Append(MakeArrayValue(train.data() + i * D, D));
            app.EndRow();
            if ((i + 1) % 100000 == 0 || i + 1 == n) {
                std::cout << "loaded rows: " << (i + 1) << "/" << n << std::endl;
            }
        }
        app.Close();
    }
    std::cout << "load_ms=" << timer.Ms() << std::endl;
    // Drop the 512MB host-side copy: it's in the table now; only `queries` is reused.
    { std::vector<float>().swap(train); }

    const size_t rss_after_load = ResidentBytes();
    std::cout << "rss_after_load_MB=" << std::fixed << std::setprecision(1) << MB(rss_after_load) << std::endl;
    const size_t full_mirror_bytes = static_cast<size_t>(n) * D * sizeof(float); // 512MB for 1M

    // Single-budget mode (argv[3] = budget in MB, "0" = unlimited): one fresh build so
    // resident RSS is clean of cross-iteration malloc retention. Else sweep all budgets
    // in one process (QPS/recall/mirror_used are clean; the rss column is only indicative).
    std::vector<size_t> budgets;
    if (argc > 2) {
        const size_t mb = static_cast<size_t>(std::stoull(argv[2]));
        budgets = {mb * 1024 * 1024};
    } else {
        budgets = {
            16ull * 1024 * 1024,  // 16 MB
            64ull * 1024 * 1024,  // 64 MB
            128ull * 1024 * 1024, // 128 MB
            256ull * 1024 * 1024, // 256 MB
            full_mirror_bytes,    // ~512 MB (fits all)
            0,                    // unlimited (full mirror, untracked)
        };
    }

    std::cout << "\n";
    std::cout << "budget_MB  mirror_used_MB  rss_MB  rss_delta_MB  build_s  qps  recall@10  recall@100\n";
    std::cout << "-------------------------------------------------------------------------------------\n";

    for (size_t budget : budgets) {
        ExecOrThrow(con, "SET vexdb_graph_memory_limit=" + std::to_string(budget));
        timer.Start();
        ExecOrThrow(con, "CREATE INDEX idx_sift ON sift USING GRAPH_INDEX (vec) WITH (metric='l2')");
        const double build_s = timer.Ms() / 1000.0;

        const uint64_t mirror_used = ReadU64(con, "SELECT vexdb_graph_mirror_used()");
        const size_t rss = ResidentBytes();

        double recall10 = 0.0, recall100 = 0.0;
        timer.Start();
        for (idx_t q = 0; q < nq; q++) {
            const string qv = MakeQueryArrayLiteral(queries.data() + q * D, D);
            const string sql = "SELECT id FROM sift ORDER BY l2_distance(vec, " + qv + ") LIMIT 100";
            auto ids = ReadTopIds(con, sql, 100);
            recall10 += RecallAtK(ids, gt.data() + q * GT_K, 10);
            recall100 += RecallAtK(ids, gt.data() + q * GT_K, 100);
        }
        const double qps = static_cast<double>(nq) / (timer.Ms() / 1000.0);

        std::cout << std::fixed << std::setprecision(1)
                  << std::setw(8) << (budget == 0 ? MB(full_mirror_bytes) : MB(budget)) << (budget == 0 ? "*" : " ")
                  << std::setw(15) << MB(mirror_used)
                  << std::setw(8) << MB(rss)
                  << std::setw(13) << MB(rss - rss_after_load)
                  << std::setw(8) << std::setprecision(1) << build_s
                  << std::setw(8) << std::setprecision(0) << qps
                  << std::setw(10) << std::setprecision(4) << (recall10 / nq)
                  << std::setw(11) << (recall100 / nq) << "\n";
        std::cout.flush();

        ExecOrThrow(con, "DROP INDEX idx_sift");
    }
    std::cout << "(* = unlimited budget=0, full mirror; mirror_used reported 0 because unlimited indexes are untracked)\n";
    return 0;
}
