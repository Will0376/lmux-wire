#include "dispatcher.h"
#include "dispatch_ipc.h"
#include "lmc1.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DISPATCH_EXIT_STARTUP 70
#define DISPATCH_EXIT_RUNTIME 71
#define DISPATCH_CR_GRACE_MS 40
#define DISPATCH_TRACE_EVENTS_MAX 256u
#define DISPATCH_TRACE_LINE_MAX 320u

extern char **environ;

enum active_loss_action {
    ACTIVE_LOSS_IDLE = 0,
    ACTIVE_LOSS_CONSOLE,
    ACTIVE_LOSS_EXIT
};

struct dispatch_app {
    int tty_fd;
    int tty_owned;
    int listen_fd;
    int peer_fd;
    int created_path;
    const char *ipc_path;
    uid_t peer_uid;
    int acquire_ctty;
    struct termios saved_tio;
    int saved_fl;
    int saved_fdfl;
    int saved_valid;
    int exclusive;
    int active;
    enum active_loss_action active_loss;
    char **console_argv;
    struct lmux_dispatch_parser parser;
    uint8_t rx_pending[LMUX_DISPATCH_IPC_MESSAGE_MAX];
    size_t rx_pending_len;
    uint8_t tx_pending[LMUX_DISPATCH_RECORD_MAX];
    size_t tx_pending_len;
    size_t tx_pending_pos;
    uint8_t tty_input[256];
    size_t tty_input_len;
    size_t tty_input_pos;
    long long cr_deadline_ms;
    int trace_fd;
    unsigned trace_events;
    unsigned long long tx_record_id;
    unsigned tx_write_calls;
    size_t tx_logical_len;
    uint32_t tx_logical_hash;
};

static volatile sig_atomic_t stop_requested;

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void trace_event(struct dispatch_app *a, const char *fmt, ...)
{
    char line[DISPATCH_TRACE_LINE_MAX];
    va_list ap;
    int off, n;
    if (!a || a->trace_fd < 0 ||
        a->trace_events >= DISPATCH_TRACE_EVENTS_MAX)
        return;
    off = snprintf(line, sizeof(line), "%lld ", monotonic_ms());
    if (off < 0 || (size_t)off >= sizeof(line))
        return;
    va_start(ap, fmt);
    n = vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(line) - (size_t)off)
        n = (int)(sizeof(line) - (size_t)off - 1);
    off += n;
    if ((size_t)off + 1 < sizeof(line))
        line[off++] = '\n';
    if (write(a->trace_fd, line, (size_t)off) != off) {
        close(a->trace_fd);
        a->trace_fd = -1;
        return;
    }
    a->trace_events++;
}

static uint32_t diagnostic_hash(const uint8_t *data, size_t len)
{
    uint32_t h = UINT32_C(2166136261);
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= UINT32_C(16777619);
    }
    return h;
}

static void trace_tx_record(struct dispatch_app *a, const uint8_t *record,
                            size_t len, size_t tty_len, tcflag_t oflag)
{
    struct lmux_dirty_record decoded;
    uint8_t reason = LMUX_DIRTY_PARSE_NONE;
    int rc = lmux_dirty_record_decode_diagnostic(record, len, &decoded,
                                                 &reason);
    a->tx_record_id++;
    a->tx_write_calls = 0;
    a->tx_logical_len = len;
    a->tx_logical_hash = diagnostic_hash(record, len);
    if (rc == 0) {
        trace_event(a,
            "tx_begin id=%llu kind=D1 type=%u len=%zu tty_len=%zu hash=%08x session=%llu seq=%u frag=%u/%u oflag=0x%lx opost=%u onlcr=%u",
            a->tx_record_id, (unsigned)decoded.type, len, tty_len,
            a->tx_logical_hash,
            (unsigned long long)decoded.session_id, decoded.packet_seq,
            (unsigned)decoded.fragment_index,
            (unsigned)decoded.fragment_count, (unsigned long)oflag,
            (unsigned)((oflag & OPOST) != 0),
            (unsigned)((oflag & OPOST) && (oflag & ONLCR)));
    } else {
        const char *kind = len >= LMUX_LMC1_DIRTY_PREFIX_LEN &&
            memcmp(record, LMUX_LMC1_DIRTY_PREFIX,
                   LMUX_LMC1_DIRTY_PREFIX_LEN) == 0 ? "C1" : "unknown";
        trace_event(a,
            "tx_begin id=%llu kind=%s len=%zu tty_len=%zu hash=%08x parse=%s oflag=0x%lx opost=%u onlcr=%u",
            a->tx_record_id, kind, len, tty_len, a->tx_logical_hash,
            rc < 0 ? lmux_dirty_parse_reason_name(reason) : "foreign",
            (unsigned long)oflag, (unsigned)((oflag & OPOST) != 0),
            (unsigned)((oflag & OPOST) && (oflag & ONLCR)));
    }
}

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

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

