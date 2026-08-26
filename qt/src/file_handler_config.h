#pragma once

#include <string>
#include <string_view>

struct EpubHandlerConfigResult {
    bool ok = false;
    bool changed = false;
    bool handlerPresent = false;
    /* The firmware runs only the first entry, so being in the list is not the
     * same as being used. */
    bool handlerFirst = false;
    bool koreaderPresent = false;
    std::string stockHandler;
    std::string output;
    std::string error;
};

/* Adds/removes handlerName in the EPUB application list while preserving the
 * rest of extensions.cfg byte-for-byte. The previous first native
 * eink-reader*.app is returned for the proxy hand-off. */
EpubHandlerConfigResult patchEpubHandlerConfig(std::string_view input,
                                               std::string_view handlerName,
                                               bool enable);
