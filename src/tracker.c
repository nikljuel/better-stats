#include "tracker.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define OPEN_GRACE_SECONDS 5
#define RTC_TOLERANCE_SECONDS 2
#define SQLITE_WAIT_MS 250

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS sessions ("
    " book_id INTEGER NOT NULL,start_time INTEGER NOT NULL,"
    " end_time INTEGER NOT NULL,active_seconds INTEGER NOT NULL DEFAULT 0,"
    " pages_start INTEGER,pages_end INTEGER,"
    " pages_moved INTEGER NOT NULL DEFAULT 0,"
    " recovered INTEGER NOT NULL DEFAULT 0,"
    " firmware_open_time INTEGER NOT NULL DEFAULT 0,"
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
    " firmware_open_time INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(book_id,start_time));";

static const char *MIGRATE =
    "INSERT OR IGNORE INTO sessions_archived"
    " (book_id,start_time,end_time,active_seconds,pages_start,pages_end,"
    " pages_moved,recovered,firmware_open_time)"
    " SELECT book_id,start_time,end_time,active_seconds,pages_start,pages_end,"
    " pages_moved,recovered,firmware_open_time"
    " FROM sessions WHERE recovered=1;"
    "DELETE FROM sessions WHERE recovered=1;";

static int sqlite_error_retryable(int error)
{
    int code = error & 0xff;
    return code == SQLITE_BUSY || code == SQLITE_LOCKED
        || code == SQLITE_IOERR || code == SQLITE_INTERRUPT
        || code == SQLITE_CANTOPEN;
}

#define STATE_COLUMNS \
    "s.bookid,s.opentime,s.position_ts," \
    "IFNULL(s.cpage,0),IFNULL(s.npage,0),IFNULL(s.completed,0)," \
    "IFNULL(s.completed_ts,0),IFNULL(b.title,''),IFNULL(b.author,'')," \
    "IFNULL((SELECT f.storageid || lower(hex(f.fast_hash)) FROM files f " \
    "WHERE f.book_id=s.bookid ORDER BY f.storageid LIMIT 1),'')"

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
    char sql[192];
    snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s",
             table, column, definition);
    return exec1(db, sql);
}

static int load_rtc_floor(tracker *t)
{
    static const char *sql =
        "SELECT IFNULL(MAX(value),0) FROM ("
        " SELECT MAX(MAX(start_time,end_time)) AS value FROM sessions"
        " UNION ALL SELECT MAX(MAX(start_time,end_time))"
        " FROM sessions_archived)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int64_t floor = sqlite3_column_int64(st, 0);
        if (floor > 0)
            t->rtc_floor = floor;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

int tracker_init(tracker *t, const char *stats_path, const char *explorer_path)
{
    memset(t, 0, sizeof(*t));
    t->explorer_path = explorer_path;
    if (sqlite3_open(stats_path, &t->stats) != SQLITE_OK) {
        int error = t->stats ? sqlite3_extended_errcode(t->stats)
            : SQLITE_CANTOPEN;
        tracker_close(t);
        return sqlite_error_retryable(error) ? TRACKER_RETRY : -1;
    }
    sqlite3_busy_timeout(t->stats, SQLITE_WAIT_MS);
    if (exec1(t->stats, "BEGIN IMMEDIATE") != 0
        || exec1(t->stats, SCHEMA) != 0
        || migrate_column(t->stats, "books", "completed_ts",
                          "INTEGER NOT NULL DEFAULT 0") != 0
        || migrate_column(t->stats, "sessions", "pages_moved",
                          "INTEGER NOT NULL DEFAULT 0") != 0
        || migrate_column(t->stats, "sessions_archived", "pages_moved",
                          "INTEGER NOT NULL DEFAULT 0") != 0
        || migrate_column(t->stats, "sessions", "firmware_open_time",
                          "INTEGER NOT NULL DEFAULT 0") != 0
        || migrate_column(t->stats, "sessions_archived", "firmware_open_time",
                          "INTEGER NOT NULL DEFAULT 0") != 0
        || exec1(t->stats, MIGRATE) != 0
        || load_rtc_floor(t) != 0
        || exec1(t->stats, "COMMIT") != 0) {
        int error = sqlite3_extended_errcode(t->stats);
        sqlite3_exec(t->stats, "ROLLBACK", NULL, NULL, NULL);
        tracker_close(t);
        return sqlite_error_retryable(error) ? TRACKER_RETRY : -1;
    }
    return 0;
}

void tracker_close(tracker *t)
{
    if (t->explorer)
        sqlite3_close(t->explorer);
    t->explorer = NULL;
    t->cached_book_path_valid = 0;
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
    sqlite3_busy_timeout(db, SQLITE_WAIT_MS);
    return db;
}

static void close_cached_explorer(tracker *t)
{
    if (t->explorer)
        sqlite3_close(t->explorer);
    t->explorer = NULL;
    t->explorer_dev = t->explorer_ino = 0;
    t->cached_book_path_valid = 0;
}

void tracker_invalidate_book_path_cache(tracker *t)
{
    if (t)
        t->cached_book_path_valid = 0;
}

static sqlite3 *cached_explorer(tracker *t)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        struct stat before, after;
        if (stat(t->explorer_path, &before) != 0) {
            close_cached_explorer(t);
            return NULL;
        }
        uint64_t dev = (uint64_t)before.st_dev;
        uint64_t ino = (uint64_t)before.st_ino;
        if (t->explorer && t->explorer_dev == dev
            && t->explorer_ino == ino)
            return t->explorer;
        close_cached_explorer(t);
        t->explorer = open_explorer(t->explorer_path);
        if (!t->explorer)
            return NULL;
        if (stat(t->explorer_path, &after) == 0
            && before.st_dev == after.st_dev
            && before.st_ino == after.st_ino) {
            t->explorer_dev = dev;
            t->explorer_ino = ino;
            return t->explorer;
        }
        close_cached_explorer(t);
    }
    return NULL;
}

static void copy_text(char *dst, size_t size, sqlite3_stmt *st, int column)
{
    const unsigned char *value = sqlite3_column_text(st, column);
    snprintf(dst, size, "%s", value ? (const char *)value : "");
}

static void fill_state(sqlite3_stmt *st, pb_state *out)
{
    memset(out, 0, sizeof(*out));
    out->bookid = sqlite3_column_int64(st, 0);
    out->opentime = sqlite3_column_int64(st, 1);
    out->position_ts = sqlite3_column_int64(st, 2);
    if (out->position_ts == 0)
        out->position_ts = out->opentime;
    out->cpage = sqlite3_column_int(st, 3);
    out->npage = sqlite3_column_int(st, 4);
    out->completed = sqlite3_column_int(st, 5);
    out->completed_ts = sqlite3_column_int64(st, 6);
    copy_text(out->title, sizeof(out->title), st, 7);
    copy_text(out->author, sizeof(out->author), st, 8);
    copy_text(out->cover, sizeof(out->cover), st, 9);
}

