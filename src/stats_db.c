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
    /* recovered = 1 is a wall-clock estimate, not measured reading: it stays in
     * the totals but must not skew the derived metrics. */
    o->session_count = (int)q_double(db,
        "SELECT COUNT(*) FROM sessions WHERE active_seconds >= 60 AND recovered = 0");
    if (o->session_count > 0)
        o->avg_session_min = q_double(db,
            "SELECT AVG(active_seconds)/60.0 FROM sessions"
            " WHERE active_seconds >= 60 AND recovered = 0");
    double mins = q_double(db,
        "SELECT SUM(active_seconds)/60.0 FROM sessions"
        " WHERE pages_start IS NOT NULL AND pages_end > pages_start"
        " AND active_seconds > 0 AND recovered = 0");
    double pages = q_double(db,
        "SELECT SUM(pages_end - pages_start) FROM sessions"
        " WHERE pages_start IS NOT NULL AND pages_end > pages_start"
        " AND active_seconds > 0 AND recovered = 0");
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

    /* Streak: consecutive reading days up to today (or yesterday). */
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT date(end_time,'unixepoch','localtime') d FROM sessions"
        " GROUP BY d HAVING SUM(active_seconds) >= 60 ORDER BY d DESC LIMIT 366";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        /* Compare noon to noon: "now" would let a day-before-yesterday entry
         * still look like a 1.5-day gap shortly after midnight. */
        time_t now = time(NULL);
        struct tm tnow;
        localtime_r(&now, &tnow);
        tnow.tm_hour = 12;
        tnow.tm_min = 0;
        tnow.tm_sec = 0;
        tnow.tm_isdst = -1;
        int streak = 0;
        time_t expect = mktime(&tnow);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *d = (const char *)sqlite3_column_text(st, 0);
            struct tm tm = {0};
            if (!d || sscanf(d, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
                break;
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            tm.tm_hour = 12;
            tm.tm_isdst = -1;
            time_t day = mktime(&tm);
            double diff = difftime(expect, day) / 86400.0;
            if (streak == 0 && diff > 1.6)
                break; /* read neither today nor yesterday -> streak 0 */
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

void stats_book(sqlite3 *db, int64_t bookid, int64_t *secs, double *pages_per_min)
{
    *secs = 0;
    *pages_per_min = 0;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT IFNULL(SUM(active_seconds),0),"
        " IFNULL(SUM(CASE WHEN pages_start IS NOT NULL AND pages_end > pages_start"
        "   AND recovered = 0 THEN pages_end - pages_start END),0),"
        " IFNULL(SUM(CASE WHEN pages_start IS NOT NULL AND pages_end > pages_start"
        "   AND recovered = 0 THEN active_seconds END),0)"
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
