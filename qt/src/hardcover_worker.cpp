#include "hardcover_worker.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <sqlite3.h>

extern "C" {
#include "daemon.h" /* explorer_db_path(), STATS_DIR */
}

namespace {

QString colText(sqlite3_stmt *st, int i)
{
    const unsigned char *t = sqlite3_column_text(st, i);
    return t ? QString::fromUtf8(reinterpret_cast<const char *>(t)) : QString();
}

/* Hardcover's cached_image has shown up as either a plain URL string or a
 * JSON object with a "url" key - handle both rather than assume one. */
QString coverUrlFromJson(const QJsonValue &v)
{
    if (v.isString())
        return v.toString();
    if (v.isObject())
        return v.toObject().value(QStringLiteral("url")).toString();
    return QString();
}

/* Strips everything except digits and a trailing check-digit 'X' (valid for
 * ISBN-10), and uppercases that X. Real epub metadata very commonly stores
 * ISBNs in hyphenated human-readable form, but Hardcover's isbn_13/isbn_10
 * columns are plain digit strings - an exact match against a hyphenated
 * value silently returns nothing. */
QString normalizeIsbn(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (const QChar &c : raw) {
        if (c.isDigit())
            out.append(c);
        else if (c.toUpper() == QLatin1Char('X'))
            out.append(QLatin1Char('X'));
    }
    return out;
}

/* Applies an `editions { ... }` GraphQL result onto a link's cached display
 * fields. Does not touch sync state (userBookId, readId, progress, dates). */
void applyEditionJson(HardcoverLink *link, const QJsonObject &ed)
{
    link->editionId = qint64(ed.value(QStringLiteral("id")).toDouble());
    const QJsonObject book = ed.value(QStringLiteral("book")).toObject();
    link->hcBookId = qint64(book.value(QStringLiteral("id")).toDouble());

    const QString edTitle = ed.value(QStringLiteral("title")).toString();
    link->editionTitle = !edTitle.isEmpty() ? edTitle
                                             : book.value(QStringLiteral("title")).toString();
    link->editionIsbn10 = ed.value(QStringLiteral("isbn_10")).toString();
    link->editionIsbn13 = ed.value(QStringLiteral("isbn_13")).toString();
    link->editionPages = ed.value(QStringLiteral("pages")).toInt();
    link->editionCoverUrl = coverUrlFromJson(ed.value(QStringLiteral("cached_image")));
    link->readingFormatId = ed.value(QStringLiteral("reading_format_id")).toInt();
    link->editionFormat = ed.value(QStringLiteral("edition_format")).toString();
    link->editionPublisher = ed.value(QStringLiteral("publisher")).toObject()
                                 .value(QStringLiteral("name")).toString();
    link->editionReleaseDate = ed.value(QStringLiteral("release_date")).toString();
}

/* Matches the reference plugin's own extraction (hardcover/lib/ui/
 * search_dialog.lua): `contributions` (cached_contributors) is an array of
 * { author: { name: "..." } }. */
QString extractAuthorNames(const QJsonValue &contributions)
{
    QStringList names;
    if (contributions.isObject()) {
        const QString direct = contributions.toObject().value(QStringLiteral("author")).toString();
        if (!direct.isEmpty())
            names.append(direct);
    }
    if (contributions.isArray()) {
        for (const QJsonValue &c : contributions.toArray()) {
            const QString name = c.toObject().value(QStringLiteral("author")).toObject()
                                      .value(QStringLiteral("name")).toString();
            if (!name.isEmpty())
                names.append(name);
        }
    }
    return names.join(QStringLiteral(", "));
}

} // namespace

HardcoverWorker::HardcoverWorker(QObject *parent) : QObject(parent) {}
HardcoverWorker::~HardcoverWorker() = default;

void HardcoverWorker::setToken(const QString &token)
{
    token_ = token;
}

