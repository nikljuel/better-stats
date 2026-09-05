#define _GNU_SOURCE
#include "daemon.h"
#include "daemon_singleton.h"
#include "tracker.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(PLATFORM_FC) || defined(BETTERSTATS_DEVICE_STATE_TEST)
#include <inkview.h>
#define HAVE_DEVICE_STATE 1
#endif

#ifndef PROC_ROOT
#define PROC_ROOT "/proc"
#endif
#ifndef READER_START_GRACE_SECONDS
#define READER_START_GRACE_SECONDS 5
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define NS_PER_SECOND 1000000000LL
#define CLOCK_TOLERANCE_NS (2LL * NS_PER_SECOND)
#ifndef DAEMON_TERMINAL_RETRY_MS
#define DAEMON_TERMINAL_RETRY_MS 3000
#endif

#if defined(CLOCK_BOOTTIME) || defined(BETTERSTATS_CLOCK_BOOTTIME)
#define BETTERSTATS_HAS_BOOTTIME 1
#ifndef BETTERSTATS_CLOCK_BOOTTIME
#define BETTERSTATS_CLOCK_BOOTTIME CLOCK_BOOTTIME
#endif
static int boottime_unavailable;
#endif

static volatile sig_atomic_t running = 1;

static void on_term(int sig)
{
    (void)sig;
    running = 0;
}

enum marker_status { MARKER_ABSENT, MARKER_VALID, MARKER_INVALID, MARKER_IO };
enum process_state { PROCESS_DEAD, PROCESS_ALIVE, PROCESS_UNKNOWN };
enum reader_state { RS_UNTRACKED, RS_TRACKING, RS_FROZEN, RS_RESTORE_PENDING };
enum marker_relation { REL_LEGACY, REL_WAIT, REL_MATCH, REL_STALE };
enum foreground_state { FG_UNKNOWN = -1, FG_BACKGROUND = 0, FG_FOREGROUND = 1 };

typedef struct {
    int pid;
    int64_t started_at;
    uint64_t start_ticks;
    int has_ticks;
    int has_book;
    char book[PATH_MAX];
} reader_marker;

typedef struct {
    int pid;
    int64_t marker_started;
    uint64_t start_ticks;
    int64_t bookid;
    int64_t opentime;
    int modern;
} reader_session;

