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
#define HANDLER_MARKER "# Better Stats autostart"
#define MAX_CONFIG_SIZE (1024U * 1024U)

static const char handler_script[] =
    "#!/bin/sh\n"
    "# Better Stats autostart\n"
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
    "fmt=epub\n"
    "case \"$1\" in *.fb2|*.fb2.zip|*.fb2.gz) fmt=fb2 ;; esac\n"
    "\n"
    "reader=\"\"\n"
    "apps=$(grep -i \"^$fmt:\" \"$cfg\" 2>/dev/null | head -n 1 | cut -d: -f4)\n"
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

static const char *formats[] = {"epub", "fb2"};
#define FORMAT_COUNT (sizeof(formats) / sizeof(formats[0]))

void bs_autostart_get(bs_autostart_status *out)
{
    char *config = NULL;
    size_t config_size = 0;
    char *handler = NULL;
    size_t handler_size = 0;
    memset(out, 0, sizeof(*out));
    if (!active_config(&config, &config_size)) {
        message(out, "Handler configuration is unavailable");
        return;
    }
    int any_ok = 0, any_present = 0, any_not_first = 0, any_koreader = 0;
    size_t i;
    for (i = 0; i < FORMAT_COUNT; ++i) {
        handler_config_result parsed;
        patch_handler_config(config, config_size, formats[i],
                             HANDLER_NAME, 0, &parsed);
        if (parsed.ok) {
            any_ok = 1;
            if (parsed.handler_present) any_present = 1;
            if (parsed.handler_present && !parsed.handler_first) any_not_first = 1;
            if (parsed.koreader_present) any_koreader = 1;
        }
        free_handler_config(&parsed);
    }
    free(config);
    if (!any_ok) {
        message(out, "Handler configuration is unavailable");
        return;
    }
    int handler_exists = installed_handler(&handler, &handler_size);
    int owned = handler_exists && contains(handler, handler_size, "# Better Stats");
    int current = handler_exists && contains(handler, handler_size, HANDLER_MARKER);
    free(handler);
    out->enabled = any_present && !any_not_first && current;
    out->available = !any_koreader && (!handler_exists || owned);
    if (any_koreader)
        message(out, "KOReader association detected");
    else if (handler_exists && !owned)
        message(out, "Handler path is already in use");
    else if (any_not_first)
        message(out, "Another reader is registered");
    else if (any_present && !out->enabled)
        message(out, "Better Stats handler needs updating");
}

void bs_autostart_set(int enabled, bs_autostart_status *out)
{
    char *original = NULL;
    size_t original_size = 0;
    char *handler = NULL;
    size_t handler_size = 0;
    handler_config_result results[FORMAT_COUNT];
    memset(out, 0, sizeof(*out));
    if (!active_config(&original, &original_size)) {
        message(out, "Handler configuration is unavailable");
        return;
    }
    const char *input = original;
    size_t input_size = original_size;
    int any_changed = 0, any_koreader = 0;
    size_t i;
    for (i = 0; i < FORMAT_COUNT; ++i) {
        patch_handler_config(input, input_size, formats[i],
                             HANDLER_NAME, enabled, &results[i]);
        if (results[i].ok) {
            if (results[i].changed) any_changed = 1;
            if (results[i].koreader_present) any_koreader = 1;
            input = results[i].output;
            input_size = results[i].output_size;
        }
    }
    if (enabled && any_koreader) {
        message(out, "KOReader association detected");
        goto cleanup;
    }
    int handler_exists = installed_handler(&handler, &handler_size);
    int owned = handler_exists && contains(handler, handler_size, "# Better Stats");
    free(handler);
    if (enabled && handler_exists && !owned) {
        message(out, "Handler path is already in use");
        goto cleanup;
    }
    if (enabled && !write_handler()) {
        message(out, "Could not install handler");
        goto cleanup;
    }
    if (any_changed) {
        if (!exists(EXTENSIONS_BACKUP)
            && !write_file(EXTENSIONS_BACKUP, original, original_size)) {
            message(out, "Could not create extensions.cfg backup");
            goto cleanup;
        }
        if (!write_file(USER_EXTENSIONS, input, input_size)) {
            message(out, "Could not write extensions.cfg");
            goto cleanup;
        }
    }
    if (!enabled && owned)
        unlink(HANDLER_PATH);
    free(original);
    for (i = 0; i < FORMAT_COUNT; ++i)
        free_handler_config(&results[i]);
    bs_autostart_get(out);
    return;
cleanup:
    free(original);
    for (i = 0; i < FORMAT_COUNT; ++i)
        free_handler_config(&results[i]);
}
