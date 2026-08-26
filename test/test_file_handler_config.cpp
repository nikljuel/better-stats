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

    /* KOReader is recognised with and without the .app suffix, any case */
    for (const char *name : {"koreader.app", "koreader", "KOReader.App"}) {
        const std::string cfg = std::string("epub:@EPUB_file:1:") + name
            + ",eink-reader_with_blink.app:ICON_EPUB\n";
        auto r = patchEpubHandlerConfig(cfg, "betterstats-handler.app", true);
        assert(r.koreaderPresent);
    }

    /* Any other reader in front is no obstacle: the handler resolves it at run
     * time and hands the book to it, so we prepend ourselves as usual. */
    auto foreign = patchEpubHandlerConfig(
        "epub:@EPUB_file:1:plato.app,eink-reader_with_blink.app:ICON_EPUB\n",
        "betterstats-handler.app", true);
    assert(foreign.ok && foreign.changed && !foreign.koreaderPresent);
    assert(foreign.output.find("betterstats-handler.app,plato.app,")
           != std::string::npos);

    /* Being in the list is not being used: the firmware runs only the first. */
    auto second = patchEpubHandlerConfig(
        "epub:@EPUB_file:1:plato.app,betterstats-handler.app,eink-reader.app:ICON_EPUB\n",
        "betterstats-handler.app", false);
    assert(second.ok && second.handlerPresent && !second.handlerFirst);
    auto firstPos = patchEpubHandlerConfig(
        "epub:@EPUB_file:1:betterstats-handler.app,eink-reader.app:ICON_EPUB\n",
        "betterstats-handler.app", false);
    assert(firstPos.ok && firstPos.handlerPresent && firstPos.handlerFirst);

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

    std::cout << "all file-handler config tests ok\n";
}
