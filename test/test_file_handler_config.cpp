#include "../qt/src/file_handler_config.h"

#include <cassert>
#include <iostream>

int main()
{
    constexpr const char *stock =
        "# user config\r\n"
        "pdf:@PDF_file:1:eink-reader_with_pdfium.app:ICON_PDF\r\n"
        "epub:@EPUB_file:1:eink-reader_with_blink.app,eink-reader_with_epub2.app:ICON_EPUB\r\n";

    auto enabled = patchEpubHandlerConfig(stock, "betterstats-handler.app", true);
    assert(enabled.ok && enabled.changed && !enabled.handlerPresent);
    assert(enabled.stockHandler == "eink-reader_with_blink.app");
    assert(enabled.output ==
        "# user config\r\n"
        "pdf:@PDF_file:1:eink-reader_with_pdfium.app:ICON_PDF\r\n"
        "epub:@EPUB_file:1:betterstats-handler.app,eink-reader_with_blink.app,eink-reader_with_epub2.app:ICON_EPUB\r\n");

    auto repeated = patchEpubHandlerConfig(enabled.output, "betterstats-handler.app", true);
    assert(repeated.ok && !repeated.changed && repeated.handlerPresent);
    assert(repeated.output == enabled.output);

    auto disabled = patchEpubHandlerConfig(enabled.output, "betterstats-handler.app", false);
    assert(disabled.ok && disabled.changed && disabled.handlerPresent);
    assert(disabled.output == stock);

    const std::string withKoreader =
        "epub:@EPUB_file:1:koreader.app,eink-reader.app:ICON_EPUB\n"
        "txt:@Text_file:1:eink-reader.app:ICON_TXT\n";
    auto ko = patchEpubHandlerConfig(withKoreader, "betterstats-handler.app", true);
    assert(ko.ok && ko.koreaderPresent && ko.stockHandler == "eink-reader.app");
    assert(ko.output.find("betterstats-handler.app,koreader.app,eink-reader.app")
           != std::string::npos);

    auto malformed = patchEpubHandlerConfig(
        "epub:@EPUB_file:1:eink-reader.app\n", "betterstats-handler.app", true);
    assert(!malformed.ok && malformed.output == "epub:@EPUB_file:1:eink-reader.app\n");

    auto missing = patchEpubHandlerConfig(
        "pdf:@PDF_file:1:eink-reader.app:ICON_PDF\n", "betterstats-handler.app", true);
    assert(!missing.ok);

    auto recover = patchEpubHandlerConfig(
        "epub:@EPUB_file:1:betterstats-handler.app:ICON_EPUB\n",
        "betterstats-handler.app", false);
    assert(recover.ok && recover.changed
           && recover.output == "epub:@EPUB_file:1::ICON_EPUB\n");

    /* The _with_<engine> names are virtual; only eink-reader.app exists. */
    assert(stockReaderBinary("eink-reader_with_blink.app") == "eink-reader.app");
    assert(stockReaderBinary("eink-reader_with_epub2.app") == "eink-reader.app");
    assert(stockReaderBinary("eink-reader_with_pdfium.app") == "eink-reader.app");
    assert(stockReaderBinary("eink-reader.app") == "eink-reader.app");
    assert(stockReaderBinary("koreader.app").empty());
    assert(stockReaderBinary("../../evil.app").empty());

    std::cout << "all file-handler config tests ok\n";
}
