#include "hardcover_bridge.h"
#include "hardcover_worker.h"
#include "inkview_bridge.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QUrl>

#include <sqlite3.h>

extern "C" {
#include "daemon.h"   /* STATS_DIR, explorer_db_path() */
#include "tracker.h"  /* tracker_read_state(), pb_state */
}

QString HardcoverBridge::tokenFilePath()
{
    return QStringLiteral(STATS_DIR "/hardcover_token.txt");
}

QString HardcoverBridge::linkDbPath()
{
    return QStringLiteral(STATS_DIR "/hardcoversync.db");
}

HardcoverBridge::HardcoverBridge(QObject *parent) : QObject(parent)
{
    QDir().mkpath(QStringLiteral(STATS_DIR));

    worker_ = new HardcoverWorker();
    worker_->moveToThread(&thread_);

    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &HardcoverBridge::requestToken, worker_, &HardcoverWorker::setToken);
    connect(this, &HardcoverBridge::requestLinkBook, worker_, &HardcoverWorker::linkBook);
    connect(this, &HardcoverBridge::requestConfirmAutoMatch, worker_, &HardcoverWorker::confirmAutoMatch);
    connect(this, &HardcoverBridge::requestSearchBook, worker_, &HardcoverWorker::searchBook);
    connect(this, &HardcoverBridge::requestPickBook, worker_, &HardcoverWorker::pickBook);
    connect(this, &HardcoverBridge::requestConfirmEditionPick, worker_, &HardcoverWorker::confirmEditionPick);
    connect(this, &HardcoverBridge::requestSyncProgress, worker_, &HardcoverWorker::syncProgress);
    connect(this, &HardcoverBridge::requestConfirmMarkReading, worker_, &HardcoverWorker::confirmMarkReading);
    connect(this, &HardcoverBridge::requestConfirmFinish, worker_, &HardcoverWorker::confirmFinish);
    connect(this, &HardcoverBridge::requestConfirmDateUpdate, worker_, &HardcoverWorker::confirmDateUpdate);
    connect(this, &HardcoverBridge::requestDeclineFinish, worker_, &HardcoverWorker::declineFinish);
    connect(this, &HardcoverBridge::requestCheckPendingFinishConfirm, worker_, &HardcoverWorker::checkPendingFinishConfirm);
    connect(this, &HardcoverBridge::requestAcknowledgeAlreadyFinished, worker_, &HardcoverWorker::acknowledgeAlreadyFinished);

    connect(worker_, &HardcoverWorker::autoMatchFound, this, &HardcoverBridge::onAutoMatchFound);
    connect(worker_, &HardcoverWorker::autoMatchNotFound, this, &HardcoverBridge::onAutoMatchNotFound);
    connect(worker_, &HardcoverWorker::bookSearchReady, this, &HardcoverBridge::onBookSearchReady);
    connect(worker_, &HardcoverWorker::editionsReady, this, &HardcoverBridge::onEditionsReady);
    connect(worker_, &HardcoverWorker::editionMatched, this, &HardcoverBridge::onEditionMatched);
    connect(worker_, &HardcoverWorker::alreadyReading, this, &HardcoverBridge::onAlreadyReading);
    connect(worker_, &HardcoverWorker::needsReadingConfirm, this, &HardcoverBridge::onNeedsReadingConfirm);
    connect(worker_, &HardcoverWorker::readingConfirmed, this, &HardcoverBridge::onReadingConfirmed);
    connect(worker_, &HardcoverWorker::needsFinishConfirm, this, &HardcoverBridge::onNeedsFinishConfirm);
    connect(worker_, &HardcoverWorker::autoSyncNeedsFinishConfirm, this, &HardcoverBridge::onAutoSyncNeedsFinishConfirm);
    connect(worker_, &HardcoverWorker::autoSyncAlreadyFinished, this, &HardcoverBridge::onAutoSyncAlreadyFinished);
    connect(worker_, &HardcoverWorker::autoSyncNeedsDateUpdate, this, &HardcoverBridge::onAutoSyncNeedsDateUpdate);
    connect(worker_, &HardcoverWorker::finishConfirmed, this, &HardcoverBridge::onFinishConfirmed);
    connect(worker_, &HardcoverWorker::progressPushed, this, &HardcoverBridge::onProgressPushed);

    thread_.start();

    const QString t = token();
    if (!t.isEmpty())
        emit requestToken(t);
}

