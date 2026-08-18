#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>
#include <sqlite3.h>

/* Active reading time: the gap between two position updates counts only up to
 * this cap. ponytail: flat 10 min, tune empirically on-device (depends on
 * whether the firmware writes position_ts per page turn or only on close). */
#define IDLE_CAP_SECONDS 600
/* Sanity cap for sessions reconstructed without a running daemon. The span
 * opentime..position_ts is pure wall clock and includes standby, so keep this
 * tight: recovered rows are an estimate, not measured reading time. */
#define RECOVERED_CAP_SECONDS (90 * 60)
#define POLL_SECONDS 30

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
    int64_t cur_row_start; /* start_time of the row we currently write to */
} tracker;

/* Opens/creates our own stats DB. 0 = ok. */
int tracker_init(tracker *t, const char *stats_path, const char *explorer_path);
/* Reads the most recently opened book state. 0 = ok, 1 = no book, <0 = error. */
int tracker_read_state(const char *explorer_path, pb_state *out);
/* Backfills sessions created while no daemon was running. */
int tracker_recover(tracker *t);
/* One poll tick: derives session progress from the state. */
int tracker_observe(tracker *t, const pb_state *s);
void tracker_close(tracker *t);

#endif
