#include "tracker.h"
#define STR_(x) #x
#define STR(x) STR_(x)
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  book_id INTEGER NOT NULL,"
    "  start_time INTEGER NOT NULL,"   /* = opentime aus explorer-3 */
    "  end_time INTEGER NOT NULL,"     /* = letztes position_ts */
    "  active_seconds INTEGER NOT NULL DEFAULT 0,"
    "  pages_start INTEGER,"           /* NULL = unbekannt */
    "  pages_end INTEGER,"
    "  recovered INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (book_id, start_time));"
    "CREATE TABLE IF NOT EXISTS books ("
    "  book_id INTEGER PRIMARY KEY,"
    "  title TEXT, author TEXT, cover TEXT,"
    "  cpage INTEGER, npage INTEGER, completed INTEGER,"
    "  last_seen INTEGER);";

/* Retrofits rows written by older versions: recovered spans were capped far
 * too generously and carried a pages_start they had no measured time for.
 * Idempotent, runs on every daemon start. */
static const char *MIGRATE =
    "UPDATE sessions SET pages_start = NULL"
    " WHERE recovered = 1 AND pages_start IS NOT NULL;"
    "UPDATE sessions SET active_seconds = " STR(SESSION_CAP_SECONDS)
    " WHERE recovered = 1 AND active_seconds > " STR(SESSION_CAP_SECONDS) ";";

static int exec1(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql error: %s\n", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int tracker_init(tracker *t, const char *stats_path, const char *explorer_path)
{
    memset(t, 0, sizeof(*t));
    t->explorer_path = explorer_path;
    if (sqlite3_open(stats_path, &t->stats) != SQLITE_OK)
        return -1;
    sqlite3_busy_timeout(t->stats, 2000);
    if (exec1(t->stats, SCHEMA) != 0)
        return -1;
    return exec1(t->stats, MIGRATE);
}

void tracker_close(tracker *t)
{
    if (t->stats)
        sqlite3_close(t->stats);
    t->stats = NULL;
}

/* Always open the firmware DB briefly and read-only. */
static sqlite3 *open_explorer(const char *path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 1000);
    return db;
}

static const char *STATE_SQL =
    "SELECT s.bookid, s.opentime, s.position_ts,"
    "  IFNULL(s.cpage,0), IFNULL(s.npage,0), IFNULL(s.completed,0),"
    "  IFNULL(b.title,''), IFNULL(b.author,''),"
    "  IFNULL((SELECT f.storageid || lower(hex(f.fast_hash)) FROM files f"
    "          WHERE f.book_id = s.bookid ORDER BY f.storageid LIMIT 1), '')"
    " FROM books_settings s JOIN books_impl b ON b.id = s.bookid"
    " WHERE s.opentime > 0 AND s.position_ts > 0"
    " ORDER BY s.opentime DESC LIMIT 1";

static void fill_state(sqlite3_stmt *st, pb_state *out)
{
    memset(out, 0, sizeof(*out));
    out->bookid = sqlite3_column_int64(st, 0);
    out->opentime = sqlite3_column_int64(st, 1);
    out->position_ts = sqlite3_column_int64(st, 2);
    /* The firmware stamps opentime when the book opens but only refreshes
     * position_ts on a page turn, so between the two position_ts still holds
     * the *previous* session's value. Reading it raw ends a session before it
     * started and makes the first page turn look like a huge gap. Clamp once
     * here, where both readers pass through. */
    if (out->position_ts < out->opentime)
        out->position_ts = out->opentime;
    out->cpage = sqlite3_column_int(st, 3);
    out->npage = sqlite3_column_int(st, 4);
    out->completed = sqlite3_column_int(st, 5);
    snprintf(out->title, sizeof(out->title), "%s", sqlite3_column_text(st, 6));
    snprintf(out->author, sizeof(out->author), "%s", sqlite3_column_text(st, 7));
    snprintf(out->cover, sizeof(out->cover), "%s", sqlite3_column_text(st, 8));
}