HardcoverBridge::~HardcoverBridge()
{
    thread_.quit();
    thread_.wait(3000);
}

bool HardcoverBridge::hasToken() const
{
    return !token().isEmpty();
}

QString HardcoverBridge::token() const
{
    QFile f(tokenFilePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}

void HardcoverBridge::setToken(const QString &token)
{
    QFile f(tokenFilePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream out(&f);
        out << token.trimmed();
    }
    emit tokenChanged();
    emit requestToken(token.trimmed());
}

void HardcoverBridge::setBusy(bool b)
{
    if (busy_ == b)
        return;
    busy_ = b;
    emit busyChanged();
}

qint64 HardcoverBridge::currentBookId() const
{
    pb_state s;
    if (tracker_read_state(explorer_db_path(), &s) != 0)
        return -1;
    return s.bookid;
}

QVariantMap HardcoverBridge::matchedEdition(qint64 bookId) const
{
    QVariantMap m;
    m["hasMatch"] = false;

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(linkDbPath().toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return m;
    }
    sqlite3_stmt *st = nullptr;
    const char *sql =
        "SELECT edition_title, isbn, edition_isbn10, edition_isbn13, edition_pages,"
        " edition_cover_url, reading_format_id, edition_format, edition_id,"
        " started_at, finished_at, last_status, edition_publisher, edition_release_date,"
        " user_book_id, edition_cover_local_path"
        " FROM links WHERE book_id=?1";
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, bookId);
        if (sqlite3_step(st) == SQLITE_ROW) {
            auto text = [&](int i) {
                const unsigned char *t = sqlite3_column_text(st, i);
                return t ? QString::fromUtf8(reinterpret_cast<const char *>(t)) : QString();
            };
            const qint64 editionId = sqlite3_column_int64(st, 8);
            m["hasMatch"] = editionId != 0;
            m["title"] = text(0);
            m["isbn"] = text(1);
            m["isbn10"] = text(2);
            m["isbn13"] = text(3);
            m["pages"] = sqlite3_column_int(st, 4);
            /* Prefer the locally-cached file (downloaded once at match time)
             * over the remote URL - this is a local read, called from
             * refresh() with no network guaranteed to be active, so a
             * remote URL here would often just fail to load silently. */
            const QString localPath = text(15);
            m["coverUrl"] = !localPath.isEmpty()
                ? QUrl::fromLocalFile(localPath).toString()
                : text(5);
            m["readingFormatId"] = sqlite3_column_int(st, 6);
            m["editionFormat"] = text(7);
            m["editionId"] = editionId;
            m["startedAt"] = text(9);
            m["finishedAt"] = text(10);
            m["status"] = sqlite3_column_int(st, 11);
            m["formatLabel"] = formatLabel(sqlite3_column_int(st, 6));
            m["publisher"] = text(12);
            m["releaseDate"] = text(13);
            m["userBookId"] = sqlite3_column_int64(st, 14);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return m;
}

QString HardcoverBridge::formatLabel(int readingFormatId)
{
    switch (readingFormatId) {
    case 1: return QStringLiteral("Physical");
    case 2: return QStringLiteral("Audio");
    case 4: return QStringLiteral("E-Book");
    default: return QString();
    }
}

void HardcoverBridge::linkBook(qint64 bookId)
{
    if (busy_ || !hasToken())
        return;
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestLinkBook(bookId);
}

void HardcoverBridge::confirmAutoMatch(qint64 bookId, qint64 editionId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestConfirmAutoMatch(bookId, editionId);
}

void HardcoverBridge::searchBook(qint64 bookId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestSearchBook(bookId);
}

void HardcoverBridge::pickBook(qint64 bookId, qint64 hcBookId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestPickBook(bookId, hcBookId);
}

void HardcoverBridge::confirmEditionPick(qint64 bookId, qint64 editionId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestConfirmEditionPick(bookId, editionId);
}

void HardcoverBridge::syncProgress(qint64 bookId)
{
    if (busy_ || !hasToken())
        return;
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestSyncProgress(bookId);
}

void HardcoverBridge::confirmMarkReading(qint64 bookId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestConfirmMarkReading(bookId);
}

void HardcoverBridge::confirmFinish(qint64 bookId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestConfirmFinish(bookId);
}

void HardcoverBridge::confirmDateUpdate(qint64 bookId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestConfirmDateUpdate(bookId);
}

void HardcoverBridge::declineFinish(qint64 bookId)
{
    inkViewEnsureNetwork();
    setBusy(true);
    emit requestDeclineFinish(bookId);
}

void HardcoverBridge::checkPendingFinishConfirm()
{
    /* Deliberately no inkViewEnsureNetwork() call here - this only reads a
     * local SQLite flag daemon.c already set, no networking happens until
     * (and unless) you actually respond to the resulting confirmation.
     * Also deliberately not touching busy_ - this runs once, invisibly, at
     * app launch and shouldn't disable the manual Sync progress button. */
    emit requestCheckPendingFinishConfirm();
}

void HardcoverBridge::acknowledgeAlreadyFinished(qint64 bookId, const QString &finishedAt)
{
    /* No API call at all here - Hardcover's own state already matches,
     * this only clears the local pending flag and records finished_at so
     * daemon.c's own completion check doesn't treat this as unhandled
     * again on the next pass. */
    emit requestAcknowledgeAlreadyFinished(bookId, finishedAt);
}

void HardcoverBridge::onAutoMatchFound(qint64 bookId, const QVariantMap &edition)
{
    setBusy(false);
    emit autoMatchFound(bookId, edition);
}

void HardcoverBridge::onAutoMatchNotFound(qint64 bookId)
{
    setBusy(false);
    emit autoMatchNotFound(bookId);
}

void HardcoverBridge::onBookSearchReady(qint64 bookId, const QVariantList &books)
{
    setBusy(false);
    emit bookSearchReady(bookId, books);
}

void HardcoverBridge::onEditionsReady(qint64 bookId, const QVariantList &editions)
{
    setBusy(false);
    emit editionsReady(bookId, editions);
}

void HardcoverBridge::onEditionMatched(qint64 bookId, bool ok, const QString &error)
{
    /* Not clearing busy_ here - checkReadingStatus runs immediately after
     * on the worker side, so this is a mid-flow signal, not the end of one. */
    emit editionMatched(bookId, ok, error);
    if (ok)
        emit matchedEditionChanged(bookId);
}

void HardcoverBridge::onAlreadyReading(qint64 bookId, const QString &startedAt)
{
    setBusy(false);
    emit alreadyReading(bookId, startedAt);
    emit matchedEditionChanged(bookId);
}

void HardcoverBridge::onNeedsReadingConfirm(qint64 bookId, const QString &todayDate)
{
    setBusy(false);
    emit needsReadingConfirm(bookId, todayDate);
}

void HardcoverBridge::onReadingConfirmed(qint64 bookId, bool ok, const QString &error)
{
    setBusy(false);
    emit readingConfirmed(bookId, ok, error);
    if (ok)
        emit matchedEditionChanged(bookId);
}

void HardcoverBridge::onNeedsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title)
{
    setBusy(false);
    emit needsFinishConfirm(bookId, finishedAtDate, title);
}

