#include "autostart.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROOT "/tmp/bs_autostart_test"
#define CONFIG_DIR ROOT "/config"
#define BIN_DIR ROOT "/bin"
#define STATS_PATH ROOT "/stats"
#define CONFIG_PATH CONFIG_DIR "/extensions.cfg"
#define SYSTEM_CONFIG_PATH CONFIG_DIR "/system-extensions.cfg"
#define BACKUP_PATH CONFIG_DIR "/extensions.cfg.backup"
#define HANDLER_PATH_TEST BIN_DIR "/betterstats-handler.app"
#define DISABLED_PATH STATS_PATH "/autostart-disabled"

static const char stock_config[] =
    "fb2:@FB2_file:1:eink-reader.app:ICON_FB2\n"
    "epub:@EPUB_file:1:eink-reader_with_blink.app,eink-reader_with_epub2.app:ICON_EPUB\n"
    "cbz:@CBZ_file:1:eink-reader.app:ICON_CBZ\n";

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static char *read_text(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)size + 1);
    assert(text != NULL);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = '\0';
    assert(fclose(file) == 0);
    return text;
}

static void reset_files(void)
{
    mkdir(ROOT, 0755);
    mkdir(CONFIG_DIR, 0755);
    mkdir(BIN_DIR, 0755);
    mkdir(STATS_PATH, 0755);
    unlink(CONFIG_PATH);
    unlink(SYSTEM_CONFIG_PATH);
    unlink(BACKUP_PATH);
    unlink(HANDLER_PATH_TEST);
    unlink(DISABLED_PATH);
    write_text(SYSTEM_CONFIG_PATH, stock_config);
    write_text(CONFIG_PATH, stock_config);
}

