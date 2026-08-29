#include "tracker.h"
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
    "  completed_ts INTEGER NOT NULL DEFAULT 0,"
    "  last_seen INTEGER);"
    "CREATE TABLE IF NOT EXISTS sessions_archived ("
    "  book_id INTEGER NOT NULL,"
    "  start_time INTEGER NOT NULL,"
    "  end_time INTEGER NOT NULL,"
    "  active_seconds INTEGER NOT NULL DEFAULT 0,"
    "  pages_start INTEGER,"
    "  pages_end INTEGER,"
    "  recovered INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (book_id, start_time));";

/* Moves estimated sessions into an archive so they no longer pollute stats.
 * Idempotent, runs on every daemon start. */
static const char *MIGRATE =
    "INSERT OR IGNORE INTO sessions_archived"
    " SELECT * FROM sessions WHERE recovered = 1;"
    "DELETE FROM sessions WHERE recovered = 1;";

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

static int migrate_completed_ts(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(books)", -1, &st, NULL)
        != SQLITE_OK)
        return -1;
    while (sqlite3_step(st) == SQLITE_ROW)
        if (!strcmp((const char *)sqlite3_column_text(st, 1), "completed_ts"))
            found = 1;
    sqlite3_finalize(st);
    return found ? 0 : exec1(db,
        "ALTER TABLE books ADD COLUMN completed_ts INTEGER NOT NULL DEFAULT 0");
}

int tracker_init(tracker *t, const char *stats_path, const char *explorer_path)
{
    memset(t, 0, sizeof(*t));
    t->explorer_path = explorer_path;
    if (sqlite3_open(stats_path, &t->stats) != SQLITE_OK) {
        tracker_close(t);
        return -1;
    }
    sqlite3_busy_timeout(t->stats, 2000);
    if (exec1(t->stats, "BEGIN IMMEDIATE") != 0
        || exec1(t->stats, SCHEMA) != 0
        || migrate_completed_ts(t->stats) != 0
        || exec1(t->stats, MIGRATE) != 0
        || exec1(t->stats, "COMMIT") != 0) {
        sqlite3_exec(t->stats, "ROLLBACK", NULL, NULL, NULL);
        tracker_close(t);
        return -1;
    }
    return 0;
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
    "  IFNULL(s.completed_ts,0),"
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
    out->completed_ts = sqlite3_column_int64(st, 6);
    snprintf(out->title, sizeof(out->title), "%s", sqlite3_column_text(st, 7));
    snprintf(out->author, sizeof(out->author), "%s", sqlite3_column_text(st, 8));
    snprintf(out->cover, sizeof(out->cover), "%s", sqlite3_column_text(st, 9));
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
        "INSERT INTO books"
        " (book_id,title,author,cover,cpage,npage,completed,completed_ts,last_seen)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)"
        " ON CONFLICT(book_id) DO UPDATE SET title=?2, author=?3,"
        /* Never trade a known cover key for an empty one: the key is derived
         * from the firmware's files row, which disappears when the book is
         * deleted, while the cached image itself stays. Overwriting here is
         * what made the picture of a finished-and-deleted book unreachable. */
        "  cover = CASE WHEN ?4 = '' THEN cover ELSE ?4 END,"
        "  cpage=?5, npage=?6, completed=?7, completed_ts=?8, last_seen=?9";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_text(st, 2, s->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->author, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s->cover, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, s->cpage);
    sqlite3_bind_int(st, 6, s->npage);
    sqlite3_bind_int(st, 7, s->completed);
    sqlite3_bind_int64(st, 8, s->completed_ts);
    sqlite3_bind_int64(st, 9, s->position_ts);
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

static void insert_session(tracker *t, int64_t bookid, int64_t start_time,
                           int64_t end_time, int cpage, int pages_start_known)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT OR IGNORE INTO sessions"
        " (book_id, start_time, end_time, pages_start, pages_end)"
        " VALUES (?1,?2,?3,?4,?5)";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, bookid);
    sqlite3_bind_int64(st, 2, start_time);
    sqlite3_bind_int64(st, 3, end_time);
    int ps = prev_pages_end(t, bookid, start_time);
    if (ps < 0 && pages_start_known)
        ps = cpage; /* session just started: current page = start page */
    if (ps >= 0)
        sqlite3_bind_int(st, 4, ps);
    else
        sqlite3_bind_null(st, 4);
    sqlite3_bind_int(st, 5, cpage);
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

/* What an observed row may claim: the time the daemon was demonstrably running
 * since the row began, but never more than the pages actually turned make
 * plausible. Presence alone would credit a book lying open on an awake device. */
static int64_t measured(int64_t present, int pages, int64_t used)
{
    if (present < 0)
        present = 0;
    if (pages < 0)
        pages = 0;
    int64_t by_pages = (int64_t)pages * SECONDS_PER_PAGE_CAP - used;
    if (by_pages < 0)
        by_pages = 0;
    return present < by_pages ? present : by_pages;
}

/* Pick up the final firmware endpoint when a book switch raced past our last
 * observation. Net page progress only needs the first and final cpage. */
