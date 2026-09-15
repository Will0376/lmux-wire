#include "lxf.h"
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
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef CRTSCTS
#define CRTSCTS 020000000000
#endif

enum ep_type {
    EP_NONE = 0,
    EP_PTY,
    EP_SOCKET,
    EP_SHELL,
    EP_FILE
};

enum file_state {
    FS_IDLE = 0,
    FS_PUT,
    FS_GET
};

enum link_state {
    LINK_WAIT_HELLO = 0,
    LINK_ESTABLISHED
};

struct ring {
    uint8_t buf[LMUX_RX_RING];
    size_t r;
    size_t w;
    size_t len;
};

struct endpoint {
    enum ep_type type;
    char path[LMUX_PATH_MAX];
    int fd;
    int listen_fd;
    int created_path;
    char slave_path[64];
    struct termios baseline_tio;
    int have_baseline_tio;
    pid_t child_pid;
    long long respawn_at_ms;
};

struct file_service {
    enum file_state state;
    uint8_t inbuf[LXF_MAX_RECORD];
    size_t in_len;
    size_t need_len;
    uint8_t outbuf[LXF_MAX_RECORD];
    size_t out_len;
    size_t out_pos;
    int fd;
    char path[LXF_PATH_MAX];
    char tmp_path[LXF_PATH_MAX];
    mode_t mode;
    uint64_t expected_size;
    int size_known;
    uint64_t count;
    struct sha256_ctx sha;
};

struct channel {
    struct endpoint ep;
    struct ring rx;
    struct ring tx;
    /*
     * Three lifetimes are intentionally separate:
     * - the physical LMUX link is app.link_state/session;
     * - ep.type/path/listen_fd are the configured endpoint lifetime;
     * - local_open/remote_open/local_paired describe one transient app stream.
     * A stream EOF/CLOSE can tear down only that app stream; it is not a
     * protocol fault and must not reset the physical link.
     */
    int local_open;
    int remote_open;
    int active;
    int local_paired;
    int ignore_peer_close;
    int pending_open;
    int pending_close;
    int pending_winsize;
    uint32_t local_generation;
    uint32_t remote_generation;
    uint32_t pending_close_generation;
    uint16_t winsize_rows;
    uint16_t winsize_cols;
    uint16_t winsize_xpixel;
    uint16_t winsize_ypixel;
    uint16_t tx_seq;
    uint16_t rx_seq;
    unsigned int remote_credit;
    unsigned int credit_pending;
    struct file_service file;
};

struct app {
    const char *carrier_path;
    const char *control_path;
    int verbose;
    int carrier_fd;
    int control_listen_fd;
    int control_fd;
    char control_in[256];
    size_t control_in_len;
    int stop;
    uint32_t local_session;
    uint32_t peer_session;
    uint32_t local_features;
    uint32_t peer_features;
    char peer_version[64];
    enum link_state link_state;
    long long last_carrier_try_ms;
    long long last_hello_ms;
    uint8_t enc_in[LMUX_MAX_ENCODED];
    size_t enc_in_len;
    int discard_until_delim;
    uint8_t out[LMUX_MAX_ENCODED];
    size_t out_len;
    size_t out_pos;
    int pending_reset;
    int pending_hello;
    int rr_next;
    unsigned long long frames_tx;
    unsigned long long frames_rx;
    unsigned long long bytes_tx;
    unsigned long long bytes_rx;
    unsigned long long crc_errors;
    unsigned long long malformed_frames;
    unsigned long long sequence_errors;
    unsigned long long resets;
    unsigned long long local_resets;
    unsigned long long peer_resets;
    unsigned long long reconnects;
    char last_reset[96];
    struct channel ch[LMUX_CHANNELS];
};

static struct app *global_app;

