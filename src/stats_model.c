#include "stats_model.h"

#include "daemon.h"
#include "stats_db.h"
#include "tracker.h"
#include "miniz.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BS_COVER_LIMIT (20U * 1024U * 1024U)

struct bs_context {
    sqlite3 *stats;
    char explorer_path[BS_PATH_MAX];
};

typedef struct {
    int year;
    int month;
    int day;
} bs_date;

typedef struct {
    bs_date day;
    char title[BS_TITLE_MAX];
    char words[BS_TITLE_MAX];
    char cover_path[BS_PATH_MAX];
    int has_file;
} finished_book;

typedef struct {
    finished_book *items;
    size_t count;
} finished_list;

static void clear_error(bs_error *error)
{
    if (error)
        memset(error, 0, sizeof(*error));
}

static int fail(bs_error *error, int code, const char *message)
{
    if (error) {
        error->code = code;
        snprintf(error->message, sizeof(error->message), "%s",
                 message ? message : "Unknown error");
    }
    return -1;
}

static int sql_fail(bs_error *error, sqlite3 *db, const char *prefix)
{
    char message[BS_MESSAGE_MAX];
    snprintf(message, sizeof(message), "%s: %s", prefix,
             db ? sqlite3_errmsg(db) : "database unavailable");
    return fail(error, 2, message);
}

static int file_exists(const char *path)
{
    return path && *path && access(path, F_OK) == 0;
}

static sqlite3 *open_explorer(const bs_context *context)
{
    sqlite3 *db = NULL;
    if (!context || sqlite3_open_v2(context->explorer_path, &db,
                                    SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 1000);
    return db;
}

static int is_leap(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* v1.3.1: CBZ covers were extracted from a random ZIP entry instead of the
 * alphabetically first image.  Delete cached covers for CBZ books that still
 * exist on the device so resolve_cover re-extracts them with the fixed order. */
static void invalidate_cbz_covers(const bs_context *context)
{
    static const char marker[] = STATS_DIR "/cbz-cover-fix";
    if (file_exists(marker))
        return;
    sqlite3 *db = open_explorer(context);
    if (!db)
        return;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT lower(hex(f.fast_hash))"
        " FROM files f WHERE lower(f.filename) LIKE '%.cbz'";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        static const char *exts[] = {".png", ".jpg", ".gif", ".bmp", ".img"};
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *key = (const char *)sqlite3_column_text(st, 0);
            if (!key || !*key)
                continue;
            char path[BS_PATH_MAX];
            size_t e;
            for (e = 0; e < sizeof(exts) / sizeof(exts[0]); ++e) {
                snprintf(path, sizeof(path), COVER_CACHE_DIR "/%s%s", key, exts[e]);
                unlink(path);
            }
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    FILE *f = fopen(marker, "w");
    if (f)
        fclose(f);
}

static int days_in_month(int year, int month)
{
    static const unsigned char days[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    return days[month - 1] + (month == 2 && is_leap(year));
}

static int day_of_year(bs_date date)
{
    int out = date.day;
    int month;
    for (month = 1; month < date.month; ++month)
        out += days_in_month(date.year, month);
    return out;
}

static bs_date date_from_day(int year, int day)
{
    bs_date out = {year, 1, day};
    while (out.month < 12 && out.day > days_in_month(year, out.month)) {
        out.day -= days_in_month(year, out.month);
        ++out.month;
    }
    return out;
}

/* Sakamoto's algorithm, converted to Monday=0. */
static int weekday(bs_date date)
{
    static const int offset[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int year = date.year - (date.month < 3);
    int sunday_zero =
        (year + year / 4 - year / 100 + year / 400
         + offset[date.month - 1] + date.day) % 7;
    return (sunday_zero + 6) % 7;
}

static int parse_date(const char *text, bs_date *out)
{
    if (!text || strlen(text) != 10 || text[4] != '-' || text[7] != '-')
        return 0;
    int i;
    for (i = 0; i < 10; ++i)
        if (i != 4 && i != 7 && !isdigit((unsigned char)text[i]))
            return 0;
    out->year = (text[0] - '0') * 1000 + (text[1] - '0') * 100
              + (text[2] - '0') * 10 + text[3] - '0';
    out->month = (text[5] - '0') * 10 + text[6] - '0';
    out->day = (text[8] - '0') * 10 + text[9] - '0';
    return out->month >= 1 && out->month <= 12 && out->day >= 1
        && out->day <= days_in_month(out->year, out->month);
}

static void format_date(char out[11], bs_date date)
{
    snprintf(out, 11, "%04d-%02d-%02d", date.year, date.month, date.day);
}

static int current_year(void)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return local.tm_year + 1900;
}

static void normalize_title(const char *title, char out[BS_TITLE_MAX])
{
    size_t at = 0;
    int space = 1;
    const unsigned char *p = (const unsigned char *)title;
    while (*p && at + 1 < BS_TITLE_MAX) {
        unsigned char c = *p++;
        if (c >= 0x80 || isalnum(c)) {
            out[at++] = c < 0x80 ? (char)tolower(c) : (char)c;
            space = 0;
        } else if (!space) {
            out[at++] = ' ';
            space = 1;
        }
    }
    if (at > 0 && out[at - 1] == ' ')
        --at;
    out[at] = '\0';
}

static int same_book(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t short_len = alen < blen ? alen : blen;
    const char *longer = alen < blen ? b : a;
    return short_len > 0 && memcmp(a, b, short_len) == 0
        && (alen == blen || longer[short_len] == ' ');
}

static int safe_cache_key(const char *key)
{
    const unsigned char *p = (const unsigned char *)key;
    if (!key || !*key)
        return 0;
    for (; *p; ++p)
        if (!isalnum(*p) && *p != '_' && *p != '-')
            return 0;
    return 1;
}

static int safe_zip_path(const char *path)
{
    const char *segment = path;
    const char *p;
    if (!path || !*path || path[0] == '/' || path[0] == '\\'
        || strchr(path, '\\') || strchr(path, ':'))
        return 0;
    for (p = path; ; ++p) {
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - segment);
            if (len == 2 && segment[0] == '.' && segment[1] == '.')
                return 0;
            if (*p == '\0')
                break;
            segment = p + 1;
        }
    }
    return 1;
}

static int normalized_zip_path(const char *opf, const char *href,
                               char out[BS_PATH_MAX])
{
    if (!href || !*href || *href == '/' || *href == '\\'
        || strchr(href, '\\') || strchr(href, ':'))
        return 0;
    const char *slash = strrchr(opf, '/');
    char joined[BS_PATH_MAX * 2];
    int n = slash
        ? snprintf(joined, sizeof(joined), "%.*s/%s", (int)(slash - opf), opf, href)
        : snprintf(joined, sizeof(joined), "%s", href);
    if (n < 0 || (size_t)n >= sizeof(joined))
        return 0;

    size_t length = 0;
    const char *at = joined;
    while (*at) {
        const char *end = strchr(at, '/');
        size_t segment = end ? (size_t)(end - at) : strlen(at);
        if (segment == 2 && at[0] == '.' && at[1] == '.') {
            if (!length)
                return 0;
            while (length && out[length - 1] != '/')
                --length;
            if (length)
                --length;
        } else if (segment && !(segment == 1 && at[0] == '.')) {
            if (length && length + 1 < BS_PATH_MAX)
                out[length++] = '/';
            if (length + segment >= BS_PATH_MAX)
                return 0;
            memcpy(out + length, at, segment);
            length += segment;
        }
        if (!end)
            break;
        at = end + 1;
    }
    out[length] = '\0';
    return safe_zip_path(out);
}

static void xml_decode(char *text)
{
    struct entity { const char *from; char to; } entities[] = {
        {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''},
        {"&lt;", '<'}, {"&gt;", '>'}
    };
    char *read = text;
    char *write = text;
    while (*read) {
        size_t i;
        int matched = 0;
        for (i = 0; i < sizeof(entities) / sizeof(entities[0]); ++i) {
            size_t n = strlen(entities[i].from);
            if (strncmp(read, entities[i].from, n) == 0) {
                *write++ = entities[i].to;
                read += n;
                matched = 1;
                break;
            }
        }
        if (!matched)
            *write++ = *read++;
    }
    *write = '\0';
}

static int xml_attr(const char *tag, size_t tag_len, const char *name,
                    char *out, size_t out_size)
{
    const char *p = tag;
    const char *end = tag + tag_len;
    size_t name_len = strlen(name);
    while (p < end) {
        while (p < end && isspace((unsigned char)*p))
            ++p;
        const char *start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-'
                           || *p == '_' || *p == ':'))
            ++p;
        if ((size_t)(p - start) != name_len || memcmp(start, name, name_len)) {
            while (p < end && !isspace((unsigned char)*p))
                ++p;
            continue;
        }
        while (p < end && isspace((unsigned char)*p))
            ++p;
        if (p >= end || *p++ != '=')
            continue;
        while (p < end && isspace((unsigned char)*p))
            ++p;
        if (p >= end || (*p != '\'' && *p != '"'))
            continue;
        char quote = *p++;
        start = p;
        while (p < end && *p != quote)
            ++p;
        if (p >= end)
            return 0;
        size_t n = (size_t)(p - start);
        if (n >= out_size)
            n = out_size - 1;
        memcpy(out, start, n);
        out[n] = '\0';
        xml_decode(out);
        return 1;
    }
    return 0;
}

