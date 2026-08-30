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
    unlink(BACKUP_PATH);
    unlink(HANDLER_PATH_TEST);
    unlink(DISABLED_PATH);
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

    puts("all autostart tests ok");
    return 0;
}
