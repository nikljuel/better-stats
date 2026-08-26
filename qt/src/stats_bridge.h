#pragma once

#include <QObject>
#include <QVariantMap>

struct bs_context;

/* Bridge between QML and the existing C modules (tracker/stats_db). */
class StatsBridge : public QObject {
    Q_OBJECT

public:
    explicit StatsBridge(QObject *parent = nullptr);
    ~StatsBridge() override;

    Q_INVOKABLE QVariantMap overall();
    Q_INVOKABLE QVariantMap currentBook();
    Q_INVOKABLE QVariantMap month(int year, int mon);
    Q_INVOKABLE QVariantMap year(int y);
    Q_INVOKABLE QVariantMap yearBooks(int y);
    Q_INVOKABLE QVariantMap autostartStatus();

private:
    bs_context *context_ = nullptr;
};
