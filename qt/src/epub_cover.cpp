#include "epub_cover.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QXmlStreamReader>

#include "miniz.h"

extern "C" {
#include "daemon.h"
}

namespace {

constexpr const char *kCacheDir = STATS_DIR "/covers";

QByteArray readZipEntry(mz_zip_archive *zip, const QString &name)
{
    size_t size = 0;
    void *p = mz_zip_reader_extract_file_to_heap(
        zip, name.toUtf8().constData(), &size, 0);
    if (!p)
        return {};
    QByteArray data(static_cast<const char *>(p), int(size));
    mz_free(p);
    return data;
}

/* Path of the OPF from META-INF/container.xml. */
QString opfPath(mz_zip_archive *zip)
{
    const QByteArray xml = readZipEntry(zip, QStringLiteral("META-INF/container.xml"));
    if (xml.isEmpty())
        return {};
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("rootfile"))
            return r.attributes().value(QLatin1String("full-path")).toString();
    }
    return {};
}

/* Cover href from the OPF: EPUB3 properties="cover-image", else EPUB2
 * <meta name="cover" content="id"> -> manifest item with that id. */
QString coverHref(const QByteArray &opf)
{
    QXmlStreamReader r(opf);
    QString coverId;
    QString byProperty;
    struct Item { QString id, href, mediaType; };
    QList<Item> items;

    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement())
            continue;
        if (r.name() == QLatin1String("meta")) {
            const auto attrs = r.attributes();
            if (attrs.value(QLatin1String("name")) == QLatin1String("cover"))
                coverId = attrs.value(QLatin1String("content")).toString();
        } else if (r.name() == QLatin1String("item")) {
            const auto attrs = r.attributes();
            Item it;
            it.id = attrs.value(QLatin1String("id")).toString();
            it.href = attrs.value(QLatin1String("href")).toString();
            it.mediaType = attrs.value(QLatin1String("media-type")).toString();
            const QString props =
                attrs.value(QLatin1String("properties")).toString();
            if (props.split(QLatin1Char(' '))
                    .contains(QLatin1String("cover-image")))
                byProperty = it.href;
            items.append(it);
        }
    }
    if (!byProperty.isEmpty())
        return byProperty;
    if (!coverId.isEmpty()) {
        for (const Item &it : items)
            if (it.id == coverId)
                return it.href;
    }
    /* Last resort: a manifest item whose id contains "cover" and that is an
     * image. */
    for (const Item &it : items)
        if (it.mediaType.startsWith(QLatin1String("image/"))
                && it.id.contains(QLatin1String("cover"), Qt::CaseInsensitive))
            return it.href;
    return {};
}

} // namespace

QString epubCover(const QString &epubPath, const QString &cacheKey)
{
    if (cacheKey.isEmpty())
        return {};
    const QString cachePath =
        QStringLiteral("%1/%2.png").arg(QLatin1String(kCacheDir), cacheKey);
    if (QFile::exists(cachePath))
        return cachePath;
    if (!QFile::exists(epubPath))
        return {};

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, epubPath.toUtf8().constData(), 0))
        return {};

    QString result;
    const QString opf = opfPath(&zip);
    if (!opf.isEmpty()) {
        const QByteArray opfData = readZipEntry(&zip, opf);
        QString href = coverHref(opfData);
        if (!href.isEmpty()) {
            /* href is relative to the OPF directory */
            const int slash = opf.lastIndexOf(QLatin1Char('/'));
            const QString base = slash >= 0 ? opf.left(slash + 1) : QString();
            QString entry = QDir::cleanPath(base + href);
            QByteArray img = readZipEntry(&zip, entry);
            if (img.isEmpty())
                img = readZipEntry(&zip, href); /* some EPUBs: from root */
            const QImage image = QImage::fromData(img);
            if (!image.isNull()) {
                QDir().mkpath(QLatin1String(kCacheDir));
                const QImage scaled = image.height() > 400
                    ? image.scaledToHeight(400, Qt::SmoothTransformation)
                    : image;
                if (scaled.save(cachePath, "PNG"))
                    result = cachePath;
            }
        }
    }
    mz_zip_reader_end(&zip);
    return result;
}
