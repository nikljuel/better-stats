#include <QByteArray>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "inkview_bridge.h"
#include "installer.h"
#include "stats_bridge.h"
#include "hardcover_bridge.h"

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

class InvertedCoverProvider final : public QQuickImageProvider {
public:
    InvertedCoverProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &) override
    {
        const QString url = QUrl::fromPercentEncoding(id.toUtf8());
        QImage image(QUrl(url).toLocalFile());
        if (size)
            *size = image.size();
        image.invertPixels();
        return image;
    }
};

} // namespace

int main(int argc, char *argv[])
{
    selectPlatformPlugin();
    QCoreApplication::setSetuidAllowed(true);

    const ScreenSize screen = openInkViewScreen();
    enableScreenInversion();

    // Register the launcher icon, refreshing it if this build ships a new one.
    ensureRegistered();

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    QGuiApplication app(argc, argv);

    const QString fontFamily = inkViewFontFamily();
    if (!fontFamily.isEmpty())
        QGuiApplication::setFont(QFont(fontFamily));

    StatsBridge stats;
    HardcoverBridge hardcover;

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("inverted-cover"),
                            new InvertedCoverProvider);
    engine.addImportPath(QString::fromUtf8(kQmlPath));
    engine.rootContext()->setContextProperty(QStringLiteral("stats"), &stats);
    engine.rootContext()->setContextProperty(QStringLiteral("hardcover"), &hardcover);
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
    QTimer::singleShot(750, &stats, &StatsBridge::automaticUpdate);
    return app.exec();
}
