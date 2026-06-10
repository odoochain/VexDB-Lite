// GRAPH_INDEX 虚拟表模块 —— M2：shadow table 持久化 + xUpdate + 暴力 KNN。
//
// 用法：
//   CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(
//       embedding FLOAT[128], metric=cosine, m=16, ef_construction=200);
//   INSERT INTO idx(rowid, embedding) VALUES (1, :vec);   -- BLOB 或 JSON 文本
//   SELECT rowid, distance FROM idx WHERE embedding MATCH :q AND k = 10;
//
// Shadow tables（普通表，经宿主连接 SQL 读写 → 随宿主事务原子回滚，这是
// FTS5/RTREE 的标准范式；铁律：禁止任何旁路 mmap/裸文件写）：
//   <name>_config (key TEXT PRIMARY KEY, value TEXT)   维度/metric/m/efc/格式版本
//   <name>_vectors(rowid INTEGER PRIMARY KEY, vec BLOB) 向量数据
//
// M2 查询是暴力扫描（功能对标 sqlite-vec）；M3 把 xFilter 切到 GraphIndexCore
// 的 ANN 搜索，%_graph shadow 表落图结构。distance 列三 metric 统一
// lower=closer（L2=sqrt、cosine=1-sim、ip=负内积），ORDER BY distance ASC 即最近。
#include "vexdb_sqlite_internal.h"
#include "vtab/graph_index_vtab.h"
#include "functions/vector_codec.h"
#include "index/graph_bridge.h"
#include "vex_distance_entry.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

using vexdb_sqlite::GetDistanceFn;
using vexdb_sqlite::GetVector;
using vexdb_sqlite::GraphBridge;
using vexdb_sqlite::VectorView;

constexpr int kFormatVersion = 1;
constexpr int kDefaultM = 16;
constexpr int kDefaultEfConstruction = 200;
constexpr int kDefaultEfSearch = 40;
// 行数不超过此值时 KNN 直接暴力扫描（小表 ANN 无性能收益反损 recall，
// 语义对齐 duck 端 vexdb_brute_force_threshold 的精度优先取向）。
constexpr sqlite3_int64 kBruteForceThreshold = 64;

// declare_vtab 的列序（rowid 之外）。embedding=0, distance=1, k=2(HIDDEN),
// cmd=3(与表同名的 HIDDEN 列，fts5 风格 special insert：
// INSERT INTO t(t) VALUES('ef_search=N') 运行时改参)。
enum Col { COL_EMBEDDING = 0, COL_DISTANCE = 1, COL_K = 2, COL_CMD = 3 };

// xBestIndex/xFilter 间的计划标记。
enum Plan { PLAN_SCAN = 0, PLAN_KNN = 1 };

struct GraphIndexVtab {
    sqlite3_vtab base;
    sqlite3 *db = nullptr;
    std::string schema;   // attached database 名（main/temp/…）
    std::string name;     // 虚拟表名
    int dim = 0;
    VexMetric metric = VexMetric::L2;
    int m = kDefaultM;
    int ef_construction = kDefaultEfConstruction;
    int ef_search = kDefaultEfSearch;

    // M3：内存 HNSW 图（懒加载：首次 KNN 时从 %_graph blob 还原或 %_vectors 重建）。
    // 一致性协议：任何写路径先清 %_graph（宿主事务内）；xSync 把 dirty 内存图
    // 序列化写回——磁盘上存在 blob 即保证与 %_vectors 一致；崩溃中途=无 blob=重建。
    std::unique_ptr<GraphBridge> graph;
    bool graph_dirty = false;
    // 行数缓存：-1=未知（首查时 count 一次），之后随 INSERT/DELETE 增减维护。
    // 1M 行表上每次 KNN 都 count(*) 全表扫（~80ms）会淹没 HNSW 本身（μs 级）。
    sqlite3_int64 row_count = -1;

    std::string ShadowName(const char *suffix) const {
        char *q = sqlite3_mprintf("\"%w\".\"%w_%s\"", schema.c_str(), name.c_str(), suffix);
        std::string s = q ? q : "";
        sqlite3_free(q);
        return s;
    }
    int SetError(const char *msg) {
        sqlite3_free(base.zErrMsg);
        base.zErrMsg = sqlite3_mprintf("%s", msg);
        return SQLITE_ERROR;
    }
};

