#pragma once

#include <QString>

/* Extracts the cover declared in the EPUB's OPF and caches it as a scaled
 * PNG. Returns the cache path, or empty on failure. */
QString epubCover(const QString &epubPath, const QString &cacheKey);
