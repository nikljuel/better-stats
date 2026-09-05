#include "daemon.h"
#include "daemon_singleton.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
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
#define EXPLORER_DB_BACKUP TEST_DIR "/explorer.backup.db"
#define DEVICE_STATE_TEST TEST_DIR "/device.state"
#define CURRENT_BOOK_TEST TEST_DIR "/current"
#define BOOK_DIR "/books"

static char *test_argv0;
static pid_t expected_daemon;

int64_t daemon_test_interval_credit(int previous, int current,
                                    int64_t boot_delta, int64_t mono_delta);
int daemon_test_boottime_fallback(void);
int daemon_test_safe_wall_split(int64_t before_wall, int64_t after_wall,
                                int64_t opened, int64_t boot_delta,
                                int64_t mono_delta, int64_t *before_credit,
                                int64_t *after_credit);
void daemon_test_switch_split(int previous, int current,
                              int old_continuous,
                              int64_t before_wall, int64_t after_wall,
                              int64_t opened, int64_t boot_delta,
                              int64_t mono_delta, int64_t *before_credit,
                              int64_t *after_credit);

/* The singleton implementation has its own exhaustive host test. Keeping this
 * integration stub tiny lets the tracking suite use its synthetic /proc tree. */
int daemon_claim(int *lockfd)
{
    int fd = open(PIDFILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    char line[32];
    int n = snprintf(line, sizeof(line), "%d\n", (int)getpid());
    if (n <= 0 || write(fd, line, (size_t)n) != n) {
        close(fd);
        return -1;
    }
    *lockfd = fd;
    return 1;
}

void daemon_release(int lockfd)
{
    if (lockfd >= 0)
        close(lockfd);
    unlink(PIDFILE);
}

int stop_daemon(void)
{
    int pid = (int)expected_daemon;
    if (pid <= 0)
        return 0;
    if (kill((pid_t)pid, SIGTERM) != 0 && errno != ESRCH)
        return -1;
    for (int i = 0; i < 100 && access(PIDFILE, F_OK) == 0; ++i)
        usleep(10 * 1000);
    return access(PIDFILE, F_OK) == 0 ? -1 : 0;
}

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

static unsigned long long proc_start_for(pid_t pid)
{
    return (unsigned long long)(unsigned int)pid * 100ULL + 7ULL;
}

static void write_proc_stat_ticks(pid_t pid, char state,
                                  unsigned long long ticks)
{
    char directory[128], path[160];
    snprintf(directory, sizeof(directory), PROC_DIR "/%d", (int)pid);
    assert(mkdir(directory, 0755) == 0 || errno == EEXIST);
    snprintf(path, sizeof(path), "%s/stat", directory);
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    fprintf(file, "%d (reader test) %c", (int)pid, state);
    for (int field = 4; field < 22; ++field)
        fprintf(file, " 0");
    fprintf(file, " %llu 0\n", ticks);
    assert(fclose(file) == 0);
}

static void write_proc_stat(pid_t pid, char state)
{
    write_proc_stat_ticks(pid, state, proc_start_for(pid));
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
    unlink(EXPLORER_DB_BACKUP);
    unlink(READER_PIDFILE);
    unlink(READER_SESSION);
    unlink(DEVICE_STATE_TEST);
    unlink(CURRENT_BOOK_TEST);
}

static void write_explorer(int bookid, int opentime)
{
    unlink(EXPLORER_DB_TEST);
    sqlite3 *db = NULL;
    assert(sqlite3_open(EXPLORER_DB_TEST, &db) == SQLITE_OK);
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "CREATE TABLE books_impl(id INTEGER PRIMARY KEY,title TEXT,author TEXT);"
        "CREATE TABLE folders(id INTEGER PRIMARY KEY,name TEXT);"
        "CREATE TABLE files(book_id INTEGER,storageid INTEGER,fast_hash BLOB,"
        " folder_id INTEGER,filename TEXT);"
        "CREATE TABLE books_settings(bookid INTEGER,profileid INTEGER,"
        " position TEXT,position_ts INTEGER,cpage INTEGER,npage INTEGER,"
        " opentime INTEGER,completed INTEGER,completed_ts INTEGER);"
        "INSERT INTO folders VALUES(1,'" BOOK_DIR "');"
        "INSERT INTO files VALUES(%d,1,X'01',1,'book%d.epub');"
        "INSERT INTO books_impl VALUES(%d,'Test','Author');"
        "INSERT INTO books_settings VALUES(%d,1,'p',%d,1,100,%d,0,0);",
        bookid, bookid, bookid, bookid, opentime, opentime);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static void update_explorer(int bookid, int opentime)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(EXPLORER_DB_TEST, &db) == SQLITE_OK);
    sqlite3_busy_timeout(db, 2000);
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO books_impl VALUES(%d,'Test%d','Author');"
        "INSERT OR REPLACE INTO files VALUES(%d,1,X'01',1,'book%d.epub');"
        "DELETE FROM books_settings;"
        "INSERT INTO books_settings VALUES(%d,1,'p',%d,1,100,%d,0,0);",
        bookid, bookid, bookid, bookid, bookid, opentime, opentime);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static void update_position(int bookid, int position, int page)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(EXPLORER_DB_TEST, &db) == SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE books_settings SET position_ts=%d,cpage=%d"
             " WHERE bookid=%d", position, page, bookid);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static void add_explorer_book(int bookid, int opentime)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(EXPLORER_DB_TEST, &db) == SQLITE_OK);
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO books_impl VALUES(%d,'Test%d','Author');"
        "INSERT INTO files VALUES(%d,1,X'01',1,'book%d.epub');"
        "INSERT INTO books_settings VALUES(%d,1,'p',%d,1,100,%d,0,0);",
        bookid, bookid, bookid, bookid, bookid, opentime, opentime);
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