struct GraphIndexCursor {
    sqlite3_vtab_cursor base;
    int plan = PLAN_SCAN;
    // SCAN 模式：流式遍历 shadow 表
    sqlite3_stmt *scan_stmt = nullptr;
    int scan_eof = 1;
    // KNN 模式：物化 top-k 结果
    struct Hit { double dist; sqlite3_int64 rowid; };
    std::vector<Hit> hits;
    size_t pos = 0;
};

GraphIndexVtab *asVtab(sqlite3_vtab *v) { return reinterpret_cast<GraphIndexVtab *>(v); }
GraphIndexCursor *asCursor(sqlite3_vtab_cursor *c) { return reinterpret_cast<GraphIndexCursor *>(c); }

// ---------- 参数解析 ----------

void TrimSpaces(std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

bool ParseMetric(const std::string &v, VexMetric &out) {
    if (v == "l2") { out = VexMetric::L2; return true; }
    if (v == "cosine") { out = VexMetric::COSINE; return true; }
    if (v == "ip" || v == "inner_product") { out = VexMetric::INNER_PRODUCT; return true; }
    return false;
}

// 解析 CREATE VIRTUAL TABLE 的参数串（argv[3..]）：
//   恰好一个向量列声明 "<col> FLOAT[<dim>]"（大小写不敏感），
//   加可选 "metric=..." "m=..." "ef_construction=..."。
bool ParseCreateArgs(int argc, const char *const *argv, GraphIndexVtab &vt,
                     std::string &col_name, std::string &err) {
    bool have_col = false;
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        TrimSpaces(arg);
        if (arg.empty()) continue;
        size_t eq = arg.find('=');
        if (eq != std::string::npos && arg.find(' ') > eq) {
            std::string key = arg.substr(0, eq), val = arg.substr(eq + 1);
            TrimSpaces(key); TrimSpaces(val);
            for (auto &c : key) c = static_cast<char>(tolower(c));
            for (auto &c : val) c = static_cast<char>(tolower(c));
            // 容忍 'cosine' / "cosine" 引号
            if (val.size() >= 2 && (val.front() == '\'' || val.front() == '"'))
                val = val.substr(1, val.size() - 2);
            if (key == "metric") {
                if (!ParseMetric(val, vt.metric)) { err = "unknown metric: " + val; return false; }
            } else if (key == "m") {
                vt.m = atoi(val.c_str());
                if (vt.m < 2 || vt.m > 256) { err = "m out of range [2,256]"; return false; }
            } else if (key == "ef_construction") {
                vt.ef_construction = atoi(val.c_str());
                if (vt.ef_construction < 1) { err = "ef_construction must be positive"; return false; }
            } else if (key == "ef_search") {
                vt.ef_search = atoi(val.c_str());
                if (vt.ef_search < 1) { err = "ef_search must be positive"; return false; }
            } else {
                err = "unknown parameter: " + key;
                return false;
            }
            continue;
        }
        // 列声明 "<name> FLOAT[<dim>]"
        size_t sp = arg.find_first_of(" \t");
        if (sp == std::string::npos) { err = "bad column declaration: " + arg; return false; }
        std::string cname = arg.substr(0, sp), ctype = arg.substr(sp + 1);
        TrimSpaces(ctype);
        std::string upper = ctype;
        for (auto &c : upper) c = static_cast<char>(toupper(c));
        if (upper.compare(0, 6, "FLOAT[") != 0 || upper.back() != ']') {
            err = "column type must be FLOAT[N]: " + arg;
            return false;
        }
        if (have_col) { err = "exactly one vector column is supported in M2"; return false; }
        int d = atoi(ctype.substr(6).c_str());
        if (d <= 0 || static_cast<size_t>(d) > vexdb_sqlite::kMaxDim) {
            err = "dimension out of range [1,65535]";
            return false;
        }
        vt.dim = d;
        col_name = cname;
        have_col = true;
    }
    if (!have_col) { err = "missing vector column declaration, e.g. embedding FLOAT[128]"; return false; }
    return true;
}

// ---------- shadow table 辅助 ----------

int ExecFmt(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

int ConfigSet(GraphIndexVtab &vt, const char *key, const std::string &val) {
    char *sql = sqlite3_mprintf(
        "INSERT OR REPLACE INTO %s(key, value) VALUES (%Q, %Q)",
        vt.ShadowName("config").c_str(), key, val.c_str());
    int rc = ExecFmt(vt.db, sql);
    sqlite3_free(sql);
    return rc;
}

bool ConfigGet(GraphIndexVtab &vt, const char *key, std::string &out) {
    char *sql = sqlite3_mprintf("SELECT value FROM %s WHERE key = %Q",
                                vt.ShadowName("config").c_str(), key);
    sqlite3_stmt *st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        out = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        ok = true;
    }
    sqlite3_finalize(st);
    sqlite3_free(sql);
    return ok;
}