static int local_tag_is(const char *tag, size_t tag_len, const char *name)
{
    const char *p = tag;
    const char *end = tag + tag_len;
    const char *local = tag;
    while (p < end && !isspace((unsigned char)*p) && *p != '/' && *p != '>') {
        if (*p == ':')
            local = p + 1;
        ++p;
    }
    return (size_t)(p - local) == strlen(name)
        && memcmp(local, name, strlen(name)) == 0;
}

static int find_first_attr(const char *xml, size_t length, const char *tag_name,
                           const char *attr, char *out, size_t out_size)
{
    const char *p = xml;
    const char *end = xml + length;
    while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
        const char *close = memchr(p, '>', (size_t)(end - p));
        if (!close)
            break;
        ++p;
        if (p < close && *p != '/' && *p != '!' && *p != '?'
            && local_tag_is(p, (size_t)(close - p), tag_name)
            && xml_attr(p, (size_t)(close - p), attr, out, out_size))
            return 1;
        p = close + 1;
    }
    return 0;
}

static void *zip_entry(mz_zip_archive *zip, const char *name, size_t *size)
{
    int index = mz_zip_reader_locate_file(zip, name, NULL, 0);
    mz_zip_archive_file_stat stat;
    if (index < 0 || !mz_zip_reader_file_stat(zip, (mz_uint)index, &stat)
        || stat.m_uncomp_size > BS_COVER_LIMIT)
        return NULL;
    return mz_zip_reader_extract_to_heap(zip, (mz_uint)index, size, 0);
}

