/* Host test: session derivation from explorer-3-like snapshots. */
#define TRACKER_TEST_API 1
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
           "CREATE TABLE folders (id INTEGER PRIMARY KEY,name TEXT);"
           "CREATE TABLE files (book_id INTEGER, storageid INTEGER, fast_hash BLOB,"
           " folder_id INTEGER,filename TEXT);"
           "CREATE TABLE books_settings (bookid INTEGER, profileid INTEGER,"
           " position TEXT, position_ts INTEGER, cpage INTEGER, npage INTEGER,"
           " opentime INTEGER, completed INTEGER, completed_ts INTEGER);");
    ex(db, "INSERT INTO books_impl VALUES (7,'Testbuch','Autorin');"
           "INSERT INTO folders VALUES (1,'/mnt/ext1/Books');"
           "INSERT INTO files VALUES (7,1,x'aabb',1,'test.epub');"
           "INSERT INTO books_settings VALUES (7,1,'p',1000,10,300,1000,0,0);");
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

static int trace_savepoints(unsigned type, void *context, void *statement,
                            void *unused)
{
    (void)unused;
    if (type == SQLITE_TRACE_STMT) {
        const char *sql = sqlite3_sql((sqlite3_stmt *)statement);
        if (sql && !strncmp(sql, "SAVEPOINT betterstats_tracker", 29))
            (*(int *)context)++;
    }
    return 0;
}

static int trace_path_queries(unsigned type, void *context, void *statement,
                              void *unused)
{
    (void)unused;
    if (type == SQLITE_TRACE_STMT) {
        const char *sql = sqlite3_sql((sqlite3_stmt *)statement);
        if (sql && strstr(sql, "SELECT DISTINCT f.book_id"))
            (*(int *)context)++;
    }
    return 0;
}

/* Keep the long-standing scenario tests readable while exercising the new
 * explicit lifecycle API. New contract tests below call the raw wrappers. */
static int raw_prepare(tracker *t, const pb_state *s, int64_t present,
                       int64_t wall)
{
    return tracker_prepare(t, s, present, present, wall);
}

static int raw_observe(tracker *t, const pb_state *s, int64_t present,
                       int64_t wall)
{
    return tracker_observe(t, s, present, present, wall);
}

static int raw_flush(tracker *t, const pb_state *s, int64_t present,
                     int64_t wall)
{
    return tracker_flush(t, s, present, present, wall);
}

static int raw_flush_bounded(tracker *t, const pb_state *s, int64_t present,
                             int64_t boot, int64_t wall, int64_t max_end)
{
    return tracker_flush_bounded(t, s, present, boot, wall, max_end);
}

static int raw_resume(tracker *t, int64_t present, int64_t wall)
{
    return tracker_resume(t, present, present, wall);
}

static int raw_observe_clock(tracker *t, const pb_state *s, int64_t present,
                             int64_t boot_time, int64_t wall)
{
    return tracker_observe(t, s, present, boot_time, wall);
}

static int raw_resume_clock(tracker *t, int64_t present, int64_t boot_time,
                            int64_t wall)
{
    return tracker_resume(t, present, boot_time, wall);
}

static int64_t compatible_wall(tracker *t, const pb_state *s)
{
    int64_t wall = s->opentime;
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(t->stats,
        "SELECT IFNULL(MAX(end_time),0) FROM sessions"
        " WHERE book_id=?1 AND firmware_open_time=?2",
        -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_int64(st, 2, s->opentime);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0) {
        wall = sqlite3_column_int64(st, 0);
        if (s->position_ts > wall)
            wall = s->position_ts;
    }
    sqlite3_finalize(st);
    return wall;
}

static void reset_compat_clock_if_idle(tracker *t)
{
    if (t->cur_book > 0)
        return;
    t->have_clock_sample = 0;
    t->rtc_rollback = t->rtc_rebase_allowed = t->wall_forward_jump = 0;
    t->last_raw_wall = t->last_clock_boot = 0;
    t->rtc_floor = t->rtc_catchup = 0;
}

static void forget_test_session(tracker *t)
{
    t->cur_book = 0;
    reset_compat_clock_if_idle(t);
}

static int observe_compat(tracker *t, const pb_state *s, int64_t present)
{
    reset_compat_clock_if_idle(t);
    if (t->cur_book != s->bookid || t->cur_open != s->opentime) {
        if (t->cur_book > 0) {
            int64_t old_present = present > t->cur_last_present
                ? present : t->cur_last_present;
            pb_state final;
            int final_rc = tracker_read_book_state(t->explorer_path,
                t->cur_book, t->cur_open, 0, &final);
            const pb_state *final_ptr = final_rc == 0
                && final.bookid == t->cur_book && final.opentime == t->cur_open
                ? &final : NULL;
            if (raw_flush(t, final_ptr, old_present, s->opentime - 1) != 0)
                return -1;
        }
        return raw_prepare(t, s, present, compatible_wall(t, s)) >= 0 ? 1 : -1;
    }
    if (present < t->cur_last_present)
        present = t->cur_last_present;
    return raw_observe(t, s, present,
                       s->position_ts > 0 ? s->position_ts : s->opentime);
}

static int prepare_compat(tracker *t, const pb_state *s, int64_t present)
{
    reset_compat_clock_if_idle(t);
    return raw_prepare(t, s, present, compatible_wall(t, s)) >= 0 ? 0 : -1;
}

static int flush_compat(tracker *t, int64_t present, int64_t wall)
{
    if (present < t->cur_last_present)
        present = t->cur_last_present;
    return raw_flush(t, NULL, present, wall);
}

#define tracker_observe(t, s, present) observe_compat((t), (s), (present))
#define tracker_prepare(t, s, present) prepare_compat((t), (s), (present))
#define tracker_resume(t, present, wall) raw_resume((t), (present), (wall))
#define tracker_flush(t, present, wall) flush_compat((t), (present), (wall))