int tracker_read_state(const char *explorer_path, pb_state *out)
{
    static const char *sql =
        "SELECT " STATE_COLUMNS
        " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
        " WHERE s.opentime>0 ORDER BY s.opentime DESC LIMIT 1";
    sqlite3 *db = open_explorer(explorer_path);
    if (!db)
        return -1;
    sqlite3_stmt *st = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
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

static int read_candidates(sqlite3 *db, const char *sql, int64_t bookid,
                           int64_t a, int64_t b, pb_state *out)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, bookid);
    if (sqlite3_bind_parameter_count(st) >= 2)
        sqlite3_bind_int64(st, 2, a);
    if (sqlite3_bind_parameter_count(st) >= 3)
        sqlite3_bind_int64(st, 3, b);
    int count = 0, rc = SQLITE_DONE;
    pb_state candidate;
    while (count < 2 && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (count == 0)
            fill_state(st, &candidate);
        count++;
    }
    if (count < 2 && rc != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (count == 1)
        *out = candidate;
    return count;
}

static int read_book_state(sqlite3 *db, int64_t bookid,
                           int64_t expected_open, int64_t marker_started,
                           pb_state *out)
{
    static const char *exact_sql =
        "SELECT " STATE_COLUMNS
        " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
        " WHERE s.bookid=?1 AND s.opentime=?2 AND s.opentime>0 LIMIT 2";
    static const char *range_sql =
        "SELECT " STATE_COLUMNS
        " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
        " WHERE s.bookid=?1 AND s.opentime BETWEEN ?2 AND ?3"
        " AND s.opentime>0 LIMIT 2";
    static const char *single_sql =
        "SELECT " STATE_COLUMNS
        " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
        " WHERE s.bookid=?1 AND s.opentime>0 LIMIT 2";
    if (!out || bookid <= 0)
        return -1;
    int count;
    if (expected_open > 0) {
        count = read_candidates(db, exact_sql, bookid, expected_open, 0, out);
        if (count < 0)
            return -1;
        return count == 1 ? 0 : 1;
    }
    if (marker_started > 0) {
        int64_t low = marker_started < INT64_MIN + OPEN_GRACE_SECONDS
            ? INT64_MIN : marker_started - OPEN_GRACE_SECONDS;
        int64_t high = marker_started > INT64_MAX - OPEN_GRACE_SECONDS
            ? INT64_MAX : marker_started + OPEN_GRACE_SECONDS;
        count = read_candidates(db, range_sql, bookid, low, high, out);
        if (count < 0 || count > 1)
            return count < 0 ? -1 : 1;
        if (count == 1)
            return 0;
    }
    count = read_candidates(db, single_sql, bookid, 0, 0, out);
    if (count < 0)
        return -1;
    return count == 1 ? 0 : 1;
}

int tracker_read_book_state(const char *explorer_path, int64_t bookid,
                            int64_t expected_open, int64_t marker_started,
                            pb_state *out)
{
    if (!out || bookid <= 0)
        return -1;
    sqlite3 *db = open_explorer(explorer_path);
    if (!db)
        return -1;
    int result = read_book_state(db, bookid, expected_open, marker_started,
                                 out);
    sqlite3_close(db);
    return result;
}

int tracker_cached_read_book_state(tracker *t, int64_t bookid,
                                   int64_t expected_open,
                                   int64_t marker_started, pb_state *out)
{
    if (!t || !out || bookid <= 0)
        return -1;
    sqlite3 *db = cached_explorer(t);
    if (!db)
        return -1;
    int result = read_book_state(db, bookid, expected_open, marker_started,
                                 out);
    if (result < 0)
        close_cached_explorer(t);
    return result;
}

static int book_id_for_path(sqlite3 *db, const char *path, int64_t *bookid)
{
    if (!path || !bookid || path[0] != '/')
        return -1;
    size_t length = strnlen(path, TRACKER_BOOK_PATH_MAX + 1);
    if (length == 0 || length > TRACKER_BOOK_PATH_MAX)
        return -1;
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1])
        return 1;
    size_t dir_length = slash == path ? 1 : (size_t)(slash - path);
    char *directory = malloc(dir_length + 1);
    if (!directory)
        return -1;
    memcpy(directory, path, dir_length);
    directory[dir_length] = '\0';
    static const char *sql =
        "SELECT DISTINCT f.book_id FROM files f"
        " JOIN folders d ON d.id=f.folder_id"
        " WHERE d.name=?1 AND f.filename=?2 AND f.book_id>0 LIMIT 2";
    sqlite3_stmt *st = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, directory, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, slash + 1, -1, SQLITE_TRANSIENT);
        int count = 0, rc = SQLITE_DONE;
        int64_t found = 0;
        while (count < 2 && (rc = sqlite3_step(st)) == SQLITE_ROW) {
            found = sqlite3_column_int64(st, 0);
            count++;
        }
        if (count == 2 || rc == SQLITE_DONE) {
            result = count == 1 ? 0 : 1;
            if (count == 1)
                *bookid = found;
        }
    }
    sqlite3_finalize(st);
    free(directory);
    return result;
}

int tracker_book_id_for_path(const char *explorer_path, const char *path,
                             int64_t *bookid)
{
    sqlite3 *db = open_explorer(explorer_path);
    if (!db)
        return -1;
    int result = book_id_for_path(db, path, bookid);
    sqlite3_close(db);
    return result;
}

int tracker_cached_book_id_for_path(tracker *t, const char *path,
                                    int64_t *bookid)
{
    if (!t || !path || !bookid || path[0] != '/')
        return -1;
    size_t length = strnlen(path, TRACKER_BOOK_PATH_MAX + 1);
    if (length == 0 || length > TRACKER_BOOK_PATH_MAX)
        return -1;
    sqlite3 *db = cached_explorer(t);
    if (!db)
        return -1;
    if (t->cached_book_path_valid
        && !strcmp(t->cached_book_path, path)) {
        *bookid = t->cached_book_path_id;
        return 0;
    }
    int result = book_id_for_path(db, path, bookid);
    if (result == 0) {
        memcpy(t->cached_book_path, path, length + 1);
        t->cached_book_path_id = *bookid;
        t->cached_book_path_valid = 1;
    } else {
        t->cached_book_path_valid = 0;
    }
    if (result < 0)
        close_cached_explorer(t);
    return result;
}

static int begin_op(sqlite3 *db)
{
    return exec1(db, "SAVEPOINT betterstats_tracker");
}