static int opf_cover_href(const char *xml, size_t length, char *href,
                          size_t href_size)
{
    char cover_id[128] = "";
    const char *p = xml;
    const char *end = xml + length;
    while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
        const char *close = memchr(p, '>', (size_t)(end - p));
        if (!close)
            break;
        ++p;
        size_t tag_len = (size_t)(close - p);
        if (local_tag_is(p, tag_len, "meta")) {
            char name[64];
            if (xml_attr(p, tag_len, "name", name, sizeof(name))
                && strcmp(name, "cover") == 0)
                xml_attr(p, tag_len, "content", cover_id, sizeof(cover_id));
        } else if (local_tag_is(p, tag_len, "item")) {
            char props[256] = "";
            char item_href[BS_PATH_MAX] = "";
            xml_attr(p, tag_len, "properties", props, sizeof(props));
            xml_attr(p, tag_len, "href", item_href, sizeof(item_href));
            if (*item_href && strstr(props, "cover-image")) {
                snprintf(href, href_size, "%s", item_href);
                return 1;
            }
        }
        p = close + 1;
    }

    p = xml;
    while (*cover_id && p < end
           && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
        const char *close = memchr(p, '>', (size_t)(end - p));
        if (!close)
            break;
        ++p;
        size_t tag_len = (size_t)(close - p);
        if (local_tag_is(p, tag_len, "item")) {
            char id[128] = "";
            xml_attr(p, tag_len, "id", id, sizeof(id));
            if (strcmp(id, cover_id) == 0
                && xml_attr(p, tag_len, "href", href, href_size))
                return 1;
        }
        p = close + 1;
    }

    /* Common malformed EPUB fallback: image item whose id contains cover. */
    p = xml;
    while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
        const char *close = memchr(p, '>', (size_t)(end - p));
        if (!close)
            break;
        ++p;
        size_t tag_len = (size_t)(close - p);
        if (local_tag_is(p, tag_len, "item")) {
            char id[128] = "";
            char type[64] = "";
            xml_attr(p, tag_len, "id", id, sizeof(id));
            xml_attr(p, tag_len, "media-type", type, sizeof(type));
            size_t i;
            for (i = 0; id[i]; ++i)
                id[i] = (char)tolower((unsigned char)id[i]);
            if (strstr(id, "cover") && strncmp(type, "image/", 6) == 0
                && xml_attr(p, tag_len, "href", href, href_size))
                return 1;
        }
        p = close + 1;
    }
    return 0;
}

static int write_cover(const char *path, const void *data, size_t size)
{
    char temp[BS_PATH_MAX + 32];
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    FILE *file = fopen(temp, "wb");
    if (!file)
        return 0;
    int ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
    if (fclose(file) != 0)
        ok = 0;
    if (ok)
        ok = rename(temp, path) == 0;
    if (!ok)
        unlink(temp);
    return ok;
}

static const char *cover_extension(const char *href)
{
    const char *dot = strrchr(href, '.');
    if (!dot)
        return ".img";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))
        return ".jpg";
    if (!strcasecmp(dot, ".png"))
        return ".png";
    if (!strcasecmp(dot, ".gif"))
        return ".gif";
    if (!strcasecmp(dot, ".bmp"))
        return ".bmp";
    return ".img";
}

static int cached_cover(const char *key, char out[BS_PATH_MAX])
{
    static const char *extensions[] = {".png", ".jpg", ".gif", ".bmp", ".img"};
    size_t i;
    for (i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        snprintf(out, BS_PATH_MAX, COVER_CACHE_DIR "/%s%s", key, extensions[i]);
        if (file_exists(out))
            return 1;
    }
    out[0] = '\0';
    return 0;
}

static int extract_epub_cover(const char *epub, const char *key,
                              char out[BS_PATH_MAX])
{
    if (!file_exists(epub) || !safe_cache_key(key))
        return 0;
    if (cached_cover(key, out))
        return 1;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, epub, 0))
        return 0;

    size_t container_size = 0;
    char *container = zip_entry(&zip, "META-INF/container.xml", &container_size);
    char opf[BS_PATH_MAX] = "";
    int ok = container
        && find_first_attr(container, container_size, "rootfile", "full-path",
                           opf, sizeof(opf))
        && safe_zip_path(opf);
    mz_free(container);

    size_t opf_size = 0;
    char *opf_xml = ok ? zip_entry(&zip, opf, &opf_size) : NULL;
    char href[BS_PATH_MAX] = "";
    ok = opf_xml && opf_cover_href(opf_xml, opf_size, href, sizeof(href));
    mz_free(opf_xml);

    char entry[BS_PATH_MAX] = "";
    if (ok) {
        char *fragment = strchr(href, '#');
        if (fragment)
            *fragment = '\0';
        ok = normalized_zip_path(opf, href, entry);
    }

    size_t image_size = 0;
    void *image = ok ? zip_entry(&zip, entry, &image_size) : NULL;
    if (!image && ok && strcmp(entry, href) != 0)
        image = zip_entry(&zip, href, &image_size);
    ok = image != NULL && image_size > 0;
    if (ok) {
        mkdir(STATS_DIR, 0755);
        mkdir(COVER_CACHE_DIR, 0755);
        snprintf(out, BS_PATH_MAX, COVER_CACHE_DIR "/%s%s", key,
                 cover_extension(href));
        ok = write_cover(out, image, image_size);
    }
    mz_free(image);
    mz_zip_reader_end(&zip);
    if (!ok)
        out[0] = '\0';
    return ok;
}