static int setup_tty(struct dispatch_app *a)
{
    struct termios tio;
    if (!isatty(a->tty_fd)) {
        errno = ENOTTY;
        return -1;
    }
    a->saved_fl = fcntl(a->tty_fd, F_GETFL, 0);
    a->saved_fdfl = fcntl(a->tty_fd, F_GETFD, 0);
    if (a->saved_fl < 0 || a->saved_fdfl < 0 ||
        tcgetattr(a->tty_fd, &a->saved_tio) < 0)
        return -1;
    a->saved_valid = 1;
    if (a->acquire_ctty) {
        if (setsid() < 0 && errno != EPERM)
            return -1;
#ifdef TIOCSCTTY
        if (ioctl(a->tty_fd, TIOCSCTTY, 0) < 0)
            return -1;
#else
        errno = ENOTSUP;
        return -1;
#endif
    }
#ifdef TIOCEXCL
    if (ioctl(a->tty_fd, TIOCEXCL) < 0)
        return -1;
    a->exclusive = 1;
#endif
    tio = a->saved_tio;
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                     ICRNL | IXON | IXOFF | IXANY);
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag |= CREAD;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(a->tty_fd, TCSANOW, &tio) < 0 ||
        set_nonblock_cloexec(a->tty_fd) < 0)
        return -1;
    /* Validate output policy before any protocol record can be emitted. */
    if (tcgetattr(a->tty_fd, &tio) < 0)
        return -1;
    {
        static const uint8_t probe[] = "~LMUX-C1:AA==\r\n";
        uint8_t encoded[sizeof(probe)];
        size_t encoded_len;
        if (lmux_dispatch_tty_encode(&tio, probe, sizeof(probe) - 1,
                                     encoded, sizeof(encoded),
                                     &encoded_len) < 0)
            return -1;
    }
    trace_event(a,
        "tty_ready fd=%d saved_oflag=0x%lx active_oflag=0x%lx opost=%u onlcr=%u",
        a->tty_fd, (unsigned long)a->saved_tio.c_oflag,
        (unsigned long)tio.c_oflag, (unsigned)((tio.c_oflag & OPOST) != 0),
        (unsigned)((tio.c_oflag & OPOST) && (tio.c_oflag & ONLCR)));
    return 0;
}

static void restore_tty(struct dispatch_app *a)
{
    if (!a->saved_valid)
        return;
    (void)tcsetattr(a->tty_fd, TCSANOW, &a->saved_tio);
    (void)fcntl(a->tty_fd, F_SETFL, a->saved_fl);
    (void)fcntl(a->tty_fd, F_SETFD, a->saved_fdfl);
#ifdef TIOCNXCL
    if (a->exclusive)
        (void)ioctl(a->tty_fd, TIOCNXCL);
#endif
    a->exclusive = 0;
}

static int make_listener(struct dispatch_app *a)
{
    struct sockaddr_un addr;
    struct stat st;
    size_t n = strlen(a->ipc_path);
    int fd;
    if (!n || n >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0 || set_nonblock_cloexec(fd) < 0)
        goto fail;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, a->ipc_path, n + 1);
    if (lstat(a->ipc_path, &st) == 0) {
        int probe;
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            goto fail;
        }
        probe = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (probe < 0)
            goto fail;
        if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(probe);
            errno = EADDRINUSE;
            goto fail;
        }
        {
            int saved = errno;
            close(probe);
            if (saved != ECONNREFUSED && saved != ENOENT) {
                errno = saved;
                goto fail;
            }
        }
        if (unlink(a->ipc_path) < 0 && errno != ENOENT)
            goto fail;
    } else if (errno != ENOENT) {
        goto fail;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        chmod(a->ipc_path, 0600) < 0 || listen(fd, 1) < 0)
        goto fail;
    a->listen_fd = fd;
    a->created_path = 1;
    return 0;
fail:
    {
        int saved = errno;
        if (fd >= 0)
            close(fd);
        errno = saved;
        return -1;
    }
}

