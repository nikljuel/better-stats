#include "tracker.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS sessions ("
    " book_id INTEGER NOT NULL,start_time INTEGER NOT NULL,"
    " end_time INTEGER NOT NULL,active_seconds INTEGER NOT NULL DEFAULT 0,"
    " pages_start INTEGER,pages_end INTEGER,"
    " pages_moved INTEGER NOT NULL DEFAULT 0,"
    " recovered INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(book_id,start_time));"
    "CREATE TABLE IF NOT EXISTS books ("
    " book_id INTEGER PRIMARY KEY,title TEXT,author TEXT,cover TEXT,"
    " cpage INTEGER,npage INTEGER,completed INTEGER,"
    " completed_ts INTEGER NOT NULL DEFAULT 0,last_seen INTEGER);"
    "CREATE TABLE IF NOT EXISTS sessions_archived ("
    " book_id INTEGER NOT NULL,start_time INTEGER NOT NULL,"
    " end_time INTEGER NOT NULL,active_seconds INTEGER NOT NULL DEFAULT 0,"
    " pages_start INTEGER,pages_end INTEGER,"
    " pages_moved INTEGER NOT NULL DEFAULT 0,"
    " recovered INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(book_id,start_time));";

static const char *MIGRATE =
    "INSERT OR IGNORE INTO sessions_archived"
    " (book_id,start_time,end_time,active_seconds,pages_start,pages_end,"
    " pages_moved,recovered)"
    " SELECT book_id,start_time,end_time,active_seconds,pages_start,pages_end,"
    " pages_moved,recovered FROM sessions WHERE recovered=1;"
    "DELETE FROM sessions WHERE recovered=1;";

static int exec1(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK)
        return 0;
    fprintf(stderr, "sql error: %s\n", err ? err : "?");
    sqlite3_free(err);
    return -1;
}

static int column_exists(sqlite3 *db, const char *table, const char *column)
{
    char sql[64];
    sqlite3_stmt *st = NULL;
    int found = 0, rc;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        if (!strcmp((const char *)sqlite3_column_text(st, 1), column))
            found = 1;
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? found : -1;
}

static int migrate_column(sqlite3 *db, const char *table, const char *column,
                          const char *definition)
{
    int found = column_exists(db, table, column);
    if (found != 0)
        return found < 0 ? -1 : 0;
    char sql[160];
    snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s",
             table, column, definition);
    return exec1(db, sql);
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
        || migrate_column(t->stats, "books", "completed_ts",
                          "INTEGER NOT NULL DEFAULT 0") != 0
        || migrate_column(t->stats, "sessions", "pages_moved",
                          "INTEGER NOT NULL DEFAULT 0") != 0
        || migrate_column(t->stats, "sessions_archived", "pages_moved",
                          "INTEGER NOT NULL DEFAULT 0") != 0
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
    "SELECT s.bookid,s.opentime,s.position_ts,"
    " IFNULL(s.cpage,0),IFNULL(s.npage,0),IFNULL(s.completed,0),"
    " IFNULL(s.completed_ts,0),IFNULL(b.title,''),IFNULL(b.author,''),"
    " IFNULL((SELECT f.storageid || lower(hex(f.fast_hash)) FROM files f"
    "         WHERE f.book_id=s.bookid ORDER BY f.storageid LIMIT 1),'')"
    " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
    " WHERE s.opentime>0 AND s.position_ts>0"
    " ORDER BY s.opentime DESC LIMIT 1";

static void fill_state(sqlite3_stmt *st, pb_state *out)
{
    memset(out, 0, sizeof(*out));
    out->bookid = sqlite3_column_int64(st, 0);
    out->opentime = sqlite3_column_int64(st, 1);
    out->position_ts = sqlite3_column_int64(st, 2);
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
    int result = -1;
    if (sqlite3_prepare_v2(db, STATE_SQL, -1, &st, NULL) == SQLITE_OK) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            fill_state(st, out);
            result = 0;
        } else if (rc == SQLITE_DONE) {
            result = 1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return result;
}

static int begin_op(sqlite3 *db)
{
    return exec1(db, "SAVEPOINT betterstats_tracker");
}

static int finish_op(sqlite3 *db, int ok)
{
    if (ok && exec1(db, "RELEASE betterstats_tracker") == 0)
        return 0;
    exec1(db, "ROLLBACK TO betterstats_tracker");
    exec1(db, "RELEASE betterstats_tracker");
    return -1;
}

