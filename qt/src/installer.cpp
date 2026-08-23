#include "installer.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "file_handler_config.h"

namespace {

// The launcher resolves the app "path" as an absolute path, but the icon
// paths relative to the storage root (/mnt/ext1). Mixing them is required:
// an absolute icon path shows no icon, a relative app path won't launch.
constexpr const char *kAppPath = "/mnt/ext1/applications/BetterStats.app";
constexpr const char *kIconDir = "/mnt/ext1/applications/icons";
constexpr const char *kIconRel = "applications/icons/betterstats.bmp";
constexpr const char *kIconFocusedRel = "applications/icons/betterstats_f.bmp";
// Absolute variants for writing the files to disk.
constexpr const char *kIconPath = "/mnt/ext1/applications/icons/betterstats.bmp";
constexpr const char *kIconFocusedPath =
    "/mnt/ext1/applications/icons/betterstats_f.bmp";
constexpr const char *kViewJson =
    "/mnt/ext1/system/config/desktop/view.json";
constexpr const char *kBackup =
    "/mnt/ext1/system/config/desktop/view.json.betterstats-backup";
constexpr const char *kAppId = "U_betterstats";
constexpr const char *kSystemExtensions = "/ebrmain/config/extensions.cfg";
constexpr const char *kUserExtensions = "/mnt/ext1/system/config/extensions.cfg";
constexpr const char *kExtensionsBackup =
    "/mnt/ext1/system/config/extensions.cfg.betterstats-backup";
constexpr const char *kHandlerDir = "/mnt/ext1/system/bin";
constexpr const char *kHandlerName = "betterstats-handler.app";
constexpr const char *kHandlerPath =
    "/mnt/ext1/system/bin/betterstats-handler.app";
constexpr const char *kHandlerMarker = "# Better Stats EPUB autostart";

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
    icon[QStringLiteral("path")] = QLatin1String(kIconRel);
    QJsonObject iconFocused;
    iconFocused[QStringLiteral("path")] = QLatin1String(kIconFocusedRel);

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

bool readFile(const char *path, QByteArray *out)
{
    QFile file{QLatin1String(path)};
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *out = file.readAll();
    return true;
}

bool writeFile(const char *path, const QByteArray &data)
{
    QSaveFile file{QLatin1String(path)};
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(data) != data.size())
        return false;
    return file.commit();
}

bool activeExtensions(QByteArray *data)
{
    const char *path = QFile::exists(QLatin1String(kUserExtensions))
        ? kUserExtensions : kSystemExtensions;
    return readFile(path, data);
}

EpubHandlerConfigResult inspectConfig(const QByteArray &data, bool enable)
{
    return patchEpubHandlerConfig(
        std::string_view(data.constData(), size_t(data.size())),
        kHandlerName, enable);
}

QByteArray installedHandler()
{
    QByteArray data;
    readFile(kHandlerPath, &data);
    return data;
}

/* The daemon is backgrounded, then the shell *becomes* the stock reader via
 * exec: same pid, same task slot the firmware already created, no orphaned
 * child. If the daemon line fails for any reason the exec still runs, so a
 * book can never be blocked by the tracking hook. */
QByteArray handlerScript(const std::string &stockHandler)
{
    return QByteArrayLiteral(
        "#!/bin/sh\n"
        "# Better Stats EPUB autostart\n"
        "app=\"/mnt/ext1/applications/BetterStats.app\"\n"
            "reader=\"/ebrmain/bin/")
        + QByteArray::fromStdString(stockReaderBinary(stockHandler))
        + QByteArrayLiteral(
            "\"\n"
            "[ -x \"$app\" ] && \"$app\" --daemon </dev/null >/dev/null 2>&1 &\n"
            "exec \"$reader\" \"$@\"\n");
}

bool writeHandlerScript(const std::string &stockHandler)
{
    const QByteArray wanted = handlerScript(stockHandler);
    if (installedHandler() == wanted)
        return true; // already current; don't rewrite flash on every launch
    if (!QDir().mkpath(QLatin1String(kHandlerDir)))
        return false;
    if (!writeFile(kHandlerPath, wanted))
        return false;
    const QFile::Permissions perms = QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::ExeOther;
    QFile::setPermissions(QLatin1String(kHandlerPath), perms);
    return true; // vfat may ignore chmod while still mounting every file executable
}