static int extract_cbz_cover(const char *cbz, const char *key,
                             char out[BS_PATH_MAX])
{
    if (!file_exists(cbz) || !safe_cache_key(key))
        return 0;
    if (cached_cover(key, out))
        return 1;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, cbz, 0))
        return 0;

    int ok = 0;
    mz_uint i, best_idx = 0;
    int have_best = 0;
    char best_name[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE] = {0};
    const char *best_ext = NULL;
    for (i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory
            || !stat.m_is_supported || stat.m_uncomp_size == 0
            || stat.m_uncomp_size > BS_COVER_LIMIT)
            continue;
        const char *extension = cover_extension(stat.m_filename);
        if (!strcmp(extension, ".img"))
            continue;
        if (!have_best || strcasecmp(stat.m_filename, best_name) < 0) {
            best_idx = i;
            have_best = 1;
            snprintf(best_name, sizeof(best_name), "%s", stat.m_filename);
            best_ext = extension;
        }
    }
    if (have_best) {
        size_t image_size = 0;
        void *image = mz_zip_reader_extract_to_heap(&zip, best_idx,
                                                     &image_size, 0);
        if (image && image_size) {
            mkdir(STATS_DIR, 0755);
            mkdir(COVER_CACHE_DIR, 0755);
            snprintf(out, BS_PATH_MAX, COVER_CACHE_DIR "/%s%s", key, best_ext);
            ok = write_cover(out, image, image_size);
        }
        mz_free(image);
    }
    mz_zip_reader_end(&zip);
    if (!ok)
        out[0] = '\0';
    return ok;
}

static size_t base64_decode(const char *src, size_t src_len,
                            unsigned char *dst, size_t dst_cap)
{
    static const unsigned char table[256] = {
        ['A']=0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
        ['a']=26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,
        ['0']=52,53,54,55,56,57,58,59,60,61, ['+']=62, ['/']=63
    };
    size_t out = 0;
    unsigned int buf = 0;
    int bits = 0;
    size_t i;
    for (i = 0; i < src_len && out < dst_cap; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '=' || c == '\0') break;
        if (isspace(c)) continue;
        if (!isalnum(c) && c != '+' && c != '/') break;
        buf = (buf << 6) | table[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[out++] = (unsigned char)(buf >> bits);
        }
    }
    return out;
}

static int read_plain_file(const char *path, char **data, size_t *size)
{
    struct stat st;
    *data = NULL;
    *size = 0;
    if (stat(path, &st) != 0 || st.st_size <= 0
        || (size_t)st.st_size > BS_COVER_LIMIT)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    *data = malloc((size_t)st.st_size + 1);
    if (!*data) { fclose(f); return 0; }
    *size = fread(*data, 1, (size_t)st.st_size, f);
    fclose(f);
    if (*size != (size_t)st.st_size) {
        free(*data); *data = NULL; *size = 0; return 0;
    }
    (*data)[*size] = '\0';
    return 1;
}

static int extract_fb2_cover(const char *path, const char *key,
                             char out[BS_PATH_MAX])
{
    if (!file_exists(path) || !safe_cache_key(key))
        return 0;
    if (cached_cover(key, out))
        return 1;

    char *xml = NULL;
    size_t xml_size = 0;
    const char *dot = strrchr(path, '.');
    int is_zip = dot && (!strcasecmp(dot, ".zip") || !strcasecmp(dot, ".gz"));

    if (is_zip) {
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_file(&zip, path, 0))
            return 0;
        int count = (int)mz_zip_reader_get_num_files(&zip);
        int i;
        for (i = 0; i < count; ++i) {
            char name[BS_PATH_MAX];
            mz_zip_reader_get_filename(&zip, (mz_uint)i, name, sizeof(name));
            size_t n = strlen(name);
            if (n >= 4 && !strcasecmp(name + n - 4, ".fb2")) {
                xml = zip_entry(&zip, name, &xml_size);
                break;
            }
        }
        mz_zip_reader_end(&zip);
    } else {
        read_plain_file(path, &xml, &xml_size);
    }
    if (!xml || !xml_size)
        return 0;

    /* Find <image ... href="#id"/> inside <coverpage> */
    char cover_id[256] = "";
    const char *cp = xml;
    const char *cp_end = NULL;
    const char *end = xml + xml_size;
    while (cp < end && (cp = memchr(cp, '<', (size_t)(end - cp))) != NULL) {
        const char *close = memchr(cp, '>', (size_t)(end - cp));
        if (!close) break;
        ++cp;
        if (local_tag_is(cp, (size_t)(close - cp), "coverpage")) {
            cp_end = close + 1;
            break;
        }
        cp = close + 1;
    }
    if (cp_end) {
        const char *limit = end;
        /* Scan forward for </coverpage> to bound the search */
        const char *s = cp_end;
        while (s < end && (s = memchr(s, '<', (size_t)(end - s))) != NULL) {
            if (s + 1 < end && s[1] == '/'
                && local_tag_is(s + 2,
                                (size_t)(end - s - 2 > 64 ? 64 : end - s - 2),
                                "coverpage")) {
                limit = s;
                break;
            }
            const char *c = memchr(s + 1, '>', (size_t)(end - s - 1));
            s = c ? c + 1 : end;
        }
        s = cp_end;
        while (s < limit && (s = memchr(s, '<', (size_t)(limit - s))) != NULL) {
            const char *close = memchr(s, '>', (size_t)(limit - s));
            if (!close) break;
            ++s;
            if (local_tag_is(s, (size_t)(close - s), "image")) {
                if (!xml_attr(s, (size_t)(close - s), "l:href", cover_id, sizeof(cover_id)))
                    xml_attr(s, (size_t)(close - s), "xlink:href", cover_id, sizeof(cover_id));
                break;
            }
            s = close + 1;
        }
    }

    /* Strip leading '#' */
    const char *binary_id = cover_id;
    if (*binary_id == '#') ++binary_id;

    int ok = 0;
    if (*binary_id) {
        /* Find <binary id="binary_id" ...>base64</binary> */
        const char *p = xml;
        while (p < end && (p = memchr(p, '<', (size_t)(end - p))) != NULL) {
            const char *close = memchr(p, '>', (size_t)(end - p));
            if (!close) break;
            ++p;
            if (*p == '/' || *p == '!' || *p == '?') { p = close + 1; continue; }
            if (!local_tag_is(p, (size_t)(close - p), "binary")) {
                p = close + 1; continue;
            }
            char id[256] = "";
            xml_attr(p, (size_t)(close - p), "id", id, sizeof(id));
            if (strcmp(id, binary_id) != 0) { p = close + 1; continue; }
            char content_type[64] = "";
            xml_attr(p, (size_t)(close - p), "content-type", content_type,
                     sizeof(content_type));
            const char *b64_start = close + 1;
            const char *b64_end = memchr(b64_start, '<', (size_t)(end - b64_start));
            if (!b64_end) break;
            size_t b64_len = (size_t)(b64_end - b64_start);
            size_t max_decoded = b64_len * 3 / 4 + 4;
            if (max_decoded > BS_COVER_LIMIT) break;
            unsigned char *image = malloc(max_decoded);
            if (!image) break;
            size_t image_size = base64_decode(b64_start, b64_len, image, max_decoded);
            if (image_size > 0) {
                mkdir(STATS_DIR, 0755);
                mkdir(COVER_CACHE_DIR, 0755);
                const char *ext = ".img";
                if (strstr(content_type, "jpeg") || strstr(content_type, "jpg"))
                    ext = ".jpg";
                else if (strstr(content_type, "png"))
                    ext = ".png";
                snprintf(out, BS_PATH_MAX, COVER_CACHE_DIR "/%s%s", key, ext);
                ok = write_cover(out, image, image_size);
            }
            free(image);
            break;
        }
    }
    free(xml);
    if (!ok) out[0] = '\0';
    return ok;
}

