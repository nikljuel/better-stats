#ifndef TEST_INKVIEW_H
#define TEST_INKVIEW_H

#include <sys/types.h>

#define TASK_NOHANDLER 0

typedef struct subtaskinfo_s {
    int id;
    char *name;
    char *book;
    int fgindex;
    int order;
    int rsv_1s;
} subtaskinfo;

typedef struct taskinfo_s {
    int task;
    int nsubtasks;
    unsigned int flags;
    int fbshmkey;
    int fbshmsize;
    pid_t mainpid;
    char *appname;
    void *icon;
    subtaskinfo *subtasks;
    int fbtempkey;
    int rsv_2, rsv_3, rsv_4;
} taskinfo;

void InitInkview(int flags);
void GetActiveTask(int *task, int *subtask);
taskinfo *GetTaskInfo(int task);
int FindTaskByBook(const char *name, int *task, int *subtask);
int get_keylock(void);

#endif