static int64_t read_max_end(int bookid)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &db) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT IFNULL(MAX(end_time),0) FROM sessions WHERE book_id=?1", -1,
        &st, NULL) == SQLITE_OK);
    sqlite3_bind_int(st, 1, bookid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    assert(sqlite3_close(db) == SQLITE_OK);
    return value;
}

static void write_reader_marker_ticks(pid_t pid, int64_t started_at,
                                      int bookid, unsigned long long ticks)
{
    FILE *f = fopen(READER_PIDFILE, "w");
    assert(f != NULL);
    fprintf(f, "%d %lld %llu\n%s/book%d.epub\n", (int)pid,
            (long long)started_at, ticks, BOOK_DIR, bookid);
    assert(fclose(f) == 0);
}

static void write_reader_marker(pid_t pid, int64_t started_at, int bookid)
{
    write_reader_marker_ticks(pid, started_at, bookid, proc_start_for(pid));
}

static void write_legacy_reader_marker(pid_t pid, int64_t started_at,
                                       int bookid)
{
    FILE *f = fopen(READER_PIDFILE, "w");
    assert(f != NULL);
    fprintf(f, "%d %lld\n%s/book%d.epub\n", (int)pid,
            (long long)started_at, BOOK_DIR, bookid);
    assert(fclose(f) == 0);
}

static void write_modern_reader_session(pid_t pid, int64_t started_at,
                                        int bookid, int opentime)
{
    FILE *f = fopen(READER_SESSION, "w");
    assert(f != NULL);
    fprintf(f, "%d %lld %llu %d %d\n", (int)pid,
            (long long)started_at, proc_start_for(pid), bookid, opentime);
    assert(fclose(f) == 0);
}

static void write_current_book(int bookid)
{
    FILE *f = fopen(CURRENT_BOOK_TEST, "w");
    assert(f != NULL);
    fprintf(f, "%s/book%d.epub\n", BOOK_DIR, bookid);
    assert(fclose(f) == 0);
}

static void assert_modern_marker(pid_t pid)
{
    FILE *f = fopen(READER_PIDFILE, "r");
    assert(f != NULL);
    int actual_pid = 0;
    long long started = 0;
    unsigned long long ticks = 0;
    assert(fscanf(f, "%d %lld %llu", &actual_pid, &started, &ticks) == 3);
    assert(fclose(f) == 0);
    assert(actual_pid == (int)pid);
    assert(ticks == proc_start_for(pid));
}

static void write_device_state_book(int locked, pid_t pid, const char *app,
                                    int bookid)
{
    FILE *f = fopen(DEVICE_STATE_TEST, "w");
    assert(f != NULL);
    fprintf(f, "%d %d %s\n", locked, (int)pid, app);
    if (bookid > 0)
        fprintf(f, "%s/book%d.epub\n", BOOK_DIR, bookid);
    assert(fclose(f) == 0);
}

static void write_device_state(int locked, pid_t pid, const char *app)
{
    write_device_state_book(locked, pid, app, 0);
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

static void sleep_ms(int ms);

static void assert_reader_session(int pid, int64_t bookid, int64_t opentime)
{
    FILE *f = fopen(READER_SESSION, "r");
    assert(f != NULL);
    int actual_pid = -1;
    long long marker_started = 0, actual_bookid = 0, actual_opentime = 0;
    unsigned long long ticks = 0;
    int fields = fscanf(f, "%d %lld %llu %lld %lld", &actual_pid,
                        &marker_started, &ticks,
                        &actual_bookid, &actual_opentime);
    if (fields != 5) {
        rewind(f);
        fields = fscanf(f, "%d %lld %lld", &actual_pid,
                        &actual_bookid, &actual_opentime);
    }
    assert(fields == 5 || fields == 3);
    assert(fclose(f) == 0);
    assert(actual_pid == pid);
    assert(actual_bookid == (long long)bookid);
    assert(actual_opentime == (long long)opentime);
    if (fields == 5 && pid > 0)
        assert(ticks > 0);
}

static void wait_reader_session(int pid, int64_t bookid, int64_t opentime)
{
    for (int i = 0; i < 30; ++i) {
        FILE *f = fopen(READER_SESSION, "r");
        int actual_pid = -1;
        long long started = 0, actual_bookid = 0, actual_opentime = 0;
        unsigned long long ticks = 0;
        int fields = f ? fscanf(f, "%d %lld %llu %lld %lld", &actual_pid,
                                &started, &ticks, &actual_bookid,
                                &actual_opentime) : 0;
        if (f)
            fclose(f);
        if ((fields == 5 && actual_pid == pid && actual_bookid == bookid
             && actual_opentime == opentime))
            return;
        sleep_ms(100);
    }
    assert_reader_session(pid, bookid, opentime);
}

static pid_t make_sleeping_child(void)
{
    pid_t p = fork();
    assert(p >= 0);
    if (p == 0) {
        pause();
        _exit(0);
    }
    write_proc_stat(p, 'S');
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
    expected_daemon = child;
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
    expected_daemon = 0;
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

static int64_t wait_active_at_least(int bookid, int64_t minimum,
                                    int timeout_ms)
{
    int64_t value = 0;
    for (int elapsed_ms = 0; elapsed_ms <= timeout_ms; elapsed_ms += 100) {
        value = read_active_seconds(bookid);
        if (value >= minimum)
            return value;
        sleep_ms(100);
    }
    return value;
}

static sqlite3 *lock_stats(void)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "BEGIN EXCLUSIVE", NULL, NULL, NULL) == SQLITE_OK);
    return db;
}

