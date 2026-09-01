#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>

struct sqlite3;

/* One linked book's local sync state, mirrored in hardcoversync.db. Most
 * actions here are triggered by an explicit button press from
 * OverviewTab.qml (Link book / confirm dialogs / Sync progress); the one
 * exception is checkPendingFinishConfirm(), checked once per app launch -
 * see its own comment for why this isn't a periodic in-app timer. */
struct HardcoverLink {
    qint64 bookId = 0;       /* our explorer-3.db books_impl.id */
    QString isbn;
    qint64 hcBookId = 0;     /* Hardcover books.id */
    qint64 editionId = 0;    /* Hardcover editions.id */
    qint64 userBookId = 0;   /* Hardcover user_books.id - 0 until status is
                               * actually confirmed (either "already reading"
                               * or a confirmed fresh "mark as reading") */
    qint64 readId = 0;       /* Hardcover user_book_reads.id */
    QString startedAt;       /* set once (existing remote value, or today on
                               * a confirmed fresh start), resent on every
                               * later update - update_user_book_read
                               * replaces the whole row rather than patching,
                               * so omitting a field nulls it out. */
    QString finishedAt;      /* set once on a confirmed finish, same rule. */
    int lastPage = -1;
    int lastStatus = 0;      /* 0 = not yet pushed */

    /* Cached display metadata, so the QML tab can show the matched edition
     * instantly from a local DB read - no network needed just to display. */
    QString editionTitle;
    QString editionIsbn10;
    QString editionIsbn13;
    int editionPages = 0;
    QString editionCoverUrl;
    QString editionCoverLocalPath; /* downloaded once at match time, so the
                                     * QML Image element never needs the
                                     * network just to display the tab */
    int readingFormatId = 0; /* 1 physical, 2 audiobook, 4 ebook */
    QString editionFormat;
    QString editionPublisher;
    QString editionReleaseDate; /* ISO date, e.g. "2021-05-13" */
};

class HardcoverWorker : public QObject {
    Q_OBJECT

public:
    explicit HardcoverWorker(QObject *parent = nullptr);
    ~HardcoverWorker() override;

public slots:
    void setToken(const QString &token);

    /* "Link book" pressed: tries an ISBN auto-match first. Emits
     * autoMatchFound (with edition display data, for a Yes/No confirm) if
     * one is found, or autoMatchNotFound (QML then falls through to the
     * manual search flow) if not. Does not touch any sync state yet - just
     * a lookup. */
    void linkBook(qint64 bookId);

    /* User confirmed the auto-matched edition ("Yes"). Saves it and
     * proceeds straight to the reading-status check. */
    void confirmAutoMatch(qint64 bookId, qint64 editionId);

    /* Manual flow, same as before: search by title/author, then list
     * editions for a chosen book. */
    void searchBook(qint64 bookId);
    void pickBook(qint64 bookId, qint64 hcBookId);

    /* User picked a specific edition from either the manual edition list.
     * Saves it and proceeds to the reading-status check (same as
     * confirmAutoMatch). */
    void confirmEditionPick(qint64 bookId, qint64 editionId);

    /* "Sync progress" pressed. If the reading-status question was never
     * resolved (userBookId==0 locally, e.g. you said No before), re-asks it
     * instead of pushing anything. Otherwise checks explorer-3.db's
     * completed flag: if freshly completed, asks to confirm finishing; if
     * not, just pushes current progress (no status change), matching the
     * koplugin's own separation of status vs progress. */
    void syncProgress(qint64 bookId);

    /* Confirmed "mark as currently reading" - pushes status=Reading with
     * started_at=today, creates the read, caches locally. */
    void confirmMarkReading(qint64 bookId);

    /* Confirmed "mark as read" (the user pressed Yes on the initial
     * autoSyncNeedsFinishConfirm/needsFinishConfirm prompt) - only now,
     * with explicit user intent to sync, checks Hardcover's own current
     * status first (a network call, deliberately not made any earlier -
     * see this function's own comment for why). If Hardcover already has
     * this book marked Read with the same date, there's nothing to push
     * (autoSyncAlreadyFinished). If marked Read with a different date,
     * asks before overwriting it (autoSyncNeedsDateUpdate) rather than
     * pushing over an existing value unprompted. Otherwise (not yet
     * marked Read, or the check itself failed/timed out) pushes directly,
     * exactly as this always did before the check existed. */
    void confirmFinish(qint64 bookId);

    /* The user said yes to autoSyncNeedsDateUpdate's prompt - pushes
     * directly, no further check (confirmFinish above already did it and
     * already knows the answer). */
    void confirmDateUpdate(qint64 bookId);

    /* Declined "mark as read" - still pushes current progress (cpage), just
     * without changing status or touching finished_at. */
    void declineFinish(qint64 bookId);

    /* Checked once per app launch (not on a timer - see main.qml's own
     * comment for why). daemon.c (a separate, continuously-running
     * process) is the thing that actually detects a book going complete
     * while this app isn't in the foreground - it just flags the row in
     * hardcoversync.db with a plain SQLite write, no networking. This
     * checks for any such flagged book and, if found, asks about it via
     * the normal autoSyncNeedsFinishConfirm/confirmFinish/declineFinish
     * path - same as it always did. */
    void checkPendingFinishConfirm();