int main(void)
{
    bs_autostart_status status;
    reset_files();

    assert(bs_autostart_prepare(&status));
    assert(status.enabled);
    assert(access(HANDLER_PATH_TEST, F_OK) == 0);
    char *config = read_text(CONFIG_PATH);
    assert(strstr(config, "fb2:@FB2_file:1:betterstats-handler.app,eink-reader.app"));
    assert(strstr(config, "epub:@EPUB_file:1:betterstats-handler.app,eink-reader_with_blink.app"));
    assert(strstr(config, "cbz:@CBZ_file:1:betterstats-handler.app,eink-reader.app"));
    free(config);

    assert(rmdir(STATS_PATH) == 0);
    write_text(STATS_PATH, "not a directory");
    bs_autostart_set(0, &status);
    assert(status.enabled);
    assert(strstr(status.message, "Could not save autostart setting"));
    assert(access(HANDLER_PATH_TEST, F_OK) == 0);
    assert(unlink(STATS_PATH) == 0);
    assert(mkdir(STATS_PATH, 0755) == 0);

    bs_autostart_set(0, &status);
    assert(!status.enabled);
    assert(access(DISABLED_PATH, F_OK) == 0);
    assert(access(HANDLER_PATH_TEST, F_OK) != 0);
    config = read_text(CONFIG_PATH);
    assert(strstr(config, "betterstats-handler.app") == NULL);
    free(config);

    assert(bs_autostart_prepare(&status));
    assert(!status.enabled);
    assert(access(HANDLER_PATH_TEST, F_OK) != 0);

    bs_autostart_set(1, &status);
    assert(status.enabled);
    assert(access(DISABLED_PATH, F_OK) != 0);
    assert(access(HANDLER_PATH_TEST, F_OK) == 0);

    /* KOReader leaves a marker-only user file after all associations are
     * disabled. Missing formats must inherit their firmware entries without
     * discarding either KOReader's marker or unrelated comments. */
    reset_files();
    const char *markers = "#koreader\n#betterstats";
    write_text(CONFIG_PATH, markers);
    bs_autostart_get(&status);
    assert(!status.enabled && status.available);
    assert(bs_autostart_prepare(&status));
    assert(status.enabled && status.available);
    config = read_text(CONFIG_PATH);
    assert(strstr(config, markers) == config);
    assert(strstr(config,
        "epub:@EPUB_file:1:betterstats-handler.app,eink-reader_with_blink.app"));
    assert(strstr(config,
        "fb2:@FB2_file:1:betterstats-handler.app,eink-reader.app"));
    assert(strstr(config,
        "cbz:@CBZ_file:1:betterstats-handler.app,eink-reader.app"));
    char *unchanged = strdup(config);
    assert(unchanged != NULL);
    free(config);
    assert(bs_autostart_prepare(&status));
    config = read_text(CONFIG_PATH);
    assert(strcmp(config, unchanged) == 0);
    free(unchanged);
    free(config);
    char *backup = read_text(BACKUP_PATH);
    assert(strcmp(backup, markers) == 0);
    free(backup);
    char *handler = read_text(HANDLER_PATH_TEST);
    assert(strstr(handler, "# Better Stats autostart v2"));
    assert(strstr(handler, "after_self=0"));
    free(handler);

    bs_autostart_set(0, &status);
    assert(!status.enabled);
    config = read_text(CONFIG_PATH);
    assert(strstr(config, markers) == config);
    assert(strstr(config, "betterstats-handler.app") == NULL);
    free(config);

    /* When KOReader owns a format, keep it first and put Better Stats directly
     * before the stock reader so selecting Better Stats uses the stock path. */
    reset_files();
    const char *mixed =
        "#koreader\n"
        "pdf:@PDF_file:1:koreader.app,eink-reader.app:ICON_PDF\n"
        "epub:@EPUB_file:1:koreader.app,eink-reader.app:ICON_EPUB";
    write_text(CONFIG_PATH, mixed);
    assert(bs_autostart_prepare(&status));
    assert(status.enabled && status.available);
    assert(strcmp(status.message, "KOReader association detected") == 0);
    config = read_text(CONFIG_PATH);
    assert(strstr(config,
        "pdf:@PDF_file:1:koreader.app,eink-reader.app:ICON_PDF"));
    assert(strstr(config,
        "epub:@EPUB_file:1:koreader.app,betterstats-handler.app,eink-reader.app"));
    free(config);
    backup = read_text(BACKUP_PATH);
    assert(strcmp(backup, mixed) == 0);
    free(backup);

    bs_autostart_set(0, &status);
    assert(!status.enabled && status.available);
    config = read_text(CONFIG_PATH);
    assert(strstr(config,
        "epub:@EPUB_file:1:koreader.app,eink-reader.app:ICON_EPUB"));
    assert(strstr(config, "betterstats-handler.app") == NULL);
    free(config);

    reset_files();
    write_text(CONFIG_PATH,
        "epub:@EPUB_file:1:plato.app,eink-reader.app:ICON_EPUB\n");
    assert(bs_autostart_prepare(&status));
    assert(status.enabled && status.available);
    assert(strcmp(status.message, "Another reader is registered") == 0);
    config = read_text(CONFIG_PATH);
    assert(strstr(config,
        "plato.app,betterstats-handler.app,eink-reader.app"));
    free(config);

    /* A present but malformed user entry is not silently shadowed by the
     * firmware fallback. */
    reset_files();
    const char *malformed =
        "#koreader\n"
        "epub:@EPUB_file:1:eink-reader.app";
    write_text(CONFIG_PATH, malformed);
    bs_autostart_get(&status);
    assert(!status.enabled && !status.available);
    assert(strcmp(status.message,
                  "Handler configuration is unavailable") == 0);
    assert(!bs_autostart_prepare(&status));
    config = read_text(CONFIG_PATH);
    assert(strcmp(config, malformed) == 0);
    free(config);
    assert(access(HANDLER_PATH_TEST, F_OK) != 0);

    puts("all autostart tests ok");
    return 0;
}
