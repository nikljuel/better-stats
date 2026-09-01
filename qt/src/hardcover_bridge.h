#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

class HardcoverWorker;

/* QML-facing bridge for optional Hardcover.app syncing. Does nothing at all
 * until a token is set, and even then, nothing happens automatically - no
 * app-open hook, no background timer. Every action here is triggered by an
 * explicit button press from OverviewTab.qml. The actual network work runs
 * on a dedicated QThread (HardcoverWorker), so a slow request never freezes
 * the UI. */
class HardcoverBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasToken READ hasToken NOTIFY tokenChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit HardcoverBridge(QObject *parent = nullptr);
    ~HardcoverBridge() override;

    bool hasToken() const;
    bool busy() const { return busy_; }

    Q_INVOKABLE QString token() const;
    Q_INVOKABLE void setToken(const QString &token);

    /* Straight from explorer-3.db (same source stats.currentBook() uses).
     * Returns -1 if none. Local read, no network. */
    Q_INVOKABLE qint64 currentBookId() const;

    /* Local-only read of hardcoversync.db for one book: title, isbn, pages,
     * cover, format, publisher, release date. hasMatch=false if never
     * linked. No network. */
    Q_INVOKABLE QVariantMap matchedEdition(qint64 bookId) const;

    /* "Link book" pressed. */
    Q_INVOKABLE void linkBook(qint64 bookId);
    Q_INVOKABLE void confirmAutoMatch(qint64 bookId, qint64 editionId);
    Q_INVOKABLE void searchBook(qint64 bookId);
    Q_INVOKABLE void pickBook(qint64 bookId, qint64 hcBookId);
    Q_INVOKABLE void confirmEditionPick(qint64 bookId, qint64 editionId);

    /* "Sync progress" pressed, and the confirm/decline responses it can
     * trigger. */
    Q_INVOKABLE void syncProgress(qint64 bookId);
    Q_INVOKABLE void confirmMarkReading(qint64 bookId);
    Q_INVOKABLE void confirmFinish(qint64 bookId);
    Q_INVOKABLE void confirmDateUpdate(qint64 bookId);
    Q_INVOKABLE void declineFinish(qint64 bookId);

    /* Checked once per app launch from main.qml's Component.onCompleted,
     * not on a timer - see main.qml's own comment for why. See
     * HardcoverWorker::checkPendingFinishConfirm for what it does. */
    Q_INVOKABLE void checkPendingFinishConfirm();
    /* For autoSyncAlreadyFinished below - nothing to sync, just clears
     * the pending flag. */
    Q_INVOKABLE void acknowledgeAlreadyFinished(qint64 bookId, const QString &finishedAt);

    Q_INVOKABLE static QString formatLabel(int readingFormatId);

signals:
    void tokenChanged();
    void busyChanged();

    void autoMatchFound(qint64 bookId, const QVariantMap &edition);
    void autoMatchNotFound(qint64 bookId);
    void bookSearchReady(qint64 bookId, const QVariantList &books);
    void editionsReady(qint64 bookId, const QVariantList &editions);
    void editionMatched(qint64 bookId, bool ok, const QString &error);

    void alreadyReading(qint64 bookId, const QString &startedAt);
    void needsReadingConfirm(qint64 bookId, const QString &todayDate);
    void readingConfirmed(qint64 bookId, bool ok, const QString &error);

    void needsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title);
    void autoSyncNeedsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title);
    /* See HardcoverWorker.h for the full reasoning behind both of these. */
    void autoSyncAlreadyFinished(qint64 bookId, const QString &finishedAtDate, const QString &title);
    void autoSyncNeedsDateUpdate(qint64 bookId, const QString &existingDate,
                                  const QString &correctDate, const QString &title);
    void finishConfirmed(qint64 bookId, bool ok, const QString &error);

    void progressPushed(qint64 bookId, bool ok, const QString &error);

    /* Emitted after anything that changes local match/sync state, so the
     * UI can refresh its matched-edition display. */
    void matchedEditionChanged(qint64 bookId);

    /* Internal: cross onto the worker thread. */
    void requestToken(const QString &token);
    void requestLinkBook(qint64 bookId);
    void requestConfirmAutoMatch(qint64 bookId, qint64 editionId);
    void requestSearchBook(qint64 bookId);
    void requestPickBook(qint64 bookId, qint64 hcBookId);
    void requestConfirmEditionPick(qint64 bookId, qint64 editionId);
    void requestSyncProgress(qint64 bookId);
    void requestConfirmMarkReading(qint64 bookId);
    void requestConfirmFinish(qint64 bookId);
    void requestConfirmDateUpdate(qint64 bookId);
    void requestDeclineFinish(qint64 bookId);
    void requestCheckPendingFinishConfirm();
    void requestAcknowledgeAlreadyFinished(qint64 bookId, const QString &finishedAt);

private slots:
    void onAutoMatchFound(qint64 bookId, const QVariantMap &edition);
    void onAutoMatchNotFound(qint64 bookId);
    void onBookSearchReady(qint64 bookId, const QVariantList &books);
    void onEditionsReady(qint64 bookId, const QVariantList &editions);
    void onEditionMatched(qint64 bookId, bool ok, const QString &error);
    void onAlreadyReading(qint64 bookId, const QString &startedAt);
    void onNeedsReadingConfirm(qint64 bookId, const QString &todayDate);
    void onReadingConfirmed(qint64 bookId, bool ok, const QString &error);
    void onNeedsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title);
    void onAutoSyncNeedsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title);
    void onAutoSyncAlreadyFinished(qint64 bookId, const QString &finishedAtDate, const QString &title);
    void onAutoSyncNeedsDateUpdate(qint64 bookId, const QString &existingDate,
                                    const QString &correctDate, const QString &title);
    void onFinishConfirmed(qint64 bookId, bool ok, const QString &error);
    void onProgressPushed(qint64 bookId, bool ok, const QString &error);

private:
    QThread thread_;
    HardcoverWorker *worker_ = nullptr;
    bool busy_ = false;

    static QString tokenFilePath();
    static QString linkDbPath();

    void setBusy(bool b);
};
