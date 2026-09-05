#define _GNU_SOURCE
#include "daemon_singleton.h"

#include <assert.h>
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
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char root[PATH_MAX];
static char primary[PATH_MAX];
static char legacy[PATH_MAX];
static char proc_root[PATH_MAX];
static char trusted_current[PATH_MAX];
static char trusted_historical[PATH_MAX];
static volatile sig_atomic_t child_should_exit;
static pid_t hook_pid;
static uint64_t hook_ticks;
static int hook_once;
static int hook_replace_primary;
static int hook_publish_snapshot;

static void remove_tree(const char *path);

static void join(char *out, size_t cap, const char *a, const char *b)
{
    int n = snprintf(out, cap, "%s/%s", a, b);
    assert(n > 0 && (size_t)n < cap);
}

static void mkdir_p(const char *path)
{
    char copy[PATH_MAX];
    size_t len = strlen(path);
    assert(len > 0 && len < sizeof(copy));
    memcpy(copy, path, len + 1);
    for (char *p = copy + 1; *p; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        assert(mkdir(copy, 0755) == 0 || errno == EEXIST);
        *p = '/';
    }
    assert(mkdir(copy, 0755) == 0 || errno == EEXIST);
}

static void write_bytes(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    const char *p = data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        assert(n > 0);
        done += (size_t)n;
    }
    assert(close(fd) == 0);
}

static void write_text(const char *path, const char *text)
{
    write_bytes(path, text, strlen(text));
}

static int read_pid(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int pid = 0;
    int ok = fscanf(f, "%d", &pid) == 1;
    fclose(f);
    return ok ? pid : 0;
}

static int read_pid_fd(int fd)
{
    char buf[32];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    assert(n > 0 && n < (ssize_t)sizeof(buf));
    buf[n] = '\0';
    return atoi(buf);
}

static void write_pid(const char *path, pid_t pid)
{
    char line[32];
    int n = snprintf(line, sizeof(line), "%d\n", (int)pid);
    assert(n > 0 && (size_t)n < sizeof(line));
    write_bytes(path, line, (size_t)n);
}

static void write_fake_stat(pid_t pid, uint64_t ticks, char state)
{
    char dir[PATH_MAX], path[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%d", proc_root, (int)pid);
    assert(n > 0 && (size_t)n < sizeof(dir));
    mkdir_p(dir);
    join(path, sizeof(path), dir, "stat");
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fprintf(f, "%d (test daemon) %c", (int)pid, state) > 0);
    for (int field = 4; field <= 21; ++field)
        assert(fprintf(f, " 0") > 0);
    assert(fprintf(f, " %llu 0\n", (unsigned long long)ticks) > 0);
    assert(fclose(f) == 0);
}

static void create_fake_process(pid_t pid, const char *exe, uint64_t ticks)
{
    char dir[PATH_MAX], path[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%d", proc_root, (int)pid);
    assert(n > 0 && (size_t)n < sizeof(dir));
    mkdir_p(dir);
    write_fake_stat(pid, ticks, 'S');

    join(path, sizeof(path), dir, "cmdline");
    char cmdline[PATH_MAX + 32];
    size_t exe_len = strlen(exe);
    static const char daemon_arg[] = "--daemon";
    assert(exe_len + 1 + sizeof(daemon_arg) <= sizeof(cmdline));
    memcpy(cmdline, exe, exe_len + 1);
    memcpy(cmdline + exe_len + 1, daemon_arg, sizeof(daemon_arg));
    write_bytes(path, cmdline, exe_len + 1 + sizeof(daemon_arg));

    join(path, sizeof(path), dir, "exe");
    unlink(path);
    assert(symlink(exe, path) == 0);
}

static void make_fake_process_ui(pid_t pid, const char *exe)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%d/cmdline", proc_root,
                     (int)pid);
    assert(n > 0 && (size_t)n < sizeof(path));
    write_bytes(path, exe, strlen(exe) + 1);
}

static void remove_fake_process(pid_t pid)
{
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%d", proc_root, (int)pid);
    assert(n > 0 && (size_t)n < sizeof(dir));
    remove_tree(dir);
}

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        unlink(path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[PATH_MAX];
        join(child, sizeof(child), path, entry->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            remove_tree(child);
        else
            unlink(child);
    }
    closedir(dir);
    rmdir(path);
}

static void on_child_term(int sig)
{
    (void)sig;
    child_should_exit = 1;
}

static pid_t spawn_process(void)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        child_should_exit = 0;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_child_term;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGTERM, &action, NULL) == 0);
        while (!child_should_exit)
            pause();
        _exit(0);
    }
    return pid;
}

static void kill_process(pid_t pid)
{
    if (kill(pid, 0) == 0) {
        assert(kill(pid, SIGKILL) == 0);
        int status;
        assert(waitpid(pid, &status, 0) == pid);
    } else {
        int status;
        (void)waitpid(pid, &status, WNOHANG);
    }
}

