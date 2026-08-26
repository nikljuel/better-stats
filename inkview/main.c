#include <inkview.h>

#include "autostart.h"
#include "daemon.h"
#include "stats_model.h"
#include "updater.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#pragma weak DrawApplicationCaption
#pragma weak GetCaptionHeight
#pragma weak InitPanel
#pragma weak LoadPNG8
#pragma weak SetApplicationCaptionHeight
#pragma weak SetPanelType

enum { TAB_OVERVIEW, TAB_STREAK, TAB_CALENDAR, TAB_YEAR, TAB_COUNT };
enum { DIALOG_NONE, DIALOG_DAY, DIALOG_MONTH };

typedef struct {
    bs_context *context;
    bs_error error;
    bs_overall overall;
    bs_current_book current;
    bs_year year;
    bs_month month;
    bs_year_books year_books;
    bs_autostart_status autostart;
    bs_update_info update;
    ifont *tiny;
    ifont *small;
    ifont *body;
    ifont *heading;
    ifont *metric;
    ifont *large;
    int width;
    int height;
    int content_height;
    int caption_height;
    int tab_height;
    int tab;
    int current_year;
    int current_month;
    int calendar_year;
    int calendar_month;
    int german;
    int loaded_tab;
    int dialog;
    int dialog_index;
    int auto_update_scheduled;
} app_state;

typedef struct {
    int x;
    int y;
    int w;
    int h;
    int close_x;
    int close_y;
    int close_w;
    int close_h;
    int columns;
} dialog_layout;

typedef struct {
    char path[BS_PATH_MAX];
    int width;
    int height;
    ibitmap *bitmap;
} cover_cache;

static app_state app;
static cover_cache cached_cover;

