#define _GNU_SOURCE
#include "daemon.h"
#include "tracker.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#ifdef CLOCK_BOOTTIME
#define TRACK_CLOCK CLOCK_BOOTTIME
#else
#define TRACK_CLOCK CLOCK_MONOTONIC
#endif

static volatile sig_atomic_t running = 1;

static void on_term(int sig)
{
    (void)sig;
    running = 0;
}

static int daemon_pid_matches(int pid)
{
    char path[64], cmdline[512];
    snprintf(path, sizeof(path), PROC_ROOT "/%d/cmdline", pid);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(cmdline, 1, sizeof(cmdline), f);
    fclose(f);
    char *arg1 = memchr(cmdline, '\0', n);
    static const char expected[] = "--daemon";
    size_t remaining = arg1 ? n - (size_t)(arg1 + 1 - cmdline) : 0;
    return remaining >= sizeof(expected)
        && memcmp(arg1 + 1, expected, sizeof(expected)) == 0;
}

static int read_pid_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[32];
    int pid = 0;
    if (fgets(line, sizeof(line), f)) {
        char *end = NULL;
        errno = 0;
        long value = strtol(line, &end, 10);
        if (!errno && value > 0 && value <= 2147483647L
            && end != line && (*end == '\n' || *end == '\0'))
            pid = (int)value;
    }
    fclose(f);
    return pid;
}