sqlite3 *HardcoverWorker::openLinkDb()
{
    sqlite3 *db = nullptr;
    const QString path = QStringLiteral(STATS_DIR "/hardcoversync.db");
    if (sqlite3_open(path.toUtf8().constData(), &db) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 2000);

    char *err = nullptr;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS links ("
        " book_id INTEGER PRIMARY KEY,"
        " isbn TEXT,"
        " hc_book_id INTEGER,"
        " edition_id INTEGER,"
        " user_book_id INTEGER,"
        " read_id INTEGER,"
        " started_at TEXT,"
        " finished_at TEXT,"
        " last_page INTEGER,"
        " last_status INTEGER,"
        " edition_title TEXT,"
        " edition_isbn10 TEXT,"
        " edition_isbn13 TEXT,"
        " edition_pages INTEGER,"
        " edition_cover_url TEXT,"
        " reading_format_id INTEGER,"
        " edition_format TEXT,"
        " edition_publisher TEXT,"
        " edition_release_date TEXT,"
        " edition_cover_local_path TEXT)",
        nullptr, nullptr, &err);
    if (err)
        sqlite3_free(err);
    /* Migrations for a links table already created before these columns
     * existed - fail silently (and harmlessly) if already there. Kept
     * here rather than at each call site, so every function using this
     * table can assume both columns already exist. */
    sqlite3_exec(db, "ALTER TABLE links ADD COLUMN edition_cover_local_path TEXT",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(db, "ALTER TABLE links ADD COLUMN pending_finish_confirm INTEGER DEFAULT 0",
                 nullptr, nullptr, nullptr);
    return db;
}

sqlite3 *HardcoverWorker::openExplorerReadOnly()
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(explorer_db_path(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 2000);
    return db;
}

HardcoverLink HardcoverWorker::loadLink(sqlite3 *db, qint64 bookId)
{
    HardcoverLink link;
    link.bookId = bookId;

    sqlite3_stmt *st = nullptr;
    const char *sql =
        "SELECT isbn, hc_book_id, edition_id, user_book_id, read_id, started_at,"
        " finished_at, last_page, last_status, edition_title, edition_isbn10,"
        " edition_isbn13, edition_pages, edition_cover_url, reading_format_id,"
        " edition_format, edition_publisher, edition_release_date,"
        " edition_cover_local_path"
        " FROM links WHERE book_id=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bookId);
        if (sqlite3_step(st) == SQLITE_ROW) {
            link.isbn = colText(st, 0);
            link.hcBookId = sqlite3_column_int64(st, 1);
            link.editionId = sqlite3_column_int64(st, 2);
            link.userBookId = sqlite3_column_int64(st, 3);
            link.readId = sqlite3_column_int64(st, 4);
            link.startedAt = colText(st, 5);
            link.finishedAt = colText(st, 6);
            link.lastPage = sqlite3_column_int(st, 7);
            link.lastStatus = sqlite3_column_int(st, 8);
            link.editionTitle = colText(st, 9);
            link.editionIsbn10 = colText(st, 10);
            link.editionIsbn13 = colText(st, 11);
            link.editionPages = sqlite3_column_int(st, 12);
            link.editionCoverUrl = colText(st, 13);
            link.readingFormatId = sqlite3_column_int(st, 14);
            link.editionFormat = colText(st, 15);
            link.editionPublisher = colText(st, 16);
            link.editionReleaseDate = colText(st, 17);
            link.editionCoverLocalPath = colText(st, 18);
        }
    }
    sqlite3_finalize(st);
    return link;
}

void HardcoverWorker::saveLink(sqlite3 *db, const HardcoverLink &link)
{
    sqlite3_stmt *st = nullptr;
    const char *sql =
        "INSERT INTO links (book_id, isbn, hc_book_id, edition_id, user_book_id,"
        " read_id, started_at, finished_at, last_page, last_status,"
        " edition_title, edition_isbn10, edition_isbn13, edition_pages,"
        " edition_cover_url, reading_format_id, edition_format,"
        " edition_publisher, edition_release_date, edition_cover_local_path)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20)"
        " ON CONFLICT(book_id) DO UPDATE SET isbn=excluded.isbn,"
        " hc_book_id=excluded.hc_book_id, edition_id=excluded.edition_id,"
        " user_book_id=excluded.user_book_id, read_id=excluded.read_id,"
        " started_at=excluded.started_at, finished_at=excluded.finished_at,"
        " last_page=excluded.last_page, last_status=excluded.last_status,"
        " edition_title=excluded.edition_title, edition_isbn10=excluded.edition_isbn10,"
        " edition_isbn13=excluded.edition_isbn13, edition_pages=excluded.edition_pages,"
        " edition_cover_url=excluded.edition_cover_url,"
        " reading_format_id=excluded.reading_format_id,"
        " edition_format=excluded.edition_format,"
        " edition_publisher=excluded.edition_publisher,"
        " edition_release_date=excluded.edition_release_date,"
        " edition_cover_local_path=excluded.edition_cover_local_path";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, link.bookId);
    sqlite3_bind_text(st, 2, link.isbn.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, link.hcBookId);
    sqlite3_bind_int64(st, 4, link.editionId);
    sqlite3_bind_int64(st, 5, link.userBookId);
    sqlite3_bind_int64(st, 6, link.readId);
    sqlite3_bind_text(st, 7, link.startedAt.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, link.finishedAt.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 9, link.lastPage);
    sqlite3_bind_int(st, 10, link.lastStatus);
    sqlite3_bind_text(st, 11, link.editionTitle.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 12, link.editionIsbn10.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 13, link.editionIsbn13.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 14, link.editionPages);
    sqlite3_bind_text(st, 15, link.editionCoverUrl.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 16, link.readingFormatId);
    sqlite3_bind_text(st, 17, link.editionFormat.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 18, link.editionPublisher.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 19, link.editionReleaseDate.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 20, link.editionCoverLocalPath.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

QJsonObject HardcoverWorker::graphql(const QString &query, const QJsonObject &variables,
                                     QString *errOut)
{
    QNetworkAccessManager mgr;
    QNetworkRequest req(QUrl(QStringLiteral("https://api.hardcover.app/v1/graphql")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Authorization", "Bearer " + token_.trimmed().toUtf8());

    QJsonObject body{{"query", query}, {"variables", variables}};
    QNetworkReply *reply = mgr.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeoutTimer.start(15000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        if (errOut)
            *errOut = QStringLiteral("request timed out");
        reply->deleteLater();
        return {};
    }

    const QByteArray raw = reply->readAll();
    const QNetworkReply::NetworkError netErr = reply->error();
    reply->deleteLater();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseErr);
    if (netErr != QNetworkReply::NoError && !doc.isObject()) {
        if (errOut)
            *errOut = QStringLiteral("network error");
        return {};
    }
    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("errors"))) {
        const QJsonArray errs = root.value(QStringLiteral("errors")).toArray();
        if (errOut && !errs.isEmpty())
            *errOut = errs.first().toObject().value(QStringLiteral("message")).toString();
        return {};
    }
    return root.value(QStringLiteral("data")).toObject();
}

