#define _GNU_SOURCE
#include "daemon.h"
#include "tracker.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PROC_ROOT
#define PROC_ROOT "/proc"
#endif
#ifndef READER_START_GRACE_SECONDS
#define READER_START_GRACE_SECONDS 5
#endif

static volatile sig_atomic_t running = 1;

static void on_term(int sig)
{
    (void)sig;
    running = 0;
}

static int daemon_pid_matches(int pid)
{
    char path[64];
    snprintf(path, sizeof(path), PROC_ROOT "/%d/cmdline", pid);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char cmdline[512];
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
        unsigned int parsed = 0;
        const char *p = line;
        while (*p >= '0' && *p <= '9'
               && parsed <= (2147483647u - (unsigned int)(*p - '0')) / 10u)
            parsed = parsed * 10u + (unsigned int)(*p++ - '0');
        if (p != line && (*p == '\n' || *p == '\0') && parsed > 0)
            pid = (int)parsed;
    }
    fclose(f);
    return pid;
}

static int pid_exists(int pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

/* New handlers add their launch time; one-field markers remain compatible. */
static int read_reader_pid(const char *path, int64_t *started_at)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int pid = 0;
    long long started = 0;
    int n = fscanf(f, "%d %lld", &pid, &started);
    fclose(f);
    if (n < 1 || pid <= 0 || started < 0)
        return 0;
    if (started_at)
        *started_at = n == 2 ? (int64_t)started : 0;
    return pid;
}

static void unlink_reader_pid_if_matches(int pid)
{
    char claimed[512];
    int n = snprintf(claimed, sizeof(claimed), "%s.claim.%d",
                     READER_PIDFILE, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(claimed)
        || rename(READER_PIDFILE, claimed) != 0)
        return;
    int found = read_reader_pid(claimed, NULL);
    if (found > 0 && found != pid
        && link(claimed, READER_PIDFILE) != 0 && errno != EEXIST)
        perror("Better Stats daemon: restore reader PID");
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
    if (ok && rename(temp, READER_SESSION) == 0)
        return 0;
    int saved_errno = errno ? errno : EIO;
    unlink(temp);
    errno = saved_errno;
    return -1;
}

static int read_reader_session(int *pid, int64_t *bookid, int64_t *opentime)
{
    FILE *f = fopen(READER_SESSION, "r");
    if (!f)
        return -1;
    long long b = 0, o = 0;
    int p = -1, n = 0;
    char line[64];
    if (fgets(line, sizeof(line), f))
        n = sscanf(line, "%d %lld %lld", &p, &b, &o);
    fclose(f);
    if (n != 3 || p < 0)
        return -1;
    *pid = p;
    *bookid = (int64_t)b;
    *opentime = (int64_t)o;
    return 0;
}

/* A stale PID can point at an unrelated process after USB mode. */
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

