#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "updater.h"

#include "daemon.h"
#include "miniz.h"
#include "sha256.h"
#include "sqlite3.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BETTERSTATS_INSTALL_ROOT
#define BETTERSTATS_INSTALL_ROOT "/mnt/ext1"
#endif

#define API_URL \
    "https://api.github.com/repos/nikljuel/better-stats/releases/latest"

/* Filled immediately before the release; empty text keeps the dialog hidden. */
static const char *release_notes_version = "v1.3.2";
static const char *release_notes_de =
    "Neu in 1.3.2:\n"
    "- Ein Fehler wurde behoben, durch den das automatische Tracking nach "
    "einem Update-Neustart ausfallen konnte.";
static const char *release_notes_en =
    "What's new in 1.3.2:\n"
    "- Fixed an issue that could prevent automatic tracking from resuming "
    "after an update restart.";

static const char *release_files[] = {
    "betterstats-qt-softfp",
    "betterstats-inkview-softfp",
    "betterstats-inkview-hardfp",
    "manifest",
    "SHA256SUMS"
};

static const char *install_root(void)
{
    const char *root = getenv("BETTERSTATS_INSTALL_ROOT");
    return root && *root ? root : BETTERSTATS_INSTALL_ROOT;
}

static const char *update_root(void)
{
    const char *root = getenv("BETTERSTATS_UPDATE_DIR");
    return root && *root ? root : STATS_DIR "/update";
}

static void path_join(char *out, size_t size, const char *a, const char *b)
{
    const size_t a_size = strlen(a);
    const size_t b_size = strlen(b);
    if (!size || a_size >= size || b_size >= size - a_size - 1) {
        if (size)
            out[0] = '\0';
        return;
    }
    memcpy(out, a, a_size);
    out[a_size] = '/';
    memcpy(out + a_size + 1, b, b_size + 1);
}

