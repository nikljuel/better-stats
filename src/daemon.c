#define _GNU_SOURCE
#include "daemon.h"
#include "tracker.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void on_term(int sig)
{
    (void)sig;
    running = 0;
}

static int daemon_pid_matches(int pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
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

/* A stale PID can point at an unrelated process after USB mode. */
static int active_daemon_pid(void)
{
    FILE *f = fopen(PIDFILE, "r");
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
    if (pid == (int)getpid()) {
        unlink(PIDFILE);
        return 0;
    }
    if (pid > 0 && kill(pid, 0) == 0 && daemon_pid_matches(pid))
        return pid;
    unlink(PIDFILE);
    return 0;
}

void stop_daemon(void)
{
    int pid = active_daemon_pid();
    if (pid > 0)
        kill(pid, SIGTERM);
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

int run_daemon(void)
{
    setsid();
    if (active_daemon_pid())
        return 0;
    mkdir(STATS_DIR, 0755);
    unlink(LEGACY_PIDFILE);
    write_pidfile();
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    tracker t;
    if (tracker_init(&t, stats_db_path(), explorer_db_path()) != 0) {
        unlink(PIDFILE);
        return 1;
    }
    tracker_recover(&t);
    const int wfd = watch_library(t.explorer_path);
    time_t last_loop = 0;
    int64_t present = 0;
    while (running) {
        pb_state s;
        /* Our own presence is the measurement. A device being read runs this
         * loop every few seconds; a locked one wakes only every few minutes and
         * the gap blows past PRESENCE_GAP_SECONDS. Summing the short gaps
         * therefore measures the time somebody was actually at the book, which
         * the firmware's two endpoints cannot distinguish from standby. */
        const time_t loop_now = time(NULL);
        const time_t gap = last_loop ? loop_now - last_loop : 0;
        if (gap > 0 && gap <= PRESENCE_GAP_SECONDS)
            present += gap;
        last_loop = loop_now;

        if (tracker_read_state(t.explorer_path, &s) == 0)
            tracker_observe(&t, &s, present);
        wait_for_library_change(wfd);
    }
    if (wfd >= 0)
        close(wfd);
    tracker_close(&t);
    unlink(PIDFILE);
    return 0;
}
