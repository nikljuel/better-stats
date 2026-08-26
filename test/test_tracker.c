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

    /* New session observed: a row starts at zero, pages_start = cpage */
    assert(tracker_observe(&t, &s, 0) == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 0);
    assert(q1(t.stats, "SELECT pages_start FROM sessions") == 10);

    /* Page turn after 60s: active += 60 */
    set_state(exp, 1000, 1060, 12);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 60) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 60);
    assert(q1(t.stats, "SELECT pages_end FROM sessions") == 12);

    /* The firmware only moves the session's end, so the row is recomputed from
     * its endpoints -- it stays one session, it does not accumulate. */
    set_state(exp, 1000, 1060 + 3600, 45); /* 35 pages: page ceiling not in play */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 3660) == 2);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=1000") == 3660);
    assert(q1(t.stats, "SELECT end_time FROM sessions WHERE start_time=1000") == 4660);
    assert(q1(t.stats, "SELECT pages_end FROM sessions WHERE start_time=1000") == 45);

    /* No new position_ts -> nothing happens */
    assert(tracker_observe(&t, &s, 0) == 0);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 1);

    /* An observed session is NOT capped: its off-time was measured, so the
     * remainder is real reading however long it is. */
    set_state(exp, 1000, 1000 + 10 * 3600, 400); /* 390 pages */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 10 * 3600) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=1000")
           == 10 * 3600);

    /* New opentime = new session; pages_start = pages_end of the old one */
    set_state(exp, 90000, 90005, 402);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 5) == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 2);
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=90000") == 400);
    /* A row starts at zero: presence is only counted from the moment the daemon
     * saw the session, never backdated to the firmware's opentime. */
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=90000") == 0);

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

    /* Backfilled sessions are capped -- nobody watched them */
    set_state(exp, 50000, 50000 + 10 * 3600, 60);
    tracker_recover(&t);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=50000") ==
           SESSION_CAP_SECONDS);

    /* Stale position_ts: the firmware stamps opentime on open but refreshes
     * position_ts only on a page turn, so it still holds the previous
     * session's value. A recovered row must not end before it started. */
    set_state(exp, 100000, 99000, 70);
    tracker_recover(&t);
    assert(q1(t.stats, "SELECT end_time FROM sessions WHERE start_time=100000") == 100000);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=100000") == 0);

    /* Same staleness live: the daemon starts mid-session, then the first page
     * turn must count only the time since opentime -- not the whole bogus gap
     * back to the stale position_ts (which would land at IDLE_CAP). */
    set_state(exp, 200000, 199000, 80);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(s.position_ts == 200000); /* clamped on read */
    assert(tracker_observe(&t, &s, 0) == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=200000") == 0);
    set_state(exp, 200000, 200120, 82);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 120) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=200000") == 120);

    /* Rows written by older versions get retrofitted on open */
    ex(t.stats, "UPDATE sessions SET active_seconds=6*3600, pages_start=5"
                " WHERE start_time=50000");
    tracker_close(&t);
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=50000") ==
           SESSION_CAP_SECONDS);
    assert(q1(t.stats, "SELECT pages_start IS NULL FROM sessions WHERE start_time=50000") == 1);

    /* Short pauses between page turns count in full */
    set_state(exp, 400000, 400000, 90);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 1);
    set_state(exp, 400000, 400000 + 60, 95);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 60) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=400000") == 60);

    /* Sparse observation is the normal case, not an error: the firmware only
     * flushes when a session ends, so one late observation must still yield the
     * full session rather than a zero-length row. */
    set_state(exp, 400000, 400000 + 60 + 3600, 130); /* 40 pages: ceiling clear */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 3660) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=400000") == 3660);
    assert(q1(t.stats, "SELECT end_time FROM sessions WHERE start_time=400000") == 403660);
    assert(q1(t.stats, "SELECT pages_end FROM sessions WHERE start_time=400000") == 130);

    /* An hour of wall clock, but the daemon was only present for ten minutes of
     * it -- the rest the device was locked. Only the presence counts. */
    set_state(exp, 500000, 500000, 200);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 1000) == 1);
    set_state(exp, 500000, 500000 + 3600, 210);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 1000 + 600) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=500000") == 600);

    /* The page ceiling bounds an awake device nobody was reading on: two pages
     * turned may buy at most 2 * SECONDS_PER_PAGE_CAP, however long we sat there. */
    set_state(exp, 600000, 600000, 300);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 5000) == 1);
    set_state(exp, 600000, 600000 + 7200, 302); /* 2 h present, 2 pages */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 5000 + 7200) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=600000")
           == 2 * SECONDS_PER_PAGE_CAP);

    /* Session across local midnight is split so both days get their time */
    set_state(exp, 82500, 82500, 20); /* 1970-01-01 23:55 CET */
    /* (the 400000 session is still current, so a new opentime starts this one) */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 1);
    set_state(exp, 82500, 82900, 22); /* 1970-01-02 00:01:40 CET */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 400) == 2);
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
