#include "daemon.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_DIR "/tmp/bs_daemon_test"
#define PROC_DIR TEST_DIR "/proc"
#define STATS_DB_TEST TEST_DIR "/stats.db"
#define EXPLORER_DB_TEST TEST_DIR "/explorer.db"
#define DEVICE_STATE_TEST TEST_DIR "/device.state"

static char *test_argv0;

static void write_cmdline(pid_t pid, const char *program)
{
    char directory[128];
    char path[160];
    snprintf(directory, sizeof(directory), PROC_DIR "/%d", (int)pid);
    assert(mkdir(directory, 0755) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/cmdline", directory);
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(program, 1, strlen(program) + 1, file) == strlen(program) + 1);
    static const char argument[] = "--daemon";
    assert(fwrite(argument, 1, sizeof(argument), file) == sizeof(argument));
    assert(fclose(file) == 0);
}

static double elapsed(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec)
        + (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static void clean_state(void)
{
    unlink(PIDFILE);
    unlink(LEGACY_PIDFILE);
    unlink(STATS_DB_TEST);
    unlink(EXPLORER_DB_TEST);
    unlink(READER_PIDFILE);
    unlink(READER_SESSION);
    unlink(DEVICE_STATE_TEST);
}

static void write_explorer(int bookid, int opentime)
{
    unlink(EXPLORER_DB_TEST);
    sqlite3 *db = NULL;
    assert(sqlite3_open(EXPLORER_DB_TEST, &db) == SQLITE_OK);
    char sql[512];
    snprintf(sql, sizeof(sql),
        "CREATE TABLE books_impl(id INTEGER PRIMARY KEY,title TEXT,author TEXT);"
        "CREATE TABLE files(book_id INTEGER,storageid INTEGER,fast_hash BLOB);"
        "CREATE TABLE books_settings(bookid INTEGER,profileid INTEGER,"
        " position TEXT,position_ts INTEGER,cpage INTEGER,npage INTEGER,"
        " opentime INTEGER,completed INTEGER,completed_ts INTEGER);"
        "INSERT INTO books_impl VALUES(%d,'Test','Author');"
        "INSERT INTO books_settings VALUES(%d,1,'p',%d,1,100,%d,0,0);",
        bookid, bookid, opentime, opentime);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static void update_explorer(int bookid, int opentime)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(EXPLORER_DB_TEST, &db) == SQLITE_OK);
    sqlite3_busy_timeout(db, 2000);
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO books_impl VALUES(%d,'Test%d','Author');"
        "DELETE FROM books_settings;"
        "INSERT INTO books_settings VALUES(%d,1,'p',%d,1,100,%d,0,0);",
        bookid, bookid, bookid, opentime, opentime);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static int64_t read_active_seconds(int bookid)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &db) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT IFNULL(SUM(active_seconds),0) FROM sessions WHERE book_id=?1", -1,
        &st, NULL) == SQLITE_OK);
    sqlite3_bind_int(st, 1, bookid);
    int64_t val = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        val = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    assert(sqlite3_close(db) == SQLITE_OK);
    return val;
}

static int read_active_rows(int bookid)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &db) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM sessions WHERE book_id=?1 AND active_seconds>0",
        -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int(st, 1, bookid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int value = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    assert(sqlite3_close(db) == SQLITE_OK);
    return value;
}

static void write_pid(const char *path, pid_t pid)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "%d\n", (int)pid);
    assert(fclose(f) == 0);
}

static void write_timed_reader_pid(pid_t pid, int64_t started_at)
{
    FILE *f = fopen(READER_PIDFILE, "w");
    assert(f != NULL);
    fprintf(f, "%d %lld\n", (int)pid, (long long)started_at);
    assert(fclose(f) == 0);
}

static void write_device_state(int locked, pid_t pid, const char *app)
{
    FILE *f = fopen(DEVICE_STATE_TEST, "w");
    assert(f != NULL);
    fprintf(f, "%d %d %s\n", locked, (int)pid, app);
    assert(fclose(f) == 0);
}

static int read_pid(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1)
        pid = 0;
    fclose(f);
    return pid;
}

