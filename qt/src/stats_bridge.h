#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include "updater.h"

struct bs_context;

/* Bridge between QML and the existing C modules (tracker/stats_db). */
class StatsBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool inverted READ inverted NOTIFY invertedChanged)
    Q_PROPERTY(bool automaticUpdates READ automaticUpdates NOTIFY updateChanged)
    Q_PROPERTY(QString updateState READ updateState NOTIFY updateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY updateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY updateChanged)
    Q_PROPERTY(int updateError READ updateError NOTIFY updateChanged)

public:
    explicit StatsBridge(QObject *parent = nullptr);
    ~StatsBridge() override;

    Q_INVOKABLE QVariantMap overall();
    Q_INVOKABLE QVariantMap currentBook();
    Q_INVOKABLE QVariantList readingBooks();
    Q_INVOKABLE QVariantList todaySessions();
    Q_INVOKABLE QVariantMap month(int year, int mon);
    Q_INVOKABLE QVariantMap year(int y);
    Q_INVOKABLE QVariantMap yearBooks(int y);
    Q_INVOKABLE QVariantMap autostartStatus();
    Q_INVOKABLE QVariantMap setAutostartEnabled(bool enabled);
    Q_INVOKABLE void rebootDevice();
    Q_INVOKABLE void setAutomaticUpdates(bool enabled);
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void installUpdate();
    Q_INVOKABLE QString releaseNotes(const QString &language);
    Q_INVOKABLE void dismissReleaseNotes();

    bool inverted() const;
    bool automaticUpdates() const;
    QString updateState() const;
    QString currentVersion() const;
    QString latestVersion() const;
    int updateError() const;
    void automaticUpdate();

signals:
    void invertedChanged();
    void updateChanged();

private:
    void checkForUpdates(bool automatic);
    void setUpdateState(const char *state);

    bs_context *context_ = nullptr;
    bs_update_info update_{};
    QString updateState_ = QStringLiteral("idle");
    bool inverted_ = false;
    bool releaseNotesPending_ = false;
    bool automaticUpdateDeferred_ = false;
};
