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

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--daemon") == 0)
        return run_daemon();

    mkdir(TEST_DIR, 0755);
    mkdir(PROC_DIR, 0755);
    unlink(PIDFILE);
    unlink(LEGACY_PIDFILE);
    unlink(STATS_DB_TEST);
    unlink(EXPLORER_DB_TEST);
    sqlite3 *db = NULL;
    assert(sqlite3_open(EXPLORER_DB_TEST, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE books_impl(id INTEGER PRIMARY KEY,title TEXT,author TEXT);"
        "CREATE TABLE files(book_id INTEGER,storageid INTEGER,fast_hash BLOB);"
        "CREATE TABLE books_settings(bookid INTEGER,profileid INTEGER,"
        " position TEXT,position_ts INTEGER,cpage INTEGER,npage INTEGER,"
        " opentime INTEGER,completed INTEGER,completed_ts INTEGER);"
        "INSERT INTO books_impl VALUES(1,'Shutdown','Test');"
        "INSERT INTO books_settings VALUES(1,1,'p',1000,1,100,1000,0,0);",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(setenv("BETTERSTATS_DB", STATS_DB_TEST, 1) == 0);
    assert(setenv("BETTERSTATS_EXPLORER_DB", EXPLORER_DB_TEST, 1) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(argv[0], argv[0], "--daemon", (char *)NULL);
        _exit(127);
    }
    write_cmdline(child, argv[0]);

    for (int i = 0; i < 200 && access(PIDFILE, F_OK) != 0; ++i)
        usleep(10 * 1000);
    assert(access(PIDFILE, F_OK) == 0);
    const struct timespec awake = {1, 200 * 1000 * 1000};
    nanosleep(&awake, NULL); /* Leave one full second for the shutdown flush. */

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

    assert(sqlite3_open(STATS_DB_TEST, &db) == SQLITE_OK);
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT active_seconds FROM sessions WHERE book_id=1", -1,
        &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    assert(sqlite3_column_int64(statement, 0) >= 1);
    sqlite3_finalize(statement);
    assert(sqlite3_close(db) == SQLITE_OK);

    puts("all daemon tests ok");
    return 0;
}
