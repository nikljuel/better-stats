#define _GNU_SOURCE
#include "daemon.h"
#include "tracker.h"
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

/* Hardcover sync integration, ported from a downstream fork - see its own
 * README/PR description for the full feature. Completion detection needs
 * the explorer-3.db + hardcoversync.db round trip far less often than
 * reading-progress tracking does, so it runs on its own separate,
 * hourly cadence rather than every poll. */
#define HARDCOVER_CHECK_SECONDS 3600

/* Checks for linked books that have gone complete since we last looked,
 * flagging them in hardcoversync.db with a plain SQLite write - no
 * networking happens here at all. The Qt app's own
 * checkPendingFinishConfirm() (called once per app launch, not on a
 * timer) is the only thing that ever actually talks to Hardcover, and
 * only once you're looking at the resulting confirmation. Silently does
 * nothing if hardcoversync.db or its "links" table doesn't exist yet
 * (e.g. the app has never been opened, so nothing has ever been linked) -
 * this daemon starts at boot and may well run before that first launch. */
static void check_hardcover_completions(const char *explorer_path)
{
    sqlite3 *linkDb = NULL;
    if (sqlite3_open(STATS_DIR "/hardcoversync.db", &linkDb) != SQLITE_OK) {
        if (linkDb)
            sqlite3_close(linkDb);
        return;
    }
    sqlite3_busy_timeout(linkDb, 1000);

    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT book_id FROM links WHERE user_book_id != 0"
        " AND (finished_at IS NULL OR finished_at = '')"
        " AND IFNULL(pending_finish_confirm,0) = 0";
    if (sqlite3_prepare_v2(linkDb, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(linkDb);
        return; /* table/column doesn't exist yet, or some other issue */
    }

    /* Collect candidates before issuing any writes, rather than
     * interleaving reads and writes against the same table mid-iteration -
     * simpler to reason about, and this table is tiny either way. */
    sqlite3_int64 candidates[64];
    int n = 0;
    while (n < 64 && sqlite3_step(st) == SQLITE_ROW)
        candidates[n++] = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    if (n == 0) {
        sqlite3_close(linkDb);
        return;
    }

    sqlite3 *exp = NULL;
    if (sqlite3_open_v2(explorer_path, &exp, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(linkDb);
        if (exp)
            sqlite3_close(exp);
        return;
    }
    sqlite3_busy_timeout(exp, 1000);

    for (int i = 0; i < n; i++) {
        sqlite3_stmt *cst = NULL;
        int completed = 0;
        if (sqlite3_prepare_v2(exp,
                "SELECT IFNULL(completed,0) FROM books_settings WHERE bookid=?1 AND profileid=1",
                -1, &cst, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(cst, 1, candidates[i]);
            if (sqlite3_step(cst) == SQLITE_ROW)
                completed = sqlite3_column_int(cst, 0);
        }
        sqlite3_finalize(cst);

        if (completed) {
            sqlite3_stmt *ust = NULL;
            if (sqlite3_prepare_v2(linkDb,
                    "UPDATE links SET pending_finish_confirm = 1 WHERE book_id = ?1",
                    -1, &ust, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(ust, 1, candidates[i]);
                sqlite3_step(ust);
            }
            sqlite3_finalize(ust);
        }
    }
    sqlite3_close(exp);
    sqlite3_close(linkDb);
}

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
    /* Starts at the full interval, not 0, so the very first loop iteration
     * checks immediately rather than waiting a whole hour after daemon
     * startup before ever looking. Incremented by the actual, measured gap
     * each time (not a fixed POLL_SECONDS assumption), since the loop can
     * return much sooner than that (an inotify wakeup) or much later (the
     * process was suspended). */
    time_t since_hardcover_check = HARDCOVER_CHECK_SECONDS;
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

        since_hardcover_check += gap;
        if (since_hardcover_check >= HARDCOVER_CHECK_SECONDS) {
            check_hardcover_completions(t.explorer_path);
            since_hardcover_check = 0;
        }

        wait_for_library_change(wfd);
    }
    {
        const time_t now = time(NULL);
        const time_t gap = last_loop ? now - last_loop : 0;
        if (gap > 0 && gap <= PRESENCE_GAP_SECONDS)
            present += gap;
        tracker_flush(&t, present, now);
    }
    if (wfd >= 0)
        close(wfd);
    tracker_close(&t);
    unlink(PIDFILE);
    return 0;
}