static void write_pidfile(void)
{
    FILE *f = fopen(PIDFILE, "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

/* Watch the directory rather than the file: SQLite writes journals next to the
 * database and may replace it, so a watch on the file alone would go stale.
 * Returns -1 if inotify is unavailable; the caller then falls back to waiting. */
static int watch_library(const char *db_path)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0)
        return -1;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", db_path);
    char *slash = strrchr(dir, '/');
    if (!slash) {
        close(fd);
        return -1;
    }
    *slash = '\0';
    if (inotify_add_watch(fd, dir,
                          IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Blocks until the firmware touches the library database, or the timeout runs
 * out. This is the whole point: a page turn makes the kernel wake us, so we no
 * longer depend on getting scheduled often enough to notice one ourselves. */
static void wait_for_library_change(int wfd)
{
    if (wfd < 0) {
        const time_t due = time(NULL) + POLL_SECONDS;
        while (running && time(NULL) < due) {
            const struct timespec slice = {0, 200 * 1000 * 1000};
            nanosleep(&slice, NULL);
        }
        return;
    }
    fd_set r;
    FD_ZERO(&r);
    FD_SET(wfd, &r);
    struct timeval tv = {POLL_SECONDS, 0};
    if (select(wfd + 1, &r, NULL, NULL, &tv) > 0) {
        char buf[4096];
        while (read(wfd, buf, sizeof(buf)) > 0)
            ; /* drain: one page turn can produce several events */
        /* Our own read of the library reopens its WAL and touches -shm in the
         * very directory we watch, so an unthrottled loop re-triggers itself
         * forever. Settle first, then drain whatever that produced. */
        const struct timespec settle = {DEBOUNCE_SECONDS, 0};
        nanosleep(&settle, NULL);
        while (read(wfd, buf, sizeof(buf)) > 0)
            ;
    }
}

static int add_awake_time(int64_t *present, int64_t *last_loop)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    const int64_t seconds = (int64_t)now.tv_sec;
    if (*last_loop >= 0 && seconds > *last_loop)
        *present += seconds - *last_loop;
    *last_loop = seconds;
    return 0;
}

enum reader_state { RS_UNTRACKED, RS_TRACKING, RS_FROZEN };

static void persist_reader_freeze(int pid, int64_t bookid, int64_t opentime)
{
    if (write_reader_session(0, bookid, opentime) == 0)
        unlink_reader_pid_if_matches(pid);
    else
        perror("Better Stats daemon: write reader session");
}

static enum reader_state init_reader_state(const pb_state *fw,
                                           int *rpid, int64_t *rbookid,
                                           int64_t *ropentime)
{
    int sp = 0;
    int64_t sb = 0, so = 0;
    *rpid = 0;
    *rbookid = 0;
    *ropentime = 0;
    if (read_reader_session(&sp, &sb, &so) != 0)
        return RS_UNTRACKED;
    /* A transient explorer DB error must not discard a persisted freeze. */
    if (!fw)
        return RS_UNTRACKED;
    if (sb != fw->bookid || so != fw->opentime) {
        if (sp > 0)
            unlink_reader_pid_if_matches(sp);
        unlink(READER_SESSION);
        return RS_UNTRACKED;
    }
    *rbookid = sb;
    *ropentime = so;
    if (sp == 0)
        return RS_FROZEN;
    if (pid_exists(sp)) {
        *rpid = sp;
        return RS_TRACKING;
    }
    persist_reader_freeze(sp, sb, so);
    return RS_FROZEN;
}

int run_daemon(void)
{
    setsid();
    if (active_daemon_pid())
        return 0;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_term;
    sigemptyset(&action.sa_mask);
    /* select() must return immediately so an update can replace this daemon. */
    if (sigaction(SIGTERM, &action, NULL) != 0
        || sigaction(SIGINT, &action, NULL) != 0)
        return 1;

    mkdir(STATS_DIR, 0755);
    unlink(LEGACY_PIDFILE);
    write_pidfile();

    tracker t;
    if (tracker_init(&t, stats_db_path(), explorer_db_path()) != 0) {
        unlink(PIDFILE);
        return 1;
    }
    tracker_recover(&t);
    const int wfd = watch_library(t.explorer_path);

    /* Reader tracking: freeze the presence counter when the reader exits. */
    int reader_pid = 0;
    int64_t reader_bookid = 0, reader_opentime = 0;
    enum reader_state rstate = RS_UNTRACKED;
    int reader_initialized = 0;
    int64_t last_loop = -1;
    int64_t present = 0;
    int status = 0;
    while (running) {
        pb_state s;
        int have_state = tracker_read_state(t.explorer_path, &s) == 0;

        /* Restore persisted state only after the firmware DB can confirm which
         * session is current. Until then, leave the marker untouched. */
        if (!reader_initialized && have_state) {
            rstate = init_reader_state(&s, &reader_pid,
                                       &reader_bookid, &reader_opentime);
            reader_initialized = 1;
            if (rstate == RS_FROZEN)
                last_loop = -1;
        }

        /* Session change resets reader tracking. */
        if (rstate != RS_UNTRACKED && have_state
            && (s.bookid != reader_bookid || s.opentime != reader_opentime)) {
            if (reader_pid > 0)
                unlink_reader_pid_if_matches(reader_pid);
            rstate = RS_UNTRACKED;
            reader_pid = 0;
            unlink(READER_SESSION);
        }

        /* State transitions. */
        if (rstate == RS_TRACKING && !pid_exists(reader_pid)) {
            if (add_awake_time(&present, &last_loop) != 0) {
                perror("Better Stats daemon: clock_gettime");
                status = 1;
                break;
            }
            tracker_flush(&t, present, time(NULL));
            last_loop = -1;
            persist_reader_freeze(reader_pid, reader_bookid, reader_opentime);
            reader_pid = 0;
            rstate = RS_FROZEN;
        } else if (have_state
                   && (rstate == RS_UNTRACKED || rstate == RS_FROZEN)) {
            int64_t started_at = 0;
            int fp = read_reader_pid(READER_PIDFILE, &started_at);
            int session_ready = started_at == 0
                || s.opentime >= started_at - READER_START_GRACE_SECONDS;
            if (fp > 0 && session_ready && pid_exists(fp)) {
                reader_pid = fp;
                reader_bookid = s.bookid;
                reader_opentime = s.opentime;
                if (write_reader_session(fp, s.bookid, s.opentime) != 0)
                    perror("Better Stats daemon: write reader session");
                rstate = RS_TRACKING;
            } else if (fp > 0 && session_ready && rstate == RS_UNTRACKED) {
                /* The reader can exit before the daemon gets its first poll.
                 * Bind that dead handler PID to the visible session instead
                 * of falling back to counting an already closed book. */
                if (add_awake_time(&present, &last_loop) != 0) {
                    perror("Better Stats daemon: clock_gettime");
                    status = 1;
                    break;
                }
                tracker_flush(&t, present, time(NULL));
                last_loop = -1;
                reader_bookid = s.bookid;
                reader_opentime = s.opentime;
                persist_reader_freeze(fp, s.bookid, s.opentime);
                reader_pid = 0;
                rstate = RS_FROZEN;
            }
        }

        /* Accumulate monotonic awake time unless frozen. */
        if (rstate != RS_FROZEN) {
            if (add_awake_time(&present, &last_loop) != 0) {
                perror("Better Stats daemon: clock_gettime");
                status = 1;
                break;
            }
        }

        if (have_state)
            tracker_observe(&t, &s, present);
        if (running)
            wait_for_library_change(wfd);
    }
    if (status == 0 && rstate != RS_FROZEN
        && add_awake_time(&present, &last_loop) != 0) {
        perror("Better Stats daemon: clock_gettime");
        status = 1;
    }
    tracker_flush(&t, present, time(NULL));
    if (wfd >= 0)
        close(wfd);
    tracker_close(&t);
    unlink(PIDFILE);
    return status;
}