int tracker_read_state(const char *explorer_path, pb_state *out)
{
    sqlite3 *db = open_explorer(explorer_path);
    if (!db)
        return -1;
    sqlite3_stmt *st = NULL;
    int rc = 1;
    if (sqlite3_prepare_v2(db, STATE_SQL, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            fill_state(st, out);
            rc = 0;
        }
    } else {
        rc = -1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rc;
}

static void upsert_book(tracker *t, const pb_state *s)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO books (book_id, title, author, cover, cpage, npage, completed, last_seen)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8)"
        " ON CONFLICT(book_id) DO UPDATE SET title=?2, author=?3,"
        /* Never trade a known cover key for an empty one: the key is derived
         * from the firmware's files row, which disappears when the book is
         * deleted, while the cached image itself stays. Overwriting here is
         * what made the picture of a finished-and-deleted book unreachable. */
        "  cover = CASE WHEN ?4 = '' THEN cover ELSE ?4 END,"
        "  cpage=?5, npage=?6, completed=?7, last_seen=?8";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_text(st, 2, s->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->author, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s->cover, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, s->cpage);
    sqlite3_bind_int(st, 6, s->npage);
    sqlite3_bind_int(st, 7, s->completed);
    sqlite3_bind_int64(st, 8, s->position_ts);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* pages_end of this book's last earlier session, -1 if none. */
static int prev_pages_end(tracker *t, int64_t bookid, int64_t before)
{
    sqlite3_stmt *st = NULL;
    int val = -1;
    const char *sql =
        "SELECT pages_end FROM sessions WHERE book_id=?1 AND start_time<?2"
        " AND pages_end IS NOT NULL ORDER BY start_time DESC LIMIT 1";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, bookid);
    sqlite3_bind_int64(st, 2, before);
    if (sqlite3_step(st) == SQLITE_ROW)
        val = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return val;
}