int HardcoverWorker::fetchPrivacySetting()
{
    QString err;
    QJsonObject data = graphql(QStringLiteral(
        "query { me { id account_privacy_setting_id } }"), {}, &err);
    const QJsonArray me = data.value(QStringLiteral("me")).toArray();
    if (me.isEmpty())
        return 1;
    const int v = me.first().toObject().value(QStringLiteral("account_privacy_setting_id")).toInt();
    return v > 0 ? v : 1;
}

QByteArray HardcoverWorker::downloadBytes(const QString &url, QString *errOut)
{
    QNetworkAccessManager mgr;
    QNetworkRequest req((QUrl(url)));
    QNetworkReply *reply = mgr.get(req);

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeoutTimer.start(15000);
    loop.exec();

    if (!reply->isFinished()) {
        reply->abort();
        if (errOut)
            *errOut = QStringLiteral("cover download timed out");
        reply->deleteLater();
        return {};
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (errOut)
            *errOut = reply->errorString();
        reply->deleteLater();
        return {};
    }
    const QByteArray data = reply->readAll();
    reply->deleteLater();
    return data;
}

QString HardcoverWorker::cacheCoverLocally(qint64 bookId, const QString &remoteUrl)
{
    if (remoteUrl.isEmpty())
        return QString();

    QString err;
    const QByteArray data = downloadBytes(remoteUrl, &err);
    if (data.isEmpty())
        return QString();

    QDir().mkpath(QStringLiteral(STATS_DIR "/covers"));

    /* Guess an extension from the URL; default to jpg (the common case for
     * Hardcover's cover CDN) if nothing recognizable is found. */
    QString ext = QStringLiteral("jpg");
    const int dot = remoteUrl.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0 && remoteUrl.size() - dot <= 5) {
        const QString candidate = remoteUrl.mid(dot + 1).toLower();
        if (candidate == QStringLiteral("png") || candidate == QStringLiteral("jpg")
                || candidate == QStringLiteral("jpeg") || candidate == QStringLiteral("webp"))
            ext = candidate;
    }

    const QString path = QStringLiteral(STATS_DIR "/covers/%1.%2").arg(bookId).arg(ext);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(data);
    f.close();
    return path;
}

bool HardcoverWorker::readBookState(qint64 bookId, QString *isbn, int *cpage, int *npage,
                                     bool *completed, qint64 *completedTs)
{
    sqlite3 *exp = openExplorerReadOnly();
    if (!exp)
        return false;
    sqlite3_stmt *st = nullptr;
    const char *sql =
        "SELECT bi.isbn, IFNULL(bs.cpage,0), IFNULL(bs.npage,0), IFNULL(bs.completed,0),"
        " IFNULL(bs.completed_ts,0)"
        " FROM books_impl bi"
        " JOIN books_settings bs ON bs.bookid = bi.id AND bs.profileid = 1"
        " WHERE bi.id = ?1";
    bool ok = false;
    if (sqlite3_prepare_v2(exp, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bookId);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *isbn = normalizeIsbn(colText(st, 0));
            *cpage = sqlite3_column_int(st, 1);
            *npage = sqlite3_column_int(st, 2);
            *completed = sqlite3_column_int(st, 3) != 0;
            *completedTs = sqlite3_column_int64(st, 4);
            ok = !isbn->isEmpty();
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(exp);
    return ok;
}

bool HardcoverWorker::readBookTitleAuthor(qint64 bookId, QString *title, QString *author)
{
    sqlite3 *exp = openExplorerReadOnly();
    if (!exp)
        return false;
    sqlite3_stmt *st = nullptr;
    const char *sql = "SELECT title, author FROM books_impl WHERE id=?1";
    bool ok = false;
    if (sqlite3_prepare_v2(exp, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bookId);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *title = colText(st, 0).trimmed();
            *author = colText(st, 1).trimmed();
            ok = !title->isEmpty();
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(exp);
    return ok;
}

void HardcoverWorker::refreshEditionMetadata(HardcoverLink *link, QString *errOut)
{
    if (link->editionId == 0)
        return;
    static const char *kEditionById = R"(
        query EditionById($id: Int!) {
          editions(where: { id: { _eq: $id } }, limit: 1) {
            id isbn_10 isbn_13 pages title cached_image reading_format_id edition_format publisher { name } release_date
            book { id title }
          }
        })";
    QJsonObject data = graphql(QString::fromUtf8(kEditionById), {{"id", double(link->editionId)}}, errOut);
    const QJsonArray editions = data.value(QStringLiteral("editions")).toArray();
    if (!editions.isEmpty())
        applyEditionJson(link, editions.first().toObject());
}

QVariantList HardcoverWorker::queryEditionsList(qint64 hcBookId, qint64 currentEditionId,
                                                 QString *errOut)
{
    static const char *kFindEditions = R"(
        query FindEditions($bookId: Int!) {
          editions(where: { book_id: { _eq: $bookId } }, order_by: { users_count: desc_nulls_last }) {
            id isbn_10 isbn_13 pages title cached_image reading_format_id edition_format release_date
            language { code2 }
          }
        })";
    QJsonObject data = graphql(QString::fromUtf8(kFindEditions), {{"bookId", double(hcBookId)}}, errOut);
    const QJsonArray editions = data.value(QStringLiteral("editions")).toArray();

    QVariantList out;
    for (const QJsonValue &v : editions) {
        const QJsonObject ed = v.toObject();
        QVariantMap m;
        m["editionId"] = qint64(ed.value(QStringLiteral("id")).toDouble());
        m["isbn10"] = ed.value(QStringLiteral("isbn_10")).toString();
        m["isbn13"] = ed.value(QStringLiteral("isbn_13")).toString();
        m["pages"] = ed.value(QStringLiteral("pages")).toInt();
        m["releaseDate"] = ed.value(QStringLiteral("release_date")).toString();
        m["languageCode"] = ed.value(QStringLiteral("language")).toObject()
                                 .value(QStringLiteral("code2")).toString();
        m["readingFormatId"] = ed.value(QStringLiteral("reading_format_id")).toInt();
        m["editionFormat"] = ed.value(QStringLiteral("edition_format")).toString();
        m["coverUrl"] = coverUrlFromJson(ed.value(QStringLiteral("cached_image")));
        m["isCurrent"] = (qint64(ed.value(QStringLiteral("id")).toDouble()) == currentEditionId);
        out.append(m);
    }
    return out;
}

