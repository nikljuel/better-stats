/* Host test: session derivation from explorer-3-like snapshots. */
#include "../src/tracker.h"
#include "../src/stats_db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXP_DB "/tmp/bs_test_explorer.db"
#define ST_DB "/tmp/bs_test_stats.db"

static void ex(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n%s\n", err ? err : "?", sql);
        exit(1);
    }
}

static sqlite3 *make_explorer(void)
{
    unlink(EXP_DB);
    sqlite3 *db;
    assert(sqlite3_open(EXP_DB, &db) == SQLITE_OK);
    ex(db, "CREATE TABLE books_impl (id INTEGER PRIMARY KEY, title TEXT, author TEXT);"
           "CREATE TABLE files (book_id INTEGER, storageid INTEGER, fast_hash BLOB);"
           "CREATE TABLE books_settings (bookid INTEGER, profileid INTEGER,"
           " position TEXT, position_ts INTEGER, cpage INTEGER, npage INTEGER,"
           " opentime INTEGER, completed INTEGER);");
    ex(db, "INSERT INTO books_impl VALUES (7,'Testbuch','Autorin');"
           "INSERT INTO files VALUES (7,1,x'aabb');"
           "INSERT INTO books_settings VALUES (7,1,'p',1000,10,300,1000,0);");
    return db;
}

static void set_state(sqlite3 *db, long open, long pos, int cpage)
{
    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE books_settings SET opentime=%ld, position_ts=%ld, cpage=%d"
             " WHERE bookid=7", open, pos, cpage);
    ex(db, sql);
}

static long q1(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    long v = -999;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW)
        v = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

int main(void)
{
    sqlite3 *exp = make_explorer();
    unlink(ST_DB);

    tracker t;
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);

    /* read_state returns the book incl. metadata + cover hash */
    pb_state s;
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(s.bookid == 7 && s.opentime == 1000 && s.cpage == 10);
    assert(strcmp(s.title, "Testbuch") == 0);
    assert(strcmp(s.cover, "1aabb") == 0);

    /* New session observed: active = pos-open (0 here), pages_start = cpage */
    assert(tracker_observe(&t, &s) == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 0);
    assert(q1(t.stats, "SELECT pages_start FROM sessions") == 10);

    /* Page turn after 60s: active += 60 */
    set_state(exp, 1000, 1060, 12);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 60);
    assert(q1(t.stats, "SELECT pages_end FROM sessions") == 12);

    /* 1h standby, then a page turn: the gap is capped at IDLE_CAP */
    set_state(exp, 1000, 1060 + 3600, 13);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    tracker_observe(&t, &s);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 60 + IDLE_CAP_SECONDS);
    assert(q1(t.stats, "SELECT end_time FROM sessions") == 4660);

    /* No new position_ts -> nothing happens */
    assert(tracker_observe(&t, &s) == 0);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 660);

    /* New open = new session; pages_start = pages_end of the old one */
    set_state(exp, 9000, 9005, 14);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s) == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 2);
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=9000") == 13);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=9000") == 5);

    /* Recovery: session created without a daemon, backfilled + dedupe */
    tracker_close(&t);
    set_state(exp, 20000, 20000 + 1200, 40);
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
    assert(tracker_recover(&t) >= 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 3);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=20000") == 1200);
    assert(q1(t.stats, "SELECT recovered FROM sessions WHERE start_time=20000") == 1);
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=20000") == 14);
    tracker_recover(&t); /* idempotent */
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 3);

    /* Recovery cap against huge spans */
    set_state(exp, 50000, 50000 + 10 * 3600, 60);
    tracker_recover(&t);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=50000") ==
           RECOVERED_CAP_SECONDS);

    /* stats_overall computes without crashing and plausibly */
    overall_stats o;
    ex(t.stats, "UPDATE books SET completed=1"); /* for the finished counter */
    assert(stats_overall(t.stats, &o) == 0);
    assert(o.books_total == 1 && o.books_finished == 1);
    assert(o.total_hours > 0);

    int secs[32];
    struct tm *tm; time_t now = time(NULL); tm = localtime(&now);
    stats_month(t.stats, tm->tm_year + 1900, tm->tm_mon + 1, secs);

    tracker_close(&t);
    printf("all tracker tests ok\n");
    return 0;
}