/* Local midnight (as epoch) of the day ts falls into. */
static int64_t local_day_start(int64_t ts)
{
    time_t tt = (time_t)ts;
    struct tm tm;
    localtime_r(&tt, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static void insert_session(tracker *t, const pb_state *s, int64_t start_time,
                           int64_t active, int pages_start_known, int recovered)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT OR IGNORE INTO sessions"
        " (book_id, start_time, end_time, active_seconds, pages_start, pages_end, recovered)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7)";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_int64(st, 2, start_time);
    sqlite3_bind_int64(st, 3, s->position_ts);
    sqlite3_bind_int64(st, 4, active);
    /* Recovered rows get no pages_start: the pages were read while nothing was
     * tracking, so the delta would not match the (capped) time. */
    int ps = recovered ? -1 : prev_pages_end(t, s->bookid, start_time);
    if (ps < 0 && pages_start_known)
        ps = s->cpage; /* session just started: current page = start page */
    if (ps >= 0)
        sqlite3_bind_int(st, 5, ps);
    else
        sqlite3_bind_null(st, 5);
    sqlite3_bind_int(st, 6, s->cpage);
    sqlite3_bind_int(st, 7, recovered);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Sets a session row's end and time. Deliberately SET, not +=: the firmware
 * only reports where a session began and when its last page turn was, so the
 * row is recomputed from those endpoints every time one of them moves. */
static void set_session(tracker *t, int64_t bookid, int64_t start_time,
                        int64_t end_time, int64_t active, int pages_end)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "UPDATE sessions SET end_time=?1, active_seconds=?2,"
        " pages_end=?3 WHERE book_id=?4 AND start_time=?5";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, end_time);
    sqlite3_bind_int64(st, 2, active);
    sqlite3_bind_int(st, 3, pages_end);
    sqlite3_bind_int64(st, 4, bookid);
    sqlite3_bind_int64(st, 5, start_time);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Backfilled sessions only: no daemon watched them, so the span between the
 * firmware's endpoints may be mostly standby. */
static int64_t bounded(int64_t seconds)
{
    if (seconds < 0)
        return 0;
    return seconds > SESSION_CAP_SECONDS ? SESSION_CAP_SECONDS : seconds;
}

/* What an observed row may claim: the time the daemon was demonstrably running
 * since the row began, but never more than the pages actually turned make
 * plausible. Presence alone would credit a book lying open on an awake device. */
static int64_t measured(int64_t present, int pages)
{
    if (present < 0)
        present = 0;
    if (pages < 0)
        pages = 0;
    const int64_t by_pages = (int64_t)pages * SECONDS_PER_PAGE_CAP;
    return present < by_pages ? present : by_pages;
}

int tracker_recover(tracker *t)
{
    sqlite3 *db = open_explorer(t->explorer_path);
    if (!db)
        return -1;
    const char *sql =
        "SELECT s.bookid, s.opentime, s.position_ts,"
        "  IFNULL(s.cpage,0), IFNULL(s.npage,0), IFNULL(s.completed,0),"
        "  IFNULL(b.title,''), IFNULL(b.author,''),"
        "  IFNULL((SELECT f.storageid || lower(hex(f.fast_hash)) FROM files f"
        "          WHERE f.book_id = s.bookid ORDER BY f.storageid LIMIT 1), '')"
        " FROM books_settings s JOIN books_impl b ON b.id = s.bookid"
        " WHERE s.opentime > 0 AND s.position_ts > 0";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        pb_state s;
        fill_state(st, &s);
        insert_session(t, &s, s.opentime, bounded(s.position_ts - s.opentime),
                       0, 1);
        upsert_book(t, &s);
        n++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/* Take over a row that already exists -- after a daemon restart the session it
 * belongs to may still be open. Returns the time it already carries, which
 * becomes the floor we build on instead of overwriting it with zero. Clearing
 * `recovered` is part of taking over: from here on the row is measured, not
 * estimated, and the derived metrics in stats_db.c only count measured rows. */
static int64_t adopt_row(tracker *t, int64_t bookid, int64_t start_time)
{
    sqlite3_stmt *st = NULL;
    int64_t active = 0;
    if (sqlite3_prepare_v2(t->stats,
                           "SELECT active_seconds FROM sessions"
                           " WHERE book_id=?1 AND start_time=?2",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bookid);
        sqlite3_bind_int64(st, 2, start_time);
        if (sqlite3_step(st) == SQLITE_ROW)
            active = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    st = NULL;
    if (sqlite3_prepare_v2(t->stats,
                           "UPDATE sessions SET recovered=0"
                           " WHERE book_id=?1 AND start_time=?2",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bookid);
        sqlite3_bind_int64(st, 2, start_time);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return active;
}

int tracker_observe(tracker *t, const pb_state *s, int64_t present)
{
    if (!s || s->bookid <= 0)
        return 0;

    const int fresh = (s->bookid != t->cur_book || s->opentime != t->cur_open);
    if (!fresh && s->position_ts <= t->cur_pos_ts)
        return 0;

    if (fresh) {
        insert_session(t, s, s->opentime, 0, 1, 0);
        t->cur_book = s->bookid;
        t->cur_open = s->opentime;
        t->cur_row_start = s->opentime;
        t->cur_row_base = adopt_row(t, s->bookid, s->opentime);
        t->cur_row_present = present;
        t->cur_row_moved = 0;
        t->cur_pages_last = s->cpage;
    }

    /* Distance, not difference: reading backwards to check something is still
     * reading, and a session that ends on an earlier page than it started must
     * not end up with a page budget of zero. */
    t->cur_row_moved += s->cpage > t->cur_pages_last
        ? s->cpage - t->cur_pages_last
        : t->cur_pages_last - s->cpage;
    t->cur_pages_last = s->cpage;

    /* Day stats group by date(end_time), so a session must not carry time
     * across a local midnight: close the row there and continue in a new one. */
    const int64_t midnight = local_day_start(s->position_ts);
    if (t->cur_row_start < midnight) {
        /* The counter says how much presence the row gained, not when within it,
         * so split it by the wall-clock share that fell before midnight. */
        const int64_t gained = present - t->cur_row_present;
        const int64_t span = s->position_ts - t->cur_row_start;
        const int64_t before =
            span > 0 ? gained * (midnight - t->cur_row_start) / span : 0;
        set_session(t, s->bookid, t->cur_row_start, midnight - 1,
                    t->cur_row_base + measured(before, t->cur_row_moved),
                    s->cpage);
        insert_session(t, s, midnight, 0, 0, 0);
        t->cur_row_start = midnight;
        t->cur_row_base = 0;
        t->cur_row_present = present - (gained - before);
        /* cur_row_moved deliberately not reset: the page budget is a safety
         * net, and the day after midnight has no movement of its own yet. */
    }

    set_session(t, s->bookid, t->cur_row_start, s->position_ts,
                t->cur_row_base
                    + measured(present - t->cur_row_present, t->cur_row_moved),
                s->cpage);
    upsert_book(t, s);
    t->cur_pos_ts = s->position_ts;
    return fresh ? 1 : 2;
}