static void log_msg(struct app *a, int verbose, const char *fmt, ...)
{
    va_list ap;
    if (verbose && !a->verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void ring_clear(struct ring *r)
{
    r->r = r->w = r->len = 0;
}

static size_t ring_free(const struct ring *r)
{
    return sizeof(r->buf) - r->len;
}

static size_t ring_peek_linear(const struct ring *r, const uint8_t **p)
{
    *p = r->buf + r->r;
    if (r->len == 0)
        return 0;
    if (r->r + r->len <= sizeof(r->buf))
        return r->len;
    return sizeof(r->buf) - r->r;
}

static void ring_drop(struct ring *r, size_t n)
{
    if (n > r->len)
        n = r->len;
    r->r = (r->r + n) % sizeof(r->buf);
    r->len -= n;
    if (r->len == 0)
        r->r = r->w = 0;
}

static size_t ring_write(struct ring *r, const uint8_t *buf, size_t n)
{
    size_t done = 0;
    if (n > ring_free(r))
        n = ring_free(r);
    while (done < n) {
        size_t chunk = sizeof(r->buf) - r->w;
        if (chunk > n - done)
            chunk = n - done;
        memcpy(r->buf + r->w, buf + done, chunk);
        r->w = (r->w + chunk) % sizeof(r->buf);
        r->len += chunk;
        done += chunk;
    }
    return done;
}

static void make_session(struct app *a)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    uint32_t v = 0;
    if (fd >= 0) {
        ssize_t r = read(fd, &v, sizeof(v));
        close(fd);
        if (r == (ssize_t)sizeof(v) && v != 0) {
            a->local_session = v;
            return;
        }
    }
    v = (uint32_t)getpid() ^ (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)a;
    if (v == 0)
        v = 1;
    a->local_session = v;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void deactivate_channel(struct channel *c);
static void spawn_shell(struct app *a, int idx);
static void file_reset(struct channel *c);
static int file_service_step(struct app *a, int idx);
static int queue_winsize_frame(struct app *a, int idx);

static void child_close_daemon_fds(void)
{
    long maxfd = sysconf(_SC_OPEN_MAX);
    int fd;
    if (maxfd < 0 || maxfd > 256)
        maxfd = 256;
    for (fd = 3; fd < maxfd; fd++)
        close(fd);
}

static void restore_pty_baseline(struct endpoint *e)
{
    int sfd;
    if (!e->have_baseline_tio)
        return;
    sfd = open(e->slave_path, O_RDWR | O_NOCTTY);
    if (sfd >= 0) {
        (void)tcsetattr(sfd, TCSANOW, &e->baseline_tio);
        close(sfd);
    }
}

static void terminate_shell(struct app *a, int idx)
{
    struct endpoint *e = &a->ch[idx].ep;
    long long deadline;
    if (e->child_pid <= 0)
        return;
    kill(e->child_pid, SIGHUP);
    deadline = now_ms() + 1000;
    while (now_ms() < deadline) {
        int st;
        pid_t r = waitpid(e->child_pid, &st, WNOHANG);
        if (r == e->child_pid) {
            e->child_pid = 0;
            return;
        }
        usleep(20000);
    }
    kill(e->child_pid, SIGKILL);
    (void)waitpid(e->child_pid, NULL, 0);
    e->child_pid = 0;
}

static void close_socket_client(struct app *a, int idx, int send_close)
{
    struct channel *c = &a->ch[idx];
    int was_paired;
    if (c->ep.type != EP_SOCKET || c->ep.fd < 0)
        return;
    was_paired = c->local_paired || c->active;
    close_fd(&c->ep.fd);
    c->local_open = 0;
    c->local_paired = 0;
    c->remote_open = 0;
    c->remote_generation = 0;
    c->pending_open = 0;
    if (send_close && a->link_state == LINK_ESTABLISHED) {
        c->pending_close = 1;
        c->pending_close_generation = c->local_generation;
        if (was_paired)
            c->ignore_peer_close = 1;
    }
    if (c->active)
        deactivate_channel(c);
    log_msg(a, 1, "channel %d local socket disconnected session=%u", idx + 1,
            c->pending_close_generation);
}

static void deactivate_channel(struct channel *c)
{
    c->active = 0;
    c->local_paired = 0;
    ring_clear(&c->rx);
    ring_clear(&c->tx);
    c->tx_seq = 0;
    c->rx_seq = 0;
    c->remote_credit = 0;
    c->credit_pending = 0;
}

static void activate_channel(struct app *a, int idx)
{
    struct channel *c = &a->ch[idx];
    ring_clear(&c->rx);
    ring_clear(&c->tx);
    c->tx_seq = 0;
    c->rx_seq = 0;
    c->remote_credit = 0;
    c->credit_pending = LMUX_RX_RING;
    c->active = 1;
    if (c->ep.type == EP_SOCKET)
        c->local_paired = 1;
    log_msg(a, 1, "channel %d active", idx + 1);
}

static void update_channel_active(struct app *a, int idx)
{
    struct channel *c = &a->ch[idx];
    int want = a->link_state == LINK_ESTABLISHED && c->local_open && c->remote_open;
    if (want && !c->active)
        activate_channel(a, idx);
    else if (!want && c->active)
        deactivate_channel(c);
}

static void reset_transport(struct app *a, int disconnect_sockets)
{
    int i;
    a->link_state = LINK_WAIT_HELLO;
    a->peer_session = 0;
    a->last_hello_ms = 0;
    a->enc_in_len = 0;
    a->discard_until_delim = 0;
    a->out_len = a->out_pos = 0;
    a->pending_reset = 0;
    a->pending_hello = 0;
    for (i = 0; i < LMUX_CHANNELS; i++) {
        struct channel *c = &a->ch[i];
        c->remote_open = 0;
        c->pending_open = 0;
        c->pending_close = 0;
        c->pending_winsize = 0;
        c->remote_generation = 0;
        c->pending_close_generation = 0;
        c->local_paired = 0;
        c->ignore_peer_close = 0;
        if (c->ep.type == EP_FILE)
            file_reset(c);
        deactivate_channel(c);
        if (disconnect_sockets && c->ep.type == EP_SOCKET && c->ep.fd >= 0) {
            close_fd(&c->ep.fd);
            c->local_open = 0;
            log_msg(a, 1, "channel %d local socket disconnected session=%u", i + 1,
                    c->local_generation);
        }
    }
}

static void protocol_fault(struct app *a, const char *why)
{
    log_msg(a, 0, "protocol fault: %s", why);
    log_msg(a, 1, "sending RESET: %s", why);
    snprintf(a->last_reset, sizeof(a->last_reset), "local protocol=%s", why);
    a->resets++;
    a->local_resets++;
    reset_transport(a, 1);
    make_session(a);
    a->pending_reset = 1;
}

static int configure_tty(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
        return -1;
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    return tcsetattr(fd, TCSANOW, &tio);
}

static void close_carrier(struct app *a)
{
    if (a->carrier_fd >= 0) {
        log_msg(a, 1, "carrier closed");
        close_fd(&a->carrier_fd);
    }
    reset_transport(a, 1);
    make_session(a);
}

static void try_open_carrier(struct app *a)
{
    long long t = now_ms();
    int fd;
    if (!a->carrier_path)
        return;
    if (a->carrier_fd >= 0 || t - a->last_carrier_try_ms < 1000)
        return;
    a->last_carrier_try_ms = t;
    fd = open(a->carrier_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        log_msg(a, 0, "carrier open failed %s: %s", a->carrier_path, strerror(errno));
        return;
    }
    if (configure_tty(fd) < 0) {
        log_msg(a, 0, "carrier termios failed %s: %s", a->carrier_path, strerror(errno));
        close(fd);
        return;
    }
    (void)tcflush(fd, TCIOFLUSH);
    if (ioctl(fd, TIOCEXCL) < 0)
        log_msg(a, 1, "warning: TIOCEXCL failed: %s", strerror(errno));
    a->carrier_fd = fd;
    reset_transport(a, 1);
    make_session(a);
    a->last_hello_ms = 0;
    log_msg(a, 1, "carrier opened %s", a->carrier_path);
}

static int queue_frame(struct app *a, struct lmux_frame *fr)
{
    if (a->out_len != a->out_pos)
        return 0;
    a->out_len = a->out_pos = 0;
    if (lmux_encode_frame(fr, a->out, sizeof(a->out), &a->out_len) < 0)
        return -1;
    a->frames_tx++;
    a->bytes_tx += fr->payload_len;
    return 1;
}

static int queue_simple(struct app *a, uint8_t type, uint8_t ch)
{
    struct lmux_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.version = LMUX_VERSION;
    fr.type = type;
    fr.channel = ch;
    return queue_frame(a, &fr);
}

static int queue_openclose(struct app *a, uint8_t type, int idx, uint32_t generation)
{
    struct lmux_frame fr;
    (void)generation;
    memset(&fr, 0, sizeof(fr));
    fr.version = LMUX_VERSION;
    fr.type = type;
    fr.channel = (uint8_t)(idx + 1);
    return queue_frame(a, &fr);
}

static int queue_hello(struct app *a)
{
    struct lmux_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.version = LMUX_VERSION;
    fr.type = LMUX_T_HELLO;
    fr.payload_len = 8;
    memcpy(fr.payload, "LMX1", 4);
    lmux_put_le32(fr.payload + 4, a->local_session);
    return queue_frame(a, &fr);
}

static int build_next_output(struct app *a)
{
    int i, n;
    long long t = now_ms();

    if (a->carrier_fd < 0 || a->out_len != a->out_pos)
        return 0;
    a->out_len = a->out_pos = 0;

    if (a->pending_reset) {
        a->pending_reset = 0;
        return queue_simple(a, LMUX_T_RESET, 0);
    }
    if (a->pending_hello) {
        a->pending_hello = 0;
        return queue_hello(a);
    }
    if (a->link_state == LINK_WAIT_HELLO) {
        if (a->last_hello_ms == 0 || t - a->last_hello_ms >= 1000) {
            a->last_hello_ms = t;
            return queue_hello(a);
        }
        return 0;
    }

    for (i = 0; i < LMUX_CHANNELS; i++) {
        if (a->ch[i].pending_close) {
            a->ch[i].pending_close = 0;
            return queue_openclose(a, LMUX_T_CLOSE, i, a->ch[i].pending_close_generation);
        }
    }
    for (i = 0; i < LMUX_CHANNELS; i++) {
        if (a->ch[i].pending_open) {
            a->ch[i].pending_open = 0;
            return queue_openclose(a, LMUX_T_OPEN, i, a->ch[i].local_generation);
        }
    }
    for (i = 0; i < LMUX_CHANNELS; i++) {
        if (a->ch[i].pending_winsize)
            return queue_winsize_frame(a, i);
    }
    for (i = 0; i < LMUX_CHANNELS; i++) {
        struct channel *c = &a->ch[i];
        if (c->active && c->credit_pending) {
            struct lmux_frame fr;
            unsigned int delta = c->credit_pending;
            if (delta > 0xffff)
                delta = 0xffff;
            memset(&fr, 0, sizeof(fr));
            fr.version = LMUX_VERSION;
            fr.type = LMUX_T_CREDIT;
            fr.channel = (uint8_t)(i + 1);
            fr.payload_len = 2;
            lmux_put_le16(fr.payload, (uint16_t)delta);
            if (queue_frame(a, &fr) > 0) {
                c->credit_pending -= delta;
                return 1;
            }
            return -1;
        }
    }
    for (n = 0; n < LMUX_CHANNELS; n++) {
        int idx = (a->rr_next + n) % LMUX_CHANNELS;
        struct channel *c = &a->ch[idx];
        if (c->active && c->tx.len && c->remote_credit) {
            struct lmux_frame fr;
            const uint8_t *p;
            size_t avail = ring_peek_linear(&c->tx, &p);
            if (avail > LMUX_MAX_PAYLOAD)
                avail = LMUX_MAX_PAYLOAD;
            if (avail > c->remote_credit)
                avail = c->remote_credit;
            memset(&fr, 0, sizeof(fr));
            fr.version = LMUX_VERSION;
            fr.type = LMUX_T_DATA;
            fr.channel = (uint8_t)(idx + 1);
            fr.sequence = c->tx_seq;
            fr.payload_len = (uint16_t)avail;
            memcpy(fr.payload, p, avail);
            if (queue_frame(a, &fr) > 0) {
                ring_drop(&c->tx, avail);
                c->remote_credit -= (unsigned int)avail;
                c->tx_seq++;
                a->rr_next = (idx + 1) % LMUX_CHANNELS;
                return 1;
            }
            return -1;
        }
    }
    return 0;
}

static void advertise_local_open(struct app *a)
{
    int i;
    for (i = 0; i < LMUX_CHANNELS; i++)
        if (a->ch[i].local_open)
            a->ch[i].pending_open = 1;
}

static int frame_generation_ok(const struct lmux_frame *fr, uint32_t *gen)
{
    if (fr->payload_len == 0) {
        *gen = 0;
        return 1;
    }
    if (fr->payload_len == 4) {
        *gen = lmux_get_le32(fr->payload);
        return *gen != 0;
    }
    return 0;
}

static void apply_winsize(struct app *a, int idx, const uint8_t *p, uint16_t len)
{
    struct winsize ws;
    struct endpoint *e = &a->ch[idx].ep;
    if (len != 8) {
        protocol_fault(a, "bad WINSIZE length");
        return;
    }
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = lmux_get_le16(p);
    ws.ws_col = lmux_get_le16(p + 2);
    ws.ws_xpixel = lmux_get_le16(p + 4);
    ws.ws_ypixel = lmux_get_le16(p + 6);
    if (ws.ws_row == 0 || ws.ws_col == 0 || ws.ws_row > 1000 || ws.ws_col > 1000) {
        log_msg(a, 1, "discard malformed WINSIZE ch%d rows=%u cols=%u",
                idx + 1, ws.ws_row, ws.ws_col);
        return;
    }
    if ((e->type == EP_PTY || e->type == EP_SHELL) && e->fd >= 0) {
        if (ioctl(e->fd, TIOCSWINSZ, &ws) < 0)
            log_msg(a, 1, "channel %d TIOCSWINSZ failed: %s", idx + 1, strerror(errno));
        else
            log_msg(a, 1, "channel %d winsize %ux%u", idx + 1, ws.ws_col, ws.ws_row);
    }
}

static int queue_winsize(struct app *a, int idx, unsigned int rows, unsigned int cols,
                         unsigned int xpixel, unsigned int ypixel)
{
    struct channel *c;
    if (idx < 0 || idx >= LMUX_CHANNELS || rows == 0 || cols == 0 || rows > 1000 || cols > 1000)
        return -1;
    if (a->link_state != LINK_ESTABLISHED || !(a->peer_features & LMUX_FEATURE_WINSIZE))
        return -1;
    c = &a->ch[idx];
    c->winsize_rows = (uint16_t)rows;
    c->winsize_cols = (uint16_t)cols;
    c->winsize_xpixel = (uint16_t)xpixel;
    c->winsize_ypixel = (uint16_t)ypixel;
    c->pending_winsize = 1;
    return 0;
}

static int queue_winsize_frame(struct app *a, int idx)
{
    struct channel *c = &a->ch[idx];
    struct lmux_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.version = LMUX_VERSION;
    fr.type = LMUX_T_WINSIZE;
    fr.channel = (uint8_t)(idx + 1);
    fr.payload_len = 8;
    lmux_put_le16(fr.payload, c->winsize_rows);
    lmux_put_le16(fr.payload + 2, c->winsize_cols);
    lmux_put_le16(fr.payload + 4, c->winsize_xpixel);
    lmux_put_le16(fr.payload + 6, c->winsize_ypixel);
    if (queue_frame(a, &fr) > 0) {
        c->pending_winsize = 0;
        return 1;
    }
    return -1;
}

static void handle_frame(struct app *a, const struct lmux_frame *fr)
{
    int idx = fr->channel ? fr->channel - 1 : -1;
    struct channel *c = idx >= 0 ? &a->ch[idx] : NULL;

    if (fr->type == LMUX_T_RESET) {
        log_msg(a, 1, "peer RESET");
        a->resets++;
        a->peer_resets++;
        snprintf(a->last_reset, sizeof(a->last_reset), "peer");
        reset_transport(a, 1);
        make_session(a);
        return;
    }
    if (fr->type == LMUX_T_HELLO) {
        uint32_t sid;
        if (fr->payload_len < 8 || memcmp(fr->payload, "LMX1", 4) != 0) {
            if (a->link_state == LINK_WAIT_HELLO)
                log_msg(a, 1, "discard malformed HELLO before session");
            else
                protocol_fault(a, "bad HELLO");
            return;
        }
        sid = lmux_get_le32(fr->payload + 4);
        a->peer_features = 0;
        a->peer_version[0] = 0;
        if (fr->payload_len >= 13) {
            uint8_t vlen;
            a->peer_features = lmux_get_le32(fr->payload + 8);
            vlen = fr->payload[12];
            if ((uint16_t)(13 + vlen) <= fr->payload_len) {
                size_t n = vlen;
                if (n >= sizeof(a->peer_version))
                    n = sizeof(a->peer_version) - 1;
                memcpy(a->peer_version, fr->payload + 13, n);
                a->peer_version[n] = 0;
            }
        }
        if (a->link_state == LINK_ESTABLISHED && sid == a->peer_session)
            return;

        log_msg(a, 1, "peer HELLO session %08x", sid);
        if (a->link_state != LINK_ESTABLISHED)
            a->reconnects++;
        reset_transport(a, 0);
        a->link_state = LINK_ESTABLISHED;
        a->peer_session = sid;
        a->pending_hello = 1;
        advertise_local_open(a);
        return;
    }
    if (a->link_state == LINK_WAIT_HELLO) {
        log_msg(a, 1, "discard stale frame type %u before HELLO", fr->type);
        return;
    }

    switch (fr->type) {
    case LMUX_T_OPEN: {
        uint32_t gen;
        if (!frame_generation_ok(fr, &gen)) {
            protocol_fault(a, "OPEN payload");
            return;
        }
        if (gen && c->remote_generation && gen < c->remote_generation) {
            log_msg(a, 1, "channel %d discard stale peer OPEN session=%u current=%u",
                    idx + 1, gen, c->remote_generation);
            return;
        }
        if (gen)
            c->remote_generation = gen;
        c->ignore_peer_close = 0;
        if (!c->remote_open)
            log_msg(a, 1, "channel %d peer client ready session=%u", idx + 1,
                    gen ? gen : c->remote_generation);
        c->remote_open = 1;
        update_channel_active(a, idx);
        if (c->active)
            log_msg(a, 1, "channel %d session active", idx + 1);
        break;
    }
    case LMUX_T_CLOSE: {
        uint32_t gen;
        int was_paired;
        if (!frame_generation_ok(fr, &gen)) {
            protocol_fault(a, "CLOSE payload");
            return;
        }
        if (gen && c->remote_open && c->remote_generation != gen) {
            log_msg(a, 1, "channel %d discard stale peer CLOSE session=%u current=%u",
                    idx + 1, gen, c->remote_generation);
            return;
        }
        if (c->ignore_peer_close) {
            c->ignore_peer_close = 0;
            c->remote_open = 0;
            c->remote_generation = 0;
            update_channel_active(a, idx);
            log_msg(a, 1, "channel %d discard expected peer CLOSE for previous app stream",
                    idx + 1);
            break;
        }
        if (c->remote_open)
            log_msg(a, 1, "channel %d peer EOF session=%u", idx + 1,
                    gen ? gen : c->remote_generation);
        was_paired = c->local_paired;
        c->remote_open = 0;
        c->remote_generation = 0;
        if (c->ep.type == EP_SOCKET && was_paired)
            close_socket_client(a, idx, 0);
        update_channel_active(a, idx);
        if (c->local_open && c->ep.type != EP_SOCKET)
            c->pending_open = 1;
        log_msg(a, 1, "channel %d session closed", idx + 1);
        break;
    }
    case LMUX_T_CREDIT: {
        uint16_t delta;
        if (fr->payload_len != 2) {
            protocol_fault(a, "bad CREDIT length");
            return;
        }
        delta = lmux_get_le16(fr->payload);
        if (!c->active) {
            log_msg(a, 1, "channel %d discard CREDIT for inactive app stream", idx + 1);
            return;
        }
        if (delta == 0 || c->remote_credit + delta > LMUX_RX_RING) {
            protocol_fault(a, "bad CREDIT accounting");
            return;
        }
        c->remote_credit += delta;
        break;
    }
    case LMUX_T_DATA:
        if (fr->payload_len == 0) {
            protocol_fault(a, "empty DATA");
            return;
        }
        if (!c->active) {
            log_msg(a, 1, "channel %d discard DATA for inactive app stream", idx + 1);
            return;
        }
        if (fr->sequence != c->rx_seq) {
            a->sequence_errors++;
            protocol_fault(a, "DATA sequence mismatch");
            return;
        }
        if (ring_free(&c->rx) < fr->payload_len) {
            protocol_fault(a, "RX ring overflow");
            return;
        }
        ring_write(&c->rx, fr->payload, fr->payload_len);
        c->rx_seq++;
        break;
    case LMUX_T_WINSIZE:
        apply_winsize(a, idx, fr->payload, fr->payload_len);
        break;
    default:
        protocol_fault(a, "unknown frame");
        break;
    }
}

static void process_encoded_record(struct app *a, const uint8_t *buf, size_t len)
{
    struct lmux_frame fr;

    if (len == 0)
        return;
    if (lmux_decode_frame(buf, len, &fr) < 0) {
        a->malformed_frames++;
        a->crc_errors++;
        if (a->link_state == LINK_WAIT_HELLO)
            log_msg(a, 1, "discard malformed frame before HELLO");
        else
            protocol_fault(a, "bad frame");
        return;
    }
    a->frames_rx++;
    a->bytes_rx += fr.payload_len;
    handle_frame(a, &fr);
}

static void feed_carrier(struct app *a)
{
    uint8_t buf[256];
    for (;;) {
        ssize_t r = read(a->carrier_fd, buf, sizeof(buf));
        ssize_t i;
        if (r > 0) {
            for (i = 0; i < r; i++) {
                if (buf[i] == 0) {
                    if (!a->discard_until_delim)
                        process_encoded_record(a, a->enc_in, a->enc_in_len);
                    a->enc_in_len = 0;
                    a->discard_until_delim = 0;
                } else if (!a->discard_until_delim) {
                    if (a->enc_in_len >= sizeof(a->enc_in)) {
                        if (a->link_state == LINK_WAIT_HELLO)
                            log_msg(a, 1, "discard oversized frame before HELLO");
                        else
                            protocol_fault(a, "oversized encoded frame");
                        a->discard_until_delim = 1;
                        a->enc_in_len = 0;
                    } else {
                        a->enc_in[a->enc_in_len++] = buf[i];
                    }
                }
            }
            continue;
        }
        if (r == 0) {
            close_carrier(a);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        if (errno == EIO || errno == ENODEV) {
            log_msg(a, 0, "carrier lost: %s", strerror(errno));
            close_carrier(a);
            return;
        }
        log_msg(a, 0, "carrier read error: %s", strerror(errno));
        close_carrier(a);
        return;
    }
}

static void flush_carrier(struct app *a)
{
    while (a->carrier_fd >= 0 && a->out_pos < a->out_len) {
        ssize_t w = write(a->carrier_fd, a->out + a->out_pos, a->out_len - a->out_pos);
        if (w > 0) {
            a->out_pos += (size_t)w;
            if (a->out_pos == a->out_len)
                a->out_pos = a->out_len = 0;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        log_msg(a, 0, "carrier write error: %s", w < 0 ? strerror(errno) : "short write");
        close_carrier(a);
        return;
    }
}

static void deliver_rx(struct app *a, int idx)
{
    struct channel *c = &a->ch[idx];
    const uint8_t *p;
    size_t n;
    int fd = c->ep.fd;

    if (!c->active || fd < 0 || c->ep.type == EP_FILE)
        return;
    while ((n = ring_peek_linear(&c->rx, &p)) > 0) {
        ssize_t w = write(fd, p, n);
        if (w > 0) {
            ring_drop(&c->rx, (size_t)w);
            c->credit_pending += (unsigned int)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (c->ep.type == EP_PTY && errno == EIO)
            return;
        log_msg(a, 0, "channel %d local write error: %s", idx + 1, strerror(errno));
        if (c->ep.type == EP_SOCKET)
            close_socket_client(a, idx, 1);
        return;
    }
}

static void collect_tx(struct app *a, int idx)
{
    struct channel *c = &a->ch[idx];
    int fd = c->ep.fd;

    if (!c->active || fd < 0 || c->ep.type == EP_FILE)
        return;
    while (ring_free(&c->tx) > 0) {
        uint8_t tmp[256];
        size_t cap = ring_free(&c->tx);
        ssize_t r;
        if (cap > sizeof(tmp))
            cap = sizeof(tmp);
        r = read(fd, tmp, cap);
        if (r > 0) {
            ring_write(&c->tx, tmp, (size_t)r);
            continue;
        }
        if (r == 0) {
            if (c->ep.type == EP_SOCKET)
                close_socket_client(a, idx, 1);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        if (c->ep.type == EP_PTY && errno == EIO)
            return;
        log_msg(a, 0, "channel %d local read error: %s", idx + 1, strerror(errno));
        if (c->ep.type == EP_SOCKET)
            close_socket_client(a, idx, 1);
        return;
    }
}

static int create_pty(struct app *a, int idx)
{
    struct endpoint *e = &a->ch[idx].ep;
    int unlock = 0;
    int ptn = -1;
    int sfd;
    struct stat st;

    e->fd = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (e->fd < 0) {
        perror("/dev/ptmx");
        return -1;
    }
    if (ioctl(e->fd, TIOCSPTLCK, &unlock) < 0 || ioctl(e->fd, TIOCGPTN, &ptn) < 0) {
        perror("pty ioctl");
        return -1;
    }
    snprintf(e->slave_path, sizeof(e->slave_path), "/dev/pts/%d", ptn);
    sfd = open(e->slave_path, O_RDWR | O_NOCTTY);
    if (sfd >= 0) {
        if (tcgetattr(sfd, &e->baseline_tio) == 0)
            e->have_baseline_tio = 1;
        close(sfd);
    }
    if (lstat(e->path, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a symlink\n", e->path);
            return -1;
        }
        if (unlink(e->path) < 0) {
            perror(e->path);
            return -1;
        }
    } else if (errno != ENOENT) {
        perror(e->path);
        return -1;
    }
    if (symlink(e->slave_path, e->path) < 0) {
        perror(e->path);
        return -1;
    }
    e->created_path = 1;
    a->ch[idx].local_open = 1;
    log_msg(a, 1, "channel %d PTY %s -> %s", idx + 1, e->path, e->slave_path);
    return 0;
}

static void spawn_shell(struct app *a, int idx)
{
    struct endpoint *e = &a->ch[idx].ep;
    pid_t pid;

    if (e->type != EP_SHELL || e->child_pid > 0 || e->fd < 0)
        return;
    restore_pty_baseline(e);
    pid = fork();
    if (pid < 0) {
        log_msg(a, 0, "channel %d fork /bin/sh failed: %s", idx + 1, strerror(errno));
        e->respawn_at_ms = now_ms() + 1000;
        return;
    }
    if (pid == 0) {
        pid_t ppid = getppid();
        int sfd;
        prctl(PR_SET_PDEATHSIG, SIGHUP);
        if (getppid() != ppid)
            _exit(127);
        if (setsid() < 0)
            _exit(127);
        sfd = open(e->slave_path, O_RDWR);
        if (sfd < 0)
            _exit(127);
        (void)ioctl(sfd, TIOCSCTTY, 0);
        if (e->have_baseline_tio)
            (void)tcsetattr(sfd, TCSANOW, &e->baseline_tio);
        if (dup2(sfd, STDIN_FILENO) < 0 ||
            dup2(sfd, STDOUT_FILENO) < 0 ||
            dup2(sfd, STDERR_FILENO) < 0)
            _exit(127);
        if (sfd > STDERR_FILENO)
            close(sfd);
        setenv("TERM", "vt100", 1);
        child_close_daemon_fds();
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(127);
    }
    e->child_pid = pid;
    e->respawn_at_ms = 0;
    log_msg(a, 1, "channel %d spawned /bin/sh pid=%ld", idx + 1, (long)pid);
}

static void reap_shells(struct app *a)
{
    int i;
    for (;;) {
        int st;
        pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid <= 0)
            break;
        for (i = 0; i < LMUX_CHANNELS; i++) {
            struct endpoint *e = &a->ch[i].ep;
            if (e->type == EP_SHELL && e->child_pid == pid) {
                e->child_pid = 0;
                e->respawn_at_ms = now_ms() + 1000;
                restore_pty_baseline(e);
                log_msg(a, 1, "channel %d shell exited", i + 1);
            }
        }
    }
}

static void supervise_shells(struct app *a)
{
    int i;
    long long t = now_ms();
    reap_shells(a);
    for (i = 0; i < LMUX_CHANNELS; i++) {
        struct endpoint *e = &a->ch[i].ep;
        if (e->type == EP_SHELL && e->child_pid <= 0 && (e->respawn_at_ms == 0 || t >= e->respawn_at_ms))
            spawn_shell(a, i);
    }
}

static int create_socket_listener(struct app *a, int idx)
{
    struct endpoint *e = &a->ch[idx].ep;
    struct sockaddr_un sa;
    struct stat st;

    if (strlen(e->path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "socket path too long: %s\n", e->path);
        return -1;
    }
    if (lstat(e->path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a socket\n", e->path);
            return -1;
        }
        if (unlink(e->path) < 0) {
            perror(e->path);
            return -1;
        }
    } else if (errno != ENOENT) {
        perror(e->path);
        return -1;
    }
    e->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (e->listen_fd < 0) {
        perror("socket");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, e->path);
    if (bind(e->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror(e->path);
        return -1;
    }
    e->created_path = 1;
    if (listen(e->listen_fd, 4) < 0) {
        perror("listen");
        return -1;
    }
    log_msg(a, 1, "channel %d socket listener %s", idx + 1, e->path);
    return 0;
}

static int create_control_listener(struct app *a)
{
    struct sockaddr_un sa;
    struct stat st;

    if (!a->control_path)
        return 0;
    if (strlen(a->control_path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "control socket path too long: %s\n", a->control_path);
        return -1;
    }
    if (lstat(a->control_path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a socket\n", a->control_path);
            return -1;
        }
        if (unlink(a->control_path) < 0) {
            perror(a->control_path);
            return -1;
        }
    } else if (errno != ENOENT) {
        perror(a->control_path);
        return -1;
    }
    a->control_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (a->control_listen_fd < 0) {
        perror("control socket");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, a->control_path);
    if (bind(a->control_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror(a->control_path);
        return -1;
    }
    if (listen(a->control_listen_fd, 4) < 0) {
        perror("control listen");
        return -1;
    }
    log_msg(a, 1, "control socket %s", a->control_path);
    return 0;
}

static void append_feature(char *buf, size_t cap, const char *name)
{
    if (buf[0] && strlen(buf) + 1 < cap)
        strncat(buf, " ", cap - strlen(buf) - 1);
    strncat(buf, name, cap - strlen(buf) - 1);
}

static void feature_string(uint32_t f, char *buf, size_t cap)
{
    buf[0] = 0;
    if (f & LMUX_FEATURE_FILE)
        append_feature(buf, cap, "FILE");
    if (f & LMUX_FEATURE_CWD_QUERY)
        append_feature(buf, cap, "CWD");
    if (f & LMUX_FEATURE_WINSIZE)
        append_feature(buf, cap, "WINSIZE");
    if (f & LMUX_FEATURE_STREAM_GEN)
        append_feature(buf, cap, "STREAM_GEN");
    if (!buf[0])
        strncpy(buf, "-", cap);
    buf[cap - 1] = 0;
}

static const char *endpoint_name(const struct channel *c)
{
    switch (c->ep.type) {
    case EP_PTY: return "pty";
    case EP_SOCKET: return "socket";
    case EP_SHELL: return "shell";
    case EP_FILE: return "file";
    default: return "none";
    }
}

static const char *local_client_state(const struct channel *c)
{
    if (!c->local_open)
        return "none";
    if (c->ep.type == EP_SOCKET)
        return c->local_paired ? "active" : "pending";
    return "persistent";
}

static const char *peer_client_state(const struct channel *c)
{
    if (!c->remote_open)
        return "none";
    if (c->active)
        return "active";
    return "pending";
}

static void control_status(struct app *a, int fd)
{
    char lf[96], pf[96], line[512];
    int i;
    feature_string(a->local_features, lf, sizeof(lf));
    feature_string(a->peer_features, pf, sizeof(pf));
    snprintf(line, sizeof(line),
             "%s\nlocal:  %s\npeer:   %s\nwire:   LMUX/%d\nmode:   %s\nlink:   %s\nfeatures: %s\npeer-features: %s\n",
             LMUX_PROJECT_NAME, LMUX_BUILD_VERSION,
             a->peer_version[0] ? a->peer_version : "-", LMUX_VERSION,
             a->peer_features ? "extended" : "legacy",
             a->link_state == LINK_ESTABLISHED ? "established" : "wait-hello",
             lf, pf);
    (void)write(fd, line, strlen(line));
    snprintf(line, sizeof(line),
             "counters: tx_frames=%llu rx_frames=%llu tx_bytes=%llu rx_bytes=%llu crc_errors=%llu malformed=%llu seq_errors=%llu resets=%llu local_resets=%llu peer_resets=%llu sessions=%llu\n",
             a->frames_tx, a->frames_rx, a->bytes_tx, a->bytes_rx, a->crc_errors,
             a->malformed_frames, a->sequence_errors, a->resets, a->local_resets,
             a->peer_resets, a->reconnects);
    (void)write(fd, line, strlen(line));
    snprintf(line, sizeof(line), "last_reset: %s\n",
             a->last_reset[0] ? a->last_reset : "-");
    (void)write(fd, line, strlen(line));
    for (i = 0; i < LMUX_CHANNELS; i++) {
        struct channel *c = &a->ch[i];
        snprintf(line, sizeof(line),
                 "ch%d %-6s configured=%s link=%s client=%s peer_client=%s tx=%zu rx=%zu generation=%u peer_generation=%u\n",
                 i + 1, endpoint_name(c),
                 c->ep.type == EP_NONE ? "no" : "yes",
                 c->active ? "active" : "idle",
                 local_client_state(c), peer_client_state(c),
                 c->tx.len, c->rx.len, c->local_generation, c->remote_generation);
        (void)write(fd, line, strlen(line));
    }
}

static void control_handle_line(struct app *a, char *line)
{
    char cmd[16];
    if (sscanf(line, "%15s", cmd) != 1)
        return;
    if (strcmp(cmd, "STATUS") == 0) {
        control_status(a, a->control_fd);
    } else if (strcmp(cmd, "WINSIZE") == 0) {
        unsigned int ch, rows, cols, xp = 0, yp = 0;
        if (sscanf(line, "%*s %u %u %u %u %u", &ch, &rows, &cols, &xp, &yp) < 3 ||
            ch < 1 || ch > LMUX_CHANNELS) {
            (void)write(a->control_fd, "ERR bad WINSIZE\n", 16);
        } else if (queue_winsize(a, (int)ch - 1, rows, cols, xp, yp) < 0) {
            (void)write(a->control_fd, "ERR WINSIZE unavailable\n", 23);
        } else {
            (void)write(a->control_fd, "OK\n", 3);
        }
    } else {
        (void)write(a->control_fd, "ERR unknown command\n", 20);
    }
}

static void accept_control(struct app *a)
{
    int fd;
    for (;;) {
        fd = accept(a->control_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            log_msg(a, 0, "control accept: %s", strerror(errno));
            return;
        }
        if (a->control_fd >= 0) {
            close(fd);
            continue;
        }
        if (set_nonblock(fd) < 0) {
            close(fd);
            continue;
        }
        a->control_fd = fd;
        a->control_in_len = 0;
    }
}

static void service_control(struct app *a)
{
    char *nl;
    ssize_t r;
    if (a->control_fd < 0)
        return;
    r = read(a->control_fd, a->control_in + a->control_in_len,
             sizeof(a->control_in) - 1 - a->control_in_len);
    if (r > 0) {
        a->control_in_len += (size_t)r;
        a->control_in[a->control_in_len] = 0;
        nl = strchr(a->control_in, '\n');
        if (nl) {
            *nl = 0;
            control_handle_line(a, a->control_in);
            close_fd(&a->control_fd);
            a->control_in_len = 0;
        } else if (a->control_in_len >= sizeof(a->control_in) - 1) {
            (void)write(a->control_fd, "ERR request too long\n", 21);
            close_fd(&a->control_fd);
            a->control_in_len = 0;
        }
    } else if (r == 0) {
        close_fd(&a->control_fd);
        a->control_in_len = 0;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close_fd(&a->control_fd);
        a->control_in_len = 0;
    }
}

static void accept_client(struct app *a, int idx)
{
    struct endpoint *e = &a->ch[idx].ep;
    for (;;) {
        int fd = accept(e->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            log_msg(a, 0, "accept %s: %s", e->path, strerror(errno));
            return;
        }
        if (e->fd >= 0) {
            close(fd);
            continue;
        }
        if (set_nonblock(fd) < 0) {
            perror("client nonblock");
            close(fd);
            continue;
        }
        e->fd = fd;
        a->ch[idx].local_generation++;
        if (a->ch[idx].local_generation == 0)
            a->ch[idx].local_generation = 1;
        a->ch[idx].local_open = 1;
        if (a->link_state == LINK_ESTABLISHED)
            a->ch[idx].pending_open = 1;
        update_channel_active(a, idx);
        log_msg(a, 1, "channel %d local client connected session=%u", idx + 1,
                a->ch[idx].local_generation);
        if (!a->ch[idx].remote_open)
            log_msg(a, 1, "channel %d waiting for peer", idx + 1);
    }
}

static int abs_path_ok(const char *p)
{
    return p && p[0] == '/' && strlen(p) < LXF_PATH_MAX;
}

static int make_tmp_for_final(const char *final, char *tmp, size_t cap)
{
    const char *slash = strrchr(final, '/');
    const char *base;
    size_t dir_len;
    if (!slash || final[0] != '/')
        return -1;
    base = slash + 1;
    if (!*base)
        return -1;
    dir_len = (size_t)(slash - final);
    if (dir_len == 0)
        dir_len = 1;
    if (snprintf(tmp, cap, "%.*s/.%s.lmux-tmp-%ld-%ld",
                 (int)dir_len, final, base, (long)getpid(), (long)time(NULL)) >= (int)cap)
        return -1;
    return 0;
}

static void file_reset(struct channel *c)
{
    struct file_service *fs = &c->file;
    if (fs->fd >= 0)
        close(fs->fd);
    if (fs->tmp_path[0])
        unlink(fs->tmp_path);
    memset(fs, 0, sizeof(*fs));
    fs->fd = -1;
}

static void file_abort_keep_output(struct channel *c)
{
    struct file_service *fs = &c->file;
    uint8_t out[LXF_MAX_RECORD];
    size_t out_len = fs->out_len;
    size_t out_pos = fs->out_pos;
    if (out_len)
        memcpy(out, fs->outbuf, out_len);
    if (fs->fd >= 0)
        close(fs->fd);
    if (fs->tmp_path[0])
        unlink(fs->tmp_path);
    memset(fs, 0, sizeof(*fs));
    fs->fd = -1;
    if (out_len) {
        memcpy(fs->outbuf, out, out_len);
        fs->out_len = out_len;
        fs->out_pos = out_pos;
    }
}

static int file_queue_record(struct channel *c, uint8_t type, const uint8_t *payload, uint32_t len)
{
    struct file_service *fs = &c->file;
    if (fs->out_len != fs->out_pos)
        return -1;
    fs->out_len = fs->out_pos = 0;
    return lxf_encode_record(type, 0, payload, len, fs->outbuf, sizeof(fs->outbuf), &fs->out_len);
}

static void file_send_error(struct channel *c, uint32_t code, const char *msg)
{
    uint8_t p[160];
    size_t n = strlen(msg);
    if (n > sizeof(p) - 5)
        n = sizeof(p) - 5;
    lmux_put_le32(p, code);
    memcpy(p + 4, msg, n);
    p[4 + n] = 0;
    (void)file_queue_record(c, LXF_T_ERROR, p, (uint32_t)(5 + n));
    file_abort_keep_output(c);
}

static void file_send_ok_simple(struct channel *c)
{
    uint8_t p[40];
    memset(p, 0, sizeof(p));
    (void)file_queue_record(c, LXF_T_OK, p, 0);
}

static void file_handle_put_request(struct app *a, int idx, const uint8_t *p, uint32_t len)
{
    struct channel *c = &a->ch[idx];
    struct file_service *fs = &c->file;
    struct stat st;
    const char *path;

    if (len < 17) {
        file_send_error(c, LXF_E_PROTOCOL, "short PUT request");
        return;
    }
    path = (const char *)p + 16;
    if (memchr(path, 0, len - 16) == NULL || !abs_path_ok(path)) {
        file_send_error(c, LXF_E_BADPATH, "TARGET path must be absolute");
        return;
    }
    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            file_send_error(c, LXF_E_NOTREG, "destination is not regular");
            return;
        }
    } else if (errno != ENOENT) {
        file_send_error(c, errno == EACCES ? LXF_E_DENIED : LXF_E_IO, strerror(errno));
        return;
    }
    if (make_tmp_for_final(path, fs->tmp_path, sizeof(fs->tmp_path)) < 0) {
        file_send_error(c, LXF_E_BADPATH, "bad destination path");
        return;
    }
    fs->fd = open(fs->tmp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fs->fd < 0) {
        file_send_error(c, errno == ENOSPC ? LXF_E_NOSPACE : LXF_E_IO, strerror(errno));
        return;
    }
    strncpy(fs->path, path, sizeof(fs->path) - 1);
    fs->mode = lmux_get_le32(p) & 0777;
    fs->expected_size = lmux_get_le64(p + 4);
    fs->size_known = (lmux_get_le32(p + 12) & LXF_F_SIZE_KNOWN) != 0;
    fs->count = 0;
    fs->state = FS_PUT;
    sha256_init(&fs->sha);
    log_msg(a, 1, "file ch%d PUT request path=%s size=%llu%s", idx + 1, fs->path,
            (unsigned long long)fs->expected_size, fs->size_known ? "" : " unknown");
    file_send_ok_simple(c);
}

static void file_handle_get_request(struct app *a, int idx, const uint8_t *p, uint32_t len)
{
    struct channel *c = &a->ch[idx];
    struct file_service *fs = &c->file;
    struct stat st;
    uint8_t ok[16];
    const char *path = (const char *)p;

    if (len < 2 || memchr(p, 0, len) == NULL || !abs_path_ok(path)) {
        file_send_error(c, LXF_E_BADPATH, "TARGET path must be absolute");
        return;
    }
    if (lstat(path, &st) < 0) {
        file_send_error(c, errno == ENOENT ? LXF_E_NOTFOUND : LXF_E_IO, strerror(errno));
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        file_send_error(c, LXF_E_NOTREG, "source is not regular");
        return;
    }
    fs->fd = open(path, O_RDONLY);
    if (fs->fd < 0) {
        file_send_error(c, errno == EACCES ? LXF_E_DENIED : LXF_E_IO, strerror(errno));
        return;
    }
    fs->state = FS_GET;
    fs->count = 0;
    fs->mode = st.st_mode & 0777;
    fs->expected_size = (uint64_t)st.st_size;
    sha256_init(&fs->sha);
    log_msg(a, 1, "file ch%d GET request path=%s size=%llu", idx + 1, path,
            (unsigned long long)fs->expected_size);
    lmux_put_le32(ok, fs->mode);
    lmux_put_le64(ok + 4, fs->expected_size);
    lmux_put_le32(ok + 12, LXF_F_SIZE_KNOWN);
    (void)file_queue_record(c, LXF_T_OK, ok, sizeof(ok));
}

static void file_handle_cwd_request(struct app *a, int idx, const uint8_t *p, uint32_t len)
{
    struct channel *c = &a->ch[idx];
    char proc[64], cwd[LXF_PATH_MAX];
    ssize_t n;
    unsigned int chan;
    struct endpoint *e;

    if (len < 1) {
        file_send_error(c, LXF_E_PROTOCOL, "short CWD request");
        return;
    }
    chan = p[0];
    if (chan < 1 || chan > LMUX_CHANNELS) {
        file_send_error(c, LXF_E_BADPATH, "invalid shell channel");
        return;
    }
    e = &a->ch[chan - 1].ep;
    if (e->type != EP_SHELL) {
        file_send_error(c, LXF_E_BADPATH, "channel is not a supervised shell");
        return;
    }
    if (e->child_pid <= 0) {
        file_send_error(c, LXF_E_IO, "shell is not running");
        return;
    }
    snprintf(proc, sizeof(proc), "/proc/%ld/cwd", (long)e->child_pid);
    n = readlink(proc, cwd, sizeof(cwd) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(cwd) - 1) {
        file_send_error(c, LXF_E_IO, "target cwd unavailable");
        return;
    }
    cwd[n] = 0;
    if (!abs_path_ok(cwd)) {
        file_send_error(c, LXF_E_BADPATH, "target cwd is not absolute");
        return;
    }
    (void)file_queue_record(c, LXF_T_CWD_REPLY, (const uint8_t *)cwd, (uint32_t)(strlen(cwd) + 1));
}

static void file_handle_record(struct app *a, int idx, uint8_t type, const uint8_t *p, uint32_t len)
{
    struct channel *c = &a->ch[idx];
    struct file_service *fs = &c->file;
    if (fs->state == FS_IDLE) {
        if (type == LXF_T_PUT_REQUEST)
            file_handle_put_request(a, idx, p, len);
        else if (type == LXF_T_GET_REQUEST)
            file_handle_get_request(a, idx, p, len);
        else if (type == LXF_T_CWD_REQUEST)
            file_handle_cwd_request(a, idx, p, len);
        else
            file_send_error(c, LXF_E_PROTOCOL, "expected request");
    } else if (fs->state == FS_PUT) {
        if (type == LXF_T_DATA) {
            if (len && write(fs->fd, p, len) != (ssize_t)len) {
                file_send_error(c, errno == ENOSPC ? LXF_E_NOSPACE : LXF_E_IO, strerror(errno));
                return;
            }
            sha256_update(&fs->sha, p, len);
            fs->count += len;
        } else if (type == LXF_T_DONE) {
            uint8_t digest[32], resp[40];
            log_msg(a, 1, "file ch%d PUT DONE", idx + 1);
            if (len < 40) {
                file_send_error(c, LXF_E_PROTOCOL, "short DONE");
                return;
            }
            log_msg(a, 1, "file ch%d PUT received bytes=%llu", idx + 1,
                    (unsigned long long)fs->count);
            sha256_final(&fs->sha, digest);
            if ((fs->size_known && fs->count != fs->expected_size) ||
                lmux_get_le64(p) != fs->count || memcmp(p + 8, digest, 32) != 0) {
                log_msg(a, 1, "file ch%d PUT hash/size mismatch", idx + 1);
                file_send_error(c, LXF_E_HASH, "PUT hash/size mismatch");
                return;
            }
            log_msg(a, 1, "file ch%d PUT hash verified", idx + 1);
            fchmod(fs->fd, fs->mode);
            fsync(fs->fd);
            close(fs->fd);
            fs->fd = -1;
            if (rename(fs->tmp_path, fs->path) < 0) {
                log_msg(a, 1, "file ch%d PUT rename failed: %s", idx + 1, strerror(errno));
                file_send_error(c, LXF_E_IO, strerror(errno));
                return;
            }
            log_msg(a, 1, "file ch%d PUT committed %s", idx + 1, fs->path);
            fs->tmp_path[0] = 0;
            lmux_put_le64(resp, fs->count);
            memcpy(resp + 8, digest, 32);
            fs->state = FS_IDLE;
            (void)file_queue_record(c, LXF_T_OK, resp, sizeof(resp));
            log_msg(a, 1, "file ch%d PUT OK sent", idx + 1);
        } else {
            file_send_error(c, LXF_E_PROTOCOL, "bad PUT message");
        }
    } else {
        file_send_error(c, LXF_E_PROTOCOL, "busy");
    }
}

static void file_pump_output(struct channel *c)
{
    struct file_service *fs = &c->file;
    while (fs->out_pos < fs->out_len && ring_free(&c->tx) > 0) {
        size_t n = fs->out_len - fs->out_pos;
        if (n > ring_free(&c->tx))
            n = ring_free(&c->tx);
        ring_write(&c->tx, fs->outbuf + fs->out_pos, n);
        fs->out_pos += n;
    }
    if (fs->out_pos == fs->out_len)
        fs->out_pos = fs->out_len = 0;
}

static int file_service_step(struct app *a, int idx)
{
    struct channel *c = &a->ch[idx];
    struct file_service *fs = &c->file;
    int progress = 0;
    if (!c->active)
        return 0;
    if (fs->out_pos < fs->out_len && ring_free(&c->tx) > 0)
        progress = 1;
    file_pump_output(c);
    if (fs->out_len != fs->out_pos)
        return progress;

    if (fs->state == FS_GET) {
        uint8_t p[LXF_MAX_PAYLOAD];
        ssize_t r = read(fs->fd, p, sizeof(p));
        if (r > 0) {
            sha256_update(&fs->sha, p, (size_t)r);
            fs->count += (uint64_t)r;
            if (file_queue_record(c, LXF_T_DATA, p, (uint32_t)r) == 0)
                file_pump_output(c);
            return 1;
        } else if (r == 0) {
            uint8_t digest[32], done[40];
            sha256_final(&fs->sha, digest);
            close(fs->fd);
            fs->fd = -1;
            lmux_put_le64(done, fs->count);
            memcpy(done + 8, digest, 32);
            fs->state = FS_IDLE;
            if (file_queue_record(c, LXF_T_DONE, done, sizeof(done)) == 0)
                file_pump_output(c);
            log_msg(a, 1, "file ch%d GET DONE bytes=%llu", idx + 1,
                    (unsigned long long)fs->count);
            return 1;
        } else if (errno != EINTR) {
            file_send_error(c, LXF_E_IO, strerror(errno));
            file_pump_output(c);
            return 1;
        }
    }

    while (fs->out_len == fs->out_pos) {
        const uint8_t *p;
        size_t n, want;

        if (fs->need_len == 0 && fs->in_len >= LXF_HEADER_LEN) {
            uint8_t type, flags;
            uint32_t len;
            if (lxf_decode_header(fs->inbuf, fs->in_len, &type, &flags, &len) < 0) {
                (void)type; (void)flags;
                file_send_error(c, LXF_E_PROTOCOL, "bad record header");
                fs->in_len = fs->need_len = 0;
                progress = 1;
                break;
            }
            fs->need_len = LXF_HEADER_LEN + len;
        }

        want = fs->need_len ? fs->need_len : LXF_HEADER_LEN;
        if (fs->in_len < want) {
            if (c->rx.len == 0)
                break;
            n = ring_peek_linear(&c->rx, &p);
            if (n > want - fs->in_len)
                n = want - fs->in_len;
            if (fs->in_len + n > sizeof(fs->inbuf)) {
                file_send_error(c, LXF_E_PROTOCOL, "record too large");
                progress = 1;
                break;
            }
            memcpy(fs->inbuf + fs->in_len, p, n);
            fs->in_len += n;
            ring_drop(&c->rx, n);
            c->credit_pending += (unsigned int)n;
            progress = 1;
            continue;
        }

        if (fs->need_len && fs->in_len >= fs->need_len) {
            uint8_t type, flags;
            uint32_t len;
            if (lxf_decode_header(fs->inbuf, fs->in_len, &type, &flags, &len) < 0) {
                (void)flags;
                file_send_error(c, LXF_E_PROTOCOL, "bad record header");
                fs->in_len = fs->need_len = 0;
                progress = 1;
                break;
            }
            (void)flags;
            file_handle_record(a, idx, type, fs->inbuf + LXF_HEADER_LEN, len);
            fs->in_len = 0;
            fs->need_len = 0;
            file_pump_output(c);
            progress = 1;
        } else {
            break;
        }
    }
    return progress;
}

static int drain_internal_work(struct app *a)
{
    int budget, progress = 0;
    for (budget = 0; budget < 32; budget++) {
        int i, any = 0;
        for (i = 0; i < LMUX_CHANNELS; i++) {
            if (a->ch[i].ep.type == EP_FILE && file_service_step(a, i))
                any = 1;
        }
        if (!any)
            break;
        progress = 1;
    }
    return progress;
}

static int add_poll(struct pollfd *pfds, int *kind, int *chan, nfds_t *n,
                    int fd, short events, int k, int c)
{
    if (*n >= 16)
        return -1;
    pfds[*n].fd = fd;
    pfds[*n].events = events;
    pfds[*n].revents = 0;
    kind[*n] = k;
    chan[*n] = c;
    (*n)++;
    return 0;
}

static void run_loop(struct app *a)
{
    while (!a->stop) {
        struct pollfd pfds[16];
        int kind[16];
        int chan[16];
        nfds_t n = 0, i;
        int timeout = 1000;

        supervise_shells(a);
        try_open_carrier(a);
        (void)drain_internal_work(a);
        (void)build_next_output(a);

        if (a->carrier_fd >= 0) {
            short ev = POLLIN;
            if (a->out_pos < a->out_len)
                ev |= POLLOUT;
            add_poll(pfds, kind, chan, &n, a->carrier_fd, ev, 1, -1);
        }
        if (a->control_listen_fd >= 0)
            add_poll(pfds, kind, chan, &n, a->control_listen_fd, POLLIN, 4, -1);
        if (a->control_fd >= 0)
            add_poll(pfds, kind, chan, &n, a->control_fd, POLLIN, 5, -1);
        for (i = 0; i < LMUX_CHANNELS; i++) {
            struct channel *c = &a->ch[i];
            if (c->ep.type == EP_SOCKET && c->ep.listen_fd >= 0)
                add_poll(pfds, kind, chan, &n, c->ep.listen_fd, POLLIN, 2, (int)i);
            if (c->ep.fd >= 0 && c->ep.type != EP_FILE) {
                short ev = 0;
                if (c->active) {
                    if (ring_free(&c->tx) > 0)
                        ev |= POLLIN;
                    if (c->rx.len > 0)
                        ev |= POLLOUT;
                }
                if (ev || c->ep.type == EP_SOCKET)
                    add_poll(pfds, kind, chan, &n, c->ep.fd, ev, 3, (int)i);
            }
        }

        if (poll(pfds, n, timeout) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        for (i = 0; i < n; i++) {
            if (!pfds[i].revents)
                continue;
            if (kind[i] == 1) {
                if (pfds[i].revents & POLLIN)
                    feed_carrier(a);
                if (pfds[i].revents & POLLOUT)
                    flush_carrier(a);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    close_carrier(a);
            } else if (kind[i] == 2) {
                if (pfds[i].revents & POLLIN)
                    accept_client(a, chan[i]);
            } else if (kind[i] == 3) {
                if (pfds[i].revents & POLLOUT)
                    deliver_rx(a, chan[i]);
                if (pfds[i].revents & POLLIN)
                    collect_tx(a, chan[i]);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    if (a->ch[chan[i]].ep.type == EP_SOCKET) {
                        if (!a->ch[chan[i]].active && a->ch[chan[i]].local_open)
                            log_msg(a, 1, "channel %d pending local client hangup",
                                    chan[i] + 1);
                        close_socket_client(a, chan[i], 1);
                        log_msg(a, 1, "channel %d waiting state cleared", chan[i] + 1);
                    }
                }
            } else if (kind[i] == 4) {
                if (pfds[i].revents & POLLIN)
                    accept_control(a);
            } else if (kind[i] == 5) {
                if (pfds[i].revents & POLLIN)
                    service_control(a);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    close_fd(&a->control_fd);
            }
        }
        flush_carrier(a);
        (void)drain_internal_work(a);
    }
}

static int parse_ch_path(const char *s, int *idx, char *path, size_t path_cap)
{
    char *end;
    long ch = strtol(s, &end, 10);
    if (ch < 1 || ch > LMUX_CHANNELS || *end != ':')
        return -1;
    if (strlen(end + 1) >= path_cap)
        return -1;
    *idx = (int)ch - 1;
    strcpy(path, end + 1);
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "lmuxd %s (%s)\n"
        "Usage: lmuxd --carrier PATH [--control PATH] [--pty N:SYMLINK] [--shell N:SYMLINK] [--socket N:PATH] [--file N] [--verbose]\n"
        "       lmuxd --help\n"
        "       lmuxd --version\n",
        LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
}

static int parse_args(struct app *a, int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--carrier") == 0) {
            if (++i >= argc)
                return -1;
            a->carrier_path = argv[i];
        } else if (strcmp(argv[i], "--control") == 0) {
            if (++i >= argc)
                return -1;
            a->control_path = argv[i];
        } else if (strcmp(argv[i], "--pty") == 0 || strcmp(argv[i], "--shell") == 0 ||
                   strcmp(argv[i], "--socket") == 0) {
            int idx;
            char path[LMUX_PATH_MAX];
            enum ep_type type = EP_SOCKET;
            if (strcmp(argv[i], "--pty") == 0)
                type = EP_PTY;
            else if (strcmp(argv[i], "--shell") == 0)
                type = EP_SHELL;
            if (++i >= argc || parse_ch_path(argv[i], &idx, path, sizeof(path)) < 0)
                return -1;
            if (a->ch[idx].ep.type != EP_NONE) {
                fprintf(stderr, "channel %d configured more than once\n", idx + 1);
                return -1;
            }
            a->ch[idx].ep.type = type;
            strcpy(a->ch[idx].ep.path, path);
        } else if (strcmp(argv[i], "--file") == 0) {
            int idx;
            char *end;
            long ch;
            if (++i >= argc)
                return -1;
            ch = strtol(argv[i], &end, 10);
            if (*end || ch < 1 || ch > LMUX_CHANNELS)
                return -1;
            idx = (int)ch - 1;
            if (a->ch[idx].ep.type != EP_NONE) {
                fprintf(stderr, "channel %d configured more than once\n", idx + 1);
                return -1;
            }
            a->ch[idx].ep.type = EP_FILE;
            a->ch[idx].local_open = 1;
            a->ch[idx].file.fd = -1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            a->verbose = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("lmuxd %s (%s)\n", LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
            exit(0);
        } else {
            return -1;
        }
    }
    if (!a->carrier_path)
        return -1;
    return 0;
}

static void cleanup(struct app *a)
{
    int i;
    a->stop = 1;
    for (i = 0; i < LMUX_CHANNELS; i++)
        if (a->ch[i].ep.type == EP_SHELL)
            terminate_shell(a, i);
    close_fd(&a->carrier_fd);
    close_fd(&a->control_fd);
    close_fd(&a->control_listen_fd);
    if (a->control_path)
        unlink(a->control_path);
    for (i = 0; i < LMUX_CHANNELS; i++) {
        struct endpoint *e = &a->ch[i].ep;
        if (e->type == EP_FILE)
            file_reset(&a->ch[i]);
        close_fd(&e->fd);
        close_fd(&e->listen_fd);
        if (e->created_path)
            unlink(e->path);
    }
}

static void compute_local_features(struct app *a)
{
    int i;
    a->local_features = LMUX_FEATURE_CWD_QUERY | LMUX_FEATURE_WINSIZE | LMUX_FEATURE_STREAM_GEN;
    for (i = 0; i < LMUX_CHANNELS; i++)
        if (a->ch[i].ep.type == EP_FILE)
            a->local_features |= LMUX_FEATURE_FILE;
}

static void on_signal(int sig)
{
    (void)sig;
    if (global_app)
        global_app->stop = 1;
}

#ifndef LMUXD_NO_MAIN
int main(int argc, char **argv)
{
    struct app a;
    int i;

    memset(&a, 0, sizeof(a));
    a.carrier_fd = -1;
    a.control_listen_fd = -1;
    a.control_fd = -1;
    for (i = 0; i < LMUX_CHANNELS; i++) {
        a.ch[i].ep.fd = -1;
        a.ch[i].ep.listen_fd = -1;
        a.ch[i].file.fd = -1;
    }
    if (argc == 1 || parse_args(&a, argc, argv) < 0) {
        usage(stderr);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    global_app = &a;
    compute_local_features(&a);
    make_session(&a);
    fprintf(stderr, "lmuxd %s (%s)\n", LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);

    for (i = 0; i < LMUX_CHANNELS; i++) {
        if (a.ch[i].ep.type == EP_PTY || a.ch[i].ep.type == EP_SHELL) {
            if (create_pty(&a, i) < 0) {
                cleanup(&a);
                return 1;
            }
            if (a.ch[i].ep.type == EP_SHELL)
                spawn_shell(&a, i);
        } else if (a.ch[i].ep.type == EP_SOCKET) {
            if (create_socket_listener(&a, i) < 0) {
                cleanup(&a);
                return 1;
            }
        }
    }
    if (create_control_listener(&a) < 0) {
        cleanup(&a);
        return 1;
    }

    run_loop(&a);
    cleanup(&a);
    return 0;
}
#endif
