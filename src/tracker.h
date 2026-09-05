#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>
#include <sqlite3.h>

#ifndef POLL_SECONDS
#define POLL_SECONDS 1
#endif
#ifndef TRACKER_CHECKPOINT_SECONDS
#define TRACKER_CHECKPOINT_SECONDS 60
#endif
/* Ceiling on what one page of reading may be worth, borrowed from the
 * winst0niuss fork. Presence alone cannot tell reading from a book lying open
 * on an awake device; the pages actually turned can. Loose on purpose: the
 * measured pace is 90-103 s a page, so this never bites during real reading and
 * only caps the pathological case. */
#define SECONDS_PER_PAGE_CAP 300
#define TRACKER_RETRY (-2)
#define TRACKER_BOOK_PATH_MAX 4096

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
    sqlite3 *explorer; /* lazy, read-only; avoids reopening firmware DB per poll */
    const char *explorer_path;
    uint64_t explorer_dev, explorer_ino;
    int cached_book_path_valid;
    int64_t cached_book_path_id;
    char cached_book_path[TRACKER_BOOK_PATH_MAX + 1];
    int64_t cur_book, cur_open, cur_pos_ts, cur_end_ts;
    int64_t cur_raw_pos_ts;
    int64_t cur_row_base;    /* active_seconds the row already had when adopted */
    int64_t cur_row_present; /* presence total when the row was adopted */
    int64_t cur_last_present;/* presence total at the last persisted endpoint */
    int64_t cur_budget_used; /* page-budget seconds persisted before this counter */
    int64_t cur_row_durable; /* active_seconds currently durable for this row */
    int cur_row_moved;       /* pages moved since, as distance -- back counts too */
    int cur_pages_last;      /* last cpage seen, to measure that distance */
    int cur_resume_page_start; /* persisted page before an offline page change */
    int64_t cur_row_start; /* start_time of the row we currently write to */
    int cur_durable_page, cur_durable_moved;

    pb_state last_input;
    int have_last_input;

    int have_clock_sample;
    int rtc_rollback, rtc_rebase_allowed, wall_forward_jump;
    int64_t last_raw_wall, last_clock_boot, rtc_floor, rtc_catchup;
    int last_error; /* original SQLite status from the last failed mutator */
} tracker;

/* Opens/creates our own stats DB. 0 = ok, TRACKER_RETRY = transient DB error. */
int tracker_init(tracker *t, const char *stats_path, const char *explorer_path);
/* Reads the most recently opened book state. 0 = ok, 1 = no book, <0 = error. */
int tracker_read_state(const char *explorer_path, pb_state *out);
/* Reads one unambiguous state for bookid. expected_open wins; marker_started
 * next selects one row within +/-5 seconds; otherwise exactly one open row is
 * required. 0 = ok, 1 = absent/ambiguous, <0 = error. */
int tracker_read_book_state(const char *explorer_path, int64_t bookid,
                            int64_t expected_open, int64_t marker_started,
                            pb_state *out);
/* Resolves an exact absolute dirname + basename to one distinct positive ID.
 * 0 = ok, 1 = absent/ambiguous, <0 = error. */
int tracker_book_id_for_path(const char *explorer_path, const char *path,
                             int64_t *bookid);
/* Daemon variants reuse one read-only explorer connection while its inode is
 * unchanged; errors close it so the next poll performs a clean reopen. */
int tracker_cached_read_book_state(tracker *t, int64_t bookid,
                                   int64_t expected_open,
                                   int64_t marker_started, pb_state *out);
int tracker_cached_book_id_for_path(tracker *t, const char *path,
                                    int64_t *bookid);
void tracker_invalidate_book_path_cache(tracker *t);
/* Imports firmware metadata atomically, excluding the current open session. */
int tracker_recover(tracker *t, int64_t skip_bookid, int64_t skip_opentime);
/* present is cumulative credited reading time; boot_time is cumulative
 * CLOCK_BOOTTIME in seconds. Adopts the exact firmware session and snapshot.
 * 1 = a first row was created, 0 = an existing/prepared session, <0 = error. */
int tracker_prepare(tracker *t, const pb_state *s, int64_t present,
                    int64_t boot_time, int64_t raw_wall);
/* Starts a new measured fragment after a pause or daemon restart. */
int tracker_resume(tracker *t, int64_t present, int64_t boot_time,
                   int64_t raw_wall);
/* One snapshot for the already prepared firmware session. Never auto-adopts. */
int tracker_observe(tracker *t, const pb_state *s, int64_t present,
                    int64_t boot_time, int64_t raw_wall);
/* True when at least one checkpoint's worth of additional active time can be
 * made durable with the current page budget. */
int tracker_checkpoint_due(const tracker *t, int64_t present);
/* Writes the current session through end_time. Call before any code path that
 * would lose the in-memory presence counter (book switch, daemon exit). Safe
 * to call when no session is active. final_state may be NULL, but when present
 * must match the prepared firmware session and is persisted in the same Tx. */
int tracker_flush(tracker *t, const pb_state *final_state, int64_t present,
                  int64_t boot_time, int64_t raw_wall);
/* Ends a fragment without allowing newly credited time past max_end. Existing
 * durable timestamps are never moved backwards. Used only at a proven switch. */
int tracker_flush_bounded(tracker *t, const pb_state *final_state,
                          int64_t present, int64_t boot_time, int64_t raw_wall,
                          int64_t max_end);
int tracker_error_retryable(const tracker *t);
void tracker_close(tracker *t);

#ifdef TRACKER_TEST_API
/* Arithmetic seam for portable overflow/boundary tests. */
int tracker_test_mul_div_floor(int64_t a, int64_t b, int64_t divisor,
                               int64_t *out);
#endif

#endif
