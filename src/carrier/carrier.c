#include "carrier.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int lmux_carrier_tty_open(struct lmux_carrier *c);
int lmux_carrier_fd_adopt(struct lmux_carrier *c, int fd);
int lmux_carrier_unix_listen_start(struct lmux_carrier *c);
int lmux_carrier_unix_connect_open(struct lmux_carrier *c);
int lmux_carrier_unix_accept(struct lmux_carrier *c);

static void close_owned_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

void lmux_carrier_init(struct lmux_carrier *c)
{
    memset(c, 0, sizeof(*c));
    c->kind = LMUX_CARRIER_NONE;
    c->fd = -1;
    c->listen_fd = -1;
}

void lmux_carrier_cleanup(struct lmux_carrier *c)
{
    if (c->owns_fd)
        close_owned_fd(&c->fd);
    else
        c->fd = -1;
    close_owned_fd(&c->listen_fd);
    if (c->created_path && c->path)
        unlink(c->path);
}

int lmux_carrier_set_tty_path(struct lmux_carrier *c, const char *path)
{
    c->kind = LMUX_CARRIER_TTY_PATH;
    c->path = path;
    c->label = path;
    return path ? 0 : -1;
}

int lmux_carrier_set_fd(struct lmux_carrier *c, int fd)
{
    if (lmux_carrier_fd_adopt(c, fd) < 0)
        return -1;
    c->open_count++;
    return 0;
}

int lmux_carrier_set_unix_listen(struct lmux_carrier *c, const char *path)
{
    c->kind = LMUX_CARRIER_UNIX_LISTEN;
    c->path = path;
    c->label = path;
    return path ? 0 : -1;
}

int lmux_carrier_set_unix_connect(struct lmux_carrier *c, const char *path)
{
    c->kind = LMUX_CARRIER_UNIX_CONNECT;
    c->path = path;
    c->label = path;
    return path ? 0 : -1;
}

int lmux_carrier_start(struct lmux_carrier *c)
{
    if (c->kind == LMUX_CARRIER_UNIX_LISTEN)
        return lmux_carrier_unix_listen_start(c);
    return 0;
}

int lmux_carrier_try_open(struct lmux_carrier *c, long long now_ms)
{
    int rc = -1;
    if (c->fd >= 0 || c->listen_fd >= 0)
        return 0;
    if (!lmux_carrier_can_reopen(c))
        return 0;
    if (now_ms < c->next_attempt_ms)
        return 0;
    c->next_attempt_ms = now_ms + 1000;
    if (c->kind == LMUX_CARRIER_TTY_PATH)
        rc = lmux_carrier_tty_open(c);
    else if (c->kind == LMUX_CARRIER_UNIX_CONNECT)
        rc = lmux_carrier_unix_connect_open(c);
    if (rc < 0) {
        c->open_fail_count++;
        c->last_errno = errno;
        return -1;
    }
    c->open_count++;
    return 1;
}

int lmux_carrier_accept(struct lmux_carrier *c)
{
    int rc;
    if (c->kind != LMUX_CARRIER_UNIX_LISTEN || c->listen_fd < 0)
        return 0;
    rc = lmux_carrier_unix_accept(c);
    if (rc > 0)
        c->open_count++;
    else if (rc < 0)
        c->last_errno = errno;
    return rc;
}

void lmux_carrier_mark_lost(struct lmux_carrier *c, long long now_ms)
{
    if (c->fd >= 0)
        c->lost_count++;
    if (c->owns_fd)
        close_owned_fd(&c->fd);
    else
        c->fd = -1;
    c->next_attempt_ms = now_ms + 250;
}

int lmux_carrier_fd(const struct lmux_carrier *c)
{
    return c->fd;
}

int lmux_carrier_listen_fd(const struct lmux_carrier *c)
{
    return c->listen_fd;
}

const char *lmux_carrier_label(const struct lmux_carrier *c)
{
    return c->label ? c->label : "-";
}

int lmux_carrier_is_configured(const struct lmux_carrier *c)
{
    return c->kind != LMUX_CARRIER_NONE;
}

int lmux_carrier_can_reopen(const struct lmux_carrier *c)
{
    return c->kind == LMUX_CARRIER_TTY_PATH ||
           c->kind == LMUX_CARRIER_UNIX_CONNECT;
}

struct lmux_carrier_io lmux_carrier_read(struct lmux_carrier *c,
                                         void *buf, size_t len)
{
    struct lmux_carrier_io r;
    r.n = read(c->fd, buf, len);
    r.err = r.n < 0 ? errno : 0;
    if (r.n > 0) {
        c->rx_bytes += (unsigned long long)r.n;
        r.status = LMUX_CARRIER_IO_PROGRESS;
    } else if (r.n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        r.status = LMUX_CARRIER_IO_WOULD_BLOCK;
    } else if (r.n == 0) {
        r.status = LMUX_CARRIER_IO_LOST;
    } else {
        r.status = LMUX_CARRIER_IO_LOST;
        c->last_errno = errno;
    }
    return r;
}

struct lmux_carrier_io lmux_carrier_write(struct lmux_carrier *c,
                                          const void *buf, size_t len)
{
    struct lmux_carrier_io r;
    r.n = write(c->fd, buf, len);
    r.err = r.n < 0 ? errno : 0;
    if (r.n > 0) {
        c->tx_bytes += (unsigned long long)r.n;
        r.status = LMUX_CARRIER_IO_PROGRESS;
    } else if (r.n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        r.status = LMUX_CARRIER_IO_WOULD_BLOCK;
    } else {
        r.status = LMUX_CARRIER_IO_LOST;
        if (r.n < 0)
            c->last_errno = errno;
    }
    return r;
}