static int make_dir(const char *path)
{
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static void update_log(const char *format, ...)
{
    char path[1024];
    path_join(path, sizeof(path), STATS_DIR, "app.log");
    FILE *file = fopen(path, "a");
    if (!file)
        return;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    fprintf(file, "%s Better Stats updater: ", stamp);
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

static int fail(bs_update_info *info, int error, const char *format, ...)
{
    info->error = error;
    va_list args;
    va_start(args, format);
    vsnprintf(info->detail, sizeof(info->detail), format, args);
    va_end(args);
    update_log("error %d: %s", error, info->detail);
    return error;
}

static int safe_name(const char *value)
{
    if (!value || !isalnum((unsigned char)*value))
        return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return 0;
    return 1;
}

static int parse_version(const char *value, int out[3])
{
    const unsigned char *p = (const unsigned char *)value;
    if (!p)
        return 0;
    if (*p == 'v' || *p == 'V')
        ++p;
    for (int i = 0; i < 3; ++i) {
        if (!isdigit(*p))
            return 0;
        int number = 0;
        while (isdigit(*p)) {
            int digit = *p++ - '0';
            if (number > (INT_MAX - digit) / 10)
                return 0;
            number = number * 10 + digit;
        }
        out[i] = number;
        if (i < 2 && *p++ != '.')
            return 0;
    }
    return *p == '\0';
}

int bs_update_version_compare(const char *a, const char *b)
{
    int va[3], vb[3];
    const int a_ok = parse_version(a, va);
    const int b_ok = parse_version(b, vb);
    if (!a_ok || !b_ok) {
        if (a_ok != b_ok)
            return a_ok ? 1 : -1;
        return strcmp(a ? a : "", b ? b : "");
    }
    for (int i = 0; i < 3; ++i)
        if (va[i] != vb[i])
            return va[i] < vb[i] ? -1 : 1;
    return 0;
}

static int disabled_path(char out[1024])
{
    return snprintf(out, 1024, "%s/updates-disabled", STATS_DIR) < 1024;
}

int bs_update_auto_enabled(void)
{
    char path[1024];
    return disabled_path(path) && access(path, F_OK) != 0;
}

int bs_update_set_auto_enabled(int enabled)
{
    char path[1024];
    if (!disabled_path(path) || !make_dir(STATS_DIR))
        return -1;
    if (enabled)
        return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
    FILE *file = fopen(path, "w");
    if (!file)
        return -1;
    return fclose(file) == 0 ? 0 : -1;
}

static void *symbol(const char *name)
{
    static void *process;
    if (!process)
        process = dlopen(NULL, RTLD_LAZY);
    return process ? dlsym(process, name) : NULL;
}

int bs_update_network_connected(void)
{
    int (*get_state)(void) = (int (*)(void))symbol("GetNetState");
    return get_state && get_state() == 2; /* InkView CONNECTED */
}

static int start_network(bs_update_info *info, int prompt)
{
    int (*silent)(const char *) =
        (int (*)(const char *))symbol("NetConnectSilent");
    if (silent && silent(NULL) == 0)
        return 1;
    if (!prompt) {
        fail(info, BS_UPDATE_ERR_NETWORK, "Wi-Fi is not connected");
        return 0;
    }
    int (*connect2)(const char *, int) =
        (int (*)(const char *, int))symbol("NetConnect2");
    if (connect2 && connect2(NULL, 0) == 0)
        return 1;
    int (*connect1)(const char *) =
        (int (*)(const char *))symbol("NetConnect");
    if (connect1 && connect1(NULL) == 0)
        return 1;
    fail(info, BS_UPDATE_ERR_NETWORK, "Wi-Fi is not connected");
    return 0;
}

static int connect_network(bs_update_info *info, int prompt)
{
    return bs_update_network_connected() || start_network(info, prompt);
}

static int reconnect_network(bs_update_info *info)
{
    int (*disconnect)(void) = (int (*)(void))symbol("NetDisconnect");
    if (disconnect)
        disconnect();
    return start_network(info, 0);
}

static int write_download(const void *data, int size, const char *path)
{
    if (!data || size <= 0)
        return -1;
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    int ok = fwrite(data, 1, (size_t)size, file) == (size_t)size
        && fflush(file) == 0;
    ok = fclose(file) == 0 && ok;
    if (!ok)
        unlink(path);
    return ok ? 0 : -1;
}

static int download_to(bs_update_info *info, const char *url, const char *path,
                       int timeout, int retry)
{
    int (*postpone)(void) = (int (*)(void))symbol("PostponeTimedPoweroff");
    if (postpone)
        postpone();

    typedef void *(*quick3_fn)(const char *, int *, int, char *, char *, int *);
    typedef void *(*quick_fn)(const char *, int *, int);
    quick3_fn quick3 = (quick3_fn)symbol("QuickDownloadExt3");
    quick_fn quick = (quick_fn)symbol("QuickDownload");
    if (!quick3 && !quick)
        return fail(info, BS_UPDATE_ERR_UNSUPPORTED,
                    "Firmware has no synchronous download function");

    for (int attempt = 0; attempt <= retry; ++attempt) {
        int size = 0;
        int net_error = 0;
        void *data = quick3
            ? quick3(url, &size, timeout, NULL, NULL, &net_error)
            : quick(url, &size, timeout);
        const int written = write_download(data, size, path);
        free(data);
        if (written == 0) {
            update_log("downloaded %d bytes from %s", size, url);
            return 0;
        }
        if (attempt < retry) {
            if (!reconnect_network(info))
                return info->error;
        } else
            return fail(info, BS_UPDATE_ERR_DOWNLOAD,
                        "Download failed (%d)", net_error);
    }
    return BS_UPDATE_ERR_DOWNLOAD;
}

static int copy_column(char *out, size_t size, sqlite3_stmt *statement, int column)
{
    const unsigned char *value = sqlite3_column_text(statement, column);
    if (!value || strlen((const char *)value) >= size)
        return 0;
    strcpy(out, (const char *)value);
    return 1;
}

static int copy_digest(char out[65], sqlite3_stmt *statement, int column)
{
    const char *value = (const char *)sqlite3_column_text(statement, column);
    if (!value) {
        out[0] = '\0';
        return sqlite3_column_type(statement, column) == SQLITE_NULL;
    }
    if (strncmp(value, "sha256:", 7) != 0 || strlen(value + 7) != 64)
        return 0;
    for (int i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)value[i + 7]))
            return 0;
        out[i] = (char)tolower((unsigned char)value[i + 7]);
    }
    out[64] = '\0';
    return 1;
}


