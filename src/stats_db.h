#ifndef STATS_DB_H
#define STATS_DB_H

#include <stdint.h>
#include <sqlite3.h>

typedef struct {
    double total_hours;
    int session_count;
    double avg_session_min;
    double pages_per_min;      /* nur aus Sessions mit bekannten Seiten */
    int books_total, books_finished;
    int streak_days;
    int today_secs, today_pages;
    int week_secs, week_pages;
} overall_stats;

typedef struct {
    char title[96];
    int first_day, last_day;   /* 1-based within the month */
} month_book;

int stats_overall(sqlite3 *db, overall_stats *o);
/* Active seconds per day of a month; secs[1..31]. Returns days in month. */
int stats_month(sqlite3 *db, int year, int mon, int secs[32]);
int stats_month_books(sqlite3 *db, int year, int mon, month_book *out, int max);
/* Total time + speed for a book (speed <= 0 if unknown). */
void stats_book(sqlite3 *db, int64_t bookid, int64_t *secs, double *pages_per_min);

#endif
