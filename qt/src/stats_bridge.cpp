#include "stats_bridge.h"

#include <QUrl>
#include <QVariantList>

#include "installer.h"

extern "C" {
#include "daemon.h"
#include "stats_model.h"
}

namespace {

QString coverUrl(const char *path)
{
    return path && *path ? QUrl::fromLocalFile(QString::fromUtf8(path)).toString()
                         : QString();
}

QVariantMap bookMap(const bs_book &book)
{
    QVariantMap out;
    out[QStringLiteral("title")] = QString::fromUtf8(book.title);
    out[QStringLiteral("coverUrl")] = coverUrl(book.cover_path);
    out[QStringLiteral("secs")] = qlonglong(book.seconds);
    if (*book.date) {
        const QString iso = QString::fromLatin1(book.date);
        out[QStringLiteral("dateStr")] = iso.mid(8, 2) + QLatin1Char('.')
            + iso.mid(5, 2) + QLatin1Char('.');
    }
    return out;
}

QVariantMap autostartMap(const AutostartStatus &status)
{
    QVariantMap out;
    out[QStringLiteral("enabled")] = status.enabled;
    out[QStringLiteral("available")] = status.available;
    out[QStringLiteral("message")] = status.message;
    return out;
}

} // namespace

StatsBridge::StatsBridge(QObject *parent) : QObject(parent)
{
    bs_error error{};
    bs_context_open(&context_, stats_db_path(), explorer_db_path(), &error);
}

StatsBridge::~StatsBridge()
{
    bs_context_close(context_);
}

QVariantMap StatsBridge::autostartStatus()
{
    return autostartMap(::autostartStatus());
}

QVariantMap StatsBridge::overall()
{
    bs_overall stats{};
    bs_error error{};
    if (bs_load_overall(context_, &stats, &error) != 0)
        return {};
    QVariantMap out;
    out[QStringLiteral("todaySecs")] = stats.today_secs;
    out[QStringLiteral("todayPages")] = stats.today_pages;
    out[QStringLiteral("weekSecs")] = stats.week_secs;
    out[QStringLiteral("avgSessionMin")] = stats.avg_session_min;
    out[QStringLiteral("pagesPerMin")] = stats.pages_per_min;
    out[QStringLiteral("totalHours")] = stats.total_hours;
    out[QStringLiteral("booksTotal")] = stats.books_total;
    out[QStringLiteral("booksFinished")] = stats.books_finished;
    out[QStringLiteral("finishedFrac")] = stats.finished_fraction;
    out[QStringLiteral("streakDays")] = stats.streak_days;
    return out;
}

QVariantMap StatsBridge::currentBook()
{
    bs_current_book book{};
    bs_error error{};
    if (bs_load_current_book(context_, &book, &error) != 0)
        return {{QStringLiteral("ok"), false}};
    QVariantMap out;
    out[QStringLiteral("ok")] = book.ok != 0;
    if (!book.ok)
        return out;
    out[QStringLiteral("title")] = QString::fromUtf8(book.title);
    out[QStringLiteral("author")] = QString::fromUtf8(book.author);
    out[QStringLiteral("percent")] = book.percent;
    out[QStringLiteral("completed")] = book.completed != 0;
    out[QStringLiteral("coverUrl")] = coverUrl(book.cover_path);
    out[QStringLiteral("bookSecs")] = qlonglong(book.book_seconds);
    out[QStringLiteral("leftSecs")] = qlonglong(book.left_seconds);
    return out;
}

QVariantMap StatsBridge::year(int value)
{
    bs_year stats{};
    bs_error error{};
    if (bs_load_year(context_, value, &stats, &error) != 0)
        return {};
    QVariantList heat;
    heat.reserve(stats.days);
    for (int i = 0; i < stats.days; ++i)
        heat.append(stats.heat[i]);
    QVariantMap out;
    out[QStringLiteral("heat")] = heat;
    out[QStringLiteral("startWeekday")] = stats.start_weekday;
    out[QStringLiteral("ndays")] = stats.days;
    out[QStringLiteral("daysRead")] = stats.days_read;
    out[QStringLiteral("currentStreak")] = stats.current_streak;
    out[QStringLiteral("bestStreak")] = stats.best_streak;
    out[QStringLiteral("bestStreakStart")] =
        QString::fromLatin1(stats.best_streak_start);
    return out;
}

QVariantMap StatsBridge::yearBooks(int value)
{
    bs_year_books books{};
    bs_error error{};
    if (bs_load_year_books(context_, value, &books, &error) != 0)
        return {};
    QVariantList months;
    for (int month = 0; month < 12; ++month) {
        QVariantList entries;
        for (size_t i = 0; i < books.month_count[month]; ++i)
            entries.append(bookMap(books.month[month][i]));
        months.append(QVariant(entries));
    }
    QVariantMap out;
    out[QStringLiteral("months")] = months;
    out[QStringLiteral("total")] = books.total;
    bs_year_books_free(&books);
    return out;
}

QVariantMap StatsBridge::month(int yearValue, int monthValue)
{
    bs_month month{};
    bs_error error{};
    if (bs_load_month(context_, yearValue, monthValue, &month, &error) != 0)
        return {};
    QVariantList days;
    for (int day = 0; day < month.days; ++day) {
        QVariantList books;
        for (size_t i = 0; i < month.day[day].book_count; ++i)
            books.append(bookMap(month.day[day].books[i]));
        QVariantMap entry;
        entry[QStringLiteral("secs")] = qlonglong(month.day[day].seconds);
        entry[QStringLiteral("books")] = books;
        days.append(entry);
    }
    QVariantMap out;
    out[QStringLiteral("ndays")] = month.days;
    out[QStringLiteral("firstWeekday")] = month.first_weekday;
    out[QStringLiteral("days")] = days;
    bs_month_free(&month);
    return out;
}
