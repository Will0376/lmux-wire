#include "session.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RX_BUF_MAX 16384

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void lmux_zmx_session_init(struct lmux_zmx_session *s)
{
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->timeout_ms = 5000;
}

int lmux_zmx_session_use_fd(struct lmux_zmx_session *s, int fd,
                            int owns_fd, unsigned timeout_ms)
{
    lmux_zmx_session_init(s);
    if (fd < 0)
        return -1;
    if (set_nonblock(fd) < 0)
        return -1;
    s->fd = fd;
    s->owns_fd = owns_fd;
    s->timeout_ms = timeout_ms ? timeout_ms : 5000;
    s->nonce_counter = ((uint64_t)getpid() << 32) ^ (uint64_t)now_ms();
    return 0;
}

int lmux_zmx_session_open_tty(struct lmux_zmx_session *s, const char *path,
                              const struct lmux_serial_config *serial,
                              unsigned timeout_ms)
{
    int fd;
    struct termios saved;
    lmux_zmx_session_init(s);
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (tcgetattr(fd, &saved) == 0) {
        s->saved_termios = saved;
        s->restore_termios = 1;
    }
    if (lmux_serial_configure_fd(fd, serial) < 0) {
        if (s->restore_termios)
            (void)tcsetattr(fd, TCSANOW, &s->saved_termios);
        close(fd);
        return -1;
    }
    s->fd = fd;
    s->owns_fd = 1;
    s->timeout_ms = timeout_ms ? timeout_ms : 5000;
    s->nonce_counter = ((uint64_t)getpid() << 32) ^ (uint64_t)now_ms();
    return 0;
}

void lmux_zmx_session_close(struct lmux_zmx_session *s)
{
    if (!s)
        return;
    if (s->fd >= 0 && s->restore_termios)
        (void)tcsetattr(s->fd, TCSANOW, &s->saved_termios);
    if (s->fd >= 0 && s->owns_fd)
        close(s->fd);
    s->fd = -1;
    s->owns_fd = 0;
    s->restore_termios = 0;
}

const char *lmux_zmx_error_name(enum lmux_zmx_error err)
{
    switch (err) {
    case LMUX_ZMX_OK: return "OK";
    case LMUX_ZMX_AUTH_REQUIRED: return "AUTH_REQUIRED";
    case LMUX_ZMX_NO_USABLE_SHELL: return "NO_USABLE_SHELL";
    case LMUX_ZMX_TIMEOUT: return "TIMEOUT";
    case LMUX_ZMX_CARRIER_LOST: return "CARRIER_LOST";
    case LMUX_ZMX_PROTOCOL_ERROR: return "PROTOCOL_ERROR";
    case LMUX_ZMX_IO_ERROR: return "IO_ERROR";
    }
    return "?";
}

static int full_write_deadline(struct lmux_zmx_session *s, const char *buf,
                               size_t len, long long deadline)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(s->fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            int timeout = (int)(deadline - now_ms());
            if (timeout <= 0) {
                s->last_error = LMUX_ZMX_TIMEOUT;
                return -1;
            }
            memset(&pfd, 0, sizeof(pfd));
            pfd.fd = s->fd;
            pfd.events = POLLOUT;
            if (poll(&pfd, 1, timeout) < 0) {
                if (errno == EINTR)
                    continue;
                s->last_error = LMUX_ZMX_IO_ERROR;
                return -1;
            }
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                s->last_error = LMUX_ZMX_CARRIER_LOST;
                return -1;
            }
            continue;
        }
        s->last_error = LMUX_ZMX_IO_ERROR;
        return -1;
    }
    return 0;
}

struct lmux_zmx_stream {
    struct lmux_zmx_session *session;
    struct lmux_zmx_command_result *res;
    long long deadline;
    unsigned timeout_ms;
    const char *begin;
    const char *ready;
    const char *data_end;
    const char *end_prefix;
    char line[1024];
    size_t line_len;
    int in_tx;
    int ready_seen;
    int done;
};

static void stream_progress(struct lmux_zmx_stream *st)
{
    st->deadline = now_ms() + (long long)st->timeout_ms;
}

static void set_phase(struct lmux_zmx_session *s, const char *phase)
{
    snprintf(s->last_phase, sizeof(s->last_phase), "%s", phase ? phase : "");
}

static void make_nonce(struct lmux_zmx_session *s, char nonce[LMUX_ZMX_NONCE_MAX])
{
    unsigned int a = 0, b = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        uint32_t r[2];
        if (read(fd, r, sizeof(r)) == (ssize_t)sizeof(r)) {
            a = r[0];
            b = r[1];
        }
        close(fd);
    }
    s->nonce_counter++;
    snprintf(nonce, LMUX_ZMX_NONCE_MAX, "%08x%08x%08llx",
             a, b, (unsigned long long)s->nonce_counter);
}

