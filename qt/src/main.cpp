#include <cstring>

extern "C" {
#include "daemon.h"
}

#include <QByteArray>
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QString>
#include <QUrl>

#include "inkview_bridge.h"
#include "installer.h"
#include "stats_bridge.h"

namespace {

constexpr const char *kPluginPath = "/ebrmain/plugins";
constexpr const char *kQmlPath = "/ebrmain/qml";
constexpr const char *kPlatformName = "pocketbook2";
constexpr const char *kSceneUrl = "qrc:/main.qml";

void selectPlatformPlugin()
{
    if (qEnvironmentVariableIsEmpty("QT_PLUGIN_PATH"))
        qputenv("QT_PLUGIN_PATH", QByteArray(kPluginPath));
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArray(kPlatformName));
}

} // namespace

int main(int argc, char *argv[])
{
    /* Daemon mode before any Qt: pure C loop, no UI. */
    if (argc > 1 && std::strcmp(argv[1], "--daemon") == 0)
        return run_daemon();

    selectPlatformPlugin();
    QCoreApplication::setSetuidAllowed(true);

    const ScreenSize screen = openInkViewScreen();

    // Register the launcher icon on first run (idempotent, no-op afterwards).
    ensureRegistered();

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    QGuiApplication app(argc, argv);

    const QString fontFamily = inkViewFontFamily();
    if (!fontFamily.isEmpty())
        QGuiApplication::setFont(QFont(fontFamily));

    StatsBridge stats;
    spawn_daemon(QGuiApplication::applicationFilePath().toUtf8().constData());

    QQmlApplicationEngine engine;
    engine.addImportPath(QString::fromUtf8(kQmlPath));
    engine.rootContext()->setContextProperty(QStringLiteral("stats"), &stats);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceLang"),
                                              inkViewLang());
    engine.rootContext()->setContextProperty(QStringLiteral("screenW"), screen.width);
    engine.rootContext()->setContextProperty(QStringLiteral("screenH"), screen.height);
    engine.rootContext()->setContextProperty(QStringLiteral("panelH"), screen.panelHeight);

    engine.load(QUrl(QString::fromUtf8(kSceneUrl)));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