static int finish_op(sqlite3 *db, int ok, int *error)
{
    if (ok && exec1(db, "RELEASE betterstats_tracker") == 0)
        return 0;
    if (*error == SQLITE_OK) {
        *error = sqlite3_extended_errcode(db);
        if (*error == SQLITE_OK)
            *error = SQLITE_ERROR;
    }
    exec1(db, "ROLLBACK TO betterstats_tracker");
    exec1(db, "RELEASE betterstats_tracker");
    return -1;
}

static int mutator_failure(tracker *t)
{
    int error = sqlite3_extended_errcode(t->stats);
    t->last_error = error == SQLITE_OK ? SQLITE_ERROR : error;
    return -1;
}

int tracker_error_retryable(const tracker *t)
{
    return t && sqlite_error_retryable(t->last_error);
}

static int upsert_book(tracker *t, const pb_state *s, int force_current,
                       int page, int64_t last_seen)
{
    static const char *sql =
        "INSERT INTO books"
        " (book_id,title,author,cover,cpage,npage,completed,completed_ts,last_seen)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
        " ON CONFLICT(book_id) DO UPDATE SET title=?2,author=?3,"
        " cover=CASE WHEN ?4='' THEN cover ELSE ?4 END,"
        " cpage=CASE WHEN ?10 OR last_seen IS NULL OR ?9>last_seen"
        "            THEN ?5 ELSE cpage END,"
        " npage=?6,completed=?7,completed_ts=?8,"
        " last_seen=CASE WHEN ?10 OR last_seen IS NULL OR ?9>last_seen"
        "                THEN ?9 ELSE last_seen END"
        " WHERE title IS NOT ?2 OR author IS NOT ?3"
        " OR (?4<>'' AND cover IS NOT ?4)"
        " OR npage IS NOT ?6 OR completed IS NOT ?7"
        " OR completed_ts IS NOT ?8"
        " OR ((?10 OR last_seen IS NULL OR ?9>last_seen)"
        "     AND (cpage IS NOT ?5 OR last_seen IS NOT ?9))";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_text(st, 2, s->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->author, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, s->cover, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, page);
    sqlite3_bind_int(st, 6, s->npage);
    sqlite3_bind_int(st, 7, s->completed);
    sqlite3_bind_int64(st, 8, s->completed_ts);
    sqlite3_bind_int64(st, 9, last_seen);
    sqlite3_bind_int(st, 10, force_current);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? 0 : -1;
}

static int same_metadata(const pb_state *a, const pb_state *b)
{
    return a->npage == b->npage && a->completed == b->completed
        && a->completed_ts == b->completed_ts
        && !strcmp(a->title, b->title)
        && !strcmp(a->author, b->author)
        && !strcmp(a->cover, b->cover);
}

static int same_input(const pb_state *a, const pb_state *b)
{
    return a->bookid == b->bookid && a->opentime == b->opentime
        && a->position_ts == b->position_ts && a->cpage == b->cpage
        && same_metadata(a, b);
}

static int64_t state_position(const pb_state *s)
{
    return s->position_ts == 0 ? s->opentime : s->position_ts;
}

static int add_page_distance(int current, int from, int to, int *out)
{
    int64_t distance = (int64_t)to - from;
    if (distance < 0)
        distance = -distance;
    if (distance > INT_MAX || current > INT_MAX - distance)
        return -1;
    *out = current + (int)distance;
    return 0;
}

static void reset_current(tracker *t)
{
    sqlite3 *stats = t->stats;
    sqlite3 *explorer = t->explorer;
    const char *explorer_path = t->explorer_path;
    uint64_t explorer_dev = t->explorer_dev;
    uint64_t explorer_ino = t->explorer_ino;
    int rtc_rollback = t->rtc_rollback;
    int rtc_rebase_allowed = t->rtc_rebase_allowed;
    int64_t rtc_floor = t->rtc_floor;
    int64_t rtc_catchup = t->rtc_catchup;
    memset(t, 0, sizeof(*t));
    t->stats = stats;
    t->explorer = explorer;
    t->explorer_path = explorer_path;
    t->explorer_dev = explorer_dev;
    t->explorer_ino = explorer_ino;
    t->rtc_rollback = rtc_rollback;
    t->rtc_rebase_allowed = rtc_rebase_allowed;
    t->rtc_floor = rtc_floor;
    t->rtc_catchup = rtc_catchup;
}

static int64_t state_floor(const tracker *t)
{
    int64_t floor = t->cur_row_start;
    if (t->cur_end_ts > floor)
        floor = t->cur_end_ts;
    if (t->cur_pos_ts > floor)
        floor = t->cur_pos_ts;
    return floor;
}

static int note_clock(tracker *t, int64_t boot_time, int64_t raw_wall)
{
    if (boot_time < 0 || raw_wall <= 0)
        return -1;
    int evidence = 0;
    int64_t expected_wall = 0;
    if (t->have_clock_sample) {
        if (boot_time < t->last_clock_boot)
            return -1;
        int64_t boot_delta = boot_time - t->last_clock_boot;
        if (boot_delta > INT64_MAX - t->last_raw_wall)
            return -1;
        expected_wall = t->last_raw_wall + boot_delta;
        if (raw_wall < t->last_raw_wall) {
            evidence = 1;
        } else {
            int64_t realtime_delta = raw_wall - t->last_raw_wall;
            evidence = boot_delta > RTC_TOLERANCE_SECONDS
                && realtime_delta < boot_delta - RTC_TOLERANCE_SECONDS;
            if (realtime_delta > boot_delta
                && realtime_delta - boot_delta > RTC_TOLERANCE_SECONDS)
                t->wall_forward_jump = 1;
        }
    }
    int64_t floor = state_floor(t);
    if (t->rtc_floor > floor)
        floor = t->rtc_floor;
    /* A durable floor below the RTC is startup evidence, not fresh evidence
     * on every tick of the same rollback epoch. Re-arming here would allow
     * more than one lower firmware snapshot to be accepted per epoch. */
    if (!t->rtc_rollback && floor > 0
        && raw_wall <= INT64_MAX - RTC_TOLERANCE_SECONDS
        && raw_wall + RTC_TOLERANCE_SECONDS < floor)
        evidence = 1;
    if (evidence) {
        t->rtc_rollback = 1;
        t->rtc_rebase_allowed = 1;
        if (expected_wall > t->rtc_catchup)
            t->rtc_catchup = expected_wall;
        if (floor > t->rtc_catchup)
            t->rtc_catchup = floor;
    }
    t->have_clock_sample = 1;
    t->last_raw_wall = raw_wall;
    t->last_clock_boot = boot_time;
    if (floor > t->rtc_floor)
        t->rtc_floor = floor;
    return 0;
}

static void finish_clock(tracker *t, int64_t raw_wall)
{
    int64_t floor = state_floor(t);
    if (floor > t->rtc_floor)
        t->rtc_floor = floor;
    int64_t catchup = t->rtc_catchup > t->rtc_floor
        ? t->rtc_catchup : t->rtc_floor;
    if (t->rtc_rollback && raw_wall >= catchup) {
        t->rtc_rollback = 0;
        t->rtc_rebase_allowed = 0;
        t->rtc_catchup = 0;
    }
}

