#include "autostart.h"

#include "daemon.h"
#include "file_handler_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SYSTEM_EXTENSIONS
#define SYSTEM_EXTENSIONS "/ebrmain/config/extensions.cfg"
#endif
#ifndef USER_EXTENSIONS
#define USER_EXTENSIONS "/mnt/ext1/system/config/extensions.cfg"
#endif
#ifndef EXTENSIONS_BACKUP
#define EXTENSIONS_BACKUP "/mnt/ext1/system/config/extensions.cfg.betterstats-backup"
#endif
#ifndef HANDLER_DIR
#define HANDLER_DIR "/mnt/ext1/system/bin"
#endif
#define HANDLER_NAME "betterstats-handler.app"
#ifndef HANDLER_PATH
#define HANDLER_PATH HANDLER_DIR "/" HANDLER_NAME
#endif
#define HANDLER_MARKER "# Better Stats autostart v2"
#define AUTOSTART_DISABLED STATS_DIR "/autostart-disabled"
#define MAX_CONFIG_SIZE (1024U * 1024U)

static const char handler_script[] =
    "#!/bin/sh\n"
    "# Better Stats autostart v2\n"
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
    "case \"$1\" in\n"
    "    *.fb2|*.fb2.zip|*.fb2.gz) fmt=fb2 ;;\n"
    "    *.[cC][bB][zZ]) fmt=cbz ;;\n"
    "esac\n"
    "\n"
    "reader=\"\"\n"
    "apps=$(grep -i \"^$fmt:\" \"$cfg\" 2>/dev/null | head -n 1 | cut -d: -f4)\n"
    "after_self=0\n"
    "IFS=,\n"
    "for name in $apps; do\n"
    "    if [ \"$after_self\" = 0 ]; then\n"
    "        [ \"$name\" = \"$self\" ] && after_self=1\n"
    "        continue\n"
    "    fi\n"
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

static const char *formats[] = {"epub", "fb2", "cbz"};
#define FORMAT_COUNT (sizeof(formats) / sizeof(formats[0]))

static int active_config(char **data, size_t *size)
{
    return read_file(exists(USER_EXTENSIONS) ? USER_EXTENSIONS : SYSTEM_EXTENSIONS,
                     data, size);
}

static int append_entry(char **data, size_t *size, const char *entry,
                        size_t entry_size)
{
    size_t separator = *size && (*data)[*size - 1] != '\n';
    if (*size + separator + entry_size > MAX_CONFIG_SIZE)
        return 0;
    char *grown = realloc(*data, *size + separator + entry_size + 1);
    if (!grown)
        return 0;
    *data = grown;
    if (separator)
        (*data)[(*size)++] = '\n';
    memcpy(*data + *size, entry, entry_size);
    *size += entry_size;
    (*data)[*size] = '\0';
    return 1;
}

/* PocketBook treats the user file as a per-format override. KOReader can
 * therefore leave it with only #koreader (or only the formats it owns). Fill
 * just our missing formats from the firmware file and preserve everything the
 * user file already contains. */