static void unlock_stats(sqlite3 *db)
{
    assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

/* BOOTTIME includes suspend while MONOTONIC does not on the target. Only an
 * interval whose two endpoints are known-reading may include that suspend. */
static void test_clock_accounting(void)
{
    puts("test_clock_accounting...");
    const int64_t second = 1000000000LL;
    assert(daemon_test_boottime_fallback());
    assert(daemon_test_interval_credit(2, 2, 10 * second, 2 * second)
           == 10 * second);
    assert(daemon_test_interval_credit(2, 1, 10 * second, 2 * second)
           == 2 * second);
    assert(daemon_test_interval_credit(2, 0, 10 * second, 2 * second)
           == 2 * second);
    assert(daemon_test_interval_credit(0, 2, 10 * second, 2 * second) == 0);

    int64_t before = -1, after = -1;
    assert(daemon_test_safe_wall_split(100 * second, 110 * second, 104,
                                       10 * second, 6 * second,
                                       &before, &after) == 1);
    assert(before == 4 * second && after == 6 * second);

    before = after = -1;
    assert(daemon_test_safe_wall_split(100 * second, 120 * second, 104,
                                       10 * second, 6 * second,
                                       &before, &after) == 0);
    assert(before == -1 && after == -1);
    assert(daemon_test_safe_wall_split(100 * second, 90 * second, 104,
                                       10 * second, 6 * second,
                                       &before, &after) == 0);

    /* An uncertain (MONOTONIC) endpoint may never turn one awake second into
     * two seconds merely because the new book opened after a suspend gap. */
    daemon_test_switch_split(1, 2, 0, 100 * second, 201 * second, 200,
                             101 * second, second, &before, &after);
    assert(before == second && after == 0);
    daemon_test_switch_split(2, 2, 1, 100 * second, 201 * second, 200,
                             101 * second, second, &before, &after);
    assert(before == 100 * second && after == second);
    daemon_test_switch_split(2, 2, 0, 100 * second, 201 * second, 200,
                             101 * second, second, &before, &after);
    assert(before == 0 && after == second);
    daemon_test_switch_split(2, 2, 0, 100 * second, 701 * second, 101,
                             601 * second, second, &before, &after);
    assert(before == 0 && after == 600 * second);
    puts("  ok");
}

/* Markerless tracking starts only after two stable full samples. */
static void test_shutdown(void)
{
    puts("test_shutdown...");
    clean_state();
    write_explorer(1, 1000);
    write_device_state_book(0, getpid(), "koreader.app", 1);

    pid_t daemon = start_daemon();
    sleep_ms(4500);
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
    write_reader_marker(reader_a, 1000, 1);
    write_device_state_book(0, reader_a, "eink-reader.app", 1);

    pid_t daemon = start_daemon();
    sleep_ms(2200);

    /* A new handler may overwrite the marker before death is detected. */
    pid_t reader_b = make_sleeping_child();
    write_reader_marker(reader_b, 1000, 1);
    write_device_state_book(0, reader_b, "eink-reader.app", 1);
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
    write_device_state_book(0, getpid(), "koreader.app", 2);
    sleep_ms(5500);

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
    write_reader_marker(reader, 1000, 1);
    write_device_state_book(0, reader, "eink-reader.app", 1);
    pid_t daemon = start_daemon();
    sleep_ms(2200);

    pid_t next = make_sleeping_child();
    update_explorer(2, 2000);
    write_reader_marker(next, 2000, 2);
    write_device_state(0, getpid(), "bookshelf.app");
    sleep_ms(1200);
    assert(read_pid(READER_PIDFILE) == (int)next);
    assert(read_active_seconds(2) == 0);

    write_device_state_book(0, next, "eink-reader.app", 2);
    sleep_ms(1200);
    assert_reader_session((int)next, 2, 2000);
    kill_and_wait(reader);
    kill_and_wait(next);
    sleep_ms(1200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(2) >= 1);
    puts("  ok");
}

static void test_debounced_switch_closes_at_new_open(void)
{
    puts("test_debounced_switch_closes_at_new_open...");
    clean_state();
    int opened_a = (int)time(NULL);
    write_explorer(41, opened_a);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, opened_a, 41);
    write_device_state_book(0, reader, "eink-reader.app", 41);
    pid_t daemon = start_daemon();
    wait_reader_session((int)reader, 41, opened_a);
    sleep_ms(2200);

    kill_and_wait(reader);
    unlink(READER_PIDFILE);
    sleep_ms(1200); /* daemon sees the death and freezes A */
    int opened_b = (int)time(NULL);
    update_explorer(42, opened_b);
    write_device_state_book(0, getpid(), "koreader.app", 42);
    sleep_ms(4200);
    stop_daemon_and_wait(daemon);
    assert(read_max_end(41) <= opened_b);
    puts("  ok");
}

/* A persisted session from another book must not rebind its PID at restart. */
static void test_stale_session_at_restart(void)
{
    puts("test_stale_session_at_restart...");
    clean_state();
    write_explorer(2, 2000);

    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 2000, 2);
    write_device_state_book(0, reader, "eink-reader.app", 2);
    FILE *f = fopen(READER_SESSION, "w");
    assert(f != NULL);
    fprintf(f, "%d 1 1000\n", (int)reader);
    assert(fclose(f) == 0);

    pid_t daemon = start_daemon();
    wait_reader_session((int)reader, 2, 2000);

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
    write_reader_marker(reader, 2000, 2);
    write_device_state_book(0, reader, "eink-reader.app", 2);
    pid_t daemon = start_daemon();
    sleep_ms((POLL_SECONDS + 1) * 1000);
    assert(access(READER_SESSION, F_OK) != 0);
    assert(read_pid(READER_PIDFILE) == (int)reader);

    update_explorer(2, 1998);
    sleep_ms(2200);
    assert_reader_session((int)reader, 2, 1998);
    assert(read_active_seconds(2) >= 1); /* includes pre-DB probe time */

    kill_and_wait(reader);
    sleep_ms((POLL_SECONDS + 1) * 1000);
    assert_reader_session(0, 2, 1998);
    stop_daemon_and_wait(daemon);
    puts("  ok");
}

