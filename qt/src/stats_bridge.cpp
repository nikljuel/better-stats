#include "stats_bridge.h"

#include <QDate>
#include <QFile>
#include <QHash>
#include <QUrl>
#include <QVariantList>
#include <QVector>

#include "epub_cover.h"

extern "C" {
#include "daemon.h"
#include "stats_db.h"
#include "tracker.h"
}

StatsBridge::StatsBridge(QObject *parent) : QObject(parent)
{
    if (sqlite3_open(stats_db_path(), &db_) == SQLITE_OK)
        sqlite3_busy_timeout(db_, 2000);
}

StatsBridge::~StatsBridge()
{
    if (db_)
        sqlite3_close(db_);
}

namespace {

QString coverUrlFor(const QString &cover)
{
    if (cover.isEmpty())
        return QString();
    QString path = QStringLiteral(COVER_DIR "/%1.png").arg(cover);
    return QFile::exists(path) ? QUrl::fromLocalFile(path).toString()
                               : QString();
}

sqlite3 *openExplorer()
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(explorer_db_path(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 1000);
    return db;
}

/* Cover for a title: first extract from the EPUB (the firmware cache holds
 * ad pages instead of the cover for some Calibre books), else fall back to
 * the firmware cache. Returns a file:// URL, or empty. */
QString resolveCoverUrl(sqlite3 *expdb, const QString &title)
{
    if (!expdb || title.isEmpty())
        return QString();
    const char *sql =
        "SELECT IFNULL(fo.name,'') || '/' || f.filename,"
        " lower(hex(f.fast_hash)), f.storageid || lower(hex(f.fast_hash))"
        " FROM books_impl b JOIN files f ON f.book_id = b.id"
        " LEFT JOIN folders fo ON fo.id = f.folder_id"
        " WHERE lower(substr(b.title,1,12)) = lower(substr(?1,1,12))"
        " ORDER BY f.modification_time DESC, f.storageid ASC";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(expdb, sql, -1, &st, nullptr) != SQLITE_OK)
        return QString();
    sqlite3_bind_text(st, 1, title.toUtf8().constData(), -1, SQLITE_TRANSIENT);

    QString result;
    QStringList cacheCandidates;
    while (sqlite3_step(st) == SQLITE_ROW && result.isEmpty()) {
        const QString path = QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(st, 0)));
        const QString key = QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(st, 1)));
        cacheCandidates.append(QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(st, 2))));
        const QString extracted = epubCover(path, key);
        if (!extracted.isEmpty())
            result = QUrl::fromLocalFile(extracted).toString();
    }
    sqlite3_finalize(st);

    if (result.isEmpty()) {
        for (const QString &c : cacheCandidates) {
            const QString url = coverUrlFor(c);
            if (!url.isEmpty())
                return url;
        }
    }
    return result;
}

struct FinishedBook {
    QDate day;
    QString title;
    QString coverUrl;
};

/* Finished books straight from the firmware DB: completed_ts is set by the
 * firmware exactly on "mark as read" (DB trigger). */
QList<FinishedBook> finishedBooks()
{
    QList<FinishedBook> out;
    sqlite3 *db = openExplorer();
    if (!db)
        return out;
    /* Dedupe by title prefix: the same books appear multiple times in
     * books_impl after delete/re-add. MIN(completed_ts) = first real finish
     * date. ponytail: prefix heuristic, exact matching only if needed. */
    const char *sql =
        "SELECT date(MIN(s.completed_ts),'unixepoch','localtime'),"
        " MIN(IFNULL(b.title,'?'))"
        " FROM books_settings s JOIN books_impl b ON b.id = s.bookid"
        " WHERE s.completed = 1 AND s.completed_ts > 0"
        " GROUP BY lower(substr(IFNULL(b.title,'?'),1,12))"
        " ORDER BY 1"; /* oldest first */
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            FinishedBook fb;
            fb.day = QDate::fromString(
                QString::fromUtf8(reinterpret_cast<const char *>(
                    sqlite3_column_text(st, 0))),
                Qt::ISODate);
            fb.title = QString::fromUtf8(
                reinterpret_cast<const char *>(sqlite3_column_text(st, 1)));
            if (fb.day.isValid())
                out.append(fb);
        }
    }
    sqlite3_finalize(st);
    for (FinishedBook &fb : out)
        fb.coverUrl = resolveCoverUrl(db, fb.title);
    sqlite3_close(db);
    return out;
}

} // namespace

