#include "supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int dup_away_from_targets(int fd)
{
    int n;
    if (fd != LMUX_SUP_WORKER_DATA_FD && fd != LMUX_SUP_WORKER_CTRL_FD)
        return fd;
    n = fcntl(fd, F_DUPFD_CLOEXEC, 5);
    if (n >= 0)
        close(fd);
    return n;
}

static int clear_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

pid_t lmux_supervisor_spawn_worker(const char *worker_path, int data_fd,
                                   int ctrl_fd, int verbose)
{
    pid_t pid;
    if (!worker_path || data_fd < 0 || ctrl_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    pid = fork();
    if (pid != 0)
        return pid;

    (void)setpgid(0, 0);
    data_fd = dup_away_from_targets(data_fd);
    ctrl_fd = dup_away_from_targets(ctrl_fd);
    if (data_fd < 0 || ctrl_fd < 0)
        _exit(127);
    if (dup2(data_fd, LMUX_SUP_WORKER_DATA_FD) < 0 ||
        dup2(ctrl_fd, LMUX_SUP_WORKER_CTRL_FD) < 0)
        _exit(127);
    if (clear_cloexec(LMUX_SUP_WORKER_DATA_FD) < 0 ||
        clear_cloexec(LMUX_SUP_WORKER_CTRL_FD) < 0)
        _exit(127);
    if (data_fd != LMUX_SUP_WORKER_DATA_FD)
        close(data_fd);
    if (ctrl_fd != LMUX_SUP_WORKER_CTRL_FD)
        close(ctrl_fd);
    {
        int fd;
        for (fd = 5; fd < 256; fd++)
            close(fd);
    }
    {
        char data_arg[32], ctrl_arg[32];
        snprintf(data_arg, sizeof(data_arg), "%d", LMUX_SUP_WORKER_DATA_FD);
        snprintf(ctrl_arg, sizeof(ctrl_arg), "%d", LMUX_SUP_WORKER_CTRL_FD);
        if (verbose)
            execl(worker_path, "lmuxd", "--worker",
                  "--carrier-fd", data_arg,
                  "--supervisor-control-fd", ctrl_arg,
                  "--verbose", (char *)NULL);
        else
            execl(worker_path, "lmuxd", "--worker",
                  "--carrier-fd", data_arg,
                  "--supervisor-control-fd", ctrl_arg,
                  (char *)NULL);
    }
    fprintf(stderr, "lmux supervisor: exec %s failed: %s\n",
            worker_path, strerror(errno));
    _exit(127);
}
