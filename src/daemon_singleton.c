#define _GNU_SOURCE
#include "daemon_singleton.h"
#include "daemon.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef PROC_ROOT
#define PROC_ROOT "/proc"
#endif
#ifndef BETTERSTATS_INSTALL_ROOT
#define BETTERSTATS_INSTALL_ROOT "/mnt/ext1"
#endif
#ifndef DAEMON_STABILIZE_MS
#define DAEMON_STABILIZE_MS 100
#endif
#ifndef DAEMON_POLL_MS
#define DAEMON_POLL_MS 100
#endif
#ifndef DAEMON_CURRENT_STOP_MS
#define DAEMON_CURRENT_STOP_MS 5000
#endif
#ifndef DAEMON_LEGACY_STOP_MS
#define DAEMON_LEGACY_STOP_MS 35000
#endif
#ifndef DAEMON_RECONCILE_MS
#define DAEMON_RECONCILE_MS 35000
#endif
#ifndef DAEMON_SCAN_MAX
#define DAEMON_SCAN_MAX 64
#endif
#ifndef DAEMON_SINGLETON_TEST_BASENAME
#define DAEMON_SINGLETON_TEST_BASENAME "test_daemon_singleton"
#endif

enum ref_source {
    REF_NONE,
    REF_PRIMARY_LOCK,
    REF_PRIMARY_FILE,
    REF_LEGACY_FILE,
    REF_PROC_SCAN
};

enum ref_mode { REF_CURRENT, REF_LEGACY };
enum validation { VALID_GONE, VALID_OK, VALID_FOREIGN, VALID_UNKNOWN };
#define STOP_RECLAIM (-2)

typedef struct {
    enum ref_source source;
    enum ref_mode mode;
    pid_t pid;
    uint64_t start_ticks;
    dev_t exe_dev;
    ino_t exe_ino;
} daemon_ref;

typedef struct {
    daemon_ref refs[DAEMON_SCAN_MAX];
    size_t count;
    int uncertain;
} proc_snapshot;

static pid_t claimed_pid;
static uint64_t claimed_ticks;
static int claimed_fd = -1;
static int legacy_hint_written;

static const char *runtime_install_root(void)
{
    const char *root = getenv("BETTERSTATS_INSTALL_ROOT");
    return root && *root ? root : BETTERSTATS_INSTALL_ROOT;
}

#ifdef DAEMON_SINGLETON_TEST
static const char *test_path(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value && *value ? value : fallback;
}

static const char *primary_path(void)
{
    return test_path("BETTERSTATS_TEST_PIDFILE", PIDFILE);
}

static const char *legacy_path(void)
{
    return test_path("BETTERSTATS_TEST_LEGACY_PIDFILE", LEGACY_PIDFILE);
}

static const char *proc_path(void)
{
    return test_path("BETTERSTATS_TEST_PROC_ROOT", PROC_ROOT);
}

static const char *install_root(void)
{
    return test_path("BETTERSTATS_TEST_INSTALL_ROOT",
                     runtime_install_root());
}

extern void daemon_singleton_test_hook(const char *, int)
    __attribute__((weak));
static void test_hook(const char *name, pid_t pid)
{
    if (daemon_singleton_test_hook)
        daemon_singleton_test_hook(name, (int)pid);
}
#else
static const char *primary_path(void) { return PIDFILE; }
static const char *legacy_path(void) { return LEGACY_PIDFILE; }
static const char *proc_path(void) { return PROC_ROOT; }
static const char *install_root(void) { return runtime_install_root(); }
static void test_hook(const char *name, pid_t pid)
{
    (void)name;
    (void)pid;
}
#endif