static int read_proc_stat(int pid, char *state, uint64_t *start_ticks)
{
    char path[64], line[4096];
    snprintf(path, sizeof(path), PROC_ROOT "/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(line, sizeof(line), f) != NULL && !ferror(f);
    fclose(f);
    if (!ok)
        return -1;
    char *rest = strrchr(line, ')');
    if (!rest || rest[1] != ' ' || !rest[2])
        return -1;
    rest += 2;
    *state = *rest;
    for (int field = 1; field < 20; ++field) {
        char *space = strchr(rest, ' ');
        if (!space)
            return -1;
        rest = space + 1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(rest, &end, 10);
    if (errno || end == rest || (*end != ' ' && *end != '\n' && *end != '\0'))
        return -1;
    *start_ticks = (uint64_t)value;
    return 0;
}

static enum process_state process_state_for(int pid, uint64_t expected,
                                            int have_expected,
                                            uint64_t *actual_out)
{
    if (pid <= 0)
        return PROCESS_DEAD;
    if (kill(pid, 0) != 0) {
        if (errno == ESRCH)
            return PROCESS_DEAD;
        if (errno != EPERM)
            return PROCESS_UNKNOWN;
    }
    char state = 0;
    uint64_t actual = 0;
    if (read_proc_stat(pid, &state, &actual) != 0)
        return PROCESS_UNKNOWN;
    if (actual_out)
        *actual_out = actual;
    if ((have_expected && actual != expected) || state == 'Z' || state == 'X')
        return PROCESS_DEAD;
    return PROCESS_ALIVE;
}

static enum marker_status read_marker_path(const char *path, reader_marker *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (!f)
        return errno == ENOENT ? MARKER_ABSENT : MARKER_IO;
    char header[160] = "", book[PATH_MAX] = "";
    int got_header = fgets(header, sizeof(header), f) != NULL;
    int got_book = got_header && fgets(book, sizeof(book), f) != NULL;
    int extra = got_book ? fgetc(f) : EOF;
    int io = ferror(f);
    if (fclose(f) != 0)
        io = 1;
    if (io)
        return MARKER_IO;
    if (!got_header || extra != EOF)
        return MARKER_INVALID;

    int used = 0, pid = 0;
    long long started = 0;
    unsigned long long ticks = 0;
    int fields = sscanf(header, " %d %lld %llu %n",
                        &pid, &started, &ticks, &used);
    if (fields < 3) {
        ticks = 0;
        used = 0;
        fields = sscanf(header, " %d %lld %n", &pid, &started, &used);
    }
    if (fields < 2) {
        started = 0;
        used = 0;
        fields = sscanf(header, " %d %n", &pid, &used);
    }
    while (header[used] == ' ' || header[used] == '\t'
           || header[used] == '\r' || header[used] == '\n')
        used++;
    if (fields < 1 || fields > 3 || header[used] || pid <= 0 || started < 0
        || (fields == 3 && ticks == 0))
        return MARKER_INVALID;

    if (got_book) {
        size_t n = strlen(book);
        while (n && (book[n - 1] == '\n' || book[n - 1] == '\r'))
            book[--n] = '\0';
        if (!n || book[0] != '/')
            return MARKER_INVALID;
        memcpy(out->book, book, n + 1);
        out->has_book = 1;
    }
    out->pid = pid;
    out->started_at = (int64_t)started;
    out->start_ticks = (uint64_t)ticks;
    out->has_ticks = fields == 3;
    return MARKER_VALID;
}

static enum marker_status read_reader_marker(reader_marker *out)
{
    return read_marker_path(READER_PIDFILE, out);
}

static int same_marker(const reader_marker *a, const reader_marker *b)
{
    return a->pid == b->pid && a->started_at == b->started_at
        && a->has_ticks == b->has_ticks
        && (!a->has_ticks || a->start_ticks == b->start_ticks)
        && a->has_book == b->has_book
        && (!a->has_book || !strcmp(a->book, b->book));
}

typedef struct { int active; char path[PATH_MAX]; } marker_claim;

static int claim_marker(const reader_marker *expected, marker_claim *claim)
{
    memset(claim, 0, sizeof(*claim));
    int n = snprintf(claim->path, sizeof(claim->path), "%s.claim.%d",
                     READER_PIDFILE, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(claim->path))
        return -1;
    unlink(claim->path);
    if (rename(READER_PIDFILE, claim->path) != 0)
        return errno == ENOENT ? 0 : -1;
    claim->active = 1;
    reader_marker found;
    if (read_marker_path(claim->path, &found) != MARKER_VALID
        || !same_marker(&found, expected)) {
        if (link(claim->path, READER_PIDFILE) != 0 && errno != EEXIST) {
            unlink(claim->path);
            claim->active = 0;
            return -1;
        }
        unlink(claim->path);
        claim->active = 0;
        return 0;
    }
    return 1;
}

static int finish_marker_claim(marker_claim *claim, int consume)
{
    if (!claim->active)
        return 0;
    int ok = 1;
    if (!consume && link(claim->path, READER_PIDFILE) != 0 && errno != EEXIST)
        ok = 0;
    if (unlink(claim->path) != 0 && errno != ENOENT)
        ok = 0;
    claim->active = 0;
    return ok ? 0 : -1;
}

/* Legacy one/two-field markers cannot distinguish PID reuse. Upgrade the
 * exact claimed file before binding it; a concurrent handler wins via EEXIST. */
static int upgrade_reader_marker(reader_marker *marker, uint64_t start_ticks)
{
    if (marker->has_ticks)
        return 1;
    if (!start_ticks)
        return -1;
    marker_claim claim;
    int claimed = claim_marker(marker, &claim);
    if (claimed <= 0)
        return claimed;

    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s.upgrade", claim.path);
    if (n < 0 || (size_t)n >= sizeof(temp)) {
        finish_marker_claim(&claim, 0);
        return -1;
    }
    unlink(temp);
    FILE *f = fopen(temp, "w");
    int ok = f != NULL;
    if (ok && fprintf(f, "%d %lld %llu\n", marker->pid,
                      (long long)marker->started_at,
                      (unsigned long long)start_ticks) <= 0)
        ok = 0;
    if (ok && marker->has_book && fprintf(f, "%s\n", marker->book) <= 0)
        ok = 0;
    if (f && fclose(f) != 0)
        ok = 0;
    if (!ok || rename(temp, claim.path) != 0) {
        unlink(temp);
        finish_marker_claim(&claim, 0);
        return -1;
    }

    reader_marker upgraded = *marker;
    upgraded.has_ticks = 1;
    upgraded.start_ticks = start_ticks;
    if (finish_marker_claim(&claim, 0) != 0)
        return -1;
    reader_marker published;
    if (read_reader_marker(&published) != MARKER_VALID
        || !same_marker(&published, &upgraded))
        return 0;
    *marker = upgraded;
    return 1;
}

static int write_reader_session(const reader_session *session)
{
    char temp[PATH_MAX];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%d",
                     READER_SESSION, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(temp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *f = fopen(temp, "w");
    if (!f)
        return -1;
    errno = 0;
    int saved = 0;
    if (fprintf(f, "%d %lld %llu %lld %lld\n", session->pid,
                (long long)session->marker_started,
                (unsigned long long)session->start_ticks,
                (long long)session->bookid,
                (long long)session->opentime) <= 0)
        saved = errno ? errno : EIO;
    if (fclose(f) != 0 && !saved)
        saved = errno ? errno : EIO;
    if (!saved && rename(temp, READER_SESSION) == 0)
        return 0;
    if (!saved)
        saved = errno ? errno : EIO;
    unlink(temp);
    errno = saved;
    return -1;
}

static int read_reader_session(reader_session *out)
{
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(READER_SESSION, "r");
    if (!f)
        return -1;
    char line[192] = "";
    int got = fgets(line, sizeof(line), f) != NULL;
    int extra = got ? fgetc(f) : EOF;
    int io = ferror(f);
    if (fclose(f) != 0)
        io = 1;
    if (!got || extra != EOF || io)
        return -1;
    int used = 0;
    long long started = 0, book = 0, open = 0;
    unsigned long long ticks = 0;
    int fields = sscanf(line, " %d %lld %llu %lld %lld %n", &out->pid,
                        &started, &ticks, &book, &open, &used);
    if (fields != 5) {
        used = 0;
        fields = sscanf(line, " %d %lld %lld %n",
                        &out->pid, &book, &open, &used);
        out->modern = 0;
    } else {
        out->modern = 1;
    }
    while (line[used] == ' ' || line[used] == '\t'
           || line[used] == '\r' || line[used] == '\n')
        used++;
    if ((fields != 3 && fields != 5) || line[used] || out->pid < 0
        || book <= 0 || open <= 0 || (out->modern && started < 0)
        || (out->modern && out->pid > 0 && !ticks))
        return -1;
    out->marker_started = out->modern ? (int64_t)started : 0;
    out->start_ticks = out->modern ? (uint64_t)ticks : 0;
    out->bookid = (int64_t)book;
    out->opentime = (int64_t)open;
    return 0;
}

typedef struct {
    int known;
    int task;
    int subtask;
    int pid;
    char app[128];
    int has_book;
    char book[PATH_MAX];
} active_task_info;

static active_task_info current_task(void)
{
    active_task_info result;
    memset(&result, 0, sizeof(result));
#ifdef HAVE_DEVICE_STATE
    GetActiveTask(&result.task, &result.subtask);
    taskinfo *info = result.task > 0 ? GetTaskInfo(result.task) : NULL;
    if (info && info->mainpid > 0) {
        result.known = 1;
        result.pid = (int)info->mainpid;
        snprintf(result.app, sizeof(result.app), "%s",
                 info->appname ? info->appname : "");
        const char *book = NULL;
        for (int i = 0; info->subtasks && i < info->nsubtasks; ++i)
            if (info->subtasks[i].id == result.subtask) {
                book = info->subtasks[i].book;
                break;
            }
        if (!book && info->subtasks && info->nsubtasks == 1)
            book = info->subtasks[0].book;
        if (book && book[0] == '/') {
            snprintf(result.book, sizeof(result.book), "%s", book);
            result.has_book = 1;
        }
    }
#endif
    return result;
}

static int current_book_fallback(const active_task_info *task,
                                 char *out, size_t size)
{
#ifdef HAVE_DEVICE_STATE
    FILE *f = fopen(CURRENT_BOOK_FILE, "r");
    if (!f)
        return 0;
    char line[PATH_MAX];
    int got = fgets(line, sizeof(line), f) != NULL;
    int extra = got ? fgetc(f) : EOF;
    int io = ferror(f);
    if (fclose(f) != 0)
        io = 1;
    if (!got || extra != EOF || io)
        return 0;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    int owner_task = 0, owner_subtask = 0;
    if (!n || line[0] != '/'
        || FindTaskByBook(line, &owner_task, &owner_subtask) < 0
        || owner_task != task->task || owner_subtask != task->subtask)
        return 0;
    snprintf(out, size, "%s", line);
    return 1;
#else
    (void)task; (void)out; (void)size;
    return 0;
#endif
}

static int active_book_path(const active_task_info *task,
                            char *out, size_t size)
{
    if (task->has_book) {
        snprintf(out, size, "%s", task->book);
        return 1;
    }
    return task->known && current_book_fallback(task, out, size);
}

static int device_locked(void)
{
#ifdef HAVE_DEVICE_STATE
    return get_keylock() != 0;
#else
    return 0;
#endif
}

static enum foreground_state reader_foreground(const active_task_info *task,
                                                int reader_pid)
{
    if (!task->known)
        return FG_UNKNOWN;
    return task->pid == reader_pid ? FG_FOREGROUND : FG_BACKGROUND;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : path ? path : "";
}

static int fallback_app_allowed(const active_task_info *task)
{
    if (!task->known)
        return 0;
    const char *app = base_name(task->app);
    static const char *blocked[] = {
        "bookshelf.app", "BetterStats.app", "betterstats-qt-softfp",
        "betterstats-inkview-softfp", "betterstats-inkview-hardfp"
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); ++i)
        if (!strcmp(app, blocked[i]))
            return 0;
    return 1;
}

static enum marker_relation marker_relation(const reader_marker *marker,
                                            const pb_state *fw)
{
    if (marker->started_at == 0)
        return REL_LEGACY;
    if (marker->started_at > fw->opentime
        && marker->started_at - fw->opentime > READER_START_GRACE_SECONDS)
        return REL_WAIT;
    if (fw->opentime > marker->started_at
        && fw->opentime - marker->started_at > READER_START_GRACE_SECONDS)
        return REL_STALE;
    return REL_MATCH;
}

static int same_firmware_session(const pb_state *fw, int64_t book, int64_t open)
{
    return fw && fw->bookid == book && fw->opentime == open;
}

enum count_mode { COUNT_NONE, COUNT_MONO, COUNT_BOOT };

typedef struct {
    int64_t boot_ns, mono_ns, wall_ns;
} clock_sample;

typedef struct {
    int result;             /* 0 state, 1 absent/ambiguous, <0 DB error */
    int trusted;            /* selected through a live task or marker path */
    int task_trusted;
    int marker_book_matches;
    int conflict;
    pb_state state;
} firmware_sample;

typedef struct {
    int valid, task, subtask, pid;
    uint64_t start_ticks;
    int64_t bookid, opentime;
    char app[128];
    char book[PATH_MAX];
} fallback_key;

enum pending_kind { PENDING_NONE, PENDING_HANDLER, PENDING_FALLBACK };
enum pending_phase { PENDING_CLOSE_OLD, PENDING_PREPARE, PENDING_OBSERVE };

typedef struct {
    enum pending_kind kind;
    enum pending_phase phase;
    reader_session reader;
    fallback_key context;
    pb_state first, latest, old_final;
    int have_old_final;
    int bounded_close;
    int64_t base_present;
    int64_t credit_ns;
    int64_t start_boot, start_wall;
    int64_t close_wall;
    int target_bounded_close;
    int64_t target_close_wall;
} pending_open;

typedef struct {
    int valid;
    enum count_mode previous_mode;
    reader_marker marker;
    fallback_key context;
    int64_t credit_ns;
    int64_t start_boot, start_wall;
} marker_probe;

typedef struct {
    tracker tracker;
    enum reader_state reader_state;
    reader_session reader;
    int recovered, restore_loaded;
    int segment_open;
    int64_t present_ns;
    enum count_mode previous_mode;
    clock_sample previous_clock;
    int have_clock;
    pb_state last_firmware;
    int have_last_firmware;
    fallback_key fallback;
    int fallback_ticks;
    int resolver_task_known, resolver_task, resolver_subtask, resolver_pid;
    uint64_t resolver_start_ticks;
    int64_t replacement_book, replacement_open;
    int replacement_ticks;
    int64_t deferred_close_end;
    int conflict_latched;
    int needs_reconcile, reconcile_ticks;
    pb_state reconcile_state;
    int have_reconcile_state;
    int dead_pid;
    uint64_t dead_start_ticks;
    enum foreground_state last_foreground;
    clock_sample attempted_clock;
    int have_attempted_clock;
    enum count_mode attempted_mode;
    int have_attempted_mode;
    pending_open pending;
    marker_probe probe;
} daemon_runtime;

static int64_t timespec_ns(const struct timespec *value)
{
    return (int64_t)value->tv_sec * NS_PER_SECOND + value->tv_nsec;
}

static int sample_clocks(clock_sample *out)
{
    struct timespec mono, wall;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0
        || clock_gettime(CLOCK_REALTIME, &wall) != 0)
        return -1;
    out->mono_ns = timespec_ns(&mono);
    out->wall_ns = timespec_ns(&wall);
#ifdef BETTERSTATS_HAS_BOOTTIME
    struct timespec boot;
    if (!boottime_unavailable
        && clock_gettime(BETTERSTATS_CLOCK_BOOTTIME, &boot) != 0) {
        if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
            return -1;
        boottime_unavailable = 1;
    }
    out->boot_ns = boottime_unavailable
        ? out->mono_ns : timespec_ns(&boot);
#else
    out->boot_ns = out->mono_ns;
#endif
    return 0;
}

#ifdef BETTERSTATS_DEVICE_STATE_TEST
int daemon_test_boottime_fallback(void)
{
    clock_sample clocks;
#ifdef BETTERSTATS_HAS_BOOTTIME
    boottime_unavailable = 0;
#endif
    return sample_clocks(&clocks) == 0
        && clocks.boot_ns == clocks.mono_ns;
}
#endif

static int clock_deltas(const clock_sample *before, const clock_sample *after,
                        int64_t *boot, int64_t *mono)
{
    *boot = after->boot_ns - before->boot_ns;
    *mono = after->mono_ns - before->mono_ns;
    if (*boot < 0 || *mono < 0)
        return -1;
    if (*mono > *boot)
        *mono = *boot;
    return 0;
}

static int wait_for_tick(void)
{
#ifdef PLATFORM_FC
    static int relative_only;
    struct timespec now, deadline;
#ifdef BETTERSTATS_HAS_BOOTTIME
    clockid_t clock_id = boottime_unavailable
        ? CLOCK_MONOTONIC : BETTERSTATS_CLOCK_BOOTTIME;
#else
    clockid_t clock_id = CLOCK_MONOTONIC;
#endif
    if (!relative_only) {
        if (clock_gettime(clock_id, &now) != 0) {
#ifdef BETTERSTATS_HAS_BOOTTIME
            if (clock_id == BETTERSTATS_CLOCK_BOOTTIME
                && (errno == EINVAL || errno == ENOSYS || errno == ENOTSUP)) {
                boottime_unavailable = 1;
                clock_id = CLOCK_MONOTONIC;
                if (clock_gettime(clock_id, &now) == 0)
                    goto have_now;
            }
#endif
            return -1;
        }
have_now:
        deadline = now;
        deadline.tv_sec += POLL_SECONDS;
        for (;;) {
            int rc = clock_nanosleep(clock_id, TIMER_ABSTIME, &deadline, NULL);
            if (rc == 0 || (rc == EINTR && !running))
                return 0;
            if (rc == EINTR)
                continue;
            if (rc == EINVAL || rc == ENOSYS || rc == ENOTSUP) {
                relative_only = 1;
                break;
            }
            errno = rc;
            return -1;
        }
    }
#endif
    struct timespec remaining = {POLL_SECONDS, 0};
    while (running && nanosleep(&remaining, &remaining) != 0)
        if (errno != EINTR)
            return -1;
    return 0;
}

static int same_snapshot(const pb_state *a, const pb_state *b)
{
    return a->bookid == b->bookid && a->opentime == b->opentime
        && a->position_ts == b->position_ts && a->cpage == b->cpage
        && a->npage == b->npage && a->completed == b->completed
        && a->completed_ts == b->completed_ts
        && !strcmp(a->title, b->title) && !strcmp(a->author, b->author)
        && !strcmp(a->cover, b->cover);
}

static int same_fallback(const fallback_key *a, const fallback_key *b)
{
    return a->valid && b->valid && a->task == b->task
        && a->subtask == b->subtask && a->pid == b->pid
        && a->start_ticks == b->start_ticks
        && a->bookid == b->bookid && a->opentime == b->opentime
        && !strcmp(a->app, b->app) && !strcmp(a->book, b->book);
}

static int fallback_task_matches(const fallback_key *key,
                                 const active_task_info *task)
{
    if (!key->valid || !task->known || task->task != key->task
        || task->subtask != key->subtask || task->pid != key->pid
        || strcmp(base_name(task->app), key->app)
        || process_state_for(task->pid, key->start_ticks, 1, NULL)
               != PROCESS_ALIVE)
        return 0;
    char path[PATH_MAX] = "";
    return active_book_path(task, path, sizeof(path))
        && !strcmp(path, key->book);
}

static int marker_matches_session(const reader_marker *marker,
                                  const reader_session *session)
{
    return marker->pid == session->pid
        && (!session->marker_started
            || marker->started_at == session->marker_started)
        && (!session->start_ticks
            || (marker->has_ticks
                && marker->start_ticks == session->start_ticks));
}

static reader_session session_for_marker(const reader_marker *marker,
                                         uint64_t actual_ticks,
                                         const pb_state *fw)
{
    reader_session value;
    memset(&value, 0, sizeof(value));
    value.pid = marker->pid;
    value.marker_started = marker->started_at;
    value.start_ticks = marker->has_ticks ? marker->start_ticks : actual_ticks;
    value.bookid = fw->bookid;
    value.opentime = fw->opentime;
    value.modern = 1;
    return value;
}

static int discard_marker(const reader_marker *marker)
{
    marker_claim claim;
    int claimed = claim_marker(marker, &claim);
    if (claimed <= 0)
        return claimed;
    return finish_marker_claim(&claim, 1) == 0 ? 1 : -1;
}

static int resolve_firmware(daemon_runtime *runtime,
                            enum marker_status marker_status,
                            const reader_marker *marker,
                            enum process_state marker_process,
                            const active_task_info *task,
                            firmware_sample *out)
{
    memset(out, 0, sizeof(*out));
    out->result = 1;
    uint64_t task_ticks = 0;
    int task_generation_known = task->known
        && process_state_for(task->pid, 0, 0, &task_ticks) == PROCESS_ALIVE;
    if (!task_generation_known || !runtime->resolver_task_known
        || task->task != runtime->resolver_task
        || task->subtask != runtime->resolver_subtask
        || task->pid != runtime->resolver_pid
        || task_ticks != runtime->resolver_start_ticks)
        tracker_invalidate_book_path_cache(&runtime->tracker);
    runtime->resolver_task_known = task_generation_known;
    if (task_generation_known) {
        runtime->resolver_task = task->task;
        runtime->resolver_subtask = task->subtask;
        runtime->resolver_pid = task->pid;
        runtime->resolver_start_ticks = task_ticks;
    }
    int64_t marker_book = 0, task_book = 0;
    int marker_result = 1, task_result = 1;
    int marker_bound = runtime->reader_state != RS_UNTRACKED
        && runtime->reader.pid > 0
        && marker_status == MARKER_VALID
        && marker_matches_session(marker, &runtime->reader);
    char task_path[PATH_MAX] = "";
    int task_has_path = active_book_path(task, task_path, sizeof(task_path));
    int same_path = marker_status == MARKER_VALID && marker->has_book
        && task_has_path && !strcmp(marker->book, task_path);
    if (task_has_path) {
        task_result = tracker_cached_book_id_for_path(&runtime->tracker,
                                                      task_path, &task_book);
    }
    if (marker_status == MARKER_VALID && marker->has_book
        && marker_result != 0) {
        if (same_path && task_result == 0) {
            marker_result = 0;
            marker_book = task_book;
        } else if (marker_process != PROCESS_DEAD || marker_bound
                   || task_result != 0) {
            marker_result = tracker_cached_book_id_for_path(
                &runtime->tracker, marker->book, &marker_book);
        }
    }
    if (marker_result < 0 || task_result < 0) {
        tracker_invalidate_book_path_cache(&runtime->tracker);
        return out->result = -1;
    }
    if (marker_result == 0 && task_result == 0 && marker_book != task_book) {
        /* Finish the exact dead process generation already bound to A before
         * considering the live task for B. Otherwise their trusted paths keep
         * restore/death handling stuck in a permanent conflict. */
        if (marker_bound && marker_process == PROCESS_DEAD) {
            task_result = 1;
        } else {
            out->conflict = 1;
            tracker_invalidate_book_path_cache(&runtime->tracker);
            return out->result = 1;
        }
    }

    int64_t book = marker_result == 0 ? marker_book
        : task_result == 0 ? task_book : 0;
    out->task_trusted = task_result == 0;
    out->marker_book_matches = !marker->has_book
        || (marker_result == 0 && marker_book == book);
    out->trusted = book > 0;
    if (!book && runtime->reader_state != RS_UNTRACKED)
        book = runtime->reader.bookid;
    if (!book) {
        tracker_invalidate_book_path_cache(&runtime->tracker);
        return out->result = 1;
    }

    int marker_is_new = marker_status == MARKER_VALID
        && runtime->reader_state != RS_UNTRACKED
        && runtime->reader.pid > 0
        && !marker_matches_session(marker, &runtime->reader);
    int64_t expected_open = book == runtime->reader.bookid && !marker_is_new
        ? runtime->reader.opentime : 0;
    int64_t marker_started = marker_result == 0 ? marker->started_at : 0;
    out->result = tracker_cached_read_book_state(&runtime->tracker, book,
                                                 expected_open,
                                                 marker_started, &out->state);
    /* A profile row may be replaced in place when the same book is reopened.
     * Only a trusted active-book path may fall back after the bound label has
     * actually disappeared; ambiguity still remains UNKNOWN. */
    if (out->result == 1 && expected_open > 0 && out->trusted) {
        out->result = tracker_cached_read_book_state(&runtime->tracker, book,
                                                     0, marker_started,
                                                     &out->state);
    }
    if (out->result != 0 || !task_generation_known)
        tracker_invalidate_book_path_cache(&runtime->tracker);
    return out->result;
}

static int add_presence(daemon_runtime *runtime, int64_t nanoseconds)
{
    if (nanoseconds < 0 || runtime->present_ns > INT64_MAX - nanoseconds)
        return -1;
    runtime->present_ns += nanoseconds;
    return 0;
}

static int64_t present_seconds(const daemon_runtime *runtime)
{
    return runtime->present_ns / NS_PER_SECOND;
}

static int safe_wall_split(const clock_sample *before,
                           const clock_sample *after, int64_t opened,
                           int64_t boot_delta, int64_t mono_delta,
                           int64_t *before_ns, int64_t *after_ns)
{
    int64_t opened_ns;
    if (boot_delta < 0 || mono_delta < 0 || mono_delta > boot_delta
        || after->wall_ns < before->wall_ns || opened <= 0
        || opened > INT64_MAX / NS_PER_SECOND)
        return 0;
    int64_t realtime_delta = after->wall_ns - before->wall_ns;
    opened_ns = opened * NS_PER_SECOND;
    int64_t difference = realtime_delta >= boot_delta
        ? realtime_delta - boot_delta : boot_delta - realtime_delta;
    if (difference > CLOCK_TOLERANCE_NS
        || opened_ns <= before->wall_ns || opened_ns > after->wall_ns)
        return 0;
    int64_t first = opened_ns - before->wall_ns;
    if (first > boot_delta)
        first = boot_delta;
    int64_t second = realtime_delta - (opened_ns - before->wall_ns);
    if (second < 0)
        return 0;
    if (second > boot_delta - first)
        second = boot_delta - first;
    *before_ns = first;
    *after_ns = second;
    return 1;
}

static int64_t interval_credit(enum count_mode previous,
                               enum count_mode current,
                               int64_t boot_delta, int64_t mono_delta)
{
    if (previous == COUNT_NONE)
        return 0;
    return previous == COUNT_BOOT && current == COUNT_BOOT
        ? boot_delta : mono_delta;
}

static void split_switch_interval(enum count_mode previous,
                                  enum count_mode current,
                                  int old_continuous,
                                  const clock_sample *before,
                                  const clock_sample *after, int64_t opened,
                                  int64_t boot_delta, int64_t mono_delta,
                                  int64_t *before_ns, int64_t *after_ns)
{
    *before_ns = previous != COUNT_NONE ? mono_delta : 0;
    *after_ns = 0;
    if (previous == COUNT_BOOT && current == COUNT_BOOT) {
        int64_t first = 0, second = 0;
        if (safe_wall_split(before, after, opened, boot_delta, mono_delta,
                            &first, &second)) {
            if (old_continuous) {
                *before_ns = first;
                *after_ns = second;
            } else {
                *after_ns = second;
                int64_t remaining = mono_delta > second
                    ? mono_delta - second : 0;
                *before_ns = first < remaining ? first : remaining;
            }
        }
    }
}

#ifdef BETTERSTATS_DEVICE_STATE_TEST
int64_t daemon_test_interval_credit(int previous, int current,
                                    int64_t boot_delta, int64_t mono_delta)
{
    return interval_credit((enum count_mode)previous,
                           (enum count_mode)current,
                           boot_delta, mono_delta);
}

int daemon_test_safe_wall_split(int64_t before_wall, int64_t after_wall,
                                int64_t opened, int64_t boot_delta,
                                int64_t mono_delta, int64_t *before_credit,
                                int64_t *after_credit)
{
    clock_sample before = {0, 0, before_wall};
    clock_sample after = {0, 0, after_wall};
    return safe_wall_split(&before, &after, opened, boot_delta, mono_delta,
                           before_credit, after_credit);
}

void daemon_test_switch_split(int previous, int current,
                              int old_continuous,
                              int64_t before_wall, int64_t after_wall,
                              int64_t opened, int64_t boot_delta,
                              int64_t mono_delta, int64_t *before_credit,
                              int64_t *after_credit)
{
    clock_sample before = {0, 0, before_wall};
    clock_sample after = {0, 0, after_wall};
    split_switch_interval((enum count_mode)previous, (enum count_mode)current,
                          old_continuous, &before, &after, opened, boot_delta,
                          mono_delta, before_credit, after_credit);
}
#endif

static int prepare_segment_at(daemon_runtime *runtime, const pb_state *fw,
                              int64_t present, int64_t boot, int64_t wall)
{
    if (runtime->tracker.cur_book == fw->bookid
        && runtime->tracker.cur_open == fw->opentime) {
        if (tracker_resume(&runtime->tracker, present, boot, wall) != 0)
            return -1;
        runtime->segment_open = 1;
        return 0;
    }
    int prepared = tracker_prepare(&runtime->tracker, fw, present, boot, wall);
    if (prepared < 0)
        return -1;
    if (prepared == 0
        && tracker_resume(&runtime->tracker, present, boot, wall) != 0)
        return -1;
    runtime->segment_open = 1;
    return 0;
}

static int add_pending_credit(daemon_runtime *runtime, int64_t nanoseconds)
{
    if (nanoseconds < 0
        || runtime->pending.credit_ns > INT64_MAX - nanoseconds)
        return -1;
    runtime->pending.credit_ns += nanoseconds;
    return 0;
}

static int transfer_probe_credit(daemon_runtime *runtime, int to_pending)
{
    int64_t credit = runtime->probe.credit_ns;
    int result = to_pending ? add_pending_credit(runtime, credit)
                            : add_presence(runtime, credit);
    if (result != 0)
        return -1;
    memset(&runtime->probe, 0, sizeof(runtime->probe));
    return 0;
}

static int close_segment(daemon_runtime *runtime, const pb_state *state,
                         int64_t boot, int64_t wall)
{
    if (!runtime->segment_open)
        return 0;
    if (tracker_flush(&runtime->tracker, state, present_seconds(runtime),
                      boot, wall) != 0)
        return -1;
    runtime->segment_open = 0;
    return 0;
}

static int close_segment_bounded(daemon_runtime *runtime,
                                 const pb_state *state,
                                 int64_t boot, int64_t wall,
                                 int64_t max_end)
{
    if (!runtime->segment_open)
        return 0;
    if (tracker_flush_bounded(&runtime->tracker, state,
                              present_seconds(runtime), boot, wall,
                              max_end) != 0)
        return -1;
    runtime->segment_open = 0;
    return 0;
}

static void remember_firmware(daemon_runtime *runtime, const pb_state *fw)
{
    runtime->last_firmware = *fw;
    runtime->have_last_firmware = 1;
}

static int adopt_saved_reader(daemon_runtime *runtime,
                              const reader_session *session,
                              const pb_state *fw)
{
    if (!session->start_ticks || write_reader_session(session) != 0)
        return -1;
    runtime->reader = *session;
    runtime->reader_state = RS_TRACKING;
    runtime->last_foreground = FG_FOREGROUND;
    runtime->needs_reconcile = 0;
    runtime->have_reconcile_state = 0;
    runtime->dead_pid = 0;
    remember_firmware(runtime, fw);
    return 0;
}

static int adopt_reader(daemon_runtime *runtime, const reader_marker *marker,
                        uint64_t actual_ticks, const pb_state *fw)
{
    reader_session session = session_for_marker(marker, actual_ticks, fw);
    return adopt_saved_reader(runtime, &session, fw);
}

static int write_frozen_session(daemon_runtime *runtime)
{
    reader_session frozen;
    memset(&frozen, 0, sizeof(frozen));
    frozen.bookid = runtime->reader.bookid;
    frozen.opentime = runtime->reader.opentime;
    frozen.modern = 1;
    return write_reader_session(&frozen);
}

/* Freeze only the process generation that was actually adopted. A marker
 * published by a newer handler is never consumed or used as death evidence. */
static int freeze_reader(daemon_runtime *runtime,
                         enum marker_status marker_status,
                         const reader_marker *marker,
                         const pb_state *final_state,
                         int64_t boot, int64_t wall)
{
    marker_claim claim;
    memset(&claim, 0, sizeof(claim));
    if (marker_status == MARKER_VALID
        && marker_matches_session(marker, &runtime->reader)) {
        int claimed = claim_marker(marker, &claim);
        if (claimed < 0)
            return -1;
    }
    if (close_segment(runtime, final_state, boot, wall) != 0
        || write_frozen_session(runtime) != 0) {
        finish_marker_claim(&claim, 0);
        return -1;
    }
    if (finish_marker_claim(&claim, 1) != 0)
        return -1;
    runtime->dead_pid = runtime->reader.pid;
    runtime->dead_start_ticks = runtime->reader.start_ticks;
    runtime->reader.pid = 0;
    runtime->reader.marker_started = 0;
    runtime->reader.start_ticks = 0;
    runtime->reader_state = RS_FROZEN;
    runtime->needs_reconcile = 1;
    runtime->reconcile_ticks = final_state ? 1 : 0;
    runtime->have_reconcile_state = final_state != NULL;
    if (final_state)
        runtime->reconcile_state = *final_state;
    return 0;
}

static int reconcile_frozen(daemon_runtime *runtime,
                            const firmware_sample *firmware,
                            enum marker_status marker_status,
                            enum process_state marker_process,
                            int64_t boot, int64_t wall)
{
    if (!runtime->needs_reconcile || firmware->result != 0
        || !same_firmware_session(&firmware->state, runtime->reader.bookid,
                                  runtime->reader.opentime)
        || (marker_status == MARKER_VALID
            && marker_process == PROCESS_ALIVE))
        return 0;
    if (runtime->dead_pid > 0
        && process_state_for(runtime->dead_pid, runtime->dead_start_ticks,
                             runtime->dead_start_ticks != 0, NULL)
               != PROCESS_DEAD)
        return 0;
    if (runtime->have_reconcile_state
        && same_snapshot(&runtime->reconcile_state, &firmware->state))
        runtime->reconcile_ticks++;
    else {
        runtime->reconcile_state = firmware->state;
        runtime->have_reconcile_state = 1;
        runtime->reconcile_ticks = 1;
    }
    if (runtime->reconcile_ticks < 2)
        return 0;
    if (tracker_flush(&runtime->tracker, &firmware->state,
                      present_seconds(runtime), boot, wall) != 0)
        return -1;
    runtime->needs_reconcile = 0;
    remember_firmware(runtime, &firmware->state);
    return 0;
}

static enum count_mode reader_count_mode(daemon_runtime *runtime,
                                         const firmware_sample *firmware,
                                         enum process_state process,
                                         const active_task_info *task,
                                         int locked_early, int locked_late)
{
    enum foreground_state foreground = reader_foreground(task,
                                                         runtime->reader.pid);
    if (foreground != FG_UNKNOWN)
        runtime->last_foreground = foreground;
    if (runtime->reader_state != RS_TRACKING || process != PROCESS_ALIVE
        || locked_early || locked_late
        || foreground == FG_BACKGROUND
        || (foreground == FG_UNKNOWN
            && runtime->last_foreground != FG_FOREGROUND))
        return COUNT_NONE;
    if (foreground == FG_UNKNOWN)
        return COUNT_MONO;
    if (firmware->result == 0
        && same_firmware_session(&firmware->state, runtime->reader.bookid,
                                 runtime->reader.opentime))
        return COUNT_BOOT;
    return COUNT_MONO;
}

static int load_restore_state(daemon_runtime *runtime)
{
    if (runtime->restore_loaded)
        return 0;
    runtime->restore_loaded = 1;
    reader_session saved;
    if (read_reader_session(&saved) == 0) {
        runtime->reader = saved;
        runtime->reader_state = RS_RESTORE_PENDING;
    } else if (access(READER_SESSION, F_OK) == 0) {
        if (unlink(READER_SESSION) != 0)
            return -1;
    }
    return 0;
}

static int begin_pending_handler(daemon_runtime *runtime,
                                 enum pending_phase phase,
                                 const reader_marker *marker,
                                 uint64_t actual_ticks,
                                 const pb_state *fw,
                                 const active_task_info *task,
                                 int64_t base_present,
                                 int64_t start_boot, int64_t start_wall,
                                 int already_bound);

static int discard_reader_restore(daemon_runtime *runtime)
{
    if (unlink(READER_SESSION) != 0 && errno != ENOENT)
        return -1;
    memset(&runtime->reader, 0, sizeof(runtime->reader));
    runtime->reader_state = RS_UNTRACKED;
    runtime->last_foreground = FG_UNKNOWN;
    return 0;
}

static int finish_restore(daemon_runtime *runtime,
                          const firmware_sample *firmware,
                          enum marker_status marker_status,
                          reader_marker *marker,
                          enum process_state marker_process,
                          uint64_t marker_actual_ticks,
                          const active_task_info *task,
                          int locked_early, int locked_late,
                          int64_t boot, int64_t wall)
{
    if (runtime->reader_state != RS_RESTORE_PENDING || firmware->result != 0)
        return 0;
    if (!same_firmware_session(&firmware->state, runtime->reader.bookid,
                               runtime->reader.opentime)) {
        if (!firmware->trusted)
            return 0;
        return discard_reader_restore(runtime);
    }
    if (runtime->reader.pid == 0) {
        if (tracker_prepare(&runtime->tracker, &firmware->state,
                            present_seconds(runtime), boot, wall) < 0)
            return -1;
        remember_firmware(runtime, &firmware->state);
        runtime->reader_state = RS_FROZEN;
        runtime->needs_reconcile = 1;
        return 0;
    }

    uint64_t actual = 0;
    enum process_state process = process_state_for(
        runtime->reader.pid, runtime->reader.start_ticks,
        runtime->reader.modern && runtime->reader.start_ticks != 0, &actual);
    if (process == PROCESS_UNKNOWN)
        return 0;
    if (process == PROCESS_DEAD) {
        if (tracker_prepare(&runtime->tracker, &firmware->state,
                            present_seconds(runtime), boot, wall) < 0)
            return -1;
        remember_firmware(runtime, &firmware->state);
        if (marker_status == MARKER_VALID
            && marker_matches_session(marker, &runtime->reader)) {
            int frozen = freeze_reader(runtime, marker_status, marker,
                                       &firmware->state, boot, wall);
            return frozen == 0 ? 0 : frozen < 0 ? -1 : 0;
        }
        runtime->dead_pid = runtime->reader.pid;
        runtime->dead_start_ticks = runtime->reader.start_ticks;
        if (write_frozen_session(runtime) != 0)
            return -1;
        runtime->reader.pid = 0;
        runtime->reader.start_ticks = 0;
        runtime->reader.marker_started = 0;
        runtime->reader_state = RS_FROZEN;
        runtime->needs_reconcile = 1;
        return 0;
    }
    if (!runtime->reader.modern) {
        enum foreground_state foreground = reader_foreground(
            task, runtime->reader.pid);
        int proven = !firmware->conflict && !locked_early && !locked_late
            && marker_status == MARKER_VALID
            && marker_process == PROCESS_ALIVE
            && marker->pid == runtime->reader.pid
            && firmware->marker_book_matches
            && marker_relation(marker, &firmware->state) != REL_WAIT
            && marker_relation(marker, &firmware->state) != REL_STALE
            && foreground == FG_FOREGROUND
            && fallback_app_allowed(task);
        if (!proven) {
            if (!task->known || locked_early || locked_late
                || firmware->conflict || marker_status == MARKER_IO
                || marker_status == MARKER_INVALID
                || (marker_status == MARKER_VALID
                    && marker_process == PROCESS_UNKNOWN))
                return 0;
            return discard_reader_restore(runtime);
        }
        if (!marker->has_ticks) {
            int upgraded = upgrade_reader_marker(marker,
                                                  marker_actual_ticks);
            if (upgraded < 0)
                return -1;
            if (upgraded == 0)
                return 0;
        }
        if (!marker->has_ticks || marker->start_ticks != actual)
            return 0;
        runtime->reader.marker_started = marker->started_at;
        runtime->reader.start_ticks = marker->start_ticks;
        runtime->reader.modern = 1;
        if (!runtime->reader.start_ticks
            || write_reader_session(&runtime->reader) != 0)
            return -1;
    }
    if (begin_pending_handler(runtime, PENDING_PREPARE, marker,
                              marker_actual_ticks, &firmware->state, task,
                              present_seconds(runtime), boot, wall, 1) != 0)
        return -1;
    remember_firmware(runtime, &firmware->state);
    runtime->reader_state = RS_TRACKING;
    runtime->last_foreground = reader_foreground(task,
                                                 runtime->reader.pid);
    return 0;
}

static int recover_once(daemon_runtime *runtime,
                        const firmware_sample *firmware)
{
    if (runtime->recovered || firmware->result != 0)
        return 0;
    if (tracker_recover(&runtime->tracker, firmware->state.bookid,
                        firmware->state.opentime) < 0) {
        runtime->tracker.last_error = SQLITE_OK;
        return 0; /* optional metadata import: retry on a later full sample */
    }
    runtime->recovered = 1;
    return 0;
}

static int observe_or_checkpoint(daemon_runtime *runtime,
                                 const pb_state *firmware,
                                 int64_t boot, int64_t wall)
{
    if (firmware) {
        int observed = tracker_observe(&runtime->tracker, firmware,
                                       present_seconds(runtime), boot, wall);
        if (observed < 0)
            return -1;
        if (observed != 0)
            return 0;
    }
    if (tracker_checkpoint_due(&runtime->tracker, present_seconds(runtime)))
        return tracker_flush(&runtime->tracker, NULL,
                             present_seconds(runtime), boot, wall);
    return 0;
}

static void set_pending_context(fallback_key *context,
                                const active_task_info *task,
                                const pb_state *fw, const char *book)
{
    memset(context, 0, sizeof(*context));
    context->task = task->task;
    context->subtask = task->subtask;
    context->pid = task->pid;
    context->bookid = fw->bookid;
    context->opentime = fw->opentime;
    snprintf(context->app, sizeof(context->app), "%s", base_name(task->app));
    snprintf(context->book, sizeof(context->book), "%s", book ? book : "");
    context->valid = task->known && book && book[0]
        && process_state_for(task->pid, 0, 0, &context->start_ticks)
               == PROCESS_ALIVE;
}

static int live_marker_context(const reader_marker *marker,
                               const active_task_info *task,
                               fallback_key *context)
{
    char path[PATH_MAX] = "";
    pb_state unknown;
    memset(&unknown, 0, sizeof(unknown));
    if (!marker->has_ticks || !marker->has_book || !task->known
        || task->pid != marker->pid
        || !active_book_path(task, path, sizeof(path))
        || strcmp(path, marker->book))
        return 0;
    set_pending_context(context, task, &unknown, path);
    return context->valid && context->start_ticks == marker->start_ticks;
}

static int begin_pending_handler(daemon_runtime *runtime,
                                 enum pending_phase phase,
                                 const reader_marker *marker,
                                 uint64_t actual_ticks,
                                 const pb_state *fw,
                                 const active_task_info *task,
                                 int64_t base_present,
                                 int64_t start_boot, int64_t start_wall,
                                 int already_bound)
{
    memset(&runtime->pending, 0, sizeof(runtime->pending));
    runtime->pending.kind = PENDING_HANDLER;
    runtime->pending.phase = phase;
    runtime->pending.reader = already_bound
        ? runtime->reader : session_for_marker(marker, actual_ticks, fw);
    runtime->pending.first = runtime->pending.latest = *fw;
    runtime->pending.base_present = base_present;
    runtime->pending.start_boot = start_boot;
    runtime->pending.start_wall = start_wall;
    char task_book[PATH_MAX] = "";
    active_book_path(task, task_book, sizeof(task_book));
    const char *book = marker->has_book
        && marker_matches_session(marker, &runtime->pending.reader)
        ? marker->book
        : task->known && task->pid == runtime->pending.reader.pid
            ? task_book : "";
    set_pending_context(&runtime->pending.context, task, fw, book);
    if (!already_bound && phase != PENDING_CLOSE_OLD
        && adopt_saved_reader(runtime, &runtime->pending.reader, fw) != 0) {
        memset(&runtime->pending, 0, sizeof(runtime->pending));
        return -1;
    }
    return 0;
}

static void begin_pending_fallback(daemon_runtime *runtime,
                                   enum pending_phase phase,
                                   const fallback_key *context,
                                   const pb_state *fw,
                                   int64_t base_present,
                                   int64_t start_boot, int64_t start_wall)
{
    memset(&runtime->pending, 0, sizeof(runtime->pending));
    runtime->pending.kind = PENDING_FALLBACK;
    runtime->pending.phase = phase;
    runtime->pending.context = *context;
    runtime->pending.first = runtime->pending.latest = *fw;
    runtime->pending.base_present = base_present;
    runtime->pending.start_boot = start_boot;
    runtime->pending.start_wall = start_wall;
}

static int pending_task_matches(const pending_open *pending,
                                const active_task_info *task)
{
    if (!task->known)
        return 0;
    char path[PATH_MAX] = "";
    int has_path = active_book_path(task, path, sizeof(path));
    if (pending->kind == PENDING_FALLBACK)
        return fallback_task_matches(&pending->context, task);
    if (task->pid != pending->reader.pid)
        return 0;
    return !has_path || !pending->context.book[0]
        || !strcmp(path, pending->context.book);
}

static enum count_mode pending_count_mode(daemon_runtime *runtime,
                                          const firmware_sample *firmware,
                                          enum marker_status marker_status,
                                          const reader_marker *marker,
                                          enum process_state marker_process,
                                          const active_task_info *task,
                                          int locked_early, int locked_late)
{
    pending_open *pending = &runtime->pending;
    if (pending->kind == PENDING_NONE || locked_early || locked_late
        || !fallback_app_allowed(task) || !pending_task_matches(pending, task))
        return COUNT_NONE;
    if (pending->kind == PENDING_HANDLER) {
        if (marker_status == MARKER_INVALID || marker_status == MARKER_IO
            || (marker_status == MARKER_VALID
                && (!marker_matches_session(marker, &pending->reader)
                    || marker_process != PROCESS_ALIVE))
            || process_state_for(pending->reader.pid,
                                 pending->reader.start_ticks, 1, NULL)
                   != PROCESS_ALIVE)
            return COUNT_NONE;
    } else if (marker_status != MARKER_ABSENT) {
        return COUNT_NONE;
    }
    if (firmware->conflict)
        return COUNT_NONE;
    if (firmware->result == 0) {
        if (!same_firmware_session(&firmware->state,
                                   pending->first.bookid,
                                   pending->first.opentime))
            return COUNT_NONE;
        pending->latest = firmware->state;
        return COUNT_BOOT;
    }
    return COUNT_MONO;
}

static int bind_pending_target(daemon_runtime *runtime)
{
    if (runtime->pending.kind == PENDING_HANDLER)
        return adopt_saved_reader(runtime, &runtime->pending.reader,
                                  &runtime->pending.first);
    memset(&runtime->reader, 0, sizeof(runtime->reader));
    runtime->reader_state = RS_UNTRACKED;
    runtime->fallback = runtime->pending.context;
    runtime->fallback_ticks = 2;
    remember_firmware(runtime, &runtime->pending.first);
    return 0;
}

static int clear_old_for_pending(daemon_runtime *runtime)
{
    runtime->segment_open = 0;
    runtime->fallback_ticks = 0;
    runtime->replacement_book = runtime->replacement_open = 0;
    runtime->replacement_ticks = 0;
    runtime->needs_reconcile = 0;
    runtime->have_reconcile_state = 0;
    runtime->deferred_close_end = 0;
    if (unlink(READER_SESSION) != 0 && errno != ENOENT)
        return -1;
    memset(&runtime->reader, 0, sizeof(runtime->reader));
    runtime->reader_state = RS_UNTRACKED;
    return bind_pending_target(runtime);
}

static int drain_pending(daemon_runtime *runtime, enum count_mode mode,
                         int64_t boot, int64_t wall, int force_close)
{
    pending_open *pending = &runtime->pending;
    if (pending->kind == PENDING_NONE)
        return 0;
    if (pending->phase == PENDING_CLOSE_OLD) {
        const pb_state *old = pending->have_old_final
            ? &pending->old_final : NULL;
        int closed = pending->bounded_close
            ? close_segment_bounded(runtime, old, boot, wall,
                                    pending->close_wall)
            : close_segment(runtime, old, boot, wall);
        if (closed != 0)
            return -1;
        if (clear_old_for_pending(runtime) != 0)
            return -1;
        pending->phase = PENDING_PREPARE;
        pending->bounded_close = 0;
    }
    if (pending->phase == PENDING_PREPARE) {
        pb_state baseline = pending->first;
        /* The full snapshot is this interval's endpoint, not its start. */
        baseline.position_ts = baseline.opentime;
        if (prepare_segment_at(runtime, &baseline, pending->base_present,
                               pending->start_boot,
                               pending->start_wall) != 0)
            return -1;
        pending->phase = PENDING_OBSERVE;
        int64_t credit = pending->credit_ns;
        pending->credit_ns = 0;
        if (add_presence(runtime, credit) != 0)
            return -1;
    }
    if (observe_or_checkpoint(runtime, &pending->latest, boot, wall) != 0)
        return -1;
    if (force_close || mode == COUNT_NONE) {
        int closed = pending->target_bounded_close
            ? close_segment_bounded(runtime, &pending->latest, boot, wall,
                                    pending->target_close_wall)
            : close_segment(runtime, &pending->latest, boot, wall);
        if (closed != 0)
            return -1;
    }
    int keep_open = runtime->segment_open;
    memset(pending, 0, sizeof(*pending));
    runtime->previous_mode = keep_open && !force_close ? mode : COUNT_NONE;
    return 0;
}

static int daemon_tick(daemon_runtime *runtime)
{
    runtime->tracker.last_error = SQLITE_OK;
    runtime->have_attempted_clock = 0;
    runtime->have_attempted_mode = 0;
    int locked_early = device_locked();
    clock_sample clocks;
    if (sample_clocks(&clocks) != 0)
        return -1;
    runtime->attempted_clock = clocks;
    runtime->have_attempted_clock = 1;

    int64_t boot_delta = 0, mono_delta = 0;
    if (runtime->have_clock
        && clock_deltas(&runtime->previous_clock, &clocks,
                        &boot_delta, &mono_delta) != 0)
        return -1;

    if (load_restore_state(runtime) != 0)
        return -1;

    reader_marker marker;
    enum marker_status marker_status = read_reader_marker(&marker);
    uint64_t marker_actual_ticks = 0;
    enum process_state marker_process = marker_status == MARKER_VALID
        ? process_state_for(marker.pid, marker.start_ticks, marker.has_ticks,
                            &marker_actual_ticks)
        : PROCESS_UNKNOWN;
    active_task_info task = current_task();
    firmware_sample firmware;
    resolve_firmware(runtime, marker_status, &marker,
                     marker_process, &task, &firmware);
    int locked_late = device_locked();
    int64_t boot = clocks.boot_ns / NS_PER_SECOND;
    int64_t wall = clocks.wall_ns / NS_PER_SECOND;

    if (recover_once(runtime, &firmware) != 0
        || finish_restore(runtime, &firmware, marker_status, &marker,
                          marker_process, marker_actual_ticks, &task,
                          locked_early, locked_late, boot, wall) != 0)
        return -1;
    if (firmware.conflict) {
        runtime->conflict_latched = 1;
    } else if (runtime->conflict_latched) {
        runtime->conflict_latched = 0;
        runtime->previous_mode = COUNT_NONE;
        runtime->fallback_ticks = 0;
    }

    enum process_state reader_process = runtime->reader_state == RS_TRACKING
        ? process_state_for(runtime->reader.pid, runtime->reader.start_ticks,
                            runtime->reader.start_ticks != 0, NULL)
        : PROCESS_UNKNOWN;
    if (runtime->reader_state == RS_TRACKING
        && reader_process == PROCESS_ALIVE
        && marker_status == MARKER_VALID && marker_process == PROCESS_DEAD
        && !marker_matches_session(&marker, &runtime->reader)
        && firmware.result != 0 && !locked_early && !locked_late
        && reader_foreground(&task, runtime->reader.pid) == FG_FOREGROUND) {
        int discarded = discard_marker(&marker);
        if (discarded < 0)
            return -1;
        marker_status = discarded > 0 ? MARKER_ABSENT : MARKER_IO;
    }
    if (runtime->reader_state == RS_UNTRACKED && runtime->segment_open
        && runtime->fallback.valid
        && marker_status == MARKER_VALID && marker_process == PROCESS_DEAD
        && (marker.pid != runtime->fallback.pid || !marker.has_ticks
            || marker.start_ticks != runtime->fallback.start_ticks)
        && firmware.result != 0 && !locked_early && !locked_late
        && fallback_app_allowed(&task)
        && fallback_task_matches(&runtime->fallback, &task)) {
        int discarded = discard_marker(&marker);
        if (discarded < 0)
            return -1;
        marker_status = discarded > 0 ? MARKER_ABSENT : MARKER_IO;
    }
    int marker_foreground = marker_status == MARKER_VALID
        && reader_foreground(&task, marker.pid) == FG_FOREGROUND;
    enum marker_relation relation = firmware.result == 0
        ? marker_relation(&marker, &firmware.state) : REL_WAIT;
    int marker_matches_fw = marker_status == MARKER_VALID
        && firmware.result == 0
        && firmware.marker_book_matches
        && relation != REL_WAIT
        && (relation != REL_STALE
            || (marker_process == PROCESS_ALIVE && marker_foreground
                && firmware.task_trusted && !firmware.conflict));
    /* A dead marker cannot describe a different live task. Consume only the
     * exact marker snapshot; a concurrently published replacement waits for
     * the next full tick. A probe tracking this exact marker defers the
     * discard so the dead-probe path can flush its accumulated credit. */
    int probe_holds_marker = runtime->probe.valid
        && marker_status == MARKER_VALID
        && same_marker(&runtime->probe.marker, &marker);
    if (marker_status == MARKER_VALID && marker_process == PROCESS_DEAD
        && firmware.result == 0 && firmware.task_trusted
        && !probe_holds_marker
        && (task.pid != marker.pid
            || (marker.has_ticks
                && marker_actual_ticks != marker.start_ticks))) {
        int discarded = discard_marker(&marker);
        if (discarded < 0)
            return -1;
        marker_status = discarded > 0 ? MARKER_ABSENT : MARKER_IO;
        marker_matches_fw = 0;
    }
    int marker_context_valid = !firmware.conflict
        && !locked_early && !locked_late && marker_foreground
        && marker_matches_fw && fallback_app_allowed(&task);
    if (marker_status == MARKER_VALID && marker_process == PROCESS_ALIVE
        && !marker.has_ticks && marker_context_valid) {
        int upgraded = upgrade_reader_marker(&marker, marker_actual_ticks);
        if (upgraded < 0)
            return -1;
        if (upgraded == 0) {
            marker_status = MARKER_IO;
            marker_process = PROCESS_UNKNOWN;
        }
    }
    int adoption_candidate = !firmware.conflict && !locked_early && !locked_late
        && marker_process == PROCESS_ALIVE && marker_foreground
        && marker_matches_fw && marker.has_ticks
        && fallback_app_allowed(&task);

    fallback_key probe_context;
    memset(&probe_context, 0, sizeof(probe_context));
    int marker_already_bound = runtime->reader_state == RS_TRACKING
        && marker_status == MARKER_VALID
        && marker_matches_session(&marker, &runtime->reader);
    int probe_safe = !marker_already_bound && !firmware.conflict
        && !locked_early && !locked_late
        && marker_status == MARKER_VALID
        && marker_process == PROCESS_ALIVE && marker_foreground
        && fallback_app_allowed(&task)
        && live_marker_context(&marker, &task, &probe_context);
    int probe_same_marker = runtime->probe.valid
        && marker_status == MARKER_VALID
        && same_marker(&runtime->probe.marker, &marker);
    if (runtime->probe.valid
        && (marker_status == MARKER_ABSENT
            || (marker_status == MARKER_VALID && !probe_same_marker)))
        memset(&runtime->probe, 0, sizeof(runtime->probe));
    if (runtime->probe.valid) {
        enum count_mode probe_mode = probe_safe && probe_same_marker
            && same_fallback(&runtime->probe.context, &probe_context)
            ? COUNT_MONO : COUNT_NONE;
        int64_t credit = runtime->have_clock
            ? interval_credit(runtime->probe.previous_mode, probe_mode,
                              boot_delta, mono_delta) : 0;
        if (runtime->probe.credit_ns > INT64_MAX - credit)
            return -1;
        runtime->probe.credit_ns += credit;
        runtime->probe.previous_mode = probe_mode;
    }
    if (!runtime->probe.valid && probe_safe && !adoption_candidate
        && runtime->pending.kind == PENDING_NONE) {
        runtime->probe.valid = 1;
        runtime->probe.marker = marker;
        runtime->probe.context = probe_context;
        runtime->probe.previous_mode = COUNT_MONO;
        runtime->probe.start_boot = boot;
        runtime->probe.start_wall = wall;
    }
    probe_same_marker = runtime->probe.valid
        && marker_status == MARKER_VALID
        && same_marker(&runtime->probe.marker, &marker);
    int probe_matches = probe_same_marker;

    if (runtime->pending.kind != PENDING_NONE) {
        if (firmware.result == 0) {
            int same_target = same_firmware_session(
                &firmware.state, runtime->pending.first.bookid,
                runtime->pending.first.opentime);
            runtime->pending.target_bounded_close = !same_target;
            runtime->pending.target_close_wall = same_target
                ? 0 : firmware.state.opentime - 1;
        }
        if (probe_matches
            && marker_matches_session(&marker, &runtime->pending.reader)) {
            if (runtime->pending.phase == PENDING_PREPARE) {
                runtime->pending.start_boot = runtime->probe.start_boot;
                runtime->pending.start_wall = runtime->probe.start_wall;
            }
            if (transfer_probe_credit(runtime, 1) != 0)
                return -1;
        }
        enum count_mode pending_mode = pending_count_mode(
            runtime, &firmware, marker_status, &marker, marker_process,
            &task, locked_early, locked_late);
        if (runtime->have_clock && runtime->previous_mode != COUNT_NONE) {
            int64_t credit = interval_credit(runtime->previous_mode,
                                             pending_mode,
                                             boot_delta, mono_delta);
            int failed = runtime->pending.phase == PENDING_OBSERVE
                ? add_presence(runtime, credit)
                : add_pending_credit(runtime, credit);
            if (failed != 0)
                return -1;
        }
        runtime->attempted_mode = pending_mode;
        runtime->have_attempted_mode = 1;
        if (drain_pending(runtime, pending_mode, boot, wall, 0) != 0)
            return -1;
        goto finish_tick;
    }
    int dead_probe_target = probe_matches
        && marker_process == PROCESS_DEAD && marker_matches_fw
        && firmware.result == 0 && !firmware.conflict;
    if (runtime->probe.valid && !adoption_candidate && !dead_probe_target) {
        runtime->previous_mode = COUNT_NONE;
        goto finish_tick;
    }

    if (runtime->reader_state == RS_UNTRACKED
        && marker_status == MARKER_VALID && marker_process == PROCESS_DEAD
        && marker_matches_fw && !firmware.conflict) {
        if (runtime->have_clock && runtime->previous_mode != COUNT_NONE
            && add_presence(runtime, mono_delta) != 0)
            return -1;
        runtime->previous_mode = COUNT_NONE;
        int same_segment = runtime->segment_open
            && runtime->tracker.cur_book == firmware.state.bookid
            && runtime->tracker.cur_open == firmware.state.opentime;
        if (runtime->segment_open && !same_segment
            && close_segment(runtime,
                runtime->have_last_firmware
                    && runtime->last_firmware.bookid == runtime->tracker.cur_book
                    && runtime->last_firmware.opentime == runtime->tracker.cur_open
                    ? &runtime->last_firmware : NULL,
                boot, wall) != 0)
            return -1;
        if (!same_segment
            && tracker_prepare(&runtime->tracker, &firmware.state,
                               present_seconds(runtime),
                               probe_matches
                                   ? runtime->probe.start_boot : boot,
                               probe_matches
                                   ? runtime->probe.start_wall : wall) < 0)
            return -1;
        if (probe_matches && transfer_probe_credit(runtime, 0) != 0)
            return -1;
        remember_firmware(runtime, &firmware.state);
        runtime->reader = session_for_marker(&marker, marker.start_ticks,
                                             &firmware.state);
        runtime->reader_state = RS_TRACKING;
        int frozen = freeze_reader(runtime, marker_status, &marker,
                                   &firmware.state, boot, wall);
        if (frozen < 0)
            return -1;
        runtime->previous_mode = COUNT_NONE;
        goto finish_tick;
    }

    if (marker_status == MARKER_VALID && marker_process == PROCESS_DEAD
        && firmware.result == 0
        && (relation == REL_STALE || relation == REL_WAIT)) {
        int discarded = discard_marker(&marker);
        if (discarded < 0)
            return -1;
        if (discarded > 0)
            marker_status = MARKER_ABSENT;
    }

    int bound = runtime->reader_state == RS_TRACKING
        || runtime->reader_state == RS_FROZEN;
    int session_changed = bound && firmware.result == 0
        && !same_firmware_session(&firmware.state, runtime->reader.bookid,
                                  runtime->reader.opentime);
    int replacement = adoption_candidate && runtime->reader_state == RS_TRACKING
        && !marker_matches_session(&marker, &runtime->reader);
    int fallback_switch = runtime->reader_state == RS_UNTRACKED
        && runtime->segment_open && adoption_candidate
        && (runtime->tracker.cur_book != firmware.state.bookid
            || runtime->tracker.cur_open != firmware.state.opentime);
    int pending_session_change = 0;
    if (session_changed && !adoption_candidate) {
        if (runtime->replacement_book == firmware.state.bookid
            && runtime->replacement_open == firmware.state.opentime) {
            runtime->replacement_ticks++;
        } else {
            runtime->replacement_book = firmware.state.bookid;
            runtime->replacement_open = firmware.state.opentime;
            runtime->replacement_ticks = 1;
        }
        if (runtime->replacement_ticks < 2) {
            pending_session_change = 1;
            runtime->deferred_close_end = firmware.state.opentime - 1;
            session_changed = 0;
        }
    } else if (!session_changed && firmware.result == 0 && bound
               && same_firmware_session(&firmware.state,
                                        runtime->reader.bookid,
                                        runtime->reader.opentime)) {
        runtime->replacement_book = runtime->replacement_open = 0;
        runtime->replacement_ticks = 0;
        runtime->deferred_close_end = 0;
    }

    /* A new, confirmed reader wins over a stale cached death. */
    int different_session = session_changed || fallback_switch;
    if ((different_session || replacement)
        && (adoption_candidate
            || (session_changed && marker_status == MARKER_ABSENT))) {
        pb_state final_old;
        const pb_state *old_state = runtime->have_last_firmware
            && same_firmware_session(&runtime->last_firmware,
                                     runtime->tracker.cur_book,
                                     runtime->tracker.cur_open)
            ? &runtime->last_firmware : NULL;
        int old_read = tracker_cached_read_book_state(
            &runtime->tracker, runtime->tracker.cur_book,
            runtime->tracker.cur_open, 0, &final_old);
        if (old_read == 0)
            old_state = &final_old;

        fallback_key switch_fallback;
        memset(&switch_fallback, 0, sizeof(switch_fallback));
        char switch_path[PATH_MAX] = "";
        if (!adoption_candidate && session_changed
            && marker_status == MARKER_ABSENT && firmware.trusted
            && !locked_early && !locked_late && fallback_app_allowed(&task)
            && active_book_path(&task, switch_path, sizeof(switch_path)))
            set_pending_context(&switch_fallback, &task, &firmware.state,
                                switch_path);
        enum count_mode candidate_mode = adoption_candidate
            || switch_fallback.valid ? COUNT_BOOT : COUNT_NONE;
        int64_t before_ns = 0, after_ns = 0;
        if (runtime->have_clock) {
            if (different_session) {
                int old_continuous = runtime->reader_state == RS_TRACKING
                    && reader_process == PROCESS_ALIVE
                    && !locked_early && !locked_late
                    && reader_foreground(&task, runtime->reader.pid)
                           == FG_FOREGROUND;
                split_switch_interval(runtime->previous_mode, candidate_mode,
                                      old_continuous,
                                      &runtime->previous_clock, &clocks,
                                      firmware.state.opentime, boot_delta,
                                      mono_delta, &before_ns, &after_ns);
            } else if (replacement && runtime->previous_mode != COUNT_NONE) {
                before_ns = mono_delta;
            }
        }
        if (add_presence(runtime, before_ns) != 0)
            return -1;
        if (adoption_candidate || switch_fallback.valid) {
            int64_t segment_boot = boot;
            int64_t segment_wall = wall;
            if (after_ns > 0) {
                segment_boot = (clocks.boot_ns - after_ns) / NS_PER_SECOND;
                segment_wall = firmware.state.opentime;
            }
            if (probe_matches) {
                segment_boot = runtime->probe.start_boot;
                segment_wall = runtime->probe.start_wall;
            }
            int pending_error = 0;
            if (adoption_candidate)
                pending_error = begin_pending_handler(
                    runtime, PENDING_CLOSE_OLD, &marker, marker_actual_ticks,
                    &firmware.state, &task, present_seconds(runtime),
                    segment_boot, segment_wall, 0);
            else
                begin_pending_fallback(runtime, PENDING_CLOSE_OLD,
                                       &switch_fallback, &firmware.state,
                                       present_seconds(runtime),
                                       segment_boot, segment_wall);
            if (pending_error != 0
                || add_pending_credit(runtime, after_ns) != 0
                || (probe_matches
                    && transfer_probe_credit(runtime, 1) != 0))
                return -1;
            runtime->pending.close_wall = different_session
                ? firmware.state.opentime - 1 : wall;
            runtime->pending.bounded_close = different_session;
            runtime->pending.have_old_final = old_state != NULL;
            if (old_state)
                runtime->pending.old_final = *old_state;
            runtime->attempted_mode = COUNT_BOOT;
            runtime->have_attempted_mode = 1;
            if (drain_pending(runtime, COUNT_BOOT, boot, wall, 0) != 0)
                return -1;
            goto finish_tick;
        }

        int closed = different_session
            ? close_segment_bounded(runtime, old_state, boot, wall,
                                    firmware.state.opentime - 1)
            : close_segment(runtime, old_state, boot, wall);
        if (closed != 0)
            return -1;
        runtime->fallback_ticks = 0;
        runtime->replacement_book = runtime->replacement_open = 0;
        runtime->replacement_ticks = 0;
        runtime->deferred_close_end = 0;
        runtime->needs_reconcile = 0;
        runtime->have_reconcile_state = 0;
        if (unlink(READER_SESSION) != 0 && errno != ENOENT)
            return -1;
        memset(&runtime->reader, 0, sizeof(runtime->reader));
        runtime->reader_state = RS_UNTRACKED;
        runtime->previous_mode = COUNT_NONE;
        goto finish_tick;
    }

    if (runtime->reader_state == RS_TRACKING
        && reader_process == PROCESS_DEAD && !adoption_candidate
        && !pending_session_change && runtime->deferred_close_end == 0) {
        int64_t tail = runtime->previous_mode != COUNT_NONE ? mono_delta : 0;
        if (add_presence(runtime, tail) != 0)
            return -1;
        runtime->previous_mode = COUNT_NONE;
        pb_state final_state;
        const pb_state *final = NULL;
        int final_result = tracker_cached_read_book_state(
            &runtime->tracker, runtime->reader.bookid,
            runtime->reader.opentime, 0, &final_state);
        if (final_result < 0)
            goto finish_tick;
        if (final_result == 0)
            final = &final_state;
        else if (runtime->have_last_firmware)
            final = &runtime->last_firmware;
        int frozen = freeze_reader(runtime, marker_status, &marker, final,
                                   boot, wall);
        if (frozen < 0)
            return -1;
        if (frozen == 0) {
            runtime->previous_mode = COUNT_NONE;
            if (final_result != 0) {
                runtime->reconcile_ticks = 0;
                runtime->have_reconcile_state = 0;
            }
        }
        goto finish_tick;
    }

    if (runtime->reader_state == RS_FROZEN) {
        if (marker_status == MARKER_VALID
            && marker_process == PROCESS_DEAD && marker_matches_fw) {
            int discarded = discard_marker(&marker);
            if (discarded < 0)
                return -1;
            if (discarded > 0)
                marker_status = MARKER_ABSENT;
        }
        if (reconcile_frozen(runtime, &firmware, marker_status,
                             marker_process, boot, wall) != 0)
            return -1;
        if (adoption_candidate
            && (!same_firmware_session(&firmware.state,
                                       runtime->reader.bookid,
                                       runtime->reader.opentime)
                || marker.started_at >= runtime->reader.opentime)) {
            if (begin_pending_handler(runtime, PENDING_PREPARE,
                                      &marker, marker_actual_ticks,
                                      &firmware.state, &task,
                                      present_seconds(runtime),
                                      probe_matches
                                          ? runtime->probe.start_boot : boot,
                                      probe_matches
                                          ? runtime->probe.start_wall : wall,
                                      0) != 0)
                return -1;
            if (probe_matches && transfer_probe_credit(runtime, 1) != 0)
                return -1;
            runtime->attempted_mode = COUNT_BOOT;
            runtime->have_attempted_mode = 1;
            if (drain_pending(runtime, COUNT_BOOT, boot, wall, 0) != 0)
                return -1;
        } else {
            runtime->previous_mode = COUNT_NONE;
        }
        goto finish_tick;
    }

    if (runtime->reader_state == RS_UNTRACKED && adoption_candidate) {
        if (runtime->segment_open
            && runtime->tracker.cur_book == firmware.state.bookid
            && runtime->tracker.cur_open == firmware.state.opentime) {
            if (adopt_reader(runtime, &marker, marker_actual_ticks,
                             &firmware.state) != 0)
                return -1;
            if (runtime->have_clock && runtime->previous_mode != COUNT_NONE
                && add_presence(runtime,
                    interval_credit(runtime->previous_mode, COUNT_BOOT,
                                    boot_delta, mono_delta)) != 0)
                return -1;
            if (probe_matches && transfer_probe_credit(runtime, 0) != 0)
                return -1;
            remember_firmware(runtime, &firmware.state);
            runtime->attempted_mode = COUNT_BOOT;
            runtime->have_attempted_mode = 1;
            if (observe_or_checkpoint(runtime, &firmware.state,
                                      boot, wall) != 0)
                return -1;
            runtime->previous_mode = COUNT_BOOT;
            goto finish_tick;
        }
        if (begin_pending_handler(runtime, PENDING_PREPARE,
                                  &marker, marker_actual_ticks,
                                  &firmware.state, &task,
                                  present_seconds(runtime),
                                  probe_matches
                                      ? runtime->probe.start_boot : boot,
                                  probe_matches
                                      ? runtime->probe.start_wall : wall,
                                  0) != 0)
            return -1;
        if (probe_matches && transfer_probe_credit(runtime, 1) != 0)
            return -1;
        runtime->attempted_mode = COUNT_BOOT;
        runtime->have_attempted_mode = 1;
        if (drain_pending(runtime, COUNT_BOOT, boot, wall, 0) != 0)
            return -1;
        goto finish_tick;
    }

    enum count_mode current_mode = reader_count_mode(
        runtime, &firmware, reader_process, &task,
        locked_early, locked_late);
    if (runtime->reader_state == RS_TRACKING
        && marker_status == MARKER_VALID
        && !marker_matches_session(&marker, &runtime->reader))
        current_mode = COUNT_NONE;
    if (marker_status == MARKER_INVALID || marker_status == MARKER_IO)
        current_mode = COUNT_NONE;

    if (runtime->reader_state == RS_UNTRACKED) {
        char fallback_path[PATH_MAX] = "";
        int have_fallback_path = active_book_path(&task, fallback_path,
                                                  sizeof(fallback_path));
        int fallback_gap = runtime->fallback.valid
            && firmware.result != 0 && marker_status == MARKER_ABSENT
            && !locked_early && !locked_late && fallback_app_allowed(&task)
            && fallback_task_matches(&runtime->fallback, &task)
            && runtime->tracker.cur_book == runtime->fallback.bookid
            && runtime->tracker.cur_open == runtime->fallback.opentime;
        if (fallback_gap) {
            current_mode = COUNT_MONO;
        } else {
        fallback_key key;
        memset(&key, 0, sizeof(key));
        if (!firmware.conflict && marker_status == MARKER_ABSENT
            && firmware.result == 0 && firmware.trusted
            && !locked_early && !locked_late
            && fallback_app_allowed(&task) && have_fallback_path
            && process_state_for(task.pid, 0, 0, &key.start_ticks)
                   == PROCESS_ALIVE) {
            key.valid = 1;
            key.task = task.task;
            key.subtask = task.subtask;
            key.pid = task.pid;
            key.bookid = firmware.state.bookid;
            key.opentime = firmware.state.opentime;
            snprintf(key.app, sizeof(key.app), "%s", base_name(task.app));
            snprintf(key.book, sizeof(key.book), "%s", fallback_path);
        }
        if (key.valid) {
            if (same_fallback(&runtime->fallback, &key))
                runtime->fallback_ticks++;
            else {
                runtime->fallback = key;
                runtime->fallback_ticks = 1;
            }
            if (runtime->segment_open
                && (runtime->tracker.cur_book != key.bookid
                    || runtime->tracker.cur_open != key.opentime))
                runtime->deferred_close_end = key.opentime - 1;
            else if (runtime->tracker.cur_book == key.bookid
                     && runtime->tracker.cur_open == key.opentime)
                runtime->deferred_close_end = 0;
        } else {
            runtime->fallback_ticks = 0;
        }
        current_mode = runtime->fallback_ticks >= 2 ? COUNT_BOOT : COUNT_NONE;
        if (current_mode != COUNT_NONE
            && (!runtime->segment_open
                || runtime->tracker.cur_book != firmware.state.bookid
                || runtime->tracker.cur_open != firmware.state.opentime)) {
            int switching = runtime->segment_open
                && (runtime->tracker.cur_book != firmware.state.bookid
                    || runtime->tracker.cur_open != firmware.state.opentime);
            int64_t before_ns = 0, after_ns = 0;
            int64_t start_boot = boot, start_wall = wall;
            if (switching && runtime->have_clock) {
                split_switch_interval(runtime->previous_mode, current_mode, 0,
                                      &runtime->previous_clock, &clocks,
                                      firmware.state.opentime, boot_delta,
                                      mono_delta, &before_ns, &after_ns);
                if (after_ns > 0) {
                    start_boot = (clocks.boot_ns - after_ns) / NS_PER_SECOND;
                    start_wall = firmware.state.opentime;
                }
            }
            if (add_presence(runtime, before_ns) != 0)
                return -1;
            begin_pending_fallback(runtime,
                                   switching ? PENDING_CLOSE_OLD
                                             : PENDING_PREPARE,
                                   &key, &firmware.state,
                                   present_seconds(runtime),
                                   start_boot, start_wall);
            if (add_pending_credit(runtime, after_ns) != 0)
                return -1;
            if (switching) {
                runtime->pending.close_wall = firmware.state.opentime - 1;
                runtime->pending.bounded_close = 1;
                runtime->pending.have_old_final = runtime->have_last_firmware
                    && same_firmware_session(&runtime->last_firmware,
                                             runtime->tracker.cur_book,
                                             runtime->tracker.cur_open);
                if (runtime->pending.have_old_final)
                    runtime->pending.old_final = runtime->last_firmware;
            }
            runtime->attempted_mode = current_mode;
            runtime->have_attempted_mode = 1;
            if (drain_pending(runtime, current_mode, boot, wall, 0) != 0)
                return -1;
            goto finish_tick;
        }
        }
    }

    if (runtime->conflict_latched)
        current_mode = COUNT_NONE;

    if (runtime->deferred_close_end > 0) {
        runtime->previous_mode = COUNT_NONE;
        goto finish_tick;
    }

    int64_t interval_ns = runtime->have_clock
        && runtime->previous_mode != COUNT_NONE
        ? interval_credit(runtime->previous_mode, current_mode,
                          boot_delta, mono_delta) : 0;
    if (current_mode != COUNT_NONE && !runtime->segment_open) {
        const pb_state *resume = NULL;
        if (firmware.result == 0
            && runtime->tracker.cur_book == firmware.state.bookid
            && runtime->tracker.cur_open == firmware.state.opentime) {
            resume = &firmware.state;
        } else if (firmware.result != 0 && runtime->have_last_firmware
                   && runtime->tracker.cur_book
                          == runtime->last_firmware.bookid
                   && runtime->tracker.cur_open
                          == runtime->last_firmware.opentime) {
            resume = &runtime->last_firmware;
        }
        if (!resume || !task.known) {
            runtime->previous_mode = COUNT_NONE;
            goto finish_tick;
        }
        if (runtime->reader_state == RS_TRACKING) {
            if (begin_pending_handler(runtime, PENDING_PREPARE,
                                      &marker, marker_actual_ticks,
                                      resume, &task,
                                      present_seconds(runtime),
                                      boot, wall, 1) != 0)
                return -1;
        } else {
            begin_pending_fallback(runtime, PENDING_PREPARE,
                                   &runtime->fallback, resume,
                                   present_seconds(runtime), boot, wall);
        }
        if (add_pending_credit(runtime, interval_ns) != 0)
            return -1;
        runtime->attempted_mode = current_mode;
        runtime->have_attempted_mode = 1;
        if (drain_pending(runtime, current_mode, boot, wall, 0) != 0)
            return -1;
        goto finish_tick;
    }
    if (add_presence(runtime, interval_ns) != 0)
        return -1;
    if (pending_session_change) {
        runtime->previous_mode = COUNT_NONE;
        goto finish_tick;
    }

    if (current_mode == COUNT_NONE && runtime->segment_open) {
        const pb_state *final = firmware.result == 0
            && runtime->tracker.cur_book == firmware.state.bookid
            && runtime->tracker.cur_open == firmware.state.opentime
            ? &firmware.state
            : runtime->have_last_firmware ? &runtime->last_firmware : NULL;
        if (close_segment(runtime, final, boot, wall) != 0)
            return -1;
    }

    const pb_state *snapshot = NULL;
    if (firmware.result == 0
        && runtime->tracker.cur_book == firmware.state.bookid
        && runtime->tracker.cur_open == firmware.state.opentime) {
        remember_firmware(runtime, &firmware.state);
        snapshot = &firmware.state;
    }
    if (runtime->segment_open) {
        runtime->attempted_mode = current_mode;
        runtime->have_attempted_mode = 1;
        if (observe_or_checkpoint(runtime, snapshot, boot, wall) != 0)
            return -1;
    }
    runtime->previous_mode = current_mode;

finish_tick:
    runtime->previous_clock = clocks;
    runtime->have_clock = 1;
    return 0;
}

static int run_tick(daemon_runtime *runtime)
{
    int result = daemon_tick(runtime);
    if (result == 0 || !runtime->have_attempted_clock
        || !tracker_error_retryable(&runtime->tracker))
        return result;
    /* Tracker mutators keep their in-memory state atomic. Presence already
     * credited by this tick remains pending; rebaseline to avoid crediting the
     * failed interval twice and retry persistence on the next poll. */
    runtime->previous_clock = runtime->attempted_clock;
    runtime->have_clock = 1;
    runtime->previous_mode = runtime->have_attempted_mode
        ? runtime->attempted_mode : COUNT_NONE;
    return 0;
}

/* Shutdown deliberately does not adopt, rebind or mutate marker state. It
 * samples every gate once, credits only the already-open fragment, then makes
 * that fragment durable in one tracker call. A successor daemon reconciles a
 * concurrent reader lifecycle from the untouched marker/session files. */
static int daemon_terminal_tick(daemon_runtime *runtime)
{
    runtime->tracker.last_error = SQLITE_OK;
    runtime->have_attempted_mode = 0;
    int locked_early = device_locked();
    clock_sample clocks;
    if (sample_clocks(&clocks) != 0)
        return -1;
    runtime->attempted_clock = clocks;
    runtime->have_attempted_clock = 1;
    int64_t boot_delta = 0, mono_delta = 0;
    if (runtime->have_clock
        && clock_deltas(&runtime->previous_clock, &clocks,
                        &boot_delta, &mono_delta) != 0)
        return -1;

    active_task_info task = current_task();
    int locked_late = device_locked();
    if (runtime->pending.kind != PENDING_NONE) {
        reader_marker marker;
        enum marker_status marker_status = read_reader_marker(&marker);
        uint64_t actual_ticks = 0;
        enum process_state marker_process = marker_status == MARKER_VALID
            ? process_state_for(marker.pid, marker.start_ticks,
                                marker.has_ticks, &actual_ticks)
            : PROCESS_UNKNOWN;
        firmware_sample firmware;
        resolve_firmware(runtime, marker_status, &marker,
                         marker_process, &task, &firmware);
        enum count_mode mode = pending_count_mode(
            runtime, &firmware, marker_status, &marker, marker_process,
            &task, locked_early, locked_late);
        if (runtime->have_clock && runtime->previous_mode != COUNT_NONE) {
            int64_t credit = interval_credit(runtime->previous_mode, mode,
                                             boot_delta, mono_delta);
            int failed = runtime->pending.phase == PENDING_OBSERVE
                ? add_presence(runtime, credit)
                : add_pending_credit(runtime, credit);
            if (failed != 0)
                return -1;
        }
        runtime->attempted_mode = mode;
        runtime->have_attempted_mode = 1;
        return drain_pending(runtime, mode,
                             clocks.boot_ns / NS_PER_SECOND,
                             clocks.wall_ns / NS_PER_SECOND, 1);
    }
    pb_state final_state;
    const pb_state *final = NULL;
    int64_t final_book = runtime->tracker.cur_book;
    int64_t final_open = runtime->tracker.cur_open;
    int final_result = final_book > 0
        ? tracker_cached_read_book_state(&runtime->tracker,
                                         final_book, final_open, 0,
                                         &final_state)
        : 1;
    if (final_result == 0)
        final = &final_state;
    else if (runtime->have_last_firmware
             && runtime->last_firmware.bookid == final_book
             && runtime->last_firmware.opentime == final_open)
        final = &runtime->last_firmware;

    enum count_mode current = COUNT_NONE;
    if (runtime->have_clock && runtime->previous_mode != COUNT_NONE) {
        int endpoint_safe = !locked_early && !locked_late;
        if (runtime->reader_state == RS_TRACKING) {
            reader_marker marker;
            enum marker_status marker_status = read_reader_marker(&marker);
            enum foreground_state foreground = reader_foreground(
                &task, runtime->reader.pid);
            if (foreground != FG_UNKNOWN)
                runtime->last_foreground = foreground;
            char task_path[PATH_MAX] = "";
            int64_t task_book = 0;
            int book_matches = active_book_path(&task, task_path,
                                                sizeof(task_path))
                && tracker_cached_book_id_for_path(&runtime->tracker,
                                                   task_path,
                                                   &task_book) == 0
                && task_book == final_book;
            endpoint_safe = endpoint_safe
                && marker_status == MARKER_VALID
                && marker_matches_session(&marker, &runtime->reader)
                && book_matches
                && process_state_for(runtime->reader.pid,
                                     runtime->reader.start_ticks,
                                     runtime->reader.start_ticks != 0,
                                     NULL) == PROCESS_ALIVE
                && foreground != FG_BACKGROUND
                && (foreground != FG_UNKNOWN
                    || runtime->last_foreground == FG_FOREGROUND);
        } else {
            int64_t task_book = 0;
            int path_ok = fallback_task_matches(&runtime->fallback, &task)
                && tracker_cached_book_id_for_path(&runtime->tracker,
                                                   runtime->fallback.book,
                                                   &task_book) == 0;
            endpoint_safe = endpoint_safe && fallback_app_allowed(&task)
                && path_ok
                && runtime->fallback.bookid == runtime->tracker.cur_book
                && runtime->fallback.opentime == runtime->tracker.cur_open
                && task_book == runtime->tracker.cur_book;
        }
        current = endpoint_safe && final_result == 0
            ? (runtime->reader_state == RS_TRACKING
               && reader_foreground(&task, runtime->reader.pid) == FG_UNKNOWN
                   ? COUNT_MONO : runtime->previous_mode)
            : COUNT_NONE;
        runtime->attempted_mode = current;
        runtime->have_attempted_mode = 1;
        int64_t credit = interval_credit(runtime->previous_mode, current,
                                         boot_delta, mono_delta);
        if (add_presence(runtime, credit) != 0)
            return -1;
    }
    if (!runtime->segment_open)
        return 0;
    int64_t boot_s = clocks.boot_ns / NS_PER_SECOND;
    int64_t wall_s = clocks.wall_ns / NS_PER_SECOND;
    if (runtime->deferred_close_end > 0)
        return tracker_flush_bounded(&runtime->tracker, final,
                                     present_seconds(runtime),
                                     boot_s, wall_s,
                                     runtime->deferred_close_end);
    return tracker_flush(&runtime->tracker, final,
                         present_seconds(runtime), boot_s, wall_s);
}

int run_daemon(void)
{
    setsid();
#ifdef HAVE_DEVICE_STATE
    InitInkview(TASK_NOHANDLER);
#endif
    mkdir(STATS_DIR, 0755);
    int lockfd = -1;
    int claimed = daemon_claim(&lockfd);
    if (claimed <= 0)
        return claimed < 0 ? 1 : 0;

    /* Before the singleton is claimed there is no tracker state to flush, so
     * the default immediate SIGTERM is the correct cancellation behaviour. */
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_term;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0
        || sigaction(SIGINT, &action, NULL) != 0) {
        daemon_release(lockfd);
        return 1;
    }

    daemon_runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.reader_state = RS_UNTRACKED;
    runtime.last_foreground = FG_UNKNOWN;
    int initialized;
    while ((initialized = tracker_init(&runtime.tracker, stats_db_path(),
                                       explorer_db_path())) == TRACKER_RETRY
           && running) {
        struct timespec retry = {1, 0};
        while (running && nanosleep(&retry, &retry) != 0 && errno == EINTR)
            ;
    }
    if (initialized != 0) {
        daemon_release(lockfd);
        return running ? 1 : 0;
    }
    if (!running) {
        tracker_close(&runtime.tracker);
        daemon_release(lockfd);
        return 0;
    }

    int status = run_tick(&runtime) != 0;
    for (;;) {
        while (!status && running) {
            if (wait_for_tick() != 0) {
                status = 1;
                break;
            }
            if (running && run_tick(&runtime) != 0)
                status = 1;
        }
        if (status)
            break;
        struct timespec started;
        if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
            status = 1;
            break;
        }
        int flushed = 0;
        while (!status && !flushed) {
            if (daemon_terminal_tick(&runtime) == 0) {
                flushed = 1;
                break;
            }
            if (!runtime.have_attempted_clock
                || !tracker_error_retryable(&runtime.tracker)) {
                status = 1;
                break;
            }
            runtime.previous_clock = runtime.attempted_clock;
            runtime.have_clock = 1;
            runtime.previous_mode = runtime.have_attempted_mode
                ? runtime.attempted_mode : COUNT_NONE;
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                status = 1;
                break;
            }
            int64_t elapsed_ms = (timespec_ns(&now) - timespec_ns(&started))
                / 1000000;
            if (elapsed_ms >= DAEMON_TERMINAL_RETRY_MS)
                break;
            struct timespec retry = {0, 100 * 1000 * 1000};
            while (nanosleep(&retry, &retry) != 0 && errno == EINTR)
                ;
        }
        if (status || flushed)
            break;
        /* Persistence stayed blocked: do not acknowledge a stop that would
         * lose RAM-only time. The caller times out and the daemon keeps going. */
        running = 1;
    }

    tracker_close(&runtime.tracker);
    daemon_release(lockfd);
    return status;
}