static int line_contains_auth_prompt(const char *line)
{
    const char *p = line;
    char lower[256];
    size_t i = 0;
    while (*p && i + 1 < sizeof(lower)) {
        lower[i++] = (char)tolower((unsigned char)*p++);
    }
    lower[i] = 0;
    return strstr(lower, "login:") || strstr(lower, "password:");
}

static int parse_status(const char *line, const char *prefix, int *status)
{
    const char *p;
    char *end = NULL;
    long v;
    if (strncmp(line, prefix, strlen(prefix)) != 0)
        return 0;
    p = line + strlen(prefix);
    if (!*p)
        return -1;
    v = strtol(p, &end, 10);
    if (!end || *end || v < 0 || v > 255)
        return -1;
    *status = (int)v;
    return 1;
}

static int append_output(struct lmux_zmx_command_result *res,
                         const char *line, size_t len)
{
    if (res->output_len + len + 1 >= sizeof(res->output))
        return -1;
    memcpy(res->output + res->output_len, line, len);
    res->output_len += len;
    res->output[res->output_len++] = '\n';
    res->output[res->output_len] = 0;
    return 0;
}

static int process_line(struct lmux_zmx_session *s,
                        struct lmux_zmx_command_result *res,
                        const char *line, const char *begin,
                        const char *end_prefix, int *in_tx)
{
    int st, p;
    if (!*in_tx) {
        if (strcmp(line, begin) == 0) {
            *in_tx = 1;
            return 0;
        }
        if (line_contains_auth_prompt(line)) {
            s->last_error = LMUX_ZMX_AUTH_REQUIRED;
            return -1;
        }
        return 0;
    }
    p = parse_status(line, end_prefix, &st);
    if (p == 1) {
        res->exit_status = st;
        return 1;
    }
    if (p < 0) {
        s->last_error = LMUX_ZMX_PROTOCOL_ERROR;
        return -1;
    }
    if (strncmp(line, "ZMX_BEGIN_", 10) == 0 ||
        strncmp(line, "ZMX_END_", 8) == 0) {
        s->last_error = LMUX_ZMX_PROTOCOL_ERROR;
        return -1;
    }
    if (append_output(res, line, strlen(line)) < 0) {
        s->last_error = LMUX_ZMX_PROTOCOL_ERROR;
        return -1;
    }
    return 0;
}

