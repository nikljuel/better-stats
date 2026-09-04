#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>
#include <sqlite3.h>

#ifndef POLL_SECONDS
#define POLL_SECONDS 1
#endif
/* Ceiling on what one page of reading may be worth, borrowed from the
 * winst0niuss fork. Presence alone cannot tell reading from a book lying open
 * on an awake device; the pages actually turned can. Loose on purpose: the
 * measured pace is 90-103 s a page, so this never bites during real reading and
 * only caps the pathological case. */
#define SECONDS_PER_PAGE_CAP 300

/* Latest reading state from the firmware DB (explorer-3.db). */
typedef struct {
    int64_t bookid;
    int64_t opentime;    /* session start (last open) */
    int64_t position_ts; /* last position update */
    int cpage, npage, completed;
    int64_t completed_ts;
    char title[256];
    char author[256];
    char cover[128];     /* "<storageid><hex-fast_hash>" or "" */
} pb_state;

typedef struct {
    sqlite3 *stats;
    const char *explorer_path;
    int64_t cur_book, cur_open, cur_pos_ts, cur_end_ts;
    int64_t cur_row_base;    /* active_seconds the row already had when adopted */
    int64_t cur_row_present; /* presence total when the row was adopted */
    int64_t cur_last_present;/* presence total at the last persisted endpoint */
    int64_t cur_budget_used; /* page-budget seconds persisted before this counter */
    int cur_row_moved;       /* pages moved since, as distance -- back counts too */
    int cur_pages_last;      /* last cpage seen, to measure that distance */
    int cur_resume_page_start; /* persisted page before an offline page change */
    int64_t cur_row_start; /* start_time of the row we currently write to */
} tracker;

/* Opens/creates our own stats DB. 0 = ok. */
int tracker_init(tracker *t, const char *stats_path, const char *explorer_path);
/* Reads the most recently opened book state. 0 = ok, 1 = no book, <0 = error. */
int tracker_read_state(const char *explorer_path, pb_state *out);
/* Imports firmware metadata atomically, excluding the current open session. */
int tracker_recover(tracker *t, int64_t skip_bookid, int64_t skip_opentime);
/* Adopts persisted state without consuming the supplied position snapshot. */
int tracker_prepare(tracker *t, const pb_state *s, int64_t present);
/* Starts a new measured fragment after a pause or daemon restart. */
int tracker_resume(tracker *t, int64_t present, int64_t wall_time);
/* One observed firmware snapshot. present is cumulative counted BOOTTIME. */
int tracker_observe(tracker *t, const pb_state *s, int64_t present);
/* Writes the current session through end_time. Call before any code path that
 * would lose the in-memory presence counter (book switch, daemon exit). Safe
 * to call when no session is active. */
int tracker_flush(tracker *t, int64_t present, int64_t end_time);
void tracker_close(tracker *t);

#endif
