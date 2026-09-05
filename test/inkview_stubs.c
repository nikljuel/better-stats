#include <inkview.h>

#include <stdio.h>
#include <string.h>

#ifndef DEVICE_STATE_FILE
#define DEVICE_STATE_FILE "/tmp/bs_daemon_test/device.state"
#endif
#ifndef CURRENT_BOOK_FILE
#define CURRENT_BOOK_FILE "/tmp/.current"
#endif

static int read_state(int *locked, int *pid, char *app, size_t app_size,
                      char *book, size_t book_size)
{
    FILE *f = fopen(DEVICE_STATE_FILE, "r");
    if (!f)
        return 0;
    char line[512] = "", value[128] = "";
    int ok = fgets(line, sizeof(line), f) != NULL
        && sscanf(line, "%d %d %127s", locked, pid, value) == 3 && *pid > 0;
    if (ok && book && book_size) {
        if (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            snprintf(book, book_size, "%s", line);
        } else {
            book[0] = '\0';
        }
    }
    fclose(f);
    if (ok)
        snprintf(app, app_size, "%s", value);
    return ok;
}

void InitInkview(int flags)
{
    (void)flags;
}

void GetActiveTask(int *task, int *subtask)
{
    int locked = 0, pid = 0;
    char app[128];
    char book[4096] = "";
    *task = read_state(&locked, &pid, app, sizeof(app), book, sizeof(book)) ? 1 : 0;
    *subtask = book[0] ? 7 : 0;
}

taskinfo *GetTaskInfo(int task)
{
    static taskinfo info;
    static char app[128];
    static char book[4096];
    static subtaskinfo subtask;
    int locked = 0, pid = 0;
    if (task != 1 || !read_state(&locked, &pid, app, sizeof(app),
                                 book, sizeof(book)))
        return NULL;
    memset(&info, 0, sizeof(info));
    info.task = 1;
    info.mainpid = pid;
    info.appname = app;
    if (book[0]) {
        memset(&subtask, 0, sizeof(subtask));
        subtask.id = 7;
        subtask.name = book;
        subtask.book = book;
        info.nsubtasks = 1;
        info.subtasks = &subtask;
    }
    return &info;
}

int FindTaskByBook(const char *name, int *task, int *subtask)
{
    int locked = 0, pid = 0;
    char app[128], book[4096] = "";
    if (!read_state(&locked, &pid, app, sizeof(app), book, sizeof(book))
        || (book[0] && strcmp(name, book)))
        return -1;
    int direct = book[0] != '\0';
    if (!direct) {
        FILE *f = fopen(CURRENT_BOOK_FILE, "r");
        int ok = f && fgets(book, sizeof(book), f) != NULL;
        if (f)
            fclose(f);
        size_t n = strlen(book);
        while (n && (book[n - 1] == '\n' || book[n - 1] == '\r'))
            book[--n] = '\0';
        if (!ok || strcmp(name, book))
            return -1;
    }
    *task = 1;
    *subtask = direct ? 7 : 0;
    return 0;
}

int get_keylock(void)
{
    int locked = 0;
    FILE *f = fopen(DEVICE_STATE_FILE, "r");
    if (!f)
        return 0;
    int ok = fscanf(f, "%d", &locked) == 1;
    fclose(f);
    return ok ? locked : 0;
}
