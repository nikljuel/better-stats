#include "file_handler_config.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace {

std::vector<std::string> splitApps(std::string_view value)
{
    std::vector<std::string> apps;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const size_t end = comma == std::string_view::npos ? value.size() : comma;
        if (end > start)
            apps.emplace_back(value.substr(start, end - start));
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return apps;
}

std::string joinApps(const std::vector<std::string> &apps)
{
    std::string out;
    for (const std::string &app : apps) {
        if (!out.empty())
            out += ',';
        out += app;
    }
    return out;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](char lhs, char rhs) {
               return std::tolower(static_cast<unsigned char>(lhs))
                   == std::tolower(static_cast<unsigned char>(rhs));
           });
}

bool isStockReader(std::string_view app)
{
    constexpr std::string_view prefix = "eink-reader";
    constexpr std::string_view suffix = ".app";
    return app.size() >= prefix.size() + suffix.size()
        && app.substr(0, prefix.size()) == prefix
        && app.substr(app.size() - suffix.size()) == suffix
        && std::all_of(app.begin(), app.end(), [](char c) {
               const unsigned char ch = static_cast<unsigned char>(c);
               return std::isalnum(ch) || c == '_' || c == '-' || c == '.';
           });
}

} // namespace

EpubHandlerConfigResult patchEpubHandlerConfig(std::string_view input,
                                               std::string_view handlerName,
                                               bool enable)
{
    EpubHandlerConfigResult result;
    result.output.assign(input);

    size_t lineStart = 0;
    while (lineStart < input.size()) {
        const size_t newline = input.find('\n', lineStart);
        const size_t lineEnd = newline == std::string_view::npos ? input.size() : newline;
        size_t contentEnd = lineEnd;
        if (contentEnd > lineStart && input[contentEnd - 1] == '\r')
            --contentEnd;
        const std::string_view line = input.substr(lineStart, contentEnd - lineStart);

        const size_t c1 = line.find(':');
        if (c1 != std::string_view::npos && line.substr(0, c1) == "epub") {
            const size_t c2 = line.find(':', c1 + 1);
            const size_t c3 = c2 == std::string_view::npos
                ? std::string_view::npos : line.find(':', c2 + 1);
            const size_t c4 = c3 == std::string_view::npos
                ? std::string_view::npos : line.find(':', c3 + 1);
            if (c4 == std::string_view::npos) {
                result.error = "Malformed EPUB entry";
                return result;
            }

            std::vector<std::string> apps = splitApps(line.substr(c3 + 1, c4 - c3 - 1));
            for (const std::string &app : apps) {
                if (app == handlerName)
                    result.handlerPresent = true;
                if (equalsIgnoreCase(app, "koreader.app")
                    || equalsIgnoreCase(app, "koreader"))
                    result.koreaderPresent = true;
                if (result.stockHandler.empty() && isStockReader(app))
                    result.stockHandler = app;
            }
            if (enable && result.stockHandler.empty()) {
                result.error = "No native EPUB reader found";
                return result;
            }

            if (enable && !result.handlerPresent) {
                apps.insert(apps.begin(), std::string(handlerName));
                result.changed = true;
            } else if (!enable && result.handlerPresent) {
                apps.erase(std::remove(apps.begin(), apps.end(), handlerName), apps.end());
                result.changed = true;
            }

            if (result.changed) {
                const size_t appStart = lineStart + c3 + 1;
                const size_t appEnd = lineStart + c4;
                result.output.replace(appStart, appEnd - appStart, joinApps(apps));
            }
            result.ok = true;
            return result;
        }

        if (newline == std::string_view::npos)
            break;
        lineStart = newline + 1;
    }

    result.error = "No EPUB entry found";
    return result;
}
