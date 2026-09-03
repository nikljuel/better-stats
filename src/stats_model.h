#ifndef STATS_MODEL_H
#define STATS_MODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BS_TITLE_MAX 256
#define BS_AUTHOR_MAX 256
#define BS_PATH_MAX 512
#define BS_MESSAGE_MAX 192

typedef struct bs_context bs_context;

typedef struct {
    int code;
    char message[BS_MESSAGE_MAX];
} bs_error;

typedef struct {
    char title[BS_TITLE_MAX];
    char author[BS_AUTHOR_MAX];
    char cover_path[BS_PATH_MAX];
    char source_path[BS_PATH_MAX];
    char date[11];
    int64_t seconds;
} bs_book;

typedef struct {
    int today_secs;
    int today_pages;
    int week_secs;
    double avg_session_min;
    double pages_per_min;
    double total_hours;
    int books_total;
    int books_finished;
    double finished_fraction;
    int streak_days;
} bs_overall;

typedef struct {
    int ok;
    char title[BS_TITLE_MAX];
    char author[BS_AUTHOR_MAX];
    char cover_path[BS_PATH_MAX];
    int percent;
    int completed;
    int64_t book_seconds;
    int64_t left_seconds;
} bs_current_book;

typedef struct {
    int heat[366];
    int days;
    int start_weekday; /* Monday = 0 */
    int days_read;
    int current_streak;
    int best_streak;
    char best_streak_start[11];
} bs_year;

typedef struct {
    int64_t seconds;
    bs_book *books;
    size_t book_count;
} bs_month_day;

typedef struct {
    int days;
    int first_weekday; /* Monday = 0 */
    bs_month_day day[31];
} bs_month;

typedef struct {
    bs_book *month[12];
    size_t month_count[12];
    int total;
} bs_year_books;

int bs_context_open(bs_context **out, const char *stats_path,
                    const char *explorer_path, bs_error *error);
void bs_context_close(bs_context *context);

int bs_load_overall(bs_context *context, bs_overall *out, bs_error *error);
int bs_load_current_book(bs_context *context, bs_current_book *out,
                         bs_error *error);
int bs_load_year(bs_context *context, int year, bs_year *out, bs_error *error);
int bs_load_month(bs_context *context, int year, int month, bs_month *out,
                  bs_error *error);
int bs_load_year_books(bs_context *context, int year, bs_year_books *out,
                       bs_error *error);

typedef struct {
    bs_current_book *books;
    size_t count;
} bs_reading_list;

int bs_load_reading_books(bs_context *context, bs_reading_list *out,
                          bs_error *error);
void bs_reading_list_free(bs_reading_list *list);

typedef struct {
    char title[BS_TITLE_MAX];
    int64_t start_time;
    int64_t end_time;
    int64_t active_seconds;
    int pages_start;
    int pages_end;
    int pages_moved;
    int pages_known;
} bs_session;

typedef struct {
    bs_session *sessions;
    size_t count;
} bs_session_list;

int bs_load_today_sessions(bs_context *context, bs_session_list *out,
                           bs_error *error);
void bs_session_list_free(bs_session_list *list);

void bs_month_free(bs_month *month);
void bs_year_books_free(bs_year_books *year);

#ifdef __cplusplus
}
#endif

#endif