void HardcoverWorker::linkBook(qint64 bookId)
{
    QString isbn;
    int cpage = 0, npage = 0;
    bool completed = false;
    qint64 completedTs = 0;
    if (!readBookState(bookId, &isbn, &cpage, &npage, &completed, &completedTs) || isbn.isEmpty()) {
        emit autoMatchNotFound(bookId);
        return;
    }

    static const char *kFindByIsbn = R"(
        query FindByIsbn($isbn: String!) {
          editions(where: { _or: [{isbn_13: {_eq: $isbn}}, {isbn_10: {_eq: $isbn}}] }, limit: 1) {
            id isbn_10 isbn_13 pages title cached_image reading_format_id edition_format publisher { name } release_date
            book { id title }
          }
        })";
    QString err;
    QJsonObject data = graphql(QString::fromUtf8(kFindByIsbn), {{"isbn", isbn}}, &err);
    const QJsonArray editions = data.value(QStringLiteral("editions")).toArray();
    if (editions.isEmpty()) {
        emit autoMatchNotFound(bookId);
        return;
    }

    HardcoverLink tmp;
    applyEditionJson(&tmp, editions.first().toObject());

    QVariantMap m;
    m["editionId"] = tmp.editionId;
    m["hcBookId"] = tmp.hcBookId;
    m["title"] = tmp.editionTitle;
    m["isbn10"] = tmp.editionIsbn10;
    m["isbn13"] = tmp.editionIsbn13;
    m["pages"] = tmp.editionPages;
    m["coverUrl"] = tmp.editionCoverUrl;
    m["readingFormatId"] = tmp.readingFormatId;
    m["publisher"] = tmp.editionPublisher;
    m["releaseDate"] = tmp.editionReleaseDate;
    emit autoMatchFound(bookId, m);
}

void HardcoverWorker::saveEditionAndCheckStatus(qint64 bookId, qint64 hcBookId, qint64 editionId)
{
    Q_UNUSED(hcBookId); /* re-derived from the edition itself in refreshEditionMetadata */
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb) {
        emit editionMatched(bookId, false, QStringLiteral("could not open local db"));
        return;
    }
    HardcoverLink link = loadLink(linkDb, bookId);
    link.editionId = editionId;
    QString err;
    refreshEditionMetadata(&link, &err);
    if (link.hcBookId == 0) {
        sqlite3_close(linkDb);
        emit editionMatched(bookId, false, err.isEmpty() ? QStringLiteral("edition lookup failed") : err);
        return;
    }
    /* Download the cover once now, while we know network is active (this
     * whole flow only runs from an explicit "Link book" press) - so the
     * matched-edition widget can show it later from a local file, without
     * ever needing the network just to display the tab. */
    if (!link.editionCoverUrl.isEmpty()) {
        const QString localPath = cacheCoverLocally(bookId, link.editionCoverUrl);
        if (!localPath.isEmpty())
            link.editionCoverLocalPath = localPath;
    }
    saveLink(linkDb, link);
    sqlite3_close(linkDb);
    emit editionMatched(bookId, true, QString());
    checkReadingStatus(bookId);
}

void HardcoverWorker::confirmAutoMatch(qint64 bookId, qint64 editionId)
{
    saveEditionAndCheckStatus(bookId, 0, editionId);
}

void HardcoverWorker::confirmEditionPick(qint64 bookId, qint64 editionId)
{
    saveEditionAndCheckStatus(bookId, 0, editionId);
}

