// M0 静态注册形态冒烟测试。
//
// 这是移动端真实形态的最小验证：扩展静态链入宿主（含 sqlite3 amalgamation），
// 不依赖运行时 .load（iOS 系统 libsqlite3 禁扩展加载、WASM 不支持 .load）。
// 验三件事：① 标量函数注册链路；② 虚拟表模块注册链路（建表成功）；
// ③ SELECT 返回 0 行（M0 无数据）。
#include <stdio.h>
#include "sqlite3.h"
#include "vexdb_sqlite.h"

static int fail(const char *what, int rc) {
    fprintf(stderr, "M0 SMOKE FAIL: %s (rc=%d)\n", what, rc);
    return 1;
}

int main(void) {
    sqlite3 *db = NULL;
    int rc;

    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return fail("open", rc);

    // 静态注册：直接调 register（无运行时 .load）。
    rc = vexdb_sqlite_register(db);
    if (rc != SQLITE_OK) return fail("register", rc);

    // ① 标量函数
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT vexdb_version()", -1, &st, NULL);
    if (rc != SQLITE_OK) return fail("prepare version", rc);
    if (sqlite3_step(st) != SQLITE_ROW) return fail("version no row", 0);
    printf("vexdb_version = %s\n", sqlite3_column_text(st, 0));
    sqlite3_finalize(st);

    // ② 虚拟表模块：建表成功
    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(embedding FLOAT[4], metric=cosine)",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "create virtual table: %s\n", errmsg ? errmsg : "(null)");
        sqlite3_free(errmsg);
        return fail("create virtual table", rc);
    }

    // ③ SELECT 返回 0 行
    int rows = 0;
    rc = sqlite3_prepare_v2(db, "SELECT * FROM idx", -1, &st, NULL);
    if (rc != SQLITE_OK) return fail("prepare select", rc);
    while (sqlite3_step(st) == SQLITE_ROW) rows++;
    sqlite3_finalize(st);
    printf("SELECT * FROM idx -> %d rows (expect 0)\n", rows);
    if (rows != 0) return fail("expected 0 rows", rows);

    sqlite3_close(db);
    printf("M0 STATIC SMOKE: PASS\n");
    return 0;
}
