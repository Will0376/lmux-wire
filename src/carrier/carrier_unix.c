#include "carrier.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_unix_listener(const char *path, int *created_path)
{
    int fd;
    struct sockaddr_un sa;
    if (!path || strlen(path) >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    unlink(path);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    if (created_path)
        *created_path = 1;
    return fd;
}

int lmux_carrier_unix_listen_start(struct lmux_carrier *c)
{
    c->listen_fd = create_unix_listener(c->path, &c->created_path);
    return c->listen_fd < 0 ? -1 : 0;
}

int lmux_carrier_unix_connect_open(struct lmux_carrier *c)
{
    struct sockaddr_un sa;
    int fd;
    if (!c->path || strlen(c->path) >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, c->path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    c->fd = fd;
    c->owns_fd = 1;
    return 0;
}

int lmux_carrier_unix_accept(struct lmux_carrier *c)
{
    int fd = accept(c->listen_fd, NULL, NULL);
    if (fd < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    (void)set_nonblock(fd);
    if (c->owns_fd && c->fd >= 0)
        close(c->fd);
    c->fd = fd;
    c->owns_fd = 1;
    return 1;
}