/* A marker newer than the only same-book firmware row stays provisional. It
 * must not resume that stale row; once the matching row appears, buffered
 * MONOTONIC time belongs to the new session exactly once. */
static void test_wait_relation_stays_provisional(void)
{
    puts("test_wait_relation_stays_provisional...");
    clean_state();
    write_explorer(37, 37000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 37006, 37);
    write_device_state_book(0, reader, "eink-reader.app", 37);
    pid_t daemon = start_daemon();
    sleep_ms(3200);
    assert(access(READER_SESSION, F_OK) != 0);
    assert(read_active_seconds(37) == 0);
    assert(read_pid(READER_PIDFILE) == (int)reader);

    update_explorer(37, 37006);
    wait_reader_session((int)reader, 37, 37006);
    assert(read_active_seconds(37) >= 2);
    stop_daemon_and_wait(daemon);
    kill_and_wait(reader);
    puts("  ok");
}

static void test_live_marker_buffers_firmware_outage(void)
{
    puts("test_live_marker_buffers_firmware_outage...");
    clean_state();
    int opened = (int)time(NULL);
    write_explorer(40, opened);
    assert(rename(EXPLORER_DB_TEST, EXPLORER_DB_BACKUP) == 0);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, opened, 40);
    write_device_state_book(0, reader, "eink-reader.app", 40);
    pid_t daemon = start_daemon();
    sleep_ms(3200);
    assert(access(READER_SESSION, F_OK) != 0);
    write_device_state_book(1, reader, "eink-reader.app", 40);
    sleep_ms(1200); /* a lock pauses, but must not erase proven probe time */
    write_device_state_book(0, reader, "eink-reader.app", 40);
    assert(rename(EXPLORER_DB_BACKUP, EXPLORER_DB_TEST) == 0);
    wait_reader_session((int)reader, 40, opened);
    assert(read_active_seconds(40) >= 2);
    stop_daemon_and_wait(daemon);
    kill_and_wait(reader);
    puts("  ok");
}

static void test_dead_probe_flushes_when_firmware_returns(void)
{
    puts("test_dead_probe_flushes_when_firmware_returns...");
    clean_state();
    int opened = (int)time(NULL);
    write_explorer(43, opened);
    assert(rename(EXPLORER_DB_TEST, EXPLORER_DB_BACKUP) == 0);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, opened, 43);
    write_device_state_book(0, reader, "eink-reader.app", 43);
    pid_t daemon = start_daemon();
    sleep_ms(3200);

    assert(rename(EXPLORER_DB_BACKUP, EXPLORER_DB_TEST) == 0);
    kill_and_wait(reader);
    sleep_ms(2200);
    assert_reader_session(0, 43, opened);
    assert(read_active_seconds(43) >= 2);
    stop_daemon_and_wait(daemon);
    puts("  ok");
}

/* A changed snapshot persists the sub-checkpoint tail in the same tracker
 * transaction; a crash immediately after the page turn cannot lose it. */
static void test_page_change_flushes_short_tail(void)
{
    puts("test_page_change_flushes_short_tail...");
    clean_state();
    write_explorer(38, 38000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 38000, 38);
    write_device_state_book(0, reader, "eink-reader.app", 38);
    pid_t daemon = start_daemon();
    wait_reader_session((int)reader, 38, 38000);
    update_position(38, 38001, 2);
    sleep_ms(1200);
    assert(read_active_seconds(38) >= 1);
    stop_daemon_and_wait(daemon);
    kill_and_wait(reader);
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
    write_reader_marker(reader, 4000, 4);

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
    write_reader_marker(reader, 5000, 5);
    write_device_state_book(0, reader, "eink-reader.app", 5);
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
    write_reader_marker(reader, 6000, 6);
    write_device_state_book(0, reader, "eink-reader.app", 6);
    pid_t daemon = start_daemon();

    sleep_ms(2200);
    int64_t before_unknown = read_active_seconds(6);
    unlink(DEVICE_STATE_TEST); /* last known FG continues, conservatively MONO */
    int64_t after_unknown = wait_active_at_least(6, before_unknown + 1, 4000);
    assert(after_unknown > before_unknown);

    write_device_state_book(0, reader, "eink-reader.app", 6);
    sleep_ms(1200);
    write_device_state_book(1, reader, "eink-reader.app", 6);
    sleep_ms(2200); /* include the one conservative tail at the lock edge */
    int64_t after_lock = read_active_seconds(6);
    assert(after_lock > after_unknown);
    sleep_ms(1200);
    assert(read_active_seconds(6) == after_lock);

    write_device_state_book(0, reader, "eink-reader.app", 6);
    sleep_ms(1500);
    write_device_state(0, getpid(), "bookshelf.app");
    int64_t after_background = wait_active_at_least(6, after_lock + 1, 3000);
    assert(after_background > after_lock);

    unlink(DEVICE_STATE_TEST); /* UNKNOWN keeps the known background state. */
    sleep_ms(1200);
    assert(read_active_seconds(6) == after_background);

    write_device_state_book(0, reader, "eink-reader.app", 6);
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
    write_device_state_book(0, getpid(), "koreader.app", 7);
    sleep_ms(3500);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(7) >= 1);
    puts("  ok");
}