void daemon_singleton_test_hook(const char *name, int pid)
{
    if (hook_publish_snapshot && strcmp(name, "between_snapshots") == 0) {
        hook_publish_snapshot = 0;
        create_fake_process(hook_pid, trusted_current, hook_ticks);
    }
    if (hook_replace_primary && hook_pid == pid
        && strcmp(name, "before_signal_revalidate") == 0) {
        hook_replace_primary = 0;
        assert(unlink(primary) == 0);
        write_text(primary, "crossing publisher\n");
    }
    if (hook_once && hook_pid == pid
        && strcmp(name, "stop_before_signal_revalidate") == 0) {
        hook_once = 0;
        write_fake_stat((pid_t)pid, hook_ticks, 'S');
    }
}

static void reset_hints(void)
{
    unlink(primary);
    unlink(legacy);
}

static void assert_raw_lock_blocked(int inherited_fd)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        (void)inherited_fd;
        int fd = open(primary, O_RDWR);
        if (fd < 0)
            _exit(2);
        struct flock lock;
        memset(&lock, 0, sizeof(lock));
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        int rc = fcntl(fd, F_SETLK, &lock);
        int blocked = rc != 0 && (errno == EACCES || errno == EAGAIN);
        close(fd);
        _exit(blocked ? 0 : 3);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_malformed_reclaim_and_lock_lifetime(void)
{
    reset_hints();
    write_text(primary, "not a pid\n");
    write_text(legacy, "partial");
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    assert(lockfd >= 0);
    assert(read_pid_fd(lockfd) == (int)getpid());
    assert(read_pid(legacy) == (int)getpid());
    assert_raw_lock_blocked(lockfd);
    daemon_release(lockfd);
    assert(access(primary, F_OK) == 0);
    assert(access(legacy, F_OK) != 0);
}

static void test_two_claimants(void)
{
    reset_hints();
    create_fake_process(getpid(), trusted_current, 1001);
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int other = -1;
        int result = daemon_claim(&other);
        if (other >= 0)
            daemon_release(other);
        _exit(result == 0 ? 0 : 4);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert_raw_lock_blocked(lockfd);
    daemon_release(lockfd);
    remove_fake_process(getpid());
}

static void test_claim_stops_lockless_old(void)
{
    reset_hints();
    pid_t old = spawn_process();
    create_fake_process(old, trusted_historical, 2001);
    write_pid(legacy, old);
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    assert(read_pid_fd(lockfd) == (int)getpid());
    assert(kill(old, 0) != 0 && errno == ESRCH);
    daemon_release(lockfd);
}

static void test_claim_retries_daemon_appearing_during_snapshot(void)
{
    reset_hints();
    pid_t old = spawn_process();
    hook_pid = old;
    hook_ticks = 2251;
    hook_publish_snapshot = 1;
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    assert(hook_publish_snapshot == 0);
    assert(kill(old, 0) != 0 && errno == ESRCH);
    daemon_release(lockfd);
    remove_fake_process(old);
}

static void test_foreign_process_is_not_signalled(void)
{
    reset_hints();
    pid_t foreign = spawn_process();
    create_fake_process(foreign, "/bin/sh", 3001);
    write_pid(primary, foreign);
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    assert(kill(foreign, 0) == 0);
    daemon_release(lockfd);
    kill_process(foreign);
}

static void test_trusted_ui_and_stale_hint_do_not_block(void)
{
    reset_hints();
    pid_t ui = spawn_process();
    create_fake_process(ui, trusted_current, 3251);
    make_fake_process_ui(ui, trusted_current);
    write_pid(primary, ui);
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    assert(kill(ui, 0) == 0);
    daemon_release(lockfd);
    kill_process(ui);
}

static void test_oversized_hint_is_ignored(void)
{
    reset_hints();
    char oversized[80];
    memset(oversized, '9', sizeof(oversized));
    write_bytes(legacy, oversized, sizeof(oversized));
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    daemon_release(lockfd);
}

static void test_unknown_trusted_process_aborts_without_signal(void)
{
    reset_hints();
    pid_t unknown = spawn_process();
    create_fake_process(unknown, trusted_current, 3501);
    char cmdline[PATH_MAX];
    int n = snprintf(cmdline, sizeof(cmdline), "%s/%d/cmdline", proc_root,
                     (int)unknown);
    assert(n > 0 && (size_t)n < sizeof(cmdline));
    assert(unlink(cmdline) == 0);
    write_pid(primary, unknown);
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == -1);
    assert(lockfd == -1);
    assert(kill(unknown, 0) == 0);
    kill_process(unknown);
}