void HardcoverWorker::searchBook(qint64 bookId)
{
    QString title, author;
    if (!readBookTitleAuthor(bookId, &title, &author)) {
        emit bookSearchReady(bookId, {});
        return;
    }

    QString err;
    static const char *kSearch = R"(
        query Search($query: String!) {
          search(query: $query, per_page: 25, page: 1, query_type: "Book") {
            ids
          }
        })";
    const QString q = author.isEmpty() ? title : (title + QLatin1Char(' ') + author);
    QJsonObject data = graphql(QString::fromUtf8(kSearch), {{"query", q}}, &err);
    const QJsonArray idsRaw = data.value(QStringLiteral("search")).toObject()
                                  .value(QStringLiteral("ids")).toArray();
    if (idsRaw.isEmpty()) {
        emit bookSearchReady(bookId, {});
        return;
    }

    QJsonArray idsInt;
    QHash<qint64, int> order;
    int i = 0;
    for (const QJsonValue &v : idsRaw) {
        const qint64 id = v.isString() ? v.toString().toLongLong() : qint64(v.toDouble());
        idsInt.append(id);
        order[id] = i++;
    }

    static const char *kHydrate = R"(
        query Hydrate($ids: [Int!]) {
          books(where: { id: { _in: $ids } }) {
            id
            title
            release_year
            pages
            cached_image
            contributions: cached_contributors
          }
        })";
    QJsonObject data2 = graphql(QString::fromUtf8(kHydrate), {{"ids", idsInt}}, &err);
    QVector<QJsonObject> books;
    for (const QJsonValue &v : data2.value(QStringLiteral("books")).toArray())
        books.append(v.toObject());

    std::sort(books.begin(), books.end(), [&](const QJsonObject &a, const QJsonObject &b) {
        return order.value(qint64(a.value(QStringLiteral("id")).toDouble()))
             < order.value(qint64(b.value(QStringLiteral("id")).toDouble()));
    });

    QVariantList out;
    for (const QJsonObject &b : books) {
        QVariantMap m;
        m["hcBookId"] = qint64(b.value(QStringLiteral("id")).toDouble());
        m["title"] = b.value(QStringLiteral("title")).toString();
        m["releaseYear"] = b.value(QStringLiteral("release_year")).toInt();
        m["pages"] = b.value(QStringLiteral("pages")).toInt();
        m["coverUrl"] = coverUrlFromJson(b.value(QStringLiteral("cached_image")));
        m["author"] = extractAuthorNames(b.value(QStringLiteral("contributions")));
        out.append(m);
    }
    emit bookSearchReady(bookId, out);
}

void HardcoverWorker::pickBook(qint64 bookId, qint64 hcBookId)
{
    QString err;
    const QVariantList out = queryEditionsList(hcBookId, 0, &err);
    emit editionsReady(bookId, out);
}