int DeclareSchema(sqlite3 *db, const std::string &col_name, const std::string &table_name) {
    // 第一列用用户声明的列名；distance 只在 KNN 计划下有值；k 是查询参数列；
    // 末列与表同名（fts5 风格 special insert 入口）。
    char *sql = sqlite3_mprintf(
        "CREATE TABLE x(\"%w\", distance REAL, k INTEGER HIDDEN, \"%w\" TEXT HIDDEN)",
        col_name.c_str(), table_name.c_str());
    int rc = sqlite3_declare_vtab(db, sql);
    sqlite3_free(sql);
    return rc;
}

// xCreate/xConnect 公共体。create=true 时建 shadow 表并写 config，
// false 时从 config 恢复参数。
int ConnectImpl(sqlite3 *db, int argc, const char *const *argv,
                sqlite3_vtab **ppVtab, char **pzErr, bool create) {
    auto *vt = new (std::nothrow) GraphIndexVtab();
    if (!vt) return SQLITE_NOMEM;
    vt->db = db;
    vt->schema = argv[1];
    vt->name = argv[2];

    std::string err, col_name;
    if (create) {
        if (!ParseCreateArgs(argc, argv, *vt, col_name, err)) {
            *pzErr = sqlite3_mprintf("GRAPH_INDEX: %s", err.c_str());
            delete vt;
            return SQLITE_ERROR;
        }
        char *sql = sqlite3_mprintf(
            "CREATE TABLE %s(key TEXT PRIMARY KEY, value TEXT);"
            "CREATE TABLE %s(rowid INTEGER PRIMARY KEY, vec BLOB NOT NULL);"
            "CREATE TABLE %s(blk INTEGER PRIMARY KEY, data BLOB NOT NULL);",
            vt->ShadowName("config").c_str(), vt->ShadowName("vectors").c_str(),
            vt->ShadowName("graph").c_str());
        int rc = ExecFmt(db, sql);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) {
            *pzErr = sqlite3_mprintf("GRAPH_INDEX: failed to create shadow tables: %s",
                                     sqlite3_errmsg(db));
            delete vt;
            return rc;
        }
        ConfigSet(*vt, "format_version", std::to_string(kFormatVersion));
        ConfigSet(*vt, "dim", std::to_string(vt->dim));
        ConfigSet(*vt, "metric", std::to_string(static_cast<uint32_t>(vt->metric)));
        ConfigSet(*vt, "m", std::to_string(vt->m));
        ConfigSet(*vt, "ef_construction", std::to_string(vt->ef_construction));
        ConfigSet(*vt, "ef_search", std::to_string(vt->ef_search));
        ConfigSet(*vt, "column", col_name);
    } else {
        std::string v;
        if (ConfigGet(*vt, "column", v)) {
            col_name = v;
        }
        if (!ConfigGet(*vt, "format_version", v)) {
            *pzErr = sqlite3_mprintf("GRAPH_INDEX: missing shadow config for %s", argv[2]);
            delete vt;
            return SQLITE_ERROR;
        }
        if (atoi(v.c_str()) > kFormatVersion) {
            *pzErr = sqlite3_mprintf(
                "GRAPH_INDEX: on-disk format v%s is newer than supported v%d", v.c_str(),
                kFormatVersion);
            delete vt;
            return SQLITE_ERROR;
        }
        ConfigGet(*vt, "dim", v);
        vt->dim = atoi(v.c_str());
        ConfigGet(*vt, "metric", v);
        vt->metric = static_cast<VexMetric>(atoi(v.c_str()));
        ConfigGet(*vt, "m", v);
        vt->m = atoi(v.c_str());
        ConfigGet(*vt, "ef_construction", v);
        vt->ef_construction = atoi(v.c_str());
        if (ConfigGet(*vt, "ef_search", v)) {
            vt->ef_search = atoi(v.c_str());
        }
    }

    if (col_name.empty()) col_name = "embedding";
    int rc = DeclareSchema(db, col_name, vt->name);
    if (rc != SQLITE_OK) {
        delete vt;
        return rc;
    }
    *ppVtab = &vt->base;
    return SQLITE_OK;
}

// ---------- module 回调 ----------