int bs_update_parse_release(const char *json, bs_update_info *info)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    if (!json || sqlite3_open(":memory:", &db) != SQLITE_OK) {
        sqlite3_close(db);
        return fail(info, BS_UPDATE_ERR_RESPONSE, "Could not parse release response");
    }

    const char *release_sql =
        "SELECT json_extract(?1,'$.tag_name'),"
        " json_extract(?1,'$.draft'), json_extract(?1,'$.prerelease')";
    int ok = sqlite3_prepare_v2(db, release_sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, json, -1, SQLITE_STATIC);
        ok = sqlite3_step(statement) == SQLITE_ROW
            && copy_column(info->latest_version, sizeof(info->latest_version),
                           statement, 0)
            && sqlite3_column_int(statement, 1) == 0
            && sqlite3_column_int(statement, 2) == 0
            && safe_name(info->latest_version);
    }
    sqlite3_finalize(statement);
    statement = NULL;
    int parsed[3];
    ok = ok && parse_version(info->latest_version, parsed);

    char expected[128];
    snprintf(expected, sizeof(expected), "BetterStats-%s.zip",
             info->latest_version);
    const char *asset_sql =
        "SELECT json_extract(value,'$.browser_download_url'),"
        " CAST(json_extract(value,'$.size') AS INTEGER),"
        " json_extract(value,'$.digest')"
        " FROM json_each(?1,'$.assets')"
        " WHERE json_extract(value,'$.name')=?2 LIMIT 1";
    if (ok)
        ok = sqlite3_prepare_v2(db, asset_sql, -1, &statement, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(statement, 1, json, -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 2, expected, -1, SQLITE_STATIC);
        ok = sqlite3_step(statement) == SQLITE_ROW
            && copy_column(info->asset_url, sizeof(info->asset_url), statement, 0);
        info->asset_size = ok ? sqlite3_column_int64(statement, 1) : 0;
        ok = ok && info->asset_size > 0
            && copy_digest(info->digest, statement, 2);
    }
    char expected_url[512];
    snprintf(expected_url, sizeof(expected_url),
             "https://github.com/nikljuel/better-stats/releases/download/%s/%s",
             info->latest_version, expected);
    ok = ok && strcmp(info->asset_url, expected_url) == 0;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return ok ? 0 : fail(info, BS_UPDATE_ERR_ASSET,
                         "Latest release has no valid %s", expected);
}

int bs_update_read_current(bs_update_info *info)
{
    char path[1024];
    path_join(path, sizeof(path), install_root(),
              "applications/betterstats/current");
    FILE *file = fopen(path, "r");
    if (!file)
        return fail(info, BS_UPDATE_ERR_INSTALL, "Could not read current release");
    const int ok = fgets(info->current_version, sizeof(info->current_version), file)
        != NULL;
    fclose(file);
    info->current_version[strcspn(info->current_version, "\r\n")] = '\0';
    return ok && safe_name(info->current_version)
        ? 0 : fail(info, BS_UPDATE_ERR_INSTALL, "Current release name is invalid");
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    rewind(file);
    char *data = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    data[size] = '\0';
    fclose(file);
    return data;
}

static int release_notes_seen_path(char out[1024])
{
    return snprintf(out, 1024, "%s/release-notes-seen", STATS_DIR) < 1024;
}

static int current_has_release_notes(const char *current)
{
    const size_t size = strlen(release_notes_version);
    return !strcmp(current, release_notes_version)
        || (!strncmp(current, release_notes_version, size)
            && current[size] == '-');
}

int bs_update_release_notes_pending(void)
{
    bs_update_info info = {0};
    if (bs_update_read_current(&info) != 0
        || !current_has_release_notes(info.current_version))
        return 0;

    char path[1024];
    if (!release_notes_seen_path(path))
        return 0;
    char *seen = read_file(path);
    if (!seen)
        return 1;
    seen[strcspn(seen, "\r\n")] = '\0';
    const int pending = !safe_name(seen)
        || bs_update_version_compare(seen, info.current_version) < 0;
    free(seen);
    return pending;
}

const char *bs_update_release_notes(const char *language)
{
    if (!bs_update_release_notes_pending())
        return NULL;
    return language && !strncmp(language, "de", 2)
        ? release_notes_de : release_notes_en;
}

int bs_update_mark_release_notes_seen(void)
{
    bs_update_info info = {0};
    if (bs_update_read_current(&info) != 0
        || !current_has_release_notes(info.current_version)
        || !make_dir(STATS_DIR))
        return -1;
    char path[1024];
    if (!release_notes_seen_path(path))
        return -1;
    FILE *file = fopen(path, "w");
    int ok = 0;
    if (file) {
        ok = fprintf(file, "%s\n", info.current_version) > 0;
        if (fclose(file) != 0)
            ok = 0;
    }
    return ok ? 0 : -1;
}