QVariantMap StatsBridge::overall()
{
    overall_stats o;
    stats_overall(db_, &o);
    QVariantMap m;
    m[QStringLiteral("todaySecs")] = o.today_secs;
    m[QStringLiteral("todayPages")] = o.today_pages;
    m[QStringLiteral("weekSecs")] = o.week_secs;
    m[QStringLiteral("avgSessionMin")] = o.avg_session_min;
    m[QStringLiteral("pagesPerMin")] = o.pages_per_min;
    m[QStringLiteral("totalHours")] = o.total_hours;
    /* Book counts deduplicated from the firmware DB rather than the tracking
     * DB (which counts each file copy separately). Total = current library. */
    int finished = finishedBooks().size();
    int total = 0;
    if (sqlite3 *exp = openExplorer()) {
        sqlite3_stmt *st = nullptr;
        const char *sql =
            "SELECT COUNT(DISTINCT lower(substr(IFNULL(b.title,'?'),1,12)))"
            " FROM books_impl b JOIN files f ON f.book_id = b.id";
        if (sqlite3_prepare_v2(exp, sql, -1, &st, nullptr) == SQLITE_OK
                && sqlite3_step(st) == SQLITE_ROW)
            total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        sqlite3_close(exp);
    }
    if (total < finished)
        total = finished; /* finished books whose file was deleted */
    m[QStringLiteral("booksTotal")] = total;
    m[QStringLiteral("booksFinished")] = finished;
    m[QStringLiteral("finishedFrac")] =
        total > 0 ? double(finished) / total : 0.0;
    m[QStringLiteral("streakDays")] = o.streak_days;
    return m;
}

QVariantMap StatsBridge::currentBook()
{
    QVariantMap m;
    pb_state s;
    if (tracker_read_state(explorer_db_path(), &s) != 0) {
        m[QStringLiteral("ok")] = false;
        return m;
    }
    m[QStringLiteral("ok")] = true;
    m[QStringLiteral("title")] = QString::fromUtf8(s.title);
    m[QStringLiteral("author")] = QString::fromUtf8(s.author);
    double prog = s.completed ? 1.0
                              : (s.npage > 0 ? double(s.cpage) / s.npage : 0.0);
    m[QStringLiteral("percent")] = int(prog * 100 + 0.5);
    m[QStringLiteral("completed")] = s.completed != 0;

    QString coverUrl;
    if (sqlite3 *exp = openExplorer()) {
        coverUrl = resolveCoverUrl(exp, QString::fromUtf8(s.title));
        sqlite3_close(exp);
    }
    if (coverUrl.isEmpty())
        coverUrl = coverUrlFor(QString::fromUtf8(s.cover));
    m[QStringLiteral("coverUrl")] = coverUrl;

    int64_t bsecs = 0;
    double bppm = 0;
    stats_book(db_, s.bookid, &bsecs, &bppm);
    if (bppm <= 0) {
        overall_stats o;
        stats_overall(db_, &o);
        bppm = o.pages_per_min;
    }
    m[QStringLiteral("bookSecs")] = qlonglong(bsecs);
    qlonglong left = 0;
    if (bppm > 0 && s.npage > s.cpage && !s.completed)
        left = qlonglong((s.npage - s.cpage) / bppm * 60.0);
    m[QStringLiteral("leftSecs")] = left;
    return m;
}


QVariantMap StatsBridge::year(int y)
{
    QVariantMap m;
    const QDate jan1(y, 1, 1);
    const int ndays = jan1.daysInYear();

    QVector<int> heat(ndays, 0);

    /* Lesetage */
    sqlite3_stmt *st = nullptr;
    const char *readSql =
        "SELECT DISTINCT date(end_time,'unixepoch','localtime') FROM sessions"
        " WHERE active_seconds > 0"
        " AND strftime('%Y', end_time,'unixepoch','localtime') = printf('%04d',?1)";
    if (sqlite3_prepare_v2(db_, readSql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, y);
        while (sqlite3_step(st) == SQLITE_ROW) {
            QDate d = QDate::fromString(
                QString::fromUtf8(reinterpret_cast<const char *>(
                    sqlite3_column_text(st, 0))),
                Qt::ISODate);
            if (d.isValid() && d.year() == y)
                heat[d.dayOfYear() - 1] = 1;
        }
    }
    sqlite3_finalize(st);

    /* Mark finish days (native marking from the firmware DB) */
    const QList<FinishedBook> finished = finishedBooks();
    for (const FinishedBook &fb : finished) {
        if (fb.day.year() == y)
            heat[fb.day.dayOfYear() - 1] = 2;
    }

    /* Streaks */
    int daysRead = 0, best = 0, run = 0, current = 0;
    QDate bestStart;
    for (int i = 0; i < ndays; i++) {
        if (heat[i] > 0) {
            daysRead++;
            run++;
            if (run > best) {
                best = run;
                bestStart = jan1.addDays(i - run + 1);
            }
        } else {
            run = 0;
        }
    }
    const QDate today = QDate::currentDate();
    if (today.year() == y) {
        int idx = today.dayOfYear() - 1;
        /* current streak counts up to today; a still-empty today does not
         * break the streak */
        if (heat[idx] == 0)
            idx--;
        while (idx >= 0 && heat[idx] > 0) {
            current++;
            idx--;
        }
    }

    QVariantList heatList;
    for (int i = 0; i < ndays; i++)
        heatList.append(heat[i]);
    m[QStringLiteral("heat")] = heatList;
    m[QStringLiteral("startWeekday")] = jan1.dayOfWeek() - 1; /* 0 = Montag */
    m[QStringLiteral("ndays")] = ndays;
    m[QStringLiteral("daysRead")] = daysRead;
    m[QStringLiteral("currentStreak")] = current;
    m[QStringLiteral("bestStreak")] = best;
    m[QStringLiteral("bestStreakStart")] =
        bestStart.isValid() ? bestStart.toString(Qt::ISODate) : QString();
    return m;
}