    /* For autoSyncAlreadyFinished above - nothing to sync, just clears the
     * flag so daemon.c stops re-asking about this same, already-correct
     * completion on the next launch. */
    void acknowledgeAlreadyFinished(qint64 bookId, const QString &finishedAt);

signals:
    void status(const QString &line);

    void autoMatchFound(qint64 bookId, const QVariantMap &edition);
    void autoMatchNotFound(qint64 bookId);

    void bookSearchReady(qint64 bookId, const QVariantList &books);
    void editionsReady(qint64 bookId, const QVariantList &editions);

    /* Emitted right after any edition gets saved (auto-confirmed or
     * manually picked), before the reading-status check even runs, so the
     * UI can refresh its "matched edition" display immediately. */
    void editionMatched(qint64 bookId, bool ok, const QString &error);

    void alreadyReading(qint64 bookId, const QString &startedAt);
    void needsReadingConfirm(qint64 bookId, const QString &todayDate);
    void readingConfirmed(qint64 bookId, bool ok, const QString &error);

    void needsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title);
    /* Separate from needsFinishConfirm above (used by manual "Sync
     * progress"), rather than reusing the same signal - main.qml and
     * OverviewTab.qml both listen for finish-confirmation events, and
     * sharing one signal would pop up two duplicate dialogs for a single
     * manual sync. */
    void autoSyncNeedsFinishConfirm(qint64 bookId, const QString &finishedAtDate, const QString &title);
    /* Hardcover already has this book marked Read, with the same finish
     * date we're about to ask about - nothing to sync, just tell the
     * user so the popup isn't a confusing "mark as read?" for a book they
     * already marked finished directly on the site. Clears
     * pending_finish_confirm on its own (via acknowledgeAlreadyFinished
     * below), since there's no decision left to make. */
    void autoSyncAlreadyFinished(qint64 bookId, const QString &finishedAtDate, const QString &title);
    /* Hardcover already has this book marked Read, but with a different
     * finish date than what we have locally - offers to update Hardcover's
     * own date to match ours, reusing confirmFinish/declineFinish
     * underneath (the actual API call - push status=Read with our
     * finishedAt - is identical either way, only the prompt differs). */
    void autoSyncNeedsDateUpdate(qint64 bookId, const QString &existingDate,
                                  const QString &correctDate, const QString &title);
    void finishConfirmed(qint64 bookId, bool ok, const QString &error);

    void progressPushed(qint64 bookId, bool ok, const QString &error);

private:
    QString token_;

    sqlite3 *openLinkDb();
    sqlite3 *openExplorerReadOnly();
    HardcoverLink loadLink(sqlite3 *db, qint64 bookId);
    void saveLink(sqlite3 *db, const HardcoverLink &link);

    QJsonObject graphql(const QString &query, const QJsonObject &variables, QString *errOut);
    int fetchPrivacySetting();

    /* Shared by confirmFinish()'s "not yet marked Read on Hardcover"
     * branch and confirmDateUpdate() - identical push either way
     * (status=Read, our own locally-known finishedAt), just reached via
     * two different prompts. */
    void pushFinishAndClearFlag(qint64 bookId);

    /* Plain GET, for downloading the actual cover image bytes (not a
     * GraphQL call). */
    QByteArray downloadBytes(const QString &url, QString *errOut);
    /* Downloads the cover once and saves it under STATS_DIR/covers/, so the
     * QML Image element can load a local file:// path instead of hitting
     * the network every time the tab displays. Returns the local file path,
     * or empty on failure (caller just keeps showing nothing, same as
     * before this existed). */
    QString cacheCoverLocally(qint64 bookId, const QString &remoteUrl);

    bool readBookState(qint64 bookId, QString *isbn, int *cpage, int *npage, bool *completed,
                       qint64 *completedTs);
    bool readBookTitleAuthor(qint64 bookId, QString *title, QString *author);

    void refreshEditionMetadata(HardcoverLink *link, QString *errOut);
    QVariantList queryEditionsList(qint64 hcBookId, qint64 currentEditionId, QString *errOut);

    /* Shared by confirmAutoMatch/confirmEditionPick: saves the edition,
     * fetches full display metadata, emits editionMatched, then runs the
     * reading-status check. */
    void saveEditionAndCheckStatus(qint64 bookId, qint64 hcBookId, qint64 editionId);

    /* Queries the book's existing user_book/read state on Hardcover and
     * emits either alreadyReading or needsReadingConfirm. */
    void checkReadingStatus(qint64 bookId);

    /* The one call that actually pushes a progress update, resending
     * whatever startedAt/finishedAt/status currently apply. Shared by
     * confirmMarkReading, confirmFinish, declineFinish, and the plain
     * progress-only path in syncProgress. */
    /* explorerPage/deviceTotalPages: the device's own current/total page
     * count for this book. Converted to a proportional page within the
     * Hardcover edition (which very often has a different page count than
     * the device's own pagination), not sent as-is. */
    bool pushRead(HardcoverLink &link, int explorerPage, int deviceTotalPages, int statusId,
                  QString *errOut);

    static QString warsawDateFromUnix(qint64 unixSecs);
    static QString todayIso();
};