int main(void)
{
    /* Fixed zone: the day-boundary logic is local-time based. */
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

    /* Portable proportional math stays exact at int64 boundaries and reports
     * a mathematically unrepresentable result instead of overflowing. */
    {
        int64_t value = -1;
        assert(tracker_test_mul_div_floor(400, 300, 400, &value) == 0
               && value == 300);
        assert(tracker_test_mul_div_floor(INT64_MAX, INT64_MAX,
                                          INT64_MAX, &value) == 0
               && value == INT64_MAX);
        assert(tracker_test_mul_div_floor(INT64_MAX - 1, INT64_MAX - 2,
                                          INT64_MAX, &value) == 0
               && value == INT64_MAX - 3);
        int64_t divisor = 86401;
        int64_t ceiling = INT64_MAX / divisor
            + (INT64_MAX % divisor != 0);
        assert(tracker_test_mul_div_floor(INT64_MAX, divisor - 1,
                                          divisor, &value) == 0
               && value == INT64_MAX - ceiling);
        assert(tracker_test_mul_div_floor(INT64_MAX, INT64_MAX, 1,
                                          &value) < 0);
        assert(tracker_test_mul_div_floor(1, 1, 0, &value) < 0);
        for (int64_t a = 0; a < 32; ++a)
            for (int64_t b = 0; b < 32; ++b)
                for (int64_t d = 1; d < 32; ++d) {
                    assert(tracker_test_mul_div_floor(a, b, d, &value) == 0);
                    assert(value == a * b / d);
                }
    }

    sqlite3 *exp = make_explorer();
    unlink(ST_DB);

    /* Upgrade the previous schema in place, preserving its rows. */
    sqlite3 *legacy = NULL;
    assert(sqlite3_open(ST_DB, &legacy) == SQLITE_OK);
    ex(legacy, "CREATE TABLE books (book_id INTEGER PRIMARY KEY,title TEXT,"
               " author TEXT,cover TEXT,cpage INTEGER,npage INTEGER,"
               " completed INTEGER,last_seen INTEGER);"
               "CREATE TABLE sessions (book_id INTEGER NOT NULL,"
               " start_time INTEGER NOT NULL,end_time INTEGER NOT NULL,"
               " active_seconds INTEGER NOT NULL DEFAULT 0,pages_start INTEGER,"
               " pages_end INTEGER,recovered INTEGER NOT NULL DEFAULT 0,"
               " PRIMARY KEY(book_id,start_time));"
               "INSERT INTO books VALUES(99,'Alt','A','',1,2,1,3);"
               "INSERT INTO sessions VALUES(98,10,20,10,1,2,1)");
    sqlite3_close(legacy);

    tracker t;
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
    sqlite3 *held_explorer = NULL;
    {
        int64_t bookid = 0;
        pb_state cached;
        assert(tracker_cached_book_id_for_path(
                   &t, "/mnt/ext1/Books/test.epub", &bookid) == 0);
        assert(bookid == 7 && t.explorer != NULL);
        sqlite3 *held = held_explorer = t.explorer;
        int path_queries = 0;
        sqlite3_trace_v2(t.explorer, SQLITE_TRACE_STMT, trace_path_queries,
                         &path_queries);
        assert(tracker_cached_book_id_for_path(
                   &t, "/mnt/ext1/Books/test.epub", &bookid) == 0);
        assert(path_queries == 0);
        tracker_invalidate_book_path_cache(&t);
        assert(tracker_cached_book_id_for_path(
                   &t, "/mnt/ext1/Books/test.epub", &bookid) == 0);
        assert(path_queries == 1);
        sqlite3_trace_v2(t.explorer, 0, NULL, NULL);
        assert(tracker_cached_read_book_state(&t, 7, 1000, 0,
                                              &cached) == 0);
        assert(cached.bookid == 7 && t.explorer == held);
    }
    assert(q1(t.stats, "SELECT completed_ts FROM books WHERE book_id=99") == 0);
    assert(q1(t.stats, "SELECT COUNT(*) FROM pragma_table_info('sessions')"
                       " WHERE name='pages_moved'") == 1);
    assert(q1(t.stats, "SELECT COUNT(*)"
                       " FROM pragma_table_info('sessions_archived')"
                       " WHERE name='pages_moved'") == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM pragma_table_info('sessions')"
                       " WHERE name='firmware_open_time'") == 1);
    assert(q1(t.stats, "SELECT COUNT(*)"
                       " FROM pragma_table_info('sessions_archived')"
                       " WHERE name='firmware_open_time'") == 1);
    assert(q1(t.stats, "SELECT recovered FROM sessions_archived"
                       " WHERE book_id=98") == 1);
    assert(q1(t.stats, "SELECT pages_moved FROM sessions_archived"
                       " WHERE book_id=98") == 0);
    assert(q1(t.stats, "SELECT firmware_open_time FROM sessions_archived"
                       " WHERE book_id=98") == 0);
    ex(t.stats, "DELETE FROM sessions_archived WHERE book_id=98");
    ex(t.stats, "DELETE FROM books WHERE book_id=99");

    /* read_state returns the book incl. metadata + cover hash */
    pb_state s;
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(s.bookid == 7 && s.opentime == 1000 && s.cpage == 10);
    assert(strcmp(s.title, "Testbuch") == 0);
    assert(strcmp(s.cover, "1aabb") == 0);
    assert(s.completed_ts == 0);
    tracker invalid_before = t;
    assert((tracker_prepare)(&t, &s, -1, 0, 1000) < 0);
    assert(memcmp(&t, &invalid_before, sizeof(t)) == 0);

    /* New session observed: a row starts at zero, pages_start = cpage */
    assert(tracker_observe(&t, &s, 0) == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 0);
    assert(q1(t.stats, "SELECT pages_start FROM sessions") == 10);
    invalid_before = t;
    assert((tracker_observe)(&t, &s, -1, 0, 1000) < 0);
    assert((tracker_resume)(&t, -1, 0, 1000) < 0);
    assert(memcmp(&t, &invalid_before, sizeof(t)) == 0);

    /* Finishing or unfinishing does not require a page turn, but both changes
     * must still reach the durable book row. */
    ex(exp, "UPDATE books_settings SET completed=1,completed_ts=12345"
            " WHERE bookid=7");
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 2);
    assert(q1(t.stats, "SELECT completed_ts FROM books WHERE book_id=7") == 12345);
    ex(exp, "UPDATE books_settings SET completed=0,completed_ts=0 WHERE bookid=7");
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 2);
    assert(q1(t.stats, "SELECT completed FROM books WHERE book_id=7") == 0);
    assert(q1(t.stats, "SELECT completed_ts FROM books WHERE book_id=7") == 0);

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

    /* A new firmware session starts from its own authoritative page. */
    set_state(exp, 90000, 90005, 402);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 5) == 1);
    assert(t.explorer == held_explorer);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 2);
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=90000") == 402);
    /* A row starts at zero: presence is only counted from the moment the daemon
     * saw the session, never backdated to the firmware's opentime. */
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=90000") == 0);

    /* Recovery: books are imported, but no sessions are created */
    tracker_close(&t);
    set_state(exp, 20000, 20000 + 1200, 40);
    ex(exp, "INSERT INTO books_impl VALUES(8,'Fertig','Autor');"
            "INSERT INTO books_settings VALUES(8,1,'p',0,0,0,0,1,23456)");
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
    assert(tracker_recover(&t, 0, 0) >= 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 2);
    assert(q1(t.stats, "SELECT completed_ts FROM books WHERE book_id=8") == 23456);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions WHERE book_id=8") == 0);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions WHERE start_time=20000") == 0);
    int recovery_changes = sqlite3_total_changes(t.stats);
    assert(tracker_recover(&t, 0, 0) >= 1);
    assert(sqlite3_total_changes(t.stats) == recovery_changes);
    ex(t.stats, "DELETE FROM books WHERE book_id=8");
    ex(exp, "DELETE FROM books_settings WHERE bookid=8;"
            "DELETE FROM books_impl WHERE id=8");
    tracker_recover(&t, 0, 0); /* idempotent */
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 2);

    /* Archive migration: recovered rows from a pre-fix DB are moved */
    ex(t.stats, "INSERT OR IGNORE INTO sessions (book_id,start_time,end_time,"
                "active_seconds,recovered) VALUES (80,2000000,2001200,1200,1)");
    tracker_close(&t);
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions WHERE book_id=80") == 0);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions_archived WHERE book_id=80") == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions_archived"
                       " WHERE book_id=80") == 1200);
    ex(t.stats, "DELETE FROM sessions_archived WHERE book_id=80");
    tracker_close(&t);
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);

    /* Stale position_ts live: the daemon starts mid-session, then the first
     * page turn must count only the time since opentime -- not the whole bogus
     * gap back to the stale position_ts. */
    set_state(exp, 200000, 199000, 80);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(s.position_ts == 199000); /* only zero is clamped; RTC logic keeps raw */
    assert(tracker_observe(&t, &s, 0) == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=200000") == 0);
    set_state(exp, 200000, 200120, 82);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 120) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=200000") == 120);

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

    /* The page ceiling bounds an awake device nobody was reading on: the page
     * being read plus two turns = 3 pages of budget, however long we sat. */
    set_state(exp, 600000, 600000, 300);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 5000) == 1);
    set_state(exp, 600000, 600000 + 7200, 302); /* 2 h present, 2 pages */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 5000 + 7200) == 2);
    assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                       " WHERE start_time IN (600000,601200)")
           == 3 * SECONDS_PER_PAGE_CAP);

    /* Session across local midnight is split so both days get their time */
    set_state(exp, 687300, 687300, 20); /* 1970-01-08 23:55 CET */
    /* (the 400000 session is still current, so a new opentime starts this one) */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 1);
    set_state(exp, 687300, 687700, 22); /* 1970-01-09 00:01:40 CET */
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 400) == 2);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=687300") == 300);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=687600") == 100);
    assert(q1(t.stats, "SELECT date(end_time,'unixepoch','localtime')='1970-01-08'"
                       " FROM sessions WHERE start_time=687300") == 1);
    assert(q1(t.stats, "SELECT date(end_time,'unixepoch','localtime')='1970-01-09'"
                       " FROM sessions WHERE start_time=687600") == 1);
    /* The post-midnight snapshot is applied only after the day split: the old
     * bucket keeps its previous page state, and the new bucket owns the turn. */
    assert(q1(t.stats, "SELECT pages_end FROM sessions WHERE start_time=687300") == 20);
    assert(q1(t.stats, "SELECT pages_moved FROM sessions WHERE start_time=687300") == 1);
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=687600") == 20);
    assert(q1(t.stats, "SELECT pages_end FROM sessions WHERE start_time=687600") == 22);
    assert(q1(t.stats, "SELECT pages_moved FROM sessions WHERE start_time=687600") == 3);

    /* A deleted book loses its files row, so the firmware can no longer supply
     * a cover key -- but the image we cached under it is still on disk. The
     * upsert must therefore never trade a known key for an empty one. */
    ex(t.stats, "INSERT INTO books (book_id,title,cover) VALUES (99,'Weg','1deadbeef')"
                " ON CONFLICT(book_id) DO UPDATE SET cover='1deadbeef'");
    {
        pb_state gone;
        memset(&gone, 0, sizeof gone);
        gone.bookid = 99;
        gone.opentime = 700000;
        gone.position_ts = 700060;
        snprintf(gone.title, sizeof gone.title, "Weg");
        gone.cover[0] = '\0'; /* files row is gone */
        assert(tracker_observe(&t, &gone, 700060) == 1);
        assert(q1(t.stats, "SELECT cover='1deadbeef' FROM books WHERE book_id=99") == 1);
    }
    /* A real key still overwrites, otherwise a moved book would keep a stale one */
    {
        pb_state back;
        memset(&back, 0, sizeof back);
        back.bookid = 99;
        back.opentime = 700100;
        back.position_ts = 700160;
        snprintf(back.title, sizeof back.title, "Weg");
        snprintf(back.cover, sizeof back.cover, "2cafebabe");
        assert(tracker_observe(&t, &back, 700160) == 1);
        assert(q1(t.stats, "SELECT cover='2cafebabe' FROM books WHERE book_id=99") == 1);
    }
    ex(t.stats, "DELETE FROM books WHERE book_id=99;"
                "DELETE FROM sessions WHERE book_id=99");
    forget_test_session(&t); /* the probe book is gone; don't resume it */

    /* A restart restores both the row and its page budget. The two turns before
     * restart still permit later time on the unchanged page. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 55;
        r.opentime = 800000;
        snprintf(r.title, sizeof r.title, "Neustart");
        r.position_ts = 800000; r.cpage = 10;
        assert(tracker_observe(&t, &r, 0) == 1);
        r.position_ts = 800060; r.cpage = 12;
        assert(tracker_observe(&t, &r, 60) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=800000") == 60);

        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(tracker_observe(&t, &r, 0) == 1); /* fresh tracker, counter at 0 */
        assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=800000") == 60);
        r.position_ts = 800360;
        assert(tracker_observe(&t, &r, 300) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=800000") == 360);
    }

    /* A page turn written by the firmware while the daemon is down restores
     * another page of budget when the same session is adopted. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 58; r.opentime = 850000;
        snprintf(r.title, sizeof r.title, "OfflinePageTurn");
        r.position_ts = 850000; r.cpage = 10;
        assert(tracker_observe(&t, &r, 0) == 1);
        r.position_ts = 850300;
        assert(tracker_observe(&t, &r, 300) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=58") == SECONDS_PER_PAGE_CAP);

        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        r.position_ts = 850301; r.cpage = 11; /* changed during downtime */
        assert(tracker_observe(&t, &r, 0) == 1);
        r.position_ts = 850421;
        assert(tracker_observe(&t, &r, 120) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=58") == 420);
    }

    /* A stale firmware snapshot after restart cannot move the durable page
     * endpoint backwards or create phantom page budget when it catches up. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 59; r.opentime = 875000;
        snprintf(r.title, sizeof r.title, "StaleRestart");
        r.position_ts = 875000; r.cpage = 10;
        assert(tracker_observe(&t, &r, 0) == 1);
        r.position_ts = 875100; r.cpage = 20;
        assert(tracker_observe(&t, &r, 100) == 2);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=59") == 11);

        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        ex(exp, "INSERT INTO books_impl VALUES(59,'StaleRestart','Autor');"
                "INSERT INTO books_settings"
                " VALUES(59,1,'p',875050,15,100,875000,0,0)");
        r.position_ts = 875050; r.cpage = 15;
        assert(tracker_observe(&t, &r, 0) == 1);
        tracker_flush(&t, 0, 875100);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE book_id=59") == 875100);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=59") == 20);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=59") == 11);
        assert(q1(t.stats, "SELECT cpage FROM books WHERE book_id=59") == 20);
        assert(q1(t.stats, "SELECT last_seen FROM books WHERE book_id=59") == 875100);

        r.position_ts = 875200; r.cpage = 20;
        assert(tracker_observe(&t, &r, 100) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=59") == 200);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=59") == 11);
        ex(exp, "DELETE FROM books_settings WHERE bookid=59;"
                "DELETE FROM books_impl WHERE id=59");
    }

    /* Reading backwards is reading. A session ending on an earlier page than it
     * started must not lose its time to a page budget of zero. */
    {
        pb_state b;
        memset(&b, 0, sizeof b);
        b.bookid = 56;
        b.opentime = 900000;
        snprintf(b.title, sizeof b.title, "Rueckwaerts");
        b.position_ts = 900060; b.cpage = 100;
        assert(tracker_observe(&t, &b, 0) == 1);
        b.position_ts = 900600; b.cpage = 120;
        assert(tracker_observe(&t, &b, 540) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=900000") == 540);
        b.position_ts = 900900; b.cpage = 90; /* back past the start page */
        assert(tracker_observe(&t, &b, 840) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions WHERE start_time=900000") == 840);
    }

    /* Recovery populates books; the daemon then tracks a fresh session. */
    set_state(exp, 950000, 950500, 300);
    tracker_close(&t);
    assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
    assert(tracker_recover(&t, 0, 0) >= 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions WHERE start_time=950000") == 0);
    assert(q1(t.stats, "SELECT book_id FROM books WHERE book_id=7") == 7);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 1);
    assert(q1(t.stats, "SELECT recovered FROM sessions WHERE start_time=950000") == 0);

    ex(t.stats, "DELETE FROM books WHERE book_id IN (55,56,58,59);"
                "DELETE FROM sessions WHERE book_id IN (55,56,58,59)");
    forget_test_session(&t);

    /* Book switch flushes the old book's accumulated time. */
    {
        pb_state a, b;
        memset(&a, 0, sizeof a);
        a.bookid = 70; a.opentime = 1100000;
        snprintf(a.title, sizeof a.title, "FlushA");
        a.position_ts = 1100000; a.cpage = 10;
        assert(tracker_observe(&t, &a, 0) == 1);
        a.position_ts = 1100060; a.cpage = 12;
        assert(tracker_observe(&t, &a, 60) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=70 AND start_time=1100000") == 60);
        /* 200 more seconds pass with no page turn, then switch to book B. */
        memset(&b, 0, sizeof b);
        b.bookid = 71; b.opentime = 1100261;
        snprintf(b.title, sizeof b.title, "FlushB");
        b.position_ts = 1100320; b.cpage = 5;
        assert(tracker_observe(&t, &b, 260) == 1);
        /* Flush captured the full 260s (1 initial + 2 turns → cap 900 > 260). */
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=70 AND start_time=1100000") == 260);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE book_id=70 AND start_time=1100000") == 1100260);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id IN (70,71);"
                "DELETE FROM books WHERE book_id IN (70,71)");
    forget_test_session(&t);

    /* A book switch may arrive before the old book's final DB write was
     * observed. Flush reads that stable old row once and keeps its final page. */
    {
        pb_state a, b;
        ex(exp, "INSERT INTO books_impl VALUES(74,'FinalPage','Autor');"
                "INSERT INTO books_settings"
                " VALUES(74,1,'p',1400000,10,100,1400000,0,0)");
        assert(tracker_read_state(EXP_DB, &a) == 0);
        assert(a.bookid == 74 && a.cpage == 10);
        assert(tracker_observe(&t, &a, 0) == 1);
        ex(exp, "UPDATE books_settings SET position_ts=1400180,cpage=13"
                " WHERE bookid=74");

        memset(&b, 0, sizeof b);
        b.bookid = 75; b.opentime = 1400201; b.position_ts = 1400201;
        b.cpage = 4; b.npage = 100;
        snprintf(b.title, sizeof b.title, "NextBook");
        assert(tracker_observe(&t, &b, 200) == 1);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=74 AND start_time=1400000") == 13);
        assert(q1(t.stats, "SELECT cpage FROM books WHERE book_id=74") == 13);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=74 AND start_time=1400000") == 200);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE book_id=74 AND start_time=1400000") == 1400200);
    }
    ex(exp, "DELETE FROM books_settings WHERE bookid=74;"
            "DELETE FROM books_impl WHERE id=74");
    ex(t.stats, "DELETE FROM sessions WHERE book_id IN (74,75);"
                "DELETE FROM books WHERE book_id IN (74,75)");
    forget_test_session(&t);

    /* tracker_flush persists in-memory time (simulates daemon exit). */
    {
        pb_state f;
        memset(&f, 0, sizeof f);
        f.bookid = 72; f.opentime = 1200000;
        snprintf(f.title, sizeof f.title, "FlushMe");
        f.position_ts = 1200000; f.cpage = 15;
        assert(tracker_observe(&t, &f, 0) == 1);
        f.position_ts = 1200060; f.cpage = 17;
        assert(tracker_observe(&t, &f, 60) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE start_time=1200000") == 60);
        /* 120 more seconds, no page turn. Without flush this would be lost. */
        tracker_flush(&t, 180, 1200180);
        /* 1 initial + 2 turns → cap 900 > 180, so full 180s credited. */
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE start_time=1200000") == 180);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE start_time=1200000") == 1200180);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=72;"
                "DELETE FROM books WHERE book_id=72");
    forget_test_session(&t);

    /* Reading one page without turning: position_ts updates but cpage stays.
     * The initial page budget (1) caps at SECONDS_PER_PAGE_CAP. */
    {
        pb_state np;
        memset(&np, 0, sizeof np);
        np.bookid = 73; np.opentime = 1300000;
        snprintf(np.title, sizeof np.title, "NoPageTurn");
        np.position_ts = 1300000; np.cpage = 5;
        assert(tracker_observe(&t, &np, 0) == 1);
        np.position_ts = 1300060; /* position_ts moved, cpage didn't */
        assert(tracker_observe(&t, &np, 60) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE start_time=1300000") == 60);
        np.position_ts = 1300400;
        assert(tracker_observe(&t, &np, 400) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE start_time=1300000") == SECONDS_PER_PAGE_CAP);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=73;"
                "DELETE FROM books WHERE book_id=73");
    forget_test_session(&t);

    /* A no-turn exit across midnight is split without granting the one-page
     * ceiling once per day. Total credit remains one page = 300 seconds. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 76; m.opentime = 82500; m.position_ts = 82500;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "MidnightFlush");
        assert(tracker_observe(&t, &m, 0) == 1);
        tracker_flush(&t, 400, 82900);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions WHERE book_id=76") == 2);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=76") == SECONDS_PER_PAGE_CAP);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=76 AND start_time=82500") == 225);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=76 AND start_time=82800") == 75);

        /* Restart after the split: the unchanged page already exhausted its
         * 300-second budget, so it must not earn another restart allowance. */
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        m.position_ts = 82900;
        assert(tracker_observe(&t, &m, 0) == 1);
        m.position_ts = 82960;
        assert(tracker_observe(&t, &m, 60) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=76 AND start_time=82800") == 75);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=76") == SECONDS_PER_PAGE_CAP);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=76;"
                "DELETE FROM books WHERE book_id=76");
    forget_test_session(&t);

    /* A checkpoint already durable before midnight cannot be redistributed
     * into tomorrow and thereby spend the same one-page budget twice. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 79; m.opentime = m.position_ts = 82500;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "MidnightCheckpoint");
        assert(raw_prepare(&t, &m, 0, 82500) == 1);
        assert(raw_flush(&t, NULL, 300, 82799) == 0);
        assert(raw_flush(&t, NULL, 400, 82900) == 0);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions WHERE book_id=79") == 2);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=79") == SECONDS_PER_PAGE_CAP);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=79 AND start_time=82800") == 0);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=79;"
                "DELETE FROM books WHERE book_id=79");
    forget_test_session(&t);

    /* A synthesized endpoint exactly at midnight must not squeeze 301 active
     * seconds into the old day's 300-second inclusive capacity. A later page
     * turn unlocks the latent second and carries it into the new day. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 101; m.opentime = m.position_ts = 82500;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "MidnightCapacity");
        assert(raw_prepare(&t, &m, 0, 82500) == 1);
        assert(raw_flush(&t, NULL, 301, 82500) == 0);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=101") == 300);
        m.cpage = 21;
        assert(raw_observe(&t, &m, 301, 82500) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=101 AND start_time=82500") == 300);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE book_id=101 AND start_time=82500") == 82799);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=101 AND start_time=82800") == 1);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=101") == 301);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=101;"
                "DELETE FROM books WHERE book_id=101");
    forget_test_session(&t);

    /* A forward wall timestamp cannot make presence already checkpointed in
     * yesterday's row measurable a second time in today's row. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 102; m.opentime = m.position_ts = 82740;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "MidnightDurable");
        assert(raw_prepare(&t, &m, 0, 82740) == 1);
        assert(raw_flush(&t, NULL, 50, 82790) == 0);
        m.position_ts = 83340;
        assert(raw_observe_clock(&t, &m, 50, 50, 83340) == 2);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=102") == 50);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=102;"
                "DELETE FROM books WHERE book_id=102");
    forget_test_session(&t);

    /* A legacy primary-key collision can move the new-day row forward. Its
     * endpoint moves with it so active time still fits the inclusive span. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 105; m.opentime = m.position_ts = 82798;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "MidnightCollision");
        assert(raw_prepare(&t, &m, 0, 82798) == 1);
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,firmware_open_time)"
                    " VALUES(105,82800,82800,0,1)");
        assert(raw_flush(&t, NULL, 4, 82801) == 0);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=105 AND firmware_open_time=82798"
                           " AND start_time=82801") == 2);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE book_id=105 AND firmware_open_time=82798"
                           " AND start_time=82801") == 82802);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=105 AND firmware_open_time=82798") == 4);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=105;"
                "DELETE FROM books WHERE book_id=105");
    forget_test_session(&t);

    /* The same collision under a proven switch may discard the displaced
     * second, but must never move A past B's hard boundary. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 106; m.opentime = m.position_ts = 82798;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "BoundedCollision");
        assert(raw_prepare(&t, &m, 0, 82798) == 1);
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,firmware_open_time)"
                    " VALUES(106,82800,82800,0,1)");
        assert(raw_flush_bounded(&t, &m, 4, 4, 82801, 82801) == 0);
        assert(q1(t.stats, "SELECT MAX(end_time) FROM sessions"
                           " WHERE book_id=106 AND firmware_open_time=82798")
               == 82801);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=106 AND firmware_open_time=82798")
               == 3);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=106;"
                "DELETE FROM books WHERE book_id=106");
    forget_test_session(&t);

    /* If the only new-day key inside the boundary is occupied, bounded close
     * commits the old row and drops the remainder instead of becoming fatal. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 110; m.opentime = m.position_ts = 82798;
        m.cpage = 20; m.npage = 100;
        assert(raw_prepare(&t, &m, 0, 82798) == 1);
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,firmware_open_time)"
                    " VALUES(110,82800,82800,0,1)");
        assert(raw_flush_bounded(&t, &m, 3, 3, 82800, 82800) == 0);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=110 AND firmware_open_time=82798")
               == 2);
        assert(q1(t.stats, "SELECT MAX(end_time) FROM sessions"
                           " WHERE book_id=110 AND firmware_open_time=82798")
               == 82799);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=110;"
                "DELETE FROM books WHERE book_id=110");
    forget_test_session(&t);

    /* A proven book switch has a hard wall-time boundary. Excess A presence is
     * discarded atomically and cannot reappear if that session is resumed. */
    {
        pb_state b;
        memset(&b, 0, sizeof b);
        b.bookid = 103; b.opentime = b.position_ts = 7000;
        b.cpage = 1; b.npage = 100;
        snprintf(b.title, sizeof b.title, "BoundedClose");
        assert(raw_prepare(&t, &b, 0, 7000) == 1);
        b.cpage = 3;
        assert(raw_observe(&t, &b, 0, 7000) == 2);
        b.position_ts = 7500; /* final firmware snapshot is already beyond B */
        assert(raw_flush_bounded(&t, &b, 500, 500, 7500, 7099) == 0);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=103") == 100);
        assert(q1(t.stats, "SELECT MAX(end_time) FROM sessions"
                           " WHERE book_id=103") == 7099);
        assert(raw_resume_clock(&t, 500, 501, 7501) == 0);
        assert(raw_flush(&t, NULL, 560, 7560) == 0);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=103") == 160);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=103;"
                "DELETE FROM books WHERE book_id=103");
    forget_test_session(&t);

    /* Restart with a stale pre-midnight firmware timestamp resumes the latest
     * durable day row rather than writing new presence back into yesterday. */
    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 77; m.opentime = 82500; m.position_ts = 82500;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "MidnightRestart");
        assert(tracker_observe(&t, &m, 0) == 1);
        m.position_ts = 82600; m.cpage = 21;
        assert(tracker_observe(&t, &m, 100) == 2);
        tracker_flush(&t, 400, 82900);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=77 AND start_time=82500") == 300);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=77 AND start_time=82800") == 100);

        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(tracker_observe(&t, &m, 0) == 1); /* position_ts still 82600 */
        tracker_flush(&t, 60, 82960);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=77 AND start_time=82500") == 300);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=77 AND start_time=82800") == 160);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=77;"
                "DELETE FROM books WHERE book_id=77");
    forget_test_session(&t);

    /* Pauses are separate rows within one firmware opentime. A transition in
     * the row's start second merges; a later resume starts a new fragment. */
    {
        pb_state p;
        memset(&p, 0, sizeof p);
        p.bookid = 80; p.opentime = 1400000; p.position_ts = 1400000;
        p.cpage = 10; p.npage = 100;
        snprintf(p.title, sizeof p.title, "Fragments");
        assert(tracker_prepare(&t, &p, 0) == 0);
        assert(tracker_resume(&t, 0, 1400000) == 0); /* same-second merge */
        assert(tracker_observe(&t, &p, 0) == 0);
        p.position_ts = 1400060; p.cpage = 11;
        assert(tracker_observe(&t, &p, 60) == 2);
        assert(tracker_flush(&t, 60, 1400060) == 0);
        assert(tracker_resume(&t, 60, 1400100) == 0);
        assert(tracker_observe(&t, &p, 60) == 0);
        assert(tracker_flush(&t, 120, 1400160) == 0);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions WHERE book_id=80") == 2);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=80") == 120);

        /* A failed write rolls back both tables and the proposed C state. */
        tracker before = t;
        long old_end = q1(t.stats, "SELECT MAX(end_time) FROM sessions"
                                   " WHERE book_id=80");
        ex(t.stats, "CREATE TRIGGER fail_session_update BEFORE UPDATE ON sessions"
                    " BEGIN SELECT RAISE(ABORT,'test'); END");
        p.position_ts = 1400220; p.cpage = 12;
        assert(tracker_observe(&t, &p, 180) < 0);
        assert(!tracker_error_retryable(&t));
        t.last_error = before.last_error; /* status is not tracker state */
        assert(memcmp(&t, &before, sizeof(t)) == 0);
        assert(q1(t.stats, "SELECT MAX(end_time) FROM sessions"
                           " WHERE book_id=80") == old_end);
        ex(t.stats, "DROP TRIGGER fail_session_update");
        assert(tracker_observe(&t, &p, 180) == 2);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=80;"
                "DELETE FROM books WHERE book_id=80");
    forget_test_session(&t);

    /* Restart catch-up is derived only from the exactly labelled fragment;
     * books is output cache, never movement input. */
    {
        pb_state p;
        memset(&p, 0, sizeof p);
        p.bookid = 81; p.opentime = 1500000; p.position_ts = 1500060;
        p.cpage = 12; p.npage = 100;
        ex(t.stats,
            "INSERT INTO books(book_id,title,cpage,npage,last_seen)"
            " VALUES(81,'Catchup',12,100,1500060);"
            "INSERT INTO sessions(book_id,start_time,end_time,active_seconds,"
            " pages_start,pages_end,pages_moved,firmware_open_time)"
            " VALUES(81,1500000,1500030,30,10,10,1,1500000)");
        assert(tracker_prepare(&t, &p, 30) == 0);
        assert(tracker_resume(&t, 30, 1500100) == 0);
        assert(q1(t.stats, "SELECT pages_start FROM sessions"
                           " WHERE book_id=81 AND start_time=1500100") == 12);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=81 AND start_time=1500100") == 12);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=81 AND start_time=1500100") == 3);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=81;"
                "DELETE FROM books WHERE book_id=81");
    forget_test_session(&t);

    /* Exact path resolution and deterministic multi-profile state selection. */
    {
        int64_t id = 0;
        pb_state selected;
        assert(tracker_book_id_for_path(EXP_DB,
                   "/mnt/ext1/Books/test.epub", &id) == 0 && id == 7);
        ex(exp, "INSERT INTO files VALUES(7,1,x'aabb',1,'test.epub')");
        assert(tracker_book_id_for_path(EXP_DB,
                   "/mnt/ext1/Books/test.epub", &id) == 0 && id == 7);
        ex(exp, "INSERT INTO files VALUES(90,1,x'cc',1,'test.epub')");
        assert(tracker_book_id_for_path(EXP_DB,
                   "/mnt/ext1/Books/test.epub", &id) == 1);
        ex(exp, "DELETE FROM files WHERE book_id=90");
        assert(tracker_book_id_for_path(EXP_DB, "relative.epub", &id) < 0);

        ex(exp, "INSERT INTO books_impl VALUES(90,'Resolver','A');"
                "INSERT INTO books_settings"
                " VALUES(90,1,'p',0,3,100,5000,0,0),"
                "       (90,2,'p',5004,4,100,5004,0,0)");
        assert(tracker_read_book_state(EXP_DB, 90, 5000, 0,
                                       &selected) == 0);
        assert(selected.opentime == 5000 && selected.position_ts == 5000);
        assert(tracker_read_book_state(EXP_DB, 90, 5999, 0,
                                       &selected) == 1);
        assert(tracker_read_book_state(EXP_DB, 90, 0, 5002,
                                       &selected) == 1);
        assert(tracker_read_book_state(EXP_DB, 90, 0, 4995,
                                       &selected) == 0);
        assert(selected.opentime == 5000);
        assert(tracker_read_book_state(EXP_DB, 90, 0, 0,
                                       &selected) == 1);
        ex(exp, "DELETE FROM books_settings WHERE bookid=90 AND profileid=2");
        assert(tracker_read_book_state(EXP_DB, 90, 0, 0,
                                       &selected) == 0);
        ex(exp, "DELETE FROM books_settings WHERE bookid=90;"
                "DELETE FROM books_impl WHERE id=90");
    }

    ex(t.stats, "DELETE FROM sessions; UPDATE books SET last_seen=0");

    /* Legacy rows (label 0) never get adopted. Initial and resume collisions
     * move forward without overwriting the existing row. */
    {
        pb_state c;
        memset(&c, 0, sizeof c);
        c.bookid = 90; c.opentime = 900; c.position_ts = 900;
        c.cpage = 1; c.npage = 20;
        snprintf(c.title, sizeof c.title, "InitialCollision");
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,firmware_open_time)"
                    " VALUES(90,1000,1000,17,0)");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &c, 0, 1000) == 1);
        assert(t.cur_row_start == 1001);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=90 AND start_time=1000") == 17);
        assert(q1(t.stats, "SELECT firmware_open_time FROM sessions"
                           " WHERE book_id=90 AND start_time=1001") == 900);

        assert(raw_flush(&t, NULL, 10, 1011) == 0);
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,firmware_open_time)"
                    " VALUES(90,1100,1100,9,0)");
        assert(tracker_resume(&t, 10, 1100) == 0);
        assert(t.cur_row_start == 1101);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=90 AND start_time=1100") == 9);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=90;"
                "DELETE FROM books WHERE book_id=90");

    /* A last-second resume collision may cross midnight instead of killing
     * tracking. Midnight rows themselves stay in the new day's bucket. */
    {
        pb_state c;
        memset(&c, 0, sizeof c);
        c.bookid = 91; c.opentime = 82000; c.position_ts = 82000;
        c.cpage = 10; c.npage = 100;
        snprintf(c.title, sizeof c.title, "EndOfDayCollision");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &c, 0, 82000) == 1);
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,firmware_open_time)"
                    " VALUES(91,82799,82799,0,0)");
        assert(tracker_resume(&t, 0, 82799) == 0);
        assert(t.cur_row_start == 82800);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=91;"
                "DELETE FROM books WHERE book_id=91");

    {
        pb_state m;
        memset(&m, 0, sizeof m);
        m.bookid = 92; m.opentime = 82500; m.position_ts = 82500;
        m.cpage = 20; m.npage = 100;
        snprintf(m.title, sizeof m.title, "MidnightCollision");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &m, 0, 82500) == 1);
        m.position_ts = 82600; m.cpage = 21;
        assert(raw_observe(&t, &m, 100, 82600) == 2);
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,firmware_open_time)"
                    " VALUES(92,82800,82800,0,0)");
        assert(raw_flush(&t, NULL, 400, 82900) == 0);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE book_id=92 AND start_time=82500") == 82799);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=92 AND start_time=82801") == 100);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=92 AND firmware_open_time=82500") == 400);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=92;"
                "DELETE FROM books WHERE book_id=92");

    /* The following clock tests use small synthetic epochs independently of
     * the long compatibility scenario above. */
    ex(t.stats, "DELETE FROM sessions; UPDATE books SET last_seen=0");

    /* An existing labelled session, not the global books cache, owns page
     * continuity. A future books timestamp is overwritten, and forward and
     * backward restart movement is persisted exactly once. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 93; r.opentime = 3000; r.position_ts = 3060;
        r.cpage = 12; r.npage = 100;
        snprintf(r.title, sizeof r.title, "RtcBaseline");
        ex(t.stats,
            "INSERT INTO books(book_id,title,cpage,npage,last_seen)"
            " VALUES(93,'poison',99,100,999999);"
            "INSERT INTO sessions(book_id,start_time,end_time,active_seconds,"
            "pages_start,pages_end,pages_moved,firmware_open_time)"
            " VALUES(93,3000,3060,60,10,10,1,3000)");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, 3060) == 0);
        assert(q1(t.stats, "SELECT cpage FROM books WHERE book_id=93") == 12);
        assert(q1(t.stats, "SELECT last_seen FROM books WHERE book_id=93") == 3060);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=93") == 3);

        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, 3060) == 0);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=93") == 3);
        r.position_ts = 3120; r.cpage = 9;
        assert(raw_observe(&t, &r, 60, 3120) == 2);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=93") == 6);
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, 3120) == 0);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=93") == 6);

        /* Lower positional data is stale without wall-clock evidence, while
         * completion metadata remains independently durable. */
        r.position_ts = 3100; r.cpage = 5;
        r.completed = 1; r.completed_ts = 3100;
        assert(raw_observe(&t, &r, 0, 3121) == 2);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=93") == 9);
        assert(q1(t.stats, "SELECT completed FROM books"
                           " WHERE book_id=93") == 1);

        /* A proven wall-clock rollback permits one lower raw rebase. */
        r.position_ts = 3000; r.cpage = 8;
        assert(raw_observe(&t, &r, 0, 3000) == 2);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=93") == 8);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=93") == 7);

        /* The persistent floor gap is not new evidence: only one lower raw
         * firmware snapshot may rebase during this rollback epoch. */
        r.position_ts = 2999; r.cpage = 7;
        assert(raw_observe(&t, &r, 0, 3001) == 0);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=93") == 8);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=93") == 7);

        /* A genuinely new backwards wall-clock step permits one new rebase. */
        r.position_ts = 2900; r.cpage = 7;
        assert(raw_observe(&t, &r, 0, 2900) == 2);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=93") == 7);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=93") == 8);
        assert(raw_observe(&t, &r, 0, 2900) == 0);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=93;"
                "DELETE FROM books WHERE book_id=93");

    /* RTC coherence uses BOOTTIME, not reading credit. A long standby can
     * expose a stopped RTC while present remains unchanged. The resulting
     * epoch permits one lower firmware rebase and stays active until the RTC
     * catches its BOOTTIME-derived target. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 99; r.opentime = r.position_ts = 3500;
        r.cpage = 1; r.npage = 100;
        snprintf(r.title, sizeof r.title, "StandbyClock");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, 3500) == 1);

        assert(raw_resume_clock(&t, 0, 600, 3500) == 0);
        assert(t.rtc_rollback && t.rtc_rebase_allowed);
        assert(t.rtc_catchup == 4100);

        r.position_ts = 3400; r.cpage = 2;
        assert(raw_observe_clock(&t, &r, 0, 601, 3501) == 2);
        assert(t.rtc_rollback && !t.rtc_rebase_allowed);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=99") == 2);

        r.position_ts = 3300; r.cpage = 3;
        assert(raw_observe_clock(&t, &r, 0, 602, 3502) == 0);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=99") == 2);

        r.position_ts = 3400; r.cpage = 2;
        assert(raw_observe_clock(&t, &r, 0, 1200, 4100) == 0);
        assert(!t.rtc_rollback && !t.rtc_rebase_allowed);

        /* Two seconds of skew are tolerated; three seconds are evidence. */
        assert(raw_observe_clock(&t, &r, 0, 1800, 4698) == 0);
        assert(!t.rtc_rollback);
        assert(raw_observe_clock(&t, &r, 0, 2400, 5295) == 0);
        assert(t.rtc_rollback && t.rtc_rebase_allowed);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=99;"
                "DELETE FROM books WHERE book_id=99");

    /* A bad RTC remains a bad RTC across a book switch. Resetting only the
     * per-session clock sample must not let B jump back into the past. */
    {
        pb_state a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.bookid = 107; a.opentime = a.position_ts = 900000; a.cpage = 1;
        b.bookid = 108; b.opentime = b.position_ts = 800001; b.cpage = 1;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &a, 0, 900000) == 1);
        assert(raw_resume_clock(&t, 0, 100, 800000) == 0);
        assert(t.rtc_rollback && t.rtc_floor >= 900000);
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,pages_end,pages_moved,firmware_open_time)"
                    " VALUES(108,899000,899000,0,1,1,800001)");
        assert(raw_prepare(&t, &b, 0, 800001) == 0);
        assert(t.rtc_rollback);
        assert(q1(t.stats, "SELECT MAX(end_time) FROM sessions"
                           " WHERE book_id=108") >= 900000);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id IN (107,108);"
                "DELETE FROM books WHERE book_id IN (107,108)");

    /* A large positive RTC correction creates only the source and destination
     * day rows; it must not write thousands of empty intermediate rows. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 109; r.opentime = r.position_ts = 100000;
        r.cpage = 1; r.npage = 100;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, 100000) == 1);
        time_t far = (time_t)(100000 + 20LL * 366 * 86400);
        struct tm far_tm;
        assert(localtime_r(&far, &far_tm) != NULL);
        far_tm.tm_hour = far_tm.tm_min = far_tm.tm_sec = 0;
        far_tm.tm_isdst = -1;
        r.position_ts = (int64_t)mktime(&far_tm);
        r.cpage = 2;
        assert(raw_observe_clock(&t, &r, 10, 10, 100010) == 2);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions"
                           " WHERE book_id=109") == 2);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=109") == 10);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=109;"
                "DELETE FROM books WHERE book_id=109");

    /* Reopening a years-old labelled row at today's unchanged snapshot must
     * not manufacture one empty database row per skipped calendar day. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        time_t far = (time_t)(100000 + 20LL * 366 * 86400);
        struct tm far_tm;
        assert(localtime_r(&far, &far_tm) != NULL);
        far_tm.tm_hour = far_tm.tm_min = far_tm.tm_sec = 0;
        far_tm.tm_isdst = -1;
        int64_t today = (int64_t)mktime(&far_tm);
        r.bookid = 110; r.opentime = 100000; r.position_ts = today;
        r.cpage = 1; r.npage = 100;
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,pages_start,pages_end,pages_moved,"
                    "firmware_open_time)"
                    " VALUES(110,100000,100000,0,1,1,1,100000)");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, today) == 0);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions"
                           " WHERE book_id=110") == 2);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=110;"
                "DELETE FROM books WHERE book_id=110");

    /* A long suspend advances BOOTTIME and wall time but contributes only the
     * small MONOTONIC awake tail; empty days are skipped. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 113; r.opentime = r.position_ts = 100000;
        r.cpage = 1; r.npage = 100;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, 100000) == 1);
        time_t far = (time_t)(100000 + 20LL * 366 * 86400);
        struct tm far_tm;
        assert(localtime_r(&far, &far_tm) != NULL);
        far_tm.tm_hour = far_tm.tm_min = far_tm.tm_sec = 0;
        far_tm.tm_isdst = -1;
        int64_t today = (int64_t)mktime(&far_tm);
        r.position_ts = today;
        r.cpage = 2;
        assert(raw_observe_clock(&t, &r, 10, today - 100000,
                                 today) == 2);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions"
                           " WHERE book_id=113") == 2);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=113") == 10);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=113;"
                "DELETE FROM books WHERE book_id=113");

    /* A forward RTC correction can arrive on an otherwise identical poll.
     * Keep that day anchor until the pending presence is made durable. */
    {
        pb_state r;
        memset(&r, 0, sizeof r);
        r.bookid = 114; r.opentime = r.position_ts = 100000;
        r.cpage = 1; r.npage = 100;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &r, 0, 100000) == 1);
        int64_t far = 100000 + 20LL * 366 * 86400;
        assert(raw_observe_clock(&t, &r, 10, 10, far) == 0);
        assert(t.wall_forward_jump);
        assert((tracker_flush)(&t, NULL, 10, 10, far + 1) == 0);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions"
                           " WHERE book_id=114") == 2);
        assert(q1(t.stats, "SELECT SUM(active_seconds) FROM sessions"
                           " WHERE book_id=114") == 10);
        assert(q1(t.stats, "SELECT MAX(start_time) FROM sessions"
                           " WHERE book_id=114") > far - 86400);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=114;"
                "DELETE FROM books WHERE book_id=114");

    /* The durable global timestamp floor survives a process restart, so a
     * reset RTC cannot put a brand-new firmware session before history. */
    {
        pb_state a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.bookid = 111; a.opentime = a.position_ts = 900000; a.cpage = 1;
        b.bookid = 112; b.opentime = b.position_ts = 800001; b.cpage = 1;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &a, 0, 900000) == 1);
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &b, 0, 800000) == 1);
        assert(q1(t.stats, "SELECT MIN(start_time) FROM sessions"
                           " WHERE book_id=112") >= 900000);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id IN (111,112);"
                "DELETE FROM books WHERE book_id IN (111,112)");

    /* Observe cannot adopt a fresh/mismatched session. */
    {
        pb_state a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.bookid = 94; a.opentime = a.position_ts = 4000; a.cpage = 1;
        b.bookid = 95; b.opentime = b.position_ts = 4001; b.cpage = 2;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &a, 0, 4000) == 1);
        tracker before = t;
        assert(raw_observe(&t, &b, 1, 4001) < 0);
        assert(memcmp(&before, &t, sizeof t) == 0);
        assert(q1(t.stats, "SELECT COUNT(*) FROM sessions"
                           " WHERE book_id=95") == 0);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id IN (94,95);"
                "DELETE FROM books WHERE book_id IN (94,95)");

    /* Identical one-second polls do not open transactions. Checkpoints follow
     * durable active gain, stop at the page cap, and a turn unlocks the full
     * still-anchored allowance in exactly one transaction. */
    {
        pb_state c;
        memset(&c, 0, sizeof c);
        c.bookid = 96; c.opentime = c.position_ts = 4000;
        c.cpage = 1; c.npage = 100;
        snprintf(c.title, sizeof c.title, "SparseWrites");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &c, 0, 4000) == 1);
        int savepoints = 0;
        sqlite3_trace_v2(t.stats, SQLITE_TRACE_STMT, trace_savepoints,
                         &savepoints);
        /* Changing stale position samples are still no reason to bypass the
         * checkpoint; only an accepted snapshot writes immediately. */
        for (int i = 1; i < 60; ++i) {
            c.position_ts = 3998 + (i & 1);
            assert(raw_observe(&t, &c, i, 4000 + i) == 0);
        }
        c.position_ts = 4000;
        assert(savepoints == 0);
        assert(!tracker_checkpoint_due(&t, 59));
        assert(tracker_checkpoint_due(&t, 60));
        assert(raw_flush(&t, NULL, 60, 4060) == 0);
        assert(savepoints == 1);
        assert(raw_flush(&t, NULL, 300, 4300) == 0);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=96") == 300);
        savepoints = 0;
        for (int i = 301; i <= 600; ++i)
            assert(raw_observe(&t, &c, i, 4000 + i) == 0);
        assert(savepoints == 0);
        assert(!tracker_checkpoint_due(&t, 600));
        assert(t.cur_last_present == 300);
        c.position_ts = 4600; c.cpage = 2;
        assert(raw_observe(&t, &c, 600, 4600) == 2);
        assert(savepoints == 1);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=96") == 600);
        sqlite3_trace_v2(t.stats, 0, NULL, NULL);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=96;"
                "DELETE FROM books WHERE book_id=96");

    /* Lossless flush expands the logical inclusive span when wall time cannot
     * represent measured time. Legacy labelled moved=0 rows regain budget. */
    {
        pb_state l;
        memset(&l, 0, sizeof l);
        l.bookid = 97; l.opentime = l.position_ts = 5000; l.cpage = 1;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &l, 0, 5000) == 1);
        assert(raw_flush(&t, NULL, 300, 5000) == 0);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=97") == 300);
        assert(q1(t.stats, "SELECT end_time FROM sessions"
                           " WHERE book_id=97") == 5299);

        l.bookid = 98; l.opentime = 6000; l.position_ts = 6299;
        l.cpage = 10;
        ex(t.stats, "INSERT INTO sessions(book_id,start_time,end_time,"
                    "active_seconds,pages_start,pages_end,pages_moved,"
                    "firmware_open_time) VALUES(98,6000,6299,300,10,10,0,6000)");
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &l, 0, 6299) == 0);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=98") == 2);
        assert(tracker_checkpoint_due(&t, 300));
        assert(raw_flush(&t, NULL, 300, 6599) == 0);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=98") == 600);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id IN (97,98);"
                "DELETE FROM books WHERE book_id IN (97,98)");

    /* A delayed book-switch sample is an endpoint, not the fragment start.
     * Applying it only after the pending credit preserves the day split. */
    {
        pb_state endpoint, baseline;
        memset(&endpoint, 0, sizeof endpoint);
        endpoint.bookid = 100;
        endpoint.opentime = 82798;
        endpoint.position_ts = 82802;
        endpoint.cpage = 10;
        endpoint.npage = 100;
        snprintf(endpoint.title, sizeof endpoint.title, "SwitchMidnight");
        baseline = endpoint;
        baseline.position_ts = baseline.opentime;
        tracker_close(&t);
        assert(tracker_init(&t, ST_DB, EXP_DB) == 0);
        assert(raw_prepare(&t, &baseline, 0, 82798) == 1);
        assert(raw_observe(&t, &endpoint, 4, 82802) == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=100 AND start_time=82798") == 2);
        assert(q1(t.stats, "SELECT active_seconds FROM sessions"
                           " WHERE book_id=100 AND start_time=82800") == 2);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=100;"
                "DELETE FROM books WHERE book_id=100");

    /* Pages/hour uses net forward progress over all measured reading time.
     * The unchanged page contributes time but no page to the numerator. */
    overall_stats o;
    int64_t book_secs = 0;
    double book_pages_per_min = 0;
    ex(t.stats, "DELETE FROM sessions;"
                "UPDATE books SET completed=1,npage=300 WHERE book_id=7;"
                "INSERT INTO sessions"
                " (book_id,start_time,end_time,active_seconds,pages_start,pages_end)"
                " VALUES (7,1,120,120,7,7),(7,121,300,180,9,10)");
    assert(stats_overall(t.stats, &o) == 0);
    assert(o.books_total == 1 && o.books_finished == 1);
    assert(o.pages_per_min > 0.199 && o.pages_per_min < 0.201);
    stats_book(t.stats, 7, &book_secs, &book_pages_per_min);
    assert(book_secs == 300);
    assert(book_pages_per_min > 0.199 && book_pages_per_min < 0.201);

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