static void test_fallback_app_change_starts_new_fragment(void)
{
    puts("test_fallback_app_change_starts_new_fragment...");
    clean_state();
    write_explorer(24, 24000);
    write_device_state_book(0, getpid(), "koreader.app", 24);
    pid_t daemon = start_daemon();
    sleep_ms(3200);
    write_device_state_book(0, getpid(), "other-reader.app", 24);
    sleep_ms(3500);
    stop_daemon_and_wait(daemon);
    assert(read_active_rows(24) == 2);
    puts("  ok");
}

/* Some firmware builds expose the active book only through /tmp/.current.
 * FindTaskByBook still has to prove that it belongs to the active task. */
static void test_owned_current_book_fallback(void)
{
    puts("test_owned_current_book_fallback...");
    clean_state();
    write_explorer(18, 18000);
    write_current_book(18);
    write_device_state(0, getpid(), "koreader.app");
    pid_t daemon = start_daemon();
    sleep_ms(3500);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(18) >= 1);
    puts("  ok");
}

/* A legacy marker is made generation-safe only after its foreground book and
 * app have been proven. A launcher PID must never be promoted into a reader. */
static void test_legacy_marker_upgrade_is_context_bound(void)
{
    puts("test_legacy_marker_upgrade_is_context_bound...");
    clean_state();
    write_explorer(19, 19000);
    pid_t reader = make_sleeping_child();
    write_legacy_reader_marker(reader, 19000, 19);
    write_device_state_book(0, reader, "eink-reader.app", 19);
    pid_t daemon = start_daemon();
    sleep_ms(1200);
    assert_modern_marker(reader);
    wait_reader_session((int)reader, 19, 19000);
    sleep_ms(1200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(19) >= 1);
    kill_and_wait(reader);

    clean_state();
    write_explorer(20, 20000);
    pid_t launcher = make_sleeping_child();
    write_legacy_reader_marker(launcher, 20000, 20);
    write_device_state_book(0, launcher, "bookshelf.app", 20);
    daemon = start_daemon();
    sleep_ms(2200);
    stop_daemon_and_wait(daemon);
    FILE *f = fopen(READER_PIDFILE, "r");
    assert(f != NULL);
    char header[128];
    assert(fgets(header, sizeof(header), f) != NULL);
    assert(fclose(f) == 0);
    int pid = 0, consumed = 0;
    long long started = 0;
    assert(sscanf(header, "%d %lld %n", &pid, &started, &consumed) == 2);
    while (header[consumed] == '\n' || header[consumed] == '\r'
           || header[consumed] == ' ' || header[consumed] == '\t')
        consumed++;
    assert(header[consumed] == '\0');
    assert(access(READER_SESSION, F_OK) != 0);
    assert(read_active_seconds(20) == 0);
    kill_and_wait(launcher);
    puts("  ok");
}

/* Slow firmware may publish opentime more than five seconds after the handler
 * marker. Exact live foreground task/path evidence still identifies it. */
static void test_live_marker_survives_slow_firmware_open(void)
{
    puts("test_live_marker_survives_slow_firmware_open...");
    clean_state();
    write_explorer(32, 32007);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 32000, 32);
    write_device_state_book(0, reader, "eink-reader.app", 32);
    pid_t daemon = start_daemon();
    sleep_ms(2200);
    assert_reader_session((int)reader, 32, 32007);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(32) >= 1);
    kill_and_wait(reader);
    puts("  ok");
}

/* A reused PID is a different process when /proc starttime changes. It stops
 * this session without ever signalling the unrelated live process. */
static void test_reader_pid_generation_change(void)
{
    puts("test_reader_pid_generation_change...");
    clean_state();
    write_explorer(8, 8000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 8000, 8);
    write_device_state_book(0, reader, "eink-reader.app", 8);
    pid_t daemon = start_daemon();
    sleep_ms(2200);

    /* Replace only the synthetic generation; the real child intentionally lives. */
    write_proc_stat_ticks(reader, 'S', proc_start_for(reader) + 1);
    sleep_ms(2200);
    assert_reader_session(0, 8, 8000);
    assert(kill(reader, 0) == 0);
    int64_t frozen = read_active_seconds(8);
    sleep_ms(1200);
    assert(read_active_seconds(8) == frozen);
    stop_daemon_and_wait(daemon);
    kill_and_wait(reader);
    puts("  ok");
}

/* A live task may reuse the old marker's numeric PID. The /proc generation,
 * not the number alone, decides whether the dead marker is stale. */
static void test_dead_marker_pid_reuse_falls_back(void)
{
    puts("test_dead_marker_pid_reuse_falls_back...");
    clean_state();
    write_explorer(21, 21000);
    pid_t reader = make_sleeping_child();
    write_reader_marker_ticks(reader, 21000, 21,
                              proc_start_for(reader) - 1);
    write_device_state_book(0, reader, "koreader.app", 21);
    pid_t daemon = start_daemon();
    sleep_ms(3500);
    assert(access(READER_PIDFILE, F_OK) != 0);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(21) >= 1);
    kill_and_wait(reader);
    puts("  ok");
}

/* Conflicting trusted handler/task paths are UNKNOWN. One consistent sample
 * clears the conflict, then normal handler tracking starts from a baseline. */
static void test_trusted_book_conflict(void)
{
    puts("test_trusted_book_conflict...");
    clean_state();
    write_explorer(9, 9000);
    add_explorer_book(10, 10000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 9000, 9);
    write_device_state_book(0, reader, "eink-reader.app", 10);
    pid_t daemon = start_daemon();
    sleep_ms(2200);
    assert(access(READER_SESSION, F_OK) != 0);
    assert(read_active_seconds(9) == 0);
    assert(read_active_seconds(10) == 0);

    write_device_state_book(0, reader, "eink-reader.app", 9);
    sleep_ms(2200);
    assert_reader_session((int)reader, 9, 9000);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(9) >= 1);
    kill_and_wait(reader);
    puts("  ok");
}

