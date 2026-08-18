#include "installer.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

constexpr const char *kAppPath = "/mnt/ext1/applications/BetterStats.app";
constexpr const char *kIconDir = "/mnt/ext1/applications/icons";
constexpr const char *kIconPath = "/mnt/ext1/applications/icons/betterstats.bmp";
constexpr const char *kIconFocusedPath =
    "/mnt/ext1/applications/icons/betterstats_f.bmp";
constexpr const char *kViewJson =
    "/mnt/ext1/system/config/desktop/view.json";
constexpr const char *kBackup =
    "/mnt/ext1/system/config/desktop/view.json.betterstats-backup";
constexpr const char *kAppId = "U_betterstats";

// Copies an embedded resource to a path on the device if it isn't there yet.
void writeResourceIfMissing(const QString &resource, const QString &dest)
{
    if (QFile::exists(dest))
        return;
    QFile src(resource);
    if (src.open(QIODevice::ReadOnly)) {
        QFile out(dest);
        if (out.open(QIODevice::WriteOnly))
            out.write(src.readAll());
    }
}

// Adds our launcher entry to view.json. Idempotent, defensive: any failure
// (missing/unparseable/read-only file) is ignored so the app still starts.
void patchViewJson()
{
    const QString viewJsonPath = QLatin1String(kViewJson);
    QFile f(viewJsonPath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;

    QJsonObject root = doc.object();
    QJsonObject apps = root.value(QStringLiteral("applications")).toObject();
    if (apps.contains(QLatin1String(kAppId)))
        return; // already registered

    // Back up the original once, before the first modification.
    if (!QFile::exists(QLatin1String(kBackup)))
        QFile::copy(QLatin1String(kViewJson), QLatin1String(kBackup));

    QJsonObject icon;
    icon[QStringLiteral("path")] = QLatin1String(kIconPath);
    QJsonObject iconFocused;
    iconFocused[QStringLiteral("path")] = QLatin1String(kIconFocusedPath);

    QJsonObject entry;
    entry[QStringLiteral("path")] = QLatin1String(kAppPath);
    entry[QStringLiteral("title")] = QStringLiteral("Better Stats");
    entry[QStringLiteral("icon")] = icon;
    entry[QStringLiteral("focused_icon")] = iconFocused;
    apps[QLatin1String(kAppId)] = entry;
    root[QStringLiteral("applications")] = apps;

    // Put the app id into the first launcher group so it shows up.
    QJsonObject view = root.value(QStringLiteral("view")).toObject();
    QJsonArray groups = view.value(QStringLiteral("groups")).toArray();
    if (!groups.isEmpty()) {
        QJsonObject g0 = groups.at(0).toObject();
        QJsonArray appList = g0.value(QStringLiteral("apps")).toArray();
        appList.append(QLatin1String(kAppId));
        g0[QStringLiteral("apps")] = appList;
        groups.replace(0, g0);
        view[QStringLiteral("groups")] = groups;
        root[QStringLiteral("view")] = view;
    }

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
}

} // namespace

void ensureRegistered()
{
    QDir().mkpath(QLatin1String(kIconDir));
    writeResourceIfMissing(QStringLiteral(":/betterstats.bmp"),
                           QLatin1String(kIconPath));
    writeResourceIfMissing(QStringLiteral(":/betterstats_f.bmp"),
                           QLatin1String(kIconFocusedPath));
    patchViewJson();
}