static const char *months_de[] =
    {"Jan", "Feb", "Mär", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
static const char *months_en[] =
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *months_full_de[] =
    {"Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August",
     "September", "Oktober", "November", "Dezember"};
static const char *months_full_en[] =
    {"January", "February", "March", "April", "May", "June", "July", "August",
     "September", "October", "November", "December"};
static const char *weekdays_de[] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
static const char *weekdays_en[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
static const char *tabs_de[] = {"Übersicht", "Serie", "Kalender", "Jahr"};
static const char *tabs_en[] = {"Overview", "Streak", "Calendar", "Year"};

static int dp(int logical)
{
    return (logical * app.width + 316) / 632;
}

static int imin(int a, int b)
{
    return a < b ? a : b;
}

static int imax(int a, int b)
{
    return a > b ? a : b;
}

static const char *tr(const char *de, const char *en)
{
    return app.german ? de : en;
}

static const char *month_name(int month)
{
    return app.german ? months_de[month - 1] : months_en[month - 1];
}

static const char *month_full_name(int month)
{
    return app.german ? months_full_de[month - 1] : months_full_en[month - 1];
}

static void format_time(int64_t seconds, char out[32])
{
    int hours = (int)(seconds / 3600);
    int minutes = (int)((seconds % 3600) / 60);
    if (hours)
        snprintf(out, 32, "%dh %02dm", hours, minutes);
    else
        snprintf(out, 32, "%d %s", minutes, tr("Min", "min"));
}

static void set_font(ifont *font, int color)
{
    SetFont(font, color);
}

static void text(int x, int y, int width, int height, const char *value,
                 int flags)
{
    DrawTextRect(x, y, width, height, value ? value : "", flags);
}

static void fill_circle(int center_x, int center_y, int radius, int color)
{
    int y;
    int radius_squared = radius * radius;
    for (y = -radius; y <= radius; ++y) {
        int x = radius;
        while (x > 0 && x * x + y * y > radius_squared)
            --x;
        FillArea(center_x - x, center_y + y, x * 2 + 1, 1, color);
    }
}

static void fill_round_rect(int x, int y, int width, int height,
                            int radius, int color)
{
    radius = imin(radius, imin(width, height) / 2);
    if (radius <= 0) {
        FillArea(x, y, width, height, color);
        return;
    }
    FillArea(x + radius, y, width - radius * 2, height, color);
    FillArea(x, y + radius, width, height - radius * 2, color);
    fill_circle(x + radius, y + radius, radius, color);
    fill_circle(x + width - radius - 1, y + radius, radius, color);
    fill_circle(x + radius, y + height - radius - 1, radius, color);
    fill_circle(x + width - radius - 1, y + height - radius - 1, radius, color);
}

static void draw_settings_icon(int center_x, int center_y)
{
    static const signed char teeth[][2] = {
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
        {1, 0}, {-1, 1}, {0, 1}, {1, 1}
    };
    int tooth = dp(3);
    int offset = dp(8);
    size_t i;
    for (i = 0; i < sizeof(teeth) / sizeof(teeth[0]); ++i)
        FillArea(center_x + teeth[i][0] * offset - tooth / 2,
                 center_y + teeth[i][1] * offset - tooth / 2,
                 tooth, tooth, BLACK);
    fill_circle(center_x, center_y, dp(7), BLACK);
    fill_circle(center_x, center_y, dp(3), WHITE);
}

static void draw_donut(int center_x, int center_y, int radius,
                       int thickness, double fraction)
{
    int degree;
    int inner = radius - thickness;
    if (fraction < 0.0)
        fraction = 0.0;
    if (fraction > 1.0)
        fraction = 1.0;
    for (degree = 0; degree < 360; ++degree) {
        double angle = (degree - 90) * 3.14159265358979323846 / 180.0;
        int color = degree < (int)(fraction * 360.0 + 0.5)
            ? BLACK : 0xd8d8d8;
        DrawLine(center_x + (int)(cos(angle) * inner),
                 center_y + (int)(sin(angle) * inner),
                 center_x + (int)(cos(angle) * radius),
                 center_y + (int)(sin(angle) * radius), color);
    }
}

static void separator(int y)
{
    int margin = dp(28);
    DrawLine(margin, y, app.width - margin, y, BLACK);
}

static ibitmap *load_cover(const char *path, int width, int height)
{
    const char *dot;
    if (!path || !*path)
        return NULL;
    if (!strcmp(cached_cover.path, path)
        && cached_cover.width == width && cached_cover.height == height)
        return cached_cover.bitmap;
    free(cached_cover.bitmap);
    memset(&cached_cover, 0, sizeof(cached_cover));
    snprintf(cached_cover.path, sizeof(cached_cover.path), "%s", path);
    cached_cover.width = width;
    cached_cover.height = height;
    dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")))
        cached_cover.bitmap = LoadJPEG(path, width, height, 100, 100, 0);
    else if (dot && !strcasecmp(dot, ".png"))
        cached_cover.bitmap = LoadPNG8
            ? LoadPNG8(path)
            : LoadPNGStretch(path, width, height, 0, DITHER_PATTERN);
    else
        cached_cover.bitmap = LoadBitmap(path);
    return cached_cover.bitmap;
}

static int draw_cover(const char *path, const char *title,
                      int x, int y, int width, int height)
{
    width &= ~1;
    height &= ~1;
    ibitmap *bitmap = load_cover(path, width, height);
    if (bitmap) {
        DrawBitmapRect(x, y, width, height, bitmap,
                       ALIGN_FIT | ALIGN_CENTER | VALIGN_MIDDLE);
        return 1;
    }
    DrawRect(x, y, width, height, LGRAY);
    char initial[2] = {title && *title ? *title : '?', '\0'};
    set_font(app.heading, DGRAY);
    text(x, y, width, height, initial, ALIGN_CENTER | VALIGN_MIDDLE);
    return 0;
}

static int content_top(void)
{
    return app.caption_height + app.tab_height;
}

static void draw_header(void)
{
    int caption = app.caption_height;
    int tab_top = caption;
    if (DrawApplicationCaption) {
        irect title = {dp(56), 0, app.width - dp(112), caption, 0};
        DrawApplicationCaption("Better Stats", &title);
    } else {
        set_font(app.heading, BLACK);
        text(0, 0, app.width, caption, "Better Stats",
             ALIGN_CENTER | VALIGN_MIDDLE | DOTS);
    }
    set_font(app.body, BLACK);
    draw_settings_icon(app.width - dp(28), caption / 2);
    DrawLine(0, caption - 1, app.width, caption - 1, LGRAY);

    int i;
    for (i = 0; i < TAB_COUNT; ++i) {
        int x = i * app.width / TAB_COUNT;
        int width = (i + 1) * app.width / TAB_COUNT - x;
        set_font(i == app.tab ? app.heading : app.body,
                 i == app.tab ? BLACK : 0x888888);
        text(x, tab_top, width, app.tab_height,
             app.german ? tabs_de[i] : tabs_en[i],
             ALIGN_CENTER | VALIGN_MIDDLE | DOTS);
        if (i == app.tab)
            FillArea(x + width * 9 / 40,
                     tab_top + app.tab_height - dp(4),
                     width * 22 / 40, dp(4), BLACK);
    }
    DrawLine(0, tab_top + app.tab_height - 1, app.width,
             tab_top + app.tab_height - 1, LGRAY);
}

static void draw_metric_column(int x, int y, int width,
                               const char *value, const char *label)
{
    set_font(app.metric, BLACK);
    text(x, y, width - dp(8), dp(40), value,
         ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
    set_font(app.small, 0x888888);
    text(x, y + dp(42), width - dp(8), dp(44), label,
         ALIGN_LEFT | VALIGN_TOP);
}

static void draw_overview(void)
{
    int margin = dp(28);
    int top = content_top() + dp(32);
    int cover_w = dp(110);
    int cover_h = dp(165);
    int info_x = margin + cover_w + dp(20);
    int info_w = app.width - info_x - margin;

    if (app.current.ok) {
        draw_cover(app.current.cover_path, app.current.title,
                   margin, top, cover_w, cover_h);
        set_font(app.heading, BLACK);
        text(info_x, top, info_w, dp(48), app.current.title,
             ALIGN_LEFT | VALIGN_TOP | DOTS);
        set_font(app.small, 0x888888);
        text(info_x, top + dp(48), info_w, dp(34), app.current.author,
             ALIGN_LEFT | VALIGN_TOP | DOTS);

        char value[64];
        snprintf(value, sizeof(value), "%s: %d %%",
                 tr("Fortschritt", "Progress"), app.current.percent);
        set_font(app.body, BLACK);
        text(info_x, top + dp(86), info_w, dp(34), value,
             ALIGN_LEFT | VALIGN_MIDDLE);
        int bar_y = top + dp(126);
        int bar_h = dp(10);
        fill_round_rect(info_x, bar_y, info_w, bar_h, bar_h / 2, 0xeeeeee);
        int progress_w = info_w * app.current.percent / 100;
        if (progress_w > 0)
            fill_round_rect(info_x, bar_y, progress_w, bar_h,
                            imin(bar_h / 2, progress_w / 2), BLACK);

        char formatted[32];
        format_time(app.current.book_seconds, formatted);
        snprintf(value, sizeof(value), "%s: %s", tr("Gelesen", "Read"), formatted);
        set_font(app.small, 0x888888);
        text(info_x, bar_y + dp(18), info_w, dp(28), value,
             ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
        if (app.current.left_seconds > 0) {
            format_time(app.current.left_seconds, formatted);
            snprintf(value, sizeof(value), "%s %s", tr("Noch ca.", "About"), formatted);
            text(info_x, bar_y + dp(48), info_w, dp(28), value,
                 ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
        }
    } else {
        set_font(app.body, 0x888888);
        text(margin, top, app.width - margin * 2, dp(60),
             tr("Noch kein Buch geöffnet", "No book opened yet"),
             ALIGN_LEFT | VALIGN_MIDDLE);
    }

    int first_separator = top + cover_h + dp(20);
    separator(first_separator);
    int metrics_top = first_separator + dp(20);
    int column_w = (app.width - margin * 2) / 3;
    char value[64];
    format_time(app.overall.today_secs, value);
    draw_metric_column(margin, metrics_top, column_w, value,
                       tr("Gelesen heute", "Read today"));
    snprintf(value, sizeof(value), "%.0f", app.overall.avg_session_min);
    draw_metric_column(margin + column_w, metrics_top, column_w, value,
                       tr("Min pro Session", "Min per session"));
    snprintf(value, sizeof(value), "%.1f", app.overall.pages_per_min);
    draw_metric_column(margin + column_w * 2, metrics_top, column_w, value,
                       tr("Seiten pro Minute", "Pages per minute"));

    int second_separator = metrics_top + dp(96);
    separator(second_separator);
    set_font(app.small, 0x888888);
    text(margin, second_separator + dp(16), app.width - margin * 2, dp(30),
         tr("ALLE BÜCHER", "ALL BOOKS"), ALIGN_LEFT | VALIGN_MIDDLE);

    int all_top = second_separator + dp(52);
    int donut_size = dp(110);
    int donut_center_x = margin + donut_size / 2;
    int donut_center_y = all_top + donut_size / 2;
    draw_donut(donut_center_x, donut_center_y, donut_size / 2 - dp(2),
               dp(13), app.overall.finished_fraction);
    snprintf(value, sizeof(value), "%d %%",
             (int)(app.overall.finished_fraction * 100.0 + 0.5));
    set_font(app.heading, BLACK);
    text(margin, all_top, donut_size, donut_size, value,
         ALIGN_CENTER | VALIGN_MIDDLE);
    set_font(app.small, 0x888888);
    text(margin, all_top + donut_size + dp(8), column_w - dp(8), dp(48),
         tr("deiner Bücher beendet", "of your books finished"),
         ALIGN_LEFT | VALIGN_TOP);

    int number_y = all_top + donut_size / 2 - dp(24);
    snprintf(value, sizeof(value), "%d", app.overall.books_finished);
    draw_metric_column(margin + column_w, number_y, column_w, value,
                       tr("Bücher beendet", "Books finished"));
    snprintf(value, sizeof(value), "%.1f", app.overall.total_hours);
    draw_metric_column(margin + column_w * 2, number_y, column_w, value,
                       tr("Stunden gesamt", "Total hours"));
}

static void streak_insight(char out[256])
{
    int year = 0;
    int month = 0;
    int day = 0;
    if (app.year.best_streak <= 0
        || sscanf(app.year.best_streak_start, "%d-%d-%d", &year, &month, &day) != 3
        || month < 1 || month > 12) {
        snprintf(out, 256, "%s",
                 tr("Noch keine Lese-Serie — heute ist ein guter Tag, um eine zu starten.",
                    "No reading streak yet — today is a good day to start one."));
        return;
    }
    if (app.german)
        snprintf(out, 256, "Deine längste Serie begann am %d. %s und hielt %d %s.",
                 day, month_full_name(month), app.year.best_streak,
                 app.year.best_streak == 1 ? "Tag" : "Tage");
    else
        snprintf(out, 256, "Your longest streak began on %s %d and lasted %d %s.",
                 month_full_name(month), day, app.year.best_streak,
                 app.year.best_streak == 1 ? "day" : "days");
}

static int month_first_day(int month, int leap)
{
    static const int starts[] =
        {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    return starts[month - 1] + (leap && month > 2);
}

static void draw_streak(void)
{
    int margin = dp(28);
    int top = content_top() + dp(10);
    int half = (app.width - margin * 2) / 2;
    char value[256];

    snprintf(value, sizeof(value), "%d", app.year.current_streak);
    set_font(app.large, BLACK);
    text(margin, top, half, dp(54), value, ALIGN_LEFT | VALIGN_MIDDLE);
    snprintf(value, sizeof(value), "%d", app.year.best_streak);
    text(margin + half, top, half, dp(54), value, ALIGN_LEFT | VALIGN_MIDDLE);
    set_font(app.small, 0x888888);
    text(margin, top + dp(56), half - dp(8), dp(38),
         tr("Tage aktuelle Serie", "Days current streak"), ALIGN_LEFT | VALIGN_TOP);
    snprintf(value, sizeof(value), "%s %d",
             tr("Tage beste Serie", "Days best streak"), app.current_year);
    text(margin + half, top + dp(56), half - dp(8), dp(38), value,
         ALIGN_LEFT | VALIGN_TOP);

    streak_insight(value);
    set_font(app.body, BLACK);
    text(margin, top + dp(102), app.width - margin * 2, dp(56), value,
         ALIGN_LEFT | VALIGN_TOP);
    snprintf(value, sizeof(value), "%d %s %d", app.year.days_read,
             tr("LESETAGE IN", "READING DAYS IN"), app.current_year);
    set_font(app.small, 0x888888);
    text(margin, top + dp(166), app.width - margin * 2, dp(30), value,
         ALIGN_LEFT | VALIGN_MIDDLE);

    int legend_y = app.content_height - dp(38);
    int heat_top = top + dp(210);
    int heat_bottom = legend_y - dp(14);
    int heat_height = heat_bottom - heat_top;
    int label_h = dp(26);
    int block_gap = dp(10);
    int width = app.width - margin * 2;
    int cell_w = width * 100 / (2700 + 26 * 18);
    int cell_h = (((heat_height - block_gap) / 2 - label_h) * 100)
               / (700 + 6 * 18);
    int cell = imax(dp(4), imin(cell_w, cell_h));
    int gap = imax(1, cell * 18 / 100);
    int block_h = 7 * cell + 6 * gap + label_h;
    int i;
    for (i = 0; i < app.year.days; ++i) {
        int slot = app.year.start_weekday + i;
        int week = slot / 7;
        int x = margin + (week % 27) * (cell + gap);
        int y = heat_top + (week >= 27 ? block_h + block_gap : 0)
              + (slot % 7) * (cell + gap);
        int state = app.year.heat[i];
        fill_round_rect(x, y, cell, cell, imax(1, cell / 7),
                        state ? BLACK : 0xd8d8d8);
        if (state == 2)
            fill_circle(x + cell / 2, y + cell / 2,
                        imax(dp(2), cell * 15 / 100), WHITE);
    }
    int leap = app.year.days == 366;
    set_font(app.tiny, 0x888888);
    for (i = 0; i < 12; ++i) {
        int week = (month_first_day(i + 1, leap) + app.year.start_weekday) / 7;
        int x = margin + (week % 27) * (cell + gap);
        int y = heat_top + (week >= 27 ? block_h + block_gap : 0)
              + 7 * cell + 6 * gap + dp(4);
        text(x, y, dp(38), label_h, month_name(i + 1),
             ALIGN_LEFT | VALIGN_TOP | DOTS);
    }

    int legend_x = margin;
    int legend_cell = dp(16);
    fill_round_rect(legend_x, legend_y, legend_cell, legend_cell,
                    dp(3), 0xd8d8d8);
    set_font(app.small, 0x888888);
    text(legend_x + legend_cell + dp(6), legend_y - dp(5), dp(84), dp(28),
         tr("nicht gelesen", "not read"), ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
    legend_x += dp(112);
    fill_round_rect(legend_x, legend_y, legend_cell, legend_cell, dp(3), BLACK);
    text(legend_x + legend_cell + dp(6), legend_y - dp(5), dp(62), dp(28),
         tr("gelesen", "read"), ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
    legend_x += dp(88);
    fill_round_rect(legend_x, legend_y, legend_cell, legend_cell, dp(3), BLACK);
    fill_circle(legend_x + legend_cell / 2, legend_y + legend_cell / 2,
                dp(3), WHITE);
    text(legend_x + legend_cell + dp(6), legend_y - dp(5), dp(112), dp(28),
         tr("Buch beendet", "book finished"),
         ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
}

static int calendar_rows(void)
{
    return (app.month.first_weekday + app.month.days + 6) / 7;
}

static int calendar_grid_top(void)
{
    return content_top() + dp(58) + dp(30) + dp(6);
}

static void draw_calendar(void)
{
    int margin = dp(28);
    int top = content_top();
    int month_h = dp(58);
    int weekdays_h = dp(30);
    char title[64];
    snprintf(title, sizeof(title), "%s %d", month_full_name(app.calendar_month),
             app.calendar_year);

    set_font(app.heading, BLACK);
    text(0, top, app.width, month_h, title, ALIGN_CENTER | VALIGN_MIDDLE | DOTS);
    set_font(app.heading, BLACK);
    DrawSymbol(margin, top + month_h / 2 - dp(9), ARROW_LEFT);
    DrawSymbol(app.width - margin - dp(18), top + month_h / 2 - dp(9),
               ARROW_RIGHT);

    int cell_w = (app.width - margin * 2) / 7;
    int i;
    set_font(app.small, 0x888888);
    for (i = 0; i < 7; ++i)
        text(margin + i * cell_w, top + month_h, cell_w, weekdays_h,
             app.german ? weekdays_de[i] : weekdays_en[i],
             ALIGN_CENTER | VALIGN_MIDDLE);

    int grid_top = calendar_grid_top();
    int rows = calendar_rows();
    int cell_h = (app.content_height - grid_top - dp(16)) / rows;
    for (i = 0; i < app.month.days; ++i) {
        int index = app.month.first_weekday + i;
        int x = margin + (index % 7) * cell_w;
        int y = grid_top + (index / 7) * cell_h;
        DrawRect(x, y, cell_w, cell_h, LGRAY);
        char day[8];
        snprintf(day, sizeof(day), "%d", i + 1);
        set_font(app.small, BLACK);
        text(x + dp(5), y + dp(3), dp(28), dp(26), day,
             ALIGN_LEFT | VALIGN_TOP);
        if (app.month.day[i].book_count) {
            bs_book *book = &app.month.day[i].books[0];
            int cover_x = x + dp(30);
            int cover_y = y + dp(3);
            int cover_w = cell_w - dp(33);
            int cover_h = cell_h - dp(6);
            draw_cover(book->cover_path, book->title,
                       cover_x, cover_y, cover_w, cover_h);
            if (app.month.day[i].book_count > 1) {
                int badge = dp(26);
                int badge_x = x + cell_w - badge / 2 - dp(2);
                int badge_y = y + cell_h - badge / 2 - dp(2);
                fill_circle(badge_x, badge_y, badge / 2, BLACK);
                char count[16];
                snprintf(count, sizeof(count), "+%u",
                         (unsigned)(app.month.day[i].book_count - 1));
                set_font(app.tiny, WHITE);
                text(badge_x - badge / 2, badge_y - badge / 2,
                     badge, badge, count, ALIGN_CENTER | VALIGN_MIDDLE);
            }
        }
    }
}

static void draw_year(void)
{
    int margin = dp(28);
    int top = content_top() + dp(10);
    int month_count = app.current_month;
    char value[96];
    snprintf(value, sizeof(value), "%d", app.year_books.total);
    set_font(app.large, BLACK);
    text(margin, top, dp(58), dp(54), value, ALIGN_LEFT | VALIGN_MIDDLE);
    snprintf(value, sizeof(value), "%s %d",
             tr("Bücher beendet in", "Books finished in"), app.current_year);
    set_font(app.body, 0x888888);
    text(margin + dp(36), top, app.width - margin * 2 - dp(36), dp(54), value,
         ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
    separator(top + dp(60));

    size_t max = 1;
    int i;
    for (i = 0; i < month_count; ++i)
        if (app.year_books.month_count[i] > max)
            max = app.year_books.month_count[i];
    int rows_top = top + dp(70);
    int rows_height = app.content_height - rows_top - dp(14);
    int row_h = rows_height / month_count;
    int label_w = dp(52);
    int bar_x = margin + label_w;
    int bar_w = app.width - bar_x - margin;
    int bar_h = imin(dp(44), row_h - dp(8));
    for (i = 0; i < month_count; ++i) {
        int row_y = rows_top + i * row_h;
        int bar_y = row_y + (row_h - bar_h) / 2;
        set_font(app.small, 0x888888);
        text(margin, row_y, label_w, row_h, month_name(i + 1),
             ALIGN_LEFT | VALIGN_MIDDLE);
        fill_round_rect(bar_x, bar_y, bar_w, bar_h, dp(6), 0xeeeeee);
        if (app.year_books.month_count[i]) {
            int fill_w = (int)(bar_w * app.year_books.month_count[i] / max);
            fill_round_rect(bar_x, bar_y, fill_w, bar_h,
                            imin(dp(6), fill_w / 2), 0xc4c4c4);
            int cover_h = bar_h - dp(6);
            int cover_w = cover_h * 7 / 10;
            int cover_x = bar_x + dp(6);
            size_t book;
            for (book = 0; book < app.year_books.month_count[i]; ++book) {
                if (cover_x + cover_w > bar_x + bar_w - dp(3))
                    break;
                draw_cover(app.year_books.month[i][book].cover_path,
                           app.year_books.month[i][book].title,
                           cover_x, bar_y + dp(3), cover_w, cover_h);
                cover_x += cover_w + dp(4);
            }
        }
    }
}

static bs_book *dialog_books(size_t *count)
{
    if (app.dialog == DIALOG_DAY && app.dialog_index >= 0
        && app.dialog_index < app.month.days) {
        *count = app.month.day[app.dialog_index].book_count;
        return app.month.day[app.dialog_index].books;
    }
    if (app.dialog == DIALOG_MONTH && app.dialog_index >= 0
        && app.dialog_index < 12) {
        *count = app.year_books.month_count[app.dialog_index];
        return app.year_books.month[app.dialog_index];
    }
    *count = 0;
    return NULL;
}

static dialog_layout detail_dialog_layout(size_t count)
{
    dialog_layout out;
    int margin = dp(20);
    int width = imin(dp(560), app.width - margin * 2);
    int inner_width = width - dp(40);
    int tile_w = dp(100);
    int gap = dp(16);
    int columns = (inner_width + gap) / (tile_w + gap);
    if (columns < 1)
        columns = 1;
    int rows = (int)((count + columns - 1) / columns);
    if (rows < 1)
        rows = 1;
    int tile_h = dp(150) + dp(30);
    int height = dp(16) + dp(40) + dp(16)
               + rows * tile_h + (rows - 1) * gap + dp(20);
    int available = app.content_height - content_top();
    out.w = width;
    out.h = imin(height, available - dp(20));
    out.x = (app.width - out.w) / 2;
    out.y = content_top() + (available - out.h) / 2;
    out.close_w = dp(48);
    out.close_h = dp(48);
    out.close_x = out.x + out.w - dp(20) - out.close_w;
    out.close_y = out.y + dp(10);
    out.columns = columns;
    return out;
}

static void draw_detail_dialog(void)
{
    size_t count;
    bs_book *books = dialog_books(&count);
    if (!books || !count)
        return;
    dialog_layout layout = detail_dialog_layout(count);
    DimArea(0, content_top(), app.width,
            app.content_height - content_top(), 0x777777);
    FillArea(layout.x, layout.y, layout.w, layout.h, WHITE);
    DrawRect(layout.x, layout.y, layout.w, layout.h, BLACK);

    char title[128];
    if (app.dialog == DIALOG_DAY) {
        char total[32];
        format_time(app.month.day[app.dialog_index].seconds, total);
        snprintf(title, sizeof(title), "%d. %s  ·  %s",
                 app.dialog_index + 1, month_full_name(app.calendar_month), total);
    } else {
        snprintf(title, sizeof(title), "%s %d  ·  %u %s",
                 month_full_name(app.dialog_index + 1), app.current_year,
                 (unsigned)count, count == 1 ? tr("Buch", "book")
                                             : tr("Bücher", "books"));
    }
    set_font(app.heading, BLACK);
    text(layout.x + dp(20), layout.y + dp(16),
         layout.w - dp(40) - layout.close_w, dp(40), title,
         ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
    text(layout.close_x, layout.close_y, layout.close_w, layout.close_h, "X",
         ALIGN_CENTER | VALIGN_MIDDLE);

    int tile_w = dp(100);
    int cover_h = dp(150);
    int gap = dp(16);
    int tiles_top = layout.y + dp(72);
    size_t i;
    for (i = 0; i < count; ++i) {
        int column = (int)i % layout.columns;
        int row = (int)i / layout.columns;
        int x = layout.x + dp(20) + column * (tile_w + gap);
        int y = tiles_top + row * (cover_h + dp(30) + gap);
        if (y + cover_h + dp(28) > layout.y + layout.h - dp(8))
            break;
        draw_cover(books[i].cover_path, books[i].title,
                   x, y, tile_w, cover_h);
        char caption[32];
        if (app.dialog == DIALOG_DAY)
            format_time(books[i].seconds, caption);
        else if (strlen(books[i].date) == 10)
            snprintf(caption, sizeof(caption), "%.2s.%.2s.",
                     books[i].date + 8, books[i].date + 5);
        else
            caption[0] = '\0';
        set_font(app.small, BLACK);
        text(x, y + cover_h + dp(5), tile_w, dp(25), caption,
             ALIGN_CENTER | VALIGN_MIDDLE | DOTS);
    }
}

static void draw_error(void)
{
    set_font(app.heading, BLACK);
    text(dp(32), content_top() + dp(60), app.width - dp(64), dp(60),
         tr("Statistiken konnten nicht geladen werden",
            "Statistics could not be loaded"),
         ALIGN_CENTER | VALIGN_MIDDLE);
    set_font(app.body, DGRAY);
    text(dp(48), content_top() + dp(140), app.width - dp(96), dp(180),
         app.error.message, ALIGN_CENTER | VALIGN_TOP);
}

static int load_tab(void)
{
    if (app.loaded_tab == app.tab)
        return app.error.code == 0;
    memset(&app.error, 0, sizeof(app.error));
    if (app.loaded_tab == TAB_CALENDAR)
        bs_month_free(&app.month);
    if (app.loaded_tab == TAB_YEAR)
        bs_year_books_free(&app.year_books);
    app.loaded_tab = app.tab;
    int ok;
    if (app.tab == TAB_OVERVIEW)
        ok = bs_load_overall(app.context, &app.overall, &app.error) == 0
            && bs_load_current_book(app.context, &app.current, &app.error) == 0;
    else if (app.tab == TAB_STREAK)
        ok = bs_load_year(app.context, app.current_year, &app.year, &app.error) == 0;
    else if (app.tab == TAB_CALENDAR)
        ok = bs_load_month(app.context, app.calendar_year, app.calendar_month,
                           &app.month, &app.error) == 0;
    else
        ok = bs_load_year_books(app.context, app.current_year, &app.year_books,
                                &app.error) == 0;
    if (!ok)
        fprintf(stderr, "Better Stats: %s\n", app.error.message);
    return ok;
}

static void draw(void)
{
    ClearScreen();
    draw_header();
    if (!load_tab())
        draw_error();
    else if (app.tab == TAB_OVERVIEW)
        draw_overview();
    else if (app.tab == TAB_STREAK)
        draw_streak();
    else if (app.tab == TAB_CALENDAR)
        draw_calendar();
    else
        draw_year();
    if (app.dialog != DIALOG_NONE)
        draw_detail_dialog();
    FullUpdate();
}

static void shift_month(int delta)
{
    if (app.loaded_tab == TAB_CALENDAR) {
        bs_month_free(&app.month);
        app.loaded_tab = -1;
    }
    app.calendar_month += delta;
    if (app.calendar_month < 1) {
        app.calendar_month = 12;
        --app.calendar_year;
    } else if (app.calendar_month > 12) {
        app.calendar_month = 1;
        ++app.calendar_year;
    }
    app.dialog = DIALOG_NONE;
    draw();
}

static void show_day(int day)
{
    if (day < 1 || day > app.month.days
        || !app.month.day[day - 1].book_count)
        return;
    app.dialog = DIALOG_DAY;
    app.dialog_index = day - 1;
    draw();
}

static void show_month(int month)
{
    if (month < 1 || month > app.current_month
        || !app.year_books.month_count[month - 1])
        return;
    app.dialog = DIALOG_MONTH;
    app.dialog_index = month - 1;
    draw();
}

static const char *update_error_text(void)
{
    switch (app.update.error) {
    case BS_UPDATE_ERR_NETWORK:
        return tr("Keine WLAN-Verbindung.", "No Wi-Fi connection.");
    case BS_UPDATE_ERR_DOWNLOAD:
        return tr("Download fehlgeschlagen.", "Download failed.");
    case BS_UPDATE_ERR_RESPONSE:
        return tr("Release-Antwort ungültig.", "Invalid release response.");
    case BS_UPDATE_ERR_ASSET:
        return tr("Kein passendes Update-Paket gefunden.",
                  "No matching update package found.");
    case BS_UPDATE_ERR_CORRUPT:
        return tr("Update-Paket ist beschädigt.", "Update package is damaged.");
    case BS_UPDATE_ERR_UNSUPPORTED:
        return tr("Diese Firmware unterstützt WLAN-Updates nicht.",
                  "This firmware does not support Wi-Fi updates.");
    default:
        return tr("Update konnte nicht installiert werden.",
                  "The update could not be installed.");
    }
}

static void install_update(int automatic)
{
    ShowHourglass();
    int result = bs_update_install(&app.update);
    HideHourglass();
    if (result != 0) {
        if (!automatic)
            Message(ICON_WARNING, tr("Update", "Update"),
                    update_error_text(), 5000);
        return;
    }
    if (bs_update_restart() == 0) {
        CloseApp();
    } else if (!automatic) {
        Message(ICON_WARNING, tr("Update", "Update"),
                tr("Neustart fehlgeschlagen.", "Restart failed."), 5000);
    }
}

static void update_dialog_handler(int button)
{
    if (button == 1)
        install_update(0);
}

static void check_for_updates(int automatic)
{
    if (automatic
        && (!bs_update_auto_enabled() || !bs_update_network_connected()))
        return;
    ShowHourglass();
    int result = bs_update_check(&app.update, automatic ? 0 : 1);
    HideHourglass();
    if (result == BS_UPDATE_AVAILABLE) {
        if (automatic) {
            install_update(1);
        } else {
            char message[256];
            snprintf(message, sizeof(message),
                     tr("Version %s ist verfügbar.", "Version %s is available."),
                     app.update.latest_version);
            Dialog(ICON_QUESTION, tr("Update", "Update"), message,
                   tr("Installieren", "Install"), tr("Abbrechen", "Cancel"),
                   update_dialog_handler);
        }
    } else if (!automatic) {
        Message(result == BS_UPDATE_CURRENT ? ICON_INFORMATION : ICON_WARNING,
                tr("Update", "Update"),
                result == BS_UPDATE_CURRENT
                    ? tr("Better Stats ist aktuell.", "Better Stats is up to date.")
                    : update_error_text(),
                5000);
    }
}

static void show_settings(void);

static void settings_dialog_handler(int button)
{
    if (button == 1) {
        if (bs_update_set_auto_enabled(!bs_update_auto_enabled()) != 0)
            Message(ICON_WARNING, tr("Update", "Update"),
                    tr("Einstellung konnte nicht gespeichert werden.",
                       "The setting could not be saved."), 5000);
        else
            show_settings();
    } else if (button == 2) {
        check_for_updates(0);
    }
}

static void show_settings(void)
{
    bs_autostart_get(&app.autostart);
    bs_update_read_current(&app.update);
    char message[768];
    snprintf(message, sizeof(message), "%s\n\n%s: %s\n%s: %s",
             app.autostart.enabled
                 ? tr("Tracking startet automatisch beim Öffnen eines EPUBs.",
                      "Tracking starts automatically when an EPUB opens.")
                 : (*app.autostart.message ? app.autostart.message
                    : tr("Automatisches Tracking ist nicht aktiv.",
                         "Automatic tracking is not active.")),
             tr("Installiert", "Installed"),
             *app.update.current_version ? app.update.current_version : "–",
             tr("Automatische Updates", "Automatic updates"),
             bs_update_auto_enabled() ? tr("Ein", "On") : tr("Aus", "Off"));
    Dialog3(ICON_INFORMATION, tr("Einstellungen", "Settings"), message,
            bs_update_auto_enabled() ? tr("Ausschalten", "Turn off")
                                     : tr("Einschalten", "Turn on"),
            tr("Jetzt prüfen", "Check now"), tr("Schließen", "Close"),
            settings_dialog_handler);
}

static void automatic_update(void)
{
    check_for_updates(1);
}

static int point_in(int x, int y, int left, int top, int width, int height)
{
    return x >= left && x < left + width && y >= top && y < top + height;
}

static void pointer_up(int x, int y)
{
    if (app.dialog != DIALOG_NONE) {
        size_t count;
        dialog_books(&count);
        dialog_layout layout = detail_dialog_layout(count);
        if (!point_in(x, y, layout.x, layout.y, layout.w, layout.h)
            || point_in(x, y, layout.close_x, layout.close_y,
                        layout.close_w, layout.close_h)) {
            app.dialog = DIALOG_NONE;
            draw();
        }
        return;
    }
    if (y < app.caption_height && x > app.width - dp(70)) {
        show_settings();
        return;
    }
    if (y >= app.caption_height && y < content_top()) {
        app.tab = x * TAB_COUNT / app.width;
        if (app.tab >= TAB_COUNT)
            app.tab = TAB_COUNT - 1;
        app.dialog = DIALOG_NONE;
        draw();
        return;
    }
    if (app.tab == TAB_CALENDAR && app.error.code == 0) {
        if (y >= content_top() && y < content_top() + dp(58)) {
            if (x < app.width / 3)
                shift_month(-1);
            else if (x >= app.width * 2 / 3)
                shift_month(1);
            return;
        }
        int margin = dp(28);
        int grid_top = calendar_grid_top();
        int cell_w = (app.width - margin * 2) / 7;
        int cell_h = (app.content_height - grid_top - dp(16)) / calendar_rows();
        if (x >= margin && x < app.width - margin && y >= grid_top) {
            int cell = (y - grid_top) / cell_h * 7 + (x - margin) / cell_w;
            show_day(cell - app.month.first_weekday + 1);
        }
    } else if (app.tab == TAB_YEAR && app.error.code == 0) {
        int rows_top = content_top() + dp(80);
        int row_h = (app.content_height - rows_top - dp(14)) / app.current_month;
        if (y >= rows_top)
            show_month((y - rows_top) / row_h + 1);
    }
}

static int handler(int type, int par1, int par2)
{
    if (type == EVT_INIT) {
        if (SetPanelType)
            SetPanelType(PANEL_ENABLED);
        if (InitPanel)
            InitPanel();
        app.width = ScreenWidth();
        app.height = ScreenHeight();
        app.content_height = app.height - PanelHeight();
        app.caption_height = dp(58);
        app.tab_height = dp(58);
        if (SetApplicationCaptionHeight)
            SetApplicationCaptionHeight(app.caption_height);
        if (GetCaptionHeight) {
            int native_height = GetCaptionHeight();
            if (native_height >= dp(40) && native_height <= dp(80))
                app.caption_height = native_height;
        }
        const char *language = currentLang();
        app.german = !language || !*language || !strncmp(language, "de", 2);
        const char *family = iv_get_default_font(FONT_FAMILY);
        app.tiny = OpenFont(family, dp(14), 1);
        app.small = OpenFont(family, dp(16), 1);
        app.body = OpenFont(family, dp(18), 1);
        app.heading = OpenFont(iv_get_default_font(FONT_BOLD), dp(20), 1);
        app.metric = OpenFont(iv_get_default_font(FONT_BOLD), dp(30), 1);
        app.large = OpenFont(iv_get_default_font(FONT_BOLD), dp(40), 1);
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        app.current_year = local.tm_year + 1900;
        app.current_month = local.tm_mon + 1;
        app.calendar_year = app.current_year;
        app.calendar_month = app.current_month;
        app.loaded_tab = -1;
        app.dialog = DIALOG_NONE;
        bs_context_open(&app.context, stats_db_path(), explorer_db_path(), &app.error);
        bs_autostart_set(1, &app.autostart);
        bs_update_read_current(&app.update);
        return 1;
    }
    if (type == EVT_SHOW || type == EVT_REPAINT) {
        draw();
        if (!app.auto_update_scheduled) {
            app.auto_update_scheduled = 1;
            SetWeakTimer("betterstats-update", automatic_update, 750);
        }
        return 1;
    }
    if (type == EVT_POINTERUP) {
        pointer_up(par1, par2);
        return 1;
    }
    if (type == EVT_KEYUP) {
        if (par1 == IV_KEY_BACK && app.dialog != DIALOG_NONE) {
            app.dialog = DIALOG_NONE;
            draw();
            return 1;
        }
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME) {
            CloseApp();
            return 1;
        }
        if (par1 == IV_KEY_MENU) {
            show_settings();
            return 1;
        }
        if (app.tab == TAB_CALENDAR
            && (par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2)) {
            shift_month(-1);
            return 1;
        }
        if (app.tab == TAB_CALENDAR
            && (par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2)) {
            shift_month(1);
            return 1;
        }
        if (par1 == IV_KEY_LEFT || par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2) {
            app.tab = (app.tab + TAB_COUNT - 1) % TAB_COUNT;
            app.dialog = DIALOG_NONE;
            draw();
            return 1;
        }
        if (par1 == IV_KEY_RIGHT || par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2) {
            app.tab = (app.tab + 1) % TAB_COUNT;
            app.dialog = DIALOG_NONE;
            draw();
            return 1;
        }
    }
    if (type == EVT_EXIT) {
        free(cached_cover.bitmap);
        memset(&cached_cover, 0, sizeof(cached_cover));
        bs_month_free(&app.month);
        bs_year_books_free(&app.year_books);
        bs_context_close(app.context);
        if (app.tiny)
            CloseFont(app.tiny);
        if (app.small)
            CloseFont(app.small);
        if (app.body)
            CloseFont(app.body);
        if (app.heading)
            CloseFont(app.heading);
        if (app.metric)
            CloseFont(app.metric);
        if (app.large)
            CloseFont(app.large);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--daemon") == 0)
        return run_daemon();
    if (argc > 1 && strcmp(argv[1], "--prepare") == 0) {
        bs_autostart_status status;
        bs_autostart_set(1, &status);
        return status.enabled ? 0 : 1;
    }
    InkViewMain(handler);
    return 0;
}
