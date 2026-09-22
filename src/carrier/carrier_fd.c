#include "carrier.h"

#include <errno.h>
#include <fcntl.h>

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int lmux_carrier_fd_adopt(struct lmux_carrier *c, int fd)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    c->kind = LMUX_CARRIER_FD;
    c->fd = fd;
    c->owns_fd = 1;
    c->label = "fd";
    return set_nonblock(c->fd);
}