static void assert_reader_session(int pid, int64_t bookid, int64_t opentime)
{
    FILE *f = fopen(READER_SESSION, "r");
    assert(f != NULL);
    int actual_pid = -1;
    long long actual_bookid = 0, actual_opentime = 0;
    assert(fscanf(f, "%d %lld %lld", &actual_pid,
                  &actual_bookid, &actual_opentime) == 3);
    assert(fclose(f) == 0);
    assert(actual_pid == pid);
    assert(actual_bookid == (long long)bookid);
    assert(actual_opentime == (long long)opentime);
}

static pid_t make_sleeping_child(void)
{
    pid_t p = fork();
    assert(p >= 0);
    if (p == 0) {
        pause();
        _exit(0);
    }
    return p;
}

static void kill_and_wait(pid_t pid)
{
    assert(kill(pid, SIGTERM) == 0);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
}

static pid_t start_daemon(void)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(test_argv0, test_argv0, "--daemon", (char *)NULL);
        _exit(127);
    }
    write_cmdline(child, test_argv0);
    for (int i = 0; i < 200 && access(PIDFILE, F_OK) != 0; ++i)
        usleep(10 * 1000);
    assert(access(PIDFILE, F_OK) == 0);
    return child;
}

static void stop_daemon_and_wait(pid_t child)
{
    struct timespec start, end;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    int stopped = stop_daemon();
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    if (stopped != 0)
        kill(child, SIGKILL);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(stopped == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(elapsed(&start, &end) < 5.0);
    assert(access(PIDFILE, F_OK) != 0);
}

static void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000 * 1000};
    nanosleep(&ts, NULL);
}

/* Original test: UNTRACKED fallback, no reader PID file. */
static void test_shutdown(void)
{
    puts("test_shutdown...");
    clean_state();
    write_explorer(1, 1000);

    pid_t daemon = start_daemon();
    sleep_ms(1200);
    stop_daemon_and_wait(daemon);

    assert(read_active_seconds(1) >= 1);
    puts("  ok");
}

/* Tests A+B: reader tracking freeze and session-change unfreeze. */
static void test_reader_tracking(void)
{
    puts("test_reader_tracking...");
    clean_state();
    write_explorer(1, 1000);

    /* A: start daemon with a living "reader" PID. */
    pid_t reader_a = make_sleeping_child();
    write_pid(READER_PIDFILE, reader_a);
    write_device_state(0, reader_a, "eink-reader.app");

    pid_t daemon = start_daemon();
    sleep_ms(2200);

    /* A new handler may overwrite the marker before death is detected. */
    pid_t reader_b = make_sleeping_child();
    write_pid(READER_PIDFILE, reader_b);
    write_device_state(0, reader_b, "eink-reader.app");
    kill_and_wait(reader_a);

    /* The marker survives A's death and B is adopted for the same firmware
     * session, covering a reopen within opentime's one-second resolution. */
    sleep_ms((POLL_SECONDS + 1) * 1000);
    assert(read_pid(READER_PIDFILE) == (int)reader_b);
    assert_reader_session((int)reader_b, 1, 1000);

    /* Once the replacement reader dies, the session freezes normally. */
    kill_and_wait(reader_b);
    sleep_ms((POLL_SECONDS + 1) * 1000);
    assert(access(READER_PIDFILE, F_OK) != 0);
    assert_reader_session(0, 1, 1000);

    /* active_seconds must have been flushed. */
    int64_t val_a = read_active_seconds(1);
    assert(val_a >= 1);

    /* Frozen: further time must not accumulate. */
    sleep_ms(2000);
    int64_t val_b = read_active_seconds(1);
    assert(val_b == val_a);

    /* B: session change must unfreeze (FROZEN → UNTRACKED → fallback). */
    update_explorer(2, 2000);
    sleep_ms(2200);

    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(1) == val_a);
    assert(read_active_seconds(2) >= 1);
    puts("  ok");
}