static int remembered_cover_key(sqlite3 *stats, const char *title,
                                char key[128])
{
    sqlite3_stmt *statement = NULL;
    key[0] = '\0';
    const char *sql =
        "SELECT cover FROM books WHERE lower(trim(title))=lower(trim(?1))"
        " AND cover IS NOT NULL AND cover<>'' LIMIT 1";
    if (!stats || sqlite3_prepare_v2(stats, sql, -1, &statement, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(statement, 1, title, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) == SQLITE_ROW)
        snprintf(key, 128, "%s", sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    return *key != '\0';
}

static void resolve_cover(sqlite3 *explorer, sqlite3 *stats, const char *title,
                          char out[BS_PATH_MAX], char source[BS_PATH_MAX])
{
    sqlite3_stmt *statement = NULL;
    char remembered[128] = "";
    char fallback[BS_PATH_MAX] = "";
    out[0] = '\0';
    if (source)
        source[0] = '\0';
    if (!title || !*title)
        return;

    const char *sql =
        "SELECT IFNULL(fo.name,'')||'/'||f.filename,"
        " lower(hex(f.fast_hash)),"
        " f.storageid||lower(hex(f.fast_hash))"
        " FROM books_impl b JOIN files f ON f.book_id=b.id"
        " LEFT JOIN folders fo ON fo.id=f.folder_id"
        " WHERE lower(trim(b.title))=lower(trim(?1))"
        " ORDER BY f.modification_time DESC,f.storageid ASC";
    if (explorer
        && sqlite3_prepare_v2(explorer, sql, -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_text(statement, 1, title, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(statement, 0);
            /* Our own cache is keyed by the hash alone. The same book is
             * indexed once per storage, so the firmware's "<storageid><hash>"
             * would give one book two keys and flip between them whenever the
             * row order changes -- orphaning the cached image each time. */
            const char *key = (const char *)sqlite3_column_text(statement, 1);
            const char *fw_key = (const char *)sqlite3_column_text(statement, 2);
            if (source && !*source)
                snprintf(source, BS_PATH_MAX, "%s", path ? path : "");
            if (safe_cache_key(key) && safe_cache_key(fw_key)) {
                snprintf(remembered, sizeof(remembered), "%s", fw_key);
                if (!*fallback && !cached_cover(key, fallback)) {
                    snprintf(fallback, sizeof(fallback), COVER_DIR "/%s.png",
                             fw_key);
                    if (!file_exists(fallback))
                        fallback[0] = '\0';
                }
                const char *ext = strrchr(path, '.');
                int is_fb2 = ext && !strcasecmp(ext, ".fb2");
                int is_fb2z = !is_fb2 && path[0]
                    && (strstr(path, ".fb2.zip") || strstr(path, ".fb2.gz"));
                if (is_fb2 || is_fb2z) {
                    if (extract_fb2_cover(path, key, out))
                        break;
                } else if (ext && !strcasecmp(ext, ".cbz")) {
                    if (extract_cbz_cover(path, key, out))
                        break;
                } else {
                    if (extract_epub_cover(path, key, out))
                        break;
                }
            }
        }
    }
    sqlite3_finalize(statement);

    if (!*out && *fallback)
        snprintf(out, BS_PATH_MAX, "%s", fallback);
    if (!*out && !*remembered)
        remembered_cover_key(stats, title, remembered);
    if (!*out && *remembered) {
        /* books.cover holds the firmware's name; the hash inside it is our own
         * cache key. Try the whole string too, for keys written before the
         * split was understood. */
        const char *hash = remembered[0] && remembered[1] ? remembered + 1 : remembered;
        if (!cached_cover(hash, out) && !cached_cover(remembered, out)) {
            snprintf(out, BS_PATH_MAX, COVER_DIR "/%s.png", remembered);
            if (!file_exists(out))
                out[0] = '\0';
        }
    }
}

static void finished_free(finished_list *list)
{
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int finished_add(finished_list *list, bs_date day, const char *title,
                        int has_file)
{
    char words[BS_TITLE_MAX];
    normalize_title(title, words);
    size_t i;
    for (i = 0; i < list->count; ++i)
        if (same_book(list->items[i].words, words))
            break;
    if (i == list->count) {
        finished_book *grown = realloc(list->items,
            (list->count + 1) * sizeof(*list->items));
        if (!grown)
            return -1;
        list->items = grown;
        memset(&list->items[list->count], 0, sizeof(*list->items));
        list->items[list->count].day = day;
        snprintf(list->items[list->count].title, BS_TITLE_MAX, "%s", title);
        snprintf(list->items[list->count].words, BS_TITLE_MAX, "%s", words);
        list->items[list->count].has_file = has_file;
        ++list->count;
        return 0;
    }

    finished_book *book = &list->items[i];
    int new_day = day.year * 10000 + day.month * 100 + day.day;
    int old_day = book->day.year * 10000 + book->day.month * 100 + book->day.day;
    if (new_day < old_day)
        book->day = day;
    if (has_file && !book->has_file) {
        snprintf(book->title, BS_TITLE_MAX, "%s", title);
        snprintf(book->words, BS_TITLE_MAX, "%s", words);
        book->has_file = 1;
    }
    return 0;
}

static int finished_collect(sqlite3_stmt *statement, finished_list *out)
{
    while (sqlite3_step(statement) == SQLITE_ROW) {
        bs_date day;
        const char *date = (const char *)sqlite3_column_text(statement, 0);
        const char *title = (const char *)sqlite3_column_text(statement, 1);
        if (parse_date(date, &day)
            && finished_add(out, day, title, sqlite3_column_int(statement, 2)) != 0)
            return -1;
    }
    return 0;
}

static int finished_compare(const void *a, const void *b)
{
    const finished_book *left = a;
    const finished_book *right = b;
    int l = left->day.year * 10000 + left->day.month * 100 + left->day.day;
    int r = right->day.year * 10000 + right->day.month * 100 + right->day.day;
    return (l > r) - (l < r);
}

static int finished_books(bs_context *context, int with_covers,
                          finished_list *out, bs_error *error)
{
    memset(out, 0, sizeof(*out));
    sqlite3 *db = open_explorer(context);
    if (!db)
        return fail(error, 3, "Firmware library database is unavailable");
    const char *sql =
        "SELECT date(s.completed_ts,'unixepoch','localtime'),IFNULL(b.title,'?'),"
        " EXISTS(SELECT 1 FROM files f WHERE f.book_id=b.id)"
        " FROM books_settings s JOIN books_impl b ON b.id=s.bookid"
        " WHERE s.completed=1 AND s.completed_ts>0 ORDER BY s.completed_ts";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) {
        sql_fail(error, db, "Unsupported firmware library schema");
        sqlite3_close(db);
        return -1;
    }

    if (finished_collect(statement, out) != 0) {
        sqlite3_finalize(statement);
        sqlite3_close(db);
        finished_free(out);
        return fail(error, 4, "Out of memory");
    }
    sqlite3_finalize(statement);

    const char *saved_sql =
        "SELECT date(completed_ts,'unixepoch','localtime'),IFNULL(title,'?'),0"
        " FROM books WHERE completed=1 AND completed_ts>0 ORDER BY completed_ts";
    if (sqlite3_prepare_v2(context->stats, saved_sql, -1, &statement, NULL)
        != SQLITE_OK) {
        sqlite3_close(db);
        finished_free(out);
        return sql_fail(error, context->stats,
                        "Could not load saved finished books");
    }
    if (finished_collect(statement, out) != 0) {
        sqlite3_finalize(statement);
        sqlite3_close(db);
        finished_free(out);
        return fail(error, 4, "Out of memory");
    }
    sqlite3_finalize(statement);
    if (out->count > 1)
        qsort(out->items, out->count, sizeof(*out->items), finished_compare);

    if (with_covers) {
        size_t i;
        for (i = 0; i < out->count; ++i)
            resolve_cover(db, context->stats, out->items[i].title,
                          out->items[i].cover_path, NULL);
    }
    sqlite3_close(db);
    return 0;
}

int bs_context_open(bs_context **out, const char *stats_path,
                    const char *explorer_path, bs_error *error)
{
    clear_error(error);
    if (!out || !stats_path || !explorer_path)
        return fail(error, 1, "Invalid statistics context");
    *out = calloc(1, sizeof(**out));
    if (!*out)
        return fail(error, 4, "Out of memory");
    snprintf((*out)->explorer_path, sizeof((*out)->explorer_path), "%s",
             explorer_path);
    tracker setup;
    if (tracker_init(&setup, stats_path, explorer_path) != 0) {
        bs_context_close(*out);
        *out = NULL;
        return fail(error, 2, "Statistics database is unavailable");
    }
    (*out)->stats = setup.stats;
    invalidate_cbz_covers(*out);
    return 0;
}

void bs_context_close(bs_context *context)
{
    if (!context)
        return;
    if (context->stats)
        sqlite3_close(context->stats);
    free(context);
}

int bs_load_overall(bs_context *context, bs_overall *out, bs_error *error)
{
    clear_error(error);
    if (!context || !out)
        return fail(error, 1, "Invalid overview request");
    memset(out, 0, sizeof(*out));
    overall_stats overall;
    stats_overall(context->stats, &overall);
    out->today_secs = overall.today_secs;
    out->today_pages = overall.today_pages;
    out->week_secs = overall.week_secs;
    out->avg_session_min = overall.avg_session_min;
    out->pages_per_min = overall.pages_per_min;
    out->total_hours = overall.total_hours;
    out->streak_days = overall.streak_days;

    finished_list finished;
    if (finished_books(context, 0, &finished, error) != 0)
        return -1;
    out->books_finished = (int)finished.count;
    finished_free(&finished);

    sqlite3 *db = open_explorer(context);
    if (!db)
        return fail(error, 3, "Firmware library database is unavailable");
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT COUNT(DISTINCT lower(trim(IFNULL(b.title,'?'))))"
        " FROM books_impl b JOIN files f ON f.book_id=b.id";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK
        && sqlite3_step(statement) == SQLITE_ROW)
        out->books_total = sqlite3_column_int(statement, 0);
    else {
        sql_fail(error, db, "Unsupported firmware library schema");
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    if (out->books_total < out->books_finished)
        out->books_total = out->books_finished;
    out->finished_fraction = out->books_total
        ? (double)out->books_finished / out->books_total : 0.0;
    return 0;
}

static void enrich_book(bs_current_book *out, sqlite3 *explorer, sqlite3 *stats,
                        int64_t bookid, const char *title, const char *author,
                        int cpage, int npage, int completed,
                        const char *cover_key)
{
    out->ok = 1;
    snprintf(out->title, sizeof(out->title), "%s", title);
    snprintf(out->author, sizeof(out->author), "%s", author);
    double progress = completed ? 1.0
        : (npage > 0 ? (double)cpage / npage : 0.0);
    out->percent = (int)(progress * 100 + 0.5);
    out->completed = completed != 0;
    if (explorer)
        resolve_cover(explorer, stats, title, out->cover_path, NULL);
    if (!*out->cover_path && safe_cache_key(cover_key)) {
        snprintf(out->cover_path, sizeof(out->cover_path), COVER_DIR "/%s.png",
                 cover_key);
        if (!file_exists(out->cover_path))
            out->cover_path[0] = '\0';
    }
    double pages_per_minute = 0;
    stats_book(stats, bookid, &out->book_seconds, &pages_per_minute);
    if (pages_per_minute <= 0) {
        overall_stats overall;
        stats_overall(stats, &overall);
        pages_per_minute = overall.pages_per_min;
    }
    if (pages_per_minute > 0 && npage > cpage && !completed)
        out->left_seconds =
            (int64_t)((npage - cpage) / pages_per_minute * 60.0);
}

int bs_load_current_book(bs_context *context, bs_current_book *out,
                         bs_error *error)
{
    clear_error(error);
    if (!context || !out)
        return fail(error, 1, "Invalid current-book request");
    memset(out, 0, sizeof(*out));
    pb_state state;
    if (tracker_read_state(context->explorer_path, &state) != 0)
        return 0;
    sqlite3 *explorer = open_explorer(context);
    enrich_book(out, explorer, context->stats, state.bookid, state.title,
                state.author, state.cpage, state.npage, state.completed,
                state.cover);
    if (explorer)
        sqlite3_close(explorer);
    return 0;
}

int bs_load_reading_books(bs_context *context, bs_reading_list *out,
                          bs_error *error)
{
    clear_error(error);
    if (!context || !out)
        return fail(error, 1, "Invalid reading-books request");
    memset(out, 0, sizeof(*out));
    sqlite3 *explorer = open_explorer(context);
    if (!explorer)
        return fail(error, 3, "Firmware library database is unavailable");
    const char *sql =
        "SELECT s.bookid, IFNULL(s.cpage,0), IFNULL(s.npage,0),"
        "  IFNULL(b.title,''), IFNULL(b.author,''),"
        "  IFNULL((SELECT f.storageid || lower(hex(f.fast_hash)) FROM files f"
        "          WHERE f.book_id = s.bookid ORDER BY f.storageid LIMIT 1), '')"
        " FROM books_settings s JOIN books_impl b ON b.id = s.bookid"
        " WHERE s.opentime > 0 AND s.completed = 0"
        "  AND EXISTS(SELECT 1 FROM files f WHERE f.book_id = s.bookid)"
        " ORDER BY s.opentime DESC";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(explorer, sql, -1, &st, NULL) != SQLITE_OK) {
        sql_fail(error, explorer, "Could not load reading books");
        sqlite3_close(explorer);
        return -1;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        bs_current_book *grown = realloc(out->books,
            (out->count + 1) * sizeof(*grown));
        if (!grown) {
            sqlite3_finalize(st);
            sqlite3_close(explorer);
            bs_reading_list_free(out);
            return fail(error, 4, "Out of memory");
        }
        out->books = grown;
        bs_current_book *book = &out->books[out->count];
        memset(book, 0, sizeof(*book));
        enrich_book(book, explorer, context->stats,
                    sqlite3_column_int64(st, 0),
                    (const char *)sqlite3_column_text(st, 3),
                    (const char *)sqlite3_column_text(st, 4),
                    sqlite3_column_int(st, 1),
                    sqlite3_column_int(st, 2),
                    0,
                    (const char *)sqlite3_column_text(st, 5));
        ++out->count;
    }
    sqlite3_finalize(st);
    sqlite3_close(explorer);
    return 0;
}

void bs_reading_list_free(bs_reading_list *list)
{
    if (!list)
        return;
    free(list->books);
    memset(list, 0, sizeof(*list));
}

int bs_load_year(bs_context *context, int year, bs_year *out, bs_error *error)
{
    clear_error(error);
    if (!context || !out || year < 1970 || year > 9999)
        return fail(error, 1, "Invalid year request");
    memset(out, 0, sizeof(*out));
    out->days = is_leap(year) ? 366 : 365;
    out->start_weekday = weekday((bs_date){year, 1, 1});

    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT date(end_time,'unixepoch','localtime') FROM sessions"
        " WHERE strftime('%Y',end_time,'unixepoch','localtime')=printf('%04d',?1)"
        " GROUP BY 1 HAVING SUM(active_seconds)>=60";
    if (sqlite3_prepare_v2(context->stats, sql, -1, &statement, NULL) != SQLITE_OK)
        return sql_fail(error, context->stats, "Could not load reading year");
    sqlite3_bind_int(statement, 1, year);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        bs_date date;
        if (parse_date((const char *)sqlite3_column_text(statement, 0), &date)
            && date.year == year)
            out->heat[day_of_year(date) - 1] = 1;
    }
    sqlite3_finalize(statement);

    finished_list finished;
    if (finished_books(context, 0, &finished, error) != 0)
        return -1;
    size_t i;
    for (i = 0; i < finished.count; ++i)
        if (finished.items[i].day.year == year)
            out->heat[day_of_year(finished.items[i].day) - 1] = 2;
    finished_free(&finished);

    int run = 0;
    int best_start_day = 0;
    for (i = 0; i < (size_t)out->days; ++i) {
        if (out->heat[i]) {
            ++out->days_read;
            ++run;
            if (run > out->best_streak) {
                out->best_streak = run;
                best_start_day = (int)i - run + 2;
            }
        } else {
            run = 0;
        }
    }
    if (best_start_day)
        format_date(out->best_streak_start, date_from_day(year, best_start_day));
    if (current_year() == year) {
        overall_stats overall;
        stats_overall(context->stats, &overall);
        out->current_streak = overall.streak_days;
    }
    return 0;
}

void bs_month_free(bs_month *month)
{
    if (!month)
        return;
    int i;
    for (i = 0; i < 31; ++i)
        free(month->day[i].books);
    memset(month, 0, sizeof(*month));
}

int bs_load_month(bs_context *context, int year, int month, bs_month *out,
                  bs_error *error)
{
    clear_error(error);
    if (!context || !out || year < 1970 || year > 9999
        || month < 1 || month > 12)
        return fail(error, 1, "Invalid month request");
    memset(out, 0, sizeof(*out));
    out->days = days_in_month(year, month);
    out->first_weekday = weekday((bs_date){year, month, 1});
    const char *sql =
        "SELECT CAST(strftime('%d',s.end_time,'unixepoch','localtime') AS INTEGER),"
        " MIN(IFNULL(b.title,'?')),MAX(IFNULL(b.cover,'')),SUM(s.active_seconds)"
        " FROM sessions s LEFT JOIN books b ON b.book_id=s.book_id"
        " WHERE strftime('%Y-%m',s.end_time,'unixepoch','localtime')"
        " =printf('%04d-%02d',?1,?2)"
        " GROUP BY 1,lower(trim(IFNULL(b.title,'?')))"
        " HAVING SUM(s.active_seconds)>=60 ORDER BY 1,4 DESC";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(context->stats, sql, -1, &statement, NULL) != SQLITE_OK)
        return sql_fail(error, context->stats, "Could not load reading month");
    sqlite3_bind_int(statement, 1, year);
    sqlite3_bind_int(statement, 2, month);
    sqlite3 *explorer = open_explorer(context);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        int day = sqlite3_column_int(statement, 0);
        if (day < 1 || day > out->days)
            continue;
        bs_month_day *target = &out->day[day - 1];
        bs_book *grown = realloc(target->books,
                                 (target->book_count + 1) * sizeof(*grown));
        if (!grown) {
            sqlite3_finalize(statement);
            if (explorer)
                sqlite3_close(explorer);
            bs_month_free(out);
            return fail(error, 4, "Out of memory");
        }
        target->books = grown;
        bs_book *book = &target->books[target->book_count++];
        memset(book, 0, sizeof(*book));
        snprintf(book->title, sizeof(book->title), "%s",
                 sqlite3_column_text(statement, 1));
        book->seconds = sqlite3_column_int64(statement, 3);
        resolve_cover(explorer, context->stats, book->title, book->cover_path,
                      book->source_path);
        if (!*book->cover_path) {
            const char *key = (const char *)sqlite3_column_text(statement, 2);
            if (safe_cache_key(key)) {
                snprintf(book->cover_path, sizeof(book->cover_path),
                         COVER_DIR "/%s.png", key);
                if (!file_exists(book->cover_path))
                    book->cover_path[0] = '\0';
            }
        }
        target->seconds += book->seconds;
    }
    sqlite3_finalize(statement);
    if (explorer)
        sqlite3_close(explorer);
    return 0;
}

