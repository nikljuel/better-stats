#include <inkview.h>

#include <stdio.h>
#include <string.h>

#ifndef DEVICE_STATE_FILE
#define DEVICE_STATE_FILE "/tmp/bs_daemon_test/device.state"
#endif

static int read_state(int *locked, int *pid, char *app, size_t app_size)
{
    FILE *f = fopen(DEVICE_STATE_FILE, "r");
    if (!f)
        return 0;
    char value[128] = "";
    int ok = fscanf(f, "%d %d %127s", locked, pid, value) == 3 && *pid > 0;
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
    *task = read_state(&locked, &pid, app, sizeof(app)) ? 1 : 0;
    *subtask = 0;
}

taskinfo *GetTaskInfo(int task)
{
    static taskinfo info;
    static char app[128];
    int locked = 0, pid = 0;
    if (task != 1 || !read_state(&locked, &pid, app, sizeof(app)))
        return NULL;
    memset(&info, 0, sizeof(info));
    info.task = 1;
    info.mainpid = pid;
    info.appname = app;
    return &info;
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