static void test_primary_inode_replacement_is_reclaimed(void)
{
    reset_hints();
    pid_t old = spawn_process();
    create_fake_process(old, trusted_current, 3751);
    write_pid(primary, old);
    hook_pid = old;
    hook_replace_primary = 1;
    int lockfd = -1;
    assert(daemon_claim(&lockfd) == 1);
    assert(hook_replace_primary == 0);
    assert(read_pid_fd(lockfd) == (int)getpid());
    assert(kill(old, 0) != 0 && errno == ESRCH);
    assert_raw_lock_blocked(lockfd);
    daemon_release(lockfd);
}

static void test_stop_lockless_daemon(void)
{
    reset_hints();
    pid_t old = spawn_process();
    create_fake_process(old, trusted_current, 4001);
    write_pid(primary, old);
    assert(stop_daemon() == 0);
    assert(kill(old, 0) != 0 && errno == ESRCH);
}

static void test_stop_locked_current_daemon(void)
{
    reset_hints();
    int start_pipe[2], ready_pipe[2];
    assert(pipe(start_pipe) == 0);
    assert(pipe(ready_pipe) == 0);
    pid_t daemon = fork();
    assert(daemon >= 0);
    if (daemon == 0) {
        close(start_pipe[1]);
        close(ready_pipe[0]);
        char byte;
        if (read(start_pipe[0], &byte, 1) != 1)
            _exit(10);
        child_should_exit = 0;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_child_term;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, NULL) != 0)
            _exit(11);
        int lockfd = -1;
        int result = daemon_claim(&lockfd);
        byte = result == 1 ? '1' : '0';
        if (write(ready_pipe[1], &byte, 1) != 1 || result != 1)
            _exit(12);
        while (!child_should_exit)
            pause();
        daemon_release(lockfd);
        _exit(0);
    }
    close(start_pipe[0]);
    close(ready_pipe[1]);
    create_fake_process(daemon, trusted_current, 4501);
    assert(write(start_pipe[1], "x", 1) == 1);
    close(start_pipe[1]);
    char ready = 0;
    assert(read(ready_pipe[0], &ready, 1) == 1 && ready == '1');
    close(ready_pipe[0]);
    assert(stop_daemon() == 0);
    assert(kill(daemon, 0) != 0 && errno == ESRCH);
}

static void test_generation_change_is_not_signalled(void)
{
    reset_hints();
    pid_t replacement = spawn_process();
    create_fake_process(replacement, trusted_current, 5001);
    write_pid(primary, replacement);
    hook_pid = replacement;
    hook_ticks = 5002;
    hook_once = 1;
    assert(stop_daemon() == -1);
    assert(hook_once == 0);
    assert(kill(replacement, 0) == 0);
    kill_process(replacement);
}

int main(void)
{
    char pattern[] = "/tmp/bs_singleton_test.XXXXXX";
    char *made = mkdtemp(pattern);
    assert(made);
    assert(strlen(made) < sizeof(root));
    strcpy(root, made);
    join(primary, sizeof(primary), root, "daemon.pid");
    join(legacy, sizeof(legacy), root, "state/legacy.pid");
    join(proc_root, sizeof(proc_root), root, "proc");
    mkdir_p(proc_root);
    char state_dir[PATH_MAX];
    join(state_dir, sizeof(state_dir), root, "state");
    mkdir_p(state_dir);
    char release_dir[PATH_MAX];
    join(release_dir, sizeof(release_dir), root,
         "applications/betterstats/releases/test");
    mkdir_p(release_dir);
    join(trusted_current, sizeof(trusted_current), release_dir,
         "test_daemon_singleton");
    write_text(trusted_current, "test\n");
    join(trusted_historical, sizeof(trusted_historical), root,
         "applications/BetterStats.app");
    write_text(trusted_historical, "test\n");

    assert(setenv("BETTERSTATS_TEST_PIDFILE", primary, 1) == 0);
    assert(setenv("BETTERSTATS_TEST_LEGACY_PIDFILE", legacy, 1) == 0);
    assert(setenv("BETTERSTATS_TEST_PROC_ROOT", proc_root, 1) == 0);
    assert(setenv("BETTERSTATS_INSTALL_ROOT", root, 1) == 0);

    test_malformed_reclaim_and_lock_lifetime();
    test_two_claimants();
    test_claim_stops_lockless_old();
    test_claim_retries_daemon_appearing_during_snapshot();
    test_foreign_process_is_not_signalled();
    test_trusted_ui_and_stale_hint_do_not_block();
    test_oversized_hint_is_ignored();
    test_unknown_trusted_process_aborts_without_signal();
    test_primary_inode_replacement_is_reclaimed();
    test_stop_lockless_daemon();
    test_stop_locked_current_daemon();
    test_generation_change_is_not_signalled();

    remove_tree(root);
    puts("daemon singleton tests passed");
    return 0;
}
