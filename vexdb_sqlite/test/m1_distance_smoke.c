// M1 距离层冒烟测试（静态注册形态）。
//
// 验证：① 四个距离函数已知值正确（容差 1e-6 相对误差）；② BLOB 与 JSON 输入
// 同值同结果；③ vexdb_f32 round-trip；④ NULL 透传；⑤ 维度不匹配/非法输入报错。
// 与 DuckDB 端的跨引擎一致性对照另由 test/m1_cross_engine_check.sh 负责。
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
#include "vexdb_sqlite.h"

static int g_fail = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "M1 FAIL: %s\n", what);
        g_fail = 1;
    }
}

// 单值查询：返回 double 结果；is_null 输出是否 NULL；返回 rc。
static int query_double(sqlite3 *db, const char *sql, double *out, int *is_null) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *is_null = (sqlite3_column_type(st, 0) == SQLITE_NULL);
        *out = sqlite3_column_double(st, 0);
        rc = SQLITE_OK;
    }
    sqlite3_finalize(st);
    return rc;
}

static void expect_value(sqlite3 *db, const char *sql, double expected, const char *what) {
    double got = 0;
    int is_null = 0;
    int rc = query_double(db, sql, &got, &is_null);
    if (rc != SQLITE_OK || is_null) {
        fprintf(stderr, "M1 FAIL: %s (rc=%d null=%d): %s\n", what, rc, is_null,
                sqlite3_errmsg(db));
        g_fail = 1;
        return;
    }
    double tol = 1e-6 * (fabs(expected) > 1.0 ? fabs(expected) : 1.0);
    if (fabs(got - expected) > tol) {
        fprintf(stderr, "M1 FAIL: %s: got %.9f expected %.9f\n", what, got, expected);
        g_fail = 1;
    }
}

static void expect_error(sqlite3 *db, const char *sql, const char *what) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    sqlite3_finalize(st);
    check(rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE, what);
}

int main(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return 1;
    if (vexdb_sqlite_register(db) != SQLITE_OK) return 1;

    // ① 已知值。a=[1,2,3], b=[4,5,6]:
    //    L2 = sqrt(27) ；IP = 32 ；neg_IP = -32
    //    cos_sim = 32/(sqrt(14)*sqrt(77)) → cosine_distance = 1 - cos_sim
    expect_value(db, "SELECT vexdb_l2_distance('[1,2,3]','[4,5,6]')",
                 sqrt(27.0), "l2 known value");
    expect_value(db, "SELECT vexdb_inner_product('[1,2,3]','[4,5,6]')",
                 32.0, "ip known value");
    expect_value(db, "SELECT vexdb_negative_inner_product('[1,2,3]','[4,5,6]')",
                 -32.0, "neg_ip known value");
    expect_value(db, "SELECT vexdb_cosine_distance('[1,2,3]','[4,5,6]')",
                 1.0 - 32.0 / (sqrt(14.0) * sqrt(77.0)), "cosine known value");
    // 特例：勾股 3-4-5、正交向量 cos=1、自身 cos=0
    expect_value(db, "SELECT vexdb_l2_distance('[0,0]','[3,4]')", 5.0, "l2 3-4-5");
    expect_value(db, "SELECT vexdb_cosine_distance('[1,0]','[0,1]')", 1.0, "cosine orthogonal");
    expect_value(db, "SELECT vexdb_cosine_distance('[1,2,3]','[1,2,3]')", 0.0, "cosine self");

    // ② BLOB 与 JSON 同值同结果
    expect_value(db,
        "SELECT vexdb_l2_distance(vexdb_f32('[1,2,3]'), vexdb_f32('[4,5,6]'))",
        sqrt(27.0), "l2 via blob");
    expect_value(db,
        "SELECT vexdb_l2_distance(vexdb_f32('[1,2,3]'), '[4,5,6]')",
        sqrt(27.0), "l2 mixed blob/json");

    // ③ f32 round-trip
    {
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(db,
            "SELECT vexdb_vector_to_json(vexdb_f32('[1.5, -2.25, 0]'))", -1, &st, NULL);
        check(rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW, "roundtrip prepare/step");
        if (rc == SQLITE_OK) {
            const char *json = (const char *)sqlite3_column_text(st, 0);
            check(json && strcmp(json, "[1.5,-2.25,0]") == 0, "roundtrip json text");
        }
        sqlite3_finalize(st);
        // blob 长度 = dim*4
        double blen = 0; int is_null = 0;
        query_double(db, "SELECT length(vexdb_f32('[1,2,3]'))", &blen, &is_null);
        check(!is_null && blen == 12, "f32 blob length 12");
    }

    // ④ NULL 透传
    {
        double v = 0; int is_null = 0;
        int rc = query_double(db, "SELECT vexdb_l2_distance(NULL, '[1,2]')", &v, &is_null);
        check(rc == SQLITE_OK && is_null, "null passthrough");
    }

    // ⑤ 错误路径
    expect_error(db, "SELECT vexdb_l2_distance('[1,2]','[1,2,3]')", "dim mismatch errors");
    expect_error(db, "SELECT vexdb_l2_distance('not json','[1,2]')", "bad json errors");
    expect_error(db, "SELECT vexdb_l2_distance(x'0102','[1,2]')", "bad blob len errors");
    expect_error(db, "SELECT vexdb_l2_distance(42,'[1,2]')", "int input errors");

    sqlite3_close(db);
    if (g_fail) {
        printf("M1 DISTANCE SMOKE: FAIL\n");
        return 1;
    }
    printf("M1 DISTANCE SMOKE: PASS\n");
    return 0;
}