bool ensureExtensionsBackup(const QByteArray &original)
{
    if (QFile::exists(QLatin1String(kExtensionsBackup)))
        return true;
    return writeFile(kExtensionsBackup, original);
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
    /* Tracking is the whole point of the app and only works with the handler in
     * place, so it is set up on first launch instead of being a switch. Silent
     * and idempotent: it keeps its hands off a KOReader or third-party EPUB
     * association, and a failure just means no autostart. */
    setAutostartEnabled(true);
}

AutostartStatus autostartStatus()
{
    AutostartStatus status;
    QByteArray config;
    if (!activeExtensions(&config)) {
        status.message = QStringLiteral("EPUB handler configuration is unavailable");
        return status;
    }

    const EpubHandlerConfigResult inspected = inspectConfig(config, false);
    if (!inspected.ok) {
        status.message = QString::fromStdString(inspected.error);
        return status;
    }
    const QByteArray handler = installedHandler();
    const bool handlerExists = QFile::exists(QLatin1String(kHandlerPath));
    const bool owned = handler.contains("# Better Stats");
    status.enabled = inspected.handlerPresent && handler.contains(kHandlerMarker);
    status.available = !inspected.koreaderPresent
        && inspected.foreignFirstHandler.empty()
        && (!handlerExists || owned);
    if (inspected.koreaderPresent)
        status.message = QStringLiteral("KOReader association detected");
    else if (!inspected.foreignFirstHandler.empty())
        status.message = QStringLiteral("Another EPUB reader is registered");
    else if (handlerExists && !owned)
        status.message = QStringLiteral("EPUB handler path is already in use");
    else if (inspected.handlerPresent && !status.enabled)
        status.message = QStringLiteral("Better Stats EPUB handler is missing");
    return status;
}

AutostartStatus setAutostartEnabled(bool enabled)
{
    QByteArray original;
    if (!activeExtensions(&original)) {
        AutostartStatus status;
        status.message = QStringLiteral("EPUB handler configuration is unavailable");
        return status;
    }

    const EpubHandlerConfigResult patched = inspectConfig(original, enabled);
    if (!patched.ok) {
        AutostartStatus status;
        status.message = QString::fromStdString(patched.error);
        return status;
    }
    /* Somebody else owns the EPUB association. Stepping in front of them would
     * silently route books to the stock reader instead of the reader the user
     * picked, so leave the list exactly as it is. KOReader bows out even when
     * it is not first, until Better Stats can actually track it. */
    if (enabled
        && (patched.koreaderPresent || !patched.foreignFirstHandler.empty())) {
        AutostartStatus status;
        status.message = patched.koreaderPresent
            ? QStringLiteral("KOReader association detected")
            : QStringLiteral("Another EPUB reader is registered");
        return status;
    }

    const QByteArray installed = installedHandler();
    if (enabled && QFile::exists(QLatin1String(kHandlerPath))
        && !installed.contains("# Better Stats")) {
        AutostartStatus status;
        status.message = QStringLiteral("EPUB handler path is already in use");
        return status;
    }

    if (enabled && !writeHandlerScript(patched.stockHandler)) {
        AutostartStatus status;
        status.message = QStringLiteral("Could not install EPUB handler");
        return status;
    }

    if (patched.changed) {
        if (!ensureExtensionsBackup(original)) {
            AutostartStatus status;
            status.message = QStringLiteral("Could not create extensions.cfg backup");
            return status;
        }
        if (!writeFile(kUserExtensions, QByteArray::fromStdString(patched.output))) {
            AutostartStatus status;
            status.message = QStringLiteral("Could not restore extensions.cfg");
            return status;
        }
    }

    if (!enabled && installed.contains("# Better Stats"))
        QFile::remove(QLatin1String(kHandlerPath));
    return autostartStatus();
}