static int64_t normalized_wall(const tracker *t, int64_t raw_wall)
{
    return t->rtc_rollback && raw_wall < t->rtc_floor
        ? t->rtc_floor : raw_wall;
}

static int checked_time(int64_t ts, time_t *out)
{
    time_t value = (time_t)ts;
    if (ts < 0 || (uint64_t)value != (uint64_t)ts)
        return -1;
    *out = value;
    return 0;
}

static int local_day_start(int64_t ts, int64_t *out)
{
    time_t value;
    struct tm tm;
    if (checked_time(ts, &value) != 0 || !localtime_r(&value, &tm))
        return -1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    value = mktime(&tm);
    if (value == (time_t)-1)
        return -1;
    *out = (int64_t)value;
    return 0;
}

static int next_local_midnight(int64_t ts, int64_t *out)
{
    time_t value;
    struct tm tm;
    if (checked_time(ts, &value) != 0 || !localtime_r(&value, &tm))
        return -1;
    tm.tm_mday++;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    value = mktime(&tm);
    if (value == (time_t)-1)
        return -1;
    *out = (int64_t)value;
    return 0;
}

static int row_key_exists(tracker *t, int64_t bookid, int64_t start)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->stats,
            "SELECT 1 FROM sessions WHERE book_id=?1 AND start_time=?2",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, bookid);
    sqlite3_bind_int64(st, 2, start);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW)
        return 1;
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Keep the legacy PK and move a rare colliding display timestamp forward. */
static int insert_session(tracker *t, int64_t bookid, int64_t firmware_open,
                          int64_t preferred, int same_day,
                          int page_start, int page_start_known,
                          int page_end, int moved, int64_t max_start,
                          int64_t *actual_out)
{
    if (preferred <= 0)
        return -1;
    int64_t day = 0;
    if (same_day && local_day_start(preferred, &day) != 0)
        return -1;
    for (int64_t candidate = preferred;; candidate++) {
        if (max_start > 0 && candidate > max_start)
            return 1;
        if (same_day) {
            int64_t candidate_day;
            if (local_day_start(candidate, &candidate_day) != 0
                || candidate_day != day)
                return -1;
        }
        int exists = row_key_exists(t, bookid, candidate);
        if (exists < 0)
            return -1;
        if (!exists) {
            static const char *sql =
                "INSERT INTO sessions"
                " (book_id,start_time,end_time,pages_start,pages_end,"
                " pages_moved,firmware_open_time)"
                " VALUES(?1,?2,?2,?3,?4,?5,?6)";
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
                return -1;
            sqlite3_bind_int64(st, 1, bookid);
            sqlite3_bind_int64(st, 2, candidate);
            if (page_start_known)
                sqlite3_bind_int(st, 3, page_start);
            else
                sqlite3_bind_null(st, 3);
            sqlite3_bind_int(st, 4, page_end);
            sqlite3_bind_int(st, 5, moved);
            sqlite3_bind_int64(st, 6, firmware_open);
            int rc = sqlite3_step(st);
            sqlite3_finalize(st);
            if (rc == SQLITE_DONE) {
                *actual_out = candidate;
                return 0;
            }
            if ((rc & 0xff) != SQLITE_CONSTRAINT)
                return -1;
        }
        if (candidate == INT64_MAX)
            return -1;
    }
}

static int set_session(tracker *t, int64_t end, int64_t active)
{
    static const char *sql =
        "UPDATE sessions SET end_time=MAX(end_time,?1),"
        " active_seconds=MAX(active_seconds,?2),pages_end=?3,"
        " pages_moved=MAX(pages_moved,?4)"
        " WHERE book_id=?5 AND start_time=?6 AND firmware_open_time=?7";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, end);
    sqlite3_bind_int64(st, 2, active);
    sqlite3_bind_int(st, 3, t->cur_pages_last);
    sqlite3_bind_int(st, 4, t->cur_row_moved);
    sqlite3_bind_int64(st, 5, t->cur_book);
    sqlite3_bind_int64(st, 6, t->cur_row_start);
    sqlite3_bind_int64(st, 7, t->cur_open);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(t->stats) == 1;
    sqlite3_finalize(st);
    if (!ok)
        return -1;
    if (end > t->cur_end_ts)
        t->cur_end_ts = end;
    if (active > t->cur_row_durable)
        t->cur_row_durable = active;
    t->cur_durable_page = t->cur_pages_last;
    if (t->cur_row_moved > t->cur_durable_moved)
        t->cur_durable_moved = t->cur_row_moved;
    return 0;
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

static int add_nonnegative(int64_t *value, int64_t amount)
{
    if (*value < 0 || amount < 0 || amount > INT64_MAX - *value)
        return -1;
    *value += amount;
    return 0;
}

/* Add two residues below modulus without overflowing. Returns the carry. */
static unsigned add_mod(uint64_t left, uint64_t right, uint64_t modulus,
                        uint64_t *remainder)
{
    if (left >= modulus - right) {
        *remainder = left - (modulus - right);
        return 1;
    }
    *remainder = left + right;
    return 0;
}

/* Exact floor(a*b/divisor) for old ARM toolchains without wide integer
 * extensions. The quotient/remainder form also detects an unrepresentable
 * result instead of relying on signed overflow. */
static int mul_div_floor(int64_t a, int64_t b, int64_t divisor, int64_t *out)
{
    if (!out || a < 0 || b < 0 || divisor <= 0)
        return -1;
    uint64_t d = (uint64_t)divisor;
    uint64_t ub = (uint64_t)b;
    uint64_t whole = (uint64_t)(a / divisor);
    uint64_t residue = (uint64_t)(a % divisor);
    if (whole && ub > (uint64_t)INT64_MAX / whole)
        return -1;
    uint64_t result = whole * ub;

    /* Long division of residue*b by d. residue is strictly below d. */
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    for (int bit = 63; bit >= 0; --bit) {
        unsigned carry = add_mod(remainder, remainder, d, &remainder);
        if (quotient > (UINT64_MAX - carry) / 2)
            return -1;
        quotient = quotient * 2 + carry;
        if ((ub >> bit) & 1U) {
            carry = add_mod(remainder, residue, d, &remainder);
            if (quotient > UINT64_MAX - carry)
                return -1;
            quotient += carry;
        }
    }
    if (result > (uint64_t)INT64_MAX
        || quotient > (uint64_t)INT64_MAX - result)
        return -1;
    *out = (int64_t)(result + quotient);
    return 0;
}

int tracker_test_mul_div_floor(int64_t a, int64_t b, int64_t divisor,
                               int64_t *out)
{
    return mul_div_floor(a, b, divisor, out);
}

