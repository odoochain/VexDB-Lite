// SQLite 标量距离函数（M1）。
//
// 计算核走 common/distance 的 SIMD dispatch（经 vex_distance_entry 隔离），
// 距离语义与 DuckDB 端 distance_functions.cpp 严格对齐（同输入同输出，
// 跨引擎对照见 test/m1_cross_engine_check.py）。
#include "vexdb_sqlite_internal.h"
#include "functions/distance_functions.h"
#include "functions/vector_codec.h"
#include "vex_distance_entry.h"

#include <cstdio>
#include <string>

namespace {

using vexdb_sqlite::GetDistanceFn;
using vexdb_sqlite::GetVector;
using vexdb_sqlite::VectorView;

// 距离函数公共骨架：NULL 透传、双参解析、维度校验，再调 compute。
// negate: IP 的"相似度"形态（inner_product = -negative_inner_product）。
template <VexMetric M, bool negate = false>
void DistanceFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL ||
        sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    VectorView a, b;
    std::string err;
    if (!GetVector(argv[0], a, err) || !GetVector(argv[1], b, err)) {
        sqlite3_result_error(ctx, err.c_str(), -1);
        return;
    }
    if (a.dim != b.dim) {
        err = "vector dimension mismatch: " + std::to_string(a.dim) + " vs " +
              std::to_string(b.dim);
        sqlite3_result_error(ctx, err.c_str(), -1);
        return;
    }
    static const auto fn = GetDistanceFn(M);
    float v = fn(a.data, b.data, static_cast<uint16_t>(a.dim));
    sqlite3_result_double(ctx, negate ? -static_cast<double>(v) : static_cast<double>(v));
}

// vexdb_f32(json_text | blob) -> blob：编码辅助。BLOB 输入校验后原样返回。
void F32Func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    VectorView v;
    std::string err;
    if (!GetVector(argv[0], v, err)) {
        sqlite3_result_error(ctx, err.c_str(), -1);
        return;
    }
    sqlite3_result_blob(ctx, v.data, static_cast<int>(v.dim * 4), SQLITE_TRANSIENT);
}

// vexdb_vector_to_json(blob | text) -> text：对照/调试输出。
void VectorToJsonFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }
    VectorView v;
    std::string err;
    if (!GetVector(argv[0], v, err)) {
        sqlite3_result_error(ctx, err.c_str(), -1);
        return;
    }
    std::string json = "[";
    char buf[32];
    for (size_t i = 0; i < v.dim; i++) {
        snprintf(buf, sizeof(buf), "%g", static_cast<double>(v.data[i]));
        if (i) json += ",";
        json += buf;
    }
    json += "]";
    sqlite3_result_text(ctx, json.c_str(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
}

}  // namespace

extern "C" int vexdb_sqlite_register_distance_functions(sqlite3 *db) {
    struct Entry {
        const char *name;
        int nargs;
        void (*fn)(sqlite3_context *, int, sqlite3_value **);
    };
    static const Entry kEntries[] = {
        {"vexdb_l2_distance", 2, DistanceFunc<VexMetric::L2>},
        {"vexdb_cosine_distance", 2, DistanceFunc<VexMetric::COSINE>},
        {"vexdb_inner_product", 2, DistanceFunc<VexMetric::INNER_PRODUCT, true>},
        {"vexdb_negative_inner_product", 2, DistanceFunc<VexMetric::INNER_PRODUCT>},
        {"vexdb_f32", 1, F32Func},
        {"vexdb_vector_to_json", 1, VectorToJsonFunc},
    };
    for (const auto &e : kEntries) {
        int rc = sqlite3_create_function(db, e.name, e.nargs,
                                         SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                                         e.fn, nullptr, nullptr);
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}
