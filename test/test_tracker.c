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
    /* Fixed zone: the day-boundary logic is local-time based. */
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

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
    /* Recovered = estimate: no pages_start, so it cannot skew pages/minute */
    assert(q1(t.stats, "SELECT pages_start IS NULL FROM sessions WHERE start_time=20000") == 1);
    tracker_recover(&t); /* idempotent */
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 3);

    /* Recovery cap against huge spans */
    set_state(exp, 50000, 50000 + 10 * 3600, 60);
    tracker_recover(&t);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=50000") ==
           RECOVERED_CAP_SECONDS);

    /* Rows written by older versions get retrofitted on open */
    ex(t.stats, "UPDATE sessions SET active_seconds=6*3600, pages_start=5"
                " WHERE start_time=50000");
    tracker_close(&t);
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=50000") ==
           RECOVERED_CAP_SECONDS);
    assert(q1(t.stats, "SELECT pages_start IS NULL FROM sessions WHERE start_time=50000") == 1);

    /* Session across local midnight is split so both days get their time */
    set_state(exp, 82500, 82500, 20); /* 1970-01-01 23:55 CET */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s) == 1);
    set_state(exp, 82500, 82900, 22); /* 1970-01-02 00:01:40 CET */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=82500") == 300);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=82800") == 100);
    assert(q1(t.stats, "SELECT date(end_time,'unixepoch','localtime')='1970-01-01'"
                       " FROM sessions WHERE start_time=82500") == 1);
    assert(q1(t.stats, "SELECT date(end_time,'unixepoch','localtime')='1970-01-02'"
                       " FROM sessions WHERE start_time=82800") == 1);
    /* the new row continues the page count instead of restarting it */
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=82800") == 22);

    /* stats_overall computes without crashing and plausibly */
    overall_stats o;
    ex(t.stats, "UPDATE books SET completed=1"); /* for the finished counter */
    assert(stats_overall(t.stats, &o) == 0);
    assert(o.books_total == 1 && o.books_finished == 1);
    assert(o.total_hours > 0);

    /* Streak: anchored at today, one day of gap ends it, < 60 s/day never counts */
    ex(t.stats, "DELETE FROM sessions");
    ex(t.stats, "INSERT INTO sessions (book_id,start_time,end_time,active_seconds)"
                " VALUES (7, strftime('%s','now','-2 days'),"
                "            strftime('%s','now','-2 days'), 120)");
    assert(stats_overall(t.stats, &o) == 0);
    assert(o.streak_days == 0);
    ex(t.stats, "DELETE FROM sessions");
    ex(t.stats, "INSERT INTO sessions (book_id,start_time,end_time,active_seconds)"
                " VALUES (7, strftime('%s','now'), strftime('%s','now'), 120),"
                "        (7, strftime('%s','now','-1 days'),"
                "            strftime('%s','now','-1 days'), 120),"
                "        (7, strftime('%s','now','-2 days'),"
                "            strftime('%s','now','-2 days'), 30)");
    assert(stats_overall(t.stats, &o) == 0);
    assert(o.streak_days == 2);

    tracker_close(&t);
    printf("all tracker tests ok\n");
    return 0;
}