static int projected_active(const tracker *t, int64_t present,
                            int64_t *active_out)
{
    if (present < t->cur_row_present)
        return -1;
    int64_t gained = measured(present - t->cur_row_present,
                              t->cur_row_moved, t->cur_budget_used);
    if (gained > INT64_MAX - t->cur_row_base)
        return -1;
    int64_t active = t->cur_row_base + gained;
    if (active < t->cur_row_durable)
        active = t->cur_row_durable;
    *active_out = active;
    return 0;
}

static int calculate_tail(const tracker *t, int64_t present, int64_t raw_wall,
                          int64_t *target_out, int64_t *end_out)
{
    int64_t target;
    if (projected_active(t, present, &target) != 0)
        return -1;
    int64_t base = state_floor(t);
    int64_t pending = present > t->cur_last_present
        ? present - t->cur_last_present : 0;
    int64_t wall_room = raw_wall > base ? raw_wall - base : 0;
    int64_t wall_advance = pending < wall_room ? pending : wall_room;
    int64_t wall_candidate = base + wall_advance;
    int64_t active_floor = t->cur_row_start;
    if (target > 0) {
        if (target - 1 > INT64_MAX - active_floor)
            return -1;
        active_floor += target - 1;
    }
    int64_t logical = wall_candidate > active_floor
        ? wall_candidate : active_floor;
    if (t->wall_forward_jump && raw_wall > logical
        && (pending > 0 || target > t->cur_row_durable))
        logical = raw_wall;
    if (logical < base)
        logical = base;
    *target_out = target;
    *end_out = logical;
    return 0;
}

/* credit_override is -1 for the normal page-budget calculation. During a
 * day split immediately before applying a new snapshot, it is the credit
 * already calculated with that snapshot. This lets time cross midnight with
 * exact totals while the old bucket keeps its pre-snapshot page metadata. */
static int write_current(tracker *t, int64_t present, int64_t end,
                         int64_t credit_override, int64_t hard_end)
{
    if (credit_override < -1 || hard_end < 0)
        return -1;
    if (hard_end > 0 && end > hard_end)
        end = hard_end;
    if (end < state_floor(t))
        end = state_floor(t);
    if (hard_end > 0 && end > hard_end)
        return -1;
    for (;;) {
        if (hard_end == 0) {
            int64_t row_target = t->cur_row_base;
            if ((credit_override >= 0
                 ? add_nonnegative(&row_target, credit_override)
                 : projected_active(t, present, &row_target)) != 0)
                return -1;
            if (row_target > 0) {
                if (row_target - 1 > INT64_MAX - t->cur_row_start)
                    return -1;
                int64_t minimum_end = t->cur_row_start + row_target - 1;
                if (end < minimum_end)
                    end = minimum_end;
            }
        }
        int64_t row_day, end_day;
        if (local_day_start(t->cur_row_start, &row_day) != 0
            || local_day_start(end, &end_day) != 0)
            return -1;
        if (row_day >= end_day)
            break;
        int64_t midnight;
        if (next_local_midnight(t->cur_row_start, &midnight) != 0
            || midnight <= t->cur_row_start || midnight > end)
            return -1;
        int64_t gained = present - t->cur_row_present;
        if (gained < 0 || t->cur_last_present < t->cur_row_present)
            return -1;
        int64_t span = end - t->cur_row_start;
        int64_t before = 0;
        if (span > 0
            && mul_div_floor(gained, midnight - t->cur_row_start,
                             span, &before) != 0)
            return -1;
        int64_t durable_presence = t->cur_last_present - t->cur_row_present;
        if (before < durable_presence)
            before = durable_presence;
        int64_t credit = credit_override >= 0
            ? credit_override
            : measured(gained, t->cur_row_moved, t->cur_budget_used);
        if (credit > gained)
            return -1;
        int64_t before_credit = 0;
        if (gained > 0
            && mul_div_floor(credit, before, gained,
                             &before_credit) != 0)
            return -1;
        if (t->cur_row_durable < t->cur_row_base)
            return -1;
        int64_t durable_gain = t->cur_row_durable - t->cur_row_base;
        if (before_credit < durable_gain)
            before_credit = durable_gain;
        int64_t old_capacity = midnight - t->cur_row_start;
        int64_t remaining_capacity = old_capacity > t->cur_row_base
            ? old_capacity - t->cur_row_base : 0;
        if (remaining_capacity < durable_gain)
            remaining_capacity = durable_gain;
        if (before_credit > remaining_capacity) {
            before_credit = remaining_capacity;
            before = remaining_capacity;
            if (before < durable_presence)
                before = durable_presence;
        }
        if (before_credit > credit)
            return -1;
        int64_t old_active = t->cur_row_base;
        if (add_nonnegative(&old_active, before_credit) != 0
            || set_session(t, midnight - 1, old_active) != 0)
            return -1;
        int endpoint_jump = t->wall_forward_jump
            || (end > t->last_raw_wall
                && end - t->last_raw_wall > RTC_TOLERANCE_SECONDS)
            || before_credit == credit
            || before_credit == durable_gain;
        int64_t preferred = endpoint_jump && midnight < end_day
            ? end_day : midnight;
        int64_t actual;
        int inserted = insert_session(
            t, t->cur_book, t->cur_open, preferred, 1,
            t->cur_pages_last, 1, t->cur_pages_last, t->cur_row_moved,
            hard_end, &actual);
        if (inserted < 0)
            return -1;
        if (inserted > 0) {
            t->cur_row_present = t->cur_last_present = present;
            t->cur_resume_page_start = t->cur_pages_last;
            t->wall_forward_jump = 0;
            return 0; /* bounded close: no key before B, discard the tail */
        }
        int64_t shift = actual - preferred;
        if (shift > 0) {
            if (hard_end > 0) {
                if (actual > hard_end)
                    return -1;
                if (end < actual)
                    end = actual;
            } else {
                if (end > INT64_MAX - shift)
                    return -1;
                end += shift;
            }
        }
        if (add_nonnegative(&t->cur_budget_used, before_credit) != 0)
            return -1;
        if (credit_override >= 0)
            credit_override -= before_credit;
        t->cur_row_start = actual;
        t->cur_row_base = 0;
        t->cur_row_present += before;
        t->cur_last_present = t->cur_row_present;
        t->cur_end_ts = actual;
        t->cur_row_durable = 0;
        t->cur_durable_page = t->cur_pages_last;
        t->cur_durable_moved = t->cur_row_moved;
        if (end < actual)
            end = actual;
    }
    int64_t target = t->cur_row_base;
    if ((credit_override >= 0
         ? add_nonnegative(&target, credit_override)
         : projected_active(t, present, &target)) != 0)
        return -1;
    if (hard_end > 0) {
        int64_t capacity = end >= t->cur_row_start
            ? end - t->cur_row_start + 1 : 0;
        if (target > capacity)
            target = capacity;
        if (target < t->cur_row_durable)
            target = t->cur_row_durable;
    }
    if (set_session(t, end, target) != 0)
        return -1;
    t->cur_last_present = present;
    t->cur_resume_page_start = t->cur_pages_last;
    if (state_floor(t) > t->rtc_floor)
        t->rtc_floor = state_floor(t);
    t->wall_forward_jump = 0;
    return 0;
}