static int peer_uid_ok(int fd, uid_t expected)
{
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        return -1;
    if (len != sizeof(cred) || cred.uid != expected) {
        errno = EACCES;
        return -1;
    }
    return 0;
#else
    (void)fd;
    (void)expected;
    errno = ENOTSUP;
    return -1;
#endif
}

static void reset_peer(struct dispatch_app *a)
{
    trace_event(a, "ipc_disconnect active=%u tx_pending=%zu/%zu rx_pending=%zu",
                (unsigned)a->active, a->tx_pending_pos, a->tx_pending_len,
                a->rx_pending_len);
    if (a->peer_fd >= 0)
        close(a->peer_fd);
    a->peer_fd = -1;
    a->active = 0;
    a->rx_pending_len = 0;
    a->tx_pending_len = a->tx_pending_pos = 0;
    a->tty_input_len = a->tty_input_pos = 0;
    a->cr_deadline_ms = 0;
    lmux_dispatch_parser_init(&a->parser);
}

static int accept_peer(struct dispatch_app *a)
{
    int fd = accept(a->listen_fd, NULL, NULL);
    if (fd < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    if (set_nonblock_cloexec(fd) < 0 || peer_uid_ok(fd, a->peer_uid) < 0 ||
        a->peer_fd >= 0) {
        close(fd);
        return 0;
    }
    a->peer_fd = fd;
    a->active = 0;
    a->rx_pending_len = 0;
    a->tx_pending_len = a->tx_pending_pos = 0;
    a->tty_input_len = a->tty_input_pos = 0;
    a->cr_deadline_ms = 0;
    lmux_dispatch_parser_init(&a->parser);
    trace_event(a, "ipc_connect fd=%d uid=%lu", fd,
                (unsigned long)a->peer_uid);
    return 1;
}

static int queue_rx_record(void *opaque, const uint8_t *record, size_t len)
{
    struct dispatch_app *a = opaque;
    size_t packet_len;
    if (a->peer_fd < 0)
        return 0;
    if (a->rx_pending_len) {
        errno = ENOBUFS;
        return -1;
    }
    if (lmux_dispatch_ipc_encode(a->rx_pending, sizeof(a->rx_pending),
                                 LMUX_DISPATCH_IPC_RX_RECORD, record, len,
                                 &packet_len) < 0)
        return -1;
    a->rx_pending_len = packet_len;
    return 0;
}

static int flush_rx_record(struct dispatch_app *a)
{
    ssize_t n;
    if (!a->rx_pending_len || a->peer_fd < 0)
        return 0;
    n = send(a->peer_fd, a->rx_pending, a->rx_pending_len, MSG_NOSIGNAL);
    if (n == (ssize_t)a->rx_pending_len) {
        a->rx_pending_len = 0;
        return 1;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    if (n >= 0)
        errno = EIO;
    return -1;
}

static int queue_tty_record(struct dispatch_app *a,
                            const uint8_t *record, size_t len)
{
    struct termios tio;
    if (a->tx_pending_len) {
        errno = ENOBUFS;
        return -1;
    }
    if (tcgetattr(a->tty_fd, &tio) < 0)
        return -1;
    if (lmux_dispatch_tty_encode(&tio, record, len, a->tx_pending,
                                 sizeof(a->tx_pending),
                                 &a->tx_pending_len) < 0)
        return -1;
    a->tx_pending_pos = 0;
    if (a->trace_fd >= 0)
        trace_tx_record(a, record, len, a->tx_pending_len, tio.c_oflag);
    return 0;
}

static int flush_tty_record(struct dispatch_app *a)
{
    ssize_t n;
    if (a->tx_pending_pos >= a->tx_pending_len)
        return 0;
    n = write(a->tty_fd, a->tx_pending + a->tx_pending_pos,
              a->tx_pending_len - a->tx_pending_pos);
    if (n > 0) {
        size_t before = a->tx_pending_pos;
        a->tx_write_calls++;
        a->tx_pending_pos += (size_t)n;
        trace_event(a,
            "tty_write id=%llu call=%u accepted=%ld range=%zu..%zu tty_total=%zu",
            a->tx_record_id, a->tx_write_calls, (long)n, before,
            a->tx_pending_pos, a->tx_pending_len);
        if (a->tx_pending_pos == a->tx_pending_len) {
            trace_event(a,
                "tx_complete id=%llu logical_len=%zu hash=%08x tty_accepted=%zu calls=%u",
                a->tx_record_id, a->tx_logical_len, a->tx_logical_hash,
                a->tx_pending_len, a->tx_write_calls);
            a->tx_pending_len = a->tx_pending_pos = 0;
        }
        return 1;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    if (n == 0)
        errno = EIO;
    trace_event(a, "tty_write_error id=%llu pos=%zu/%zu errno=%d",
                a->tx_record_id, a->tx_pending_pos, a->tx_pending_len,
                errno);
    return -1;
}

static int send_release(struct dispatch_app *a)
{
    uint8_t msg[LMUX_DISPATCH_IPC_HEADER_LEN];
    size_t len;
    if (a->peer_fd < 0)
        return 0;
    if (lmux_dispatch_ipc_encode(msg, sizeof(msg),
                                 LMUX_DISPATCH_IPC_RELEASE, NULL, 0,
                                 &len) < 0)
        return -1;
    (void)send(a->peer_fd, msg, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    return 0;
}

static int console_handoff(struct dispatch_app *a)
{
    int i;
    if (!a->console_argv)
        return 0;
    trace_event(a, "console_handoff executable=%s", a->console_argv[0]);
    (void)send_release(a);
    if (a->peer_fd >= 0)
        close(a->peer_fd);
    a->peer_fd = -1;
    if (a->listen_fd >= 0)
        close(a->listen_fd);
    a->listen_fd = -1;
    if (a->created_path)
        unlink(a->ipc_path);
    restore_tty(a);
    for (i = 0; i < 3; i++)
        if (a->tty_fd != i && dup2(a->tty_fd, i) < 0)
            return -1;
    for (i = 0; i < 3; i++) {
        int fl = fcntl(i, F_GETFL, 0);
        int fdfl = fcntl(i, F_GETFD, 0);
        if (fl >= 0)
            (void)fcntl(i, F_SETFL, fl & ~O_NONBLOCK);
        if (fdfl >= 0)
            (void)fcntl(i, F_SETFD, fdfl & ~FD_CLOEXEC);
    }
    if (a->tty_owned && a->tty_fd > 2)
        close(a->tty_fd);
    execve(a->console_argv[0], a->console_argv, environ);
    return -1;
}

static int handle_peer_packet(struct dispatch_app *a)
{
    uint8_t packet[LMUX_DISPATCH_IPC_MESSAGE_MAX];
    struct lmux_dispatch_ipc_message msg;
    struct iovec iov = { packet, sizeof(packet) };
    struct msghdr hdr;
    ssize_t n;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    n = recvmsg(a->peer_fd, &hdr, 0);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        return 2;
    }
    if ((hdr.msg_flags & MSG_TRUNC) ||
        lmux_dispatch_ipc_decode(packet, (size_t)n, &msg) < 0)
        return -1;
    switch (msg.type) {
    case LMUX_DISPATCH_IPC_TX_RECORD:
        trace_event(a, "ipc_rx type=TX_RECORD bytes=%u",
                    (unsigned)msg.payload_len);
        return queue_tty_record(a, msg.payload, msg.payload_len);
    case LMUX_DISPATCH_IPC_ACTIVE:
        a->active = 1;
        trace_event(a, "ipc_rx type=ACTIVE");
        return 0;
    case LMUX_DISPATCH_IPC_IDLE:
        a->active = 0;
        trace_event(a, "ipc_rx type=IDLE");
        return 0;
    case LMUX_DISPATCH_IPC_RELEASE:
        trace_event(a, "ipc_rx type=RELEASE");
        return 1;
    default:
        errno = EPROTO;
        return -1;
    }
}

static int process_tty_input(struct dispatch_app *a)
{
    while (!a->rx_pending_len && a->tty_input_pos < a->tty_input_len) {
        int rc = lmux_dispatch_parser_feed(
            &a->parser, a->tty_input + a->tty_input_pos, 1,
            queue_rx_record, a);
        a->tty_input_pos++;
        if (rc != 0)
            return rc;
    }
    if (a->tty_input_pos == a->tty_input_len)
        a->tty_input_pos = a->tty_input_len = 0;
    return 0;
}

static int run_dispatcher(struct dispatch_app *a)
{
    while (!stop_requested) {
        struct pollfd pfd[3];
        nfds_t n = 0;
        int tty_slot, listen_slot, peer_slot = -1;
        int timeout = -1;
        int input_rc = process_tty_input(a);
        if (input_rc > 0)
            return console_handoff(a) < 0 ? -1 : 0;
        if (input_rc < 0)
            return -1;
        if (a->parser.pending_cr) {
            long long now = monotonic_ms();
            long long remaining;
            if (!a->cr_deadline_ms)
                a->cr_deadline_ms = now + DISPATCH_CR_GRACE_MS;
            remaining = a->cr_deadline_ms - now;
            timeout = remaining > 0 ? (int)remaining : 0;
        } else {
            a->cr_deadline_ms = 0;
        }
        tty_slot = (int)n;
        pfd[n].fd = a->tty_fd;
        pfd[n].events = (a->rx_pending_len || a->tty_input_len) ? 0 : POLLIN;
        if (a->tx_pending_len)
            pfd[n].events |= POLLOUT;
        pfd[n++].revents = 0;
        listen_slot = (int)n;
        pfd[n].fd = a->listen_fd;
        pfd[n].events = POLLIN; /* accept-and-reject concurrent peers */
        pfd[n++].revents = 0;
        if (a->peer_fd >= 0) {
            peer_slot = (int)n;
            pfd[n].fd = a->peer_fd;
            pfd[n].events = a->tx_pending_len ? 0 : POLLIN;
            if (a->rx_pending_len)
                pfd[n].events |= POLLOUT;
            pfd[n++].revents = 0;
        }
        int poll_rc = poll(pfd, n, timeout);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pfd[listen_slot].revents & POLLIN)
            if (accept_peer(a) < 0)
                return -1;
        if (peer_slot >= 0 && (pfd[peer_slot].revents & POLLOUT))
            if (flush_rx_record(a) < 0)
                return -1;
        if (peer_slot >= 0 && (pfd[peer_slot].revents & POLLIN)) {
            int rc = handle_peer_packet(a);
            if (rc == 1)
                return console_handoff(a) < 0 ? -1 : 0;
            if (rc == 2) {
                int was_active = a->active;
                reset_peer(a);
                if (was_active && a->active_loss == ACTIVE_LOSS_CONSOLE)
                    return console_handoff(a) < 0 ? -1 : 0;
                if (was_active && a->active_loss == ACTIVE_LOSS_EXIT)
                    return -1;
            } else if (rc < 0) {
                if (a->active && a->active_loss == ACTIVE_LOSS_CONSOLE)
                    return console_handoff(a) < 0 ? -1 : 0;
                return -1;
            }
        }
        if (pfd[tty_slot].revents & POLLOUT)
            if (flush_tty_record(a) < 0)
                return -1;
        if (pfd[tty_slot].revents & POLLIN) {
            ssize_t got = read(a->tty_fd, a->tty_input,
                               sizeof(a->tty_input));
            if (got > 0) {
                int rc;
                a->tty_input_len = (size_t)got;
                a->tty_input_pos = 0;
                rc = process_tty_input(a);
                if (rc > 0)
                    return console_handoff(a) < 0 ? -1 : 0;
                if (rc < 0)
                    return -1;
            } else if (got == 0) {
                errno = EIO;
                return -1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR) {
                return -1;
            }
        }
        if (pfd[tty_slot].revents & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
        if (peer_slot >= 0 &&
            (pfd[peer_slot].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            int was_active = a->active;
            reset_peer(a);
            if (was_active && a->active_loss == ACTIVE_LOSS_CONSOLE)
                return console_handoff(a) < 0 ? -1 : 0;
            if (was_active && a->active_loss == ACTIVE_LOSS_EXIT)
                return -1;
        }
        if (!a->parser.pending_cr) {
            a->cr_deadline_ms = 0;
        } else if (a->cr_deadline_ms &&
                   monotonic_ms() >= a->cr_deadline_ms) {
            a->cr_deadline_ms = 0;
            if (lmux_dispatch_parser_finish_cr(&a->parser))
                return console_handoff(a) < 0 ? -1 : 0;
        }
    }
    return 0;
}

static void cleanup(struct dispatch_app *a)
{
    trace_event(a, "dispatcher_exit active=%u events=%u",
                (unsigned)a->active, a->trace_events);
    if (a->peer_fd >= 0)
        close(a->peer_fd);
    if (a->listen_fd >= 0)
        close(a->listen_fd);
    if (a->created_path)
        unlink(a->ipc_path);
    restore_tty(a);
    if (a->tty_owned && a->tty_fd >= 0)
        close(a->tty_fd);
    if (a->trace_fd >= 0)
        close(a->trace_fd);
    a->trace_fd = -1;
}

static int usage(FILE *f)
{
    fprintf(f,
        "usage: lmux-uart-dispatch (--tty PATH | --tty-fd FD) --ipc-listen PATH\n"
        "       [--peer-uid UID] [--acquire-ctty]\n"
        "       [--active-loss console|idle|exit]\n"
        "       [--trace-file REGULAR_FILE]\n"
        "       [--console-exec /absolute/program [ARG...]]\n");
    return -1;
}

int lmux_uart_dispatch_main(int argc, char **argv)
{
    struct dispatch_app a;
    const char *tty_path = NULL;
    const char *trace_path = NULL;
    int tty_fd_arg = -1;
    int i, rc;
    memset(&a, 0, sizeof(a));
    a.tty_fd = a.listen_fd = a.peer_fd = a.trace_fd = -1;
    a.peer_uid = geteuid();
    a.active_loss = ACTIVE_LOSS_CONSOLE;
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("lmux-uart-dispatch %s (%s)\n", LMUX_BUILD_VERSION,
               LMUX_PROJECT_NAME);
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tty") == 0 && ++i < argc)
            tty_path = argv[i];
        else if (strcmp(argv[i], "--tty-fd") == 0 && ++i < argc)
            tty_fd_arg = atoi(argv[i]);
        else if (strcmp(argv[i], "--ipc-listen") == 0 && ++i < argc)
            a.ipc_path = argv[i];
        else if (strcmp(argv[i], "--peer-uid") == 0 && ++i < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[i], &end, 10);
            if (!end || *end || value > 0xffffffffUL)
                return usage(stderr), DISPATCH_EXIT_STARTUP;
            a.peer_uid = (uid_t)value;
        } else if (strcmp(argv[i], "--acquire-ctty") == 0)
            a.acquire_ctty = 1;
        else if (strcmp(argv[i], "--active-loss") == 0 && ++i < argc) {
            if (strcmp(argv[i], "console") == 0)
                a.active_loss = ACTIVE_LOSS_CONSOLE;
            else if (strcmp(argv[i], "idle") == 0)
                a.active_loss = ACTIVE_LOSS_IDLE;
            else if (strcmp(argv[i], "exit") == 0)
                a.active_loss = ACTIVE_LOSS_EXIT;
            else
                return usage(stderr), DISPATCH_EXIT_STARTUP;
        } else if (strcmp(argv[i], "--trace-file") == 0 && ++i < argc) {
            trace_path = argv[i];
        } else if (strcmp(argv[i], "--console-exec") == 0 && ++i < argc) {
            if (argv[i][0] != '/')
                return usage(stderr), DISPATCH_EXIT_STARTUP;
            a.console_argv = &argv[i];
            break;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            return usage(stderr), DISPATCH_EXIT_STARTUP;
        }
    }
    if ((!tty_path && tty_fd_arg < 0) || (tty_path && tty_fd_arg >= 0) ||
        !a.ipc_path)
        return usage(stderr), DISPATCH_EXIT_STARTUP;
    if (a.active_loss == ACTIVE_LOSS_CONSOLE && !a.console_argv)
        a.active_loss = ACTIVE_LOSS_IDLE;
    if (tty_path) {
        a.tty_fd = open(tty_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        a.tty_owned = 1;
    } else {
        a.tty_fd = tty_fd_arg;
    }
    if (trace_path) {
        struct stat st;
        a.trace_fd = open(trace_path,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                          O_NOFOLLOW, 0600);
        if (a.trace_fd < 0 || fstat(a.trace_fd, &st) < 0 ||
            !S_ISREG(st.st_mode)) {
            int saved = a.trace_fd < 0 ? errno : EINVAL;
            if (a.trace_fd >= 0) {
                close(a.trace_fd);
                a.trace_fd = -1;
            }
            errno = saved;
            perror("lmux-uart-dispatch trace");
            cleanup(&a);
            return DISPATCH_EXIT_STARTUP;
        }
    }
    if (a.tty_fd < 0 || setup_tty(&a) < 0 || make_listener(&a) < 0) {
        perror("lmux-uart-dispatch startup");
        cleanup(&a);
        return DISPATCH_EXIT_STARTUP;
    }
    lmux_dispatch_parser_init(&a.parser);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    rc = run_dispatcher(&a);
    if (rc < 0) {
        trace_event(&a, "runtime_failure errno=%d", errno);
        perror("lmux-uart-dispatch runtime");
    }
    cleanup(&a);
    return rc < 0 ? DISPATCH_EXIT_RUNTIME : 0;
}

int main(int argc, char **argv)
{
    return lmux_uart_dispatch_main(argc, argv);
}