static int upsert_book(tracker *t, const pb_state *s)
{
    static const char *sql =
        "INSERT INTO books"
        " (book_id,title,author,cover,cpage,npage,completed,completed_ts,last_seen)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
        " ON CONFLICT(book_id) DO UPDATE SET title=?2,author=?3,"
        " cover=CASE WHEN ?4='' THEN cover ELSE ?4 END,"
        " cpage=CASE WHEN last_seen IS NULL OR ?9>last_seen THEN ?5 ELSE cpage END,"
        " npage=?6,completed=?7,completed_ts=?8,"
        " last_seen=CASE WHEN last_seen IS NULL OR ?9>last_seen"
        "                THEN ?9 ELSE last_seen END";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_text(st, 2, s->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->author, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s->cover, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, s->cpage);
    sqlite3_bind_int(st, 6, s->npage);
    sqlite3_bind_int(st, 7, s->completed);
    sqlite3_bind_int64(st, 8, s->completed_ts);
    sqlite3_bind_int64(st, 9, s->position_ts);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

static int previous_page(tracker *t, int64_t bookid, int64_t before,
                         int *page, int *known)
{
    sqlite3_stmt *st = NULL;
    *known = 0;
    if (sqlite3_prepare_v2(t->stats,
            "SELECT pages_end FROM sessions WHERE book_id=?1 AND start_time<?2"
            " AND pages_end IS NOT NULL ORDER BY start_time DESC LIMIT 1",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, bookid);
    sqlite3_bind_int64(st, 2, before);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        *page = sqlite3_column_int(st, 0);
        *known = 1;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

/* 1 inserted, 0 identical row already present, -1 error/conflict. */
static int insert_session(tracker *t, int64_t bookid, int64_t start,
                          int64_t end, int page_start, int page_start_known,
                          int page_end, int moved)
{
    sqlite3_stmt *st = NULL;
    static const char *insert =
        "INSERT OR IGNORE INTO sessions"
        " (book_id,start_time,end_time,pages_start,pages_end,pages_moved)"
        " VALUES(?1,?2,?3,?4,?5,?6)";
    if (sqlite3_prepare_v2(t->stats, insert, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, bookid);
    sqlite3_bind_int64(st, 2, start);
    sqlite3_bind_int64(st, 3, end);
    if (page_start_known)
        sqlite3_bind_int(st, 4, page_start);
    else
        sqlite3_bind_null(st, 4);
    sqlite3_bind_int(st, 5, page_end);
    sqlite3_bind_int(st, 6, moved);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return -1;
    if (sqlite3_changes(t->stats) == 1)
        return 1;

    static const char *verify =
        "SELECT end_time,active_seconds,pages_start,pages_end,pages_moved"
        " FROM sessions WHERE book_id=?1 AND start_time=?2";
    if (sqlite3_prepare_v2(t->stats, verify, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, bookid);
    sqlite3_bind_int64(st, 2, start);
    int rc = sqlite3_step(st);
    int same = rc == SQLITE_ROW
        && sqlite3_column_int64(st, 0) == end
        && sqlite3_column_int64(st, 1) == 0
        && ((page_start_known && sqlite3_column_type(st, 2) != SQLITE_NULL
             && sqlite3_column_int(st, 2) == page_start)
            || (!page_start_known && sqlite3_column_type(st, 2) == SQLITE_NULL))
        && sqlite3_column_int(st, 3) == page_end
        && sqlite3_column_int(st, 4) == moved;
    sqlite3_finalize(st);
    return same ? 0 : -1;
}

static int set_session(tracker *t, int64_t end, int64_t active)
{
    sqlite3_stmt *st = NULL;
    static const char *sql =
        "UPDATE sessions SET end_time=MAX(end_time,?1),"
        " active_seconds=MAX(active_seconds,?2),pages_end=?3,"
        " pages_moved=MAX(pages_moved,?4)"
        " WHERE book_id=?5 AND start_time=?6";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, end);
    sqlite3_bind_int64(st, 2, active);
    sqlite3_bind_int(st, 3, t->cur_pages_last);
    sqlite3_bind_int(st, 4, t->cur_row_moved);
    sqlite3_bind_int64(st, 5, t->cur_book);
    sqlite3_bind_int64(st, 6, t->cur_row_start);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(t->stats) == 1;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

static int64_t measured(int64_t present, int pages, int64_t used)
{
    if (present < 0)
        present = 0;
    if (pages < 0)
        pages = 0;
    int64_t available = (int64_t)pages * SECONDS_PER_PAGE_CAP - used;
    if (available < 0)
        available = 0;
    return present < available ? present : available;
}

static int64_t row_active(const tracker *t, int64_t present, int64_t end)
{
    int64_t active = t->cur_row_base
        + measured(present - t->cur_row_present, t->cur_row_moved,
                   t->cur_budget_used);
    int64_t span = end >= t->cur_row_start ? end - t->cur_row_start + 1 : 0;
    if (active > span && span >= t->cur_row_base)
        active = span;
    return active;
}

static int64_t local_day_start(int64_t ts)
{
    time_t value = (time_t)ts;
    struct tm tm;
    localtime_r(&value, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static int64_t next_local_midnight(int64_t ts)
{
    time_t value = (time_t)ts;
    struct tm tm;
    localtime_r(&value, &tm);
    tm.tm_mday++;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static int write_current(tracker *t, int64_t present, int64_t end)
{
    if (end < t->cur_pos_ts)
        end = t->cur_pos_ts;
    if (end < t->cur_end_ts)
        end = t->cur_end_ts;
    if (end < t->cur_row_start)
        end = t->cur_row_start;

    while (t->cur_row_start < local_day_start(end)) {
        int64_t midnight = next_local_midnight(t->cur_row_start);
        int64_t gained = present - t->cur_row_present;
        if (gained < 0)
            gained = 0;
        int64_t span = end - t->cur_row_start;
        int64_t before = span > 0
            ? gained * (midnight - t->cur_row_start) / span : 0;
        int64_t credit = measured(gained, t->cur_row_moved,
                                  t->cur_budget_used);
        int64_t before_credit = gained > 0 ? credit * before / gained : 0;
        if (set_session(t, midnight - 1,
                        t->cur_row_base + before_credit) != 0)
            return -1;
        if (insert_session(t, t->cur_book, midnight, midnight,
                           t->cur_pages_last, 1, t->cur_pages_last,
                           t->cur_row_moved) < 0)
            return -1;
        t->cur_budget_used += before_credit;
        t->cur_row_start = midnight;
        t->cur_row_base = 0;
        t->cur_row_present += before;
        t->cur_end_ts = midnight;
    }

    if (set_session(t, end, row_active(t, present, end)) != 0)
        return -1;
    t->cur_end_ts = end;
    t->cur_last_present = present;
    return 0;
}

/* Explorer refresh is best-effort at runtime; Stats writes remain fatal. */
static int refresh_current(tracker *t)
{
    sqlite3 *db = open_explorer(t->explorer_path);
    if (!db)
        return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT position_ts,IFNULL(cpage,0) FROM books_settings"
            " WHERE bookid=?1 AND opentime=?2 ORDER BY profileid LIMIT 1",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, t->cur_book);
        sqlite3_bind_int64(st, 2, t->cur_open);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int64_t pos = sqlite3_column_int64(st, 0);
            int page = sqlite3_column_int(st, 1);
            if (pos > t->cur_pos_ts) {
                t->cur_row_moved += page > t->cur_pages_last
                    ? page - t->cur_pages_last : t->cur_pages_last - page;
                t->cur_pages_last = page;
                t->cur_pos_ts = pos;
                found = 1;
            }
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!found)
        return 0;

    sqlite3_stmt *update = NULL;
    if (sqlite3_prepare_v2(t->stats,
            "UPDATE books SET cpage=?1,last_seen=?2 WHERE book_id=?3",
            -1, &update, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(update, 1, t->cur_pages_last);
    sqlite3_bind_int64(update, 2, t->cur_pos_ts);
    sqlite3_bind_int64(update, 3, t->cur_book);
    int ok = sqlite3_step(update) == SQLITE_DONE;
    sqlite3_finalize(update);
    return ok ? 0 : -1;
}

static int flush_inner(tracker *t, int64_t present, int64_t wall_end)
{
    if (t->cur_book <= 0)
        return 0;
    if (refresh_current(t) != 0)
        return -1;
    int64_t pending = present - t->cur_last_present;
    if (pending < 0)
        pending = 0;
    int64_t base = t->cur_pos_ts > t->cur_end_ts
        ? t->cur_pos_ts : t->cur_end_ts;
    int64_t available = wall_end > base ? wall_end - base : 0;
    int64_t logical_end = base + (pending < available ? pending : available);
    if (write_current(t, present, logical_end) != 0)
        return -1;
    t->cur_resume_page_start = t->cur_pages_last;
    return 0;
}

static void clear_current(tracker *t)
{
    t->cur_book = t->cur_open = t->cur_pos_ts = t->cur_end_ts = 0;
    t->cur_row_base = t->cur_row_present = t->cur_last_present = 0;
    t->cur_budget_used = 0;
    t->cur_row_moved = t->cur_pages_last = t->cur_resume_page_start = 0;
    t->cur_row_start = 0;
}

static int load_prepared_session(tracker *t, const pb_state *s,
                                 int64_t present)
{
    sqlite3_stmt *st = NULL;
    static const char *sql =
        "SELECT start_time,end_time,active_seconds,pages_end,pages_moved"
        " FROM sessions WHERE book_id=?1 AND start_time>=?2 ORDER BY start_time";
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_int64(st, 2, s->opentime);

    int rows = 0, rc, latest_page = s->cpage, latest_page_known = 0;
    int moved = 1;
    int64_t total_active = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        rows++;
        t->cur_row_start = sqlite3_column_int64(st, 0);
        t->cur_end_ts = sqlite3_column_int64(st, 1);
        t->cur_row_base = sqlite3_column_int64(st, 2);
        total_active += t->cur_row_base;
        if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
            latest_page = sqlite3_column_int(st, 3);
            latest_page_known = 1;
        }
        int value = sqlite3_column_int(st, 4);
        if (value > moved)
            moved = value;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return -1;

    t->cur_book = s->bookid;
    t->cur_open = s->opentime;
    t->cur_pos_ts = s->opentime;
    t->cur_row_moved = moved;
    t->cur_pages_last = latest_page;
    t->cur_resume_page_start = latest_page;
    t->cur_row_present = present;
    t->cur_last_present = present;

    if (!rows) {
        int page = 0, known = 0;
        if (previous_page(t, s->bookid, s->opentime, &page, &known) != 0)
            return -1;
        if (!known) {
            page = s->cpage;
            known = 1;
        }
        if (insert_session(t, s->bookid, s->opentime, s->opentime,
                           page, known, s->cpage, 1) < 0)
            return -1;
        t->cur_row_start = t->cur_end_ts = s->opentime;
        t->cur_row_base = t->cur_budget_used = 0;
        t->cur_pages_last = s->cpage;
        t->cur_resume_page_start = s->cpage;
        return 0;
    }

    t->cur_budget_used = total_active;
    sqlite3_stmt *book = NULL;
    if (sqlite3_prepare_v2(t->stats,
            "SELECT cpage,last_seen FROM books WHERE book_id=?1",
            -1, &book, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(book, 1, s->bookid);
    rc = sqlite3_step(book);
    if (rc == SQLITE_ROW && sqlite3_column_type(book, 1) != SQLITE_NULL
        && sqlite3_column_int64(book, 1) >= s->opentime) {
        int page = sqlite3_column_int(book, 0);
        int64_t pos = sqlite3_column_int64(book, 1);
        if (latest_page_known && page != latest_page)
            t->cur_row_moved += page > latest_page
                ? page - latest_page : latest_page - page;
        t->cur_pages_last = page;
        t->cur_pos_ts = pos;
    }
    sqlite3_finalize(book);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

static int prepare_inner(tracker *t, const pb_state *s, int64_t present)
{
    if (!s || s->bookid <= 0)
        return -1;
    if (t->cur_book == s->bookid && t->cur_open == s->opentime)
        return 0;
    if (t->cur_book > 0 && flush_inner(t, present, s->opentime - 1) != 0)
        return -1;
    clear_current(t);
    return load_prepared_session(t, s, present);
}

int tracker_prepare(tracker *t, const pb_state *s, int64_t present)
{
    tracker proposed = *t;
    if (begin_op(t->stats) != 0)
        return -1;
    int rc = prepare_inner(&proposed, s, present);
    if (finish_op(t->stats, rc == 0) != 0)
        return -1;
    *t = proposed;
    return 0;
}

static int resume_inner(tracker *t, int64_t present, int64_t wall)
{
    if (t->cur_book <= 0)
        return -1;
    if (wall == t->cur_row_start) {
        if (set_session(t, t->cur_end_ts, t->cur_row_base) != 0)
            return -1;
        t->cur_row_present = present;
        t->cur_last_present = present;
        t->cur_resume_page_start = t->cur_pages_last;
        return 0;
    }
    if (wall < t->cur_row_start)
        return -1;
    int64_t durable = row_active(t, t->cur_last_present, t->cur_end_ts);
    int64_t gained = durable - t->cur_row_base;
    if (gained > 0)
        t->cur_budget_used += gained;
    if (insert_session(t, t->cur_book, wall, wall,
                       t->cur_resume_page_start, 1,
                       t->cur_pages_last, t->cur_row_moved) != 1)
        return -1;
    t->cur_row_start = t->cur_end_ts = wall;
    t->cur_row_base = 0;
    t->cur_row_present = t->cur_last_present = present;
    t->cur_resume_page_start = t->cur_pages_last;
    return 0;
}

int tracker_resume(tracker *t, int64_t present, int64_t wall_time)
{
    tracker proposed = *t;
    if (begin_op(t->stats) != 0)
        return -1;
    int rc = resume_inner(&proposed, present, wall_time);
    if (finish_op(t->stats, rc == 0) != 0)
        return -1;
    *t = proposed;
    return 0;
}

static int observe_inner(tracker *t, const pb_state *s, int64_t present,
                         int *fresh_out)
{
    if (!s || s->bookid <= 0)
        return 0;
    int fresh = s->bookid != t->cur_book || s->opentime != t->cur_open;
    if (fresh && prepare_inner(t, s, present) != 0)
        return -1;
    if (upsert_book(t, s) != 0)
        return -1;
    if (s->position_ts <= t->cur_pos_ts) {
        *fresh_out = fresh;
        return 0;
    }
    t->cur_row_moved += s->cpage > t->cur_pages_last
        ? s->cpage - t->cur_pages_last : t->cur_pages_last - s->cpage;
    t->cur_pages_last = s->cpage;
    t->cur_pos_ts = s->position_ts;
    if (write_current(t, present, s->position_ts) != 0)
        return -1;
    *fresh_out = fresh;
    return 1;
}

int tracker_observe(tracker *t, const pb_state *s, int64_t present)
{
    tracker proposed = *t;
    if (begin_op(t->stats) != 0)
        return -1;
    int fresh = 0;
    int changed = observe_inner(&proposed, s, present, &fresh);
    if (finish_op(t->stats, changed >= 0) != 0)
        return -1;
    *t = proposed;
    return fresh ? 1 : changed ? 2 : 0;
}

int tracker_flush(tracker *t, int64_t present, int64_t end_time)
{
    tracker proposed = *t;
    if (begin_op(t->stats) != 0)
        return -1;
    int rc = flush_inner(&proposed, present, end_time);
    if (finish_op(t->stats, rc == 0) != 0)
        return -1;
    *t = proposed;
    return 0;
}

int tracker_recover(tracker *t, int64_t skip_bookid, int64_t skip_opentime)
{
    sqlite3 *db = open_explorer(t->explorer_path);
    if (!db)
        return -1;
    static const char *sql =
        "SELECT s.bookid,s.opentime,s.position_ts,"
        " IFNULL(s.cpage,0),IFNULL(s.npage,0),IFNULL(s.completed,0),"
        " IFNULL(s.completed_ts,0),IFNULL(b.title,''),IFNULL(b.author,''),"
        " IFNULL((SELECT f.storageid || lower(hex(f.fast_hash)) FROM files f"
        "         WHERE f.book_id=s.bookid ORDER BY f.storageid LIMIT 1),'')"
        " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
        " WHERE (s.opentime>0 AND s.position_ts>0)"
        "    OR (s.completed=1 AND s.completed_ts>0)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    if (begin_op(t->stats) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    int n = 0, ok = 1, rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        pb_state s;
        fill_state(st, &s);
        if (s.bookid == skip_bookid && s.opentime == skip_opentime)
            continue;
        if (upsert_book(t, &s) != 0) {
            ok = 0;
            break;
        }
        n++;
    }
    if (rc != SQLITE_DONE)
        ok = 0;
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (finish_op(t->stats, ok) != 0)
        return -1;
    return n;
}
