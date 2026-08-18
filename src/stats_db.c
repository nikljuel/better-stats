#include "stats_db.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static double q_double(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    double v = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return v;
}

int stats_overall(sqlite3 *db, overall_stats *o)
{
    memset(o, 0, sizeof(*o));
    o->total_hours = q_double(db, "SELECT SUM(active_seconds)/3600.0 FROM sessions");
    o->session_count = (int)q_double(db,
        "SELECT COUNT(*) FROM sessions WHERE active_seconds >= 60");
    if (o->session_count > 0)
        o->avg_session_min = q_double(db,
            "SELECT AVG(active_seconds)/60.0 FROM sessions WHERE active_seconds >= 60");
    double mins = q_double(db,
        "SELECT SUM(active_seconds)/60.0 FROM sessions"
        " WHERE pages_start IS NOT NULL AND pages_end > pages_start AND active_seconds > 0");
    double pages = q_double(db,
        "SELECT SUM(pages_end - pages_start) FROM sessions"
        " WHERE pages_start IS NOT NULL AND pages_end > pages_start AND active_seconds > 0");
    if (mins > 0)
        o->pages_per_min = pages / mins;
    o->books_total = (int)q_double(db, "SELECT COUNT(*) FROM books");
    o->books_finished = (int)q_double(db,
        "SELECT COUNT(*) FROM books WHERE completed = 1");
    o->today_secs = (int)q_double(db,
        "SELECT IFNULL(SUM(active_seconds),0) FROM sessions"
        " WHERE date(end_time,'unixepoch','localtime') = date('now','localtime')");
    o->today_pages = (int)q_double(db,
        "SELECT IFNULL(SUM(pages_end - pages_start),0) FROM sessions"
        " WHERE pages_start IS NOT NULL AND pages_end > pages_start"
        " AND date(end_time,'unixepoch','localtime') = date('now','localtime')");
    o->week_secs = (int)q_double(db,
        "SELECT IFNULL(SUM(active_seconds),0) FROM sessions"
        " WHERE end_time >= strftime('%s','now','localtime','start of day','-6 days','utc')");
    o->week_pages = (int)q_double(db,
        "SELECT IFNULL(SUM(pages_end - pages_start),0) FROM sessions"
        " WHERE pages_start IS NOT NULL AND pages_end > pages_start"
        " AND end_time >= strftime('%s','now','localtime','start of day','-6 days','utc')");

    /* Streak: aufeinanderfolgende Lesetage bis heute (oder gestern). */
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT DISTINCT date(end_time,'unixepoch','localtime') d FROM sessions"
        " WHERE active_seconds > 0 ORDER BY d DESC LIMIT 366";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        time_t now = time(NULL);
        int streak = 0;
        time_t expect = now;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *d = (const char *)sqlite3_column_text(st, 0);
            struct tm tm = {0};
            if (!d || sscanf(d, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
                break;
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            tm.tm_hour = 12;
            time_t day = mktime(&tm);
            double diff = difftime(expect, day) / 86400.0;
            if (streak == 0 && diff > 1.6)
                break; /* weder heute noch gestern gelesen -> Streak 0 */
            if (streak > 0 && (diff < 0.4 || diff > 1.6))
                break;
            streak++;
            expect = day;
        }
        o->streak_days = streak;
    }
    sqlite3_finalize(st);
    return 0;
}

static int days_in_month(int year, int mon)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int n = d[mon - 1];
    if (mon == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        n = 29;
    return n;
}

int stats_month(sqlite3 *db, int year, int mon, int secs[32])
{
    memset(secs, 0, sizeof(int) * 32);
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT CAST(strftime('%d', end_time,'unixepoch','localtime') AS INTEGER),"
        " SUM(active_seconds) FROM sessions"
        " WHERE strftime('%Y-%m', end_time,'unixepoch','localtime') = printf('%04d-%02d',?1,?2)"
        " GROUP BY 1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, year);
        sqlite3_bind_int(st, 2, mon);
        while (sqlite3_step(st) == SQLITE_ROW) {
            int d = sqlite3_column_int(st, 0);
            if (d >= 1 && d <= 31)
                secs[d] = sqlite3_column_int(st, 1);
        }
    }
    sqlite3_finalize(st);
    return days_in_month(year, mon);
}

int stats_month_books(sqlite3 *db, int year, int mon, month_book *out, int max)
{
    sqlite3_stmt *st = NULL;
    int n = 0;
    const char *sql =
        "SELECT IFNULL(b.title,'?'),"
        " MIN(CAST(strftime('%d', s.end_time,'unixepoch','localtime') AS INTEGER)),"
        " MAX(CAST(strftime('%d', s.end_time,'unixepoch','localtime') AS INTEGER))"
        " FROM sessions s LEFT JOIN books b ON b.book_id = s.book_id"
        " WHERE strftime('%Y-%m', s.end_time,'unixepoch','localtime') = printf('%04d-%02d',?1,?2)"
        " AND s.active_seconds >= 60"
        " GROUP BY s.book_id ORDER BY SUM(s.active_seconds) DESC LIMIT ?3";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, year);
        sqlite3_bind_int(st, 2, mon);
        sqlite3_bind_int(st, 3, max);
        while (sqlite3_step(st) == SQLITE_ROW && n < max) {
            snprintf(out[n].title, sizeof(out[n].title), "%s",
                     sqlite3_column_text(st, 0));
            out[n].first_day = sqlite3_column_int(st, 1);
            out[n].last_day = sqlite3_column_int(st, 2);
            n++;
        }
    }
    sqlite3_finalize(st);
    return n;
}

void stats_book(sqlite3 *db, int64_t bookid, int64_t *secs, double *pages_per_min)
{
    *secs = 0;
    *pages_per_min = 0;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT IFNULL(SUM(active_seconds),0),"
        " IFNULL(SUM(CASE WHEN pages_start IS NOT NULL AND pages_end > pages_start"
        "   THEN pages_end - pages_start END),0),"
        " IFNULL(SUM(CASE WHEN pages_start IS NOT NULL AND pages_end > pages_start"
        "   THEN active_seconds END),0)"
        " FROM sessions WHERE book_id = ?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bookid);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *secs = sqlite3_column_int64(st, 0);
            double pages = sqlite3_column_double(st, 1);
            double s = sqlite3_column_double(st, 2);
            if (s > 0)
                *pages_per_min = pages / (s / 60.0);
        }
    }
    sqlite3_finalize(st);
}
