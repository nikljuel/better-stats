#ifndef BETTERSTATS_TEST_INOTIFY_H
#define BETTERSTATS_TEST_INOTIFY_H

#include <unistd.h>

#define IN_NONBLOCK 0x01
#define IN_CLOEXEC 0x02
#define IN_MODIFY 0x04
#define IN_CLOSE_WRITE 0x08
#define IN_CREATE 0x10
#define IN_MOVED_TO 0x20

static inline int inotify_init1(int flags)
{
    (void)flags;
    int fds[2];
    return pipe(fds) == 0 ? fds[0] : -1;
}

static inline int inotify_add_watch(int fd, const char *path, unsigned int mask)
{
    (void)fd;
    (void)path;
    (void)mask;
    return 0;
}

#endif