static int checked_path(char *out, size_t cap, const char *fmt, int pid,
                        const char *leaf)
{
    int n;
    if (leaf)
        n = snprintf(out, cap, fmt, proc_path(), pid, leaf);
    else
        n = snprintf(out, cap, fmt, proc_path(), pid);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int sleep_millis(int milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

static int64_t monotonic_millis(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    if (ts.tv_sec > (INT64_MAX - ts.tv_nsec / 1000000) / 1000)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int parse_pid_text(const char *buf, size_t len, pid_t *pid)
{
    if (!buf || !len || len >= 64)
        return -1;
    char copy[64];
    memcpy(copy, buf, len);
    copy[len] = '\0';
    char *end = NULL;
    errno = 0;
    long value = strtol(copy, &end, 10);
    if (errno || end == copy || value <= 0 || value > INT_MAX)
        return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end)
        return -1;
    *pid = (pid_t)value;
    return 0;
}

static int read_small_fd(int fd, char *buf, size_t cap, size_t *len)
{
    ssize_t n;
    do {
        n = pread(fd, buf, cap, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return -1;
    *len = (size_t)n;
    return 0;
}

static int read_small_path(const char *path, char *buf, size_t cap,
                           size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            *len = 0;
            return 0;
        }
        return -1;
    }
    int rc = read_small_fd(fd, buf, cap, len);
    int saved = errno;
    if (close(fd) != 0 && rc == 0)
        rc = -1;
    errno = saved;
    return rc;
}

static int read_proc_stat(pid_t pid, char *state, uint64_t *ticks)
{
    char path[PATH_MAX], line[4096];
    if (checked_path(path, sizeof(path), "%s/%d/%s", (int)pid, "stat") != 0)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n;
    do {
        n = read(fd, line, sizeof(line) - 1);
    } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n <= 0 || n == (ssize_t)sizeof(line) - 1)
        return -1;
    line[n] = '\0';
    char *rest = strrchr(line, ')');
    if (!rest || rest[1] != ' ' || !rest[2])
        return -1;
    rest += 2;
    char *save = NULL;
    char *token = strtok_r(rest, " ", &save);
    if (!token || token[1] != '\0')
        return -1;
    *state = token[0];
    for (int index = 2; index <= 20; ++index) {
        token = strtok_r(NULL, " ", &save);
        if (!token)
            return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(token, &end, 10);
    if (errno || end == token || (*end && *end != '\n') || value == 0)
        return -1;
    *ticks = (uint64_t)value;
    return 0;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int safe_component(const char *value, size_t len)
{
    if (!len || (len == 1 && value[0] == '.')
        || (len == 2 && value[0] == '.' && value[1] == '.'))
        return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '.' || c == '_'
              || c == '-'))
            return 0;
    }
    return 1;
}

static int trusted_executable(const char *exe, enum ref_mode *mode)
{
    char historical[PATH_MAX], prefix[PATH_MAX];
    int n = snprintf(historical, sizeof(historical),
                     "%s/applications/BetterStats.app", install_root());
    if (n <= 0 || (size_t)n >= sizeof(historical))
        return 0;
    if (strcmp(exe, historical) == 0) {
        *mode = REF_LEGACY;
        return 1;
    }
    n = snprintf(prefix, sizeof(prefix),
                 "%s/applications/betterstats/releases/", install_root());
    if (n <= 0 || (size_t)n >= sizeof(prefix))
        return 0;
    size_t prefix_len = (size_t)n;
    if (strncmp(exe, prefix, prefix_len) != 0)
        return 0;
    const char *component = exe + prefix_len;
    const char *slash = strchr(component, '/');
    if (!slash || strchr(slash + 1, '/')
        || !safe_component(component, (size_t)(slash - component)))
        return 0;
    const char *base = slash + 1;
    if (strcmp(base, "betterstats-inkview-softfp") != 0
        && strcmp(base, "betterstats-inkview-hardfp") != 0
#ifdef DAEMON_SINGLETON_TEST
        && strcmp(base, DAEMON_SINGLETON_TEST_BASENAME) != 0
#endif
       )
        return 0;
    *mode = REF_LEGACY; /* A lockless release is an upgrade predecessor. */
    return 1;
}

/* 1 = daemon argv, 0 = valid non-daemon argv, -1 = unreadable/indeterminate. */
static int read_cmdline(pid_t pid, char *arg0, size_t cap)
{
    char path[PATH_MAX], buf[4096];
    if (checked_path(path, sizeof(path), "%s/%d/%s", (int)pid,
                     "cmdline") != 0)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf + used, sizeof(buf) - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
        if (used == sizeof(buf)) {
            close(fd);
            return -1;
        }
    }
    if (close(fd) != 0 || used < 2 || buf[used - 1] != '\0')
        return -1;
    size_t arg0_len = strnlen(buf, used);
    if (arg0_len == used || arg0_len >= cap)
        return -1;
    memcpy(arg0, buf, arg0_len + 1);
    if (arg0_len + 1 == used)
        return 0;
    const char *arg1 = buf + arg0_len + 1;
    size_t remaining = used - arg0_len - 1;
    size_t arg1_len = strnlen(arg1, remaining);
    if (arg1_len == remaining)
        return -1;
    return strcmp(arg1, "--daemon") == 0 ? 1 : 0;
}

