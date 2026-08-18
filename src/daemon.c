#include "daemon.h"
#include "tracker.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void on_term(int sig)
{
    (void)sig;
    running = 0;
}

const char *stats_db_path(void)
{
    const char *p = getenv("BETTERSTATS_DB");
    return p ? p : STATS_DB;
}

const char *explorer_db_path(void)
{
    const char *p = getenv("BETTERSTATS_EXPLORER_DB");
    return p ? p : EXPLORER_DB;
}

/* 1 if a daemon is already running. */
static int daemon_alive(void)
{
    FILE *f = fopen(PIDFILE, "r");
    if (!f)
        return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1)
        pid = 0;
    fclose(f);
    return pid > 0 && kill(pid, 0) == 0;
}

static void write_pidfile(void)
{
    FILE *f = fopen(PIDFILE, "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

int run_daemon(void)
{
    if (daemon_alive())
        return 0;
    mkdir(STATS_DIR, 0755);
    write_pidfile();
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    tracker t;
    if (tracker_init(&t, stats_db_path(), explorer_db_path()) != 0)
        return 1;
    tracker_recover(&t);
    while (running) {
        pb_state s;
        if (tracker_read_state(t.explorer_path, &s) == 0)
            tracker_observe(&t, &s);
        for (int i = 0; i < POLL_SECONDS && running; i++)
            sleep(1);
    }
    tracker_close(&t);
    unlink(PIDFILE);
    return 0;
}

void spawn_daemon(const char *self)
{
    if (daemon_alive())
        return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(self, self, "--daemon", (char *)NULL);
        _exit(1);
    }
}
