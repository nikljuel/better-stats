#include "autostart.h"

#include "file_handler_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSTEM_EXTENSIONS "/ebrmain/config/extensions.cfg"
#define USER_EXTENSIONS "/mnt/ext1/system/config/extensions.cfg"
#define EXTENSIONS_BACKUP "/mnt/ext1/system/config/extensions.cfg.betterstats-backup"
#define HANDLER_DIR "/mnt/ext1/system/bin"
#define HANDLER_NAME "betterstats-handler.app"
#define HANDLER_PATH HANDLER_DIR "/" HANDLER_NAME
#define HANDLER_MARKER "# Better Stats EPUB autostart"
#define MAX_CONFIG_SIZE (1024U * 1024U)

static const char handler_script[] =
    "#!/bin/sh\n"
    "# Better Stats EPUB autostart\n"
    "self=\"betterstats-handler.app\"\n"
    "app=\"/mnt/ext1/applications/BetterStats.app\"\n"
    "cfg=\"/mnt/ext1/system/config/extensions.cfg\"\n"
    "[ -f \"$cfg\" ] || cfg=\"/ebrmain/config/extensions.cfg\"\n"
    "\n"
    "[ -x \"$app\" ] && \"$app\" --daemon </dev/null >/dev/null 2>&1 &\n"
    "\n"
    "find_app() {\n"
    "    for dir in /ebrmain/bin /mnt/ext1/system/bin /mnt/ext1/applications; do\n"
    "        [ -x \"$dir/$1\" ] && { echo \"$dir/$1\"; return 0; }\n"
    "    done\n"
    "    return 1\n"
    "}\n"
    "\n"
    "reader=\"\"\n"
    "apps=$(grep -i \"^epub:\" \"$cfg\" 2>/dev/null | head -n 1 | cut -d: -f4)\n"
    "IFS=,\n"
    "for name in $apps; do\n"
    "    [ \"$name\" = \"$self\" ] && continue\n"
    "    reader=$(find_app \"$name\") && break\n"
    "    case \"$name\" in\n"
    "        *_with_*) reader=$(find_app \"${name%%_with_*}.app\") && break ;;\n"
    "    esac\n"
    "done\n"
    "unset IFS\n"
    "[ -n \"$reader\" ] || reader=\"/ebrmain/bin/eink-reader.app\"\n"
    "exec \"$reader\" \"$@\"\n";

static void message(bs_autostart_status *out, const char *text)
{
    snprintf(out->message, sizeof(out->message), "%s", text ? text : "");
}

static int exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int read_file(const char *path, char **data, size_t *size)
{
    struct stat stat_value;
    *data = NULL;
    *size = 0;
    if (stat(path, &stat_value) != 0 || stat_value.st_size < 0
        || (size_t)stat_value.st_size > MAX_CONFIG_SIZE)
        return 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    *data = malloc((size_t)stat_value.st_size + 1);
    if (!*data) {
        fclose(file);
        return 0;
    }
    *size = fread(*data, 1, (size_t)stat_value.st_size, file);
    int ok = !ferror(file) && *size == (size_t)stat_value.st_size;
    fclose(file);
    (*data)[*size] = '\0';
    if (!ok) {
        free(*data);
        *data = NULL;
        *size = 0;
    }
    return ok;
}

static int write_file(const char *path, const char *data, size_t size)
{
    char temp[512];
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 0;
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, data + written, size - written);
        if (n <= 0)
            break;
        written += (size_t)n;
    }
    int ok = written == size;
    if (fsync(fd) != 0)
        ok = 0;
    if (close(fd) != 0)
        ok = 0;
    if (ok)
        ok = rename(temp, path) == 0;
    if (!ok)
        unlink(temp);
    return ok;
}

static int active_config(char **data, size_t *size)
{
    return read_file(exists(USER_EXTENSIONS) ? USER_EXTENSIONS : SYSTEM_EXTENSIONS,
                     data, size);
}

static int installed_handler(char **data, size_t *size)
{
    return read_file(HANDLER_PATH, data, size);
}