/* A handler marker may appear after markerless fallback already opened the
 * exact same firmware session. Binding the PID must preserve its pending tail
 * and must not try to resume an unflushed open fragment. */
static void test_fallback_binds_same_handler_session(void)
{
    puts("test_fallback_binds_same_handler_session...");
    clean_state();
    write_explorer(11, 11000);
    write_device_state_book(0, getpid(), "koreader.app", 11);
    pid_t daemon = start_daemon();
    sleep_ms(3200);

    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 11000, 11);
    write_device_state_book(0, reader, "eink-reader.app", 11);
    for (int i = 0; i < 30 && access(READER_SESSION, F_OK) != 0; ++i)
        sleep_ms(100);
    assert_reader_session((int)reader, 11, 11000);
    assert(kill(daemon, 0) == 0);

    kill_and_wait(reader);
    sleep_ms(2200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(11) >= 2);
    puts("  ok");
}

/* Switching from an open markerless fragment to another handler book must
 * flush A before tracker_prepare resets the in-memory session for B. */
static void test_fallback_switches_to_handler_book(void)
{
    puts("test_fallback_switches_to_handler_book...");
    clean_state();
    int opened = (int)time(NULL);
    write_explorer(12, opened);
    write_device_state_book(0, getpid(), "koreader.app", 12);
    pid_t daemon = start_daemon();
    sleep_ms(3200);

    pid_t reader = make_sleeping_child();
    int next_open = (int)time(NULL);
    update_explorer(13, next_open);
    write_reader_marker(reader, next_open, 13);
    write_device_state_book(0, reader, "eink-reader.app", 13);
    for (int i = 0; i < 30; ++i) {
        if (access(READER_SESSION, F_OK) == 0)
            break;
        sleep_ms(100);
    }
    assert_reader_session((int)reader, 13, next_open);
    assert(read_active_seconds(12) >= 1);

    sleep_ms(1200);
    kill_and_wait(reader);
    sleep_ms(2200);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(13) >= 1);
    puts("  ok");
}

/* A dead marker for another (or not-yet-visible) handler launch cannot freeze
 * an exact, living markerless task selected by the firmware task API. */
static void run_dead_marker_fallback_case(int marker_book, int book,
                                          int marker_started, int opentime)
{
    clean_state();
    write_explorer(book, opentime);
    if (marker_book != book)
        add_explorer_book(marker_book, opentime);
    pid_t stale = make_sleeping_child();
    kill_and_wait(stale);
    write_reader_marker(stale, marker_started, marker_book);
    write_device_state_book(0, getpid(), "koreader.app", book);

    pid_t daemon = start_daemon();
    sleep_ms(3500);
    assert(access(READER_PIDFILE, F_OK) != 0);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(book) >= 1);
}

static void test_dead_marker_does_not_block_fallback(void)
{
    puts("test_dead_marker_does_not_block_fallback...");
    run_dead_marker_fallback_case(14, 15, 15000, 15000);
    run_dead_marker_fallback_case(16, 16, 16100, 16000);
    puts("  ok");
}

/* During a transient explorer-DB outage a bound reader intentionally counts
 * only MONOTONIC time. Its 60-second production checkpoint must still run;
 * this test lowers that existing threshold to two seconds. */
static void test_checkpoint_during_firmware_gap(void)
{
    puts("test_checkpoint_during_firmware_gap...");
    clean_state();
    write_explorer(17, 17000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 17000, 17);
    write_device_state_book(0, reader, "eink-reader.app", 17);
    pid_t daemon = start_daemon();
    sleep_ms(1200);

    int64_t before = read_active_seconds(17);
    assert(rename(EXPLORER_DB_TEST, EXPLORER_DB_BACKUP) == 0);
    sleep_ms(2500); /* the exact bound marker keeps the MONOTONIC gap path */
    assert(read_active_seconds(17) >= before + 1);
    pid_t stale = make_sleeping_child();
    kill_and_wait(stale);
    write_reader_marker(stale, 17999, 99);
    sleep_ms(2500);
    assert(read_active_seconds(17) >= 2);
    assert(access(READER_PIDFILE, F_OK) != 0);
    assert(rename(EXPLORER_DB_BACKUP, EXPLORER_DB_TEST) == 0);

    kill_and_wait(reader);
    sleep_ms(2200);
    stop_daemon_and_wait(daemon);
    puts("  ok");
}

static void test_fallback_continues_during_firmware_gap(void)
{
    puts("test_fallback_continues_during_firmware_gap...");
    clean_state();
    write_explorer(22, 22000);
    write_device_state_book(0, getpid(), "koreader.app", 22);
    pid_t daemon = start_daemon();
    sleep_ms(3200);
    int64_t before = read_active_seconds(22);

    assert(rename(EXPLORER_DB_TEST, EXPLORER_DB_BACKUP) == 0);
    pid_t stale = make_sleeping_child();
    kill_and_wait(stale);
    write_reader_marker(stale, 22999, 99);
    sleep_ms(3500);
    int64_t during = read_active_seconds(22);
    assert(during >= before + 2);
    assert(access(READER_PIDFILE, F_OK) != 0);
    assert(kill(daemon, 0) == 0);
    assert(rename(EXPLORER_DB_BACKUP, EXPLORER_DB_TEST) == 0);

    sleep_ms(1200);
    stop_daemon_and_wait(daemon);
    puts("  ok");
}