typedef struct {
    int book_dirty;
    int position_accepted;
} snapshot_change;

static int apply_snapshot(tracker *t, const pb_state *s, int force_book,
                          snapshot_change *change)
{
    memset(change, 0, sizeof(*change));
    if (!s || s->bookid != t->cur_book || s->opentime != t->cur_open)
        return -1;
    int metadata_changed = !t->have_last_input
        || !same_metadata(&t->last_input, s);
    int64_t raw = state_position(s);
    int page_changed = s->cpage != t->cur_pages_last;
    int accept = raw > t->cur_raw_pos_ts
        || (raw == t->cur_raw_pos_ts && page_changed);
    if (raw < t->cur_raw_pos_ts && t->rtc_rollback
        && t->rtc_rebase_allowed) {
        accept = 1;
        t->rtc_rebase_allowed = 0;
    }
    if (accept) {
        if (page_changed
            && add_page_distance(t->cur_row_moved, t->cur_pages_last,
                                 s->cpage, &t->cur_row_moved) != 0)
            return -1;
        t->cur_pages_last = s->cpage;
        t->cur_raw_pos_ts = raw;
        int64_t logical = raw;
        if (t->rtc_rollback && logical < t->rtc_floor)
            logical = t->rtc_floor;
        if (logical < t->cur_pos_ts)
            logical = t->cur_pos_ts;
        t->cur_pos_ts = logical;
        if (logical > t->rtc_floor)
            t->rtc_floor = logical;
        change->position_accepted = 1;
    }
    change->book_dirty = force_book || metadata_changed
        || change->position_accepted;
    t->last_input = *s;
    t->have_last_input = 1;
    return 0;
}

static int tail_changed(const tracker *t, int64_t target, int64_t end)
{
    return target > t->cur_row_durable || end > t->cur_end_ts
        || t->cur_pages_last != t->cur_durable_page
        || t->cur_row_moved != t->cur_durable_moved;
}

static int persist_sync(tracker *t, const pb_state *metadata,
                        int book_dirty, int64_t present, int64_t raw_wall)
{
    int64_t target, end;
    if (calculate_tail(t, present, raw_wall, &target, &end) != 0)
        return -1;
    if (book_dirty
        && upsert_book(t, metadata, 1, t->cur_pages_last,
                       t->cur_pos_ts) != 0)
        return -1;
    if (tail_changed(t, target, end)
        && write_current(t, present, end, -1, 0) != 0)
        return -1;
    return 0;
}

/* Work out the complete post-snapshot endpoint without touching the caller's
 * state. The endpoint tells sync_snapshot whether it must close an older day
 * before the new page snapshot is applied. */
static int plan_snapshot(const tracker *before, const pb_state *s,
                         int force_book, int64_t present, int64_t raw_wall,
                         tracker *after, snapshot_change *change,
                         int64_t *target, int64_t *end, int *split_day)
{
    *after = *before;
    if (apply_snapshot(after, s, force_book, change) != 0
        || calculate_tail(after, present, raw_wall, target, end) != 0)
        return -1;
    int64_t before_day, end_day;
    if (local_day_start(before->cur_row_start, &before_day) != 0
        || local_day_start(*end, &end_day) != 0)
        return -1;
    *split_day = before_day < end_day;
    return 0;
}

/* Persist one snapshot and its tail in the caller's existing savepoint. */
static int sync_snapshot(tracker *t, const pb_state *s, int force_book,
                         int64_t present, int64_t raw_wall)
{
    tracker planned;
    snapshot_change plan_change;
    int64_t target, end;
    int split_day;
    if (plan_snapshot(t, s, force_book, present, raw_wall, &planned,
                      &plan_change, &target, &end, &split_day) != 0)
        return -1;
    if (split_day) {
        if (target < t->cur_row_base
            || write_current(t, present, end,
                             target - t->cur_row_base, 0) != 0)
            return -1;
    }
    snapshot_change applied;
    if (apply_snapshot(t, s, force_book, &applied) != 0
        || persist_sync(t, s, applied.book_dirty,
                        present, raw_wall) != 0)
        return -1;
    return 0;
}

static int load_existing_session(tracker *t, const pb_state *s,
                                 int64_t present)
{
    static const char *sql =
        "SELECT start_time,end_time,active_seconds,pages_end,pages_moved"
        " FROM sessions WHERE book_id=?1 AND firmware_open_time=?2"
        " ORDER BY start_time";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(t->stats, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, s->bookid);
    sqlite3_bind_int64(st, 2, s->opentime);
    int rows = 0, rc, page = s->cpage, page_known = 0;
    int max_moved = 0, any_moved = 0;
    int64_t total_active = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int64_t active = sqlite3_column_int64(st, 2);
        if (active < 0 || total_active > INT64_MAX - active) {
            sqlite3_finalize(st);
            return -1;
        }
        total_active += active;
        rows++;
        t->cur_row_start = sqlite3_column_int64(st, 0);
        t->cur_end_ts = sqlite3_column_int64(st, 1);
        t->cur_row_base = t->cur_row_durable = active;
        if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
            page = sqlite3_column_int(st, 3);
            page_known = 1;
        }
        int moved = sqlite3_column_int(st, 4);
        if (moved > 0)
            any_moved = 1;
        if (moved > max_moved)
            max_moved = moved;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return -1;
    if (!rows)
        return 1;

    int64_t minimum = total_active / SECONDS_PER_PAGE_CAP
        + (any_moved ? total_active % SECONDS_PER_PAGE_CAP != 0 : 1);
    if (minimum < 1)
        minimum = 1;
    if (minimum > INT_MAX)
        return -1;
    int inferred = any_moved && max_moved > minimum
        ? max_moved : (int)minimum;
    t->cur_book = s->bookid;
    t->cur_open = s->opentime;
    t->cur_pos_ts = state_floor(t);
    t->cur_raw_pos_ts = t->cur_pos_ts;
    t->cur_row_present = present;
    t->cur_last_present = present;
    t->cur_budget_used = total_active;
    t->cur_row_moved = inferred;
    t->cur_pages_last = page_known ? page : s->cpage;
    t->cur_resume_page_start = t->cur_pages_last;
    t->cur_durable_page = t->cur_pages_last;
    t->cur_durable_moved = max_moved;
    int64_t floor = state_floor(t);
    if (floor > t->rtc_floor)
        t->rtc_floor = floor;
    return 0;
}