QVariantMap StatsBridge::yearBooks(int y)
{
    QVariantMap m;
    QVector<QVariantList> perMonth(13);
    int total = 0;

    const QList<FinishedBook> finished = finishedBooks();
    for (const FinishedBook &fb : finished) {
        if (fb.day.year() != y)
            continue;
        QVariantMap b;
        b[QStringLiteral("title")] = fb.title;
        b[QStringLiteral("coverUrl")] = fb.coverUrl;
        b[QStringLiteral("dateStr")] = fb.day.toString(QStringLiteral("dd.MM."));
        perMonth[fb.day.month()].append(b);
        total++;
    }

    QVariantList months;
    for (int i = 1; i <= 12; i++)
        months.append(QVariant(perMonth[i]));
    m[QStringLiteral("months")] = months;
    m[QStringLiteral("total")] = total;
    return m;
}

QVariantMap StatsBridge::month(int year, int mon)
{
    QVariantMap m;
    const QDate first(year, mon, 1);
    const int ndays = first.daysInMonth();
    m[QStringLiteral("ndays")] = ndays;
    m[QStringLiteral("firstWeekday")] = first.dayOfWeek() - 1; /* 0 = Montag */

    /* Per day: books with reading time, sorted descending. */
    QVector<QVariantList> perDay(ndays + 1);
    QVector<qlonglong> daySecs(ndays + 1, 0);
    /* GROUP BY title prefix instead of book_id: the same books exist as
     * several file copies and would otherwise appear twice. */
    const char *sql =
        "SELECT CAST(strftime('%d', s.end_time,'unixepoch','localtime') AS INTEGER),"
        " MIN(IFNULL(b.title,'?')), MAX(IFNULL(b.cover,'')), SUM(s.active_seconds)"
        " FROM sessions s LEFT JOIN books b ON b.book_id = s.book_id"
        " WHERE strftime('%Y-%m', s.end_time,'unixepoch','localtime')"
        "   = printf('%04d-%02d',?1,?2) AND s.active_seconds >= 60"
        " GROUP BY 1, lower(substr(IFNULL(b.title,'?'),1,12))"
        " ORDER BY 1, 4 DESC";
    sqlite3 *exp = openExplorer();
    QHash<QString, QString> coverCache; /* title -> URL, once per call */
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, year);
        sqlite3_bind_int(st, 2, mon);
        while (sqlite3_step(st) == SQLITE_ROW) {
            int d = sqlite3_column_int(st, 0);
            if (d < 1 || d > ndays)
                continue;
            QVariantMap b;
            const QString title = QString::fromUtf8(
                reinterpret_cast<const char *>(sqlite3_column_text(st, 1)));
            b[QStringLiteral("title")] = title;
            const QString cover = QString::fromUtf8(
                reinterpret_cast<const char *>(sqlite3_column_text(st, 2)));
            if (!coverCache.contains(title)) {
                QString url = resolveCoverUrl(exp, title);
                if (url.isEmpty())
                    url = coverUrlFor(cover);
                coverCache.insert(title, url);
            }
            b[QStringLiteral("coverUrl")] = coverCache.value(title);
            qlonglong secs = sqlite3_column_int64(st, 3);
            b[QStringLiteral("secs")] = secs;
            perDay[d].append(b);
            daySecs[d] += secs;
        }
    }
    sqlite3_finalize(st);
    if (exp)
        sqlite3_close(exp);

    QVariantList days;
    for (int d = 1; d <= ndays; d++) {
        QVariantMap e;
        e[QStringLiteral("secs")] = daySecs[d];
        e[QStringLiteral("books")] = perDay[d];
        days.append(e);
    }
    m[QStringLiteral("days")] = days;
    return m;
}
