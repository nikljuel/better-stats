#pragma once

#include <string>
#include <string_view>

struct EpubHandlerConfigResult {
    bool ok = false;
    bool changed = false;
    bool handlerPresent = false;
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

/* Maps an extensions.cfg handler name to the binary that actually exists in
 * /ebrmain/bin. The "_with_<engine>" names are virtual: only eink-reader.app
 * is a real file. Returns "" if the name is not an eink-reader variant. */
std::string stockReaderBinary(std::string_view handler);
