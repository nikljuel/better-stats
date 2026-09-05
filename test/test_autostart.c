#include "autostart.h"
#include "daemon.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
#define RUNTIME_HANDLER ROOT "/marker-handler.sh"
#define FALLBACK_HANDLER ROOT "/marker-handler-fallback.sh"
#define RUNTIME_MARKER ROOT "/reader.pid"
#define FALLBACK_MARKER ROOT "/reader-fallback.pid"

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

static char *replace_all(const char *input, const char *needle,
                         const char *replacement)
{
    assert(needle[0] != '\0');
    size_t count = 0;
    const char *at = input;
    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += strlen(needle);
    }
    size_t input_size = strlen(input);
    size_t needle_size = strlen(needle);
    size_t replacement_size = strlen(replacement);
    size_t output_size = input_size;
    if (replacement_size >= needle_size)
        output_size += count * (replacement_size - needle_size);
    else
        output_size -= count * (needle_size - replacement_size);
    char *output = malloc(output_size + 1);
    assert(output != NULL);
    char *out = output;
    at = input;
    const char *match;
    while ((match = strstr(at, needle)) != NULL) {
        size_t prefix = (size_t)(match - at);
        memcpy(out, at, prefix);
        out += prefix;
        memcpy(out, replacement, replacement_size);
        out += replacement_size;
        at = match + needle_size;
    }
    strcpy(out, at);
    return output;
}

static void write_marker_prefix(const char *handler, const char *script_path,
                                const char *marker_path, int force_fallback)
{
    char *script = replace_all(handler, READER_PIDFILE, marker_path);
    if (force_fallback) {
        char *changed = replace_all(script, "/proc/$$/stat",
                                    ROOT "/missing-proc-stat");
        free(script);
        script = changed;
    }
    char *daemon_start = strstr(script,
        "[ -x \"$app\" ] && \"$app\" --daemon");
    assert(daemon_start != NULL);
    strcpy(daemon_start, "exit 0\n");
    write_text(script_path, script);
    assert(chmod(script_path, 0755) == 0);
    free(script);
}

static void run_script(const char *script, const char *book_path)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        execl("/bin/sh", "sh", script, book_path, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void assert_marker(const char *marker_path, const char *book_path,
                          int expected_fields)
{
    char *marker = read_text(marker_path);
    char *newline = strchr(marker, '\n');
    assert(newline != NULL);
    *newline = '\0';
    char *path = newline + 1;
    char *path_end = strchr(path, '\n');
    assert(path_end != NULL && path_end[1] == '\0');
    *path_end = '\0';
    assert(strcmp(path, book_path) == 0);

    unsigned long long pid = 0, started = 0, proc_start = 0;
    char extra = '\0';
    int fields = sscanf(marker, "%llu %llu %llu %c",
                        &pid, &started, &proc_start, &extra);
    assert(fields == expected_fields);
    assert(pid > 0 && started > 0);
    if (fields == 3)
        assert(proc_start > 0);
    free(marker);
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
    unlink(RUNTIME_HANDLER);
    unlink(FALLBACK_HANDLER);
    unlink(RUNTIME_MARKER);
    unlink(FALLBACK_MARKER);
    unlink(ROOT "/missing-proc-stat");
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
    assert(strstr(handler, "# Better Stats autostart v3"));
    assert(strstr(handler, "after_self=0"));
    {
        const char *function_line = strstr(handler, "read_proc_start() {");
        const char *stat_line = strstr(handler, "stat=$(cat \"/proc/$$/stat\"");
        const char *strip_line = strstr(handler, "rest=${stat##*\\) }");
        const char *token_line = strstr(handler, "value=${20}");
        const char *header_line = strstr(handler,
            "printf '%s %s %s\\n' \"$$\"");
        const char *fallback_line = strstr(handler,
            "printf '%s %s\\n' \"$$\"");
        const char *path_line = strstr(handler,
            "printf '%s\\n' \"$1\"");
        const char *publish_line = strstr(handler, "mv -f");
        const char *daemon_line = strstr(handler, "--daemon");
        assert(function_line != NULL && stat_line != NULL);
        assert(strip_line != NULL && token_line != NULL);
        assert(header_line != NULL && fallback_line != NULL && path_line != NULL);
        assert(publish_line != NULL && daemon_line != NULL);
        assert(function_line < stat_line && stat_line < strip_line);
        assert(strip_line < token_line);
        assert(token_line < header_line && header_line < path_line);
        assert(fallback_line < path_line);
        assert(path_line < publish_line && publish_line < daemon_line);
    }

    /* Execute the generated marker prefix with an argument containing spaces.
     * Linux exercises the three-field /proc path; hosts without /proc use the
     * same two-field fallback as the PocketBook error path. */
    const char *book_path = ROOT "/Books/A book with spaces.epub";
    write_marker_prefix(handler, RUNTIME_HANDLER, RUNTIME_MARKER, 0);
    run_script(RUNTIME_HANDLER, book_path);
    assert_marker(RUNTIME_MARKER, book_path,
                  access("/proc/self/stat", R_OK) == 0 ? 3 : 2);

    write_marker_prefix(handler, FALLBACK_HANDLER, FALLBACK_MARKER, 1);
    run_script(FALLBACK_HANDLER, book_path);
    assert_marker(FALLBACK_MARKER, book_path, 2);
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
