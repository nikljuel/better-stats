#ifndef TEST_INKVIEW_H
#define TEST_INKVIEW_H

#include <sys/types.h>

#define TASK_NOHANDLER 0

typedef struct taskinfo_s {
    int task;
    int nsubtasks;
    unsigned int flags;
    int fbshmkey;
    int fbshmsize;
    pid_t mainpid;
    char *appname;
} taskinfo;

void InitInkview(int flags);
void GetActiveTask(int *task, int *subtask);
taskinfo *GetTaskInfo(int task);
int get_keylock(void);

#endif