int vtabCreate(sqlite3 *db, void *, int argc, const char *const *argv,
               sqlite3_vtab **ppVtab, char **pzErr) {
    return ConnectImpl(db, argc, argv, ppVtab, pzErr, /*create=*/true);
}

int vtabConnect(sqlite3 *db, void *, int argc, const char *const *argv,
                sqlite3_vtab **ppVtab, char **pzErr) {
    return ConnectImpl(db, argc, argv, ppVtab, pzErr, /*create=*/false);
}

int vtabDisconnect(sqlite3_vtab *pVtab) {
    delete asVtab(pVtab);
    return SQLITE_OK;
}

int vtabDestroy(sqlite3_vtab *pVtab) {
    auto *vt = asVtab(pVtab);
    char *sql = sqlite3_mprintf(
        "DROP TABLE IF EXISTS %s; DROP TABLE IF EXISTS %s; DROP TABLE IF EXISTS %s;",
        vt->ShadowName("config").c_str(), vt->ShadowName("vectors").c_str(),
        vt->ShadowName("graph").c_str());
    int rc = ExecFmt(vt->db, sql);
    sqlite3_free(sql);
    delete vt;
    return rc;
}

// ---------- M3：内存图生命周期 ----------

sqlite3_int64 CountVectors(GraphIndexVtab &vt) {
    if (vt.row_count >= 0) return vt.row_count;
    char *sql = sqlite3_mprintf("SELECT count(*) FROM %s", vt.ShadowName("vectors").c_str());
    sqlite3_stmt *st = nullptr;
    sqlite3_int64 n = 0;
    if (sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_free(sql);
    vt.row_count = n;
    return n;
}

// 任何写路径调用：作废持久化图（宿主事务内，随事务回滚）。
int InvalidatePersistedGraph(GraphIndexVtab &vt) {
    char *sql = sqlite3_mprintf("DELETE FROM %s", vt.ShadowName("graph").c_str());
    int rc = ExecFmt(vt.db, sql);
    sqlite3_free(sql);
    return rc;
}

// 确保内存图就绪：优先 %_graph blob 还原；无/损坏/参数不符则从 %_vectors 重建。
// 重建标记 dirty（待下一次写事务的 xSync 落盘；查询路径绝不写库）。
int EnsureGraph(GraphIndexVtab &vt) {
    if (vt.graph) return SQLITE_OK;

    // 1) 尝试 load 持久化 blob
    {
        char *sql = sqlite3_mprintf("SELECT data FROM %s WHERE blk = 0",
                                    vt.ShadowName("graph").c_str());
        sqlite3_stmt *st = nullptr;
        int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
            const char *blob = static_cast<const char *>(sqlite3_column_blob(st, 0));
            size_t len = size_t(sqlite3_column_bytes(st, 0));
            std::string err;
            vt.graph = GraphBridge::LoadFromBlob(blob, len, uint16_t(vt.dim), vt.m,
                                                 vt.ef_construction, vt.metric, err);
            // load 失败不报错：fall through 重建（blob 视为陈旧）
        }
        sqlite3_finalize(st);
        if (vt.graph) {
            vt.graph_dirty = false;
            return SQLITE_OK;
        }
    }

    // 2) 从 %_vectors 全量重建。两段式：先串行 SQL 预读全部 (rowid, vec) 进
    //    内存（之后计算线程绝不触碰 sqlite3 句柄——线程合法性的边界），再
    //    BuildBulk 多线程建图（M3+ 并行；构建期独占，xFilter 同线程等待完成）。
    std::vector<float> vecs;
    std::vector<int64_t> rowids;
    {
        char *sql = sqlite3_mprintf("SELECT rowid, vec FROM %s ORDER BY rowid",
                                    vt.ShadowName("vectors").c_str());
        sqlite3_stmt *st = nullptr;
        int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (sqlite3_column_bytes(st, 1) != vt.dim * 4) continue;  // 防御脏数据
            const float *v = static_cast<const float *>(sqlite3_column_blob(st, 1));
            vecs.insert(vecs.end(), v, v + vt.dim);
            rowids.push_back(sqlite3_column_int64(st, 0));
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return rc;
    }
    auto bridge = std::make_unique<GraphBridge>(uint16_t(vt.dim), vt.m, vt.ef_construction,
                                                vt.metric);
    unsigned hw = std::thread::hardware_concurrency();
    int n_threads = int(hw > 1 ? (hw > 8 ? 8 : hw) : 1);  // 端侧保守上限 8
    try {
        bridge->BuildBulk(vecs.data(), rowids.data(), rowids.size(), n_threads);
    } catch (const std::exception &e) {
        return vt.SetError(e.what());
    }
    vt.graph = std::move(bridge);
    vt.graph_dirty = true;
    return SQLITE_OK;
}