static int read_executable(pid_t pid, char *exe, size_t cap,
                           dev_t *dev, ino_t *ino, enum ref_mode *mode)
{
    char path[PATH_MAX];
    if (checked_path(path, sizeof(path), "%s/%d/%s", (int)pid, "exe") != 0)
        return -1;
    ssize_t n = readlink(path, exe, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1)
        return -1;
    exe[n] = '\0';
    static const char deleted[] = " (deleted)";
    size_t len = (size_t)n, suffix = sizeof(deleted) - 1;
    if (len >= suffix && strcmp(exe + len - suffix, deleted) == 0)
        exe[len - suffix] = '\0';
    if (!trusted_executable(exe, mode))
        return 0;
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *dev = st.st_dev;
    *ino = st.st_ino;
    return 1;
}

static int process_has_trusted_exe(pid_t pid)
{
    char link_path[PATH_MAX], exe[PATH_MAX];
    if (checked_path(link_path, sizeof(link_path), "%s/%d/%s", (int)pid,
                     "exe") != 0)
        return 0;
    ssize_t n = readlink(link_path, exe, sizeof(exe) - 1);
    if (n <= 0 || n == (ssize_t)sizeof(exe) - 1)
        return 0;
    exe[n] = '\0';
    static const char deleted[] = " (deleted)";
    size_t len = (size_t)n, suffix = sizeof(deleted) - 1;
    if (len >= suffix && strcmp(exe + len - suffix, deleted) == 0)
        exe[len - suffix] = '\0';
    enum ref_mode mode;
    return trusted_executable(exe, &mode);
}

static enum validation validate_process(pid_t pid, const daemon_ref *expected,
                                        daemon_ref *out)
{
    if (pid <= 0)
        return VALID_GONE;
    if (kill(pid, 0) != 0) {
        if (errno == ESRCH)
            return VALID_GONE;
        if (errno != EPERM)
            return VALID_UNKNOWN;
    }
    char state = 0, arg0[PATH_MAX], exe[PATH_MAX];
    uint64_t ticks = 0;
    if (read_proc_stat(pid, &state, &ticks) != 0) {
        if (kill(pid, 0) != 0 && errno == ESRCH)
            return VALID_GONE;
        return VALID_UNKNOWN;
    }
    if (state == 'Z' || state == 'X')
        return VALID_GONE;
    if (expected && expected->start_ticks != ticks)
        return VALID_FOREIGN;
    int cmdline = read_cmdline(pid, arg0, sizeof(arg0));
    if (cmdline < 0)
        return VALID_UNKNOWN;
    if (cmdline == 0)
        return VALID_FOREIGN;
    enum ref_mode mode = REF_LEGACY;
    dev_t dev = 0;
    ino_t ino = 0;
    int exe_result = read_executable(pid, exe, sizeof(exe), &dev, &ino, &mode);
    if (exe_result < 0)
        return VALID_UNKNOWN;
    if (exe_result == 0)
        return VALID_FOREIGN;
    if (strcmp(path_basename(arg0), path_basename(exe)) != 0)
        return VALID_FOREIGN;
    if (expected && (expected->exe_dev != dev || expected->exe_ino != ino))
        return VALID_FOREIGN;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->mode = mode;
        out->pid = pid;
        out->start_ticks = ticks;
        out->exe_dev = dev;
        out->exe_ino = ino;
    }
    return VALID_OK;
}

static int same_identity(const daemon_ref *a, const daemon_ref *b)
{
    return a->pid == b->pid && a->start_ticks == b->start_ticks
        && a->exe_dev == b->exe_dev && a->exe_ino == b->exe_ino;
}

static int add_ref(proc_snapshot *snapshot, const daemon_ref *ref)
{
    for (size_t i = 0; i < snapshot->count; ++i)
        if (same_identity(&snapshot->refs[i], ref))
            return 0;
    if (snapshot->count == DAEMON_SCAN_MAX)
        return -1;
    snapshot->refs[snapshot->count++] = *ref;
    return 0;
}