static int pid_exists(int pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static int active_daemon_pid(void)
{
    int pid = read_pid_file(PIDFILE);
    if (pid == 0)
        return 0;
    if (pid == (int)getpid()) {
        unlink(PIDFILE);
        return 0;
    }
    if (pid_exists(pid) && daemon_pid_matches(pid))
        return pid;
    unlink(PIDFILE);
    return 0;
}

int stop_daemon(void)
{
    int pid = active_daemon_pid();
    if (pid <= 0)
        return 0;
    if (kill(pid, SIGTERM) != 0)
        return active_daemon_pid() ? -1 : 0;
    const struct timespec slice = {0, 100 * 1000 * 1000};
    for (int i = 0; i < 50; ++i) {
        if (active_daemon_pid() != pid)
            return 0;
        nanosleep(&slice, NULL);
    }
    return active_daemon_pid() == pid ? -1 : 0;
}

static int write_pidfile(void)
{
    FILE *f = fopen(PIDFILE, "w");
    if (!f)
        return -1;
    int ok = fprintf(f, "%d\n", (int)getpid()) > 0;
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

enum marker_status { MARKER_ABSENT, MARKER_VALID, MARKER_INVALID, MARKER_IO };
typedef struct { int pid; int64_t started_at; } reader_marker;

static enum marker_status read_reader_marker(reader_marker *out)
{
    FILE *f = fopen(READER_PIDFILE, "r");
    if (!f)
        return errno == ENOENT ? MARKER_ABSENT : MARKER_IO;
    char line[96];
    if (!fgets(line, sizeof(line), f)) {
        int io = ferror(f);
        fclose(f);
        return io ? MARKER_IO : MARKER_INVALID;
    }
    int close_error = fclose(f) != 0;
    int pid = 0, used = 0, tail = 0;
    long long started = 0;
    int fields = sscanf(line, " %d %lld %n", &pid, &started, &used);
    if (fields == 1)
        used = 0, fields = sscanf(line, " %d %n", &pid, &used);
    while (line[used] == ' ' || line[used] == '\t'
           || line[used] == '\r' || line[used] == '\n')
        used++;
    tail = line[used] != '\0';
    if (close_error)
        return MARKER_IO;
    if ((fields != 1 && fields != 2) || pid <= 0 || started < 0 || tail)
        return MARKER_INVALID;
    out->pid = pid;
    out->started_at = fields == 2 ? (int64_t)started : 0;
    return MARKER_VALID;
}

static int same_marker(const reader_marker *a, const reader_marker *b)
{
    return a->pid == b->pid && a->started_at == b->started_at;
}

/* Rename first so a concurrent handler can safely publish its own marker. */
static void unlink_marker_if_matches(const reader_marker *expected)
{
    char claimed[512];
    int n = snprintf(claimed, sizeof(claimed), "%s.claim.%d",
                     READER_PIDFILE, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(claimed)
        || rename(READER_PIDFILE, claimed) != 0)
        return;
    reader_marker found = {0, 0};
    FILE *f = fopen(claimed, "r");
    long long started = 0;
    int fields = f ? fscanf(f, "%d %lld", &found.pid, &started) : 0;
    if (f)
        fclose(f);
    found.started_at = fields == 2 ? (int64_t)started : 0;
    if (fields < 1 || !same_marker(&found, expected)) {
        if (link(claimed, READER_PIDFILE) != 0 && errno != EEXIST)
            perror("Better Stats daemon: restore reader marker");
    }
    unlink(claimed);
}

static int write_reader_session(int pid, int64_t bookid, int64_t opentime)
{
    char temp[512];
    int n = snprintf(temp, sizeof(temp), "%s.tmp.%d",
                     READER_SESSION, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(temp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *f = fopen(temp, "w");
    if (!f)
        return -1;
    int ok = fprintf(f, "%d %lld %lld\n", pid,
                     (long long)bookid, (long long)opentime) > 0;
    if (fclose(f) != 0)
        ok = 0;
    int saved = EIO;
    if (ok) {
        if (rename(temp, READER_SESSION) == 0)
            return 0;
        saved = errno;
    }
    unlink(temp);
    errno = saved;
    return -1;
}

static int read_reader_session(int *pid, int64_t *bookid, int64_t *opentime)
{
    FILE *f = fopen(READER_SESSION, "r");
    if (!f)
        return -1;
    char line[96] = "";
    int p = -1, used = 0;
    long long b = 0, o = 0;
    int fields = fgets(line, sizeof(line), f)
        ? sscanf(line, " %d %lld %lld %n", &p, &b, &o, &used) : 0;
    fclose(f);
    while (line[used] == ' ' || line[used] == '\t'
           || line[used] == '\r' || line[used] == '\n')
        used++;
    if (fields != 3 || line[used] != '\0' || p < 0 || b <= 0 || o <= 0)
        return -1;
    *pid = p;
    *bookid = (int64_t)b;
    *opentime = (int64_t)o;
    return 0;
}

enum reader_state { RS_UNTRACKED, RS_TRACKING, RS_FROZEN };
enum marker_relation { REL_LEGACY, REL_WAIT, REL_MATCH, REL_STALE };
enum foreground_state { FG_UNKNOWN = -1, FG_BACKGROUND = 0, FG_FOREGROUND = 1 };

typedef struct {
    int known;
    int pid;
    char app[128];
} active_task_info;

static active_task_info current_task(void)
{
    active_task_info result;
    memset(&result, 0, sizeof(result));
#ifdef HAVE_DEVICE_STATE
    int task = 0, subtask = 0;
    GetActiveTask(&task, &subtask);
    (void)subtask;
    taskinfo *info = task > 0 ? GetTaskInfo(task) : NULL;
    if (info && info->mainpid > 0) {
        result.known = 1;
        result.pid = (int)info->mainpid;
        snprintf(result.app, sizeof(result.app), "%s",
                 info->appname ? info->appname : "");
    }
#endif
    return result;
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
        return 1;
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
    if (fw->opentime < marker->started_at - READER_START_GRACE_SECONDS)
        return REL_WAIT;
    if (fw->opentime > marker->started_at + READER_START_GRACE_SECONDS)
        return REL_STALE;
    return REL_MATCH;
}

static int same_firmware_session(const pb_state *fw, int64_t book, int64_t open)
{
    return fw && fw->bookid == book && fw->opentime == open;
}

static int persist_freeze(const reader_marker *marker,
                          int64_t bookid, int64_t opentime)
{
    if (write_reader_session(0, bookid, opentime) != 0)
        return -1;
    unlink_marker_if_matches(marker);
    return 0;
}

static int64_t clock_ns(struct timespec *out)
{
    if (clock_gettime(TRACK_CLOCK, out) != 0)
        return -1;
    return (int64_t)out->tv_sec * 1000000000LL + out->tv_nsec;
}

static void add_second(struct timespec *value)
{
    value->tv_sec += POLL_SECONDS;
}

static void wait_for_tick(struct timespec *deadline)
{
#ifdef PLATFORM_FC
    if (running) {
        int rc = clock_nanosleep(TRACK_CLOCK, TIMER_ABSTIME, deadline, NULL);
        (void)rc; /* EINTR and other failures both return control to the loop. */
    }
    struct timespec now;
    if (clock_gettime(TRACK_CLOCK, &now) == 0) {
        *deadline = now;
        add_second(deadline);
    }
#else
    (void)deadline;
    struct timespec delay = {POLL_SECONDS, 0};
    nanosleep(&delay, NULL); /* EINTR deliberately ends the wait. */
#endif
}

static int close_counting(tracker *t, const pb_state *fw, int have_fw,
                          int64_t present, int64_t wall, int observe)
{
    if (observe && have_fw
        && same_firmware_session(fw, t->cur_book, t->cur_open)
        && tracker_observe(t, fw, present) < 0)
        return -1;
    return tracker_flush(t, present, wall);
}

int run_daemon(void)
{
    setsid();
    if (active_daemon_pid())
        return 0;

#ifdef HAVE_DEVICE_STATE
    InitInkview(TASK_NOHANDLER);
#endif
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_term;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) != 0
        || sigaction(SIGINT, &action, NULL) != 0)
        return 1;

    mkdir(STATS_DIR, 0755);
    unlink(LEGACY_PIDFILE);
    if (write_pidfile() != 0)
        return 1;

    tracker t;
    if (tracker_init(&t, stats_db_path(), explorer_db_path()) != 0) {
        unlink(PIDFILE);
        return 1;
    }

    enum reader_state rstate = RS_UNTRACKED;
    int reader_pid = 0, foreground = FG_BACKGROUND;
    int64_t reader_book = 0, reader_open = 0;
    int recovered = 0, restored = 0, was_counting = 0, status = 0;
    int64_t present_ns = 0;
    struct timespec last_clock, deadline;
    int64_t last_ns = clock_ns(&last_clock);
    if (last_ns < 0) {
        status = 1;
        goto cleanup;
    }
    deadline = last_clock;
    add_second(&deadline);

    while (running) {
        struct timespec now_clock;
        int64_t now_ns = clock_ns(&now_clock);
        if (now_ns < 0) {
            status = 1;
            break;
        }
        if (was_counting && now_ns > last_ns)
            present_ns += now_ns - last_ns;
        last_ns = now_ns;
        int64_t present = present_ns / 1000000000LL;
        int64_t wall = (int64_t)time(NULL);

        pb_state fw;
        int fw_result = tracker_read_state(t.explorer_path, &fw);
        int have_fw = fw_result == 0;
        if (!recovered && fw_result >= 0) {
            if (tracker_recover(&t, have_fw ? fw.bookid : 0,
                                have_fw ? fw.opentime : 0) < 0) {
                status = 1;
                break;
            }
            recovered = 1;
        }

        if (!restored && recovered && have_fw) {
            int saved_pid = 0;
            int64_t saved_book = 0, saved_open = 0;
            if (read_reader_session(&saved_pid, &saved_book, &saved_open) == 0
                && have_fw
                && same_firmware_session(&fw, saved_book, saved_open)) {
                reader_book = saved_book;
                reader_open = saved_open;
                if (saved_pid == 0) {
                    rstate = RS_FROZEN;
                } else if (pid_exists(saved_pid)) {
                    reader_pid = saved_pid;
                    rstate = RS_TRACKING;
                } else {
                    reader_marker dead = {saved_pid, 0};
                    if (write_reader_session(0, saved_book, saved_open) != 0) {
                        status = 1;
                        break;
                    }
                    unlink_marker_if_matches(&dead);
                    rstate = RS_FROZEN;
                }
            } else {
                unlink(READER_SESSION);
            }
            restored = 1;
        }

        reader_marker marker = {0, 0};
        enum marker_status marker_status = read_reader_marker(&marker);
        active_task_info task = current_task();
        int locked = device_locked();

        /* A broken marker is not absence: pause without discarding a binding. */
        if (marker_status == MARKER_INVALID || marker_status == MARKER_IO
            || (!have_fw && rstate != RS_TRACKING)) {
            if (was_counting
                && close_counting(&t, &fw, have_fw, present, wall, 1) != 0) {
                status = 1;
                break;
            }
            was_counting = 0;
            if (running)
                wait_for_tick(&deadline);
            continue;
        }

        enum marker_relation relation = REL_LEGACY;
        if (marker_status == MARKER_VALID && have_fw) {
            relation = marker_relation(&marker, &fw);
            if (relation == REL_STALE) {
                unlink_marker_if_matches(&marker);
                marker_status = MARKER_ABSENT;
            }
        }

        if (marker_status == MARKER_VALID
            && (relation == REL_WAIT
                || (!have_fw
                    && !(rstate == RS_TRACKING
                         && marker.pid == reader_pid)))) {
            if (was_counting && tracker_flush(&t, present, wall) != 0) {
                status = 1;
                break;
            }
            was_counting = 0;
            if (running)
                wait_for_tick(&deadline);
            continue;
        }

        int session_changed = have_fw && rstate != RS_UNTRACKED
            && !same_firmware_session(&fw, reader_book, reader_open);
        int marker_live = marker_status == MARKER_VALID && pid_exists(marker.pid);
        int marker_fg = marker_status == MARKER_VALID
            ? reader_foreground(&task, marker.pid) : FG_UNKNOWN;
        int marker_matches = marker_status == MARKER_VALID
            && (relation == REL_MATCH || relation == REL_LEGACY);

        /* Prefer a confirmed replacement over freezing the cached dead PID. */
        if (have_fw && marker_matches && marker_live
            && marker_fg == FG_FOREGROUND
            && (rstate != RS_TRACKING || reader_pid != marker.pid
                || session_changed)) {
            int replacing = rstate == RS_TRACKING && reader_pid != marker.pid;
            if (replacing && was_counting) {
                int64_t boundary = session_changed ? fw.opentime - 1 : wall;
                if (close_counting(&t, &fw, have_fw, present,
                                   boundary, !session_changed) != 0) {
                    status = 1;
                    break;
                }
                was_counting = 0;
            }
            if (tracker_prepare(&t, &fw, present) != 0
                || write_reader_session(marker.pid, fw.bookid, fw.opentime) != 0) {
                status = 1;
                break;
            }
            reader_pid = marker.pid;
            reader_book = fw.bookid;
            reader_open = fw.opentime;
            rstate = RS_TRACKING;
            foreground = FG_FOREGROUND;
            session_changed = 0;
        } else if (session_changed && marker_status == MARKER_VALID
                   && marker_matches && marker_live) {
            /* Matching but not foreground: keep the new session pending. */
            if (was_counting && tracker_flush(&t, present, fw.opentime - 1) != 0) {
                status = 1;
                break;
            }
            was_counting = 0;
            rstate = RS_UNTRACKED;
            reader_pid = 0;
            reader_book = reader_open = 0;
            unlink(READER_SESSION);
            if (running)
                wait_for_tick(&deadline);
            continue;
        }

        if (marker_status == MARKER_VALID && relation == REL_LEGACY
            && !marker_live
            && !(rstate == RS_TRACKING && reader_pid == marker.pid
                 && !session_changed)) {
            unlink_marker_if_matches(&marker);
            marker_status = MARKER_ABSENT;
            marker_matches = 0;
        }

        if (have_fw && marker_matches && !marker_live
            && !(rstate == RS_TRACKING && reader_pid == marker.pid
                 && !session_changed)) {
            if (tracker_prepare(&t, &fw, present) != 0
                || tracker_observe(&t, &fw, present) < 0
                || persist_freeze(&marker, fw.bookid, fw.opentime) != 0) {
                status = 1;
                break;
            }
            reader_pid = 0;
            reader_book = fw.bookid;
            reader_open = fw.opentime;
            rstate = RS_FROZEN;
            was_counting = 0;
        } else if (rstate == RS_TRACKING && !pid_exists(reader_pid)) {
            reader_marker old = {reader_pid, 0}, current = {0, 0};
            if (read_reader_marker(&current) == MARKER_VALID
                && current.pid == reader_pid)
                old = current;
            if (close_counting(&t, &fw, have_fw, present, wall, 1) != 0
                || persist_freeze(&old, reader_book, reader_open) != 0) {
                status = 1;
                break;
            }
            reader_pid = 0;
            rstate = RS_FROZEN;
            was_counting = 0;
        } else if (session_changed) {
            if (was_counting && tracker_flush(&t, present, fw.opentime - 1) != 0) {
                status = 1;
                break;
            }
            was_counting = 0;
            rstate = RS_UNTRACKED;
            reader_pid = 0;
            reader_book = reader_open = 0;
            unlink(READER_SESSION);
        }

        if (rstate == RS_TRACKING) {
            enum foreground_state observed = reader_foreground(&task, reader_pid);
            if (observed != FG_UNKNOWN)
                foreground = observed;
        }

        int counting_now = 0;
        if (have_fw && !locked) {
            if (rstate == RS_TRACKING)
                counting_now = foreground == FG_FOREGROUND;
            else if (rstate == RS_UNTRACKED && marker_status == MARKER_ABSENT)
                counting_now = fallback_app_allowed(&task);
        } else if (!have_fw && rstate == RS_TRACKING && !locked) {
            counting_now = foreground == FG_FOREGROUND;
        }

        if (counting_now && !was_counting) {
            int failed = have_fw
                ? tracker_prepare(&t, &fw, present) != 0
                    || tracker_resume(&t, present, wall) != 0
                    || tracker_observe(&t, &fw, present) < 0
                : tracker_resume(&t, present, wall) != 0;
            if (failed) {
                status = 1;
                break;
            }
        } else if (counting_now) {
            if (have_fw && tracker_observe(&t, &fw, present) < 0) {
                status = 1;
                break;
            }
        } else if (was_counting) {
            if (close_counting(&t, &fw, have_fw, present, wall, 1) != 0) {
                status = 1;
                break;
            }
        }
        was_counting = counting_now;
        if (running)
            wait_for_tick(&deadline);
    }

    if (status == 0) {
        struct timespec final_clock;
        int64_t final_ns = clock_ns(&final_clock);
        if (final_ns < 0) {
            status = 1;
        } else {
            if (was_counting && final_ns > last_ns)
                present_ns += final_ns - last_ns;
            if (tracker_flush(&t, present_ns / 1000000000LL,
                              (int64_t)time(NULL)) != 0)
                status = 1;
        }
    }

cleanup:
    tracker_close(&t);
    unlink(PIDFILE);
    return status;
}
