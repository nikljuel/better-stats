#include "file_handler_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static epub_handler_config_result patch(const char *config, int enable)
{
    epub_handler_config_result out;
    patch_epub_handler_config(config, strlen(config),
                              "betterstats-handler.app", enable, &out);
    return out;
}

int main(void)
{
    const char *stock =
        "# user config\r\n"
        "pdf:@PDF_file:1:eink-reader_with_pdfium.app:ICON_PDF\r\n"
        "epub:@EPUB_file:1:eink-reader_with_blink.app,eink-reader_with_epub2.app:ICON_EPUB\r\n";
    const char *wanted =
        "# user config\r\n"
        "pdf:@PDF_file:1:eink-reader_with_pdfium.app:ICON_PDF\r\n"
        "epub:@EPUB_file:1:betterstats-handler.app,eink-reader_with_blink.app,eink-reader_with_epub2.app:ICON_EPUB\r\n";

    epub_handler_config_result enabled = patch(stock, 1);
    assert(enabled.ok && enabled.changed && !enabled.handler_present);
    assert(strcmp(enabled.stock_handler, "eink-reader_with_blink.app") == 0);
    assert(enabled.output_size == strlen(wanted));
    assert(memcmp(enabled.output, wanted, enabled.output_size) == 0);

    epub_handler_config_result repeated = patch(enabled.output, 1);
    assert(repeated.ok && !repeated.changed && repeated.handler_present);
    epub_handler_config_result disabled = patch(enabled.output, 0);
    assert(disabled.ok && disabled.changed && disabled.handler_present);
    assert(disabled.output_size == strlen(stock));
    assert(memcmp(disabled.output, stock, disabled.output_size) == 0);
    free_epub_handler_config(&enabled);
    free_epub_handler_config(&repeated);
    free_epub_handler_config(&disabled);

    const char *ko =
        "epub:@EPUB_file:1:KOReader.App,eink-reader.app:ICON_EPUB\n";
    epub_handler_config_result koreader = patch(ko, 1);
    assert(koreader.ok && koreader.koreader_present);
    free_epub_handler_config(&koreader);

    epub_handler_config_result foreign = patch(
        "epub:@EPUB_file:1:plato.app,eink-reader.app:ICON_EPUB\n", 1);
    assert(foreign.ok && foreign.changed && !foreign.koreader_present);
    assert(strstr(foreign.output, "betterstats-handler.app,plato.app"));
    free_epub_handler_config(&foreign);

    epub_handler_config_result second = patch(
        "epub:@EPUB_file:1:plato.app,betterstats-handler.app,eink-reader.app:ICON_EPUB\n",
        0);
    assert(second.ok && second.handler_present && !second.handler_first);
    free_epub_handler_config(&second);

    epub_handler_config_result malformed = patch(
        "epub:@EPUB_file:1:eink-reader.app\n", 1);
    assert(!malformed.ok);
    free_epub_handler_config(&malformed);
    epub_handler_config_result missing = patch(
        "pdf:@PDF_file:1:eink-reader.app:ICON_PDF\n", 1);
    assert(!missing.ok);
    free_epub_handler_config(&missing);

    puts("all file-handler config tests ok");
    return 0;
}
