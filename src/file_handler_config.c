#include "file_handler_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *start;
    size_t length;
} slice;

static int slice_equal(slice value, const char *text)
{
    size_t length = strlen(text);
    return value.length == length && memcmp(value.start, text, length) == 0;
}

static int slice_equal_fold(slice value, const char *text)
{
    size_t length = strlen(text);
    size_t i;
    if (value.length != length)
        return 0;
    for (i = 0; i < length; ++i)
        if (tolower((unsigned char)value.start[i])
            != tolower((unsigned char)text[i]))
            return 0;
    return 1;
}

static int stock_reader(slice app)
{
    static const char prefix[] = "eink-reader";
    static const char suffix[] = ".app";
    size_t i;
    if (app.length < sizeof(prefix) - 1 + sizeof(suffix) - 1
        || memcmp(app.start, prefix, sizeof(prefix) - 1)
        || memcmp(app.start + app.length - sizeof(suffix) + 1,
                  suffix, sizeof(suffix) - 1))
        return 0;
    for (i = 0; i < app.length; ++i) {
        unsigned char c = (unsigned char)app.start[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.')
            return 0;
    }
    return 1;
}

static void set_error(handler_config_result *out, const char *message)
{
    snprintf(out->error, sizeof(out->error), "%s", message);
}

void free_handler_config(handler_config_result *result)
{
    if (!result)
        return;
    free(result->output);
    memset(result, 0, sizeof(*result));
}

int patch_handler_config(const char *input, size_t input_size,
                         const char *format, const char *handler_name,
                         int enable, handler_config_result *out)
{
    size_t line_start = 0;
    memset(out, 0, sizeof(*out));
    out->output = malloc(input_size + strlen(handler_name) + 2);
    if (!out->output) {
        set_error(out, "Out of memory");
        return -1;
    }
    memcpy(out->output, input, input_size);
    out->output[input_size] = '\0';
    out->output_size = input_size;

    while (line_start < input_size) {
        const char *newline = memchr(input + line_start, '\n', input_size - line_start);
        size_t line_end = newline ? (size_t)(newline - input) : input_size;
        size_t content_end = line_end;
        if (content_end > line_start && input[content_end - 1] == '\r')
            --content_end;
        const char *line = input + line_start;
        size_t line_length = content_end - line_start;
        const char *c1 = memchr(line, ':', line_length);
        size_t fmt_len = strlen(format);
        if (c1 && (size_t)(c1 - line) == fmt_len
            && slice_equal_fold((slice){line, fmt_len}, format)) {
            out->entry_found = 1;
            out->entry_start = line_start;
            out->entry_size = (newline ? line_end + 1 : line_end) - line_start;
            const char *end = line + line_length;
            const char *c2 = memchr(c1 + 1, ':', (size_t)(end - c1 - 1));
            const char *c3 = c2 ? memchr(c2 + 1, ':', (size_t)(end - c2 - 1)) : NULL;
            const char *c4 = c3 ? memchr(c3 + 1, ':', (size_t)(end - c3 - 1)) : NULL;
            if (!c4) {
                set_error(out, "Malformed config entry");
                return -1;
            }

            slice apps[64];
            size_t app_count = 0;
            const char *at = c3 + 1;
            while (at <= c4 && app_count < sizeof(apps) / sizeof(apps[0])) {
                const char *comma = memchr(at, ',', (size_t)(c4 - at));
                const char *app_end = comma ? comma : c4;
                if (app_end > at)
                    apps[app_count++] = (slice){at, (size_t)(app_end - at)};
                if (!comma)
                    break;
                at = comma + 1;
            }
            if (at < c4 && app_count == sizeof(apps) / sizeof(apps[0])) {
                set_error(out, "Too many handlers");
                return -1;
            }

            size_t i, stock_index = app_count, handler_count = 0;
            for (i = 0; i < app_count; ++i) {
                if (slice_equal(apps[i], handler_name)) {
                    out->handler_present = 1;
                    ++handler_count;
                }
                if (stock_index == app_count && stock_reader(apps[i])) {
                    stock_index = i;
                    snprintf(out->stock_handler, sizeof(out->stock_handler), "%.*s",
                             (int)apps[i].length, apps[i].start);
                }
            }
            for (i = 0; i < stock_index; ++i) {
                if (slice_equal(apps[i], handler_name))
                    continue;
                if (slice_equal_fold(apps[i], "koreader.app")
                    || slice_equal_fold(apps[i], "koreader"))
                    out->koreader_present = 1;
                else
                    out->other_reader_present = 1;
            }
            out->handler_ready = handler_count == 1 && stock_index > 0
                && slice_equal(apps[stock_index - 1], handler_name);
            if (enable && !*out->stock_handler) {
                set_error(out, "No native reader found");
                return -1;
            }

            char joined[4096];
            size_t joined_size = 0;
            for (i = 0; i < app_count; ++i) {
                if (slice_equal(apps[i], handler_name))
                    continue;
                if (enable && i == stock_index) {
                    size_t n = strlen(handler_name);
                    if (joined_size)
                        joined[joined_size++] = ',';
                    if (joined_size + n >= sizeof(joined)) {
                        set_error(out, "Handler entry is too long");
                        return -1;
                    }
                    memcpy(joined + joined_size, handler_name, n);
                    joined_size += n;
                }
                if (joined_size)
                    joined[joined_size++] = ',';
                if (joined_size + apps[i].length >= sizeof(joined)) {
                    set_error(out, "Handler entry is too long");
                    return -1;
                }
                memcpy(joined + joined_size, apps[i].start, apps[i].length);
                joined_size += apps[i].length;
            }

            size_t app_start = (size_t)(c3 + 1 - input);
            size_t app_end = (size_t)(c4 - input);
            out->changed = joined_size != app_end - app_start
                || memcmp(joined, input + app_start, joined_size) != 0;
            if (out->changed) {
                size_t suffix = input_size - app_end;
                size_t new_size = app_start + joined_size + suffix;
                char *grown = realloc(out->output, new_size + 1);
                if (!grown) {
                    set_error(out, "Out of memory");
                    return -1;
                }
                out->output = grown;
                memmove(out->output + app_start + joined_size,
                        input + app_end, suffix);
                memcpy(out->output + app_start, joined, joined_size);
                out->output[new_size] = '\0';
                out->output_size = new_size;
            }
            out->ok = 1;
            return 0;
        }
        if (!newline)
            break;
        line_start = line_end + 1;
    }
    set_error(out, "No entry found");
    return -1;
}
