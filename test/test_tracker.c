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
           " opentime INTEGER, completed INTEGER, completed_ts INTEGER);");
    ex(db, "INSERT INTO books_impl VALUES (7,'Testbuch','Autorin');"
           "INSERT INTO files VALUES (7,1,x'aabb');"
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

int main(void)
{
    /* Fixed zone: the day-boundary logic is local-time based. */
    setenv("TZ", "Europe/Berlin", 1);
    tzset();

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
    assert(q1(t.stats, "SELECT completed_ts FROM books WHERE book_id=99") == 0);
    assert(q1(t.stats, "SELECT COUNT(*) FROM pragma_table_info('sessions')"
                       " WHERE name='pages_moved'") == 1);
    assert(q1(t.stats, "SELECT COUNT(*)"
                       " FROM pragma_table_info('sessions_archived')"
                       " WHERE name='pages_moved'") == 1);
    assert(q1(t.stats, "SELECT recovered FROM sessions_archived"
                       " WHERE book_id=98") == 1);
    assert(q1(t.stats, "SELECT pages_moved FROM sessions_archived"
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

    /* New session observed: a row starts at zero, pages_start = cpage */
    assert(tracker_observe(&t, &s, 0) == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 1);
    assert(q1(t.stats, "SELECT active_seconds FROM sessions") == 0);
    assert(q1(t.stats, "SELECT pages_start FROM sessions") == 10);

    /* Finishing or unfinishing does not require a page turn, but both changes
     * must still reach the durable book row. */
    ex(exp, "UPDATE books_settings SET completed=1,completed_ts=12345"
            " WHERE bookid=7");
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 0);
    assert(q1(t.stats, "SELECT completed_ts FROM books WHERE book_id=7") == 12345);
    ex(exp, "UPDATE books_settings SET completed=0,completed_ts=0 WHERE bookid=7");
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 0) == 0);
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

    /* New opentime = new session; pages_start = pages_end of the old one */
    set_state(exp, 90000, 90005, 402);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(tracker_observe(&t, &s, 5) == 1);
    assert(q1(t.stats, "SELECT COUNT(*) FROM sessions") == 2);
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=90000") == 400);
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

    /* Stale position_ts live: the daemon starts mid-session, then the first
     * page turn must count only the time since opentime -- not the whole bogus
     * gap back to the stale position_ts. */
    set_state(exp, 200000, 199000, 80);
    assert(tracker_read_state(EXP_DB, &s) == 0);
    assert(s.position_ts == 200000); /* clamped on read */
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
    /* the new row continues the page count instead of restarting it */
    assert(q1(t.stats, "SELECT pages_start FROM sessions WHERE start_time=687600") == 22);

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
    t.cur_book = 0; /* the probe book is gone; don't resume it */

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
    t.cur_book = 0;

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
    t.cur_book = 0;

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
    t.cur_book = 0;

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
    t.cur_book = 0;

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
    t.cur_book = 0;

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
    t.cur_book = 0;

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
    t.cur_book = 0;

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
        assert(memcmp(&t, &before, sizeof(t)) == 0);
        assert(q1(t.stats, "SELECT MAX(end_time) FROM sessions"
                           " WHERE book_id=80") == old_end);
        ex(t.stats, "DROP TRIGGER fail_session_update");
        assert(tracker_observe(&t, &p, 180) == 2);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=80;"
                "DELETE FROM books WHERE book_id=80");
    t.cur_book = 0;

    /* A page change durable in books but not yet in the last fragment is
     * charged once and shown as the next fragment's start-to-end change. */
    {
        pb_state p;
        memset(&p, 0, sizeof p);
        p.bookid = 81; p.opentime = 1500000; p.position_ts = 1500060;
        p.cpage = 12; p.npage = 100;
        ex(t.stats,
            "INSERT INTO books(book_id,title,cpage,npage,last_seen)"
            " VALUES(81,'Catchup',12,100,1500060);"
            "INSERT INTO sessions(book_id,start_time,end_time,active_seconds,"
            " pages_start,pages_end,pages_moved)"
            " VALUES(81,1500000,1500030,30,10,10,1)");
        assert(tracker_prepare(&t, &p, 30) == 0);
        assert(tracker_resume(&t, 30, 1500100) == 0);
        assert(q1(t.stats, "SELECT pages_start FROM sessions"
                           " WHERE book_id=81 AND start_time=1500100") == 10);
        assert(q1(t.stats, "SELECT pages_end FROM sessions"
                           " WHERE book_id=81 AND start_time=1500100") == 12);
        assert(q1(t.stats, "SELECT pages_moved FROM sessions"
                           " WHERE book_id=81 AND start_time=1500100") == 3);
    }
    ex(t.stats, "DELETE FROM sessions WHERE book_id=81;"
                "DELETE FROM books WHERE book_id=81");
    t.cur_book = 0;

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