static void refresh_current(tracker *t)
{
    sqlite3 *db = open_explorer(t->explorer_path);
    if (!db)
        return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT position_ts,IFNULL(cpage,0)"
                           " FROM books_settings WHERE bookid=?1 AND opentime=?2"
                           " ORDER BY profileid LIMIT 1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, t->cur_book);
        sqlite3_bind_int64(st, 2, t->cur_open);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const int64_t position_ts = sqlite3_column_int64(st, 0);
            const int cpage = sqlite3_column_int(st, 1);
            t->cur_row_moved += cpage > t->cur_pages_last
                ? cpage - t->cur_pages_last
                : t->cur_pages_last - cpage;
            t->cur_pages_last = cpage;
            sqlite3_stmt *update = NULL;
            if (sqlite3_prepare_v2(t->stats,
                                   "UPDATE books SET cpage=?1,last_seen=?2"
                                   " WHERE book_id=?3",
                                   -1, &update, NULL) == SQLITE_OK) {
                sqlite3_bind_int(update, 1, cpage);
                sqlite3_bind_int64(update, 2, position_ts);
                sqlite3_bind_int64(update, 3, t->cur_book);
                sqlite3_step(update);
            }
            sqlite3_finalize(update);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static int64_t row_active(tracker *t, int64_t present, int64_t end_time)
{
    int64_t active = t->cur_row_base
        + measured(present - t->cur_row_present, t->cur_row_moved,
                   t->cur_budget_used);
    const int64_t span = end_time >= t->cur_row_start
        ? end_time - t->cur_row_start + 1 : 0;
    if (active > span && span >= t->cur_row_base)
        active = span;
    return active;
}

/* Persist the current row at a real boundary, splitting it if that boundary
 * crossed local midnight without another firmware position update. */
static void write_current(tracker *t, int64_t present, int64_t end_time)
{
    if (end_time < t->cur_pos_ts)
        end_time = t->cur_pos_ts;
    if (end_time < t->cur_row_start)
        end_time = t->cur_row_start;

    const int64_t midnight = local_day_start(end_time);
    if (t->cur_row_start < midnight) {
        int64_t gained = present - t->cur_row_present;
        if (gained < 0)
            gained = 0;
        const int64_t span = end_time - t->cur_row_start;
        const int64_t before = span > 0
            ? gained * (midnight - t->cur_row_start) / span : 0;
        const int64_t credit = measured(gained, t->cur_row_moved,
                                        t->cur_budget_used);
        const int64_t before_credit = gained > 0
            ? credit * before / gained : 0;
        set_session(t, t->cur_book, t->cur_row_start, midnight - 1,
                    t->cur_row_base + before_credit,
                    t->cur_pages_last);
        insert_session(t, t->cur_book, midnight, end_time,
                       t->cur_pages_last, 0);
        t->cur_row_start = midnight;
        t->cur_row_base = 0;
        t->cur_row_present = present - (gained - before);
        t->cur_budget_used += before_credit;
    }

    set_session(t, t->cur_book, t->cur_row_start, end_time,
                row_active(t, present, end_time), t->cur_pages_last);
    t->cur_last_present = present;
}

void tracker_flush(tracker *t, int64_t present, int64_t end_time)
{
    if (t->cur_book <= 0)
        return;
    const int64_t pending = present - t->cur_last_present;
    int64_t logical_end = t->cur_pos_ts;
    if (pending > 0 && end_time > logical_end) {
        logical_end += pending;
        if (logical_end > end_time)
            logical_end = end_time;
    }
    refresh_current(t);
    write_current(t, present, logical_end);
}

int tracker_recover(tracker *t)
{
    sqlite3 *db = open_explorer(t->explorer_path);
    if (!db)
        return -1;
    const char *sql =
        "SELECT s.bookid, s.opentime, s.position_ts,"
        "  IFNULL(s.cpage,0), IFNULL(s.npage,0), IFNULL(s.completed,0),"
        "  IFNULL(s.completed_ts,0),"
        "  IFNULL(b.title,''), IFNULL(b.author,''),"
        "  IFNULL((SELECT f.storageid || lower(hex(f.fast_hash)) FROM files f"
        "          WHERE f.book_id = s.bookid ORDER BY f.storageid LIMIT 1), '')"
        " FROM books_settings s JOIN books_impl b ON b.id = s.bookid"
        " WHERE (s.opentime > 0 AND s.position_ts > 0)"
        "    OR (s.completed = 1 AND s.completed_ts > 0)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        pb_state s;
        fill_state(st, &s);
        upsert_book(t, &s);
        n++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/* Take over a row that already exists after a daemon restart. Its persisted
 * time becomes the floor we build on instead of overwriting it with zero. */
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
    return active;
}

int tracker_observe(tracker *t, const pb_state *s, int64_t present)
{
    if (!s || s->bookid <= 0)
        return 0;

    /* Marking a book finished does not necessarily turn a page. Persist its
     * metadata before the position-based early return. */
    upsert_book(t, s);

    const int fresh = (s->bookid != t->cur_book || s->opentime != t->cur_open);
    if (!fresh && s->position_ts <= t->cur_pos_ts)
        return 0;

    if (fresh) {
        tracker_flush(t, present, s->opentime - 1);
        insert_session(t, s->bookid, s->opentime, s->position_ts, s->cpage, 1);
        t->cur_book = s->bookid;
        t->cur_open = s->opentime;
        t->cur_row_start = s->opentime;
        t->cur_row_base = adopt_row(t, s->bookid, s->opentime);
        t->cur_row_present = present;
        t->cur_last_present = present;
        t->cur_budget_used = 0;
        t->cur_row_moved = 1; /* the page being read counts */
        t->cur_pages_last = s->cpage;
        t->cur_pos_ts = s->position_ts;
    }

    /* Distance, not difference: reading backwards to check something is still
     * reading, and a session that ends on an earlier page than it started must
     * not end up with a page budget of zero. */
    t->cur_row_moved += s->cpage > t->cur_pages_last
        ? s->cpage - t->cur_pages_last
        : t->cur_pages_last - s->cpage;
    t->cur_pages_last = s->cpage;

    write_current(t, present, s->position_ts);
    t->cur_pos_ts = s->position_ts;
    return fresh ? 1 : 2;
}
