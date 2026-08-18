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

int stats_overall(sqlite3 *db, overall_stats *o);
/* Total time + speed for a book (speed <= 0 if unknown). */
void stats_book(sqlite3 *db, int64_t bookid, int64_t *secs, double *pages_per_min);

#endif
