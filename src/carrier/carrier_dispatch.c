#include "carrier.h"
#include "dispatch_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static int set_nonblock_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    int fdfl = fcntl(fd, F_GETFD, 0);
    if (fl < 0 || fdfl < 0 ||
        fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static int peer_is_socket_owner(int fd, const char *path)
{
#ifdef SO_PEERCRED
    struct ucred cred;
    struct stat st;
    socklen_t len = sizeof(cred);
    if (lstat(path, &st) < 0 || !S_ISSOCK(st.st_mode) ||
        (st.st_mode & 0077) != 0 ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        return -1;
    if (len != sizeof(cred) || cred.uid != st.st_uid) {
        errno = EACCES;
        return -1;
    }
    return 0;
#else
    (void)fd; (void)path;
    errno = ENOTSUP;
    return -1;
#endif
}

int lmux_carrier_dispatch_connect_open(struct lmux_carrier *c)
{
    struct sockaddr_un addr;
    size_t n;
    int fd;
    if (!c || !c->path) {
        errno = EINVAL;
        return -1;
    }
    n = strlen(c->path);
    if (n == 0 || n >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, c->path, n + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (peer_is_socket_owner(fd, c->path) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (set_nonblock_cloexec(fd) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    c->fd = fd;
    c->owns_fd = 1;
    c->dispatch_tx_len = 0;
    c->dispatch_tx_unreported = 0;
    c->dispatch_control = 0;
    c->dispatch_rx_len = c->dispatch_rx_pos = 0;
    return 0;
}

static int send_message(int fd, uint8_t type, const void *payload, size_t len)
{
    uint8_t packet[LMUX_DISPATCH_IPC_MESSAGE_MAX];
    size_t packet_len;
    ssize_t n;
    if (lmux_dispatch_ipc_encode(packet, sizeof(packet), type, payload, len,
                                 &packet_len) < 0)
        return -1;
    n = send(fd, packet, packet_len, MSG_NOSIGNAL);
    if (n == (ssize_t)packet_len)
        return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    if (n >= 0)
        errno = EIO; /* SOCK_SEQPACKET must not partially send a record. */
    return -1;
}

int lmux_carrier_dispatch_flush_control(struct lmux_carrier *c)
{
    int rc;
    if (!c || c->kind != LMUX_CARRIER_DISPATCH_CONNECT || c->fd < 0 ||
        !c->dispatch_control) {
        errno = EINVAL;
        return -1;
    }
    rc = send_message(c->fd, c->dispatch_control, NULL, 0);
    if (rc > 0)
        c->dispatch_control = 0;
    return rc;
}

struct lmux_carrier_io lmux_carrier_dispatch_read(struct lmux_carrier *c,
                                                   void *buf, size_t len)
{
    uint8_t packet[LMUX_DISPATCH_IPC_MESSAGE_MAX];
    struct lmux_dispatch_ipc_message msg;
    struct iovec iov = { packet, sizeof(packet) };
    struct msghdr hdr;
    struct lmux_carrier_io r = { LMUX_CARRIER_IO_WOULD_BLOCK, 0, 0 };
    ssize_t n;
    if (c->dispatch_rx_pos < c->dispatch_rx_len) {
        size_t take = c->dispatch_rx_len - c->dispatch_rx_pos;
        if (take > len)
            take = len;
        memcpy(buf, c->dispatch_rx_record + c->dispatch_rx_pos, take);
        c->dispatch_rx_pos += take;
        if (c->dispatch_rx_pos == c->dispatch_rx_len)
            c->dispatch_rx_pos = c->dispatch_rx_len = 0;
        r.n = (ssize_t)take;
        r.status = LMUX_CARRIER_IO_PROGRESS;
        c->rx_bytes += take;
        return r;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    n = recvmsg(c->fd, &hdr, 0);
    if (n < 0) {
        r.err = errno;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            r.status = LMUX_CARRIER_IO_LOST;
        return r;
    }
    if (n == 0) {
        r.status = LMUX_CARRIER_IO_LOST;
        return r;
    }
    if ((hdr.msg_flags & MSG_TRUNC) ||
        lmux_dispatch_ipc_decode(packet, (size_t)n, &msg) < 0) {
        r.status = LMUX_CARRIER_IO_FATAL;
        r.err = (hdr.msg_flags & MSG_TRUNC) ? EMSGSIZE : errno;
        return r;
    }
    if (msg.type == LMUX_DISPATCH_IPC_RELEASE) {
        r.status = LMUX_CARRIER_IO_LOST;
        r.err = ECONNRESET;
        return r;
    }
    if (msg.type != LMUX_DISPATCH_IPC_RX_RECORD) {
        r.status = LMUX_CARRIER_IO_FATAL;
        r.err = EPROTO;
        return r;
    }
    memcpy(c->dispatch_rx_record, msg.payload, msg.payload_len);
    c->dispatch_rx_len = msg.payload_len;
    c->dispatch_rx_pos = 0;
    {
        size_t take = msg.payload_len > len ? len : msg.payload_len;
        memcpy(buf, c->dispatch_rx_record, take);
        c->dispatch_rx_pos = take;
        if (c->dispatch_rx_pos == c->dispatch_rx_len)
            c->dispatch_rx_pos = c->dispatch_rx_len = 0;
        r.n = (ssize_t)take;
    }
    r.status = LMUX_CARRIER_IO_PROGRESS;
    c->rx_bytes += (size_t)r.n;
    return r;
}

struct lmux_carrier_io lmux_carrier_dispatch_write(struct lmux_carrier *c,
                                                    const void *buf,
                                                    size_t len)
{
    const uint8_t *p = buf;
    struct lmux_carrier_io r = { LMUX_CARRIER_IO_WOULD_BLOCK, 0, 0 };
    size_t take;
    int rc;
    if (c->dispatch_tx_len &&
        c->dispatch_tx_record[c->dispatch_tx_len - 1] == '\n') {
        rc = send_message(c->fd, LMUX_DISPATCH_IPC_TX_RECORD,
                          c->dispatch_tx_record, c->dispatch_tx_len);
        if (rc <= 0) {
            if (rc < 0) {
                r.status = LMUX_CARRIER_IO_LOST;
                r.err = errno;
            }
            return r;
        }
        c->tx_bytes += c->dispatch_tx_len;
        r.n = (ssize_t)c->dispatch_tx_unreported;
        c->dispatch_tx_len = 0;
        c->dispatch_tx_unreported = 0;
        r.status = r.n ? LMUX_CARRIER_IO_PROGRESS :
            LMUX_CARRIER_IO_WOULD_BLOCK;
        return r;
    }
    if (!len)
        return r;
    take = len;
    if (take > sizeof(c->dispatch_tx_record) - c->dispatch_tx_len)
        take = sizeof(c->dispatch_tx_record) - c->dispatch_tx_len;
    if (!take) {
        r.status = LMUX_CARRIER_IO_FATAL;
        r.err = EMSGSIZE;
        return r;
    }
    memcpy(c->dispatch_tx_record + c->dispatch_tx_len, p, take);
    c->dispatch_tx_len += take;
    r.n = (ssize_t)take;
    r.status = LMUX_CARRIER_IO_PROGRESS;
    if (memchr(p, '\n', take) != NULL &&
        c->dispatch_tx_record[c->dispatch_tx_len - 1] != '\n') {
        r.status = LMUX_CARRIER_IO_FATAL;
        r.err = EPROTO;
        return r;
    }
    if (c->dispatch_tx_record[c->dispatch_tx_len - 1] == '\n') {
        c->dispatch_tx_unreported = take;
        rc = send_message(c->fd, LMUX_DISPATCH_IPC_TX_RECORD,
                          c->dispatch_tx_record, c->dispatch_tx_len);
        if (rc > 0) {
            c->tx_bytes += c->dispatch_tx_len;
            c->dispatch_tx_len = 0;
            c->dispatch_tx_unreported = 0;
        } else if (rc < 0) {
            r.status = LMUX_CARRIER_IO_LOST;
            r.err = errno;
        } else {
            r.n = 0;
            r.status = LMUX_CARRIER_IO_WOULD_BLOCK;
        }
    }
    return r;
}
