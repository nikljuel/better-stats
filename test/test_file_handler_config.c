#include "file_handler_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static handler_config_result patch(const char *format, const char *config,
                                   int enable)
{
    handler_config_result out;
    patch_handler_config(config, strlen(config), format,
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

    handler_config_result enabled = patch("epub", stock, 1);
    assert(enabled.ok && enabled.changed && !enabled.handler_present);
    assert(strcmp(enabled.stock_handler, "eink-reader_with_blink.app") == 0);
    assert(enabled.output_size == strlen(wanted));
    assert(memcmp(enabled.output, wanted, enabled.output_size) == 0);

    handler_config_result repeated = patch("epub", enabled.output, 1);
    assert(repeated.ok && !repeated.changed && repeated.handler_present);
    handler_config_result disabled = patch("epub", enabled.output, 0);
    assert(disabled.ok && disabled.changed && disabled.handler_present);
    assert(disabled.output_size == strlen(stock));
    assert(memcmp(disabled.output, stock, disabled.output_size) == 0);
    free_handler_config(&enabled);
    free_handler_config(&repeated);
    free_handler_config(&disabled);

    const char *ko =
        "epub:@EPUB_file:1:KOReader.App,eink-reader.app:ICON_EPUB\n";
    handler_config_result koreader = patch("epub", ko, 1);
    assert(koreader.ok && koreader.koreader_present);
    free_handler_config(&koreader);

    handler_config_result foreign = patch("epub",
        "epub:@EPUB_file:1:plato.app,eink-reader.app:ICON_EPUB\n", 1);
    assert(foreign.ok && foreign.changed && !foreign.koreader_present);
    assert(strstr(foreign.output, "betterstats-handler.app,plato.app"));
    free_handler_config(&foreign);

    handler_config_result second = patch("epub",
        "epub:@EPUB_file:1:plato.app,betterstats-handler.app,eink-reader.app:ICON_EPUB\n",
        0);
    assert(second.ok && second.handler_present && !second.handler_first);
    free_handler_config(&second);

    handler_config_result malformed = patch("epub",
        "epub:@EPUB_file:1:eink-reader.app\n", 1);
    assert(!malformed.ok);
    free_handler_config(&malformed);
    handler_config_result missing = patch("epub",
        "pdf:@PDF_file:1:eink-reader.app:ICON_PDF\n", 1);
    assert(!missing.ok);
    free_handler_config(&missing);

    /* FB2 format support */
    const char *fb2_stock =
        "epub:@EPUB_file:1:eink-reader.app:ICON_EPUB\n"
        "fb2:@FB2_file:1:eink-reader.app:ICON_FB2\n";
    handler_config_result fb2_enabled = patch("fb2", fb2_stock, 1);
    assert(fb2_enabled.ok && fb2_enabled.changed);
    assert(strstr(fb2_enabled.output, "fb2:@FB2_file:1:betterstats-handler.app,eink-reader.app:ICON_FB2"));
    /* epub line should be untouched */
    assert(strstr(fb2_enabled.output, "epub:@EPUB_file:1:eink-reader.app:ICON_EPUB"));
    free_handler_config(&fb2_enabled);

    handler_config_result fb2_missing = patch("fb2",
        "epub:@EPUB_file:1:eink-reader.app:ICON_EPUB\n", 1);
    assert(!fb2_missing.ok);
    free_handler_config(&fb2_missing);

    const char *cbz_stock =
        "cbz:@CBZ_file:1:eink-reader.app:ICON_CBZ\n";
    handler_config_result cbz_enabled = patch("cbz", cbz_stock, 1);
    assert(cbz_enabled.ok && cbz_enabled.changed);
    assert(strstr(cbz_enabled.output,
        "cbz:@CBZ_file:1:betterstats-handler.app,eink-reader.app:ICON_CBZ"));
    free_handler_config(&cbz_enabled);

    /* Chain: patch epub then fb2 */
    handler_config_result chain1 = patch("epub", fb2_stock, 1);
    assert(chain1.ok && chain1.changed);
    handler_config_result chain2 = patch("fb2", chain1.output, 1);
    assert(chain2.ok && chain2.changed);
    assert(strstr(chain2.output, "epub:@EPUB_file:1:betterstats-handler.app,eink-reader.app:ICON_EPUB"));
    assert(strstr(chain2.output, "fb2:@FB2_file:1:betterstats-handler.app,eink-reader.app:ICON_FB2"));
    free_handler_config(&chain1);
    free_handler_config(&chain2);

    puts("all file-handler config tests ok");
    return 0;
}