static int inspect_hint(proc_snapshot *snapshot, const char *buf, size_t len,
                        enum ref_source source)
{
    pid_t pid;
    if (parse_pid_text(buf, len, &pid) != 0 || pid == getpid())
        return 0;
    daemon_ref ref;
    enum validation result = validate_process(pid, NULL, &ref);
    if (result == VALID_UNKNOWN) {
        snapshot->uncertain = 1;
        return 0;
    }
    if (result != VALID_OK)
        return 0;
    ref.source = source;
    return add_ref(snapshot, &ref);
}

static int scan_processes(proc_snapshot *snapshot)
{
    DIR *dir = opendir(proc_path());
    if (!dir)
        return -1;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno) {
                closedir(dir);
                return -1;
            }
            break;
        }
        char *end = NULL;
        errno = 0;
        long value = strtol(entry->d_name, &end, 10);
        if (errno || end == entry->d_name || *end || value <= 0
            || value > INT_MAX || value == (long)getpid())
            continue;
        daemon_ref ref;
        enum validation result = validate_process((pid_t)value, NULL, &ref);
        if (result == VALID_OK) {
            ref.source = REF_PROC_SCAN;
            if (add_ref(snapshot, &ref) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (result == VALID_UNKNOWN
                   && process_has_trusted_exe((pid_t)value)) {
            snapshot->uncertain = 1;
        }
    }
    return closedir(dir) == 0 ? 0 : -1;
}

static int take_snapshot(int lockfd, proc_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    char primary[64], legacy[64];
    size_t primary_len = 0, legacy_len = 0;
    int rc;
    if (lockfd >= 0)
        rc = read_small_fd(lockfd, primary, sizeof(primary), &primary_len);
    else
        rc = read_small_path(primary_path(), primary, sizeof(primary),
                             &primary_len);
    if (rc != 0
        || read_small_path(legacy_path(), legacy, sizeof(legacy),
                           &legacy_len) != 0)
        return -1;
    if (inspect_hint(snapshot, primary, primary_len, REF_PRIMARY_FILE) != 0
        || inspect_hint(snapshot, legacy, legacy_len, REF_LEGACY_FILE) != 0
        || scan_processes(snapshot) != 0)
        return -1;
    return 0;
}

static int stable_refs(int lockfd, daemon_ref *refs, size_t *count)
{
    proc_snapshot first, second;
    if (take_snapshot(lockfd, &first) != 0
        || sleep_millis(DAEMON_STABILIZE_MS) != 0)
        return -1;
    test_hook("between_snapshots", 0);
    if (take_snapshot(lockfd, &second) != 0)
        return -1;
    if (first.uncertain || second.uncertain)
        return -1;
    /* A daemon that appears during stabilization must be reconciled on the
     * next pass; accepting only the intersection could publish beside it. */
    for (size_t j = 0; j < second.count; ++j) {
        int seen = 0;
        for (size_t i = 0; i < first.count; ++i)
            if (same_identity(&first.refs[i], &second.refs[j])) {
                seen = 1;
                break;
            }
        if (!seen)
            return 1;
    }
    *count = 0;
    for (size_t i = 0; i < first.count; ++i) {
        for (size_t j = 0; j < second.count; ++j) {
            if (!same_identity(&first.refs[i], &second.refs[j]))
                continue;
            if (*count == DAEMON_SCAN_MAX)
                return -1;
            refs[*count] = second.refs[j];
            if (first.refs[i].source != REF_PROC_SCAN)
                refs[*count].source = first.refs[i].source;
            (*count)++;
            break;
        }
    }
    return 0;
}

static int inode_is_current(int fd)
{
    struct stat held, visible;
    if (fstat(fd, &held) != 0)
        return -1;
    if (stat(primary_path(), &visible) != 0)
        return errno == ENOENT ? 0 : -1;
    return held.st_dev == visible.st_dev && held.st_ino == visible.st_ino;
}

static int set_lock(int fd)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLK, &lock);
}

static int get_lock_owner(int fd, pid_t *owner)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_GETLK, &lock) != 0)
        return -1;
    *owner = lock.l_type == F_UNLCK ? 0 : lock.l_pid;
    return 0;
}