static int prepare_different(tracker *t, const pb_state *s, int64_t present,
                             int64_t boot_time, int64_t raw_wall)
{
    reset_current(t);
    int loaded = load_existing_session(t, s, present);
    if (loaded < 0)
        return -1;
    if (loaded == 1) {
        t->cur_book = s->bookid;
        t->cur_open = s->opentime;
        if (note_clock(t, boot_time, raw_wall) != 0)
            return -1;
        int64_t actual;
        if (insert_session(t, s->bookid, s->opentime,
                           normalized_wall(t, raw_wall), 0,
                           s->cpage, 1, s->cpage, 1, 0, &actual) != 0)
            return -1;
        t->cur_row_start = t->cur_end_ts = actual;
        t->cur_row_base = t->cur_row_durable = 0;
        t->cur_row_present = t->cur_last_present = present;
        t->cur_budget_used = 0;
        t->cur_row_moved = t->cur_durable_moved = 1;
        t->cur_pages_last = t->cur_resume_page_start
            = t->cur_durable_page = s->cpage;
        t->cur_raw_pos_ts = state_position(s);
        t->cur_pos_ts = t->cur_raw_pos_ts > actual
            ? t->cur_raw_pos_ts : actual;
        if (state_floor(t) > t->rtc_floor)
            t->rtc_floor = state_floor(t);
        t->wall_forward_jump = 0;
        t->last_input = *s;
        t->have_last_input = 1;
        if (upsert_book(t, s, 1, t->cur_pages_last, t->cur_pos_ts) != 0)
            return -1;
        int64_t target, end;
        if (calculate_tail(t, present, raw_wall, &target, &end) != 0)
            return -1;
        if (tail_changed(t, target, end)
            && write_current(t, present, end, -1, 0) != 0)
            return -1;
        finish_clock(t, raw_wall);
        return 1;
    }
    if (note_clock(t, boot_time, raw_wall) != 0)
        return -1;
    if (sync_snapshot(t, s, 1, present, raw_wall) != 0)
        return -1;
    finish_clock(t, raw_wall);
    return 0;
}

int tracker_prepare(tracker *t, const pb_state *s, int64_t present,
                    int64_t boot_time, int64_t raw_wall)
{
    if (!t || !s || present < 0 || s->bookid <= 0 || s->opentime <= 0)
        return -1;
    t->last_error = SQLITE_OK;
    tracker proposed = *t;
    if (begin_op(t->stats) != 0)
        return mutator_failure(t);
    int rc;
    if (proposed.cur_book == s->bookid && proposed.cur_open == s->opentime) {
        if (note_clock(&proposed, boot_time, raw_wall) != 0) {
            rc = -1;
        } else {
            rc = sync_snapshot(&proposed, s, 1, present, raw_wall);
            if (rc == 0)
                finish_clock(&proposed, raw_wall);
        }
    } else {
        rc = prepare_different(&proposed, s, present, boot_time, raw_wall);
    }
    int error = SQLITE_OK;
    if (finish_op(t->stats, rc >= 0, &error) != 0) {
        t->last_error = error;
        return -1;
    }
    *t = proposed;
    t->last_error = SQLITE_OK;
    return rc;
}

int tracker_resume(tracker *t, int64_t present, int64_t boot_time,
                   int64_t raw_wall)
{
    if (!t || present < 0)
        return -1;
    t->last_error = SQLITE_OK;
    if (t->cur_book <= 0)
        return -1;
    tracker proposed = *t;
    if (note_clock(&proposed, boot_time, raw_wall) != 0)
        return -1;
    int64_t projected;
    if (projected_active(&proposed, present, &projected) != 0
        || projected > proposed.cur_row_durable)
        return -1;
    int64_t logical = normalized_wall(&proposed, raw_wall);
    if (logical == proposed.cur_row_start) {
        if (add_nonnegative(&proposed.cur_budget_used,
                            proposed.cur_row_durable
                            - proposed.cur_row_base) != 0)
            return -1;
        proposed.cur_row_base = proposed.cur_row_durable;
        proposed.cur_row_present = proposed.cur_last_present = present;
        proposed.cur_resume_page_start = proposed.cur_pages_last;
        proposed.wall_forward_jump = 0;
        finish_clock(&proposed, raw_wall);
        *t = proposed;
        return 0;
    }
    if (proposed.cur_end_ts == INT64_MAX)
        return -1;
    int64_t minimum = proposed.cur_end_ts + 1;
    if (logical < minimum)
        logical = minimum;
    if (begin_op(t->stats) != 0)
        return mutator_failure(t);
    int64_t actual;
    int rc = insert_session(&proposed, proposed.cur_book, proposed.cur_open,
                            logical, 0, proposed.cur_resume_page_start, 1,
                            proposed.cur_pages_last, proposed.cur_row_moved,
                            0, &actual);
    if (rc == 0) {
        if (add_nonnegative(&proposed.cur_budget_used,
                            proposed.cur_row_durable
                            - proposed.cur_row_base) != 0)
            rc = -1;
    }
    if (rc == 0) {
        proposed.cur_row_start = proposed.cur_end_ts = actual;
        proposed.cur_row_base = proposed.cur_row_durable = 0;
        proposed.cur_row_present = proposed.cur_last_present = present;
        proposed.cur_resume_page_start = proposed.cur_pages_last;
        proposed.cur_durable_page = proposed.cur_pages_last;
        proposed.cur_durable_moved = proposed.cur_row_moved;
        if (actual > proposed.rtc_floor)
            proposed.rtc_floor = actual;
        proposed.wall_forward_jump = 0;
        finish_clock(&proposed, raw_wall);
    }
    int error = SQLITE_OK;
    if (finish_op(t->stats, rc == 0, &error) != 0) {
        t->last_error = error;
        return -1;
    }
    *t = proposed;
    t->last_error = SQLITE_OK;
    return 0;
}

int tracker_observe(tracker *t, const pb_state *s, int64_t present,
                    int64_t boot_time, int64_t raw_wall)
{
    if (!t || !s || present < 0 || s->bookid != t->cur_book
        || s->opentime != t->cur_open)
        return -1;
    t->last_error = SQLITE_OK;
    tracker proposed = *t;
    if (note_clock(&proposed, boot_time, raw_wall) != 0)
        return -1;
    if (proposed.have_last_input && same_input(&proposed.last_input, s)) {
        finish_clock(&proposed, raw_wall);
        *t = proposed;
        return 0;
    }
    tracker planned;
    snapshot_change change;
    int64_t target, end;
    int split_day;
    if (plan_snapshot(&proposed, s, 0, present, raw_wall, &planned,
                      &change, &target, &end, &split_day) != 0)
        return -1;
    int db_needed = split_day || change.book_dirty;
    if (!db_needed) {
        finish_clock(&planned, raw_wall);
        *t = planned;
        return 0;
    }
    if (begin_op(t->stats) != 0)
        return mutator_failure(t);
    int rc = sync_snapshot(&proposed, s, 0, present, raw_wall);
    if (rc == 0)
        finish_clock(&proposed, raw_wall);
    int error = SQLITE_OK;
    if (finish_op(t->stats, rc == 0, &error) != 0) {
        t->last_error = error;
        return -1;
    }
    *t = proposed;
    t->last_error = SQLITE_OK;
    return 2;
}