static int complete_config(const char *input, size_t input_size,
                           char **data, size_t *size)
{
    char *system = NULL;
    size_t system_size = 0;
    *data = malloc(input_size + 1);
    if (!*data)
        return 0;
    memcpy(*data, input, input_size);
    (*data)[input_size] = '\0';
    *size = input_size;

    size_t i;
    for (i = 0; i < FORMAT_COUNT; ++i) {
        handler_config_result current;
        patch_handler_config(*data, *size, formats[i], HANDLER_NAME, 0,
                             &current);
        if (current.ok) {
            free_handler_config(&current);
            continue;
        }
        int malformed = current.entry_found;
        free_handler_config(&current);
        if (malformed)
            goto failure;
        if (!system && !read_file(SYSTEM_EXTENSIONS, &system, &system_size))
            goto failure;

        handler_config_result fallback;
        patch_handler_config(system, system_size, formats[i], HANDLER_NAME, 0,
                             &fallback);
        if (!fallback.ok) {
            int invalid = fallback.entry_found;
            free_handler_config(&fallback);
            if (invalid)
                goto failure;
            continue;
        }
        int ok = append_entry(data, size, system + fallback.entry_start,
                              fallback.entry_size);
        free_handler_config(&fallback);
        if (!ok)
            goto failure;
    }
    free(system);
    return 1;

failure:
    free(system);
    free(*data);
    *data = NULL;
    *size = 0;
    return 0;
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

static int preference_enabled(void)
{
    return access(AUTOSTART_DISABLED, F_OK) != 0;
}

static int write_preference(int enabled)
{
    if (enabled)
        return unlink(AUTOSTART_DISABLED) == 0 || errno == ENOENT;
    mkdir(STATS_DIR, 0755);
    FILE *file = fopen(AUTOSTART_DISABLED, "wb");
    if (!file)
        return 0;
    if (fclose(file) == 0)
        return 1;
    unlink(AUTOSTART_DISABLED);
    return 0;
}

void bs_autostart_get(bs_autostart_status *out)
{
    char *raw = NULL, *config = NULL;
    size_t raw_size = 0, config_size = 0;
    char *handler = NULL;
    size_t handler_size = 0;
    memset(out, 0, sizeof(*out));
    if (!active_config(&raw, &raw_size)
        || !complete_config(raw, raw_size, &config, &config_size)) {
        free(raw);
        message(out, "Handler configuration is unavailable");
        return;
    }
    free(raw);
    int supported = 0, ready = 0, any_present = 0;
    int any_koreader = 0, any_other_reader = 0;
    size_t i;
    for (i = 0; i < FORMAT_COUNT; ++i) {
        handler_config_result parsed;
        patch_handler_config(config, config_size, formats[i],
                             HANDLER_NAME, 0, &parsed);
        if (parsed.ok && *parsed.stock_handler) {
            ++supported;
            if (parsed.handler_present) any_present = 1;
            if (parsed.handler_ready) ++ready;
            if (parsed.koreader_present) any_koreader = 1;
            if (parsed.other_reader_present) any_other_reader = 1;
        }
        free_handler_config(&parsed);
    }
    free(config);
    if (!supported) {
        message(out, "Handler configuration is unavailable");
        return;
    }
    int handler_exists = installed_handler(&handler, &handler_size);
    int owned = handler_exists && contains(handler, handler_size, "# Better Stats");
    int current = handler_exists && contains(handler, handler_size, HANDLER_MARKER);
    free(handler);
    out->enabled = ready == supported && current;
    out->available = !handler_exists || owned;
    if (handler_exists && !owned)
        message(out, "Handler path is already in use");
    else if (any_koreader)
        message(out, "KOReader association detected");
    else if (any_other_reader)
        message(out, "Another reader is registered");
    else if (any_present && !out->enabled)
        message(out, "Better Stats handler needs updating");
}

static int apply_autostart(int enabled, bs_autostart_status *out)
{
    char *original = NULL, *complete = NULL;
    size_t original_size = 0, complete_size = 0;
    char *handler = NULL;
    size_t handler_size = 0;
    const char *failure = NULL;
    handler_config_result results[FORMAT_COUNT];
    memset(results, 0, sizeof(results));
    memset(out, 0, sizeof(*out));
    if (!active_config(&original, &original_size)) {
        message(out, "Handler configuration is unavailable");
        return 0;
    }
    const char *input = original;
    size_t input_size = original_size;
    int any_changed = 0, supported = 0;
    if (enabled) {
        if (!complete_config(original, original_size, &complete,
                             &complete_size)) {
            failure = "Handler configuration is unavailable";
            goto cleanup;
        }
        input = complete;
        input_size = complete_size;
        any_changed = complete_size != original_size
            || memcmp(complete, original, original_size) != 0;
    }
    size_t i;
    for (i = 0; i < FORMAT_COUNT; ++i) {
        patch_handler_config(input, input_size, formats[i],
                             HANDLER_NAME, enabled, &results[i]);
        if (results[i].ok) {
            ++supported;
            if (results[i].changed) any_changed = 1;
            input = results[i].output;
            input_size = results[i].output_size;
        }
    }
    if (enabled && !supported) {
        failure = "Handler configuration is unavailable";
        goto cleanup;
    }
    int handler_exists = installed_handler(&handler, &handler_size);
    int owned = handler_exists && contains(handler, handler_size, "# Better Stats");
    free(handler);
    if (enabled && handler_exists && !owned) {
        failure = "Handler path is already in use";
        goto cleanup;
    }
    if (enabled && !write_handler()) {
        failure = "Could not install handler";
        goto cleanup;
    }
    if (any_changed) {
        if (!exists(EXTENSIONS_BACKUP)
            && !write_file(EXTENSIONS_BACKUP, original, original_size)) {
            failure = "Could not create extensions.cfg backup";
            goto cleanup;
        }
        if (!write_file(USER_EXTENSIONS, input, input_size)) {
            failure = "Could not write extensions.cfg";
            goto cleanup;
        }
    }
    if (!enabled && owned)
        unlink(HANDLER_PATH);
    free(original);
    free(complete);
    for (i = 0; i < FORMAT_COUNT; ++i)
        free_handler_config(&results[i]);
    bs_autostart_get(out);
    return out->enabled == !!enabled;
cleanup:
    free(original);
    free(complete);
    for (i = 0; i < FORMAT_COUNT; ++i)
        free_handler_config(&results[i]);
    bs_autostart_get(out);
    message(out, failure);
    return 0;
}

void bs_autostart_set(int enabled, bs_autostart_status *out)
{
    if (!write_preference(enabled)) {
        bs_autostart_get(out);
        message(out, "Could not save autostart setting");
        return;
    }
    apply_autostart(enabled, out);
}

int bs_autostart_prepare(bs_autostart_status *out)
{
    return apply_autostart(preference_enabled(), out);
}