void HardcoverBridge::onAutoSyncNeedsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title)
{
    /* No setBusy(false) here - checkPendingFinishConfirm() never set
     * busy_ true in the first place (see its own comment for why). */
    emit autoSyncNeedsFinishConfirm(bookId, finishedAtDate, title);
}

void HardcoverBridge::onAutoSyncAlreadyFinished(qint64 bookId, const QString &finishedAtDate, const QString &title)
{
    /* Always follows a confirmFinish() call, which always sets busy_ true
     * first - this signal is only ever emitted from inside confirmFinish()
     * now, so there's no other path to guard against. */
    setBusy(false);
    emit autoSyncAlreadyFinished(bookId, finishedAtDate, title);
}

void HardcoverBridge::onAutoSyncNeedsDateUpdate(qint64 bookId, const QString &existingDate,
                                                 const QString &correctDate, const QString &title)
{
    /* Same reasoning as onAutoSyncAlreadyFinished above. */
    setBusy(false);
    emit autoSyncNeedsDateUpdate(bookId, existingDate, correctDate, title);
}

void HardcoverBridge::onFinishConfirmed(qint64 bookId, bool ok, const QString &error)
{
    setBusy(false);
    emit finishConfirmed(bookId, ok, error);
    if (ok)
        emit matchedEditionChanged(bookId);
}

void HardcoverBridge::onProgressPushed(qint64 bookId, bool ok, const QString &error)
{
    setBusy(false);
    emit progressPushed(bookId, ok, error);
    if (ok)
        emit matchedEditionChanged(bookId);
}