static int contains(const char *data, size_t size, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;
    for (i = 0; i + n <= size; ++i)
        if (memcmp(data + i, needle, n) == 0)
            return 1;
    return 0;
}

static int write_handler(void)
{
    char *installed = NULL;
    size_t installed_size = 0;
    if (installed_handler(&installed, &installed_size)
        && installed_size == sizeof(handler_script) - 1
        && memcmp(installed, handler_script, installed_size) == 0) {
        free(installed);
        return 1;
    }
    free(installed);
    mkdir(HANDLER_DIR, 0755);
    if (!write_file(HANDLER_PATH, handler_script, sizeof(handler_script) - 1))
        return 0;
    chmod(HANDLER_PATH, 0755);
    return 1;
}

void bs_autostart_get(bs_autostart_status *out)
{
    char *config = NULL;
    size_t config_size = 0;
    char *handler = NULL;
    size_t handler_size = 0;
    epub_handler_config_result parsed;
    memset(out, 0, sizeof(*out));
    if (!active_config(&config, &config_size)) {
        message(out, "EPUB handler configuration is unavailable");
        return;
    }
    patch_epub_handler_config(config, config_size, HANDLER_NAME, 0, &parsed);
    free(config);
    if (!parsed.ok) {
        message(out, parsed.error);
        free_epub_handler_config(&parsed);
        return;
    }
    int handler_exists = installed_handler(&handler, &handler_size);
    int owned = handler_exists && contains(handler, handler_size, "# Better Stats");
    int current = handler_exists && contains(handler, handler_size, HANDLER_MARKER);
    free(handler);
    out->enabled = parsed.handler_first && current;
    out->available = !parsed.koreader_present && (!handler_exists || owned);
    if (parsed.koreader_present)
        message(out, "KOReader association detected");
    else if (handler_exists && !owned)
        message(out, "EPUB handler path is already in use");
    else if (parsed.handler_present && !parsed.handler_first)
        message(out, "Another EPUB reader is registered");
    else if (parsed.handler_present && !out->enabled)
        message(out, "Better Stats EPUB handler is missing");
    free_epub_handler_config(&parsed);
}

void bs_autostart_set(int enabled, bs_autostart_status *out)
{
    char *original = NULL;
    size_t original_size = 0;
    char *handler = NULL;
    size_t handler_size = 0;
    epub_handler_config_result parsed;
    memset(out, 0, sizeof(*out));
    if (!active_config(&original, &original_size)) {
        message(out, "EPUB handler configuration is unavailable");
        return;
    }
    patch_epub_handler_config(original, original_size, HANDLER_NAME, enabled,
                              &parsed);
    if (!parsed.ok) {
        message(out, parsed.error);
        free(original);
        free_epub_handler_config(&parsed);
        return;
    }
    if (enabled && parsed.koreader_present) {
        message(out, "KOReader association detected");
        free(original);
        free_epub_handler_config(&parsed);
        return;
    }
    int handler_exists = installed_handler(&handler, &handler_size);
    int owned = handler_exists && contains(handler, handler_size, "# Better Stats");
    free(handler);
    if (enabled && handler_exists && !owned) {
        message(out, "EPUB handler path is already in use");
        free(original);
        free_epub_handler_config(&parsed);
        return;
    }
    if (enabled && !write_handler()) {
        message(out, "Could not install EPUB handler");
        free(original);
        free_epub_handler_config(&parsed);
        return;
    }
    if (parsed.changed) {
        if (!exists(EXTENSIONS_BACKUP)
            && !write_file(EXTENSIONS_BACKUP, original, original_size)) {
            message(out, "Could not create extensions.cfg backup");
            free(original);
            free_epub_handler_config(&parsed);
            return;
        }
        if (!write_file(USER_EXTENSIONS, parsed.output, parsed.output_size)) {
            message(out, "Could not write extensions.cfg");
            free(original);
            free_epub_handler_config(&parsed);
            return;
        }
    }
    if (!enabled && owned)
        unlink(HANDLER_PATH);
    free(original);
    free_epub_handler_config(&parsed);
    bs_autostart_get(out);
}