bool HardcoverWorker::pushRead(HardcoverLink &link, int explorerPage, int deviceTotalPages,
                                int statusId, QString *errOut)
{
    /* The device's own page count very often doesn't match the linked
     * Hardcover edition's page count (different print run, different
     * format entirely). Map proportionally instead of sending the device's
     * raw page number: e.g. page 112 of 368 on the device, in a 450-page
     * edition, becomes round(112 * 450 / 368) = 137. Falls back to sending
     * the raw page if either total is unknown, still clamped below. */
    int pushPage;
    if (deviceTotalPages > 0 && link.editionPages > 0)
        pushPage = int(std::round(double(explorerPage) * link.editionPages / deviceTotalPages));
    else
        pushPage = explorerPage;
    if (pushPage < 1)
        pushPage = 1;
    if (link.editionPages > 0 && pushPage > link.editionPages)
        pushPage = link.editionPages;

    static const char *kUpsertUserBook = R"(
        mutation Upsert($object: UserBookCreateInput!) {
          insert_user_book(object: $object) {
            error
            user_book { id status_id edition_id }
          }
        })";
    QJsonObject object{
        {"book_id", double(link.hcBookId)},
        {"edition_id", double(link.editionId)},
        {"status_id", statusId},
        {"privacy_setting_id", fetchPrivacySetting()},
    };
    QJsonObject data = graphql(QString::fromUtf8(kUpsertUserBook), {{"object", object}}, errOut);
    const QJsonObject insertResult = data.value(QStringLiteral("insert_user_book")).toObject();
    const QJsonObject userBook = insertResult.value(QStringLiteral("user_book")).toObject();
    if (userBook.isEmpty()) {
        if (errOut && errOut->isEmpty())
            *errOut = QStringLiteral("no user_book in response");
        return false;
    }
    link.userBookId = qint64(userBook.value(QStringLiteral("id")).toDouble());

    if (link.readId == 0) {
        /* Defensive re-check before creating: insert_user_book just above
         * is an upsert (safe to call repeatedly), but this create call
         * is not - if pushRead() somehow runs twice in quick succession
         * for the same book before the first call's own readId gets
         * persisted back to our local DB (both calls loading link fresh,
         * both seeing readId==0), this would otherwise create two
         * separate reads on Hardcover with the same start date - exactly
         * a real, confirmed report from an actual linking session, not
         * a hypothetical. Ask Hardcover directly whether userBookId
         * already has a read attached before assuming it doesn't. */
        static const char *kExistingRead = R"(
            query ExistingRead($userBookId: Int!) {
              user_book_reads(where: { user_book_id: { _eq: $userBookId } },
                               order_by: { id: desc }, limit: 1) {
                id
              }
            })";
        QString existErr;
        QJsonObject existData = graphql(QString::fromUtf8(kExistingRead),
            {{"userBookId", double(link.userBookId)}}, &existErr);
        /* existErr deliberately unchecked - a failed check here just means
         * this falls through to the normal create path below, exactly
         * the pre-fix behavior, not a reason to fail pushRead() outright. */
        const QJsonArray existingReads =
            existData.value(QStringLiteral("user_book_reads")).toArray();
        if (!existingReads.isEmpty()) {
            link.readId = qint64(
                existingReads.first().toObject().value(QStringLiteral("id")).toDouble());
        }
    }

    if (link.readId == 0) {
        static const char *kCreateRead = R"(
            mutation CreateRead($id: Int!, $pages: Int, $editionId: Int) {
              insert_user_book_read(user_book_id: $id, user_book_read: {
                progress_pages: $pages, edition_id: $editionId
              }) {
                error
                user_book_read { id }
              }
            })";
        QJsonObject data2 = graphql(QString::fromUtf8(kCreateRead),
            {{"id", double(link.userBookId)}, {"pages", pushPage}, {"editionId", double(link.editionId)}},
            errOut);
        const QJsonObject read = data2.value(QStringLiteral("insert_user_book_read")).toObject()
                                      .value(QStringLiteral("user_book_read")).toObject();
        if (read.isEmpty()) {
            if (errOut && errOut->isEmpty())
                *errOut = QStringLiteral("could not create read");
            return false;
        }
        link.readId = qint64(read.value(QStringLiteral("id")).toDouble());
    }

    /* update_user_book_read replaces the whole row rather than patching, so
     * every field we care about must be resent every time. */
    QString updateQuery = QStringLiteral(R"(
        mutation UpdateRead($id: Int!, $pages: Int, $editionId: Int, $startedAt: date%1) {
          update_user_book_read(id: $id, object: {
            progress_pages: $pages, edition_id: $editionId, started_at: $startedAt%2
          }) {
            error
            user_book_read { id }
          }
        })").arg(link.finishedAt.isEmpty() ? QString() : QStringLiteral(", $finishedAt: date"),
                  link.finishedAt.isEmpty() ? QString() : QStringLiteral(", finished_at: $finishedAt"));
    QJsonObject vars{
        {"id", double(link.readId)},
        {"pages", pushPage},
        {"editionId", double(link.editionId)},
        {"startedAt", link.startedAt},
    };
    if (!link.finishedAt.isEmpty())
        vars["finishedAt"] = link.finishedAt;
    QJsonObject data3 = graphql(updateQuery, vars, errOut);
    const QJsonObject updatedRead = data3.value(QStringLiteral("update_user_book_read")).toObject()
                                         .value(QStringLiteral("user_book_read")).toObject();
    if (updatedRead.isEmpty()) {
        if (errOut && errOut->isEmpty())
            *errOut = QStringLiteral("could not update read");
        return false;
    }

    link.lastPage = pushPage;
    link.lastStatus = statusId;
    return true;
}

void HardcoverWorker::checkReadingStatus(qint64 bookId)
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb) {
        emit needsReadingConfirm(bookId, todayIso());
        return;
    }
    HardcoverLink link = loadLink(linkDb, bookId);

    static const char *kExistingUserBook = R"(
        query ExistingUserBook($bookId: Int!) {
          user_books(where: { book_id: { _eq: $bookId } }, limit: 1) {
            id
            status_id
            user_book_reads(order_by: {id: desc}, limit: 1) {
              id
              started_at
            }
          }
        })";
    QString err;
    QJsonObject data = graphql(QString::fromUtf8(kExistingUserBook),
                                {{"bookId", double(link.hcBookId)}}, &err);
    const QJsonArray existing = data.value(QStringLiteral("user_books")).toArray();

    if (!existing.isEmpty()) {
        const QJsonObject ub = existing.first().toObject();
        if (ub.value(QStringLiteral("status_id")).toInt() == 2) {
            const QJsonArray reads = ub.value(QStringLiteral("user_book_reads")).toArray();
            QString startedAt;
            qint64 readId = 0;
            if (!reads.isEmpty()) {
                const QJsonObject r = reads.first().toObject();
                startedAt = r.value(QStringLiteral("started_at")).toString();
                readId = qint64(r.value(QStringLiteral("id")).toDouble());
            }
            link.userBookId = qint64(ub.value(QStringLiteral("id")).toDouble());
            link.readId = readId;
            link.startedAt = startedAt;
            saveLink(linkDb, link);
            sqlite3_close(linkDb);
            emit alreadyReading(bookId, startedAt);
            return;
        }
    }
    sqlite3_close(linkDb);
    emit needsReadingConfirm(bookId, todayIso());
}

void HardcoverWorker::confirmMarkReading(qint64 bookId)
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb) {
        emit readingConfirmed(bookId, false, QStringLiteral("could not open local db"));
        return;
    }
    HardcoverLink link = loadLink(linkDb, bookId);
    link.startedAt = todayIso();

    QString isbn;
    int cpage = 0, npage = 0;
    bool completed = false;
    qint64 completedTs = 0;
    int explorerPage = 0;
    if (readBookState(bookId, &isbn, &cpage, &npage, &completed, &completedTs))
        explorerPage = completed ? npage : cpage;

    QString err;
    const bool ok = pushRead(link, explorerPage, npage, 2, &err);
    if (ok)
        saveLink(linkDb, link);
    sqlite3_close(linkDb);
    emit readingConfirmed(bookId, ok, err);
}