/* A matching marker stays pending until that reader is actually foreground. */
static void test_quick_unhandled_switch(void)
{
    puts("test_pending_reader_switch...");
    clean_state();
    write_explorer(1, 1000);

    pid_t reader = make_sleeping_child();
    write_pid(READER_PIDFILE, reader);
    write_device_state(0, reader, "eink-reader.app");
    pid_t daemon = start_daemon();
    sleep_ms(2200);

    pid_t next = make_sleeping_child();
    update_explorer(2, 2000);
    write_timed_reader_pid(next, 2000);
    write_device_state(0, getpid(), "bookshelf.app");
    sleep_ms(1200);
    assert(read_pid(READER_PIDFILE) == (int)next);
    assert(read_active_seconds(2) == 0);

    write_device_state(0, next, "eink-reader.app");
    sleep_ms(1200);
    assert_reader_session((int)next, 2, 2000);
    kill_and_wait(reader);
    kill_and_wait(next);
    sleep_ms(1200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(2) >= 1);
    puts("  ok");
}

/* A persisted session from another book must not rebind its PID at restart. */
static void test_stale_session_at_restart(void)
{
    puts("test_stale_session_at_restart...");
    clean_state();
    write_explorer(2, 2000);

    pid_t reader = make_sleeping_child();
    write_timed_reader_pid(reader, 1000);
    write_device_state(0, reader, "eink-reader.app");
    FILE *f = fopen(READER_SESSION, "w");
    assert(f != NULL);
    fprintf(f, "%d 1 1000\n", (int)reader);
    assert(fclose(f) == 0);

    pid_t daemon = start_daemon();
    for (int i = 0; i < 30 && access(READER_PIDFILE, F_OK) == 0; ++i)
        sleep_ms(100);
    assert(access(READER_PIDFILE, F_OK) != 0);
    assert(access(READER_SESSION, F_OK) != 0);

    kill_and_wait(reader);
    sleep_ms(1200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(2) >= 1);
    puts("  ok");
}

/* A new handler must wait until its matching firmware session is visible. The
 * firmware can stamp opentime just before it invokes the handler. */
static void test_pid_waits_for_firmware_session(void)
{
    puts("test_pid_waits_for_firmware_session...");
    clean_state();
    write_explorer(1, 1000);

    pid_t reader = make_sleeping_child();
    write_timed_reader_pid(reader, 2000);
    write_device_state(0, reader, "eink-reader.app");
    pid_t daemon = start_daemon();
    sleep_ms((POLL_SECONDS + 1) * 1000);
    assert(access(READER_SESSION, F_OK) != 0);
    assert(read_pid(READER_PIDFILE) == (int)reader);

    update_explorer(2, 1998);
    sleep_ms(2200);
    assert_reader_session((int)reader, 2, 1998);

    kill_and_wait(reader);
    sleep_ms((POLL_SECONDS + 1) * 1000);
    assert_reader_session(0, 2, 1998);
    stop_daemon_and_wait(daemon);
    puts("  ok");
}

/* A handler reader may already be dead before the first daemon poll. */
static void test_dead_before_adoption(void)
{
    puts("test_dead_before_adoption...");
    clean_state();
    write_explorer(4, 4000);

    pid_t reader = make_sleeping_child();
    kill_and_wait(reader);
    write_timed_reader_pid(reader, 4000);

    pid_t daemon = start_daemon();
    sleep_ms((POLL_SECONDS + 1) * 1000);
    assert_reader_session(0, 4, 4000);
    assert(access(READER_PIDFILE, F_OK) != 0);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(4) == 0);
    puts("  ok");
}

/* Opening Better Stats can stop the daemon before its next reader-death poll.
 * The replacement daemon must normalize that dead persisted PID to FROZEN. */
static void test_restart_immediately_after_reader_death(void)
{
    puts("test_restart_immediately_after_reader_death...");
    clean_state();
    write_explorer(5, 5000);

    pid_t reader = make_sleeping_child();
    write_pid(READER_PIDFILE, reader);
    write_device_state(0, reader, "eink-reader.app");
    pid_t daemon = start_daemon();
    for (int i = 0; i < 200 && access(READER_SESSION, F_OK) != 0; ++i)
        sleep_ms(10);
    assert_reader_session((int)reader, 5, 5000);
    sleep_ms(1200);

    /* Hold the daemon in its wait so it cannot observe the reader death before
     * the launch-triggered shutdown signal arrives. */
    assert(kill(daemon, SIGSTOP) == 0);
    int status = 0;
    assert(waitpid(daemon, &status, WUNTRACED) == daemon);
    assert(WIFSTOPPED(status));
    kill_and_wait(reader);
    assert(kill(daemon, SIGTERM) == 0);
    assert(kill(daemon, SIGCONT) == 0);
    assert(waitpid(daemon, &status, 0) == daemon);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(access(PIDFILE, F_OK) != 0);

    int64_t stopped = read_active_seconds(5);
    daemon = start_daemon();
    for (int i = 0; i < 200; ++i) {
        FILE *f = fopen(READER_SESSION, "r");
        int pid = -1;
        if (f) {
            fscanf(f, "%d", &pid);
            fclose(f);
        }
        if (pid == 0)
            break;
        sleep_ms(10);
    }
    assert_reader_session(0, 5, 5000);
    sleep_ms(2200);
    assert(read_active_seconds(5) == stopped);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(5) == stopped);
    puts("  ok");
}