static void test_fallback_pid_generation_change(void)
{
    puts("test_fallback_pid_generation_change...");
    clean_state();
    write_proc_stat(getpid(), 'S');
    write_explorer(39, 39000);
    write_device_state_book(0, getpid(), "koreader.app", 39);
    pid_t daemon = start_daemon();
    sleep_ms(3500);
    assert(read_active_rows(39) == 1);

    write_proc_stat_ticks(getpid(), 'S', proc_start_for(getpid()) + 1);
    sleep_ms(3500);
    stop_daemon_and_wait(daemon);
    assert(read_active_rows(39) == 2);
    write_proc_stat(getpid(), 'S');
    puts("  ok");
}

static void test_stats_lock_is_retryable(void)
{
    puts("test_stats_lock_is_retryable...");
    clean_state();
    write_explorer(23, 23000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 23000, 23);
    write_device_state_book(0, reader, "eink-reader.app", 23);
    pid_t daemon = start_daemon();
    sleep_ms(2200);
    int64_t before = read_active_seconds(23);

    sqlite3 *locker = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &locker) == SQLITE_OK);
    sqlite3_busy_timeout(locker, 3000);
    assert(sqlite3_exec(locker, "BEGIN EXCLUSIVE", NULL, NULL, NULL)
           == SQLITE_OK);
    sleep_ms(4500); /* exceeds the daemon connection's two-second timeout */
    assert(kill(daemon, 0) == 0);
    assert(sqlite3_exec(locker, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(locker) == SQLITE_OK);

    sleep_ms(3500);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(23) >= before + 6);
    kill_and_wait(reader);
    puts("  ok");
}

/* Preparing, resuming, and switching all use the same retry-safe open path.
 * Time proven safe while our DB is locked must survive each lifecycle edge. */
static void test_pending_lifecycle_preserves_locked_time(void)
{
    puts("test_pending_lifecycle_preserves_locked_time...");

    clean_state();
    int opened = (int)time(NULL);
    write_explorer(33, opened);
    write_device_state(0, getpid(), "bookshelf.app");
    pid_t daemon = start_daemon();
    sleep_ms(1200);
    sqlite3 *locker = lock_stats();
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, opened, 33);
    write_device_state_book(0, reader, "eink-reader.app", 33);
    sleep_ms(4500);
    unlock_stats(locker);
    sleep_ms(1300);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(33) >= 3);
    kill_and_wait(reader);

    clean_state();
    opened = (int)time(NULL);
    write_explorer(34, opened);
    reader = make_sleeping_child();
    write_reader_marker(reader, opened, 34);
    write_device_state_book(0, reader, "eink-reader.app", 34);
    daemon = start_daemon();
    sleep_ms(3200);
    write_device_state_book(1, reader, "eink-reader.app", 34);
    sleep_ms(2200);
    int64_t before_resume = read_active_seconds(34);
    locker = lock_stats();
    write_device_state_book(0, reader, "eink-reader.app", 34);
    sleep_ms(4500);
    unlock_stats(locker);
    sleep_ms(1300);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(34) >= before_resume + 3);
    kill_and_wait(reader);

    clean_state();
    opened = (int)time(NULL);
    write_explorer(35, opened);
    pid_t reader_a = make_sleeping_child();
    write_reader_marker(reader_a, opened, 35);
    write_device_state_book(0, reader_a, "eink-reader.app", 35);
    daemon = start_daemon();
    sleep_ms(3200);
    locker = lock_stats();
    pid_t reader_b = make_sleeping_child();
    int next_open = (int)time(NULL);
    update_explorer(36, next_open);
    write_reader_marker(reader_b, next_open, 36);
    write_device_state_book(0, reader_b, "eink-reader.app", 36);
    sleep_ms(4500);
    unlock_stats(locker);
    sleep_ms(1300);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(36) >= 3);
    kill_and_wait(reader_a);
    kill_and_wait(reader_b);
    puts("  ok");
}

/* A dead persisted reader for A is resolved before a trusted live task for B;
 * otherwise the two paths remain in conflict forever after a restart. */
static void test_dead_restore_does_not_block_new_task(void)
{
    puts("test_dead_restore_does_not_block_new_task...");
    clean_state();
    write_explorer(26, 26000);
    add_explorer_book(27, 27000);
    pid_t dead = make_sleeping_child();
    write_reader_marker(dead, 26000, 26);
    write_modern_reader_session(dead, 26000, 26, 26000);
    kill_and_wait(dead);
    write_device_state_book(0, getpid(), "koreader.app", 27);

    pid_t daemon = start_daemon();
    sleep_ms(7200);
    assert(access(READER_PIDFILE, F_OK) != 0);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(27) >= 1);
    puts("  ok");
}

/* A fresh valid marker is stronger than a stale bound reader even while the
 * explorer DB is unavailable and the active task cannot be sampled. */
static void test_new_marker_stops_old_reader_during_firmware_gap(void)
{
    puts("test_new_marker_stops_old_reader_during_firmware_gap...");
    clean_state();
    write_explorer(28, 28000);
    pid_t old_reader = make_sleeping_child();
    write_reader_marker(old_reader, 28000, 28);
    write_device_state_book(0, old_reader, "eink-reader.app", 28);
    pid_t daemon = start_daemon();
    sleep_ms(3200);

    assert(rename(EXPLORER_DB_TEST, EXPLORER_DB_BACKUP) == 0);
    pid_t new_reader = make_sleeping_child();
    write_reader_marker(new_reader, 29000, 29);
    unlink(DEVICE_STATE_TEST);
    sleep_ms(2500);
    int64_t stopped = read_active_seconds(28);
    sleep_ms(2500);
    assert(read_active_seconds(28) == stopped);
    assert(kill(daemon, 0) == 0);

    assert(rename(EXPLORER_DB_BACKUP, EXPLORER_DB_TEST) == 0);
    stop_daemon_and_wait(daemon);
    kill_and_wait(old_reader);
    kill_and_wait(new_reader);
    puts("  ok");
}

