#include <QByteArray>
#include <QFile>
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
    const QByteArray readyPath = qgetenv("BETTERSTATS_READY_FILE");
    if (!readyPath.isEmpty()) {
        QFile ready(QString::fromUtf8(readyPath));
        if (ready.open(QIODevice::WriteOnly | QIODevice::Truncate))
            ready.close();
    }
    return app.exec();
}