int vtabBestIndex(sqlite3_vtab *, sqlite3_index_info *info) {
    int match_idx = -1, k_idx = -1;
    for (int i = 0; i < info->nConstraint; i++) {
        const auto &c = info->aConstraint[i];
        if (!c.usable) continue;
        if (c.iColumn == COL_EMBEDDING && c.op == SQLITE_INDEX_CONSTRAINT_MATCH)
            match_idx = i;
        if (c.iColumn == COL_K && c.op == SQLITE_INDEX_CONSTRAINT_EQ)
            k_idx = i;
    }
    if (match_idx >= 0 && k_idx >= 0) {
        info->aConstraintUsage[match_idx].argvIndex = 1;
        info->aConstraintUsage[match_idx].omit = 1;
        info->aConstraintUsage[k_idx].argvIndex = 2;
        info->aConstraintUsage[k_idx].omit = 1;
        info->idxNum = PLAN_KNN;
        info->estimatedCost = 100.0;
        info->estimatedRows = 10;
        // KNN 结果已按 distance 升序物化
        if (info->nOrderBy == 1 && info->aOrderBy[0].iColumn == COL_DISTANCE &&
            !info->aOrderBy[0].desc) {
            info->orderByConsumed = 1;
        }
        return SQLITE_OK;
    }
    if (match_idx >= 0) {
        // 有 MATCH 没 k：明确报错好过静默全扫
        return SQLITE_CONSTRAINT;
    }
    info->idxNum = PLAN_SCAN;
    info->estimatedCost = 1e6;
    return SQLITE_OK;
}

int vtabOpen(sqlite3_vtab *, sqlite3_vtab_cursor **ppCur) {
    auto *c = new (std::nothrow) GraphIndexCursor();
    if (!c) return SQLITE_NOMEM;
    *ppCur = &c->base;
    return SQLITE_OK;
}

int vtabClose(sqlite3_vtab_cursor *cur) {
    auto *c = asCursor(cur);
    sqlite3_finalize(c->scan_stmt);
    delete c;
    return SQLITE_OK;
}

int FilterKnn(GraphIndexVtab &vt, GraphIndexCursor &cur, sqlite3_value **argv) {
    VectorView q;
    std::string err;
    if (!GetVector(argv[0], q, err)) return vt.SetError(err.c_str());
    if (static_cast<int>(q.dim) != vt.dim) {
        err = "query dimension " + std::to_string(q.dim) + " != index dimension " +
              std::to_string(vt.dim);
        return vt.SetError(err.c_str());
    }
    sqlite3_int64 k = sqlite3_value_int64(argv[1]);
    if (k <= 0) return vt.SetError("k must be positive");

    // M3：行数超过阈值走 HNSW 图（懒加载）；小表保持暴力（精度优先）。
    if (CountVectors(vt) > kBruteForceThreshold) {
        int rc = EnsureGraph(vt);
        if (rc != SQLITE_OK) return rc;
        std::vector<std::pair<double, int64_t>> hits;
        vt.graph->Search(q.data, size_t(k), uint32_t(vt.ef_search), hits);
        // distance 列重算为用户语义（与暴力路径/标量函数严格一致）。
        // 算法返回的 dist 是内核原始值，且 common 的 Distancer 主模板与各
        // Arch 特化对 COSINE 语义不一致（主模板=1-cos，特化=-cos；ODR 隐患，
        // 不同 TU/构建可能拿到不同实现）——排序单调同向不受影响，但输出值
        // 不可信。k 次点查+重算成本可忽略，换取对内核语义漂移彻底免疫。
        const auto dist_fn = GetDistanceFn(vt.metric);
        char *sql = sqlite3_mprintf("SELECT vec FROM %s WHERE rowid = ?1",
                                    vt.ShadowName("vectors").c_str());
        sqlite3_stmt *st = nullptr;
        rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        cur.hits.clear();
        cur.hits.reserve(hits.size());
        for (const auto &h : hits) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, h.second);
            double d = h.first;
            if (sqlite3_step(st) == SQLITE_ROW &&
                sqlite3_column_bytes(st, 0) == vt.dim * 4) {
                d = dist_fn(q.data, static_cast<const float *>(sqlite3_column_blob(st, 0)),
                            static_cast<uint16_t>(vt.dim));
            }
            cur.hits.push_back({d, h.second});
        }
        sqlite3_finalize(st);
        std::sort(cur.hits.begin(), cur.hits.end(),
                  [](const GraphIndexCursor::Hit &a, const GraphIndexCursor::Hit &b) {
                      return a.dist < b.dist;
                  });
        cur.pos = 0;
        return SQLITE_OK;
    }

    const auto dist_fn = GetDistanceFn(vt.metric);
    char *sql = sqlite3_mprintf("SELECT rowid, vec FROM %s", vt.ShadowName("vectors").c_str());
    sqlite3_stmt *st = nullptr;
    int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;

    // 暴力扫描 + 大小 k 的最大堆（堆顶=当前第 k 近，更近则替换）。
    auto cmp = [](const GraphIndexCursor::Hit &a, const GraphIndexCursor::Hit &b) {
        return a.dist < b.dist;
    };
    auto &heap = cur.hits;
    heap.clear();
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(st, 1);
        if (len != vt.dim * 4) continue;  // 防御：维度不符的脏数据跳过
        const float *vec = static_cast<const float *>(sqlite3_column_blob(st, 1));
        double d = dist_fn(q.data, vec, static_cast<uint16_t>(vt.dim));
        if (heap.size() < static_cast<size_t>(k)) {
            heap.push_back({d, sqlite3_column_int64(st, 0)});
            std::push_heap(heap.begin(), heap.end(), cmp);
        } else if (d < heap.front().dist) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.back() = {d, sqlite3_column_int64(st, 0)};
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return rc;
    std::sort_heap(heap.begin(), heap.end(), cmp);  // 升序：最近在前
    cur.pos = 0;
    return SQLITE_OK;
}