int bs_update_check(bs_update_info *info, int connect_mode)
{
    memset(info, 0, sizeof(*info));
    if (bs_update_read_current(info) != 0)
        return info->error;
    if (connect_mode != BS_UPDATE_CONNECT_NONE
        && !connect_network(info, connect_mode == BS_UPDATE_CONNECT_PROMPT))
        return info->error;

    if (!make_dir(STATS_DIR) || !make_dir(update_root()))
        return fail(info, BS_UPDATE_ERR_INSTALL, "Could not create update directory");
    char json_path[1024];
    path_join(json_path, sizeof(json_path), update_root(), "latest.json");
    unlink(json_path);
    if (download_to(info, API_URL, json_path, 20, 1) != 0)
        return info->error;
    char *json = read_file(json_path);
    unlink(json_path);
    if (!json)
        return fail(info, BS_UPDATE_ERR_RESPONSE, "Could not read release response");
    const int parsed = bs_update_parse_release(json, info);
    free(json);
    if (parsed != 0)
        return info->error;
    info->error = 0;
    info->detail[0] = '\0';
    update_log("current %s, latest %s", info->current_version,
               info->latest_version);
    return bs_update_version_compare(info->current_version,
                                     info->latest_version) < 0
        ? BS_UPDATE_AVAILABLE : BS_UPDATE_CURRENT;
}

static int sha256_file(const char *path, char out[65])
{
    return bs_sha256_file(path, out);
}

static int run_checksum(const char *directory)
{
    return bs_sha256_check(directory);
}

static void remove_release(const char *directory)
{
    char path[1024];
    for (size_t i = 0; i < sizeof(release_files) / sizeof(release_files[0]); ++i) {
        path_join(path, sizeof(path), directory, release_files[i]);
        unlink(path);
    }
    rmdir(directory);
}

static int looks_like_elf(const char *path)
{
    unsigned char magic[4];
    FILE *file = fopen(path, "rb");
    const int ok = file && fread(magic, 1, sizeof(magic), file) == sizeof(magic)
        && memcmp(magic, "\177" "ELF", sizeof(magic)) == 0;
    if (file)
        fclose(file);
    return ok;
}

static int extract_entry(mz_zip_archive *archive, const char *name,
                         const char *destination)
{
    int index = mz_zip_reader_locate_file(archive, name, NULL, 0);
    return index >= 0 && mz_zip_reader_extract_to_file(
        archive, (mz_uint)index, destination, 0);
}

static int manifest_matches(const char *directory, const char *version)
{
    char path[1024];
    path_join(path, sizeof(path), directory, "manifest");
    char *manifest = read_file(path);
    char expected[128];
    snprintf(expected, sizeof(expected), "version=%s\n", version);
    const int ok = manifest && strcmp(manifest, expected) == 0;
    free(manifest);
    return ok;
}