static int stream_process_rx(struct lmux_zmx_stream *st)
{
    char buf[512];
    ssize_t n;
    n = read(st->session->fd, buf, sizeof(buf));
    if (n > 0) {
        ssize_t i;
        for (i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\r')
                continue;
            st->line[st->line_len] = 0;
            if (!st->in_tx && line_contains_auth_prompt(st->line)) {
                st->session->last_error = LMUX_ZMX_AUTH_REQUIRED;
                return -1;
            }
            if (ch == '\n') {
                int r;
                st->line[st->line_len] = 0;
                if (st->ready && !st->ready_seen) {
                    if (strcmp(st->line, st->ready) == 0)
                        st->ready_seen = 1;
                    st->line_len = 0;
                    continue;
                }
                if (st->ready) {
                    int status, p;
                    p = parse_status(st->line, st->end_prefix, &status);
                    if (p == 1) {
                        st->res->exit_status = status;
                        st->done = 1;
                        st->session->last_error = LMUX_ZMX_OK;
                        st->line_len = 0;
                        return 1;
                    }
                    st->line_len = 0;
                    continue;
                }
                r = process_line(st->session, st->res, st->line,
                                 st->begin, st->end_prefix, &st->in_tx);
                st->line_len = 0;
                if (r == 1) {
                    st->done = 1;
                    st->session->last_error = LMUX_ZMX_OK;
                    return 1;
                }
                if (r < 0)
                    return -1;
            } else if (st->line_len + 1 < sizeof(st->line)) {
                st->line[st->line_len++] = ch;
            } else {
                st->session->last_error = LMUX_ZMX_PROTOCOL_ERROR;
                return -1;
            }
        }
        return 0;
    }
    if (n == 0) {
        st->session->last_error = LMUX_ZMX_CARRIER_LOST;
        return -1;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return 0;
    st->session->last_error = LMUX_ZMX_IO_ERROR;
    return -1;
}

int lmux_zmx_stream_write(struct lmux_zmx_stream *st,
                          const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd;
        int timeout = (int)(st->deadline - now_ms());
        ssize_t n;
        if (timeout <= 0) {
            st->session->last_error = LMUX_ZMX_TIMEOUT;
            return -1;
        }
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = st->session->fd;
        pfd.events = POLLOUT | POLLIN;
        if (poll(&pfd, 1, timeout) < 0) {
            if (errno == EINTR)
                continue;
            st->session->last_error = LMUX_ZMX_IO_ERROR;
            return -1;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            st->session->last_error = LMUX_ZMX_CARRIER_LOST;
            return -1;
        }
        if (pfd.revents & POLLIN) {
            if (stream_process_rx(st) < 0)
                return -1;
            stream_progress(st);
        }
        if (!(pfd.revents & POLLOUT))
            continue;
        n = write(st->session->fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            stream_progress(st);
            continue;
        }
        if (n < 0 && (errno == EINTR ||
                      errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        st->session->last_error = LMUX_ZMX_IO_ERROR;
        return -1;
    }
    return 0;
}

int lmux_zmx_session_command(struct lmux_zmx_session *s, const char *command,
                             struct lmux_zmx_command_result *res)
{
    char nonce[LMUX_ZMX_NONCE_MAX];
    char begin[80], end_prefix[96], wire[1024];
    char rx[RX_BUF_MAX], line[1024];
    size_t rx_len = 0, line_len = 0;
    int in_tx = 0;
    long long deadline;

    if (!s || s->fd < 0 || !command || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    res->exit_status = -1;
    make_nonce(s, nonce);
    snprintf(begin, sizeof(begin), "ZMX_BEGIN_%s", nonce);
    snprintf(end_prefix, sizeof(end_prefix), "ZMX_END_%s_", nonce);
    snprintf(wire, sizeof(wire),
             "\nprintf '%s\\n'; ( %s ); zmx_s=$?; printf 'ZMX_END_%s_%%d\\n' \"$zmx_s\"\n",
             begin, command, nonce);
    deadline = now_ms() + (long long)s->timeout_ms;
    if (full_write_deadline(s, wire, strlen(wire), deadline) < 0)
        return -1;

    for (;;) {
        ssize_t n;
        struct pollfd pfd;
        int timeout = (int)(deadline - now_ms());
        if (timeout <= 0) {
            s->last_error = in_tx ? LMUX_ZMX_PROTOCOL_ERROR : LMUX_ZMX_TIMEOUT;
            return -1;
        }
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = s->fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, timeout) < 0) {
            if (errno == EINTR)
                continue;
            s->last_error = LMUX_ZMX_IO_ERROR;
            return -1;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            s->last_error = LMUX_ZMX_CARRIER_LOST;
            return -1;
        }
        if (!(pfd.revents & POLLIN))
            continue;
        n = read(s->fd, rx + rx_len, sizeof(rx) - rx_len);
        if (n > 0) {
            size_t i;
            rx_len += (size_t)n;
            for (i = 0; i < rx_len; i++) {
                char ch = rx[i];
                if (ch == '\r')
                    continue;
                line[line_len] = 0;
                if (!in_tx && line_contains_auth_prompt(line)) {
                    s->last_error = LMUX_ZMX_AUTH_REQUIRED;
                    return -1;
                }
                if (ch == '\n') {
                    int r;
                    line[line_len] = 0;
                    r = process_line(s, res, line, begin, end_prefix, &in_tx);
                    line_len = 0;
                    if (r == 1) {
                        s->last_error = LMUX_ZMX_OK;
                        return 0;
                    }
                    if (r < 0)
                        return -1;
                } else if (line_len + 1 < sizeof(line)) {
                    line[line_len++] = ch;
                } else {
                    s->last_error = LMUX_ZMX_PROTOCOL_ERROR;
                    return -1;
                }
            }
            rx_len = 0;
            continue;
        }
        if (n == 0) {
            s->last_error = LMUX_ZMX_CARRIER_LOST;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        s->last_error = LMUX_ZMX_IO_ERROR;
        return -1;
    }
}

int lmux_zmx_session_stream_command(struct lmux_zmx_session *s,
                                    const char *before_payload,
                                    lmux_zmx_stream_payload_fn payload,
                                    void *payload_ctx,
                                    struct lmux_zmx_command_result *res)
{
    char nonce[LMUX_ZMX_NONCE_MAX];
    char begin[80], end_prefix[96], prefix[1200], suffix[160];
    struct lmux_zmx_stream st;
    long long deadline;

    if (!s || s->fd < 0 || !before_payload || !payload || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    res->exit_status = -1;
    make_nonce(s, nonce);
    snprintf(begin, sizeof(begin), "ZMX_BEGIN_%s", nonce);
    snprintf(end_prefix, sizeof(end_prefix), "ZMX_END_%s_", nonce);
    snprintf(prefix, sizeof(prefix), "\nprintf '%s\\n'; %s", begin,
             before_payload);
    snprintf(suffix, sizeof(suffix),
             "\nzmx_s=$?; printf 'ZMX_END_%s_%%d\\n' \"$zmx_s\"\n",
             nonce);
    deadline = now_ms() + (long long)s->timeout_ms;
    memset(&st, 0, sizeof(st));
    st.session = s;
    st.res = res;
    st.deadline = deadline;
    st.timeout_ms = s->timeout_ms;
    st.begin = begin;
    st.end_prefix = end_prefix;

    if (lmux_zmx_stream_write(&st, prefix, strlen(prefix)) < 0)
        return -1;
    if (payload(&st, payload_ctx) < 0)
        return -1;
    if (lmux_zmx_stream_write(&st, suffix, strlen(suffix)) < 0)
        return -1;

    while (!st.done) {
        struct pollfd pfd;
        int timeout = (int)(deadline - now_ms());
        if (timeout <= 0) {
            s->last_error = st.in_tx ? LMUX_ZMX_PROTOCOL_ERROR : LMUX_ZMX_TIMEOUT;
            return -1;
        }
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = s->fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, timeout) < 0) {
            if (errno == EINTR)
                continue;
            s->last_error = LMUX_ZMX_IO_ERROR;
            return -1;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            s->last_error = LMUX_ZMX_CARRIER_LOST;
            return -1;
        }
        if ((pfd.revents & POLLIN) && stream_process_rx(&st) < 0)
            return -1;
    }
    return 0;
}

static int stream_wait(struct lmux_zmx_stream *st, int want_ready)
{
    while (!st->done && (!want_ready || !st->ready_seen)) {
        struct pollfd pfd;
        int timeout = (int)(st->deadline - now_ms());
        if (timeout <= 0) {
            st->session->last_error = LMUX_ZMX_TIMEOUT;
            return -1;
        }
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = st->session->fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, timeout) < 0) {
            if (errno == EINTR)
                continue;
            st->session->last_error = LMUX_ZMX_IO_ERROR;
            return -1;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            st->session->last_error = LMUX_ZMX_CARRIER_LOST;
            return -1;
        }
        if ((pfd.revents & POLLIN) && stream_process_rx(st) < 0)
            return -1;
        if (pfd.revents & POLLIN)
            stream_progress(st);
    }
    return 0;
}