void bs_year_books_free(bs_year_books *year)
{
    if (!year)
        return;
    int i;
    for (i = 0; i < 12; ++i)
        free(year->month[i]);
    memset(year, 0, sizeof(*year));
}

int bs_load_year_books(bs_context *context, int year, bs_year_books *out,
                       bs_error *error)
{
    clear_error(error);
    if (!context || !out || year < 1970 || year > 9999)
        return fail(error, 1, "Invalid finished-books request");
    memset(out, 0, sizeof(*out));
    finished_list finished;
    if (finished_books(context, 1, &finished, error) != 0)
        return -1;
    size_t i;
    for (i = 0; i < finished.count; ++i) {
        finished_book *source = &finished.items[i];
        if (source->day.year != year)
            continue;
        int month = source->day.month - 1;
        bs_book *grown = realloc(out->month[month],
            (out->month_count[month] + 1) * sizeof(*grown));
        if (!grown) {
            finished_free(&finished);
            bs_year_books_free(out);
            return fail(error, 4, "Out of memory");
        }
        out->month[month] = grown;
        bs_book *book = &out->month[month][out->month_count[month]++];
        memset(book, 0, sizeof(*book));
        snprintf(book->title, sizeof(book->title), "%s", source->title);
        snprintf(book->cover_path, sizeof(book->cover_path), "%s",
                 source->cover_path);
        format_date(book->date, source->day);
        ++out->total;
    }
    finished_free(&finished);
    return 0;
}
