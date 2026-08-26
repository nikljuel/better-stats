#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>
#include <sqlite3.h>

/* Ceiling for sessions nobody watched: tracker_recover() backfills from the
 * firmware's two endpoints alone, with no idea how much of the span the device
 * spent switched off. Observed sessions are NOT capped -- their off-time is
 * measured, so capping them would only truncate genuine long reading. */
#define SESSION_CAP_SECONDS (90 * 60)
#define POLL_SECONDS 30
/* How long a gap between two daemon loops may be and still count as the daemon
 * having been continuously present. Reading time is the sum of such gaps: the
 * firmware never reports page turns, but a locked device wakes only every few
 * minutes while a device being read runs the loop every few seconds, so
 * presence is the signal. Measured on a PB710: 4-6 runs/min while reading
 * against 0.6 while locked. Derived from the poll interval, not fitted to one
 * device -- an awake daemon comes round at least every POLL_SECONDS. */
#define PRESENCE_GAP_SECONDS (4 * POLL_SECONDS)
/* Ceiling on what one page of reading may be worth, borrowed from the
 * winst0niuss fork. Presence alone cannot tell reading from a book lying open
 * on an awake device; the pages actually turned can. Loose on purpose: the
 * measured pace is 90-103 s a page, so this never bites during real reading and
 * only caps the pathological case. */
#define SECONDS_PER_PAGE_CAP 300
/* Settle time after a library write before reading it back; also breaks the
 * self-trigger loop our own read would otherwise cause. */
#define DEBOUNCE_SECONDS 2

/* Latest reading state from the firmware DB (explorer-3.db). */
typedef struct {
    int64_t bookid;
    int64_t opentime;    /* session start (last open) */
    int64_t position_ts; /* last position update */
    int cpage, npage, completed;
    char title[256];
    char author[256];
    char cover[128];     /* "<storageid><hex-fast_hash>" or "" */
} pb_state;

typedef struct {
    sqlite3 *stats;
    const char *explorer_path;
    int64_t cur_book, cur_open, cur_pos_ts;
    int64_t cur_row_present; /* presence total when the current row started */
    int cur_row_pages;       /* cpage when the current row started */
    int64_t cur_row_start; /* start_time of the row we currently write to */
} tracker;

/* Opens/creates our own stats DB. 0 = ok. */
int tracker_init(tracker *t, const char *stats_path, const char *explorer_path);
/* Reads the most recently opened book state. 0 = ok, 1 = no book, <0 = error. */
int tracker_read_state(const char *explorer_path, pb_state *out);
/* Backfills sessions created while no daemon was running. */
int tracker_recover(tracker *t);
/* One poll tick. present is the daemon's own cumulative count of seconds it was
 * demonstrably running (see PRESENCE_GAP_SECONDS). The firmware's endpoints
 * span standby as well as reading and cannot tell them apart, so the session's
 * time comes from this counter rather than from the endpoints. */
int tracker_observe(tracker *t, const pb_state *s, int64_t present);
void tracker_close(tracker *t);

#endif