int lmux_zmx_session_stream_foreground(struct lmux_zmx_session *s,
                                       const char *start_fmt,
                                       const char *command_arg,
                                       lmux_zmx_stream_payload_fn payload,
                                       void *payload_ctx,
                                       struct lmux_zmx_command_result *res)
{
    char nonce[LMUX_ZMX_NONCE_MAX];
    char ready[80], data_end[96], end_prefix[96], start[1600];
    struct lmux_zmx_stream st;

    if (!s || s->fd < 0 || !start_fmt || !payload || !res)
        return -1;
    memset(res, 0, sizeof(*res));
    res->exit_status = -1;
    make_nonce(s, nonce);
    snprintf(ready, sizeof(ready), "ZMX_READY_%s", nonce);
    snprintf(data_end, sizeof(data_end), "~ZMX_DATA_END_%s\n", nonce);
    snprintf(end_prefix, sizeof(end_prefix), "ZMX_END_%s_", nonce);
    if (snprintf(start, sizeof(start), start_fmt, nonce,
                 command_arg ? command_arg : "", nonce) >= (int)sizeof(start)) {
        s->last_error = LMUX_ZMX_PROTOCOL_ERROR;
        return -1;
    }

    memset(&st, 0, sizeof(st));
    st.session = s;
    st.res = res;
    st.timeout_ms = s->timeout_ms;
    st.ready = ready;
    st.data_end = data_end;
    st.end_prefix = end_prefix;
    stream_progress(&st);

    set_phase(s, "starting");
    if (lmux_zmx_stream_write(&st, start, strlen(start)) < 0)
        return -1;
    set_phase(s, "waiting-ready");
    if (stream_wait(&st, 1) < 0)
        return -1;
    set_phase(s, "streaming");
    if (payload(&st, payload_ctx) < 0)
        return -1;
    if (lmux_zmx_stream_write(&st, data_end, strlen(data_end)) < 0)
        return -1;
    set_phase(s, "waiting-end");
    if (stream_wait(&st, 0) < 0)
        return -1;
    set_phase(s, "");
    return 0;
}

int lmux_zmx_session_establish(struct lmux_zmx_session *s)
{
    struct lmux_zmx_command_result res;
    if (full_write_deadline(s, "\n", 1, now_ms() + (long long)s->timeout_ms) < 0)
        return -1;
    if (lmux_zmx_session_command(s, ":", &res) < 0)
        return -1;
    if (res.exit_status != 0) {
        s->last_error = LMUX_ZMX_NO_USABLE_SHELL;
        return -1;
    }
    return 0;
}