void HardcoverWorker::syncProgress(qint64 bookId)
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb) {
        emit progressPushed(bookId, false, QStringLiteral("could not open local db"));
        return;
    }
    HardcoverLink link = loadLink(linkDb, bookId);
    sqlite3_close(linkDb);

    if (link.editionId == 0) {
        emit progressPushed(bookId, false, QStringLiteral("no edition matched yet - use Link book first"));
        return;
    }
    if (link.userBookId == 0) {
        /* Reading status was never resolved (or you said No before) -
         * re-ask instead of silently pushing nothing. */
        checkReadingStatus(bookId);
        return;
    }

    QString isbn;
    int cpage = 0, npage = 0;
    bool completed = false;
    qint64 completedTs = 0;
    if (!readBookState(bookId, &isbn, &cpage, &npage, &completed, &completedTs)) {
        emit progressPushed(bookId, false, QStringLiteral("could not read book state"));
        return;
    }

    if (completed && link.finishedAt.isEmpty()) {
        const QString finishedAt = completedTs > 0 ? warsawDateFromUnix(completedTs) : todayIso();
        emit needsFinishConfirm(bookId, finishedAt, link.editionTitle);
        return;
    }

    sqlite3 *linkDb2 = openLinkDb();
    if (!linkDb2) {
        emit progressPushed(bookId, false, QStringLiteral("could not open local db"));
        return;
    }
    HardcoverLink link2 = loadLink(linkDb2, bookId);
    QString err;
    const int explorerPage = completed ? npage : cpage;
    const int statusId = completed ? 3 : 2;
    const bool ok = pushRead(link2, explorerPage, npage, statusId, &err);
    if (ok)
        saveLink(linkDb2, link2);
    sqlite3_close(linkDb2);
    emit progressPushed(bookId, ok, err);
}

void HardcoverWorker::pushFinishAndClearFlag(qint64 bookId)
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb) {
        emit finishConfirmed(bookId, false, QStringLiteral("could not open local db"));
        return;
    }
    HardcoverLink link = loadLink(linkDb, bookId);

    QString isbn;
    int cpage = 0, npage = 0;
    bool completed = false;
    qint64 completedTs = 0;
    readBookState(bookId, &isbn, &cpage, &npage, &completed, &completedTs);
    link.finishedAt = completedTs > 0 ? warsawDateFromUnix(completedTs) : todayIso();

    QString err;
    const bool ok = pushRead(link, npage, npage, 3, &err);
    if (ok)
        saveLink(linkDb, link);
    /* Clear regardless of push success - either way this specific
     * completion event has now been handled, so daemon.c's flag
     * shouldn't keep re-asking about it on the next app launch. */
    sqlite3_exec(linkDb,
        QStringLiteral("UPDATE links SET pending_finish_confirm = 0 WHERE book_id = %1")
            .arg(bookId).toUtf8().constData(),
        nullptr, nullptr, nullptr);
    sqlite3_close(linkDb);
    emit finishConfirmed(bookId, ok, err);
}

void HardcoverWorker::confirmFinish(qint64 bookId)
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb) {
        emit finishConfirmed(bookId, false, QStringLiteral("could not open local db"));
        return;
    }
    HardcoverLink link = loadLink(linkDb, bookId);

    QString isbn;
    int cpage = 0, npage = 0;
    bool completed = false;
    qint64 completedTs = 0;
    readBookState(bookId, &isbn, &cpage, &npage, &completed, &completedTs);
    const QString finishedAt = completedTs > 0 ? warsawDateFromUnix(completedTs) : todayIso();

    /* Only now - the user has explicitly pressed Yes to syncing - is a
     * network call appropriate at all. Deliberately not made any earlier
     * (e.g. from checkPendingFinishConfirm(), which runs from an app-
     * launch hook, exactly the context inkview_bridge.h's own comment
     * warns never to force network activity from). Our own local
     * finished_at only ever tracks what WE last pushed, so it stays blank
     * if this book was marked Read directly on the Hardcover site itself,
     * outside this app entirely - ask Hardcover what it actually has. */
    QString err;
    static const char *kExistingFinishStatus = R"(
        query ExistingFinishStatus($bookId: Int!) {
          user_books(where: { book_id: { _eq: $bookId } }, limit: 1) {
            id
            status_id
            user_book_reads(order_by: {id: desc}, limit: 1) {
              id
              finished_at
            }
          }
        })";
    QJsonObject data = graphql(QString::fromUtf8(kExistingFinishStatus),
                                {{"bookId", double(link.hcBookId)}}, &err);
    const QJsonArray existing = data.value(QStringLiteral("user_books")).toArray();
    if (!existing.isEmpty()) {
        const QJsonObject ub = existing.first().toObject();
        if (ub.value(QStringLiteral("status_id")).toInt() == 3) {
            const QJsonArray reads = ub.value(QStringLiteral("user_book_reads")).toArray();
            const QString existingFinishedAt = reads.isEmpty() ? QString()
                : reads.first().toObject().value(QStringLiteral("finished_at")).toString();
            const QString editionTitle = link.editionTitle;
            sqlite3_close(linkDb);
            if (existingFinishedAt == finishedAt) {
                /* Already correct - nothing to push, just inform and
                 * clear the flag (no API call needed at all). */
                acknowledgeAlreadyFinished(bookId, finishedAt);
                emit autoSyncAlreadyFinished(bookId, finishedAt, editionTitle);
            } else {
                /* Left pending on purpose - confirmDateUpdate() or
                 * acknowledgeAlreadyFinished() (via the "No" path) clears
                 * it once the user actually answers this. */
                emit autoSyncNeedsDateUpdate(bookId, existingFinishedAt, finishedAt, editionTitle);
            }
            return;
        }
    }

    /* Not yet marked Read on Hardcover, or the check above failed/timed
     * out (no network, request error) - degrades to exactly the original,
     * always-worked behavior: push it directly. */
    sqlite3_close(linkDb);
    pushFinishAndClearFlag(bookId);
}