/* A transient lock during initial schema setup must delay startup rather than
 * leave an installed daemon absent. */
static void test_stats_lock_at_startup_is_retryable(void)
{
    puts("test_stats_lock_at_startup_is_retryable...");
    clean_state();
    write_explorer(30, 30000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 30000, 30);
    write_device_state_book(0, reader, "eink-reader.app", 30);

    sqlite3 *locker = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &locker) == SQLITE_OK);
    assert(sqlite3_exec(locker, "BEGIN EXCLUSIVE", NULL, NULL, NULL)
           == SQLITE_OK);
    pid_t daemon = start_daemon();
    sleep_ms(2600);
    assert(kill(daemon, 0) == 0);
    assert(access(PIDFILE, F_OK) == 0);
    assert(sqlite3_exec(locker, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(locker) == SQLITE_OK);

    sleep_ms(4500);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(30) >= 1);
    kill_and_wait(reader);
    puts("  ok");
}

/* If a terminal flush remains locked past its retry budget, the daemon resumes
 * serving. A failed updater stop can therefore never create a daemon gap. */
static void test_terminal_flush_timeout_resumes_daemon(void)
{
    puts("test_terminal_flush_timeout_resumes_daemon...");
    clean_state();
    write_explorer(31, 31000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 31000, 31);
    write_device_state_book(0, reader, "eink-reader.app", 31);
    pid_t daemon = start_daemon();
    sleep_ms(1200);

    sqlite3 *locker = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &locker) == SQLITE_OK);
    assert(sqlite3_exec(locker, "BEGIN EXCLUSIVE", NULL, NULL, NULL)
           == SQLITE_OK);
    assert(kill(daemon, SIGTERM) == 0);
    sleep_ms(5200);
    assert(kill(daemon, 0) == 0);
    assert(access(PIDFILE, F_OK) == 0);
    assert(sqlite3_exec(locker, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(locker) == SQLITE_OK);

    sleep_ms(3200);
    assert(kill(daemon, 0) == 0);
    stop_daemon_and_wait(daemon);
    assert(read_active_seconds(31) >= 1);
    kill_and_wait(reader);
    puts("  ok");
}

static void test_terminal_flush_waits_for_stats_unlock(void)
{
    puts("test_terminal_flush_waits_for_stats_unlock...");
    clean_state();
    write_explorer(25, 25000);
    pid_t reader = make_sleeping_child();
    write_reader_marker(reader, 25000, 25);
    write_device_state_book(0, reader, "eink-reader.app", 25);
    pid_t daemon = start_daemon();
    sleep_ms(1200); /* one pending second, below the test checkpoint */

    sqlite3 *locker = NULL;
    assert(sqlite3_open(STATS_DB_TEST, &locker) == SQLITE_OK);
    sqlite3_busy_timeout(locker, 3000);
    assert(sqlite3_exec(locker, "BEGIN EXCLUSIVE", NULL, NULL, NULL)
           == SQLITE_OK);
    assert(kill(daemon, SIGTERM) == 0);
    sleep_ms(2500);
    int status = 0;
    assert(waitpid(daemon, &status, WNOHANG) == 0);

    assert(sqlite3_exec(locker, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(locker) == SQLITE_OK);
    pid_t waited = 0;
    for (int i = 0; i < 60 && waited == 0; ++i) {
        waited = waitpid(daemon, &status, WNOHANG);
        sleep_ms(100);
    }
    assert(waited == daemon);
    expected_daemon = 0;
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(access(PIDFILE, F_OK) != 0);
    assert(read_active_seconds(25) >= 1);
    kill_and_wait(reader);
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
    write_proc_stat(getpid(), 'S');
    assert(setenv("BETTERSTATS_DB", STATS_DB_TEST, 1) == 0);
    assert(setenv("BETTERSTATS_EXPLORER_DB", EXPLORER_DB_TEST, 1) == 0);

    test_clock_accounting();
    test_shutdown();
    test_reader_tracking();
    test_quick_unhandled_switch();
    test_debounced_switch_closes_at_new_open();
    test_stale_session_at_restart();
    test_pid_waits_for_firmware_session();
    test_wait_relation_stays_provisional();
    test_live_marker_buffers_firmware_outage();
    test_dead_probe_flushes_when_firmware_returns();
    test_page_change_flushes_short_tail();
    test_dead_before_adoption();
    test_restart_immediately_after_reader_death();
    test_persisted_freeze();
    test_device_state_gates();
    test_untracked_app_exclusions();
    test_fallback_app_change_starts_new_fragment();
    test_owned_current_book_fallback();
    test_legacy_marker_upgrade_is_context_bound();
    test_live_marker_survives_slow_firmware_open();
    test_reader_pid_generation_change();
    test_dead_marker_pid_reuse_falls_back();
    test_trusted_book_conflict();
    test_fallback_binds_same_handler_session();
    test_fallback_switches_to_handler_book();
    test_dead_marker_does_not_block_fallback();
    test_checkpoint_during_firmware_gap();
    test_fallback_continues_during_firmware_gap();
    test_fallback_pid_generation_change();
    test_stats_lock_is_retryable();
    test_pending_lifecycle_preserves_locked_time();
    test_terminal_flush_waits_for_stats_unlock();
    test_dead_restore_does_not_block_new_task();
    test_new_marker_stops_old_reader_during_firmware_gap();
    test_stats_lock_at_startup_is_retryable();
    test_terminal_flush_timeout_resumes_daemon();

    puts("all daemon tests ok");
    return 0;
}
