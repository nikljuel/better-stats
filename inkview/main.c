#include <inkview.h>

#include "autostart.h"
#include "daemon.h"
#include "stats_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { TAB_OVERVIEW, TAB_STREAK, TAB_CALENDAR, TAB_YEAR, TAB_COUNT };

typedef struct {
    bs_context *context;
    bs_error error;
    bs_overall overall;
    bs_current_book current;
    bs_year year;
    bs_month month;
    bs_year_books year_books;
    bs_autostart_status autostart;
    ifont *body;
    ifont *small;
    ifont *heading;
    ifont *large;
    int width;
    int height;
    int content_height;
    int tab;
    int year_value;
    int month_value;
    int german;
    int loaded_tab;
} app_state;

static app_state app;

static const char *months_de[] =
    {"Jan", "Feb", "Mär", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
static const char *months_en[] =
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *tabs_de[] = {"Übersicht", "Serie", "Kalender", "Jahr"};
static const char *tabs_en[] = {"Overview", "Streak", "Calendar", "Year"};

static const char *tr(const char *de, const char *en)
{
    return app.german ? de : en;
}

static const char *month_name(int month)
{
    return app.german ? months_de[month - 1] : months_en[month - 1];
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

static void separator(int y)
{
    DrawLine(24, y, app.width - 24, y, LGRAY);
}

static ibitmap *load_cover(const char *path, int width, int height)
{
    const char *dot;
    if (!path || !*path)
        return NULL;
    dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")))
        return LoadJPEG(path, width, height, 1, 1, 1);
    if (dot && !strcasecmp(dot, ".png"))
        return LoadPNGStretch(path, width, height, 1, 1);
    return LoadBitmap(path);
}

static void draw_cover(const char *path, const char *title,
                       int x, int y, int width, int height)
{
    ibitmap *bitmap = load_cover(path, width, height);
    if (bitmap) {
        DrawBitmapRect(x, y, width, height, bitmap, ALIGN_CENTER | VALIGN_MIDDLE);
        free(bitmap);
        return;
    }
    DrawRect(x, y, width, height, LGRAY);
    char initial[2] = {title && *title ? *title : '?', '\0'};
    set_font(app.large, DGRAY);
    text(x, y, width, height, initial, ALIGN_CENTER | VALIGN_MIDDLE);
}

static void draw_header(void)
{
    const int header_h = 58;
    const int tab_h = 54;
    set_font(app.heading, BLACK);
    text(24, 0, app.width - 96, header_h, "Better Stats",
         ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
    set_font(app.heading, BLACK);
    text(app.width - 72, 0, 48, header_h, "⚙",
         ALIGN_CENTER | VALIGN_MIDDLE);
    separator(header_h - 1);

    int i;
    for (i = 0; i < TAB_COUNT; ++i) {
        int x = i * app.width / TAB_COUNT;
        int width = (i + 1) * app.width / TAB_COUNT - x;
        set_font(i == app.tab ? app.body : app.small,
                 i == app.tab ? BLACK : DGRAY);
        text(x, header_h, width, tab_h, app.german ? tabs_de[i] : tabs_en[i],
             ALIGN_CENTER | VALIGN_MIDDLE | DOTS);
        if (i == app.tab)
            FillArea(x + width / 5, header_h + tab_h - 4,
                     width * 3 / 5, 4, BLACK);
    }
    DrawLine(0, header_h + tab_h - 1, app.width, header_h + tab_h - 1, LGRAY);
}

static int content_top(void)
{
    return 118;
}

static void metric(int x, int y, int width, int height,
                   const char *value, const char *label)
{
    DrawRect(x, y, width, height, LGRAY);
    set_font(app.heading, BLACK);
    text(x + 8, y + 8, width - 16, height / 2, value,
         ALIGN_CENTER | VALIGN_MIDDLE | DOTS);
    set_font(app.small, DGRAY);
    text(x + 8, y + height / 2, width - 16, height / 2 - 8, label,
         ALIGN_CENTER | VALIGN_MIDDLE | DOTS);
}

static void draw_overview(void)
{
    int top = content_top() + 18;
    int margin = 28;
    int cover_w = app.width / 4;
    int cover_h = cover_w * 3 / 2;
    draw_cover(app.current.cover_path, app.current.title,
               margin, top, cover_w, cover_h);
    int info_x = margin + cover_w + 24;
    int info_w = app.width - info_x - margin;
    set_font(app.heading, BLACK);
    text(info_x, top, info_w, 64,
         app.current.ok ? app.current.title : tr("Kein aktuelles Buch", "No current book"),
         ALIGN_LEFT | VALIGN_TOP | DOTS);
    set_font(app.body, DGRAY);
    text(info_x, top + 62, info_w, 42, app.current.author,
         ALIGN_LEFT | VALIGN_TOP | DOTS);
    if (app.current.ok) {
        char progress[64];
        snprintf(progress, sizeof(progress), "%d%%", app.current.percent);
        set_font(app.large, BLACK);
        text(info_x, top + 110, info_w, 54, progress,
             ALIGN_LEFT | VALIGN_MIDDLE);
        int bar_y = top + 170;
        DrawRect(info_x, bar_y, info_w, 20, LGRAY);
        FillArea(info_x + 2, bar_y + 2,
                 (info_w - 4) * app.current.percent / 100, 16, DGRAY);
        char left[64];
        char formatted[32];
        format_time(app.current.left_seconds, formatted);
        snprintf(left, sizeof(left), "%s: %s", tr("Verbleibend", "Remaining"),
                 formatted);
        set_font(app.small, DGRAY);
        text(info_x, bar_y + 30, info_w, 40, left,
             ALIGN_LEFT | VALIGN_TOP | DOTS);
    }

    int metrics_top = top + cover_h + 24;
    int gap = 12;
    int tile_w = (app.width - 2 * margin - gap) / 2;
    int tile_h = (app.content_height - metrics_top - 24 - gap) / 2;
    if (tile_h < 82)
        tile_h = 82;
    char value[64];
    format_time(app.overall.today_secs, value);
    metric(margin, metrics_top, tile_w, tile_h, value,
           tr("Heute gelesen", "Read today"));
    snprintf(value, sizeof(value), "%.0f %s", app.overall.avg_session_min,
             tr("Min", "min"));
    metric(margin + tile_w + gap, metrics_top, tile_w, tile_h, value,
           tr("Ø Sitzung", "Avg session"));
    snprintf(value, sizeof(value), "%.1f", app.overall.pages_per_min);
    metric(margin, metrics_top + tile_h + gap, tile_w, tile_h, value,
           tr("Seiten / Min", "Pages / min"));
    snprintf(value, sizeof(value), "%d / %d", app.overall.books_finished,
             app.overall.books_total);
    metric(margin + tile_w + gap, metrics_top + tile_h + gap,
           tile_w, tile_h, value, tr("Bücher beendet", "Books finished"));
}

static void draw_streak(void)
{
    int top = content_top() + 18;
    int margin = 28;
    char value[64];
    snprintf(value, sizeof(value), "%d", app.year.current_streak);
    metric(margin, top, (app.width - margin * 2 - 12) / 2, 112,
           value, tr("Aktuelle Serie", "Current streak"));
    snprintf(value, sizeof(value), "%d", app.year.best_streak);
    metric(app.width / 2 + 6, top, (app.width - margin * 2 - 12) / 2, 112,
           value, tr("Beste Serie", "Best streak"));

    int grid_top = top + 150;
    int cell = (app.width - margin * 2) / 54;
    if (cell < 5)
        cell = 5;
    int gap = cell > 7 ? 2 : 1;
    int i;
    for (i = 0; i < app.year.days; ++i) {
        int index = app.year.start_weekday + i;
        int x = margin + (index / 7) * cell;
        int y = grid_top + (index % 7) * cell;
        int color = app.year.heat[i] == 2 ? BLACK
                  : app.year.heat[i] == 1 ? DGRAY : LGRAY;
        FillArea(x, y, cell - gap, cell - gap, color);
    }
    set_font(app.body, BLACK);
    snprintf(value, sizeof(value), "%d %s", app.year.days_read,
             tr("Lesetage dieses Jahr", "reading days this year"));
    text(margin, grid_top + 7 * cell + 30, app.width - 2 * margin, 46,
         value, ALIGN_LEFT | VALIGN_MIDDLE);
    set_font(app.small, DGRAY);
    snprintf(value, sizeof(value), "%s: %s",
             tr("Längste Serie begann", "Longest streak began"),
             *app.year.best_streak_start ? app.year.best_streak_start : "–");
    text(margin, grid_top + 7 * cell + 76, app.width - 2 * margin, 46,
         value, ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
}

static void draw_calendar(void)
{
    int top = content_top() + 8;
    int margin = 20;
    char title[64];
    snprintf(title, sizeof(title), "%s %d", month_name(app.month_value),
             app.year_value);
    set_font(app.heading, BLACK);
    text(margin, top, app.width - 2 * margin, 48, title,
         ALIGN_CENTER | VALIGN_MIDDLE);
    static const char *week_de[] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
    static const char *week_en[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    int grid_top = top + 84;
    int cell_w = (app.width - 2 * margin) / 7;
    int rows = (app.month.first_weekday + app.month.days + 6) / 7;
    int cell_h = (app.content_height - grid_top - 24) / rows;
    int i;
    set_font(app.small, DGRAY);
    for (i = 0; i < 7; ++i)
        text(margin + i * cell_w, top + 48, cell_w, 32,
             app.german ? week_de[i] : week_en[i],
             ALIGN_CENTER | VALIGN_MIDDLE);
    for (i = 0; i < app.month.days; ++i) {
        int index = app.month.first_weekday + i;
        int x = margin + (index % 7) * cell_w;
        int y = grid_top + (index / 7) * cell_h;
        DrawRect(x, y, cell_w, cell_h, LGRAY);
        char day[8];
        snprintf(day, sizeof(day), "%d", i + 1);
        set_font(app.small, BLACK);
        text(x + 4, y + 2, 28, 26, day, ALIGN_LEFT | VALIGN_TOP);
        if (app.month.day[i].book_count) {
            bs_book *book = &app.month.day[i].books[0];
            draw_cover(book->cover_path, book->title,
                       x + cell_w / 3, y + 4, cell_w * 2 / 3 - 4, cell_h - 8);
            if (app.month.day[i].book_count > 1) {
                char count[16];
                snprintf(count, sizeof(count), "+%u",
                         (unsigned)(app.month.day[i].book_count - 1));
                set_font(app.small, BLACK);
                text(x + 2, y + cell_h - 28, cell_w / 3, 24, count,
                     ALIGN_CENTER | VALIGN_MIDDLE);
            }
        }
    }
}

static void draw_year(void)
{
    int top = content_top() + 18;
    int margin = 28;
    char heading[96];
    snprintf(heading, sizeof(heading), "%d %s %d", app.year_books.total,
             tr("Bücher beendet in", "books finished in"), app.year_value);
    set_font(app.heading, BLACK);
    text(margin, top, app.width - 2 * margin, 54, heading,
         ALIGN_LEFT | VALIGN_MIDDLE | DOTS);
    separator(top + 60);
    size_t max = 1;
    int i;
    for (i = 0; i < 12; ++i)
        if (app.year_books.month_count[i] > max)
            max = app.year_books.month_count[i];
    int row_top = top + 78;
    int row_h = (app.content_height - row_top - 18) / 12;
    for (i = 0; i < 12; ++i) {
        set_font(app.small, DGRAY);
        text(margin, row_top + i * row_h, 64, row_h,
             month_name(i + 1), ALIGN_LEFT | VALIGN_MIDDLE);
        int bar_x = margin + 68;
        int bar_w = app.width - bar_x - margin;
        FillArea(bar_x, row_top + i * row_h + 4, bar_w, row_h - 8, 0xeeeeee);
        if (app.year_books.month_count[i])
            FillArea(bar_x, row_top + i * row_h + 4,
                     (int)(bar_w * app.year_books.month_count[i] / max),
                     row_h - 8, LGRAY);
        char count[16];
        snprintf(count, sizeof(count), "%u",
                 (unsigned)app.year_books.month_count[i]);
        set_font(app.small, BLACK);
        text(bar_x + 8, row_top + i * row_h, bar_w - 16, row_h,
             count, ALIGN_RIGHT | VALIGN_MIDDLE);
    }
}

static void draw_error(void)
{
    set_font(app.heading, BLACK);
    text(32, content_top() + 60, app.width - 64, 60,
         tr("Statistiken konnten nicht geladen werden",
            "Statistics could not be loaded"),
         ALIGN_CENTER | VALIGN_MIDDLE);
    set_font(app.body, DGRAY);
    text(48, content_top() + 140, app.width - 96, 180,
         app.error.message, ALIGN_CENTER | VALIGN_TOP);
    set_font(app.small, DGRAY);
    text(48, content_top() + 330, app.width - 96, 80,
         tr("Details stehen in system/pbreadstats/app.log.",
            "Details are in system/pbreadstats/app.log."),
         ALIGN_CENTER | VALIGN_TOP);
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
        ok = bs_load_year(app.context, app.year_value, &app.year, &app.error) == 0;
    else if (app.tab == TAB_CALENDAR)
        ok = bs_load_month(app.context, app.year_value, app.month_value,
                           &app.month, &app.error) == 0;
    else
        ok = bs_load_year_books(app.context, app.year_value, &app.year_books,
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
    FullUpdate();
}

static void show_day(int day)
{
    if (day < 1 || day > app.month.days || !app.month.day[day - 1].book_count)
        return;
    char title[64];
    char body[1024] = "";
    char total[32];
    format_time(app.month.day[day - 1].seconds, total);
    snprintf(title, sizeof(title), "%d. %s · %s", day,
             month_name(app.month_value), total);
    size_t i;
    for (i = 0; i < app.month.day[day - 1].book_count; ++i) {
        char duration[32];
        format_time(app.month.day[day - 1].books[i].seconds, duration);
        size_t used = strlen(body);
        snprintf(body + used, sizeof(body) - used, "%s%s — %s",
                 used ? "\n" : "", app.month.day[day - 1].books[i].title,
                 duration);
    }
    Message(ICON_INFORMATION, title, body, 8000);
}

static void show_month(int month)
{
    if (month < 1 || month > 12 || !app.year_books.month_count[month - 1])
        return;
    char title[64];
    char body[1024] = "";
    snprintf(title, sizeof(title), "%s %d", month_name(month), app.year_value);
    size_t i;
    for (i = 0; i < app.year_books.month_count[month - 1]; ++i) {
        bs_book *book = &app.year_books.month[month - 1][i];
        size_t used = strlen(body);
        snprintf(body + used, sizeof(body) - used, "%s%s — %.5s",
                 used ? "\n" : "", book->title, book->date + 5);
    }
    Message(ICON_INFORMATION, title, body, 8000);
}

static void pointer_up(int x, int y)
{
    if (y < 58 && x > app.width - 88) {
        bs_autostart_get(&app.autostart);
        Message(ICON_INFORMATION, tr("Einstellungen", "Settings"),
                app.autostart.enabled
                    ? tr("Tracking startet automatisch beim Öffnen eines EPUBs.",
                         "Tracking starts automatically when an EPUB opens.")
                    : (*app.autostart.message ? app.autostart.message
                       : tr("Automatisches Tracking ist nicht aktiv.",
                            "Automatic tracking is not active.")),
                6000);
        return;
    }
    if (y >= 58 && y < content_top()) {
        app.tab = x * TAB_COUNT / app.width;
        if (app.tab >= TAB_COUNT)
            app.tab = TAB_COUNT - 1;
        draw();
        return;
    }
    if (app.tab == TAB_CALENDAR && app.error.code == 0) {
        int margin = 20;
        int grid_top = content_top() + 92;
        int cell_w = (app.width - 2 * margin) / 7;
        int rows = (app.month.first_weekday + app.month.days + 6) / 7;
        int cell_h = (app.content_height - grid_top - 24) / rows;
        if (x >= margin && x < app.width - margin && y >= grid_top) {
            int cell = (y - grid_top) / cell_h * 7 + (x - margin) / cell_w;
            show_day(cell - app.month.first_weekday + 1);
        }
    } else if (app.tab == TAB_YEAR && app.error.code == 0) {
        int top = content_top() + 18;
        int row_top = top + 78;
        int row_h = (app.content_height - row_top - 18) / 12;
        if (y >= row_top)
            show_month((y - row_top) / row_h + 1);
    }
}

static int handler(int type, int par1, int par2)
{
    if (type == EVT_INIT) {
        app.width = ScreenWidth();
        app.height = ScreenHeight();
        app.content_height = app.height - PanelHeight();
        const char *language = currentLang();
        app.german = !language || !*language || !strncmp(language, "de", 2);
        const char *family = iv_get_default_font(FONT_FAMILY);
        app.small = OpenFont(family, 18, 1);
        app.body = OpenFont(family, 22, 1);
        app.heading = OpenFont(iv_get_default_font(FONT_BOLD), 28, 1);
        app.large = OpenFont(iv_get_default_font(FONT_BOLD), 40, 1);
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        app.year_value = local.tm_year + 1900;
        app.month_value = local.tm_mon + 1;
        app.loaded_tab = -1;
        bs_context_open(&app.context, stats_db_path(), explorer_db_path(), &app.error);
        bs_autostart_set(1, &app.autostart);
        return 1;
    }
    if (type == EVT_SHOW || type == EVT_REPAINT) {
        draw();
        return 1;
    }
    if (type == EVT_POINTERUP) {
        pointer_up(par1, par2);
        return 1;
    }
    if (type == EVT_KEYUP) {
        if (par1 == IV_KEY_BACK || par1 == IV_KEY_HOME) {
            CloseApp();
            return 1;
        }
        if (par1 == IV_KEY_LEFT || par1 == IV_KEY_PREV || par1 == IV_KEY_PREV2) {
            app.tab = (app.tab + TAB_COUNT - 1) % TAB_COUNT;
            draw();
            return 1;
        }
        if (par1 == IV_KEY_RIGHT || par1 == IV_KEY_NEXT || par1 == IV_KEY_NEXT2) {
            app.tab = (app.tab + 1) % TAB_COUNT;
            draw();
            return 1;
        }
    }
    if (type == EVT_EXIT) {
        bs_month_free(&app.month);
        bs_year_books_free(&app.year_books);
        bs_context_close(app.context);
        if (app.small)
            CloseFont(app.small);
        if (app.body)
            CloseFont(app.body);
        if (app.heading)
            CloseFont(app.heading);
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