void HardcoverWorker::confirmDateUpdate(qint64 bookId)
{
    pushFinishAndClearFlag(bookId);
}

void HardcoverWorker::declineFinish(qint64 bookId)
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb) {
        emit progressPushed(bookId, false, QStringLiteral("could not open local db"));
        return;
    }
    HardcoverLink link = loadLink(linkDb, bookId);

    QString isbn;
    int cpage = 0, npage = 0;
    bool completed = false;
    qint64 completedTs = 0;
    readBookState(bookId, &isbn, &cpage, &npage, &completed, &completedTs);

    QString err;
    /* Declined finishing - stay "Reading" status-wise, just push current page. */
    const bool ok = pushRead(link, cpage, npage, 2, &err);
    if (ok)
        saveLink(linkDb, link);
    /* Same reasoning as confirmFinish - this completion event has been
     * handled (you said not yet), so don't keep re-asking about it. */
    sqlite3_exec(linkDb,
        QStringLiteral("UPDATE links SET pending_finish_confirm = 0 WHERE book_id = %1")
            .arg(bookId).toUtf8().constData(),
        nullptr, nullptr, nullptr);
    sqlite3_close(linkDb);
    emit progressPushed(bookId, ok, err);
}

void HardcoverWorker::acknowledgeAlreadyFinished(qint64 bookId, const QString &finishedAt)
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb)
        return;
    /* No API call at all - Hardcover's own state already matches (or, for
     * the "declined a date update" case, stays whatever it already was) -
     * but finished_at MUST be recorded locally too, not just the pending
     * flag cleared. Confirmed on-device to be a real bug otherwise:
     * check_hardcover_completions()'s own candidate query in daemon.c
     * matches on (finished_at IS NULL OR '') AND pending_finish_confirm=0
     * - clearing only the flag left finished_at empty, so the very next
     * hourly check (or app restart) saw an apparently-unhandled
     * completion again and re-set the same flag, re-showing the same
     * popup indefinitely. */
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(linkDb,
            "UPDATE links SET pending_finish_confirm = 0, finished_at = ?1 WHERE book_id = ?2",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, finishedAt.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, bookId);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(linkDb);
}

void HardcoverWorker::checkPendingFinishConfirm()
{
    sqlite3 *linkDb = openLinkDb();
    if (!linkDb)
        return;

    /* daemon.c sets pending_finish_confirm=1 the moment it notices a
     * linked book went complete, via a plain SQLite write - no networking
     * happens there at all. This just looks for the first such flag and
     * asks about it, the same way a manual "Sync progress" press would.
     * openLinkDb() already ran the column migration above. */
    sqlite3_stmt *st = nullptr;
    qint64 bookId = 0;
    const char *sql =
        "SELECT book_id FROM links WHERE pending_finish_confirm = 1"
        " AND user_book_id != 0 AND (finished_at IS NULL OR finished_at = '')"
        " ORDER BY book_id LIMIT 1";
    if (sqlite3_prepare_v2(linkDb, sql, -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            bookId = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (bookId == 0) {
        sqlite3_close(linkDb);
        return;
    }

    HardcoverLink link = loadLink(linkDb, bookId);
    sqlite3_close(linkDb);

    QString isbn;
    int cpage = 0, npage = 0;
    bool completed = false;
    qint64 completedTs = 0;
    if (!readBookState(bookId, &isbn, &cpage, &npage, &completed, &completedTs) || !completed)
        return; /* stale flag (e.g. you'd already handled it elsewhere) - ignore */

    const QString finishedAt = completedTs > 0 ? warsawDateFromUnix(completedTs) : todayIso();
    emit autoSyncNeedsFinishConfirm(bookId, finishedAt, link.editionTitle);
}

QString HardcoverWorker::warsawDateFromUnix(qint64 unixSecs)
{
    /* Fixed UTC+2 (CEST) offset - no reliance on a full IANA timezone
     * database, which this stripped-down Qt build may not have. Known
     * limitation: off by one hour during Polish winter (CET, UTC+1) - only
     * matters for a completion right at a midnight boundary. */
    QDateTime dt = QDateTime::fromSecsSinceEpoch(unixSecs, Qt::UTC).addSecs(2 * 3600);
    return dt.date().toString(Qt::ISODate);
}

QString HardcoverWorker::todayIso()
{
    return QDate::currentDate().toString(Qt::ISODate);
}
