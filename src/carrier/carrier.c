#include "carrier.h"

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int lmux_carrier_tty_open(struct lmux_carrier *c);
int lmux_carrier_fd_adopt(struct lmux_carrier *c, int fd);
int lmux_carrier_unix_listen_start(struct lmux_carrier *c);
int lmux_carrier_unix_connect_open(struct lmux_carrier *c);
int lmux_carrier_unix_accept(struct lmux_carrier *c);
int lmux_carrier_dispatch_connect_open(struct lmux_carrier *c);
struct lmux_carrier_io lmux_carrier_dispatch_read(struct lmux_carrier *c,
                                                   void *buf, size_t len);
struct lmux_carrier_io lmux_carrier_dispatch_write(struct lmux_carrier *c,
                                                    const void *buf,
                                                    size_t len);
int lmux_carrier_dispatch_flush_control(struct lmux_carrier *c);

static void close_owned_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void release_exclusive(struct lmux_carrier *c)
{
    if (!c->exclusive_acquired || c->fd < 0)
        return;
#ifdef TIOCNXCL
    (void)ioctl(c->fd, TIOCNXCL);
#endif
    c->exclusive_acquired = 0;
}

void lmux_carrier_init(struct lmux_carrier *c)
{
    memset(c, 0, sizeof(*c));
    c->kind = LMUX_CARRIER_NONE;
    c->fd = -1;
    c->listen_fd = -1;
    c->tty_policy = LMUX_TTY_POLICY_BYTE_STREAM;
    lmux_serial_config_default(&c->serial);
}

void lmux_carrier_cleanup(struct lmux_carrier *c)
{
    release_exclusive(c);
    if (c->owns_fd)
        close_owned_fd(&c->fd);
    else
        c->fd = -1;
    close_owned_fd(&c->listen_fd);
    if (c->created_path && c->path)
        unlink(c->path);
    c->exclusive_acquired = 0;
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
    c->last_errno = 0;
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

int lmux_carrier_set_dispatch_connect(struct lmux_carrier *c,
                                      const char *path)
{
    c->kind = LMUX_CARRIER_DISPATCH_CONNECT;
    c->path = path;
    c->label = path;
    return path ? 0 : -1;
}

void lmux_carrier_set_exclusive(struct lmux_carrier *c, int required)
{
    c->exclusive_requested = required != 0;
}

void lmux_carrier_set_tty_policy(struct lmux_carrier *c,
                                 enum lmux_tty_policy policy)
{
    c->tty_policy = policy;
}

int lmux_carrier_start(struct lmux_carrier *c)
{
    if (c->kind == LMUX_CARRIER_UNIX_LISTEN) {
        int rc = lmux_carrier_unix_listen_start(c);
        if (rc == 0)
            c->last_errno = 0;
        return rc;
    }
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
    else if (c->kind == LMUX_CARRIER_UNIX_LISTEN)
        rc = lmux_carrier_unix_listen_start(c);
    else if (c->kind == LMUX_CARRIER_UNIX_CONNECT)
        rc = lmux_carrier_unix_connect_open(c);
    else if (c->kind == LMUX_CARRIER_DISPATCH_CONNECT)
        rc = lmux_carrier_dispatch_connect_open(c);
    if (rc < 0) {
        c->open_fail_count++;
        c->last_errno = errno;
        return -1;
    }
    c->last_errno = 0;
    c->open_count++;
    return 1;
}

int lmux_carrier_accept(struct lmux_carrier *c)
{
    int rc;
    if (c->kind != LMUX_CARRIER_UNIX_LISTEN || c->listen_fd < 0)
        return 0;
    rc = lmux_carrier_unix_accept(c);
    if (rc > 0) {
        c->last_errno = 0;
        c->open_count++;
    } else if (rc < 0) {
        c->last_errno = errno;
    }
    return rc;
}

void lmux_carrier_close(struct lmux_carrier *c)
{
    if (!c)
        return;
    release_exclusive(c);
    if (c->owns_fd)
        close_owned_fd(&c->fd);
    else
        c->fd = -1;
    close_owned_fd(&c->listen_fd);
    if (c->created_path && c->path)
        unlink(c->path);
    c->created_path = 0;
    c->exclusive_acquired = 0;
    c->dispatch_tx_len = 0;
    c->dispatch_tx_unreported = 0;
    c->dispatch_control = 0;
    c->dispatch_rx_len = 0;
    c->dispatch_rx_pos = 0;
    c->last_errno = 0;
}

void lmux_carrier_mark_lost(struct lmux_carrier *c, long long now_ms)
{
    if (c->fd >= 0)
        c->lost_count++;
    if (c->last_errno == 0)
        c->last_errno = EPIPE;
    release_exclusive(c);
    if (c->owns_fd)
        close_owned_fd(&c->fd);
    else
        c->fd = -1;
    c->exclusive_acquired = 0;
    c->dispatch_tx_len = 0;
    c->dispatch_tx_unreported = 0;
    c->dispatch_control = 0;
    c->dispatch_rx_len = 0;
    c->dispatch_rx_pos = 0;
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
           c->kind == LMUX_CARRIER_UNIX_LISTEN ||
           c->kind == LMUX_CARRIER_UNIX_CONNECT ||
           c->kind == LMUX_CARRIER_DISPATCH_CONNECT;
}

int lmux_carrier_wants_write(const struct lmux_carrier *c)
{
    return c && c->kind == LMUX_CARRIER_DISPATCH_CONNECT &&
        (c->dispatch_control != 0 ||
         (c->dispatch_tx_len &&
          c->dispatch_tx_record[c->dispatch_tx_len - 1] == '\n'));
}

int lmux_carrier_dispatch_control(struct lmux_carrier *c, uint8_t type)
{
    if (!c || c->kind != LMUX_CARRIER_DISPATCH_CONNECT ||
        c->dispatch_control || type < 3 || type > 5) {
        errno = EBUSY;
        return -1;
    }
    c->dispatch_control = type;
    return 0;
}

int lmux_carrier_flush_pending(struct lmux_carrier *c)
{
    if (!c || c->kind != LMUX_CARRIER_DISPATCH_CONNECT)
        return 0;
    if (c->dispatch_tx_unreported) {
        errno = EBUSY;
        return 0;
    }
    if (c->dispatch_control)
        return lmux_carrier_dispatch_flush_control(c);
    return 0;
}

void lmux_carrier_dispatch_abort(struct lmux_carrier *c)
{
    if (!c || c->kind != LMUX_CARRIER_DISPATCH_CONNECT)
        return;
    c->dispatch_tx_len = 0;
    c->dispatch_tx_unreported = 0;
    c->dispatch_rx_len = 0;
    c->dispatch_rx_pos = 0;
    c->dispatch_control = 0;
}

struct lmux_carrier_io lmux_carrier_read(struct lmux_carrier *c,
                                         void *buf, size_t len)
{
    struct lmux_carrier_io r;
    if (c->kind == LMUX_CARRIER_DISPATCH_CONNECT)
        return lmux_carrier_dispatch_read(c, buf, len);
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
    if (c->kind == LMUX_CARRIER_DISPATCH_CONNECT) {
        if (c->dispatch_control) {
            int rc = lmux_carrier_dispatch_flush_control(c);
            r.n = 0;
            r.err = rc < 0 ? errno : 0;
            r.status = rc < 0 ? LMUX_CARRIER_IO_LOST :
                LMUX_CARRIER_IO_WOULD_BLOCK;
            return r;
        }
        return lmux_carrier_dispatch_write(c, buf, len);
    }
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