int vtabFilter(sqlite3_vtab_cursor *pCur, int idxNum, const char *, int argc,
               sqlite3_value **argv) {
    auto *cur = asCursor(pCur);
    auto *vt = asVtab(pCur->pVtab);
    cur->plan = idxNum;
    sqlite3_finalize(cur->scan_stmt);
    cur->scan_stmt = nullptr;
    cur->hits.clear();
    cur->pos = 0;

    if (idxNum == PLAN_KNN) {
        if (argc < 2) return vt->SetError("internal: KNN plan expects 2 args");
        return FilterKnn(*vt, *cur, argv);
    }
    char *sql = sqlite3_mprintf("SELECT rowid, vec FROM %s ORDER BY rowid",
                                vt->ShadowName("vectors").c_str());
    int rc = sqlite3_prepare_v2(vt->db, sql, -1, &cur->scan_stmt, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
    cur->scan_eof = (sqlite3_step(cur->scan_stmt) != SQLITE_ROW);
    return SQLITE_OK;
}

int vtabNext(sqlite3_vtab_cursor *pCur) {
    auto *cur = asCursor(pCur);
    if (cur->plan == PLAN_KNN) {
        cur->pos++;
    } else {
        cur->scan_eof = (sqlite3_step(cur->scan_stmt) != SQLITE_ROW);
    }
    return SQLITE_OK;
}

int vtabEof(sqlite3_vtab_cursor *pCur) {
    auto *cur = asCursor(pCur);
    return cur->plan == PLAN_KNN ? cur->pos >= cur->hits.size() : cur->scan_eof;
}

int vtabColumn(sqlite3_vtab_cursor *pCur, sqlite3_context *ctx, int col) {
    auto *cur = asCursor(pCur);
    if (cur->plan == PLAN_KNN) {
        if (col == COL_DISTANCE) {
            sqlite3_result_double(ctx, cur->hits[cur->pos].dist);
        } else {
            sqlite3_result_null(ctx);  // KNN 模式不回吐向量本体（用 rowid join 原表）
        }
        return SQLITE_OK;
    }
    if (col == COL_EMBEDDING) {
        sqlite3_result_value(ctx, sqlite3_column_value(cur->scan_stmt, 1));
    } else {
        sqlite3_result_null(ctx);
    }
    return SQLITE_OK;
}

int vtabRowid(sqlite3_vtab_cursor *pCur, sqlite3_int64 *pRowid) {
    auto *cur = asCursor(pCur);
    *pRowid = cur->plan == PLAN_KNN ? cur->hits[cur->pos].rowid
                                    : sqlite3_column_int64(cur->scan_stmt, 0);
    return SQLITE_OK;
}

// INSERT/DELETE/UPDATE。argv 布局（SQLite vtab 约定）：
//   DELETE: argc=1, argv[0]=旧 rowid
//   INSERT: argc=N+2, argv[0]=NULL, argv[1]=新 rowid（或 NULL 自动分配）, argv[2..]=列值
//   UPDATE: argc=N+2, argv[0]=旧 rowid, argv[1]=新 rowid, argv[2..]=列值
int vtabUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv,
               sqlite3_int64 *pRowid) {
    auto *vt = asVtab(pVtab);
    // DELETE：HNSW 不做物理删点，M3 策略=作废内存图与持久化 blob，下次查询重建
    //（重建只读 %_vectors，已删行天然不在）。
    if (argc == 1) {
        char *sql = sqlite3_mprintf("DELETE FROM %s WHERE rowid = %lld",
                                    vt->ShadowName("vectors").c_str(),
                                    sqlite3_value_int64(argv[0]));
        int rc = ExecFmt(vt->db, sql);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        if (vt->row_count > 0 && sqlite3_changes(vt->db) > 0) vt->row_count--;
        vt->graph.reset();
        vt->graph_dirty = false;
        return InvalidatePersistedGraph(*vt);
    }
    // special insert（fts5 风格）：INSERT INTO t(t) VALUES('ef_search=N')
    // 运行时改参，同连接即时生效并持久化进 config，不触碰数据与内存图。
    if (argc > 2 + COL_CMD && sqlite3_value_type(argv[2 + COL_CMD]) == SQLITE_TEXT) {
        std::string cmd = reinterpret_cast<const char *>(sqlite3_value_text(argv[2 + COL_CMD]));
        size_t eq = cmd.find('=');
        std::string key = cmd.substr(0, eq == std::string::npos ? cmd.size() : eq);
        TrimSpaces(key);
        if (key == "ef_search" && eq != std::string::npos) {
            int v = atoi(cmd.c_str() + eq + 1);
            if (v < 1) return vt->SetError("ef_search must be positive");
            vt->ef_search = v;
            ConfigSet(*vt, "ef_search", std::to_string(v));
            return SQLITE_OK;
        }
        return vt->SetError(("unknown command: " + cmd).c_str());
    }

    // INSERT / UPDATE 的向量值
    sqlite3_value *vec_val = argv[2 + COL_EMBEDDING];
    if (sqlite3_value_type(vec_val) == SQLITE_NULL)
        return vt->SetError("embedding must not be NULL");
    VectorView v;
    std::string err;
    if (!GetVector(vec_val, v, err)) return vt->SetError(err.c_str());
    if (static_cast<int>(v.dim) != vt->dim) {
        err = "vector dimension " + std::to_string(v.dim) + " != index dimension " +
              std::to_string(vt->dim);
        return vt->SetError(err.c_str());
    }
    // distance/k 列不可写
    if (sqlite3_value_type(argv[2 + COL_DISTANCE]) != SQLITE_NULL ||
        sqlite3_value_type(argv[2 + COL_K]) != SQLITE_NULL)
        return vt->SetError("distance/k columns are read-only");

    bool is_insert = sqlite3_value_type(argv[0]) == SQLITE_NULL;
    sqlite3_int64 old_rowid = is_insert ? 0 : sqlite3_value_int64(argv[0]);
    bool has_new_rowid = sqlite3_value_type(argv[1]) != SQLITE_NULL;
    sqlite3_int64 new_rowid = has_new_rowid ? sqlite3_value_int64(argv[1]) : 0;

    sqlite3_stmt *st = nullptr;
    int rc;
    if (is_insert) {
        char *sql = sqlite3_mprintf(
            has_new_rowid ? "INSERT INTO %s(rowid, vec) VALUES (?1, ?2)"
                          : "INSERT INTO %s(vec) VALUES (?2)",
            vt->ShadowName("vectors").c_str());
        rc = sqlite3_prepare_v2(vt->db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        if (has_new_rowid) sqlite3_bind_int64(st, 1, new_rowid);
    } else {
        // UPDATE（含 rowid 变更）
        char *sql = sqlite3_mprintf(
            "UPDATE %s SET rowid = ?1, vec = ?2 WHERE rowid = ?3",
            vt->ShadowName("vectors").c_str());
        rc = sqlite3_prepare_v2(vt->db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        sqlite3_bind_int64(st, 1, has_new_rowid ? new_rowid : old_rowid);
        sqlite3_bind_int64(st, 3, old_rowid);
    }
    sqlite3_bind_blob(st, 2, v.data, static_cast<int>(v.dim * 4), SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) return SQLITE_CONSTRAINT;  // 重复 rowid 等
        return vt->SetError(sqlite3_errmsg(vt->db));
    }
    sqlite3_int64 final_rowid =
        is_insert ? (has_new_rowid ? new_rowid : sqlite3_last_insert_rowid(vt->db))
                  : (has_new_rowid ? new_rowid : old_rowid);
    if (is_insert) *pRowid = final_rowid;

    // M3 图维护：持久化 blob 一律作废（xSync 重写）；内存图 INSERT 真增量，
    // UPDATE 改了已有向量 → 作废重建。
    rc = InvalidatePersistedGraph(*vt);
    if (rc != SQLITE_OK) return rc;
    if (is_insert) {
        if (vt->row_count >= 0) vt->row_count++;
        if (vt->graph) {
            vt->graph->Insert(v.data, final_rowid);
            vt->graph_dirty = true;
        }
    } else {
        vt->graph.reset();
        vt->graph_dirty = false;
    }
    return SQLITE_OK;
}

// ---------- 事务钩子（M3） ----------
// 写入本身全经 shadow 表 SQL（随宿主事务原子回滚）；钩子只管内存图的一致性：
//   xSync    : 把 dirty 内存图序列化写回 %_graph（可失败的工作必须放这里）
//   xRollback: 事务回滚 → 内存图可能含已回滚的插入，作废
//   xCommit  : 清理无事可做（blob 已在 xSync 落盘）

int vtabBegin(sqlite3_vtab *) { return SQLITE_OK; }

int vtabSync(sqlite3_vtab *pVtab) {
    auto *vt = asVtab(pVtab);
    if (!vt->graph || !vt->graph_dirty) return SQLITE_OK;
    std::vector<char> blob;
    vt->graph->SerializeToBlob(blob);
    char *sql = sqlite3_mprintf("INSERT OR REPLACE INTO %s(blk, data) VALUES (0, ?1)",
                                vt->ShadowName("graph").c_str());
    sqlite3_stmt *st = nullptr;
    int rc = sqlite3_prepare_v2(vt->db, sql, -1, &st, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_blob64(st, 1, blob.data(), blob.size(), SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return vt->SetError(sqlite3_errmsg(vt->db));
    vt->graph_dirty = false;
    return SQLITE_OK;
}

int vtabCommit(sqlite3_vtab *) { return SQLITE_OK; }

int vtabRollback(sqlite3_vtab *pVtab) {
    auto *vt = asVtab(pVtab);
    vt->graph.reset();
    vt->graph_dirty = false;
    return SQLITE_OK;
}

int vtabShadowName(const char *name) {
    return strcmp(name, "config") == 0 || strcmp(name, "vectors") == 0 ||
           strcmp(name, "graph") == 0;  // graph 留给 M3
}

}  // namespace

// iVersion=3 启用 xShadowName（DEFENSIVE 模式下保护 shadow 表不被普通 SQL 误写）。
// M3 填 xBegin/xSync/xCommit/xRollback（内存图的事务钩子；M2 全部写入直接经
// shadow 表 SQL，事务性由宿主天然保证，无需钩子）。
const sqlite3_module vexdb_graph_index_module = {
    /* iVersion      */ 3,
    /* xCreate       */ vtabCreate,
    /* xConnect      */ vtabConnect,
    /* xBestIndex    */ vtabBestIndex,
    /* xDisconnect   */ vtabDisconnect,
    /* xDestroy      */ vtabDestroy,
    /* xOpen         */ vtabOpen,
    /* xClose        */ vtabClose,
    /* xFilter       */ vtabFilter,
    /* xNext         */ vtabNext,
    /* xEof          */ vtabEof,
    /* xColumn       */ vtabColumn,
    /* xRowid        */ vtabRowid,
    /* xUpdate       */ vtabUpdate,
    /* xBegin        */ vtabBegin,
    /* xSync         */ vtabSync,
    /* xCommit       */ vtabCommit,
    /* xRollback     */ vtabRollback,
    /* xFindFunction */ nullptr,
    /* xRename       */ nullptr,
    /* xSavepoint    */ nullptr,
    /* xRelease      */ nullptr,
    /* xRollbackTo   */ nullptr,
    /* xShadowName   */ vtabShadowName,
};