int tracker_checkpoint_due(const tracker *t, int64_t present)
{
    int64_t target;
    return t && t->cur_book > 0
        && projected_active(t, present, &target) == 0
        && target - t->cur_row_durable >= TRACKER_CHECKPOINT_SECONDS;
}

static int consume_bounded_tail(tracker *t, int64_t present)
{
    if (present < t->cur_row_present
        || t->cur_row_durable < t->cur_row_base
        || add_nonnegative(&t->cur_budget_used,
                           t->cur_row_durable - t->cur_row_base) != 0)
        return -1;
    t->cur_row_base = t->cur_row_durable;
    t->cur_row_present = t->cur_last_present = present;
    t->cur_resume_page_start = t->cur_pages_last;
    return 0;
}

static int persist_bounded(tracker *t, const pb_state *final_state,
                           int64_t present, int64_t end, int64_t target,
                           int64_t max_end)
{
    if (target < t->cur_row_base)
        return -1;
    /* Persist the bounded endpoint before applying the final position. That
     * snapshot may lie beyond the next session's proven boundary. */
    int64_t hard_end = state_floor(t) > max_end ? state_floor(t) : max_end;
    if (write_current(t, present, end, target - t->cur_row_base,
                      hard_end) != 0)
        return -1;

    snapshot_change change;
    memset(&change, 0, sizeof(change));
    if (final_state
        && (apply_snapshot(t, final_state, 0, &change) != 0
            || (change.book_dirty
                && upsert_book(t, final_state, 1, t->cur_pages_last,
                               t->cur_pos_ts) != 0)))
        return -1;
    if (final_state
        && set_session(t, t->cur_end_ts, t->cur_row_durable) != 0)
        return -1;
    return consume_bounded_tail(t, present);
}

static int tracker_flush_policy(tracker *t, const pb_state *final_state,
                                int64_t present, int64_t boot_time,
                                int64_t raw_wall, int bounded,
                                int64_t max_end)
{
    if (!t || present < 0 || raw_wall <= 0 || (bounded && max_end <= 0))
        return -1;
    t->last_error = SQLITE_OK;
    if (t->cur_book <= 0)
        return final_state ? -1 : 0;
    if (final_state && (final_state->bookid != t->cur_book
                        || final_state->opentime != t->cur_open))
        return -1;
    tracker proposed = *t;
    if (note_clock(&proposed, boot_time, raw_wall) != 0)
        return -1;
    int64_t target, end;
    int split_day = 0;
    int book_dirty = 0;
    tracker planned = proposed;
    if (final_state) {
        snapshot_change change;
        if (plan_snapshot(&proposed, final_state, 0, present, raw_wall,
                          &planned, &change, &target, &end,
                          &split_day) != 0)
            return -1;
        book_dirty = change.book_dirty;
    } else if (calculate_tail(&planned, present, raw_wall,
                              &target, &end) != 0) {
        return -1;
    }
    if (bounded) {
        int64_t floor = state_floor(&proposed);
        if (floor <= max_end && end > max_end)
            end = max_end;
        else if (floor > max_end)
            end = floor;
        int64_t capacity = 0;
        if (max_end >= planned.cur_row_start) {
            if (max_end - planned.cur_row_start == INT64_MAX)
                return -1;
            capacity = max_end - planned.cur_row_start + 1;
        }
        if (target > capacity)
            target = capacity;
        if (target < planned.cur_row_durable)
            target = planned.cur_row_durable;
        if (target < planned.cur_row_base)
            return -1;
    }
    int db_needed = split_day || book_dirty
        || tail_changed(&planned, target, end);
    if (!db_needed) {
        if (bounded && consume_bounded_tail(&planned, present) != 0)
            return -1;
        finish_clock(&planned, raw_wall);
        *t = planned;
        return 0;
    }
    if (begin_op(t->stats) != 0)
        return mutator_failure(t);
    int rc;
    if (bounded) {
        rc = persist_bounded(&proposed, final_state, present, end, target,
                             max_end);
    } else if (final_state) {
        rc = sync_snapshot(&proposed, final_state, 0,
                           present, raw_wall);
    } else {
        rc = persist_sync(&proposed, &proposed.last_input, 0,
                          present, raw_wall);
    }
    if (rc == 0)
        finish_clock(&proposed, raw_wall);
    int error = SQLITE_OK;
    if (finish_op(t->stats, rc == 0, &error) != 0) {
        t->last_error = error;
        return -1;
    }
    *t = proposed;
    t->last_error = SQLITE_OK;
    return 0;
}

int tracker_flush(tracker *t, const pb_state *final_state, int64_t present,
                  int64_t boot_time, int64_t raw_wall)
{
    return tracker_flush_policy(t, final_state, present, boot_time, raw_wall,
                                0, 0);
}

int tracker_flush_bounded(tracker *t, const pb_state *final_state,
                          int64_t present, int64_t boot_time, int64_t raw_wall,
                          int64_t max_end)
{
    return tracker_flush_policy(t, final_state, present, boot_time, raw_wall,
                                1, max_end);
}

int tracker_recover(tracker *t, int64_t skip_bookid, int64_t skip_opentime)
{
    if (!t)
        return -1;
    t->last_error = SQLITE_OK;
    sqlite3 *db = open_explorer(t->explorer_path);
    if (!db)
        return -1;
    static const char *sql =
        "SELECT " STATE_COLUMNS
        " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
        " WHERE s.opentime>0 OR (s.completed=1 AND s.completed_ts>0)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    if (begin_op(t->stats) != 0) {
        sqlite3_finalize(st);
        sqlite3_close(db);
        return mutator_failure(t);
    }
    int n = 0, ok = 1, rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        pb_state s;
        fill_state(st, &s);
        if (s.bookid == skip_bookid && s.opentime == skip_opentime)
            continue;
        if (upsert_book(t, &s, 0, s.cpage, state_position(&s)) != 0) {
            ok = 0;
            break;
        }
        n++;
    }
    if (rc != SQLITE_DONE)
        ok = 0;
    sqlite3_finalize(st);
    sqlite3_close(db);
    int error = SQLITE_OK;
    if (finish_op(t->stats, ok, &error) != 0) {
        t->last_error = error;
        return -1;
    }
    t->last_error = SQLITE_OK;
    return n;
}
