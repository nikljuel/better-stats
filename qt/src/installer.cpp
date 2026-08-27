#include "installer.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

extern "C" {
#include "autostart.h"
}

namespace {

constexpr const char *kAppPath = "/mnt/ext1/applications/BetterStats.app";
constexpr const char *kIconDir = "/mnt/ext1/applications/icons";
constexpr const char *kIconRel = "applications/icons/betterstats.bmp";
constexpr const char *kIconFocusedRel = "applications/icons/betterstats_f.bmp";
constexpr const char *kIconPath =
    "/mnt/ext1/applications/icons/betterstats.bmp";
constexpr const char *kIconFocusedPath =
    "/mnt/ext1/applications/icons/betterstats_f.bmp";
constexpr const char *kViewJson =
    "/mnt/ext1/system/config/desktop/view.json";
constexpr const char *kBackup =
    "/mnt/ext1/system/config/desktop/view.json.betterstats-backup";
constexpr const char *kAppId = "U_betterstats";

/* Compare rather than skip when the file is already there: the icons ship with
 * the app and change between releases, so an existing install must pick the new
 * one up. Skipping on equal content keeps this a no-op on every later start
 * instead of rewriting flash each time. */
void writeResourceIfChanged(const QString &resource, const QString &dest)
{
    QFile src(resource);
    if (!src.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = src.readAll();
    QFile existing(dest);
    if (existing.open(QIODevice::ReadOnly)) {
        const bool same = existing.readAll() == data;
        existing.close();
        if (same)
            return;
    }
    QFile out(dest);
    if (out.open(QIODevice::WriteOnly))
        out.write(data);
}

void patchViewJson()
{
    QFile file{QLatin1String(kViewJson)};
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray raw = file.readAll();
    file.close();
    QJsonParseError error;
    QJsonDocument document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;
    QJsonObject root = document.object();
    QJsonObject apps = root.value(QStringLiteral("applications")).toObject();
    if (apps.contains(QLatin1String(kAppId)))
        return;
    if (!QFile::exists(QLatin1String(kBackup)))
        QFile::copy(QLatin1String(kViewJson), QLatin1String(kBackup));

    QJsonObject entry;
    entry[QStringLiteral("path")] = QLatin1String(kAppPath);
    entry[QStringLiteral("title")] = QStringLiteral("Better Stats");
    entry[QStringLiteral("icon")] =
        QJsonObject{{QStringLiteral("path"), QLatin1String(kIconRel)}};
    entry[QStringLiteral("focused_icon")] =
        QJsonObject{{QStringLiteral("path"), QLatin1String(kIconFocusedRel)}};
    apps[QLatin1String(kAppId)] = entry;
    root[QStringLiteral("applications")] = apps;

    QJsonObject view = root.value(QStringLiteral("view")).toObject();
    QJsonArray groups = view.value(QStringLiteral("groups")).toArray();
    if (!groups.isEmpty()) {
        QJsonObject first = groups.at(0).toObject();
        QJsonArray list = first.value(QStringLiteral("apps")).toArray();
        list.append(QLatin1String(kAppId));
        first[QStringLiteral("apps")] = list;
        groups.replace(0, first);
        view[QStringLiteral("groups")] = groups;
        root[QStringLiteral("view")] = view;
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
    }
}

AutostartStatus fromCore(const bs_autostart_status &source)
{
    AutostartStatus out;
    out.enabled = source.enabled != 0;
    out.available = source.available != 0;
    out.message = QString::fromUtf8(source.message);
    return out;
}

} // namespace

void ensureRegistered()
{
    QDir().mkpath(QLatin1String(kIconDir));
    writeResourceIfChanged(QStringLiteral(":/betterstats.bmp"),
                           QLatin1String(kIconPath));
    writeResourceIfChanged(QStringLiteral(":/betterstats_f.bmp"),
                           QLatin1String(kIconFocusedPath));
    patchViewJson();
    setAutostartEnabled(true);
}

AutostartStatus autostartStatus()
{
    bs_autostart_status status{};
    bs_autostart_get(&status);
    return fromCore(status);
}

AutostartStatus setAutostartEnabled(bool enabled)
{
    bs_autostart_status status{};
    bs_autostart_set(enabled, &status);
    return fromCore(status);
}