static int run_activator(const char *base, const char *version,
                         const char *activator)
{
    pid_t pid = fork();
    if (pid == 0) {
        setenv("BETTERSTATS_BASE", base, 1);
        execl("/bin/sh", "sh", activator, version, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    return pid > 0 && waitpid(pid, &status, 0) == pid
        && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int bs_update_install(bs_update_info *info)
{
    if (!safe_name(info->latest_version) || !*info->asset_url
        || info->asset_size <= 0)
        return fail(info, BS_UPDATE_ERR_ASSET, "No checked release to install");
    if (!connect_network(info, 0))
        return info->error;
    if (!make_dir(STATS_DIR) || !make_dir(update_root()))
        return fail(info, BS_UPDATE_ERR_INSTALL, "Could not create update directory");

    char zip_path[1024];
    path_join(zip_path, sizeof(zip_path), update_root(), "BetterStats.zip");
    unlink(zip_path);
    if (download_to(info, info->asset_url, zip_path, 60, 1) != 0)
        return info->error;
    struct stat zip_stat;
    if (stat(zip_path, &zip_stat) != 0) {
        unlink(zip_path);
        return fail(info, BS_UPDATE_ERR_CORRUPT,
                    "Could not stat downloaded ZIP");
    }
    if (zip_stat.st_size != info->asset_size) {
        update_log("size mismatch: got %lld, expected %lld",
                   (long long)zip_stat.st_size, info->asset_size);
        unlink(zip_path);
        return fail(info, BS_UPDATE_ERR_CORRUPT,
                    "ZIP size mismatch: %lld vs %lld",
                    (long long)zip_stat.st_size, info->asset_size);
    }
    if (*info->digest) {
        char actual[65];
        if (!sha256_file(zip_path, actual)) {
            unlink(zip_path);
            return fail(info, BS_UPDATE_ERR_CORRUPT,
                        "SHA-256 of downloaded ZIP could not be computed");
        }
        if (strcmp(actual, info->digest) != 0) {
            update_log("SHA-256 mismatch: got %s, expected %s",
                       actual, info->digest);
            unlink(zip_path);
            return fail(info, BS_UPDATE_ERR_CORRUPT,
                        "Downloaded ZIP does not match its GitHub SHA-256");
        }
    } else {
        update_log("GitHub SHA-256 unavailable; using bundled SHA256SUMS");
    }

    char base[1024], releases[1024], final[1024], stage[1024];
    path_join(base, sizeof(base), install_root(), "applications/betterstats");
    path_join(releases, sizeof(releases), base, "releases");
    path_join(final, sizeof(final), releases, info->latest_version);
    char stage_name[128];
    snprintf(stage_name, sizeof(stage_name), ".%s.tmp",
             info->latest_version);
    path_join(stage, sizeof(stage), releases, stage_name);
    if (!make_dir(base) || !make_dir(releases)) {
        unlink(zip_path);
        return fail(info, BS_UPDATE_ERR_INSTALL, "Could not create release directory");
    }
    remove_release(stage);
    if (!make_dir(stage)) {
        unlink(zip_path);
        return fail(info, BS_UPDATE_ERR_INSTALL, "Could not stage release");
    }

    mz_zip_archive archive;
    memset(&archive, 0, sizeof(archive));
    int ok = mz_zip_reader_init_file(&archive, zip_path, 0);
    char archive_path[1024], destination[1024];
    for (size_t i = 0; ok && i < sizeof(release_files) / sizeof(release_files[0]); ++i) {
        snprintf(archive_path, sizeof(archive_path),
                 "applications/betterstats/releases/%s/%s",
                 info->latest_version, release_files[i]);
        path_join(destination, sizeof(destination), stage, release_files[i]);
        ok = extract_entry(&archive, archive_path, destination);
    }
    char launcher_new[1024], activator_new[1024];
    path_join(launcher_new, sizeof(launcher_new), update_root(), "BetterStats.app.new");
    path_join(activator_new, sizeof(activator_new), update_root(), "activate-release.new");
    unlink(launcher_new);
    unlink(activator_new);
    if (ok)
        ok = extract_entry(&archive, "applications/BetterStats.app", launcher_new)
            && extract_entry(&archive,
                "applications/betterstats/activate-release", activator_new);
    if (archive.m_pState)
        mz_zip_reader_end(&archive);
    unlink(zip_path);

    const char *bundle_error = ok ? NULL : "Release archive is incomplete";
    for (int i = 0; !bundle_error && i < 3; ++i) {
        path_join(destination, sizeof(destination), stage, release_files[i]);
        if (!looks_like_elf(destination))
            bundle_error = "Release executable is missing or invalid";
    }
    if (!bundle_error && !manifest_matches(stage, info->latest_version))
        bundle_error = "Release manifest does not match its version";
    if (!bundle_error && !run_checksum(stage))
        bundle_error = "Release SHA256SUMS validation failed";
    if (bundle_error) {
        remove_release(stage);
        unlink(launcher_new);
        unlink(activator_new);
        return fail(info, BS_UPDATE_ERR_CORRUPT, "%s", bundle_error);
    }

    if (access(final, F_OK) == 0) {
        if (!manifest_matches(final, info->latest_version) || !run_checksum(final)) {
            remove_release(final);
            if (rename(stage, final) != 0)
                ok = 0;
        } else {
            remove_release(stage);
        }
    } else if (rename(stage, final) != 0) {
        ok = 0;
    }

    char launcher[1024], activator[1024];
    path_join(launcher, sizeof(launcher), install_root(),
              "applications/BetterStats.app");
    path_join(activator, sizeof(activator), base, "activate-release");
    ok = ok && rename(launcher_new, launcher) == 0
        && rename(activator_new, activator) == 0;
    if (ok) {
        sync();
        ok = run_activator(base, info->latest_version, activator);
    }
    unlink(launcher_new);
    unlink(activator_new);
    if (!ok)
        return fail(info, BS_UPDATE_ERR_INSTALL,
                    "Could not activate the staged release");
    sync();
    update_log("activated %s", info->latest_version);
    info->error = 0;
    info->detail[0] = '\0';
    return 0;
}

int bs_update_restart(void)
{
    if (stop_daemon() != 0)
        return -1;

    pid_t child = fork();
    if (child != 0)
        return child > 0 ? 0 : -1;
    setsid();
    sleep(2);
    char launcher[1024];
    path_join(launcher, sizeof(launcher), install_root(),
              "applications/BetterStats.app");
    execl("/bin/sh", "sh", launcher, (char *)NULL);
    _exit(127);
}