/* Test C: daemon restart with persisted freeze state. */
static void test_persisted_freeze(void)
{
    puts("test_persisted_freeze...");
    clean_state();

    /* Write a session file with pid=0 → persisted FROZEN. */
    {
        FILE *f = fopen(READER_SESSION, "w");
        assert(f != NULL);
        fprintf(f, "0 3 3000\n");
        assert(fclose(f) == 0);
    }

    pid_t daemon = start_daemon();
    /* A temporarily unavailable firmware DB must not discard the marker. */
    sleep_ms(1200);
    assert_reader_session(0, 3, 3000);
    write_explorer(3, 3000);
    sleep_ms(2200);
    stop_daemon_and_wait(daemon);

    assert(read_active_seconds(3) == 0);
    puts("  ok");
}

/* Foreground and keylock transitions split measured fragments. An unknown
 * task retains the last known foreground decision instead of guessing. */
static void test_device_state_gates(void)
{
    puts("test_device_state_gates...");
    clean_state();
    write_explorer(6, 6000);
    pid_t reader = make_sleeping_child();
    write_timed_reader_pid(reader, 6000);
    write_device_state(0, reader, "eink-reader.app");
    pid_t daemon = start_daemon();

    sleep_ms(2200);
    write_device_state(1, reader, "eink-reader.app");
    sleep_ms(1200);
    int64_t after_lock = read_active_seconds(6);
    assert(after_lock >= 1);
    sleep_ms(1200);
    assert(read_active_seconds(6) == after_lock);

    write_device_state(0, reader, "eink-reader.app");
    sleep_ms(1200);
    write_device_state(0, getpid(), "bookshelf.app");
    sleep_ms(1200);
    int64_t after_background = read_active_seconds(6);
    assert(after_background > after_lock);

    unlink(DEVICE_STATE_TEST); /* UNKNOWN keeps the known background state. */
    sleep_ms(1200);
    assert(read_active_seconds(6) == after_background);

    write_device_state(0, reader, "eink-reader.app");
    sleep_ms(2200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(6) > after_background);
    assert(read_active_rows(6) == 3);
    kill_and_wait(reader);
    puts("  ok");
}

/* Without a handler marker the fallback is deliberately fail-open, except for
 * the launcher and Better Stats itself. */
static void test_untracked_app_exclusions(void)
{
    puts("test_untracked_app_exclusions...");
    clean_state();
    write_explorer(7, 7000);
    write_device_state(0, getpid(), "bookshelf.app");
    pid_t daemon = start_daemon();
    sleep_ms(1200);
    assert(read_active_seconds(7) == 0);
    write_device_state(0, getpid(), "koreader.app");
    sleep_ms(2200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(7) >= 1);
    puts("  ok");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--daemon") == 0)
        return run_daemon();

    test_argv0 = argv[0];
    setbuf(stdout, NULL);
    mkdir(TEST_DIR, 0755);
    mkdir(PROC_DIR, 0755);
    assert(setenv("BETTERSTATS_DB", STATS_DB_TEST, 1) == 0);
    assert(setenv("BETTERSTATS_EXPLORER_DB", EXPLORER_DB_TEST, 1) == 0);

    test_shutdown();
    test_reader_tracking();
    test_quick_unhandled_switch();
    test_stale_session_at_restart();
    test_pid_waits_for_firmware_session();
    test_dead_before_adoption();
    test_restart_immediately_after_reader_death();
    test_persisted_freeze();
    test_device_state_gates();
    test_untracked_app_exclusions();

    puts("all daemon tests ok");
    return 0;
}