static int claim_visible_file(int *fd_out)
{
    int fd = open(primary_path(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (set_lock(fd) == 0) {
            *fd_out = fd;
            return 1;
        }
        if (errno != EACCES && errno != EAGAIN)
            break;
        pid_t owner = 0;
        if (get_lock_owner(fd, &owner) != 0)
            break;
        if (owner == 0)
            continue;
        daemon_ref ref;
        enum validation result = validate_process(owner, NULL, &ref);
        if (result == VALID_OK) {
            close(fd);
            return 0;
        }
        if (result != VALID_GONE)
            break;
        /* The reported owner exited while it was being validated. */
    }
    close(fd);
    return -1;
}

static int write_all_at(int fd, const char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, buf + done, len - done, (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int publish_primary(int fd, pid_t pid)
{
    char line[32];
    int n = snprintf(line, sizeof(line), "%d\n", (int)pid);
    if (n <= 0 || (size_t)n >= sizeof(line) || ftruncate(fd, 0) != 0
        || write_all_at(fd, line, (size_t)n) != 0 || fsync(fd) != 0)
        return -1;
    return 0;
}

static int make_parent(const char *path)
{
    char parent[PATH_MAX];
    size_t len = strlen(path);
    if (!len || len >= sizeof(parent))
        return -1;
    memcpy(parent, path, len + 1);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return 0;
    *slash = '\0';
    if (mkdir(parent, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static int atomic_write_hint(pid_t pid)
{
    const char *path = legacy_path();
    if (make_parent(path) != 0)
        return -1;
    char temp[PATH_MAX], line[32];
    int n = snprintf(temp, sizeof(temp), "%s.%d.tmp", path, (int)getpid());
    int line_n = snprintf(line, sizeof(line), "%d\n", (int)pid);
    if (n <= 0 || (size_t)n >= sizeof(temp) || line_n <= 0
        || (size_t)line_n >= sizeof(line))
        return -1;
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0 && errno == EEXIST) {
        unlink(temp);
        fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    }
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < (size_t)line_n) {
        ssize_t wrote = write(fd, line + done, (size_t)line_n - done);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            break;
        done += (size_t)wrote;
    }
    int ok = done == (size_t)line_n && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    if (ok && rename(temp, path) != 0)
        ok = 0;
    if (!ok) {
        int saved = errno;
        unlink(temp);
        errno = saved;
        return -1;
    }
    return 0;
}

static int hint_is_pid(const char *path, pid_t pid)
{
    char buf[64];
    size_t len = 0;
    pid_t found = 0;
    return read_small_path(path, buf, sizeof(buf), &len) == 0
        && parse_pid_text(buf, len, &found) == 0 && found == pid;
}

static void remove_own_legacy_hint(pid_t pid)
{
    const char *path = legacy_path();
    if (!hint_is_pid(path, pid))
        return;
    char claim[PATH_MAX];
    int n = snprintf(claim, sizeof(claim), "%s.release.%d", path,
                     (int)getpid());
    if (n <= 0 || (size_t)n >= sizeof(claim))
        return;
    unlink(claim);
    if (rename(path, claim) != 0)
        return;
    if (!hint_is_pid(claim, pid) && link(claim, path) != 0 && errno != EEXIST) {
        /* Keep the claimed file for diagnosis rather than overwrite a race. */
        return;
    }
    unlink(claim);
}

static int ref_still_exact(const daemon_ref *ref)
{
    daemon_ref current;
    enum validation result = validate_process(ref->pid, ref, &current);
    return result == VALID_OK && same_identity(ref, &current);
}

static int wait_for_ref_exit(const daemon_ref *ref, int timeout_ms)
{
    int64_t start = monotonic_millis();
    if (start < 0)
        return -1;
    for (;;) {
        int status;
        (void)waitpid(ref->pid, &status, WNOHANG);
        daemon_ref current;
        enum validation result = validate_process(ref->pid, ref, &current);
        if (result == VALID_GONE)
            return 0;
        if (result != VALID_OK || !same_identity(ref, &current))
            return -1;
        int64_t now = monotonic_millis();
        if (now < 0 || now - start >= timeout_ms)
            return -1;
        if (sleep_millis(DAEMON_POLL_MS) != 0)
            return -1;
    }
}

static int stop_ref(const daemon_ref *ref, int timeout_ms, int lockfd)
{
    test_hook("before_signal_revalidate", ref->pid);
    if (!ref_still_exact(ref))
        return -1;
    if (lockfd >= 0 && inode_is_current(lockfd) != 1)
        return STOP_RECLAIM;
    if (kill(ref->pid, SIGTERM) != 0) {
        if (errno == ESRCH)
            return validate_process(ref->pid, ref, NULL) == VALID_GONE ? 0 : -1;
        return -1;
    }
    return wait_for_ref_exit(ref, timeout_ms);
}

static void abandon_publication(int fd, int primary_published)
{
    if (legacy_hint_written)
        remove_own_legacy_hint(getpid());
    legacy_hint_written = 0;
    if (fd >= 0) {
        if (primary_published) {
        (void)ftruncate(fd, 0);
        (void)fsync(fd);
        }
        close(fd);
    }
}

int daemon_claim(int *lockfd)
{
    if (!lockfd) {
        errno = EINVAL;
        return -1;
    }
    *lockfd = -1;
    if (claimed_pid == getpid() && claimed_fd >= 0)
        return 0;
    int64_t start = monotonic_millis();
    if (start < 0)
        return -1;
    int fd = -1;
    int primary_published = 0;
    for (;;) {
        int64_t now = monotonic_millis();
        if (now < 0 || now - start >= DAEMON_RECONCILE_MS)
            goto fail;
        if (fd < 0) {
            int claim = claim_visible_file(&fd);
            if (claim <= 0) {
                if (legacy_hint_written)
                    remove_own_legacy_hint(getpid());
                legacy_hint_written = 0;
                return claim;
            }
        }
        int current = inode_is_current(fd);
        if (current < 0)
            goto fail;
        if (!current) {
            if (legacy_hint_written)
                remove_own_legacy_hint(getpid());
            legacy_hint_written = 0;
            close(fd);
            fd = -1;
            primary_published = 0;
            continue;
        }

        daemon_ref refs[DAEMON_SCAN_MAX];
        size_t count = 0;
        int stable = stable_refs(fd, refs, &count);
        if (stable < 0)
            goto fail;
        if (stable > 0)
            continue;
        if (count) {
            if (inode_is_current(fd) != 1)
                continue;
            now = monotonic_millis();
            if (now < 0)
                goto fail;
            int64_t remaining = DAEMON_RECONCILE_MS - (now - start);
            if (remaining <= 0)
                goto fail;
            int timeout = remaining < DAEMON_LEGACY_STOP_MS
                ? (int)remaining : DAEMON_LEGACY_STOP_MS;
            int stopped = stop_ref(&refs[0], timeout, fd);
            if (stopped == STOP_RECLAIM) {
                if (legacy_hint_written)
                    remove_own_legacy_hint(getpid());
                legacy_hint_written = 0;
                close(fd);
                fd = -1;
                primary_published = 0;
                continue;
            }
            if (stopped != 0)
                goto fail;
            continue;
        }

        uint64_t self_ticks = 0;
        char state = 0;
        if (read_proc_stat(getpid(), &state, &self_ticks) != 0
#ifdef DAEMON_SINGLETON_TEST
            /* The host test proc tree may intentionally omit self. */
            && (self_ticks = (uint64_t)getpid()) == 0
#endif
           )
            goto fail;
        if (state == 'Z' || state == 'X')
            goto fail;
        if (inode_is_current(fd) != 1)
            continue;
        primary_published = 1;
        if (publish_primary(fd, getpid()) != 0
            || atomic_write_hint(getpid()) != 0)
            goto fail;
        legacy_hint_written = 1;

        stable = stable_refs(fd, refs, &count);
        if (stable < 0)
            goto fail;
        if (stable > 0)
            continue;
        current = inode_is_current(fd);
        if (current < 0)
            goto fail;
        if (!current) {
            remove_own_legacy_hint(getpid());
            legacy_hint_written = 0;
            close(fd);
            fd = -1;
            primary_published = 0;
            continue;
        }
        char own[64];
        size_t own_len = 0;
        pid_t own_pid = 0;
        int primary_matches = read_small_fd(fd, own, sizeof(own), &own_len) == 0
            && parse_pid_text(own, own_len, &own_pid) == 0
            && own_pid == getpid();
        int legacy_matches = hint_is_pid(legacy_path(), getpid());
        if (!primary_matches || !legacy_matches)
            continue;
        if (count) {
            now = monotonic_millis();
            if (now < 0)
                goto fail;
            int64_t remaining = DAEMON_RECONCILE_MS - (now - start);
            if (remaining <= 0)
                goto fail;
            int timeout = remaining < DAEMON_LEGACY_STOP_MS
                ? (int)remaining : DAEMON_LEGACY_STOP_MS;
            int stopped = stop_ref(&refs[0], timeout, fd);
            if (stopped == STOP_RECLAIM) {
                if (legacy_hint_written)
                    remove_own_legacy_hint(getpid());
                legacy_hint_written = 0;
                close(fd);
                fd = -1;
                primary_published = 0;
                continue;
            }
            if (stopped != 0)
                goto fail;
            continue;
        }
        claimed_pid = getpid();
        claimed_ticks = self_ticks;
        claimed_fd = fd;
        *lockfd = fd;
        return 1;
    }

fail:
    abandon_publication(fd, primary_published);
    return -1;
}

void daemon_release(int lockfd)
{
    if (claimed_pid == getpid() && lockfd >= 0 && lockfd == claimed_fd) {
        char state = 0;
        uint64_t ticks = 0;
        int same_generation = read_proc_stat(getpid(), &state, &ticks) == 0
            && ticks == claimed_ticks;
#ifdef DAEMON_SINGLETON_TEST
        if (!same_generation && claimed_ticks == (uint64_t)getpid())
            same_generation = 1;
#endif
        if (legacy_hint_written && same_generation)
            remove_own_legacy_hint(claimed_pid);
        legacy_hint_written = 0;
        claimed_pid = 0;
        claimed_ticks = 0;
        claimed_fd = -1;
    }
    if (lockfd >= 0)
        close(lockfd);
}

static int lookup_daemon(daemon_ref *out)
{
    memset(out, 0, sizeof(*out));
    int fd = open(primary_path(), O_RDWR);
    if (fd >= 0) {
        pid_t owner = 0;
        if (get_lock_owner(fd, &owner) != 0) {
            close(fd);
            return -1;
        }
        if (owner > 0) {
            daemon_ref ref;
            enum validation result = validate_process(owner, NULL, &ref);
            if (result == VALID_GONE) {
                if (get_lock_owner(fd, &owner) != 0) {
                    close(fd);
                    return -1;
                }
                if (owner == 0) {
                    close(fd);
                    return lookup_daemon(out);
                }
                result = validate_process(owner, NULL, &ref);
            }
            close(fd);
            if (result != VALID_OK)
                return -1;
            ref.source = REF_PRIMARY_LOCK;
            ref.mode = REF_CURRENT;
            *out = ref;
            return 1;
        }
        close(fd);
    } else if (errno != ENOENT) {
        return -1;
    }

    daemon_ref refs[DAEMON_SCAN_MAX];
    size_t count = 0;
    if (stable_refs(-1, refs, &count) != 0)
        return -1;
    if (count == 0)
        return 0;
    if (count != 1)
        return -1;
    refs[0].mode = REF_LEGACY;
    *out = refs[0];
    return 1;
}

int stop_daemon(void)
{
    daemon_ref original;
    int found = lookup_daemon(&original);
    if (found <= 0)
        return found;
    test_hook("stop_before_signal_revalidate", original.pid);
    if (!ref_still_exact(&original)) {
        daemon_ref replacement;
        int again = lookup_daemon(&replacement);
        return again == 0 ? 0 : -1;
    }
    if (kill(original.pid, SIGTERM) != 0) {
        if (errno != ESRCH)
            return -1;
        daemon_ref replacement;
        int again = lookup_daemon(&replacement);
        return again == 0 ? 0 : -1;
    }

    int timeout = original.mode == REF_CURRENT
        ? DAEMON_CURRENT_STOP_MS : DAEMON_LEGACY_STOP_MS;
    int64_t start = monotonic_millis();
    if (start < 0)
        return -1;
    for (;;) {
        int status;
        (void)waitpid(original.pid, &status, WNOHANG);
        daemon_ref current;
        int current_found = lookup_daemon(&current);
        if (current_found == 0)
            return 0;
        if (current_found < 0 || !same_identity(&original, &current))
            return -1;
        int64_t now = monotonic_millis();
        if (now < 0 || now - start >= timeout)
            return -1;
        if (sleep_millis(DAEMON_POLL_MS) != 0)
            return -1;
    }
}
