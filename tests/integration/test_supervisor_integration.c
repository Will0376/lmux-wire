#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#include "boot_protocol.h"
#include "carrier_manager.h"
#include "dirty.h"
#include "host_control_structured.h"
#include "link_protocol.h"
#include "lmc1.h"
#include "lxf.h"
#include "lmux.h"
#include "services.h"

static int fail(const char *msg)
{
    fprintf(stderr, "supervisor integration failed: %s\n", msg);
    return 1;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd_event(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms) > 0 ? pfd.revents : 0;
}

static int test_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int connect_unix_test(const char *path)
{
    int fd, err = 0;
    socklen_t errlen = sizeof(err);
    struct sockaddr_un sa;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (errno != EINPROGRESS ||
            !(wait_fd_event(fd, POLLOUT | POLLERR | POLLHUP | POLLNVAL, 1000) & POLLOUT) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int wait_connect(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int fd = connect_unix_test(path);
        if (fd >= 0)
            return fd;
        usleep(20000);
    }
    return -1;
}

static int wait_path_exists(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        struct stat st;
        if (lstat(path, &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int path_exists_now(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

struct saved_env {
    int had;
    char *value;
};

static int save_env_var(struct saved_env *saved, const char *name)
{
    const char *value = getenv(name);
    saved->had = value != NULL;
    saved->value = value ? strdup(value) : NULL;
    return value && !saved->value ? -1 : 0;
}

static void restore_env_var(const char *name, struct saved_env *saved)
{
    if (saved->had)
        setenv(name, saved->value ? saved->value : "", 1);
    else
        unsetenv(name);
    free(saved->value);
    saved->value = NULL;
    saved->had = 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w > 0) {
            p += w;
            len -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            (void)wait_fd_event(fd, POLLOUT, 1000);
        } else {
            return -1;
        }
    }
    return 0;
}

static int hex_encode_bytes(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char hexdig[] = "0123456789abcdef";
    size_t i;
    if (cap < len * 2 + 1)
        return -1;
    for (i = 0; i < len; i++) {
        out[i * 2] = hexdig[in[i] >> 4];
        out[i * 2 + 1] = hexdig[in[i] & 15];
    }
    out[len * 2] = 0;
    return 0;
}

static int hex_encode_cstr(const char *in, char *out, size_t cap)
{
    return hex_encode_bytes((const uint8_t *)in, strlen(in), out, cap);
}

static int read_until(int fd, const char *needle, char *out, size_t cap, int timeout_ms)
{
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    if (cap)
        out[0] = 0;
    while (now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r;
            if (len + 1 >= cap)
                return -1;
            r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                if (strstr(out, needle))
                    return 0;
            } else if (r == 0) {
                return -1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return -1;
            }
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int shell_cmd_expect(int fd, const char *cmd, const char *needle,
                            char *out, size_t cap)
{
    return write_all(fd, cmd, strlen(cmd)) < 0 ||
           write_all(fd, "\n", 1) < 0 ||
           read_until(fd, needle, out, cap, 7000) < 0 ? -1 : 0;
}

static int extract_cpid(const char *text, char *out, size_t cap)
{
    const char *start = strstr(text, "CPID:");
    const char *end;
    size_t len;
    if (!start || !out || cap == 0)
        return -1;
    start += 5;
    end = strstr(start, ":END");
    if (!end || end == start)
        return -1;
    len = (size_t)(end - start);
    if (len >= cap)
        return -1;
    memcpy(out, start, len);
    out[len] = 0;
    return 0;
}

static int make_file(const char *path, size_t len);

static int read_exact_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t off = 0;
    long long end = now_ms() + timeout_ms;
    while (off < len && now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, buf + off, len - off);
            if (r > 0) {
                off += (size_t)r;
                continue;
            }
            if (r == 0)
                return -1;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return off == len ? 0 : -1;
}

static int supervisor_unix_exchange(const char *host_sock, const char *target_sock,
                                    int target_first, uint8_t tag)
{
    uint8_t h_msg[] = { 'H', tag, 0x00, 0xff, 0x03, 0x7f };
    uint8_t t_msg[] = { 'T', tag, 0xff, 0x00, 0x7f, 0x03 };
    uint8_t got[sizeof(h_msg)];
    int h = -1, t = -1;
    int rc = -1;

    if (target_first) {
        t = connect_unix_test(target_sock);
        if (t < 0 || write_all(t, t_msg, sizeof(t_msg)) < 0)
            goto out;
        h = connect_unix_test(host_sock);
        if (h < 0 ||
            read_exact_timeout(h, got, sizeof(t_msg), 5000) < 0 ||
            memcmp(got, t_msg, sizeof(t_msg)) != 0)
            goto out;
    } else {
        h = connect_unix_test(host_sock);
        if (h < 0 || write_all(h, h_msg, sizeof(h_msg)) < 0)
            goto out;
        t = connect_unix_test(target_sock);
        if (t < 0 ||
            read_exact_timeout(t, got, sizeof(h_msg), 5000) < 0 ||
            memcmp(got, h_msg, sizeof(h_msg)) != 0)
            goto out;
    }
    if (write_all(h, h_msg, sizeof(h_msg)) < 0 ||
        read_exact_timeout(t, got, sizeof(h_msg), 5000) < 0 ||
        memcmp(got, h_msg, sizeof(h_msg)) != 0 ||
        write_all(t, t_msg, sizeof(t_msg)) < 0 ||
        read_exact_timeout(h, got, sizeof(t_msg), 5000) < 0 ||
        memcmp(got, t_msg, sizeof(t_msg)) != 0)
        goto out;
    rc = 0;
out:
    if (h >= 0)
        close(h);
    if (t >= 0)
        close(t);
    return rc;
}

static int read_status(const char *control, char *out, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t len = 0;
    long long end = now_ms() + 2000;
    if (fd < 0 || cap == 0)
        return -1;
    out[0] = 0;
    if (write_all(fd, "STATUS\n", 7) < 0) {
        close(fd);
        return -1;
    }
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                continue;
            }
            if (r == 0) {
                close(fd);
                return len ? 0 : -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            close(fd);
            return len ? 0 : -1;
        }
    }
    close(fd);
    return len ? 0 : -1;
}

static int control_request_test(const char *control, const char *req,
                                char *out, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t len = 0;
    long long end = now_ms() + 5000;
    if (fd < 0 || cap == 0)
        return -1;
    out[0] = 0;
    if (write_all(fd, req, strlen(req)) < 0) {
        close(fd);
        return -1;
    }
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                continue;
            }
            if (r == 0) {
                close(fd);
                return len ? 0 : -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            close(fd);
            return len ? 0 : -1;
        }
    }
    close(fd);
    return len ? 0 : -1;
}

static int control_request_timeout_test(const char *control, const char *req,
                                        int timeout_ms, char *out, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    if (fd < 0 || cap == 0)
        return -1;
    out[0] = 0;
    if (write_all(fd, req, strlen(req)) < 0) {
        close(fd);
        return -1;
    }
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                continue;
            }
            if (r == 0) {
                close(fd);
                return len ? 0 : -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            close(fd);
            return len ? 0 : -1;
        }
    }
    close(fd);
    return len ? 0 : -1;
}

static int exec_request_test(const char *control, const char *req,
                             char *out, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t len = 0;
    long long end = now_ms() + 20000;
    int last_re = 0, last_errno = 0, saw_eof = 0;
    if (fd < 0 || cap == 0) {
        fprintf(stderr, "exec_request_test setup failed fd=%d cap=%zu errno=%d req=%.80s",
                fd, cap, errno, req);
        return -1;
    }
    out[0] = 0;
    if (write_all(fd, req, strlen(req)) < 0) {
        fprintf(stderr, "exec_request_test write failed errno=%d req=%.80s",
                errno, req);
        close(fd);
        return -1;
    }
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re)
            last_re = re;
        if (re & POLLIN) {
            ssize_t r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                if (strstr(out, "\nDONE\n") || strstr(out, "ERR EXEC"))
                    break;
                continue;
            }
            if (r == 0) {
                saw_eof = 1;
                break;
            }
            last_errno = errno;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            fprintf(stderr, "exec_request_test read failed errno=%d re=0x%x req=%.80s",
                    errno, re, req);
            close(fd);
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            int retry = 0;
            for (;;) {
                ssize_t r = read(fd, out + len, cap - len - 1);
                if (r > 0) {
                    len += (size_t)r;
                    out[len] = 0;
                    if (len + 1 >= cap ||
                        strstr(out, "\nDONE\n") || strstr(out, "ERR EXEC"))
                        break;
                    continue;
                }
                if (r < 0 && errno == EINTR)
                    continue;
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    last_errno = errno;
                    retry = 1;
                    break;
                }
                if (r == 0)
                    saw_eof = 1;
                else if (r < 0)
                    last_errno = errno;
                break;
            }
            if (retry)
                continue;
            break;
        }
    }
    close(fd);
    if (!len)
        fprintf(stderr, "exec_request_test empty response re=0x%x errno=%d eof=%d req=%.80s",
                last_re, last_errno, saw_eof, req);
    return len ? 0 : -1;
}

static int parse_exec_id_line(const char *text, uint64_t *out)
{
    const char *p = strstr(text, "OK EXEC exec_id=");
    unsigned long long id;
    if (!p || sscanf(p, "OK EXEC exec_id=%llu", &id) != 1 || id == 0)
        return -1;
    *out = (uint64_t)id;
    return 0;
}

static void hc2_test_put_u64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)v);
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t hc2_test_get_u64(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) |
        ((uint64_t)lmux_get_le32(p + 4) << 32);
}

static int hc2_send_frame(int fd, uint16_t type, uint64_t req,
                          const uint8_t *payload, uint32_t len)
{
    uint8_t hdr[LMUX_HC2_HEADER_LEN];
    if (len > LMUX_HC2_MAX_PAYLOAD)
        return -1;
    lmux_put_le32(hdr, LMUX_HC2_MAGIC);
    lmux_put_le16(hdr + 4, LMUX_HC2_VERSION);
    lmux_put_le16(hdr + 6, type);
    lmux_put_le16(hdr + 8, 0);
    lmux_put_le16(hdr + 10, 0);
    hc2_test_put_u64(hdr + 12, req);
    lmux_put_le32(hdr + 20, len);
    return write_all(fd, hdr, sizeof(hdr)) < 0 ||
           (len && write_all(fd, payload, len) < 0) ? -1 : 0;
}

static size_t hc2_build_frame(uint8_t *out, size_t cap, uint16_t type,
                              uint64_t req, const uint8_t *payload,
                              uint32_t len)
{
    if (cap < LMUX_HC2_HEADER_LEN + len || len > LMUX_HC2_MAX_PAYLOAD)
        return 0;
    lmux_put_le32(out, LMUX_HC2_MAGIC);
    lmux_put_le16(out + 4, LMUX_HC2_VERSION);
    lmux_put_le16(out + 6, type);
    lmux_put_le16(out + 8, 0);
    lmux_put_le16(out + 10, 0);
    hc2_test_put_u64(out + 12, req);
    lmux_put_le32(out + 20, len);
    if (len)
        memcpy(out + LMUX_HC2_HEADER_LEN, payload, len);
    return LMUX_HC2_HEADER_LEN + len;
}

enum {
    HC2_READ_OK = 0,
    HC2_READ_TIMEOUT = -2,
    HC2_READ_EOF = -3,
    HC2_READ_PROTOCOL = -4,
    HC2_READ_IO = -5
};

struct hc2_reader {
    int initialized;
    uint8_t buf[LMUX_HC2_HEADER_LEN + LMUX_HC2_MAX_PAYLOAD];
    size_t have;
    size_t need;
};

static struct hc2_reader hc2_readers[FD_SETSIZE];

static void hc2_reader_reset_fd(int fd)
{
    if (fd >= 0 && fd < FD_SETSIZE)
        memset(&hc2_readers[fd], 0, sizeof(hc2_readers[fd]));
}

static int hc2_read_frame_limit(int fd, uint16_t *type, uint64_t *req,
                                uint8_t *payload, uint32_t *len,
                                int timeout_ms, size_t read_limit)
{
    struct hc2_reader *rd;
    long long end = now_ms() + timeout_ms;
    if (fd < 0 || fd >= FD_SETSIZE)
        return HC2_READ_IO;
    rd = &hc2_readers[fd];
    if (!rd->initialized) {
        rd->initialized = 1;
        rd->need = LMUX_HC2_HEADER_LEN;
    }
    while (now_ms() < end) {
        uint32_t payload_len;
        size_t want;
        if (rd->have >= rd->need) {
            if (rd->need == LMUX_HC2_HEADER_LEN) {
                if (lmux_get_le32(rd->buf) != LMUX_HC2_MAGIC ||
                    lmux_get_le16(rd->buf + 4) != LMUX_HC2_VERSION) {
                    hc2_reader_reset_fd(fd);
                    return HC2_READ_PROTOCOL;
                }
                payload_len = lmux_get_le32(rd->buf + 20);
                if (payload_len > LMUX_HC2_MAX_PAYLOAD) {
                    hc2_reader_reset_fd(fd);
                    return HC2_READ_PROTOCOL;
                }
                rd->need = LMUX_HC2_HEADER_LEN + payload_len;
                if (rd->have >= rd->need)
                    continue;
            } else {
                payload_len = lmux_get_le32(rd->buf + 20);
                *type = lmux_get_le16(rd->buf + 6);
                *req = hc2_test_get_u64(rd->buf + 12);
                *len = payload_len;
                if (payload_len)
                    memcpy(payload, rd->buf + LMUX_HC2_HEADER_LEN, payload_len);
                hc2_reader_reset_fd(fd);
                return HC2_READ_OK;
            }
        }
        want = rd->need - rd->have;
        if (want > read_limit)
            want = read_limit;
        {
        int rem = (int)(end - now_ms());
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL,
                               rem < 100 ? rem : 100);
        if (re & (POLLIN | POLLHUP)) {
            ssize_t r = read(fd, rd->buf + rd->have, want);
            if (r > 0) {
                rd->have += (size_t)r;
                continue;
            }
            if (r == 0) {
                hc2_reader_reset_fd(fd);
                return HC2_READ_EOF;
            }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            hc2_reader_reset_fd(fd);
            return HC2_READ_IO;
        }
        if (re & (POLLERR | POLLNVAL)) {
            hc2_reader_reset_fd(fd);
            return HC2_READ_EOF;
        }
        }
    }
    return HC2_READ_TIMEOUT;
}

static int hc2_read_frame(int fd, uint16_t *type, uint64_t *req,
                          uint8_t *payload, uint32_t *len, int timeout_ms)
{
    return hc2_read_frame_limit(fd, type, req, payload, len, timeout_ms,
                                LMUX_HC2_HEADER_LEN + LMUX_HC2_MAX_PAYLOAD);
}

static int hc2_read_frame_tiny(int fd, uint16_t *type, uint64_t *req,
                               uint8_t *payload, uint32_t *len,
                               int timeout_ms)
{
    return hc2_read_frame_limit(fd, type, req, payload, len, timeout_ms, 1);
}

static int hc2_expect_error(int fd, uint64_t want_req, uint32_t want_code)
{
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type = 0;
    uint64_t req = 0;
    uint32_t len = 0;
    if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0 ||
        type != LMUX_HC2_ERROR || req != want_req || len < 4 ||
        lmux_get_le32(payload) != want_code) {
        fprintf(stderr,
                "hc2_expect_error want_req=%llu want_code=%u got_type=%u got_req=%llu got_len=%u got_code=%u\n",
                (unsigned long long)want_req, want_code, type,
                (unsigned long long)req, len,
                len >= 4 ? lmux_get_le32(payload) : 0);
        return -1;
    }
    return 0;
}

static int hc2_reader_fragmentation_selftest(void)
{
    uint8_t frame[LMUX_HC2_HEADER_LEN + 64], payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req;
    uint32_t len;
    int sp[2] = {-1, -1};
    size_t flen;
    const char *stage = "header split";
    const uint8_t binary[] = {0x00, 0xff, 0x41, 0x0a, 0x7f};

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return -1;
    flen = hc2_build_frame(frame, sizeof(frame), LMUX_HC2_STATUS_REPLY, 101,
                           binary, sizeof(binary));
    if (!flen || write_all(sp[0], frame, 5) < 0 ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 10) != HC2_READ_TIMEOUT ||
        write_all(sp[0], frame + 5, flen - 5) < 0 ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 1000) != HC2_READ_OK ||
        type != LMUX_HC2_STATUS_REPLY || req != 101 ||
        len != sizeof(binary) || memcmp(payload, binary, sizeof(binary)) != 0)
        goto fail;
    close(sp[0]); close(sp[1]); sp[0] = sp[1] = -1;

    stage = "payload split";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return -1;
    flen = hc2_build_frame(frame, sizeof(frame), LMUX_HC2_EXEC_STDOUT, 102,
                           binary, sizeof(binary));
    if (!flen ||
        write_all(sp[0], frame, LMUX_HC2_HEADER_LEN + 2) < 0 ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 10) != HC2_READ_TIMEOUT ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 10) != HC2_READ_TIMEOUT ||
        write_all(sp[0], frame + LMUX_HC2_HEADER_LEN + 2,
                  flen - LMUX_HC2_HEADER_LEN - 2) < 0 ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 1000) != HC2_READ_OK ||
        type != LMUX_HC2_EXEC_STDOUT || req != 102 ||
        len != sizeof(binary) || memcmp(payload, binary, sizeof(binary)) != 0)
        goto fail;
    close(sp[0]); close(sp[1]); sp[0] = sp[1] = -1;

    stage = "repeated timeout";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return -1;
    flen = hc2_build_frame(frame, sizeof(frame), LMUX_HC2_EXEC_STDERR, 103,
                           binary, sizeof(binary));
    if (!flen || write_all(sp[0], frame, 3) < 0 ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 10) != HC2_READ_TIMEOUT ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 10) != HC2_READ_TIMEOUT ||
        write_all(sp[0], frame + 3, flen - 3) < 0 ||
        hc2_read_frame(sp[1], &type, &req, payload, &len, 1000) != HC2_READ_OK ||
        type != LMUX_HC2_EXEC_STDERR || req != 103 ||
        len != sizeof(binary) || memcmp(payload, binary, sizeof(binary)) != 0)
        goto fail;
    close(sp[0]); close(sp[1]); sp[0] = sp[1] = -1;

    stage = "coalesced frames";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return -1;
    flen = hc2_build_frame(frame, sizeof(frame), LMUX_HC2_EXEC_STATE, 104,
                           binary, sizeof(binary));
    {
        uint8_t two[(LMUX_HC2_HEADER_LEN + 64) * 2];
        size_t flen2 = hc2_build_frame(two, sizeof(two), LMUX_HC2_ERROR, 105,
                                       binary, sizeof(binary));
        if (!flen || !flen2 || flen + flen2 > sizeof(two))
            goto fail;
        memcpy(two, frame, flen);
        if (hc2_build_frame(two + flen, sizeof(two) - flen,
                            LMUX_HC2_ERROR, 105, binary,
                            sizeof(binary)) != flen2 ||
            write_all(sp[0], two, flen + flen2) < 0 ||
            hc2_read_frame(sp[1], &type, &req, payload, &len, 1000) != HC2_READ_OK ||
            type != LMUX_HC2_EXEC_STATE || req != 104 ||
            hc2_read_frame(sp[1], &type, &req, payload, &len, 1000) != HC2_READ_OK ||
            type != LMUX_HC2_ERROR || req != 105)
            goto fail;
    }
    close(sp[0]); close(sp[1]); sp[0] = sp[1] = -1;

    stage = "eof mid-frame";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return -1;
    if (write_all(sp[0], frame, 4) < 0)
        goto fail;
    close(sp[0]); sp[0] = -1;
    if (hc2_read_frame(sp[1], &type, &req, payload, &len, 1000) != HC2_READ_EOF)
        goto fail;
    close(sp[1]); sp[1] = -1;

    return 0;
fail:
    fprintf(stderr, "hc2_reader_fragmentation_selftest stage=%s\n", stage);
    if (sp[0] >= 0) close(sp[0]);
    if (sp[1] >= 0) close(sp[1]);
    return -1;
}

static int hc2_connect_hello(const char *path)
{
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req;
    uint32_t len;
    uint32_t caps;
    int fd = wait_connect(path, 3000);
    if (fd < 0)
        return -1;
    if (hc2_send_frame(fd, LMUX_HC2_CLIENT_HELLO, 1, NULL, 0) < 0 ||
        hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0 ||
        type != LMUX_HC2_SERVER_HELLO || req != 1 || len < 14) {
        close(fd);
        return -1;
    }
    caps = lmux_get_le32(payload);
    if ((caps & (LMUX_HC2_CAP_STATUS | LMUX_HC2_CAP_EXEC |
                 LMUX_HC2_CAP_FILE | LMUX_HC2_CAP_FILE_LIFECYCLE)) !=
        (LMUX_HC2_CAP_STATUS | LMUX_HC2_CAP_EXEC |
         LMUX_HC2_CAP_FILE | LMUX_HC2_CAP_FILE_LIFECYCLE)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int hc2_connect_hello_retry(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int fd = hc2_connect_hello(path);
        if (fd >= 0)
            return fd;
        usleep(20000);
    }
    return -1;
}

static int hc2_connect_hello_fragmented(const char *path)
{
    uint8_t frame[LMUX_HC2_HEADER_LEN], payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req;
    uint32_t len;
    int fd = wait_connect(path, 3000);
    size_t flen;
    if (fd < 0)
        return -1;
    flen = hc2_build_frame(frame, sizeof(frame), LMUX_HC2_CLIENT_HELLO, 11,
                           NULL, 0);
    if (flen != sizeof(frame) || write_all(fd, frame, 7) < 0) {
        close(fd);
        return -1;
    }
    usleep(20000);
    if (write_all(fd, frame + 7, flen - 7) < 0 ||
        hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0 ||
        type != LMUX_HC2_SERVER_HELLO || req != 11) {
        close(fd);
        return -1;
    }
    return fd;
}

static int hc2_connect_hello_fragmented_retry(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int fd = hc2_connect_hello_fragmented(path);
        if (fd >= 0)
            return fd;
        usleep(20000);
    }
    return -1;
}

static int hc2_hello_status_batch(const char *path, int timeout_ms)
{
    uint8_t buf[LMUX_HC2_HEADER_LEN * 2], payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req;
    uint32_t len;
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int fd = wait_connect(path, 500);
        size_t a, b;
        if (fd < 0)
            continue;
        a = hc2_build_frame(buf, sizeof(buf), LMUX_HC2_CLIENT_HELLO, 21,
                            NULL, 0);
        b = hc2_build_frame(buf + a, sizeof(buf) - a,
                            LMUX_HC2_STATUS_REQUEST, 22, NULL, 0);
        if (a && b && write_all(fd, buf, a + b) == 0 &&
            hc2_read_frame(fd, &type, &req, payload, &len, 1000) == 0 &&
            type == LMUX_HC2_SERVER_HELLO && req == 21 &&
            hc2_read_frame(fd, &type, &req, payload, &len, 1000) == 0 &&
            type == LMUX_HC2_STATUS_REPLY && req == 22)
            return fd;
        close(fd);
        usleep(20000);
    }
    return -1;
}

static int hc2_bad_magic_smoke(const char *path)
{
    int fd = wait_connect(path, 3000);
    uint8_t bad[LMUX_HC2_HEADER_LEN];
    if (fd < 0)
        return -1;
    memset(bad, 0xa5, sizeof(bad));
    if (write_all(fd, bad, sizeof(bad)) < 0) {
        close(fd);
        return -1;
    }
    (void)wait_fd_event(fd, POLLHUP | POLLERR | POLLNVAL, 500);
    close(fd);
    return 0;
}

static int hc2_malformed_smoke(const char *path)
{
    uint8_t hdr[LMUX_HC2_HEADER_LEN];
    int fd;
    const char *where = "connect";

    fd = wait_connect(path, 3000);
    if (fd < 0)
        return -1;
    where = "bad version";
    memset(hdr, 0, sizeof(hdr));
    lmux_put_le32(hdr, LMUX_HC2_MAGIC);
    lmux_put_le16(hdr + 4, LMUX_HC2_VERSION + 1);
    if (write_all(fd, hdr, sizeof(hdr)) < 0) {
        close(fd);
        return -1;
    }
    (void)wait_fd_event(fd, POLLHUP | POLLERR | POLLNVAL, 500);
    close(fd);

    fd = wait_connect(path, 3000);
    if (fd < 0)
        return -1;
    where = "oversized";
    memset(hdr, 0, sizeof(hdr));
    lmux_put_le32(hdr, LMUX_HC2_MAGIC);
    lmux_put_le16(hdr + 4, LMUX_HC2_VERSION);
    lmux_put_le32(hdr + 20, LMUX_HC2_MAX_PAYLOAD + 1);
    if (write_all(fd, hdr, sizeof(hdr)) < 0) {
        close(fd);
        return -1;
    }
    (void)wait_fd_event(fd, POLLHUP | POLLERR | POLLNVAL, 500);
    close(fd);

    fd = wait_connect(path, 3000);
    if (fd < 0)
        return -1;
    where = "truncated header";
    if (write_all(fd, hdr, 5) < 0) {
        close(fd);
        return -1;
    }
    close(fd);

    fd = wait_connect(path, 3000);
    if (fd < 0)
        return -1;
    where = "truncated payload";
    memset(hdr, 0, sizeof(hdr));
    lmux_put_le32(hdr, LMUX_HC2_MAGIC);
    lmux_put_le16(hdr + 4, LMUX_HC2_VERSION);
    lmux_put_le16(hdr + 6, LMUX_HC2_CLIENT_HELLO);
    hc2_test_put_u64(hdr + 12, 19);
    lmux_put_le32(hdr + 20, 4);
    if (write_all(fd, hdr, sizeof(hdr) + 0) < 0) {
        close(fd);
        return -1;
    }
    close(fd);

    where = "fragmented hello";
    fd = hc2_connect_hello_fragmented_retry(path, 3000);
    if (fd < 0) {
        fprintf(stderr, "hc2_malformed_smoke where=%s\n", where);
        return -1;
    }
    close(fd);

    where = "hello plus status";
    fd = hc2_hello_status_batch(path, 3000);
    if (fd < 0) {
        fprintf(stderr, "hc2_malformed_smoke where=%s\n", where);
        return -1;
    }
    where = "zero request id";
    if (hc2_send_frame(fd, LMUX_HC2_STATUS_REQUEST, 0, NULL, 0) < 0 ||
        hc2_expect_error(fd, 0, LMUX_HC2_E_PROTOCOL) < 0 ||
        hc2_send_frame(fd, 999, 23, NULL, 0) < 0 ||
        hc2_expect_error(fd, 23, LMUX_HC2_E_PROTOCOL) < 0) {
        fprintf(stderr, "hc2_malformed_smoke where=%s\n", where);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int hc2_second_client_rejected(const char *path)
{
    int fd = wait_connect(path, 1000);
    uint8_t p;
    if (fd < 0)
        return -1;
    (void)wait_fd_event(fd, POLLHUP | POLLERR | POLLNVAL, 500);
    if (read(fd, &p, 1) > 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int hc2_put_string(uint8_t *p, size_t cap, size_t *off, const char *s)
{
    size_t n = strlen(s);
    if (n > UINT16_MAX || *off + 2 + n > cap)
        return -1;
    lmux_put_le16(p + *off, (uint16_t)n);
    *off += 2;
    memcpy(p + *off, s, n);
    *off += n;
    return 0;
}

static int hc2_exec_start(int fd, uint64_t req, const char *cwd,
                          const char **argv, uint16_t argc)
{
    uint8_t p[LMUX_HC2_MAX_PAYLOAD];
    size_t off = 0;
    uint16_t i;
    if (hc2_put_string(p, sizeof(p), &off, cwd ? cwd : "") < 0 ||
        off + 2 > sizeof(p))
        return -1;
    lmux_put_le16(p + off, argc);
    off += 2;
    for (i = 0; i < argc; i++)
        if (hc2_put_string(p, sizeof(p), &off, argv[i]) < 0)
            return -1;
    return hc2_send_frame(fd, LMUX_HC2_EXEC_START, req, p, (uint32_t)off);
}

static const char *hc2_type_name(uint16_t type);

static int hc2_exec_start_ready_ref(int fd, uint64_t req, const char *cwd,
                                    const char **argv, uint16_t argc,
                                    uint64_t *ref_out)
{
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t got_req;
    uint32_t len;
    long long end = now_ms() + 7000;
    uint16_t last_type = 0;
    uint64_t last_req = 0;
    uint32_t last_len = 0;
    uint32_t last_err = 0;
    while (now_ms() < end) {
        int rem = (int)(end - now_ms());
        if (hc2_exec_start(fd, req, cwd, argv, argc) < 0) {
            fprintf(stderr, "hc2_exec_start send failed req=%llu\n",
                    (unsigned long long)req);
            return -1;
        }
        for (;;) {
            int rr;
            rem = (int)(end - now_ms());
            if (rem <= 0) {
                fprintf(stderr,
                        "hc2_exec_start timeout req=%llu last=%s(%u) last_req=%llu len=%u err=%u\n",
                        (unsigned long long)req, hc2_type_name(last_type),
                        last_type, (unsigned long long)last_req, last_len,
                        last_err);
                return -1;
            }
            rr = hc2_read_frame(fd, &type, &got_req, payload, &len, rem);
            if (rr < 0) {
                fprintf(stderr,
                        "hc2_exec_start read failed req=%llu last=%s(%u) last_req=%llu len=%u err=%u\n",
                        (unsigned long long)req, hc2_type_name(last_type),
                        last_type, (unsigned long long)last_req, last_len,
                        last_err);
                return -1;
            }
            last_type = type;
            last_req = got_req;
            last_len = len;
            if (type == LMUX_HC2_ERROR && len >= 4)
                last_err = lmux_get_le32(payload);
            if (type == LMUX_HC2_ERROR && got_req == req &&
                len >= 4 &&
                (lmux_get_le32(payload) == LMUX_HC2_E_NOT_CONNECTED ||
                 lmux_get_le32(payload) == LMUX_HC2_E_BUSY)) {
                usleep(20000);
                break;
            }
            if (type == LMUX_HC2_ERROR && got_req == req) {
                fprintf(stderr,
                        "hc2_exec_start error req=%llu err=%u len=%u\n",
                        (unsigned long long)req,
                        len >= 4 ? lmux_get_le32(payload) : 0, len);
                return -1;
            }
            if (type == LMUX_HC2_EXEC_STARTED && got_req == req && len >= 32) {
                if (ref_out)
                    *ref_out = hc2_test_get_u64(payload);
                return 0;
            }
            if (got_req != req ||
                type == LMUX_HC2_EXEC_STDOUT ||
                type == LMUX_HC2_EXEC_STDERR ||
                type == LMUX_HC2_EXEC_STATE ||
                type == LMUX_HC2_EXEC_LOST)
                continue;
            fprintf(stderr,
                    "hc2_exec_start unexpected req=%llu got=%s(%u) got_req=%llu len=%u\n",
                    (unsigned long long)req, hc2_type_name(type), type,
                    (unsigned long long)got_req, len);
            return -1;
        }
    }
    fprintf(stderr,
            "hc2_exec_start deadline req=%llu last=%s(%u) last_req=%llu len=%u err=%u\n",
            (unsigned long long)req, hc2_type_name(last_type), last_type,
            (unsigned long long)last_req, last_len, last_err);
    return -1;
}

static int hc2_exec_start_ready(int fd, uint64_t req, const char *cwd,
                                const char **argv, uint16_t argc)
{
    return hc2_exec_start_ready_ref(fd, req, cwd, argv, argc, NULL);
}

static int hc2_run_echo(int fd, uint64_t req, const char *text)
{
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t got_req;
    uint64_t ref = 0;
    uint32_t len;
    char seen[256];
    char want[256];
    size_t seen_len = 0;
    int done = 0;
    int released = 0;
    long long end;
    const char *argv[] = {"/bin/echo", text};
    if (snprintf(want, sizeof(want), "%s\n", text) <= 0 ||
        strlen(text) + 1 >= sizeof(want))
        return -1;
    memset(seen, 0, sizeof(seen));
    if (hc2_exec_start_ready_ref(fd, req, "", argv, 2, &ref) < 0 || !ref)
        return -1;
    end = now_ms() + 7000;
    while (!done && now_ms() < end) {
        if (hc2_read_frame(fd, &type, &got_req, payload, &len,
                           (int)(end - now_ms())) < 0)
            return -1;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8 &&
            hc2_test_get_u64(payload) == ref) {
            size_t n = len - 8;
            if (seen_len + n >= sizeof(seen))
                return -1;
            memcpy(seen + seen_len, payload + 8, n);
            seen_len += n;
            seen[seen_len] = 0;
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   hc2_test_get_u64(payload) == ref &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if ((type == LMUX_HC2_EXEC_LOST && len >= 8 &&
                    hc2_test_get_u64(payload) == ref) ||
                   (type == LMUX_HC2_ERROR && got_req == req && len >= 4 &&
                    lmux_get_le32(payload) != LMUX_HC2_OK)) {
            return -1;
        }
    }
    if (!done)
        return -1;
    if (strcmp(seen, want) != 0)
        return -1;
    hc2_test_put_u64(payload, ref);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_RELEASE, req + 1000000,
                       payload, 8) < 0)
        return -1;
    end = now_ms() + 3000;
    while (!released && now_ms() < end) {
        if (hc2_read_frame(fd, &type, &got_req, payload, &len,
                           (int)(end - now_ms())) < 0)
            return -1;
        if (type == LMUX_HC2_EXEC_STATE && got_req == req + 1000000)
            released = 1;
        else if (type == LMUX_HC2_ERROR && got_req == req + 1000000 &&
                 len >= 4 && lmux_get_le32(payload) != LMUX_HC2_OK)
            return -1;
    }
    return released ? 0 : -1;
}

static int hc2_file_start(int fd, uint16_t type, uint64_t req,
                          const char *a, const char *b)
{
    uint8_t p[LMUX_HC2_MAX_PAYLOAD];
    size_t off = 0;
    if (hc2_put_string(p, sizeof(p), &off, a) < 0 ||
        hc2_put_string(p, sizeof(p), &off, b) < 0)
        return -1;
    return hc2_send_frame(fd, type, req, p, (uint32_t)off);
}

static int hc2_file_start_idempotent(int fd, uint16_t type, uint64_t req,
                                     const char *request_namespace,
                                     const char *request_token,
                                     const char *a, const char *b)
{
    uint8_t p[LMUX_HC2_MAX_PAYLOAD];
    size_t off = 0;
    if (hc2_put_string(p, sizeof(p), &off, request_namespace) < 0 ||
        hc2_put_string(p, sizeof(p), &off, request_token) < 0 ||
        hc2_put_string(p, sizeof(p), &off, a) < 0 ||
        hc2_put_string(p, sizeof(p), &off, b) < 0)
        return -1;
    return hc2_send_frame(fd, type, req, p, (uint32_t)off);
}

static int hc2_file_status_token(int fd, uint64_t req,
                                 const char *request_namespace,
                                 const char *request_token)
{
    uint8_t p[LMUX_HC2_MAX_PAYLOAD];
    size_t off = 0;
    if (hc2_put_string(p, sizeof(p), &off, request_namespace) < 0 ||
        hc2_put_string(p, sizeof(p), &off, request_token) < 0)
        return -1;
    return hc2_send_frame(fd, LMUX_HC2_FILE_STATUS_TOKEN, req,
                          p, (uint32_t)off);
}

static int hc2_wait_file_state(int fd, uint64_t want_req, int timeout_ms,
                               uint8_t *state, uint64_t *op_id)
{
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req;
    uint32_t len;
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int rem = (int)(end - now_ms());
        if (hc2_read_frame(fd, &type, &req, payload, &len, rem) < 0)
            return -1;
        if (type == LMUX_HC2_FILE_STATE && req == want_req && len >= 96) {
            if (state)
                *state = payload[53];
            if (op_id)
                *op_id = hc2_test_get_u64(payload);
            return 0;
        }
        if (type == LMUX_HC2_ERROR && req == want_req)
            return -1;
    }
    return -1;
}

static int hc2_wait_file_done(int fd, uint64_t want_op, int timeout_ms)
{
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req;
    uint32_t len;
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int rem = (int)(end - now_ms());
        if (hc2_read_frame(fd, &type, &req, payload, &len, rem) < 0)
            return -1;
        if (type == LMUX_HC2_FILE_DONE && len >= 48 &&
            hc2_test_get_u64(payload) == want_op)
            return 0;
        if (type == LMUX_HC2_FILE_ERROR && len >= 12 &&
            hc2_test_get_u64(payload) == want_op)
            return -1;
    }
    return -1;
}

static int wait_marker(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        struct stat st;
        if (stat(path, &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int read_size_marker(const char *path, size_t *out)
{
    FILE *f = fopen(path, "r");
    unsigned long long v;
    if (!f)
        return -1;
    if (fscanf(f, "%llu", &v) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = (size_t)v;
    return 0;
}

static int verify_stdout_hex_pattern(const char *out, size_t expected)
{
    static const char pat[] = "0123456789abcdef";
    const char *p = out;
    size_t pos = 0;
    while ((p = strstr(p, "STDOUT_HEX ")) != NULL) {
        p += strlen("STDOUT_HEX ");
        while (p[0] && p[1] && p[0] != '\n' && p[1] != '\n') {
            unsigned value;
            if (sscanf(p, "%2x", &value) != 1)
                return -1;
            if (pos >= expected || value != (unsigned char)pat[pos & 15])
                return -1;
            pos++;
            p += 2;
        }
    }
    return pos == expected ? 0 : -1;
}

static size_t count_stdout_hex_bytes(const char *out)
{
    const char *p = out;
    size_t pos = 0;
    while ((p = strstr(p, "STDOUT_HEX ")) != NULL) {
        p += strlen("STDOUT_HEX ");
        while (p[0] && p[1] && p[0] != '\n' && p[1] != '\n') {
            pos++;
            p += 2;
        }
    }
    return pos;
}

static int hex_encode_pattern(size_t len, char *out, size_t cap)
{
    static const char hexdigits[] = "0123456789abcdef";
    size_t i;
    if (cap < len * 2 + 1)
        return -1;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)(i & 255);
        out[i * 2] = hexdigits[b >> 4];
        out[i * 2 + 1] = hexdigits[b & 15];
    }
    out[len * 2] = 0;
    return 0;
}

static int read_line_fd(int fd, char *out, size_t cap, int timeout_ms)
{
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    if (cap)
        out[0] = 0;
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            char ch;
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                out[len++] = ch;
                out[len] = 0;
                if (ch == '\n')
                    return 0;
                continue;
            }
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int boot_info_ok(const char *control, char *out, size_t cap)
{
    return control_request_test(control, "BOOT GET_INFO\n", out, cap) == 0 &&
           strstr(out, "OK GET_INFO") &&
           strstr(out, "outer_version=1") &&
           strstr(out, "boot_version=1") &&
           strstr(out, "capabilities=0x0000001f");
}

static int wait_boot_info_ok(const char *control, int timeout_ms,
                             char *out, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        out[0] = 0;
    while (now_ms() < end) {
        if (boot_info_ok(control, out, cap))
            return 0;
        usleep(50000);
    }
    return -1;
}

static int boot_worker_status_ok(const char *control, const char *state,
                                 char *out, size_t cap)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "worker_state=%s", state);
    return control_request_test(control, "BOOT WORKER_STATUS\n", out, cap) == 0 &&
           strstr(out, "OK WORKER_STATUS") &&
           strstr(out, needle) &&
           strstr(out, "worker_incarnation=");
}

static unsigned long long status_value(const char *status, const char *key)
{
    const char *p = strstr(status, key);
    if (!p)
        return 0;
    p += strlen(key);
    return strtoull(p, NULL, 10);
}

static int status_has_ready_layers(const char *status, int outer)
{
    if (!strstr(status, "connection: ESTABLISHED") ||
        !strstr(status, "factories: unix-rendezvous=yes pty=yes file=yes") ||
        !strstr(status, "desired_sent=yes"))
        return 0;
    if (!outer)
        return 1;
    return strstr(status, "outer: ESTABLISHED") &&
           strstr(status, "pre_established_drops=0") &&
           status_value(status, "transport_tx_bytes=") > 0 &&
           status_value(status, "transport_rx_bytes=") > 0;
}

static int wait_ready_layers(const char *control, int outer, int timeout_ms,
                             char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0 &&
            status_has_ready_layers(last, outer))
            return 0;
        usleep(50000);
    }
    return -1;
}

static int wait_exec_service_ready(const char *control, int timeout_ms,
                                   char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0 &&
            strstr(last, "__exec") &&
            strstr(last, "type=exec") &&
            strstr(last, "acknowledged=yes"))
            return 0;
        usleep(50000);
    }
    return -1;
}

static int wait_ready_layers_after_outer_change(const char *control,
                                                unsigned long long old_local,
                                                unsigned long long old_peer,
                                                int timeout_ms,
                                                char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0 &&
            status_has_ready_layers(last, 1) &&
            status_value(last, "local_conn=") != old_local &&
            status_value(last, "peer_conn=") != old_peer)
            return 0;
        usleep(50000);
    }
    return -1;
}

static int wait_outer_only(const char *control, int timeout_ms,
                           char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0 &&
            strstr(last, "outer: ESTABLISHED") &&
            strstr(last, "outer_boot: caps=BOOT_INFO,BOOT_UPLOAD,BOOT_MANAGED,BOOT_SUPERVISOR"))
            return 0;
        usleep(50000);
    }
    return -1;
}

static int wait_pending_state(const char *control, int want_pending,
                              int timeout_ms, char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0) {
            int pending = strstr(last, "console1") &&
                          strstr(last, "pending_fd=yes");
            if (!!pending == !!want_pending)
                return 0;
        }
        usleep(20000);
    }
    return -1;
}

static int wait_fault_injected(int fd, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        char ch;
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            if (read(fd, &ch, 1) == 1 && ch == 'I')
                return 0;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int wait_no_extra_fault_injected(int fd, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        char ch;
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 50);
        if (re & POLLIN) {
            if (read(fd, &ch, 1) == 1 && ch == 'I')
                return -1;
            continue;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return 0;
    }
    return 0;
}

static int wait_ack_drop_report(int fd, int timeout_ms, char *dir,
                                unsigned long long *conn,
                                unsigned long long *ack_peer)
{
    char buf[128];
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    if (dir)
        *dir = 0;
    if (conn)
        *conn = 0;
    if (ack_peer)
        *ack_peer = 0;
    while (now_ms() < end && len + 1 < sizeof(buf)) {
        char ch;
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                if (ch == '\n') {
                    buf[len] = 0;
                    if (sscanf(buf, "%c %llu %llu", dir, conn, ack_peer) == 3)
                        return 0;
                    return -1;
                }
                buf[len++] = ch;
                continue;
            }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int discard_one_cobs_record(int fd, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    unsigned char ch;
    size_t n = 0;
    while (now_ms() < end) {
        int rem = (int)(end - now_ms());
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, rem);
        if (re & POLLIN) {
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                n++;
                if (ch == 0)
                    return n > 1 ? 0 : -1;
                continue;
            }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static long parse_pid_marker(const char *buf, const char *prefix)
{
    const char *p = strstr(buf, prefix);
    if (!p)
        return -1;
    p += strlen(prefix);
    return strtol(p, NULL, 10);
}

static void dump_excerpt(const char *label, const char *buf)
{
    size_t i, n = strlen(buf);
    fprintf(stderr, "%s len=%zu data=", label, n);
    if (n > 200)
        n = 200;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch >= 32 && ch < 127 && ch != '\\')
            fputc(ch, stderr);
        else
            fprintf(stderr, "\\x%02x", ch);
    }
    fputc('\n', stderr);
}

static void dump_text_full(const char *label, const char *buf)
{
    size_t i, n = strlen(buf);
    fprintf(stderr, "%s len=%zu data=", label, n);
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch >= 32 && ch < 127 && ch != '\\')
            fputc(ch, stderr);
        else
            fprintf(stderr, "\\x%02x", ch);
    }
    fputc('\n', stderr);
}

static const char *hc2_type_name(uint16_t type)
{
    switch (type) {
    case LMUX_HC2_CLIENT_HELLO: return "CLIENT_HELLO";
    case LMUX_HC2_SERVER_HELLO: return "SERVER_HELLO";
    case LMUX_HC2_ERROR: return "ERROR";
    case LMUX_HC2_STATUS_REQUEST: return "STATUS_REQUEST";
    case LMUX_HC2_STATUS_REPLY: return "STATUS_REPLY";
    case LMUX_HC2_EXEC_START: return "EXEC_START";
    case LMUX_HC2_EXEC_STARTED: return "EXEC_STARTED";
    case LMUX_HC2_EXEC_ATTACH: return "EXEC_ATTACH";
    case LMUX_HC2_EXEC_STATUS: return "EXEC_STATUS";
    case LMUX_HC2_EXEC_STDIN_DATA: return "EXEC_STDIN_DATA";
    case LMUX_HC2_EXEC_STDIN_CLOSE: return "EXEC_STDIN_CLOSE";
    case LMUX_HC2_EXEC_SIGNAL: return "EXEC_SIGNAL";
    case LMUX_HC2_EXEC_RELEASE: return "EXEC_RELEASE";
    case LMUX_HC2_EXEC_STDOUT: return "EXEC_STDOUT";
    case LMUX_HC2_EXEC_STDERR: return "EXEC_STDERR";
    case LMUX_HC2_EXEC_STATE: return "EXEC_STATE";
    case LMUX_HC2_EXEC_LOST: return "EXEC_LOST";
    case LMUX_HC2_FILE_PUT_START: return "FILE_PUT_START";
    case LMUX_HC2_FILE_GET_START: return "FILE_GET_START";
    case LMUX_HC2_FILE_STARTED: return "FILE_STARTED";
    case LMUX_HC2_FILE_PROGRESS: return "FILE_PROGRESS";
    case LMUX_HC2_FILE_DONE: return "FILE_DONE";
    case LMUX_HC2_FILE_ERROR: return "FILE_ERROR";
    case LMUX_HC2_FILE_STATUS: return "FILE_STATUS";
    case LMUX_HC2_FILE_CANCEL: return "FILE_CANCEL";
    case LMUX_HC2_FILE_STATE: return "FILE_STATE";
    case LMUX_HC2_FILE_PUT_START_IDEMPOTENT: return "FILE_PUT_START_IDEMPOTENT";
    case LMUX_HC2_FILE_GET_START_IDEMPOTENT: return "FILE_GET_START_IDEMPOTENT";
    case LMUX_HC2_FILE_STATUS_TOKEN: return "FILE_STATUS_TOKEN";
    default: return "UNKNOWN";
    }
}

static void note_hc2_frame(uint16_t type, uint64_t req, uint32_t len,
                           uint16_t *last_type, uint64_t *last_req,
                           uint32_t *last_len, unsigned *frame_count)
{
    *last_type = type;
    *last_req = req;
    *last_len = len;
    (*frame_count)++;
}

static void stop_pid(pid_t pid)
{
    long long end;
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    end = now_ms() + 1500;
    while (now_ms() < end) {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
}

static int pid_alive(pid_t pid)
{
    if (pid <= 0)
        return 0;
    return kill(pid, 0) == 0 || errno == EPERM;
}

static void stop_pgrp(pid_t pid)
{
    if (pid <= 0)
        return;
    kill(-pid, SIGTERM);
    stop_pid(pid);
    kill(-pid, SIGKILL);
}

static pid_t spawn_supervisor_with_storage(const char *carrier,
                                           const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX_SUP_IT_OUTER")) {
            if (storage_root) {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-listen", carrier,
                          "--storage-root", storage_root, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-listen", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            } else {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-listen", carrier, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-listen", carrier, (char *)NULL);
            }
        } else {
            if (storage_root) {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--carrier-listen", carrier,
                          "--storage-root", storage_root, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-listen", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            } else {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--carrier-listen", carrier, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-listen", carrier, (char *)NULL);
            }
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_exec_storage(const char *exe, const char *carrier,
                                           const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (storage_root) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl(exe, "lmuxd", "--supervisor", "--outer",
                      "--carrier-listen", carrier, "--storage-root",
                      storage_root, "--verbose", (char *)NULL);
            execl(exe, "lmuxd", "--supervisor", "--outer",
                  "--carrier-listen", carrier, "--storage-root",
                  storage_root, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl(exe, "lmuxd", "--supervisor", "--outer",
                      "--carrier-listen", carrier, "--verbose", (char *)NULL);
            execl(exe, "lmuxd", "--supervisor", "--outer",
                  "--carrier-listen", carrier, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_respawner(const char *exe, const char *carrier,
                                        const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        for (;;) {
            pid_t child = fork();
            int st;
            if (child == 0) {
                if (storage_root)
                    execl(exe, "lmuxd", "--supervisor", "--outer",
                          "--carrier-listen", carrier, "--storage-root",
                          storage_root, (char *)NULL);
                execl(exe, "lmuxd", "--supervisor", "--outer",
                      "--carrier-listen", carrier, (char *)NULL);
                _exit(127);
            }
            if (child < 0)
                _exit(127);
            while (waitpid(child, &st, 0) < 0 && errno == EINTR)
                ;
            usleep(100000);
        }
    }
    return pid;
}

static pid_t spawn_supervisor(const char *carrier)
{
    return spawn_supervisor_with_storage(carrier, NULL);
}

static pid_t spawn_supervisor_bad_worker(const char *carrier)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor",
              "--outer", "--carrier-listen", carrier,
              "--worker-path", "/tmp/lmux-no-such-worker", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int supervisor_storage_start_fails(const char *carrier,
                                          const char *storage_root)
{
    pid_t pid = fork();
    int st;
    if (pid == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor",
              "--outer", "--carrier-listen", carrier,
              "--storage-root", storage_root, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) != 0 ? 0 : -1;
}

static int invalid_storage_root_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-storage-it-XXXXXX";
    char carrier[160], missing[180], file[180];
    int fd, rc = 1;
    if (!mkdtemp(dir))
        return fail("mkdtemp storage invalid");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(missing, sizeof(missing), "%s/missing", dir);
    snprintf(file, sizeof(file), "%s/file", dir);
    fd = open(file, O_CREAT | O_WRONLY, 0600);
    if (fd < 0)
        goto out;
    close(fd);
    if (supervisor_storage_start_fails(carrier, "relative") < 0 ||
        supervisor_storage_start_fails(carrier, missing) < 0 ||
        supervisor_storage_start_fails(carrier, file) < 0)
        goto out;
    rc = 0;
out:
    unlink(file);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static pid_t spawn_host(const char *carrier, const char *control,
                        const char *pty1, const char *file1)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX_SUP_IT_OUTER")) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-connect", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-connect", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--carrier-connect", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host_structured(const char *carrier, const char *control,
                                   const char *structured,
                                   const char *pty1, const char *file1)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX_SUP_IT_OUTER")) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-connect", carrier, "--control", control,
                      "--structured-control", structured,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-connect", carrier, "--control", control,
                  "--structured-control", structured,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--carrier-connect", carrier, "--control", control,
                      "--structured-control", structured,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--structured-control", structured,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host_structured_unix(const char *carrier, const char *control,
                                        const char *structured,
                                        const char *pty1, const char *file1,
                                        const char *unix1)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX_SUP_IT_OUTER")) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-connect", carrier, "--control", control,
                      "--structured-control", structured,
                      "--pty", pty1, "--file", file1, "--unix", unix1,
                      "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-connect", carrier, "--control", control,
                  "--structured-control", structured,
                  "--pty", pty1, "--file", file1, "--unix", unix1, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--carrier-connect", carrier, "--control", control,
                      "--structured-control", structured,
                      "--pty", pty1, "--file", file1, "--unix", unix1,
                      "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--structured-control", structured,
                  "--pty", pty1, "--file", file1, "--unix", unix1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host_listen(const char *carrier, const char *control,
                               const char *pty1, const char *file1, int outer)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (outer) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-listen", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-listen", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--carrier-listen", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-listen", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_fd(int fd, int outer)
{
    pid_t pid = fork();
    char fdarg[32];
    snprintf(fdarg, sizeof(fdarg), "%d", fd);
    if (pid == 0) {
        if (outer) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-fd", fdarg, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--supervisor",
                  "--outer", "--carrier-fd", fdarg, (char *)NULL);
        } else {
            execl("build/host/lmuxd", "lmuxd", "--supervisor",
                  "--carrier-fd", fdarg, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_connect_storage(const char *carrier, int outer,
                                              const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (outer) {
            if (storage_root) {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-connect", carrier,
                          "--storage-root", storage_root, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-connect", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            } else {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-connect", carrier, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-connect", carrier, (char *)NULL);
            }
        } else {
            if (storage_root)
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-connect", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            else
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-connect", carrier, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_connect(const char *carrier, int outer)
{
    return spawn_supervisor_connect_storage(carrier, outer, NULL);
}

static pid_t spawn_host_fd(int fd, const char *control,
                           const char *pty1, const char *file1, int outer)
{
    pid_t pid = fork();
    char fdarg[32];
    snprintf(fdarg, sizeof(fdarg), "%d", fd);
    if (pid == 0) {
        if (outer) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-fd", fdarg, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-fd", fdarg, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-fd", fdarg, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host_fd_structured(int fd, const char *control,
                                      const char *structured,
                                      const char *pty1,
                                      const char *file1, int outer)
{
    pid_t pid = fork();
    char fdarg[32];
    snprintf(fdarg, sizeof(fdarg), "%d", fd);
    if (pid == 0) {
        if (outer) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-fd", fdarg, "--control", control,
                      "--structured-control", structured,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-fd", fdarg, "--control", control,
                  "--structured-control", structured,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-fd", fdarg, "--control", control,
                  "--structured-control", structured,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

struct test_ring {
    unsigned char buf[8192];
    size_t r;
    size_t w;
    size_t len;
};

static void test_ring_clear(struct test_ring *r)
{
    r->r = r->w = r->len = 0;
}

static size_t test_ring_space(const struct test_ring *r)
{
    return sizeof(r->buf) - r->len;
}

static size_t test_ring_write(struct test_ring *r, const unsigned char *buf, size_t len)
{
    size_t done = 0;
    if (len > test_ring_space(r))
        len = test_ring_space(r);
    while (done < len) {
        size_t chunk = sizeof(r->buf) - r->w;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(r->buf + r->w, buf + done, chunk);
        r->w = (r->w + chunk) % sizeof(r->buf);
        r->len += chunk;
        done += chunk;
    }
    return done;
}

static size_t test_ring_peek(const struct test_ring *r, const unsigned char **p)
{
    size_t chunk = sizeof(r->buf) - r->r;
    if (chunk > r->len)
        chunk = r->len;
    *p = r->buf + r->r;
    return chunk;
}

static void test_ring_consume(struct test_ring *r, size_t len)
{
    if (len > r->len)
        len = r->len;
    r->r = (r->r + len) % sizeof(r->buf);
    r->len -= len;
    if (r->len == 0)
        r->r = r->w = 0;
}

static ssize_t test_ring_write_fd(struct test_ring *r, int fd)
{
    const unsigned char *p;
    size_t n = test_ring_peek(r, &p);
    ssize_t w;
    if (!n)
        return 0;
    w = write(fd, p, n);
    if (w > 0)
        test_ring_consume(r, (size_t)w);
    return w;
}

static pid_t spawn_persistent_proxy(const char *path, int stable_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        int lfd, cfd = -1;
        struct sockaddr_un sa;
        struct test_ring to_stable = {{0}, 0, 0, 0};
        struct test_ring to_client = {{0}, 0, 0, 0};
        signal(SIGPIPE, SIG_IGN);
        unlink(path);
        lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (lfd < 0)
            _exit(127);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(lfd, 4) < 0)
            _exit(127);
        (void)test_set_nonblock(stable_fd);
        for (;;) {
            struct pollfd pfds[3];
            nfds_t n = 0, i;
            pfds[n].fd = lfd;
            pfds[n].events = cfd < 0 ? POLLIN : 0;
            pfds[n++].revents = 0;
            pfds[n].fd = stable_fd;
            pfds[n].events = 0;
            if (cfd >= 0 && test_ring_space(&to_client) > 0)
                pfds[n].events |= POLLIN;
            if (to_stable.len > 0)
                pfds[n].events |= POLLOUT;
            pfds[n++].revents = 0;
            if (cfd >= 0) {
                pfds[n].fd = cfd;
                pfds[n].events = 0;
                if (test_ring_space(&to_stable) > 0)
                    pfds[n].events |= POLLIN;
                if (to_client.len > 0)
                    pfds[n].events |= POLLOUT;
                pfds[n++].revents = 0;
            }
            if (poll(pfds, n, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < n; i++) {
                if (!pfds[i].revents)
                    continue;
                if (pfds[i].fd == lfd && (pfds[i].revents & POLLIN)) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd >= 0) {
                        (void)test_set_nonblock(fd);
                        if (cfd >= 0)
                            close(cfd);
                        cfd = fd;
                        test_ring_clear(&to_client);
                    }
                } else if (pfds[i].fd == stable_fd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(stable_fd, tmp, sizeof(tmp));
                        if (r > 0)
                            (void)test_ring_write(&to_client, tmp, (size_t)r);
                        else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                            _exit(0);
                    }
                    if (pfds[i].revents & POLLOUT) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_stable, &p);
                        ssize_t w = write(stable_fd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_stable, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            _exit(0);
                    }
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                        _exit(0);
                } else if (cfd >= 0 && pfds[i].fd == cfd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(cfd, tmp, sizeof(tmp));
                        if (r > 0)
                            (void)test_ring_write(&to_stable, tmp, (size_t)r);
                        else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            close(cfd);
                            cfd = -1;
                            test_ring_clear(&to_client);
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & POLLOUT)) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_client, &p);
                        ssize_t w = write(cfd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_client, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            close(cfd);
                            cfd = -1;
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                        close(cfd);
                        cfd = -1;
                        test_ring_clear(&to_client);
                    }
                }
            }
        }
    }
    return pid;
}

enum outer_fault_kind {
    OUTER_FAULT_BAD_CRC = 1,
    OUTER_FAULT_SEQ_GAP = 2,
    OUTER_FAULT_BOOT_BAD_HASH = 3,
    OUTER_FAULT_BOOT_BAD_OFFSET = 4,
    OUTER_FAULT_BOOT_BAD_CRC = 5,
    OUTER_FAULT_BOOT_CONFIRM_REQ_BAD_CRC = 6,
    OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC = 7
};

enum outer_fault_dir {
    OUTER_FAULT_HOST_TO_TARGET = 1,
    OUTER_FAULT_TARGET_TO_HOST = 2
};

static const char *outer_fault_kind_name(enum outer_fault_kind kind)
{
    switch (kind) {
    case OUTER_FAULT_BAD_CRC:
        return "BAD_CRC";
    case OUTER_FAULT_SEQ_GAP:
        return "SEQ_GAP";
    case OUTER_FAULT_BOOT_BAD_HASH:
        return "BOOT_BAD_HASH";
    case OUTER_FAULT_BOOT_BAD_OFFSET:
        return "BOOT_BAD_OFFSET";
    case OUTER_FAULT_BOOT_BAD_CRC:
        return "BOOT_BAD_CRC";
    case OUTER_FAULT_BOOT_CONFIRM_REQ_BAD_CRC:
        return "BOOT_CONFIRM_REQ_BAD_CRC";
    case OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC:
        return "BOOT_CONFIRM_RESP_BAD_CRC";
    }
    return "UNKNOWN";
}

static const char *outer_fault_dir_name(enum outer_fault_dir dir)
{
    switch (dir) {
    case OUTER_FAULT_HOST_TO_TARGET:
        return "HOST_TO_TARGET";
    case OUTER_FAULT_TARGET_TO_HOST:
        return "TARGET_TO_HOST";
    }
    return "UNKNOWN";
}

struct outer_fault_filter {
    int armed;
    int injected;
    enum outer_fault_kind kind;
    uint8_t rec[LMUX_LINK_MAX_ENCODED];
    size_t rec_len;
};

static int fault_transform_record(struct outer_fault_filter *f,
                                  const uint8_t *rec, size_t rec_len,
                                  uint8_t *out, size_t out_cap,
                                  size_t *out_len)
{
    uint8_t decoded[LMUX_LINK_MAX_DECODED];
    size_t decoded_len, enc_len;
    uint16_t header_len;
    uint32_t payload_len;
    uint8_t frame_type;
    uint8_t *payload;
    uint16_t opcode = 0;
    int matched = 0;

    *out_len = 0;
    if (!f->armed || f->injected ||
        lmux_cobs_decode(rec, rec_len, decoded, sizeof(decoded), &decoded_len) < 0 ||
        decoded_len < LMUX_LINK_HEADER_LEN + 4 ||
        decoded[0] != LMUX_LINK_MAGIC0 || decoded[1] != LMUX_LINK_MAGIC1 ||
        decoded[2] != LMUX_LINK_MAGIC2 || decoded[3] != LMUX_LINK_MAGIC3 ||
        decoded[6] < LMUX_LINK_T_HELLO || decoded[6] > LMUX_LINK_T_BOOT_EVENT) {
        if (rec_len + 1 > out_cap)
            return -1;
        memcpy(out, rec, rec_len);
        out[rec_len] = 0;
        *out_len = rec_len + 1;
        return 0;
    }
    frame_type = decoded[6];
    header_len = lmux_get_le16(decoded + 8);
    payload_len = lmux_get_le32(decoded + 12);
    if (header_len < LMUX_LINK_HEADER_LEN ||
        header_len > LMUX_LINK_MAX_HEADER_LEN ||
        payload_len > LMUX_LINK_MAX_PAYLOAD ||
        decoded_len != (size_t)header_len + payload_len + 4) {
        if (rec_len + 1 > out_cap)
            return -1;
        memcpy(out, rec, rec_len);
        out[rec_len] = 0;
        *out_len = rec_len + 1;
        return 0;
    }
    payload = decoded + header_len;
    if (frame_type == LMUX_LINK_T_BOOT_REQUEST &&
        payload_len >= LMUX_BOOT_REQ_HEADER_LEN)
        opcode = lmux_get_le16(payload);
    else if (frame_type == LMUX_LINK_T_BOOT_RESPONSE &&
             payload_len >= LMUX_BOOT_RESP_HEADER_LEN)
        opcode = lmux_get_le16(payload);

    if ((f->kind == OUTER_FAULT_BAD_CRC &&
         frame_type == LMUX_LINK_T_TRANSPORT) ||
        (f->kind == OUTER_FAULT_BOOT_BAD_CRC &&
         frame_type == LMUX_LINK_T_BOOT_REQUEST &&
         opcode == LMUX_BOOT_UPLOAD_DATA) ||
        (f->kind == OUTER_FAULT_BOOT_CONFIRM_REQ_BAD_CRC &&
         frame_type == LMUX_LINK_T_BOOT_REQUEST &&
         opcode == LMUX_BOOT_CONFIRM_CANDIDATE) ||
        (f->kind == OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC &&
         frame_type == LMUX_LINK_T_BOOT_RESPONSE &&
         opcode == LMUX_BOOT_CONFIRM_CANDIDATE)) {
        decoded[decoded_len - 1] ^= 0x5a;
        matched = 1;
    } else if (f->kind == OUTER_FAULT_SEQ_GAP &&
               frame_type == LMUX_LINK_T_TRANSPORT) {
        f->injected = 1;
        *out_len = 0;
        return 0;
    } else if (f->kind == OUTER_FAULT_BOOT_BAD_HASH &&
               frame_type == LMUX_LINK_T_BOOT_REQUEST &&
               opcode == LMUX_BOOT_UPLOAD_END &&
               payload_len >= LMUX_BOOT_REQ_HEADER_LEN + 40) {
        payload[LMUX_BOOT_REQ_HEADER_LEN + 8] ^= 0x5a;
        lmux_put_le32(decoded + decoded_len - 4,
                      lmux_crc32c(decoded, decoded_len - 4));
        matched = 1;
    } else if (f->kind == OUTER_FAULT_BOOT_BAD_OFFSET &&
               frame_type == LMUX_LINK_T_BOOT_REQUEST &&
               opcode == LMUX_BOOT_UPLOAD_DATA &&
               payload_len >= LMUX_BOOT_REQ_HEADER_LEN + 16) {
        payload[LMUX_BOOT_REQ_HEADER_LEN + 8] ^= 0x01;
        lmux_put_le32(decoded + decoded_len - 4,
                      lmux_crc32c(decoded, decoded_len - 4));
        matched = 1;
    }
    if (!matched) {
        if (rec_len + 1 > out_cap)
            return -1;
        memcpy(out, rec, rec_len);
        out[rec_len] = 0;
        *out_len = rec_len + 1;
        return 0;
    }
    if (lmux_cobs_encode(decoded, decoded_len, out, out_cap - 1, &enc_len) < 0)
        return -1;
    out[enc_len++] = 0;
    *out_len = enc_len;
    f->injected = 1;
    return 0;
}

static int fault_filter_feed(struct outer_fault_filter *f,
                             const uint8_t *in, size_t in_len,
                             struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (!f->armed || f->injected) {
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            continue;
        }
        if (ch == 0) {
            uint8_t out[LMUX_LINK_MAX_ENCODED];
            size_t out_len = 0;
            if (fault_transform_record(f, f->rec, f->rec_len, out, sizeof(out), &out_len) < 0)
                return -1;
            if (test_ring_write(dst, out, out_len) != out_len)
                return -1;
            f->rec_len = 0;
            if (f->injected && notify_fd >= 0)
                (void)write(notify_fd, "I", 1);
            continue;
        }
        if (f->rec_len >= sizeof(f->rec))
            return -1;
        f->rec[f->rec_len++] = ch;
    }
    return 0;
}

static pid_t spawn_fault_proxy(const char *path, int stable_fd,
                               enum outer_fault_kind kind,
                               enum outer_fault_dir dir,
                               int arm_fd, int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        int lfd, cfd = -1;
        struct sockaddr_un sa;
        struct test_ring to_stable = {{0}, 0, 0, 0};
        struct test_ring to_client = {{0}, 0, 0, 0};
        struct outer_fault_filter h2t, t2h;
        signal(SIGPIPE, SIG_IGN);
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        h2t.kind = kind;
        t2h.kind = kind;
        unlink(path);
        lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (lfd < 0)
            _exit(127);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(lfd, 4) < 0)
            _exit(127);
        (void)test_set_nonblock(stable_fd);
        (void)test_set_nonblock(arm_fd);
        for (;;) {
            struct pollfd pfds[4];
            nfds_t n = 0, i;
            pfds[n].fd = arm_fd;
            pfds[n].events = POLLIN;
            pfds[n++].revents = 0;
            pfds[n].fd = lfd;
            pfds[n].events = cfd < 0 ? POLLIN : 0;
            pfds[n++].revents = 0;
            pfds[n].fd = stable_fd;
            pfds[n].events = 0;
            if (cfd >= 0 && test_ring_space(&to_client) > LMUX_LINK_MAX_ENCODED)
                pfds[n].events |= POLLIN;
            if (to_stable.len > 0)
                pfds[n].events |= POLLOUT;
            pfds[n++].revents = 0;
            if (cfd >= 0) {
                pfds[n].fd = cfd;
                pfds[n].events = 0;
                if (test_ring_space(&to_stable) > LMUX_LINK_MAX_ENCODED)
                    pfds[n].events |= POLLIN;
                if (to_client.len > 0)
                    pfds[n].events |= POLLOUT;
                pfds[n++].revents = 0;
            }
            if (poll(pfds, n, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < n; i++) {
                if (!pfds[i].revents)
                    continue;
                if (pfds[i].fd == arm_fd && (pfds[i].revents & POLLIN)) {
                    char tmp[16];
                    while (read(arm_fd, tmp, sizeof(tmp)) > 0) {
                        if (dir == OUTER_FAULT_HOST_TO_TARGET)
                            h2t.armed = 1;
                        else
                            t2h.armed = 1;
                    }
                } else if (pfds[i].fd == lfd && (pfds[i].revents & POLLIN)) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd >= 0) {
                        (void)test_set_nonblock(fd);
                        if (cfd >= 0)
                            close(cfd);
                        cfd = fd;
                        test_ring_clear(&to_client);
                    }
                } else if (pfds[i].fd == stable_fd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(stable_fd, tmp, sizeof(tmp));
                        if (r > 0) {
                            if (fault_filter_feed(&h2t, tmp, (size_t)r, &to_client,
                                                  h2t.injected ? -1 : notify_fd) < 0)
                                _exit(127);
                        } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            _exit(0);
                        }
                    }
                    if (pfds[i].revents & POLLOUT) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_stable, &p);
                        ssize_t w = write(stable_fd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_stable, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            _exit(0);
                    }
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                        _exit(0);
                } else if (cfd >= 0 && pfds[i].fd == cfd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(cfd, tmp, sizeof(tmp));
                        if (r > 0) {
                            if (fault_filter_feed(&t2h, tmp, (size_t)r, &to_stable,
                                                  t2h.injected ? -1 : notify_fd) < 0)
                                _exit(127);
                        } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            close(cfd);
                            cfd = -1;
                            test_ring_clear(&to_client);
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & POLLOUT)) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_client, &p);
                        ssize_t w = write(cfd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_client, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            close(cfd);
                            cfd = -1;
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                        close(cfd);
                        cfd = -1;
                        test_ring_clear(&to_client);
                    }
                }
            }
        }
    }
    return pid;
}

static pid_t spawn_fault_proxy_fds(int host_fd, int target_fd,
                                   enum outer_fault_kind kind,
                                   enum outer_fault_dir dir,
                                   int arm_fd, int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        struct outer_fault_filter h2t, t2h;
        signal(SIGPIPE, SIG_IGN);
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        h2t.kind = kind;
        t2h.kind = kind;
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        (void)test_set_nonblock(arm_fd);
        for (;;) {
            struct pollfd pfds[3];
            nfds_t n = 0, i;
            pfds[n].fd = arm_fd;
            pfds[n].events = POLLIN;
            pfds[n++].revents = 0;
            pfds[n].fd = host_fd;
            pfds[n].events = 0;
            if (test_ring_space(&to_target) > LMUX_LINK_MAX_ENCODED)
                pfds[n].events |= POLLIN;
            if (to_host.len > 0)
                pfds[n].events |= POLLOUT;
            pfds[n++].revents = 0;
            pfds[n].fd = target_fd;
            pfds[n].events = 0;
            if (test_ring_space(&to_host) > LMUX_LINK_MAX_ENCODED)
                pfds[n].events |= POLLIN;
            if (to_target.len > 0)
                pfds[n].events |= POLLOUT;
            pfds[n++].revents = 0;
            if (poll(pfds, n, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < n; i++) {
                if (!pfds[i].revents)
                    continue;
                if (pfds[i].fd == arm_fd && (pfds[i].revents & POLLIN)) {
                    char tmp[16];
                    while (read(arm_fd, tmp, sizeof(tmp)) > 0) {
                        if (dir == OUTER_FAULT_HOST_TO_TARGET)
                            h2t.armed = 1;
                        else
                            t2h.armed = 1;
                    }
                } else if (pfds[i].fd == host_fd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(host_fd, tmp, sizeof(tmp));
                        if (r > 0) {
                            if (fault_filter_feed(&h2t, tmp, (size_t)r, &to_target,
                                                  h2t.injected ? -1 : notify_fd) < 0)
                                _exit(127);
                        } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                              errno != EWOULDBLOCK && errno != EINTR)) {
                            _exit(0);
                        }
                    }
                    if (pfds[i].revents & POLLOUT) {
                        ssize_t w = test_ring_write_fd(&to_host, host_fd);
                        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            _exit(0);
                    }
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                        _exit(0);
                } else if (pfds[i].fd == target_fd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(target_fd, tmp, sizeof(tmp));
                        if (r > 0) {
                            if (fault_filter_feed(&t2h, tmp, (size_t)r, &to_host,
                                                  t2h.injected ? -1 : notify_fd) < 0)
                                _exit(127);
                        } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                              errno != EWOULDBLOCK && errno != EINTR)) {
                            _exit(0);
                        }
                    }
                    if (pfds[i].revents & POLLOUT) {
                        ssize_t w = test_ring_write_fd(&to_target, target_fd);
                        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            _exit(0);
                    }
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                        _exit(0);
                }
            }
        }
    }
    return pid;
}

struct lmc_proxy_record {
    uint8_t buf[8192];
    size_t len;
    int discard;
};

static uint8_t lmc_proxy_record_type(const struct lmc_proxy_record *rec)
{
    uint8_t decoded[128];
    size_t decoded_len = 0;
    if (!rec || rec->len <= 1 ||
        lmux_cobs_decode(rec->buf, rec->len - 1, decoded,
                         sizeof(decoded), &decoded_len) < 0 ||
        decoded_len < 6 || memcmp(decoded, "LMC1", 4) != 0)
        return 0;
    return decoded[5];
}

static int lmc_proxy_feed(int dst, struct lmc_proxy_record *rec,
                          const uint8_t *buf, size_t len,
                          uint8_t drop_type, int *dropped)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (rec->discard) {
            if (buf[i] == 0)
                rec->discard = 0;
            continue;
        }
        if (rec->len == sizeof(rec->buf)) {
            rec->len = 0;
            rec->discard = 1;
            continue;
        }
        rec->buf[rec->len++] = buf[i];
        if (buf[i] == 0) {
            uint8_t decoded[128];
            size_t decoded_len = 0;
            int drop = 0;
            if (!*dropped && rec->len > 1 &&
                lmux_cobs_decode(rec->buf, rec->len - 1, decoded,
                                 sizeof(decoded), &decoded_len) == 0 &&
                decoded_len >= 6 && memcmp(decoded, "LMC1", 4) == 0 &&
                decoded[5] == drop_type) {
                drop = 1;
                *dropped = 1;
            }
            if (!drop && write_all(dst, rec->buf, rec->len) < 0)
                return -1;
            rec->len = 0;
        }
    }
    return 0;
}

static pid_t spawn_lmc_drop_proxy(int host_fd, int target_fd,
                                  int drop_host_to_target,
                                  uint8_t drop_type, int note_fd)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    {
        struct lmc_proxy_record h2t, t2h;
        int dropped = 0;
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        for (;;) {
            struct pollfd p[2];
            int pr;
            p[0].fd = host_fd;
            p[0].events = POLLIN;
            p[0].revents = 0;
            p[1].fd = target_fd;
            p[1].events = POLLIN;
            p[1].revents = 0;
            pr = poll(p, 2, -1);
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr < 0)
                _exit(2);
            if (p[0].revents & POLLIN) {
                uint8_t buf[512];
                ssize_t r = read(host_fd, buf, sizeof(buf));
                int before = dropped;
                if (r <= 0 || lmc_proxy_feed(target_fd, &h2t, buf, (size_t)r,
                        drop_host_to_target ? drop_type : 0, &dropped) < 0)
                    _exit(0);
                if (!before && dropped)
                    (void)write(note_fd, "D", 1);
            }
            if (p[1].revents & POLLIN) {
                uint8_t buf[512];
                ssize_t r = read(target_fd, buf, sizeof(buf));
                int before = dropped;
                if (r <= 0 || lmc_proxy_feed(host_fd, &t2h, buf, (size_t)r,
                        drop_host_to_target ? 0 : drop_type, &dropped) < 0)
                    _exit(0);
                if (!before && dropped)
                    (void)write(note_fd, "D", 1);
            }
            if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL))
                _exit(0);
            if (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))
                _exit(0);
        }
    }
}

/*
 * Exercise the recovery path without timing sleeps: lose the original
 * COMMITTED, two QUERY_REPLY records, and the duplicate-COMMIT replies that
 * occur between them.  Resolving QUERY records are duplicated before OUTER
 * establishment.  The parent can replay the last QUERY after reconciliation
 * through replay_fd; an 'A' notification means its reply crossed the proxy.
 */
static pid_t spawn_lmc_query_recovery_proxy(int host_fd, int target_fd,
                                            int note_fd, int replay_fd)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    {
        struct lmc_proxy_record h2t, t2h, last_query;
        int recovery = 0, committed_drops = 3, query_reply_drops = 2;
        int duplicate_queries = 2, late_pending = 0;
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        memset(&last_query, 0, sizeof(last_query));
        for (;;) {
            struct pollfd p[3];
            int pr, which;
            p[0].fd = host_fd;
            p[0].events = POLLIN;
            p[0].revents = 0;
            p[1].fd = target_fd;
            p[1].events = POLLIN;
            p[1].revents = 0;
            p[2].fd = replay_fd;
            p[2].events = POLLIN;
            p[2].revents = 0;
            pr = poll(p, 3, -1);
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr < 0)
                _exit(2);
            for (which = 0; which < 2; which++) {
                int src = which == 0 ? host_fd : target_fd;
                int dst = which == 0 ? target_fd : host_fd;
                struct lmc_proxy_record *rec = which == 0 ? &h2t : &t2h;
                uint8_t buf[512];
                ssize_t r;
                size_t i;
                if (!(p[which].revents & POLLIN))
                    continue;
                r = read(src, buf, sizeof(buf));
                if (r <= 0)
                    _exit(0);
                for (i = 0; i < (size_t)r; i++) {
                    uint8_t type;
                    int drop = 0, copies = 1;
                    if (rec->len == sizeof(rec->buf))
                        _exit(3);
                    rec->buf[rec->len++] = buf[i];
                    if (buf[i] != 0)
                        continue;
                    type = lmc_proxy_record_type(rec);
                    if (which == 0 && type == LMUX_LMC1_QUERY) {
                        last_query = *rec;
                        if (recovery && duplicate_queries > 0) {
                            duplicate_queries--;
                            copies = 2;
                            (void)write(note_fd, "Q", 1);
                        }
                    } else if (which == 1 && type == LMUX_LMC1_COMMITTED &&
                               committed_drops > 0) {
                        recovery = 1;
                        committed_drops--;
                        drop = 1;
                        (void)write(note_fd, "C", 1);
                    } else if (which == 1 && recovery &&
                               type == LMUX_LMC1_QUERY_REPLY &&
                               query_reply_drops > 0) {
                        query_reply_drops--;
                        drop = 1;
                        (void)write(note_fd, "R", 1);
                    }
                    if (!drop) {
                        while (copies-- > 0)
                            if (write_all(dst, rec->buf, rec->len) < 0)
                                _exit(0);
                        if (which == 1 && late_pending &&
                            type == LMUX_LMC1_QUERY_REPLY) {
                            late_pending = 0;
                            (void)write(note_fd, "A", 1);
                        }
                    }
                    rec->len = 0;
                }
            }
            if (p[2].revents & POLLIN) {
                uint8_t command;
                if (read(replay_fd, &command, 1) != 1)
                    _exit(0);
                if (command == 'L' && last_query.len > 0) {
                    if (write_all(target_fd, last_query.buf,
                                  last_query.len) < 0)
                        _exit(0);
                    late_pending = 1;
                }
            }
            if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL))
                _exit(0);
            if (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))
                _exit(0);
        }
    }
}

static int wait_note_char(int fd, char wanted, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL,
                               100);
        if (re & POLLIN) {
            char ch;
            if (read(fd, &ch, 1) == 1 && ch == wanted)
                return 0;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

struct dirty_fault_record {
    uint8_t buf[LMUX_DIRTY_RECORD_MAX + 64];
    size_t len;
    int collecting;
};

struct dirty_bootstrap_capture {
    int count;
    uint8_t type;
};

static void dirty_bootstrap_capture_cb(void *opaque,
                                       const struct lmux_lmc1_msg *msg)
{
    struct dirty_bootstrap_capture *capture = opaque;
    capture->count++;
    capture->type = msg->type;
}

static uint8_t dirty_bootstrap_record_type(const uint8_t *buf, size_t len)
{
    struct lmux_lmc1_dirty_io io;
    struct dirty_bootstrap_capture capture;
    memset(&capture, 0, sizeof(capture));
    lmux_lmc1_dirty_init(&io);
    if (lmux_lmc1_dirty_feed(&io, buf, len, dirty_bootstrap_capture_cb,
                             &capture) < 0 || capture.count != 1)
        return 0;
    return capture.type;
}

static int dirty_fault_forward_record(int dst, struct dirty_fault_record *rec,
                                      int host_to_target,
                                      int *bootstrap_corrupted,
                                      int *committed_dropped,
                                      int *data_corrupted,
                                      int *target_data_dropped,
                                      int *target_data_corrupted,
                                      int *ack_dropped)
{
    struct lmux_dirty_record decoded;
    int is_bootstrap = rec->len >= LMUX_LMC1_DIRTY_PREFIX_LEN &&
        memcmp(rec->buf, LMUX_LMC1_DIRTY_PREFIX,
               LMUX_LMC1_DIRTY_PREFIX_LEN) == 0;
    int is_dirty = rec->len >= LMUX_DIRTY_PREFIX_LEN &&
        memcmp(rec->buf, LMUX_DIRTY_PREFIX, LMUX_DIRTY_PREFIX_LEN) == 0;
    if (write_all(dst, (const uint8_t *)"console-noise\r\n", 15) < 0)
        return -1;
    if (host_to_target && is_bootstrap && !*bootstrap_corrupted) {
        size_t half = rec->len / 2;
        *bootstrap_corrupted = 1;
        return write_all(dst, rec->buf, half) < 0 ||
            write_all(dst, (const uint8_t *)"!", 1) < 0 ||
            write_all(dst, rec->buf + half, rec->len - half) < 0 ? -1 : 0;
    }
    if (!host_to_target && is_bootstrap && !*committed_dropped &&
        dirty_bootstrap_record_type(rec->buf, rec->len) ==
            LMUX_LMC1_COMMITTED) {
        *committed_dropped = 1;
        return 0;
    }
    if (is_dirty &&
        lmux_dirty_record_decode(rec->buf, rec->len, &decoded) == 0) {
        if (host_to_target && decoded.type == LMUX_DIRTY_DATA &&
            !*data_corrupted) {
            size_t half = rec->len / 2;
            *data_corrupted = 1;
            return write_all(dst, rec->buf, half) < 0 ||
                write_all(dst, (const uint8_t *)"!", 1) < 0 ||
                write_all(dst, rec->buf + half, rec->len - half) < 0 ? -1 : 0;
        }
        if (!host_to_target && decoded.type == LMUX_DIRTY_DATA &&
            !*target_data_dropped) {
            *target_data_dropped = 1;
            return 0;
        }
        if (!host_to_target && decoded.type == LMUX_DIRTY_DATA &&
            !*target_data_corrupted) {
            static const uint8_t console[] =
                "kernel-console-inside-target-record";
            size_t half = rec->len / 2;
            *target_data_corrupted = 1;
            return write_all(dst, rec->buf, half) < 0 ||
                write_all(dst, console, sizeof(console) - 1) < 0 ||
                write_all(dst, rec->buf + half, rec->len - half) < 0 ? -1 : 0;
        }
        if (!host_to_target && decoded.type == LMUX_DIRTY_ACK &&
            !*ack_dropped) {
            *ack_dropped = 1;
            return 0;
        }
    }
    return write_all(dst, rec->buf, rec->len);
}

static int dirty_fault_feed(int dst, struct dirty_fault_record *rec,
                            const uint8_t *buf, size_t len,
                            int host_to_target, int *bootstrap_corrupted,
                            int *committed_dropped,
                            int *data_corrupted, int *target_data_dropped,
                            int *target_data_corrupted, int *ack_dropped)
{
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t ch = buf[i];
        if (!rec->collecting) {
            if (ch != '~') {
                if (write_all(dst, &ch, 1) < 0)
                    return -1;
                continue;
            }
            rec->collecting = 1;
            rec->len = 0;
        }
        if (rec->len == sizeof(rec->buf)) {
            if (write_all(dst, rec->buf, rec->len) < 0)
                return -1;
            rec->collecting = 0;
            rec->len = 0;
            if (ch != '~' && write_all(dst, &ch, 1) < 0)
                return -1;
            continue;
        }
        rec->buf[rec->len++] = ch;
        if (ch == '\n') {
            if (dirty_fault_forward_record(dst, rec, host_to_target,
                                           bootstrap_corrupted,
                                           committed_dropped,
                                           data_corrupted,
                                           target_data_dropped,
                                           target_data_corrupted,
                                           ack_dropped) < 0)
                return -1;
            rec->collecting = 0;
            rec->len = 0;
        }
    }
    return 0;
}

static pid_t spawn_dirty_fault_proxy(int host_fd, int target_fd, int note_fd)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    {
        struct dirty_fault_record h2t, t2h;
        int bootstrap_corrupted = 0, committed_dropped = 0;
        int data_corrupted = 0, target_data_dropped = 0;
        int target_data_corrupted = 0, ack_dropped = 0;
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        signal(SIGPIPE, SIG_IGN);
        for (;;) {
            struct pollfd p[2];
            int pr;
            p[0].fd = host_fd; p[0].events = POLLIN; p[0].revents = 0;
            p[1].fd = target_fd; p[1].events = POLLIN; p[1].revents = 0;
            pr = poll(p, 2, -1);
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr < 0)
                _exit(2);
            if (p[0].revents & POLLIN) {
                uint8_t buf[512];
                ssize_t r = read(host_fd, buf, sizeof(buf));
                if (r <= 0 || dirty_fault_feed(target_fd, &h2t, buf,
                        (size_t)r, 1, &bootstrap_corrupted,
                        &committed_dropped,
                        &data_corrupted, &target_data_dropped,
                        &target_data_corrupted, &ack_dropped) < 0)
                    _exit(0);
            }
            if (p[1].revents & POLLIN) {
                uint8_t buf[512];
                ssize_t r = read(target_fd, buf, sizeof(buf));
                if (r <= 0 || dirty_fault_feed(host_fd, &t2h, buf,
                        (size_t)r, 0, &bootstrap_corrupted,
                        &committed_dropped,
                        &data_corrupted, &target_data_dropped,
                        &target_data_corrupted, &ack_dropped) < 0)
                    _exit(0);
            }
            if (bootstrap_corrupted && committed_dropped &&
                data_corrupted && target_data_dropped &&
                target_data_corrupted && ack_dropped &&
                note_fd >= 0) {
                (void)write(note_fd, "F", 1);
                note_fd = -1;
            }
            if ((p[0].revents | p[1].revents) &
                (POLLHUP | POLLERR | POLLNVAL))
                _exit(0);
        }
    }
}

/*
 * A deterministic, bounded approximation of a full-duplex 115200 8N1 link.
 * Eleven bytes per millisecond is 110 kbit/s on the wire.  The small chunks
 * also exercise partial reads/writes without injecting corruption or loss.
 */
static pid_t spawn_dirty_baud_proxy(int host_fd, int target_fd)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        long long host_due = 0, target_due = 0;
        signal(SIGPIPE, SIG_IGN);
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        for (;;) {
            struct pollfd p[2];
            long long now = now_ms();
            int pr, i;
            p[0].fd = host_fd;
            p[0].events = test_ring_space(&to_target) >= 256 ? POLLIN : 0;
            if (to_host.len && now >= host_due)
                p[0].events |= POLLOUT;
            p[0].revents = 0;
            p[1].fd = target_fd;
            p[1].events = test_ring_space(&to_host) >= 256 ? POLLIN : 0;
            if (to_target.len && now >= target_due)
                p[1].events |= POLLOUT;
            p[1].revents = 0;
            pr = poll(p, 2, (to_host.len || to_target.len) ? 1 : 1000);
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr < 0)
                _exit(127);
            for (i = 0; i < 2; i++) {
                int fd = p[i].fd;
                struct test_ring *in = i == 0 ? &to_target : &to_host;
                struct test_ring *out = i == 0 ? &to_host : &to_target;
                long long *due = i == 0 ? &host_due : &target_due;
                if (p[i].revents & POLLIN) {
                    uint8_t buf[256];
                    ssize_t r = read(fd, buf, sizeof(buf));
                    if (r > 0) {
                        if (test_ring_write(in, buf, (size_t)r) != (size_t)r)
                            _exit(127);
                    } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                          errno != EWOULDBLOCK && errno != EINTR)) {
                        _exit(0);
                    }
                }
                if (p[i].revents & POLLOUT) {
                    const unsigned char *buf;
                    size_t n = test_ring_peek(out, &buf);
                    ssize_t w;
                    if (n > 11)
                        n = 11;
                    w = write(fd, buf, n);
                    if (w > 0) {
                        test_ring_consume(out, (size_t)w);
                        *due = now_ms() + 1;
                    } else if (w < 0 && errno != EAGAIN &&
                               errno != EWOULDBLOCK && errno != EINTR) {
                        _exit(0);
                    }
                }
                if (p[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    _exit(0);
            }
        }
    }
}

static int dirty_blackhole_feed(int dst, struct dirty_fault_record *rec,
                                const uint8_t *buf, size_t len,
                                int *dropped, int note_fd)
{
    size_t i;
    for (i = 0; i < len; i++) {
        uint8_t ch = buf[i];
        if (!rec->collecting) {
            if (ch != '~') {
                if (write_all(dst, &ch, 1) < 0)
                    return -1;
                continue;
            }
            rec->collecting = 1;
            rec->len = 0;
        }
        if (rec->len == sizeof(rec->buf))
            return -1;
        rec->buf[rec->len++] = ch;
        if (ch == '\n') {
            int is_dirty = rec->len >= LMUX_DIRTY_PREFIX_LEN &&
                memcmp(rec->buf, LMUX_DIRTY_PREFIX,
                       LMUX_DIRTY_PREFIX_LEN) == 0;
            if (is_dirty) {
                if (!*dropped && note_fd >= 0)
                    (void)write(note_fd, "X", 1);
                *dropped = 1;
            } else if (write_all(dst, rec->buf, rec->len) < 0) {
                return -1;
            }
            rec->collecting = 0;
            rec->len = 0;
        }
    }
    return 0;
}

static pid_t spawn_dirty_blackhole_proxy(int host_fd, int target_fd,
                                         int note_fd, int disappear)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    {
        struct dirty_fault_record h2t, t2h;
        int dropped = 0;
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        signal(SIGPIPE, SIG_IGN);
        for (;;) {
            struct pollfd p[2];
            int pr;
            p[0].fd = host_fd; p[0].events = POLLIN; p[0].revents = 0;
            p[1].fd = target_fd; p[1].events = POLLIN; p[1].revents = 0;
            pr = poll(p, 2, -1);
            if (pr < 0 && errno == EINTR) continue;
            if (pr < 0) _exit(2);
            if (p[0].revents & POLLIN) {
                uint8_t buf[512];
                int was_dropped = dropped;
                ssize_t r = read(host_fd, buf, sizeof(buf));
                if (r <= 0 || dirty_blackhole_feed(target_fd, &h2t, buf,
                        (size_t)r, &dropped, note_fd) < 0)
                    _exit(0);
                if (disappear && !was_dropped && dropped)
                    _exit(0);
            }
            if (p[1].revents & POLLIN) {
                uint8_t buf[512];
                int was_dropped = dropped;
                ssize_t r = read(target_fd, buf, sizeof(buf));
                if (r <= 0 || dirty_blackhole_feed(host_fd, &t2h, buf,
                        (size_t)r, &dropped, note_fd) < 0)
                    _exit(0);
                if (disappear && !was_dropped && dropped)
                    _exit(0);
            }
            if ((p[0].revents | p[1].revents) &
                (POLLHUP | POLLERR | POLLNVAL))
                _exit(0);
        }
    }
}

static int first_hello_truncate_feed(int *injected, uint8_t *rec,
                                     size_t *rec_len,
                                     const uint8_t *in, size_t in_len,
                                     struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (*injected) {
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            continue;
        }
        if (ch == 0) {
            size_t keep = *rec_len / 2;
            if (keep == 0 && *rec_len > 0)
                keep = 1;
            if (test_ring_write(dst, rec, keep) != keep)
                return -1;
            *rec_len = 0;
            *injected = 1;
            if (notify_fd >= 0)
                (void)write(notify_fd, "I", 1);
            continue;
        }
        if (*rec_len >= LMUX_LINK_MAX_ENCODED)
            return -1;
        rec[(*rec_len)++] = ch;
    }
    return 0;
}

static pid_t spawn_truncate_first_hello_proxy(int host_fd, int target_fd,
                                              int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        uint8_t rec[LMUX_LINK_MAX_ENCODED];
        size_t rec_len = 0;
        int injected = 0;
        signal(SIGPIPE, SIG_IGN);
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        for (;;) {
            struct pollfd pfds[2];
            nfds_t i;
            pfds[0].fd = host_fd;
            pfds[0].events = 0;
            if (test_ring_space(&to_target) > LMUX_LINK_MAX_ENCODED)
                pfds[0].events |= POLLIN;
            if (to_host.len > 0)
                pfds[0].events |= POLLOUT;
            pfds[0].revents = 0;
            pfds[1].fd = target_fd;
            pfds[1].events = 0;
            if (test_ring_space(&to_host) > LMUX_LINK_MAX_ENCODED)
                pfds[1].events |= POLLIN;
            if (to_target.len > 0)
                pfds[1].events |= POLLOUT;
            pfds[1].revents = 0;
            if (poll(pfds, 2, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < 2; i++) {
                int fd = pfds[i].fd;
                struct test_ring *out = i == 0 ? &to_target : &to_host;
                if (pfds[i].revents & POLLIN) {
                    uint8_t tmp[512];
                    ssize_t r = read(fd, tmp, sizeof(tmp));
                    if (r > 0) {
                        if (i == 1) {
                            if (first_hello_truncate_feed(&injected, rec, &rec_len,
                                                          tmp, (size_t)r, out,
                                                          notify_fd) < 0)
                                _exit(127);
                        } else if (test_ring_write(out, tmp, (size_t)r) != (size_t)r) {
                            _exit(127);
                        }
                    } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                          errno != EWOULDBLOCK && errno != EINTR)) {
                        _exit(0);
                    }
                }
                if (pfds[i].revents & POLLOUT) {
                    struct test_ring *src = i == 0 ? &to_host : &to_target;
                    const unsigned char *p;
                    size_t chunk = test_ring_peek(src, &p);
                    ssize_t w = write(fd, p, chunk);
                    if (w > 0)
                        test_ring_consume(src, (size_t)w);
                    else if (w < 0 && errno != EAGAIN &&
                             errno != EWOULDBLOCK && errno != EINTR)
                        _exit(0);
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    _exit(0);
            }
        }
    }
    return pid;
}

static pid_t spawn_dirty_blackhole_reconnect_proxy(const char *path,
                                                    int target_fd,
                                                    int note_fd)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    {
        int lfd, host_fd = -1;
        struct sockaddr_un sa;
        struct dirty_fault_record h2t, t2h;
        int dropped = 0;
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        signal(SIGPIPE, SIG_IGN);
        unlink(path);
        lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (lfd < 0)
            _exit(127);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(lfd, 4) < 0)
            _exit(127);
        for (;;) {
            struct pollfd p[3];
            nfds_t n = 0;
            int pr;
            p[n].fd = lfd; p[n].events = host_fd < 0 ? POLLIN : 0;
            p[n++].revents = 0;
            p[n].fd = target_fd; p[n].events = POLLIN; p[n++].revents = 0;
            if (host_fd >= 0) {
                p[n].fd = host_fd; p[n].events = POLLIN; p[n++].revents = 0;
            }
            pr = poll(p, n, -1);
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr < 0)
                _exit(2);
            if (p[0].revents & POLLIN) {
                int fd = accept(lfd, NULL, NULL);
                if (fd >= 0) {
                    host_fd = fd;
                    memset(&h2t, 0, sizeof(h2t));
                    memset(&t2h, 0, sizeof(t2h));
                }
            }
            if (p[1].revents & POLLIN) {
                uint8_t buf[512];
                ssize_t r = read(target_fd, buf, sizeof(buf));
                if (r <= 0)
                    _exit(0);
                if (host_fd >= 0 && dirty_blackhole_feed(host_fd, &t2h,
                        buf, (size_t)r, &dropped, note_fd) < 0) {
                    close(host_fd);
                    host_fd = -1;
                }
            }
            if (host_fd >= 0 && n == 3 && (p[2].revents & POLLIN)) {
                uint8_t buf[512];
                ssize_t r = read(host_fd, buf, sizeof(buf));
                if (r <= 0 || dirty_blackhole_feed(target_fd, &h2t, buf,
                        (size_t)r, &dropped, note_fd) < 0) {
                    close(host_fd);
                    host_fd = -1;
                }
            }
            if (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))
                _exit(0);
            if (host_fd >= 0 && n == 3 &&
                (p[2].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                close(host_fd);
                host_fd = -1;
            }
        }
    }
}

static int drop_first_ack_feed(int *dropped, char dir,
                               uint8_t *rec, size_t *rec_len,
                               const uint8_t *in, size_t in_len,
                               struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (ch == 0) {
            struct lmux_link_frame fr;
            if (!*dropped && *rec_len > 0 &&
                lmux_link_decode_frame(rec, *rec_len, &fr) == 0) {
                if (fr.type == LMUX_LINK_T_HELLO_ACK) {
                    uint64_t ack_peer = 0;
                    char msg[96];
                    (void)lmux_link_parse_hello_ack_payload(fr.payload,
                                                            fr.payload_len,
                                                            &ack_peer);
                    *dropped = 1;
                    *rec_len = 0;
                    if (notify_fd >= 0) {
                        int n = snprintf(msg, sizeof(msg), "%c %llu %llu\n",
                                         dir,
                                         (unsigned long long)fr.conn_id,
                                         (unsigned long long)ack_peer);
                        if (n > 0)
                            (void)write(notify_fd, msg, (size_t)n);
                    }
                    continue;
                }
            }
            if (*rec_len > 0 && test_ring_write(dst, rec, *rec_len) != *rec_len)
                return -1;
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            *rec_len = 0;
            continue;
        }
        if (*rec_len >= LMUX_LINK_MAX_ENCODED)
            return -1;
        rec[(*rec_len)++] = ch;
    }
    return 0;
}

static pid_t spawn_drop_first_ack_proxy(int host_fd, int target_fd,
                                        int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        uint8_t h2t_rec[LMUX_LINK_MAX_ENCODED];
        uint8_t t2h_rec[LMUX_LINK_MAX_ENCODED];
        size_t h2t_rec_len = 0, t2h_rec_len = 0;
        int dropped = 0;
        signal(SIGPIPE, SIG_IGN);
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        for (;;) {
            struct pollfd pfds[2];
            nfds_t i;
            pfds[0].fd = host_fd;
            pfds[0].events = 0;
            if (test_ring_space(&to_target) > LMUX_LINK_MAX_ENCODED)
                pfds[0].events |= POLLIN;
            if (to_host.len > 0)
                pfds[0].events |= POLLOUT;
            pfds[0].revents = 0;
            pfds[1].fd = target_fd;
            pfds[1].events = 0;
            if (test_ring_space(&to_host) > LMUX_LINK_MAX_ENCODED)
                pfds[1].events |= POLLIN;
            if (to_target.len > 0)
                pfds[1].events |= POLLOUT;
            pfds[1].revents = 0;
            if (poll(pfds, 2, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < 2; i++) {
                int fd = pfds[i].fd;
                struct test_ring *out = i == 0 ? &to_target : &to_host;
                if (pfds[i].revents & POLLIN) {
                    uint8_t tmp[512];
                    ssize_t r = read(fd, tmp, sizeof(tmp));
                    if (r > 0) {
                        if (i == 0) {
                            if (drop_first_ack_feed(&dropped, 'H', h2t_rec,
                                                    &h2t_rec_len,
                                                    tmp, (size_t)r, out,
                                                    notify_fd) < 0)
                                _exit(127);
                        } else {
                            if (drop_first_ack_feed(&dropped, 'T', t2h_rec,
                                                    &t2h_rec_len,
                                                    tmp, (size_t)r, out,
                                                    notify_fd) < 0)
                                _exit(127);
                        }
                    } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                          errno != EWOULDBLOCK && errno != EINTR)) {
                        _exit(0);
                    }
                }
                if (pfds[i].revents & POLLOUT) {
                    struct test_ring *src = i == 0 ? &to_host : &to_target;
                    const unsigned char *p;
                    size_t chunk = test_ring_peek(src, &p);
                    ssize_t w = write(fd, p, chunk);
                    if (w > 0)
                        test_ring_consume(src, (size_t)w);
                    else if (w < 0 && errno != EAGAIN &&
                             errno != EWOULDBLOCK && errno != EINTR)
                        _exit(0);
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    _exit(0);
            }
        }
    }
    return pid;
}

static int duplicate_first_hello_feed(int *duplicated, uint8_t *rec,
                                      size_t *rec_len,
                                      const uint8_t *in, size_t in_len,
                                      struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (*duplicated) {
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            continue;
        }
        if (ch == 0) {
            struct lmux_link_frame fr;
            if (*rec_len > 0 &&
                lmux_link_decode_frame(rec, *rec_len, &fr) == 0 &&
                fr.type == LMUX_LINK_T_HELLO) {
                int n;
                for (n = 0; n < 8; n++) {
                    if (test_ring_write(dst, rec, *rec_len) != *rec_len ||
                        test_ring_write(dst, &ch, 1) != 1)
                        return -1;
                }
                *duplicated = 1;
                *rec_len = 0;
                if (notify_fd >= 0)
                    (void)write(notify_fd, "I", 1);
                continue;
            }
            if (*rec_len > 0 && test_ring_write(dst, rec, *rec_len) != *rec_len)
                return -1;
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            *rec_len = 0;
            continue;
        }
        if (*rec_len >= LMUX_LINK_MAX_ENCODED)
            return -1;
        rec[(*rec_len)++] = ch;
    }
    return 0;
}

static pid_t spawn_duplicate_first_target_hello_proxy(int host_fd, int target_fd,
                                                      int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        uint8_t rec[LMUX_LINK_MAX_ENCODED];
        size_t rec_len = 0;
        int duplicated = 0;
        signal(SIGPIPE, SIG_IGN);
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        for (;;) {
            struct pollfd pfds[2];
            nfds_t i;
            pfds[0].fd = host_fd;
            pfds[0].events = 0;
            if (test_ring_space(&to_target) > LMUX_LINK_MAX_ENCODED)
                pfds[0].events |= POLLIN;
            if (to_host.len > 0)
                pfds[0].events |= POLLOUT;
            pfds[0].revents = 0;
            pfds[1].fd = target_fd;
            pfds[1].events = 0;
            if (test_ring_space(&to_host) > 1024)
                pfds[1].events |= POLLIN;
            if (to_target.len > 0)
                pfds[1].events |= POLLOUT;
            pfds[1].revents = 0;
            if (poll(pfds, 2, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < 2; i++) {
                int fd = pfds[i].fd;
                struct test_ring *out = i == 0 ? &to_target : &to_host;
                if (pfds[i].revents & POLLIN) {
                    uint8_t tmp[512];
                    ssize_t r = read(fd, tmp, sizeof(tmp));
                    if (r > 0) {
                        if (i == 1) {
                            if (duplicate_first_hello_feed(&duplicated, rec,
                                                           &rec_len, tmp,
                                                           (size_t)r, out,
                                                           notify_fd) < 0)
                                _exit(127);
                        } else if (test_ring_write(out, tmp, (size_t)r) != (size_t)r) {
                            _exit(127);
                        }
                    } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                          errno != EWOULDBLOCK && errno != EINTR)) {
                        _exit(0);
                    }
                }
                if (pfds[i].revents & POLLOUT) {
                    struct test_ring *src = i == 0 ? &to_host : &to_target;
                    const unsigned char *p;
                    size_t chunk = test_ring_peek(src, &p);
                    ssize_t w = write(fd, p, chunk);
                    if (w > 0)
                        test_ring_consume(src, (size_t)w);
                    else if (w < 0 && errno != EAGAIN &&
                             errno != EWOULDBLOCK && errno != EINTR)
                        _exit(0);
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    _exit(0);
            }
        }
    }
    return pid;
}

static int count_fds(pid_t pid)
{
    char path[64];
    DIR *d;
    struct dirent *de;
    int n = 0;
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    d = opendir(path);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            n++;
    }
    closedir(d);
    return n;
}

static void unlink_dir_files(const char *path)
{
    DIR *d;
    struct dirent *de;
    d = opendir(path);
    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        char child[512];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) <= 0 ||
            strlen(child) >= sizeof(child))
            continue;
        unlink(child);
    }
    closedir(d);
}

static int count_lmux_temp_files(const char *path)
{
    DIR *d;
    struct dirent *de;
    int n = 0;
    d = opendir(path);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL)
        if (strstr(de->d_name, ".lmux-tmp-"))
            n++;
    closedir(d);
    return n;
}

static int wait_lmux_temp_at_least(const char *path, int want, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int n = count_lmux_temp_files(path);
        if (n >= want)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int wait_lmux_temp_count(const char *path, int want, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int n = count_lmux_temp_files(path);
        if (n == want)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int wait_fd_count(pid_t pid, int want, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    int n = -1;
    while (now_ms() < end) {
        n = count_fds(pid);
        if (n == want)
            return n;
        usleep(20000);
    }
    return n;
}

static pid_t find_child_pid(pid_t ppid)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        char path[128], buf[512];
        FILE *f;
        long pid, parent = -1;
        char *p;
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        pid = strtol(de->d_name, NULL, 10);
        snprintf(path, sizeof(path), "/proc/%ld/status", pid);
        f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "PPid:", 5) == 0) {
                p = buf + 5;
                parent = strtol(p, NULL, 10);
                break;
            }
        }
        fclose(f);
        if (parent == (long)ppid) {
            closedir(d);
            return (pid_t)pid;
        }
    }
    closedir(d);
    return -1;
}

static pid_t wait_child_pid(pid_t ppid, pid_t not_pid, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        pid_t p = find_child_pid(ppid);
        if (p > 0 && p != not_pid)
            return p;
        usleep(20000);
    }
    return -1;
}

static int carrier_manager_standby_without_usb_test(void)
{
    char dir[] = "/tmp/lmux-carrier-manager-it-XXXXXX";
    char usb[192], uart[192], control[192], console[192], file_sock[192];
    char uart_spec[256], pty_arg[400], file_arg[400], out[8192];
    pid_t sup = -1, host = -1, worker = -1;
    int uart_fd = -1, rc = 1;
    const char *why = "setup";
    static const uint8_t noise[] = "boot console noise before LMC1\r\nlogin: ";

    out[0] = '\0';
    if (!mkdtemp(dir))
        return fail("carrier manager mkdtemp");
    snprintf(usb, sizeof(usb), "%s/usb.sock", dir);
    snprintf(uart, sizeof(uart), "%s/uart.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(uart_spec, sizeof(uart_spec), "rescue=%s", uart);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = fork();
    if (sup == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
                  "--carrier-connect", usb, "--carrier-name", "main",
                  "--standby-carrier-listen", uart_spec,
                  "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-connect", usb, "--carrier-name", "main",
              "--standby-carrier-listen", uart_spec, (char *)NULL);
        _exit(127);
    }
    if (sup < 0 || wait_path_exists(uart, 5000) < 0) {
        why = "uart listener did not start";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker did not start";
        goto out;
    }

    uart_fd = wait_connect(uart, 5000);
    if (uart_fd < 0 || write_all(uart_fd, noise, sizeof(noise) - 1) < 0) {
        why = "uart standby connect/write failed";
        goto out;
    }
    {
        long long end = now_ms() + 1000;
        while (now_ms() < end) {
            if (!pid_alive(sup) || !pid_alive(worker)) {
                why = "supervisor or worker died under standby noise";
                goto out;
            }
            if (poll(NULL, 0, 20) < 0 && errno != EINTR)
                goto out;
        }
    }
    close(uart_fd);
    uart_fd = -1;

    host = fork();
    if (host == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-listen", usb,
                  "--carrier-name", "main", "--standby-carrier-connect",
                  uart_spec, "--control", control, "--pty", pty_arg,
                  "--file", file_arg, "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--role", "host",
              "--outer", "--carrier-listen", usb,
              "--carrier-name", "main", "--standby-carrier-connect",
              uart_spec, "--control", control, "--pty", pty_arg,
              "--file", file_arg, (char *)NULL);
        _exit(127);
    }
    if (host < 0 || wait_ready_layers(control, 1, 15000,
                                      out, sizeof(out)) < 0) {
        why = "usb outer/lmux did not establish";
        goto out;
    }
    if (!pid_alive(sup) || !pid_alive(worker) ||
        find_child_pid(sup) != worker) {
        why = "worker changed during usb establishment";
        goto out;
    }
    if (!strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "carrier_manager: active=main") ||
        !strstr(out, "carrier_endpoint: name=main") ||
        !strstr(out, "carrier_endpoint: name=rescue") ||
        strstr(out, "carrier_endpoint: name=usb") ||
        strstr(out, "carrier_endpoint: name=uart")) {
        why = "status lacks named main/rescue endpoints";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier manager standby failure: %s\n", why);
        dump_excerpt("carrier manager standby without usb", out);
    }
    if (uart_fd >= 0)
        close(uart_fd);
    stop_pid(host);
    stop_pid(sup);
    unlink(usb);
    unlink(uart);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int carrier_manager_usb_without_uart_test(void)
{
    char dir[] = "/tmp/lmux-carrier-missing-uart-it-XXXXXX";
    char usb[192], uart[192], control[192], console[192], file_sock[192];
    char uart_spec[256], pty_arg[400], file_arg[400], out[8192];
    pid_t sup = -1, host = -1, worker = -1;
    int rc = 1;
    const char *why = "setup";

    out[0] = '\0';
    if (!mkdtemp(dir))
        return fail("carrier manager missing uart mkdtemp");
    snprintf(usb, sizeof(usb), "%s/usb.sock", dir);
    snprintf(uart, sizeof(uart), "%s/missing-uart", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(uart_spec, sizeof(uart_spec), "rescue=%s", uart);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = fork();
    if (sup == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
                  "--carrier-listen", usb, "--standby-carrier", uart_spec,
                  "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-listen", usb, "--standby-carrier", uart_spec,
              (char *)NULL);
        _exit(127);
    }
    if (sup < 0 || wait_path_exists(usb, 5000) < 0) {
        why = "usb listener did not start";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker did not start";
        goto out;
    }

    host = fork();
    if (host == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-connect", usb, "--control", control,
                  "--pty", pty_arg, "--file", file_arg, "--verbose",
                  (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--role", "host",
              "--outer", "--carrier-connect", usb, "--control", control,
              "--pty", pty_arg, "--file", file_arg, (char *)NULL);
        _exit(127);
    }
    if (host < 0 || wait_ready_layers(control, 1, 15000,
                                      out, sizeof(out)) < 0) {
        why = "usb outer/lmux did not establish";
        goto out;
    }
    if (!pid_alive(sup) || !pid_alive(worker) ||
        find_child_pid(sup) != worker) {
        why = "supervisor or worker changed";
        goto out;
    }
    if (!strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "carrier_manager: active=primary") ||
        !strstr(out, "carrier_endpoint: name=primary")) {
        why = "legacy status lacks active primary";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier manager missing uart failure: %s\n", why);
        dump_excerpt("carrier manager usb without uart", out);
    }
    stop_pid(host);
    stop_pid(sup);
    unlink(usb);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int run_lmux_carrier_cli_mode(const char *action, const char *name,
                                     const char *mode, const char *control,
                                     char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (!action || !control || !out || cap == 0 || pipe(p) < 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        if (name && strcmp(action, "switch") == 0)
            execl("build/host/lmux", "lmux", "carrier", action, name,
                  "--mode", mode, "--timeout-ms", "15000",
                  "--control", control, (char *)NULL);
        else if (name)
            execl("build/host/lmux", "lmux", "carrier", action, name,
                  "--control", control, (char *)NULL);
        else
            execl("build/host/lmux", "lmux", "carrier", action,
                  "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t n = read(p[0], out + len, cap - len - 1);
        if (n > 0) {
            len += (size_t)n;
            out[len] = 0;
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            close(p[0]);
            return -1;
        }
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_carrier_cli(const char *action, const char *name,
                                const char *control, char *out, size_t cap)
{
    return run_lmux_carrier_cli_mode(action, name, "raw", control, out, cap);
}

static int carrier_dispatcher_handover_console_test(void)
{
    char dir[] = "/tmp/lmux-dispatch-supervisor-it-XXXXXX";
    char primary[192], control[192], dispatch_sock[192];
    char rescue_fd_spec[64], rescue_dispatch_spec[256], out[8192];
    char *slave_name;
    struct termios tio;
    pid_t dispatcher = -1, host = -1, sup = -1, worker = -1;
    int master = -1, slave = -1, flags, status, rc = 1;
    const char *why = "setup";

    if (!mkdtemp(dir))
        return fail("dispatcher handover mkdtemp");
    snprintf(primary, sizeof(primary), "%s/primary.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(dispatch_sock, sizeof(dispatch_sock), "%s/dispatch.sock", dir);
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
        !(slave_name = ptsname(master))) {
        why = "PTY allocation";
        goto out;
    }
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0 || tcgetattr(slave, &tio) < 0) {
        why = "PTY slave";
        goto out;
    }
    tio.c_oflag |= OPOST | ONLCR;
    if (tcsetattr(slave, TCSANOW, &tio) < 0) {
        why = "PTY termios";
        goto out;
    }
    close(slave);
    slave = -1;
    dispatcher = fork();
    if (dispatcher == 0) {
        execl("build/host/lmux-uart-dispatch", "lmux-uart-dispatch",
              "--tty", slave_name, "--ipc-listen", dispatch_sock,
              "--console-exec", "/bin/true", (char *)NULL);
        _exit(127);
    }
    if (dispatcher < 0 || wait_path_exists(dispatch_sock, 5000) < 0) {
        why = "dispatcher startup";
        goto out;
    }
    flags = fcntl(master, F_GETFD, 0);
    if (flags < 0 || fcntl(master, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
        why = "master inheritance";
        goto out;
    }
    snprintf(rescue_fd_spec, sizeof(rescue_fd_spec), "rescue=%d", master);
    snprintf(rescue_dispatch_spec, sizeof(rescue_dispatch_spec),
             "rescue=%s", dispatch_sock);
    host = fork();
    if (host == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
                  "--carrier-listen", primary, "--carrier-name", "primary",
                  "--standby-carrier-fd", rescue_fd_spec,
                  "--control", control, "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-listen", primary, "--carrier-name", "primary",
              "--standby-carrier-fd", rescue_fd_spec,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    if (host < 0 || wait_path_exists(primary, 5000) < 0) {
        why = "host startup";
        goto out;
    }
    close(master);
    master = -1;
    sup = fork();
    if (sup == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
                  "--carrier-connect", primary, "--carrier-name", "primary",
                  "--standby-dispatch", rescue_dispatch_spec,
                  "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-connect", primary, "--carrier-name", "primary",
              "--standby-dispatch", rescue_dispatch_spec, (char *)NULL);
        _exit(127);
    }
    if (sup < 0 || wait_ready_layers(control, 1, 15000,
                                     out, sizeof(out)) < 0) {
        why = "initial establishment";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker PID";
        goto out;
    }
    if (run_lmux_carrier_cli_mode("switch", "rescue", "dirty", control,
                                  out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=rescue mode=dirty") ||
        read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "connection: ESTABLISHED") ||
        find_child_pid(sup) != worker) {
        why = "DIRTY dispatcher handover";
        goto out;
    }
    if (run_lmux_carrier_cli("console", "rescue", control,
                             out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER CONSOLE name=rescue")) {
        why = "emergency console CLI";
        goto out;
    }
    if (waitpid(dispatcher, &status, 0) != dispatcher ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        why = "console exec outcome";
        goto out;
    }
    dispatcher = -1;
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier: down") ||
        !strstr(out,
                "carrier_console: state=released endpoint=rescue held_mask=0x2")) {
        why = "host UART release state";
        goto out;
    }
    if (run_lmux_carrier_cli_mode("switch", "primary", "raw", control,
                                  out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=primary mode=raw") ||
        read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out,
                "carrier_console: state=released endpoint=rescue held_mask=0x2")) {
        why = "USB recovery with UART released";
        goto out;
    }
    if (find_child_pid(sup) != worker) {
        why = "worker replaced";
        goto out;
    }
    rc = 0;
out:
    if (rc) {
        fprintf(stderr, "dispatcher handover failure: %s\n", why);
        dump_excerpt("dispatcher handover", out);
    }
    if (master >= 0) close(master);
    if (slave >= 0) close(slave);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(dispatcher);
    unlink(primary);
    unlink(control);
    unlink(dispatch_sock);
    rmdir(dir);
    return rc;
}

static int lmc_test_send(int fd, const struct lmux_lmc1_msg *msg,
                         size_t split_at)
{
    uint8_t record[LMUX_LMC1_MAX_ENCODED];
    size_t len = 0;
    if (lmux_lmc1_encode(msg, record, sizeof(record), &len) < 0)
        return -1;
    if (split_at == 0 || split_at >= len)
        return write_all(fd, record, len);
    return write_all(fd, record, split_at) < 0 ||
           write_all(fd, record + split_at, len - split_at) < 0 ? -1 : 0;
}

static int lmc_test_read(int fd, struct lmux_lmc1_msg *msg, int timeout_ms)
{
    uint8_t record[LMUX_LMC1_RAW_RECORD_MAX];
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        uint8_t buf[256];
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL,
                               100);
        if (re & POLLIN) {
            ssize_t n = read(fd, buf, sizeof(buf));
            size_t i;
            if (n <= 0)
                return -1;
            for (i = 0; i < (size_t)n; i++) {
                if (buf[i] == 0) {
                    int decoded = len ? lmux_lmc1_decode(record, len, msg) : 1;
                    len = 0;
                    if (decoded == 0)
                        return 0;
                } else if (len < sizeof(record)) {
                    record[len++] = buf[i];
                } else {
                    len = 0;
                }
            }
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int lmc_test_request(int fd, const struct lmux_lmc1_msg *request,
                            struct lmux_lmc1_msg *reply)
{
    return lmc_test_send(fd, request, 0) < 0 ||
        lmc_test_read(fd, reply, 5000) < 0 ||
        !lmux_lmc1_nonce_equal(request->nonce, reply->nonce) ? -1 : 0;
}

static void lmc_test_nonce(uint8_t nonce[LMUX_LMC1_NONCE_LEN], uint8_t value)
{
    memset(nonce, value, LMUX_LMC1_NONCE_LEN);
}

static int carrier_lmc1_state_machine_test(void)
{
    char dir[] = "/tmp/lmux-lmc1-state-it-XXXXXX";
    char primary[192], rescue[192], spec[256];
    struct lmux_lmc1_msg query, prepare, ready, request, reply, commit;
    pid_t sup = -1, worker = -1;
    int fd = -1, rc = 1;
    const char *why = "setup";

    if (!mkdtemp(dir))
        return fail("LMC1 state mkdtemp");
    snprintf(primary, sizeof(primary), "%s/primary-missing", dir);
    snprintf(rescue, sizeof(rescue), "%s/rescue.sock", dir);
    snprintf(spec, sizeof(spec), "rescue=%s", rescue);
    sup = fork();
    if (sup == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-connect", primary,
              "--standby-carrier-listen", spec, (char *)NULL);
        _exit(127);
    }
    if (sup < 0 || wait_path_exists(rescue, 5000) < 0 ||
        (worker = wait_child_pid(sup, -1, 5000)) <= 0 ||
        (fd = wait_connect(rescue, 5000)) < 0) {
        why = "target standby";
        goto out;
    }
    memset(&query, 0, sizeof(query));
    query.type = LMUX_LMC1_QUERY;
    query.mode = LMUX_CARRIER_MODE_RAW;
    strcpy(query.name, "rescue");
    lmc_test_nonce(query.nonce, 1);
    if (lmc_test_request(fd, &query, &reply) < 0 ||
        reply.type != LMUX_LMC1_QUERY_REPLY || reply.generation != 1 ||
        strcmp(reply.name, "primary") != 0) {
        why = "initial QUERY";
        goto out;
    }
    memset(&prepare, 0, sizeof(prepare));
    prepare.type = LMUX_LMC1_PREPARE;
    prepare.mode = LMUX_CARRIER_MODE_RAW;
    prepare.generation = reply.generation;
    memcpy(prepare.boot_id, reply.boot_id, sizeof(prepare.boot_id));
    strcpy(prepare.name, "rescue");
    lmc_test_nonce(prepare.nonce, 2);
    {
        uint8_t encoded[LMUX_LMC1_MAX_ENCODED];
        size_t encoded_len = 0, half;
        if (lmux_lmc1_encode(&prepare, encoded, sizeof(encoded),
                             &encoded_len) < 0) {
            why = "split PREPARE encode";
            goto out;
        }
        half = encoded_len / 2;
        if (write_all(fd, encoded, half) < 0 ||
            (wait_fd_event(fd, POLLIN | POLLHUP | POLLERR, 100) & POLLIN) ||
            write_all(fd, encoded + half, encoded_len - half) < 0 ||
            lmc_test_read(fd, &ready, 5000) < 0 ||
            ready.type != LMUX_LMC1_READY ||
            !lmux_lmc1_nonce_equal(ready.nonce, prepare.nonce)) {
            why = "split PREPARE/READY";
            goto out;
        }
    }
    if (lmc_test_request(fd, &prepare, &reply) < 0 ||
        reply.type != LMUX_LMC1_READY ||
        memcmp(reply.cookie, ready.cookie, sizeof(reply.cookie)) != 0) {
        why = "duplicate PREPARE";
        goto out;
    }
    request = prepare;
    lmc_test_nonce(request.nonce, 3);
    if (lmc_test_request(fd, &request, &reply) < 0 ||
        reply.type != LMUX_LMC1_ERROR ||
        reply.status != LMUX_LMC1_STATUS_BUSY) {
        why = "competing PREPARE BUSY";
        goto out;
    }
    request = prepare;
    lmc_test_nonce(request.nonce, 4);
    request.generation++;
    if (lmc_test_request(fd, &request, &reply) < 0 ||
        reply.type != LMUX_LMC1_ERROR ||
        reply.status != LMUX_LMC1_STATUS_STALE) {
        why = "stale generation";
        goto out;
    }
    request = prepare;
    lmc_test_nonce(request.nonce, 5);
    request.boot_id[0] ^= 1;
    if (lmc_test_request(fd, &request, &reply) < 0 ||
        reply.type != LMUX_LMC1_ERROR ||
        reply.status != LMUX_LMC1_STATUS_STALE) {
        why = "stale boot ID";
        goto out;
    }
    request = prepare;
    lmc_test_nonce(request.nonce, 6);
    strcpy(request.name, "unknown");
    if (lmc_test_request(fd, &request, &reply) < 0 ||
        reply.type != LMUX_LMC1_ERROR ||
        reply.status != LMUX_LMC1_STATUS_UNKNOWN_CARRIER) {
        why = "unknown carrier";
        goto out;
    }
    commit = prepare;
    commit.type = LMUX_LMC1_COMMIT;
    memcpy(commit.cookie, ready.cookie, sizeof(commit.cookie));
    request = commit;
    request.cookie[0] ^= 1;
    if (lmc_test_request(fd, &request, &reply) < 0 ||
        reply.type != LMUX_LMC1_ERROR ||
        reply.status != LMUX_LMC1_STATUS_BAD_COOKIE) {
        why = "bad COMMIT cookie";
        goto out;
    }
    if (lmc_test_request(fd, &commit, &reply) < 0 ||
        reply.type != LMUX_LMC1_COMMITTED || reply.generation != 2 ||
        strcmp(reply.name, "rescue") != 0) {
        why = "COMMIT";
        goto out;
    }
    if (lmc_test_request(fd, &commit, &reply) < 0 ||
        reply.type != LMUX_LMC1_COMMITTED || reply.generation != 2) {
        why = "duplicate COMMIT";
        goto out;
    }
    query.generation = 0;
    lmc_test_nonce(query.nonce, 7);
    if (lmc_test_request(fd, &query, &reply) < 0 ||
        reply.type != LMUX_LMC1_QUERY_REPLY || reply.generation != 2 ||
        strcmp(reply.name, "rescue") != 0 ||
        find_child_pid(sup) != worker) {
        why = "post-COMMIT QUERY/worker";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "LMC1 state machine failure: %s\n", why);
    if (fd >= 0) close(fd);
    stop_pid(sup);
    unlink(rescue);
    rmdir(dir);
    return rc;
}

static int carrier_raw_switch_roundtrip_test(void)
{
    char dir[] = "/tmp/lmux-carrier-switch-it-XXXXXX";
    char primary[192], rescue[192], control[192], structured[192];
    char console[192], file_sock[192], file_src[192], file_target[192];
    char rescue_spec[256], pty_arg[400], file_arg[400], out[16384];
    char pty_out[512], pid_before[64], pid_after[64];
    pid_t host = -1, sup = -1, worker = -1;
    uint8_t hc2_payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t hc2_type = 0;
    uint64_t hc2_req = 0, file_op = 0;
    uint32_t hc2_len = 0;
    int cfd = -1, sfd = -1, worker_stopped = 0, rc = 1;
    const char *why = "setup";
    unsigned long long old_local, old_peer;

    if (!mkdtemp(dir))
        return fail("carrier switch mkdtemp");
    snprintf(primary, sizeof(primary), "%s/primary.sock", dir);
    snprintf(rescue, sizeof(rescue), "%s/rescue.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/structured.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(file_src, sizeof(file_src), "%s/file-busy-src", dir);
    snprintf(file_target, sizeof(file_target), "%s/file-busy-target", dir);
    snprintf(rescue_spec, sizeof(rescue_spec), "rescue=%s", rescue);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    host = fork();
    if (host == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
                  "--carrier-listen", primary, "--carrier-name", "primary",
                  "--standby-carrier-connect", rescue_spec,
                  "--control", control, "--structured-control", structured,
                  "--pty", pty_arg, "--file", file_arg,
                  "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-listen", primary, "--carrier-name", "primary",
              "--standby-carrier-connect", rescue_spec,
              "--control", control, "--structured-control", structured,
              "--pty", pty_arg, "--file", file_arg,
              (char *)NULL);
        _exit(127);
    }
    if (host < 0 || wait_path_exists(primary, 5000) < 0) {
        why = "host primary listener";
        goto out;
    }
    sup = fork();
    if (sup == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
                  "--carrier-connect", primary, "--carrier-name", "primary",
                  "--standby-carrier-listen", rescue_spec,
                  "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-connect", primary, "--carrier-name", "primary",
              "--standby-carrier-listen", rescue_spec, (char *)NULL);
        _exit(127);
    }
    if (sup < 0 || wait_ready_layers(control, 1, 15000,
                                     out, sizeof(out)) < 0) {
        why = "initial primary establishment";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker pid";
        goto out;
    }
    old_local = status_value(out, "local_conn=");
    old_peer = status_value(out, "peer_conn=");
    if (control_request_test(control,
            "CARRIER SWITCH missing raw 1000\n", out, sizeof(out)) < 0 ||
        !strstr(out, "ERR CARRIER unknown-carrier")) {
        why = "switch validation";
        goto out;
    }
    sfd = hc2_connect_hello_retry(structured, 3000);
    if (sfd < 0 || make_file(file_src, 1024 * 1024) < 0 ||
        hc2_file_start(sfd, LMUX_HC2_FILE_PUT_START, 600,
                       file_src, file_target) < 0) {
        why = "FILE start";
        goto out;
    }
    while (!file_op) {
        if (hc2_read_frame(sfd, &hc2_type, &hc2_req, hc2_payload,
                           &hc2_len, 5000) < 0 ||
            hc2_type == LMUX_HC2_FILE_ERROR || hc2_type == LMUX_HC2_ERROR) {
            why = "FILE started event";
            goto out;
        }
        if (hc2_type == LMUX_HC2_FILE_STARTED && hc2_len >= 24)
            file_op = hc2_test_get_u64(hc2_payload);
    }
    if (kill(worker, SIGSTOP) < 0) {
        why = "FILE stop worker";
        goto out;
    }
    worker_stopped = 1;
    if (control_request_test(control,
            "CARRIER SWITCH rescue raw 1000\n", out, sizeof(out)) < 0 ||
        !strstr(out, "ERR CARRIER BUSY file-active")) {
        why = "FILE switch BUSY";
        goto out;
    }
    if (kill(worker, SIGCONT) < 0) {
        why = "FILE resume worker";
        goto out;
    }
    worker_stopped = 0;
    for (;;) {
        if (hc2_read_frame(sfd, &hc2_type, &hc2_req, hc2_payload,
                           &hc2_len, 10000) < 0 ||
            hc2_type == LMUX_HC2_FILE_ERROR || hc2_type == LMUX_HC2_ERROR) {
            why = "FILE completion";
            goto out;
        }
        if (hc2_type == LMUX_HC2_FILE_DONE && hc2_len >= 48 &&
            hc2_test_get_u64(hc2_payload) == file_op)
            break;
    }
    close(sfd);
    sfd = -1;
    cfd = wait_connect(console, 5000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s:%s\\n' C PID \"$$\" END",
                         ":END", pty_out, sizeof(pty_out)) < 0 ||
        extract_cpid(pty_out, pid_before, sizeof(pid_before)) < 0) {
        why = "initial PTY pid";
        goto out;
    }

    if (run_lmux_carrier_cli("switch", "rescue", control,
                             out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=rescue mode=raw generation=2")) {
        why = "primary to rescue";
        goto out;
    }
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=rescue generation=2") ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "seq_errors=0") ||
        status_value(out, "local_conn=") == old_local ||
        status_value(out, "peer_conn=") == old_peer ||
        find_child_pid(sup) != worker) {
        why = "rescue observable state";
        goto out;
    }
    if (run_lmux_carrier_cli("status", NULL, control,
                             out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=rescue generation=2") ||
        !strstr(out, "carrier_switch: phase=idle")) {
        why = "carrier CLI status";
        goto out;
    }
    sfd = hc2_connect_hello_retry(structured, 3000);
    if (sfd < 0 || hc2_run_echo(sfd, 700, "B2_STRUCTURED_EXEC") < 0) {
        why = "structured EXEC after rescue";
        goto out;
    }
    close(sfd);
    sfd = -1;
    if (run_lmux_carrier_cli_mode("switch", "rescue", "dirty", control,
                                 out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=rescue mode=dirty generation=3") ||
        read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=rescue generation=3 mode=dirty") ||
        !strstr(out, "connection: ESTABLISHED") ||
        find_child_pid(sup) != worker) {
        why = "rescue raw to dirty";
        goto out;
    }
    sfd = hc2_connect_hello_retry(structured, 3000);
    if (sfd < 0 || hc2_run_echo(sfd, 701, "B4_DIRTY_EXEC") < 0) {
        why = "structured EXEC after dirty";
        goto out;
    }
    close(sfd);
    sfd = -1;
    if (control_request_timeout_test(control,
            "CARRIER SWITCH rescue raw 15000\n", 18000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=rescue mode=raw generation=4") ||
        find_child_pid(sup) != worker) {
        why = "rescue dirty to raw";
        goto out;
    }
    /* Keep the old local endpoint attached through retirement.  This makes
     * LINK_DOWN, rather than an unrelated pre-switch STREAM_CLOSE race, the
     * event that detaches the old stream. */
    close(cfd);
    cfd = -1;
    cfd = wait_connect(console, 5000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s:%s\\n' C PID \"$$\" END",
                         ":END", pty_out, sizeof(pty_out)) < 0 ||
        extract_cpid(pty_out, pid_after, sizeof(pid_after)) < 0 ||
        strcmp(pid_before, pid_after) != 0) {
        why = "PTY durable across rescue";
        goto out;
    }
    close(cfd);
    cfd = -1;

    if (control_request_timeout_test(control,
            "CARRIER SWITCH primary raw 15000\n", 18000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=primary mode=raw generation=5") ||
        read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=primary generation=5") ||
        !strstr(out, "seq_errors=0") ||
        find_child_pid(sup) != worker) {
        why = "rescue to primary";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier raw switch failure: %s\n", why);
        dump_excerpt("carrier raw switch", out);
    }
    if (cfd >= 0)
        close(cfd);
    if (sfd >= 0)
        close(sfd);
    if (worker_stopped && worker > 0)
        (void)kill(worker, SIGCONT);
    stop_pid(host);
    stop_pid(sup);
    unlink(primary);
    unlink(rescue);
    unlink(control);
    unlink(structured);
    unlink(console);
    unlink(file_sock);
    unlink(file_src);
    unlink(file_target);
    rmdir(dir);
    return rc;
}

static int carrier_recovery_without_primary_one(const char *mode)
{
    char dir[] = "/tmp/lmux-carrier-recovery-it-XXXXXX";
    char host_missing[192], target_missing[192], rescue[192];
    char control[192], console[192], file_sock[192];
    char rescue_spec[256], pty_arg[400], file_arg[400], out[16384];
    char request[128], expected[128];
    pid_t host = -1, sup = -1, worker = -1;
    int rc = 1;
    const char *why = "setup";

    if (!mkdtemp(dir))
        return fail("carrier recovery mkdtemp");
    snprintf(host_missing, sizeof(host_missing), "%s/host-primary-missing", dir);
    snprintf(target_missing, sizeof(target_missing), "%s/target-primary-missing", dir);
    snprintf(rescue, sizeof(rescue), "%s/rescue.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(rescue_spec, sizeof(rescue_spec), "rescue=%s", rescue);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    snprintf(request, sizeof(request),
             "CARRIER SWITCH rescue %s 15000\n", mode);
    snprintf(expected, sizeof(expected),
             "OK CARRIER active=rescue mode=%s generation=2", mode);

    sup = fork();
    if (sup == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-connect", target_missing,
              "--standby-carrier-listen", rescue_spec, (char *)NULL);
        _exit(127);
    }
    if (sup < 0 || wait_path_exists(rescue, 5000) < 0) {
        why = "target rescue standby";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker before recovery";
        goto out;
    }
    host = fork();
    if (host == 0) {
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-connect", host_missing,
              "--standby-carrier-connect", rescue_spec,
              "--control", control, "--pty", pty_arg, "--file", file_arg,
              (char *)NULL);
        _exit(127);
    }
    if (host < 0 || wait_path_exists(control, 5000) < 0) {
        why = "host control before recovery";
        goto out;
    }
    if (control_request_timeout_test(control,
            request, 18000,
            out, sizeof(out)) < 0 ||
        !strstr(out, expected) ||
        wait_ready_layers(control, 1, 10000, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=rescue generation=2") ||
        find_child_pid(sup) != worker) {
        why = "standby recovery convergence";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier raw recovery failure: %s\n", why);
        dump_excerpt("carrier raw recovery", out);
    }
    stop_pid(host);
    stop_pid(sup);
    unlink(host_missing);
    unlink(target_missing);
    unlink(rescue);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int carrier_raw_recovery_without_primary_test(void)
{
    return carrier_recovery_without_primary_one("raw") ||
        carrier_recovery_without_primary_one("dirty");
}

static int carrier_raw_ambiguous_fault_one(int drop_host_to_target,
                                           uint8_t drop_type,
                                           const char *label,
                                           int query_recovery_stress)
{
    char dir[] = "/tmp/lmux-carrier-ambiguous-it-XXXXXX";
    char control[192], console[192], file_sock[192];
    char pty_arg[400], file_arg[400], hfd[32], tfd[32];
    char hrescue[64], trescue[64], out[16384];
    int primary[2] = { -1, -1 }, hc[2] = { -1, -1 }, tc[2] = { -1, -1 };
    int note[2] = { -1, -1 }, rc = 1;
    int replay[2] = { -1, -1 };
    pid_t host = -1, sup = -1, proxy = -1, worker = -1;
    const char *why = "setup";

    if (!mkdtemp(dir) || socketpair(AF_UNIX, SOCK_STREAM, 0, primary) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, hc) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, tc) < 0 || pipe(note) < 0 ||
        (query_recovery_stress && pipe(replay) < 0))
        return fail("carrier ambiguous setup");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    snprintf(hfd, sizeof(hfd), "%d", primary[0]);
    snprintf(tfd, sizeof(tfd), "%d", primary[1]);
    snprintf(hrescue, sizeof(hrescue), "rescue=%d", hc[1]);
    snprintf(trescue, sizeof(trescue), "rescue=%d", tc[1]);
    proxy = query_recovery_stress ?
        spawn_lmc_query_recovery_proxy(hc[0], tc[0], note[1], replay[0]) :
        spawn_lmc_drop_proxy(hc[0], tc[0], drop_host_to_target,
                             drop_type, note[1]);
    if (proxy < 0) {
        why = "proxy";
        goto out;
    }
    host = fork();
    if (host == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
                  "--carrier-fd", hfd, "--standby-carrier-fd", hrescue,
                  "--control", control, "--pty", pty_arg, "--file", file_arg,
                  "--verbose", (char *)NULL);
        else
            execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
                  "--carrier-fd", hfd, "--standby-carrier-fd", hrescue,
                  "--control", control, "--pty", pty_arg, "--file", file_arg,
                  (char *)NULL);
        _exit(127);
    }
    sup = fork();
    if (sup == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
                  "--carrier-fd", tfd, "--standby-carrier-fd", trescue,
                  "--verbose", (char *)NULL);
        else
            execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
                  "--carrier-fd", tfd, "--standby-carrier-fd", trescue,
                  (char *)NULL);
        _exit(127);
    }
    close(primary[0]); primary[0] = -1;
    close(primary[1]); primary[1] = -1;
    close(hc[0]); hc[0] = -1;
    close(hc[1]); hc[1] = -1;
    close(tc[0]); tc[0] = -1;
    close(tc[1]); tc[1] = -1;
    close(note[1]); note[1] = -1;
    if (replay[0] >= 0) {
        close(replay[0]);
        replay[0] = -1;
    }
    if (host < 0 || sup < 0 ||
        wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0) {
        why = "initial establishment";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker";
        goto out;
    }
    if (control_request_timeout_test(control,
            "CARRIER SWITCH rescue raw 15000\n", 18000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=rescue mode=raw generation=2") ||
        !(wait_fd_event(note[0], POLLIN, 2000) & POLLIN)) {
        why = "fault recovery switch";
        goto out;
    }
    {
        char mark = 0;
        ssize_t mark_read = read(note[0], &mark, 1);
        int status_rc = read_status(control, out, sizeof(out));
        pid_t current_worker = find_child_pid(sup);
        if (mark_read != 1 ||
            (!query_recovery_stress && mark != 'D') || status_rc < 0 ||
            !strstr(out, "carrier_manager: active=rescue generation=2") ||
            !strstr(out, "seq_errors=0") || current_worker != worker) {
            fprintf(stderr,
                    "carrier ambiguous observation mark_read=%ld mark=%d "
                    "status_rc=%d worker=%ld current=%ld active=%d seq0=%d\n",
                    (long)mark_read, (int)(unsigned char)mark, status_rc,
                    (long)worker, (long)current_worker,
                    strstr(out,
                        "carrier_manager: active=rescue generation=2") != NULL,
                    strstr(out, "seq_errors=0") != NULL);
            why = "fault not observed or bad authority";
            goto out;
        }
        if (query_recovery_stress) {
            const char *connection = strstr(out, "connection: ESTABLISHED");
            unsigned long long outer_local = status_value(
                out, "outer: ESTABLISHED local_conn=");
            unsigned long long outer_peer = status_value(
                out, "peer_conn=");
            unsigned long long lmux_local = connection ?
                status_value(connection, "local_conn=") : 0;
            unsigned long long lmux_peer = connection ?
                status_value(connection, "peer_conn=") : 0;
            unsigned long long established = connection ?
                status_value(connection, "established=") : 0;
            if (write_all(replay[1], (const uint8_t *)"L", 1) < 0 ||
                wait_note_char(note[0], 'A', 5000) < 0 ||
                read_status(control, out, sizeof(out)) < 0 ||
                !strstr(out, "outer: ESTABLISHED") ||
                !strstr(out, "connection: ESTABLISHED") ||
                !strstr(out, "seq_errors=0") ||
                status_value(out, "outer: ESTABLISHED local_conn=") !=
                    outer_local ||
                status_value(out, "peer_conn=") != outer_peer ||
                status_value(strstr(out, "connection: ESTABLISHED"),
                             "local_conn=") != lmux_local ||
                status_value(strstr(out, "connection: ESTABLISHED"),
                             "peer_conn=") != lmux_peer ||
                status_value(strstr(out, "connection: ESTABLISHED"),
                             "established=") != established ||
                find_child_pid(sup) != worker) {
                why = "late duplicate QUERY changed established session";
                goto out;
            }
        }
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier ambiguous %s failure: %s\n", label, why);
        dump_excerpt("carrier ambiguous", out);
    }
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (primary[0] >= 0) close(primary[0]);
    if (primary[1] >= 0) close(primary[1]);
    if (hc[0] >= 0) close(hc[0]);
    if (hc[1] >= 0) close(hc[1]);
    if (tc[0] >= 0) close(tc[0]);
    if (tc[1] >= 0) close(tc[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    if (replay[0] >= 0) close(replay[0]);
    if (replay[1] >= 0) close(replay[1]);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int carrier_raw_ambiguous_fault_test(void)
{
    return carrier_raw_ambiguous_fault_one(0, LMUX_LMC1_COMMITTED,
                                            "lost-COMMITTED", 0) ||
        carrier_raw_ambiguous_fault_one(0, LMUX_LMC1_COMMITTED,
                                        "lost-COMMITTED-two-QUERY_REPLY",
                                        1) ||
        carrier_raw_ambiguous_fault_one(1, LMUX_LMC1_COMMIT,
                                        "lost-COMMIT", 0);
}

static int carrier_dirty_fault_handover_test(void)
{
    char dir[] = "/tmp/lmux-carrier-dirty-fault-it-XXXXXX";
    char control[192], structured[192], hfd[32], tfd[32];
    char hrescue[64], trescue[64], out[16384];
    int primary[2] = { -1, -1 }, hc[2] = { -1, -1 };
    int tc[2] = { -1, -1 }, note[2] = { -1, -1 };
    int sfd = -1, rc = 1;
    pid_t host = -1, sup = -1, proxy = -1, worker = -1;
    const char *why = "setup";
    if (!mkdtemp(dir) || socketpair(AF_UNIX, SOCK_STREAM, 0, primary) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, hc) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, tc) < 0 || pipe(note) < 0)
        return fail("carrier dirty fault setup");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/structured.sock", dir);
    snprintf(hfd, sizeof(hfd), "%d", primary[0]);
    snprintf(tfd, sizeof(tfd), "%d", primary[1]);
    snprintf(hrescue, sizeof(hrescue), "rescue=%d", hc[1]);
    snprintf(trescue, sizeof(trescue), "rescue=%d", tc[1]);
    proxy = spawn_dirty_fault_proxy(hc[0], tc[0], note[1]);
    host = fork();
    if (host == 0) {
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-fd", hfd, "--standby-carrier-fd", hrescue,
              "--control", control, "--structured-control", structured,
              (char *)NULL);
        _exit(127);
    }
    sup = fork();
    if (sup == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-fd", tfd, "--standby-carrier-fd", trescue,
              (char *)NULL);
        _exit(127);
    }
    close(primary[0]); primary[0] = -1;
    close(primary[1]); primary[1] = -1;
    close(hc[0]); hc[0] = -1;
    close(hc[1]); hc[1] = -1;
    close(tc[0]); tc[0] = -1;
    close(tc[1]); tc[1] = -1;
    close(note[1]); note[1] = -1;
    if (host < 0 || sup < 0 || proxy < 0 ||
        wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0) {
        why = "initial establishment";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker";
        goto out;
    }
    if (control_request_timeout_test(control,
            "CARRIER SWITCH rescue dirty 30000\n", 34000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=rescue mode=dirty generation=2") ||
        !(wait_fd_event(note[0], POLLIN, 5000) & POLLIN)) {
        why = "faulted DIRTY convergence";
        goto out;
    }
    {
        char mark = 0;
        if (read(note[0], &mark, 1) != 1 || mark != 'F') {
            why = "fault injection not observed";
            goto out;
        }
    }
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_dirty: state=active") ||
        !strstr(out, "carrier_dirty_rxdiag:") ||
        status_value(out, "retransmitted_fragments=") == 0 ||
        status_value(out, "foreign_bytes=") == 0 ||
        status_value(out, "invalid_alphabet=") == 0 ||
        find_child_pid(sup) != worker) {
        fprintf(stderr,
                "DIRTY status counters retransmit=%llu foreign=%llu alphabet=%llu worker=%ld expected=%ld\n",
                status_value(out, "retransmitted_fragments="),
                status_value(out, "foreign_bytes="),
                status_value(out, "invalid_alphabet="),
                (long)find_child_pid(sup), (long)worker);
        {
            const char *diag = strstr(out, "carrier_dirty_rxdiag:");
            const char *end = diag ? strchr(diag, '\n') : NULL;
            if (diag) {
                size_t width = end ? (size_t)(end - diag) : strlen(diag);
                fprintf(stderr, "DIRTY rxdiag %.*s\n",
                        (int)width, diag);
            }
        }
        why = "DIRTY counters/worker";
        goto out;
    }
    sfd = hc2_connect_hello_retry(structured, 5000);
    if (sfd < 0 || hc2_run_echo(sfd, 811, "B4_DIRTY_FAULT_EXEC") < 0) {
        why = "EXEC after fault recovery";
        goto out;
    }
    close(sfd); sfd = -1;
    if (control_request_timeout_test(control,
            "CARRIER SWITCH primary raw 15000\n", 18000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=primary mode=raw generation=3") ||
        find_child_pid(sup) != worker) {
        why = "DIRTY to primary RAW";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier DIRTY fault failure: %s\n", why);
        dump_excerpt("carrier DIRTY fault", out);
    }
    if (sfd >= 0) close(sfd);
    stop_pid(host); stop_pid(sup); stop_pid(proxy);
    if (primary[0] >= 0) close(primary[0]);
    if (primary[1] >= 0) close(primary[1]);
    if (hc[0] >= 0) close(hc[0]);
    if (hc[1] >= 0) close(hc[1]);
    if (tc[0] >= 0) close(tc[0]);
    if (tc[1] >= 0) close(tc[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(control); unlink(structured); rmdir(dir);
    return rc;
}

static int carrier_dirty_structured_control_test(void)
{
    char dir[] = "/tmp/lmux-carrier-dirty-exec-it-XXXXXX";
    char control[192], structured[192], hfd[32], tfd[32];
    char hrescue[64], trescue[64], out[32768];
    int primary[2] = { -1, -1 }, hc[2] = { -1, -1 };
    int tc[2] = { -1, -1 }, fd = -1, rc = 1;
    pid_t host = -1, sup = -1, proxy = -1, worker = -1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type = 0;
    uint64_t req = 0, ref = 0;
    uint32_t len = 0;
    int heartbeat = 0, reattached = 0, signal_ok = 0, terminal = 0;
    int local_closed = 0;
    long long end;
    const char *why = "setup";
    const char *long_argv[] = {
        "/bin/sh", "-c",
        "trap 'exit 0' TERM; while :; do printf H; sleep 1; done"
    };
    const char *p;

    if (!mkdtemp(dir) || socketpair(AF_UNIX, SOCK_STREAM, 0, primary) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, hc) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, tc) < 0)
        return fail("carrier dirty structured setup");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/structured.sock", dir);
    snprintf(hfd, sizeof(hfd), "%d", primary[0]);
    snprintf(tfd, sizeof(tfd), "%d", primary[1]);
    snprintf(hrescue, sizeof(hrescue), "rescue=%d", hc[1]);
    snprintf(trescue, sizeof(trescue), "rescue=%d", tc[1]);
    proxy = spawn_dirty_baud_proxy(hc[0], tc[0]);
    host = fork();
    if (host == 0) {
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-fd", hfd, "--standby-carrier-fd", hrescue,
              "--control", control, "--structured-control", structured,
              (char *)NULL);
        _exit(127);
    }
    sup = fork();
    if (sup == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-fd", tfd, "--standby-carrier-fd", trescue,
              (char *)NULL);
        _exit(127);
    }
    close(primary[0]); primary[0] = -1;
    close(primary[1]); primary[1] = -1;
    close(hc[0]); hc[0] = -1;
    close(hc[1]); hc[1] = -1;
    close(tc[0]); tc[0] = -1;
    close(tc[1]); tc[1] = -1;
    if (host < 0 || sup < 0 || proxy < 0 ||
        wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0) {
        why = "initial establishment";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker";
        goto out;
    }
    fd = hc2_connect_hello_retry(structured, 5000);
    if (fd < 0 || hc2_exec_start_ready_ref(fd, 900, "", long_argv, 3,
                                           &ref) < 0 || !ref) {
        why = "long EXEC start";
        goto out;
    }
    if (control_request_timeout_test(control,
            "CARRIER SWITCH rescue dirty 30000\n", 34000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=rescue mode=dirty generation=2") ||
        find_child_pid(sup) != worker) {
        why = "RAW to throttled DIRTY";
        goto out;
    }

    /* Use delivered heartbeat events, not a sleep, to age status polling. */
    end = now_ms() + 15000;
    while (now_ms() < end && (!reattached || heartbeat < 6)) {
        int rr = hc2_read_frame(fd, &type, &req, payload, &len,
                                (int)(end - now_ms()));
        if (rr < 0)
            break;
        if (type == LMUX_HC2_EXEC_STATE && req == 0 && len >= 32 &&
            hc2_test_get_u64(payload) == ref &&
            payload[16] == V2_EXEC_STATUS_RUNNING)
            reattached = 1;
        else if (type == LMUX_HC2_EXEC_STDOUT && len >= 8 &&
                 hc2_test_get_u64(payload) == ref)
            heartbeat += (int)(len - 8);
        else if (type == LMUX_HC2_EXEC_LOST ||
                 (type == LMUX_HC2_ERROR && len >= 4 &&
                  lmux_get_le32(payload) != LMUX_HC2_OK)) {
            why = "reattach error";
            goto out;
        }
    }
    if (!reattached || heartbeat < 6) {
        why = "reattach heartbeat";
        goto out;
    }
    if (read_status(control, out, sizeof(out)) < 0) {
        why = "status during DIRTY EXEC";
        goto out;
    }
    for (p = out; (p = strstr(p, "state=LOCAL_CLOSED")) != NULL; p++)
        local_closed++;
    if (local_closed > 4) {
        fprintf(stderr, "dirty structured LOCAL_CLOSED streams=%d\n",
                local_closed);
        why = "control stream retirement backlog";
        goto out;
    }
    if (hc2_run_echo(fd, 901, "DIRTY_SHORT_EXEC") < 0) {
        why = "short EXEC over DIRTY";
        goto out;
    }

    hc2_test_put_u64(payload, ref);
    payload[8] = LMUX_HC2_SIG_TERM;
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_SIGNAL, 902, payload, 9) < 0) {
        why = "TERM send";
        goto out;
    }
    end = now_ms() + 15000;
    while (now_ms() < end && (!signal_ok || !terminal)) {
        if (hc2_read_frame(fd, &type, &req, payload, &len,
                           (int)(end - now_ms())) < 0)
            break;
        if (type == LMUX_HC2_ERROR && req == 902 && len >= 4) {
            if (lmux_get_le32(payload) != LMUX_HC2_OK) {
                why = "TERM reply error";
                goto out;
            }
            signal_ok = 1;
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   hc2_test_get_u64(payload) == ref &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            terminal = 1;
        } else if (type == LMUX_HC2_EXEC_LOST) {
            why = "TERM lost EXEC";
            goto out;
        }
    }
    if (!signal_ok || !terminal) {
        why = "TERM completion";
        goto out;
    }
    hc2_test_put_u64(payload, ref);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_RELEASE, 903, payload, 8) < 0) {
        why = "release send";
        goto out;
    }
    end = now_ms() + 10000;
    while (now_ms() < end) {
        if (hc2_read_frame(fd, &type, &req, payload, &len,
                           (int)(end - now_ms())) < 0)
            break;
        if (type == LMUX_HC2_EXEC_STATE && req == 903) {
            rc = 0;
            break;
        }
        if (type == LMUX_HC2_ERROR && req == 903 && len >= 4 &&
            lmux_get_le32(payload) != LMUX_HC2_OK) {
            why = "release reply error";
            goto out;
        }
    }
    if (rc != 0)
        why = "release completion";
out:
    if (rc != 0) {
        fprintf(stderr,
                "carrier DIRTY structured failure: %s ref=%llu heartbeat=%d reattached=%d local_closed=%d last=%s(%u) req=%llu len=%u\n",
                why, (unsigned long long)ref, heartbeat, reattached,
                local_closed, hc2_type_name(type), type,
                (unsigned long long)req, len);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("carrier DIRTY structured", out);
    }
    if (fd >= 0) close(fd);
    stop_pid(host); stop_pid(sup); stop_pid(proxy);
    if (primary[0] >= 0) close(primary[0]);
    if (primary[1] >= 0) close(primary[1]);
    if (hc[0] >= 0) close(hc[0]);
    if (hc[1] >= 0) close(hc[1]);
    if (tc[0] >= 0) close(tc[0]);
    if (tc[1] >= 0) close(tc[1]);
    unlink(control); unlink(structured); rmdir(dir);
    return rc;
}

static int carrier_dirty_explicit_failure_one(int disappear)
{
    char dir[] = "/tmp/lmux-carrier-dirty-fail-it-XXXXXX";
    char control[192], hfd[32], tfd[32], hrescue[64], trescue[64];
    char out[16384];
    int primary[2] = { -1, -1 }, hc[2] = { -1, -1 };
    int tc[2] = { -1, -1 }, note[2] = { -1, -1 };
    pid_t host = -1, sup = -1, proxy = -1, worker = -1;
    int rc = 1;
    const char *why = "setup";
    if (!mkdtemp(dir) || socketpair(AF_UNIX, SOCK_STREAM, 0, primary) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, hc) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, tc) < 0 || pipe(note) < 0)
        return fail("carrier dirty failure setup");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(hfd, sizeof(hfd), "%d", primary[0]);
    snprintf(tfd, sizeof(tfd), "%d", primary[1]);
    snprintf(hrescue, sizeof(hrescue), "rescue=%d", hc[1]);
    snprintf(trescue, sizeof(trescue), "rescue=%d", tc[1]);
    proxy = spawn_dirty_blackhole_proxy(hc[0], tc[0], note[1], disappear);
    host = fork();
    if (host == 0) {
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-fd", hfd, "--standby-carrier-fd", hrescue,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    sup = fork();
    if (sup == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-fd", tfd, "--standby-carrier-fd", trescue,
              (char *)NULL);
        _exit(127);
    }
    close(primary[0]); primary[0] = -1;
    close(primary[1]); primary[1] = -1;
    close(hc[0]); hc[0] = -1;
    close(hc[1]); hc[1] = -1;
    close(tc[0]); tc[0] = -1;
    close(tc[1]); tc[1] = -1;
    close(note[1]); note[1] = -1;
    if (host < 0 || sup < 0 || proxy < 0 ||
        wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0) {
        why = "initial establishment";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0) {
        why = "worker";
        goto out;
    }
    if (control_request_timeout_test(control,
            "CARRIER SWITCH rescue dirty 5000\n", 9000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "ERR CARRIER committed-but-down") ||
        !(wait_fd_event(note[0], POLLIN, 2000) & POLLIN)) {
        why = "explicit committed-but-down result";
        goto out;
    }
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=rescue generation=2 mode=dirty") ||
        !strstr(out, "protocol_failed=yes") ||
        !strstr(out, "carrier_switch: phase=idle") ||
        !strstr(out, "committed_but_down=yes") ||
        strstr(out, "connection: ESTABLISHED") ||
        find_child_pid(sup) != worker) {
        why = "explicit failure state/worker";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier DIRTY %s failure: %s\n",
                disappear ? "candidate-disappearance" : "explicit", why);
        dump_excerpt("carrier DIRTY failure", out);
    }
    stop_pid(host); stop_pid(sup); stop_pid(proxy);
    if (primary[0] >= 0) close(primary[0]);
    if (primary[1] >= 0) close(primary[1]);
    if (hc[0] >= 0) close(hc[0]);
    if (hc[1] >= 0) close(hc[1]);
    if (tc[0] >= 0) close(tc[0]);
    if (tc[1] >= 0) close(tc[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(control); rmdir(dir);
    return rc;
}

static int carrier_dirty_explicit_failure_test(void)
{
    return carrier_dirty_explicit_failure_one(0) ||
        carrier_dirty_explicit_failure_one(1);
}

static unsigned long long carrier_endpoint_counter(const char *status,
                                                   const char *name,
                                                   const char *field)
{
    char marker[64];
    const char *line, *value;
    snprintf(marker, sizeof(marker), "carrier_endpoint: name=%s ", name);
    line = strstr(status, marker);
    if (!line)
        return ULLONG_MAX;
    value = strstr(line, field);
    if (!value || (strchr(line, '\n') && value > strchr(line, '\n')))
        return ULLONG_MAX;
    return strtoull(value + strlen(field), NULL, 10);
}

static int carrier_dirty_reopen_hold_test(void)
{
    char dir[] = "/tmp/lmux-carrier-dirty-reopen-it-XXXXXX";
    char rescue[192], control[192], hfd[32], tfd[32];
    char hrescue[256], trescue[64], out[16384];
    int primary[2] = { -1, -1 }, tc[2] = { -1, -1 };
    int note[2] = { -1, -1 };
    pid_t host = -1, sup = -1, proxy = -1;
    unsigned long long opens, lost, failed_session;
    int rc = 1;
    const char *why = "setup";
    if (!mkdtemp(dir) || socketpair(AF_UNIX, SOCK_STREAM, 0, primary) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, tc) < 0 || pipe(note) < 0)
        return fail("carrier dirty reopen setup");
    snprintf(rescue, sizeof(rescue), "%s/rescue.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(hfd, sizeof(hfd), "%d", primary[0]);
    snprintf(tfd, sizeof(tfd), "%d", primary[1]);
    snprintf(hrescue, sizeof(hrescue), "rescue=%s", rescue);
    snprintf(trescue, sizeof(trescue), "rescue=%d", tc[1]);
    proxy = spawn_dirty_blackhole_reconnect_proxy(rescue, tc[0], note[1]);
    if (proxy < 0 || wait_path_exists(rescue, 3000) < 0) {
        why = "proxy listener";
        goto out;
    }
    host = fork();
    if (host == 0) {
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-fd", hfd, "--standby-carrier-connect", hrescue,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    sup = fork();
    if (sup == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-fd", tfd, "--standby-carrier-fd", trescue,
              (char *)NULL);
        _exit(127);
    }
    close(primary[0]); primary[0] = -1;
    close(primary[1]); primary[1] = -1;
    close(tc[0]); tc[0] = -1;
    close(tc[1]); tc[1] = -1;
    close(note[1]); note[1] = -1;
    if (host < 0 || sup < 0 ||
        wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0) {
        why = "initial establishment";
        goto out;
    }
    if (control_request_timeout_test(control,
            "CARRIER SWITCH rescue dirty 5000\n", 9000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "ERR CARRIER committed-but-down")) {
        why = "explicit failure";
        goto out;
    }
    {
        long long end = now_ms() + 3000;
        int held_open = 0;
        while (now_ms() < end) {
            if (read_status(control, out, sizeof(out)) == 0 &&
                strstr(out, "name=rescue type=unix-connect-test") &&
                strstr(out, "active=yes mode=dirty state=open") &&
                strstr(out, "protocol_failed=yes")) {
                held_open = 1;
                break;
            }
            usleep(20000);
        }
        if (!held_open) {
            why = "failed incarnation hold status";
            goto out;
        }
    }
    opens = carrier_endpoint_counter(out, "rescue", "opens=");
    lost = carrier_endpoint_counter(out, "rescue", "lost=");
    failed_session = status_value(out, "local_session=");
    if (opens == ULLONG_MAX || lost == ULLONG_MAX || failed_session == 0) {
        why = "failed incarnation counters";
        goto out;
    }
    {
        long long end = now_ms() + 1200;
        while (now_ms() < end) {
            if (read_status(control, out, sizeof(out)) < 0) {
                why = "status during hold";
                goto out;
            }
            if (carrier_endpoint_counter(out, "rescue", "opens=") != opens ||
                carrier_endpoint_counter(out, "rescue", "lost=") != lost) {
                why = "reopen storm after protocol failure";
                goto out;
            }
            usleep(50000);
        }
    }
    if (control_request_timeout_test(control,
            "CARRIER SWITCH primary raw 15000\n", 18000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "OK CARRIER active=primary mode=raw generation=3") ||
        wait_ready_layers(control, 1, 5000, out, sizeof(out)) < 0) {
        why = "return to primary after DIRTY failure";
        goto out;
    }
    if (control_request_timeout_test(control,
            "CARRIER SWITCH rescue dirty 5000\n", 9000,
            out, sizeof(out)) < 0 ||
        !strstr(out, "ERR CARRIER committed-but-down") ||
        read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=rescue generation=4") ||
        !strstr(out, "protocol_failed=yes") ||
        status_value(out, "local_session=") == 0 ||
        status_value(out, "local_session=") == failed_session) {
        why = "fresh DIRTY incarnation after new COMMIT";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier DIRTY reopen hold failure: %s\n", why);
        dump_excerpt("carrier DIRTY reopen hold", out);
    }
    stop_pid(host); stop_pid(sup); stop_pid(proxy);
    if (primary[0] >= 0) close(primary[0]);
    if (primary[1] >= 0) close(primary[1]);
    if (tc[0] >= 0) close(tc[0]);
    if (tc[1] >= 0) close(tc[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(rescue); unlink(control); rmdir(dir);
    return rc;
}

static int carrier_raw_old_loss_timing_one(int after_ready)
{
    char dir[] = "/tmp/lmux-carrier-old-loss-it-XXXXXX";
    char primary_path[192], control[192], tprimary[32];
    char hrescue[64], trescue[64], out[16384];
    int tp[2] = { -1, -1 };
    int hc[2] = { -1, -1 }, tc[2] = { -1, -1 };
    int note[2] = { -1, -1 }, cfd = -1, rc = 1;
    pid_t primary_proxy = -1, candidate_proxy = -1;
    pid_t host = -1, sup = -1, worker = -1;
    unsigned long long old_local = 0, old_peer = 0;
    const char *why = "setup";

    if (!mkdtemp(dir) || socketpair(AF_UNIX, SOCK_STREAM, 0, tp) < 0)
        return fail("carrier old-loss primary setup");
    snprintf(primary_path, sizeof(primary_path), "%s/primary.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);

    /* This proxy is the physical old carrier.  It is spawned before the
     * candidate descriptors exist, so killing it closes every old-carrier
     * peer without accidentally retaining the rescue link. */
    primary_proxy = spawn_persistent_proxy(primary_path, tp[1]);
    if (primary_proxy < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, hc) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, tc) < 0 || pipe(note) < 0) {
        why = "carrier proxies";
        goto out;
    }
    snprintf(tprimary, sizeof(tprimary), "%d", tp[0]);
    snprintf(hrescue, sizeof(hrescue), "rescue=%d", hc[1]);
    snprintf(trescue, sizeof(trescue), "rescue=%d", tc[1]);

    host = fork();
    if (host == 0) {
        close(tp[0]); close(tp[1]);
        close(hc[0]); close(tc[0]); close(tc[1]);
        close(note[0]); close(note[1]);
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-connect", primary_path,
              "--standby-carrier-fd", hrescue,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    sup = fork();
    if (sup == 0) {
        close(tp[1]);
        close(hc[0]); close(hc[1]); close(tc[0]);
        close(note[0]); close(note[1]);
        execl("build/host/lmuxd", "lmuxd", "--supervisor", "--outer",
              "--carrier-fd", tprimary,
              "--standby-carrier-fd", trescue, (char *)NULL);
        _exit(127);
    }
    close(tp[0]); tp[0] = -1;
    close(tp[1]); tp[1] = -1;
    close(hc[1]); hc[1] = -1;
    close(tc[1]); tc[1] = -1;

    /* In the after-READY case, dropping the first COMMIT proves READY was
     * already accepted before the old physical carrier is removed. */
    candidate_proxy = spawn_lmc_drop_proxy(hc[0], tc[0], 1,
        after_ready ? LMUX_LMC1_COMMIT : 0, note[1]);
    close(hc[0]); hc[0] = -1;
    close(tc[0]); tc[0] = -1;
    close(note[1]); note[1] = -1;
    if (host < 0 || sup < 0 || candidate_proxy < 0 ||
        wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0) {
        why = "initial primary establishment";
        goto out;
    }
    worker = wait_child_pid(sup, -1, 5000);
    old_local = status_value(out, "local_conn=");
    old_peer = status_value(out, "peer_conn=");
    if (worker <= 0 || old_local == 0 || old_peer == 0) {
        why = "initial observable state";
        goto out;
    }

    cfd = wait_connect(control, 3000);
    if (cfd < 0) {
        why = "switch control connect";
        goto out;
    }
    if (!after_ready) {
        stop_pid(primary_proxy);
        primary_proxy = -1;
    }
    if (write_all(cfd, "CARRIER SWITCH rescue raw 15000\n", 32) < 0) {
        why = "switch request";
        goto out;
    }
    if (after_ready) {
        char mark = 0;
        if (!(wait_fd_event(note[0], POLLIN, 7000) & POLLIN) ||
            read(note[0], &mark, 1) != 1 || mark != 'D') {
            why = "READY/first COMMIT boundary";
            goto out;
        }
        stop_pid(primary_proxy);
        primary_proxy = -1;
    }
    if (read_until(cfd, "OK CARRIER active=rescue mode=raw generation=2",
                   out, sizeof(out), 18000) < 0) {
        why = after_ready ? "switch after READY loss" :
                            "switch after pre-PREPARE loss";
        goto out;
    }
    close(cfd);
    cfd = -1;
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "carrier_manager: active=rescue generation=2") ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "seq_errors=0") ||
        status_value(out, "local_conn=") == old_local ||
        status_value(out, "peer_conn=") == old_peer ||
        find_child_pid(sup) != worker) {
        why = "post-loss authority/reconciliation";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier old-loss %s failure: %s\n",
                after_ready ? "after-READY" : "before-PREPARE", why);
        dump_excerpt("carrier old-loss", out);
    }
    if (cfd >= 0) close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(primary_proxy);
    stop_pid(candidate_proxy);
    if (tp[0] >= 0) close(tp[0]);
    if (tp[1] >= 0) close(tp[1]);
    if (hc[0] >= 0) close(hc[0]);
    if (hc[1] >= 0) close(hc[1]);
    if (tc[0] >= 0) close(tc[0]);
    if (tc[1] >= 0) close(tc[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(primary_path);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int carrier_raw_old_loss_timing_test(void)
{
    return carrier_raw_old_loss_timing_one(0) ||
        carrier_raw_old_loss_timing_one(1);
}

static int carrier_raw_unavailable_busy_test(void)
{
    char dir[] = "/tmp/lmux-carrier-unavailable-it-XXXXXX";
    char primary[192], rescue[192], control[192], rescue_spec[256];
    char first[8192] = { 0 }, second[512] = { 0 };
    pid_t host = -1;
    int fd1 = -1, fd2 = -1, peer = -1, rc = 1;
    const char *why = "setup";

    if (!mkdtemp(dir))
        return fail("carrier unavailable mkdtemp");
    snprintf(primary, sizeof(primary), "%s/primary-missing", dir);
    snprintf(rescue, sizeof(rescue), "%s/rescue-missing", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(rescue_spec, sizeof(rescue_spec), "rescue=%s", rescue);
    host = fork();
    if (host == 0) {
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier-connect", primary,
              "--standby-carrier-listen", rescue_spec,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    if (host < 0 || wait_path_exists(control, 5000) < 0 ||
        wait_path_exists(rescue, 5000) < 0) {
        why = "host control/listener";
        goto out;
    }
    fd1 = wait_connect(control, 3000);
    if (fd1 < 0 ||
        write_all(fd1, "CARRIER SWITCH rescue raw 1000\n", 31) < 0) {
        why = "first request";
        goto out;
    }
    peer = wait_connect(rescue, 3000);
    if (peer < 0) {
        why = "candidate accept";
        goto out;
    }
    /* A complete QUERY record is the protocol event proving that the first
     * request has entered the switch state machine. */
    {
        uint8_t record[LMUX_LMC1_MAX_ENCODED];
        size_t have = 0;
        long long end = now_ms() + 3000;
        while (have < sizeof(record) && now_ms() < end) {
            int re = wait_fd_event(peer, POLLIN | POLLHUP | POLLERR, 100);
            if (re & POLLIN) {
                ssize_t n = read(peer, record + have, sizeof(record) - have);
                size_t i;
                if (n <= 0)
                    break;
                for (i = 0; i < (size_t)n; i++)
                    if (record[have + i] == 0) {
                        have += i + 1;
                        goto query_seen;
                    }
                have += (size_t)n;
            }
        }
        why = "candidate QUERY";
        goto out;
query_seen:
        ;
    }
    if (read_status(control, first, sizeof(first)) < 0 ||
        !strstr(first, "carrier_switch: phase=") ||
        strstr(first, "carrier_switch: phase=idle")) {
        why = "read-only status during switch";
        goto out;
    }
    fd2 = wait_connect(control, 3000);
    if (fd2 < 0) {
        why = "concurrent request";
        goto out;
    }
    (void)write_all(fd2, "CARRIER SWITCH primary raw 1000\n", 32);
    if (read_until(fd2, "ERR CARRIER BUSY switch-active", second,
                   sizeof(second), 3000) < 0) {
        why = "concurrent BUSY";
        goto out;
    }
    close(fd1);
    fd1 = -1;
    {
        long long end = now_ms() + 4000;
        int finished = 0;
        while (now_ms() < end) {
            if (read_status(control, first, sizeof(first)) == 0 &&
                strstr(first, "carrier_switch: phase=idle") &&
                strstr(first, "last_error=timeout")) {
                finished = 1;
                break;
            }
            usleep(20000);
        }
        if (!finished) {
            why = "daemon switch completion after CLI close";
            goto out;
        }
    }
    if (read_status(control, first, sizeof(first)) < 0) {
        why = "read-only status after ambiguous CLI close";
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "carrier unavailable/BUSY failure: %s\n", why);
        dump_excerpt("carrier unavailable first", first);
        dump_excerpt("carrier unavailable second", second);
    }
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    if (peer >= 0) close(peer);
    stop_pid(host);
    unlink(rescue);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int legacy_named_primary_status_test(void)
{
    char dir[] = "/tmp/lmux-carrier-legacy-it-XXXXXX";
    char carrier[192], control[192], out[8192];
    pid_t host = -1;
    int rc = 1;
    long long end;

    out[0] = '\0';
    if (!mkdtemp(dir))
        return fail("legacy carrier mkdtemp");
    snprintf(carrier, sizeof(carrier), "%s/missing-primary", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    host = fork();
    if (host == 0) {
        execl("build/host/lmuxd", "lmuxd", "--role", "host", "--outer",
              "--carrier", carrier, "--control", control, (char *)NULL);
        _exit(127);
    }
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    end = now_ms() + 3000;
    while (now_ms() < end) {
        if (read_status(control, out, sizeof(out)) == 0 &&
            strstr(out, "carrier_manager: active=primary") &&
            strstr(out, "carrier_endpoint: name=primary type=tty") &&
            strstr(out, "configured=yes active=yes mode=raw state=failed") &&
            strstr(out, carrier)) {
            rc = 0;
            break;
        }
        usleep(20000);
    }
out:
    if (rc != 0)
        dump_excerpt("legacy named primary status", out);
    stop_pid(host);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int named_carrier_cli_rejects(const char *spec)
{
    pid_t pid = fork();
    int st;
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDERR_FILENO);
            close(dn);
        }
        execl("build/host/lmuxd", "lmuxd", "--supervisor",
              "--carrier-connect", "/tmp/lmux-name-validation-unused",
              "--standby-carrier", spec, "--version", (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) != 0 ? 0 : -1;
}

static int named_carrier_cli_validation_test(void)
{
    char long_spec[LMUX_CARRIER_NAME_MAX + 32];
    memset(long_spec, 'a', LMUX_CARRIER_NAME_MAX + 1);
    strcpy(long_spec + LMUX_CARRIER_NAME_MAX + 1, "=/tmp/unused");
    if (named_carrier_cli_rejects("primary=/tmp/duplicate") < 0 ||
        named_carrier_cli_rejects("=/tmp/empty") < 0 ||
        named_carrier_cli_rejects("bad/name=/tmp/invalid") < 0 ||
        named_carrier_cli_rejects(long_spec) < 0)
        return fail("named carrier CLI validation");
    return 0;
}

static int pid_exists(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static int wait_pid_gone(pid_t pid, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (!pid_exists(pid))
            return 0;
        usleep(20000);
    }
    return pid_exists(pid) ? -1 : 0;
}

static int run_lmux_file(const char *op, const char *sock,
                         const char *a, const char *b)
{
    pid_t pid = fork();
    int st;
    if (pid == 0) {
        execl("build/host/lmux", "lmux", op, "--socket", sock, a, b, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static pid_t start_lmux_file(const char *op, const char *sock,
                             const char *a, const char *b)
{
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO)
                close(dn);
        }
        execl("build/host/lmux", "lmux", op, "--socket", sock, a, b, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int run_lmux_boot_cli(const char *cmd, const char *control,
                             char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", cmd,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_boot_upload(const char *control, const char *file,
                                char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0 ||
            dup2(p[1], STDERR_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", "upload", file,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_deploy_ex(const char *control, const char *file,
                              unsigned timeout_ms, int restart_supervisor,
                              char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    char timeout[32];
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    snprintf(timeout, sizeof(timeout), "%u", timeout_ms);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (dup2(p[1], STDERR_FILENO) < 0)
            _exit(127);
        close(p[1]);
        if (restart_supervisor)
            execl("build/host/lmux", "lmux", "deploy", file,
                  "--restart-supervisor", "--timeout-ms", timeout,
                  "--control", control, (char *)NULL);
        else
            execl("build/host/lmux", "lmux", "deploy", file,
                  "--timeout-ms", timeout, "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_deploy(const char *control, const char *file,
                           unsigned timeout_ms, char *out, size_t cap)
{
    return run_lmux_deploy_ex(control, file, timeout_ms, 0, out, cap);
}

static int run_lmux_deploy_restart_only(const char *control,
                                        char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "deploy", "--restart-supervisor",
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_deploy_wait_fault(const char *control, const char *file,
                                      unsigned timeout_ms, int fault_fd,
                                      char *out, size_t cap)
{
    int p[2] = {-1, -1}, st = 0, injected = 0, child_done = 0;
    pid_t pid;
    size_t len = 0;
    char timeout[32];
    long long deadline = now_ms() + 60000;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    snprintf(timeout, sizeof(timeout), "%u", timeout_ms);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "deploy", file,
              "--timeout-ms", timeout, "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    p[1] = -1;
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    (void)test_set_nonblock(p[0]);
    (void)test_set_nonblock(fault_fd);
    while (now_ms() < deadline) {
        struct pollfd pfds[2];
        int n = 0;
        pfds[n].fd = p[0];
        pfds[n].events = POLLIN | POLLHUP | POLLERR | POLLNVAL;
        pfds[n++].revents = 0;
        if (!injected) {
            pfds[n].fd = fault_fd;
            pfds[n].events = POLLIN | POLLHUP | POLLERR | POLLNVAL;
            pfds[n++].revents = 0;
        }
        (void)poll(pfds, n, 100);
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                ssize_t r;
                if (len + 1 >= cap)
                    break;
                r = read(p[0], out + len, cap - len - 1);
                if (r > 0) {
                    len += (size_t)r;
                    out[len] = 0;
                    continue;
                }
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    break;
                break;
            }
        }
        if (n > 1 && (pfds[1].revents & POLLIN)) {
            char ch;
            while (read(fault_fd, &ch, 1) == 1) {
                if (ch == 'I')
                    injected = 1;
            }
        }
        if (!child_done && waitpid(pid, &st, WNOHANG) == pid)
            child_done = 1;
        if (child_done) {
            for (;;) {
                ssize_t r;
                if (len + 1 >= cap)
                    break;
                r = read(p[0], out + len, cap - len - 1);
                if (r > 0) {
                    len += (size_t)r;
                    out[len] = 0;
                    continue;
                }
                break;
            }
            close(p[0]);
            return injected && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
        }
    }
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    close(p[0]);
    return -1;
}

static int run_lmux_boot_activate(const char *control, const char *sha,
                                  unsigned timeout_ms, char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    char timeout[32];
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    snprintf(timeout, sizeof(timeout), "%u", timeout_ms);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", "activate", sha,
              "--timeout-ms", timeout, "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_boot_id(const char *cmd, const char *control,
                            const char *id, char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", cmd, id,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_boot_sha(const char *cmd, const char *control,
                             const char *sha, char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", cmd, sha,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int sha_file_hex(const char *path, char hex[65])
{
    int fd = open(path, O_RDONLY);
    struct sha256_ctx sha;
    uint8_t buf[512], digest[32];
    if (fd < 0)
        return -1;
    sha256_init(&sha);
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r > 0) {
            sha256_update(&sha, buf, (size_t)r);
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    close(fd);
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    return 0;
}

static int copy_with_suffix(const char *src, const char *dst, const char *suffix)
{
    int in = -1, out = -1, rc = -1;
    uint8_t buf[4096];
    in = open(src, O_RDONLY);
    if (in < 0)
        goto done;
    out = open(dst, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (out < 0)
        goto done;
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r > 0) {
            if (write_all(out, buf, (size_t)r) < 0)
                goto done;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        goto done;
    }
    if (suffix && write_all(out, suffix, strlen(suffix)) < 0)
        goto done;
    if (fchmod(out, 0755) < 0)
        goto done;
    rc = 0;
done:
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return rc;
}

static void test_put_le64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)v);
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

static void fill_digest(uint8_t digest[32], unsigned seed)
{
    size_t i;
    for (i = 0; i < 32; i++)
        digest[i] = (uint8_t)(seed + i * 17u + 1u);
}

static int worker_artifact_path(const char *workers, const uint8_t digest[32],
                                char *out, size_t cap)
{
    char hex[65];
    sha256_hex(digest, hex);
    return snprintf(out, cap, "%s/%s.lmuxd", workers, hex) > 0 &&
           strlen(out) < cap ? 0 : -1;
}

static int write_worker_state_fixture(const char *root, uint16_t phase,
                                      uint64_t generation,
                                      uint64_t activation_id,
                                      const uint8_t confirmed[32],
                                      const uint8_t candidate[32],
                                      int corrupt_crc,
                                      int reserved_nonzero)
{
    uint8_t rec[128];
    char path[220];
    uint32_t crc;
    int fd;

    if (snprintf(path, sizeof(path), "%s/worker.state", root) <= 0 ||
        strlen(path) >= sizeof(path))
        return -1;
    memset(rec, 0, sizeof(rec));
    rec[0] = 'L';
    rec[1] = 'M';
    rec[2] = 'W';
    rec[3] = 'S';
    lmux_put_le16(rec + 4, 1);
    lmux_put_le16(rec + 6, phase);
    test_put_le64(rec + 8, generation);
    test_put_le64(rec + 16, activation_id);
    memcpy(rec + 24, confirmed, 32);
    memcpy(rec + 56, candidate, 32);
    if (reserved_nonzero)
        rec[88] = 1;
    crc = lmux_crc32c(rec, 124);
    if (corrupt_crc)
        crc ^= 0x01020304u;
    lmux_put_le32(rec + 124, crc);

    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    if (write_all(fd, rec, sizeof(rec)) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int write_small_executable(const char *path, const char *data)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (fd < 0)
        return -1;
    if (write_all(fd, data, strlen(data)) < 0 || fchmod(fd, 0755) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int partial_upload_disconnect(const char *control, const char *file,
                                     uint64_t size, size_t partial)
{
    int fd = -1, cfd = -1, rc = -1;
    char req[128], line[256];
    uint8_t buf[512];
    size_t sent = 0;
    fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    cfd = connect_unix_test(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "BOOT UPLOAD %llu\n",
             (unsigned long long)size);
    if (write_all(cfd, req, strlen(req)) < 0 ||
        read_line_fd(cfd, line, sizeof(line), 5000) < 0 ||
        strncmp(line, "READY", 5) != 0)
        goto out;
    while (sent < partial) {
        size_t want = partial - sent;
        ssize_t r;
        if (want > sizeof(buf))
            want = sizeof(buf);
        r = read(fd, buf, want);
        if (r <= 0)
            goto out;
        if (write_all(cfd, buf, (size_t)r) < 0)
            goto out;
        sent += (size_t)r;
    }
    rc = 0;
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int partial_deploy_disconnect(const char *control, const char *file,
                                     uint64_t size, size_t partial,
                                     unsigned timeout_ms)
{
    int fd = -1, cfd = -1, rc = -1;
    char req[128], line[256];
    uint8_t buf[512];
    size_t sent = 0;
    fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    cfd = connect_unix_test(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "DEPLOY %llu %u\n",
             (unsigned long long)size, timeout_ms);
    if (write_all(cfd, req, strlen(req)) < 0 ||
        read_line_fd(cfd, line, sizeof(line), 5000) < 0 ||
        strncmp(line, "READY", 5) != 0)
        goto out;
    while (sent < partial) {
        size_t want = partial - sent;
        ssize_t r;
        if (want > sizeof(buf))
            want = sizeof(buf);
        r = read(fd, buf, want);
        if (r <= 0)
            goto out;
        if (write_all(cfd, buf, (size_t)r) < 0)
            goto out;
        sent += (size_t)r;
    }
    rc = 0;
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int deploy_disconnect_after_activate(const char *control,
                                            const char *file,
                                            uint64_t size,
                                            unsigned timeout_ms)
{
    int fd = -1, cfd = -1, rc = -1;
    char req[128], line[256];
    uint8_t buf[512];
    uint64_t sent = 0;
    fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    cfd = connect_unix_test(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "DEPLOY %llu %u\n",
             (unsigned long long)size, timeout_ms);
    if (write_all(cfd, req, strlen(req)) < 0 ||
        read_line_fd(cfd, line, sizeof(line), 5000) < 0 ||
        strncmp(line, "READY", 5) != 0)
        goto out;
    while (sent < size) {
        size_t want = size - sent > sizeof(buf) ? sizeof(buf) : (size_t)(size - sent);
        ssize_t r = read(fd, buf, want);
        if (r <= 0)
            goto out;
        if (write_all(cfd, buf, (size_t)r) < 0)
            goto out;
        sent += (uint64_t)r;
    }
    for (;;) {
        if (read_line_fd(cfd, line, sizeof(line), 20000) < 0)
            goto out;
        if (strncmp(line, "activation started", 18) == 0) {
            rc = 0;
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0)
            goto out;
    }
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int wait_path_absent(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (lstat(path, &(struct stat){0}) < 0 && errno == ENOENT)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int make_file(const char *path, size_t len)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    size_t i;
    if (fd < 0)
        return -1;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)(i * 31u + (i >> 7));
        if (write(fd, &b, 1) != 1) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

static int files_equal(const char *a, const char *b)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    unsigned char ba[512], bb[512];
    if (fa < 0 || fb < 0)
        return -1;
    for (;;) {
        ssize_t ra = read(fa, ba, sizeof(ba));
        ssize_t rb = read(fb, bb, sizeof(bb));
        if (ra != rb) {
            close(fa); close(fb);
            return -1;
        }
        if (ra <= 0) {
            close(fa); close(fb);
            return ra == 0 ? 0 : -1;
        }
        if (memcmp(ba, bb, (size_t)ra) != 0) {
            close(fa); close(fb);
            return -1;
        }
    }
}

static int integration_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-it-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], upload_src[160], upload_final[512], upload_tmp[512];
    char candidate_src[160], candidate_final[512], baseline_final[512];
    char other_src[160], other_final[512], bad_worker_src[160], bad_worker_final[512];
    char deploy_src[160], deploy_final[512];
    char active_dir[160], active_exe[220], active_hex[65];
    char pty_arg[320], file_arg[320];
    char src[160], target[160], got[160];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    int cfd = -1, rc = 1;
    long shell_pid = -1, shell_pid_after = -1, shell_pid_new = -1, shell_pid_sup_restart = -1;
    long shell_pid_candidate = -1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(storage_root, sizeof(storage_root), "%s/custom-storage", dir);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(upload_src, sizeof(upload_src), "%s/upload-worker.bin", dir);
    snprintf(candidate_src, sizeof(candidate_src), "%s/candidate-worker.lmuxd", dir);
    snprintf(other_src, sizeof(other_src), "%s/other-worker.lmuxd", dir);
    snprintf(bad_worker_src, sizeof(bad_worker_src), "%s/bad-worker.lmuxd", dir);
    snprintf(deploy_src, sizeof(deploy_src), "%s/deploy-worker.lmuxd", dir);
    snprintf(active_dir, sizeof(active_dir), "%s/active", dir);
    snprintf(active_exe, sizeof(active_exe), "%s/lmuxd", active_dir);
    snprintf(upload_tmp, sizeof(upload_tmp), "%s/.upload.tmp", workers);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    snprintf(src, sizeof(src), "%s/src.bin", dir);
    snprintf(target, sizeof(target), "%s/target.bin", dir);
    snprintf(got, sizeof(got), "%s/got.bin", dir);

    if (mkdir(storage_root, 0755) < 0 || mkdir(active_dir, 0755) < 0)
        goto out;
    sup = spawn_supervisor_with_storage(carrier,
                                        getenv("LMUX_SUP_IT_OUTER") ? storage_root : NULL);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    where = "host sockets";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_path_exists(console, 5000) < 0 ||
        wait_path_exists(file_sock, 5000) < 0)
        goto out;
    where = "startup layers";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") != NULL,
                          12000, out, sizeof(out)) < 0) {
        dump_excerpt("startup status", out);
        goto out;
    }
    if (getenv("LMUX_SUP_IT_OUTER")) {
        where = "boot get_info";
        if (!boot_info_ok(control, out, sizeof(out))) {
            dump_excerpt("boot get_info", out);
            goto out;
        }
        where = "boot worker_status active";
        if (!boot_worker_status_ok(control, "ACTIVE", out, sizeof(out))) {
            dump_excerpt("boot worker_status", out);
            goto out;
        }
        where = "boot cli";
        if (run_lmux_boot_cli("info", control, out, sizeof(out)) < 0 ||
            !strstr(out, "OK GET_INFO") ||
            run_lmux_boot_cli("worker-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "OK WORKER_STATUS")) {
            dump_excerpt("boot cli", out);
            goto out;
        }
    }
    where = "console through supervisor";
    cfd = wait_connect(console, 10000);
    if (cfd < 0)
        goto out;
    usleep(2000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID \"$$\"", "SPID:", out, sizeof(out)) < 0) {
        dump_excerpt("console first", out);
        goto out;
    }
    shell_pid = parse_pid_marker(out, "SPID:");
    if (shell_pid <= 0) {
        dump_excerpt("console first parse", out);
        goto out;
    }
    if (getenv("LMUX_SUP_IT_OUTER")) {
        int k;
        char hex[65], candidate_hex[65], baseline_hex[65], activation_id[32];
        char deploy_hex[65];
        char stale_activation_id[32], other_hex[65], bad_hex[65];
        unsigned long long outer_local_before, outer_peer_before;
        unsigned long long testing_generation;
        int fd_base = -1, fd_end = -1;
        struct stat st;
        where = "boot during pty";
        for (k = 0; k < 5; k++) {
            if (!boot_info_ok(control, out, sizeof(out)) ||
                !boot_worker_status_ok(control, "ACTIVE", out, sizeof(out))) {
                dump_excerpt("boot during pty", out);
                goto out;
            }
        }
        if (shell_cmd_expect(cfd, "printf '%s\\n' BOOT_PTY_ALIVE",
                             "BOOT_PTY_ALIVE", out, sizeof(out)) < 0) {
            dump_excerpt("pty after boot", out);
            goto out;
        }
        where = "boot upload success";
        if (make_file(upload_src, 384 * 1024) < 0 ||
            sha_file_hex(upload_src, hex) < 0) {
            goto out;
        }
        if (snprintf(upload_final, sizeof(upload_final), "%s/%s.lmuxd",
                     workers, hex) <= 0 ||
            strlen(upload_final) >= sizeof(upload_final))
            goto out;
        where = "boot upload abort cleanup";
        if (partial_upload_disconnect(control, upload_src, 384 * 1024, 4096) < 0 ||
            wait_path_absent(upload_tmp, 5000) < 0) {
            if (lstat(upload_tmp, &st) == 0)
                fprintf(stderr, "upload temp still present after abort\n");
            goto out;
        }
        where = "boot upload success";
        if (run_lmux_boot_upload(control, upload_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            !strstr(out, hex) ||
            lstat(upload_tmp, &st) == 0 ||
            lstat(upload_final, &st) < 0 ||
            (st.st_mode & 0111) == 0 ||
            files_equal(upload_src, upload_final) < 0) {
            dump_excerpt("boot upload", out);
            goto out;
        }
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' U PID \"$$\"",
                             "UPID:", out, sizeof(out)) < 0 ||
            parse_pid_marker(out, "UPID:") != shell_pid) {
            dump_excerpt("pty after upload", out);
            goto out;
        }
        where = "managed activation candidate upload";
        if (copy_with_suffix("build/host/lmuxd", candidate_src,
                             "LMUX_P4_TEST_CANDIDATE\n") < 0 ||
            sha_file_hex(candidate_src, candidate_hex) < 0)
            goto out;
        if (snprintf(candidate_final, sizeof(candidate_final), "%s/%s.lmuxd",
                     workers, candidate_hex) <= 0 ||
            strlen(candidate_final) >= sizeof(candidate_final))
            goto out;
        if (run_lmux_boot_upload(control, candidate_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            lstat(candidate_final, &st) < 0)
            goto out;
        where = "managed activation baseline identity";
        {
            char proc[64];
            snprintf(proc, sizeof(proc), "/proc/%ld/exe", (long)worker);
            if (sha_file_hex(proc, baseline_hex) < 0)
                goto out;
        }
        if (snprintf(baseline_final, sizeof(baseline_final), "%s/%s.lmuxd",
                     workers, baseline_hex) <= 0 ||
            strlen(baseline_final) >= sizeof(baseline_final))
            goto out;
        where = "managed activate";
        if (read_status(control, out, sizeof(out)) < 0)
            goto out;
        outer_local_before = status_value(out, "local_conn=");
        outer_peer_before = status_value(out, "peer_conn=");
        close(cfd);
        cfd = -1;
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") == 0) {
            dump_excerpt("activate", out);
            goto out;
        }
        snprintf(activation_id, sizeof(activation_id), "%llu",
                 status_value(out, "activation_id="));
        if (lstat(baseline_final, &st) < 0)
            goto out;
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            status_value(out, "local_conn=") != outer_local_before ||
            status_value(out, "peer_conn=") != outer_peer_before) {
            dump_excerpt("activate ready", out);
            goto out;
        }
        cfd = wait_connect(console, 10000);
        if (cfd < 0)
            goto out;
        usleep(1000000);
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' C PID \"$$\"",
                             "CPID:", out, sizeof(out)) < 0) {
            dump_excerpt("candidate console", out);
            goto out;
        }
        shell_pid_candidate = parse_pid_marker(out, "CPID:");
        if (shell_pid_candidate <= 0 || shell_pid_candidate == shell_pid) {
            dump_excerpt("candidate pid", out);
            goto out;
        }
        where = "managed explicit rollback";
        snprintf(stale_activation_id, sizeof(stale_activation_id), "%s", activation_id);
        if (run_lmux_boot_id("rollback", control, activation_id, out, sizeof(out)) < 0 ||
            !strstr(out, "OK ROLLBACK")) {
            dump_excerpt("rollback", out);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, baseline_hex)) {
            dump_excerpt("rollback status", out);
            goto out;
        }
        where = "managed activate stable no-op";
        if (run_lmux_boot_activate(control, baseline_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=STABLE") ||
            wait_child_pid(sup, worker, 1000) != -1) {
            dump_excerpt("activate no-op", out);
            goto out;
        }
        where = "managed candidate corruption before activate";
        if (copy_with_suffix("build/host/lmuxd", other_src,
                             "LMUX_P4_TEST_CORRUPT_CANDIDATE\n") < 0 ||
            sha_file_hex(other_src, other_hex) < 0)
            goto out;
        if (snprintf(other_final, sizeof(other_final), "%s/%s.lmuxd",
                     workers, other_hex) <= 0 ||
            strlen(other_final) >= sizeof(other_final))
            goto out;
        if (run_lmux_boot_upload(control, other_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            lstat(other_final, &st) < 0)
            goto out;
        {
            int fd = open(other_final, O_WRONLY | O_APPEND);
            if (fd < 0)
                goto out;
            if (write_all(fd, "corrupt\n", 8) < 0) {
                close(fd);
                goto out;
            }
            close(fd);
        }
        if (run_lmux_boot_activate(control, other_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT") ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, baseline_hex)) {
            dump_excerpt("corrupt candidate activate", out);
            goto out;
        }
        where = "managed candidate crash rollback";
        if (make_file(bad_worker_src, 4096) < 0 ||
            sha_file_hex(bad_worker_src, bad_hex) < 0)
            goto out;
        if (snprintf(bad_worker_final, sizeof(bad_worker_final), "%s/%s.lmuxd",
                     workers, bad_hex) <= 0 ||
            strlen(bad_worker_final) >= sizeof(bad_worker_final))
            goto out;
        if (run_lmux_boot_upload(control, bad_worker_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            lstat(bad_worker_final, &st) < 0)
            goto out;
        if (run_lmux_boot_activate(control, bad_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING"))
            goto out;
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        {
            long long deadline = now_ms() + 12000;
            int stable = 0;
            while (now_ms() < deadline) {
                if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) == 0 &&
                    strstr(out, "phase=STABLE") &&
                    strstr(out, baseline_hex)) {
                    stable = 1;
                    break;
                }
                usleep(50000);
            }
            if (!stable) {
                dump_excerpt("candidate crash rollback status", out);
                goto out;
            }
        }
        worker2 = wait_child_pid(sup, -1, 1000);
        if (worker2 <= 0)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed timeout rollback";
        if (run_lmux_boot_activate(control, candidate_hex, 1000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING")) {
            dump_excerpt("activate timeout", out);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        {
            long long deadline = now_ms() + 8000;
            int stable = 0;
            while (now_ms() < deadline) {
                if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) == 0 &&
                    strstr(out, "phase=STABLE") &&
                    strstr(out, baseline_hex)) {
                    stable = 1;
                    break;
                }
                usleep(50000);
            }
            if (!stable) {
                dump_excerpt("timeout status", out);
                goto out;
            }
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed activate second";
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") == 0) {
            dump_excerpt("activate second", out);
            goto out;
        }
        snprintf(activation_id, sizeof(activation_id), "%llu",
                 status_value(out, "activation_id="));
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed activate testing idempotent";
        if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id))
            goto out;
        testing_generation = status_value(out, "generation=");
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") != strtoull(activation_id, NULL, 10) ||
            status_value(out, "generation=") != testing_generation ||
            wait_child_pid(sup, worker, 1000) != -1) {
            dump_excerpt("testing activate idempotent", out);
            goto out;
        }
        where = "managed activate busy";
        if (copy_with_suffix("build/host/lmuxd", other_src,
                             "LMUX_P4_TEST_BUSY_CANDIDATE\n") < 0 ||
            sha_file_hex(other_src, other_hex) < 0)
            goto out;
        if (run_lmux_boot_upload(control, other_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD"))
            goto out;
        if (run_lmux_boot_activate(control, other_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT BUSY") ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id) ||
            status_value(out, "generation=") != testing_generation) {
            dump_excerpt("testing activate busy", out);
            goto out;
        }
        where = "managed stale activation ids";
        if (!strcmp(stale_activation_id, activation_id) ||
            run_lmux_boot_id("confirm", control, stale_activation_id,
                             out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT ID_MISMATCH") ||
            run_lmux_boot_id("rollback", control, stale_activation_id,
                             out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT ID_MISMATCH") ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id) ||
            status_value(out, "generation=") != testing_generation ||
            wait_child_pid(sup, worker, 1000) != -1) {
            dump_excerpt("stale activation ids", out);
            goto out;
        }
        where = "managed supervisor restart while testing";
        close(cfd);
        cfd = -1;
        stop_pid(sup);
        sup = -1;
        worker = -1;
        sup = spawn_supervisor_with_storage(carrier, storage_root);
        if (sup < 0)
            goto out;
        worker = wait_child_pid(sup, -1, 10000);
        if (worker <= 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, baseline_hex)) {
            dump_excerpt("restart while testing status", out);
            goto out;
        }
        where = "managed activate third";
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") == 0) {
            dump_excerpt("activate third", out);
            goto out;
        }
        snprintf(activation_id, sizeof(activation_id), "%llu",
                 status_value(out, "activation_id="));
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed physical loss pauses testing";
        stop_pid(host);
        host = -1;
        sleep(1);
        if (!pid_exists(worker))
            goto out;
        host = spawn_host(carrier, control, pty_arg, file_arg);
        if (host < 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id)) {
            dump_excerpt("testing after physical loss", out);
            goto out;
        }
        where = "managed confirm";
        if (run_lmux_boot_id("confirm", control, activation_id, out, sizeof(out)) < 0 ||
            !strstr(out, "OK CONFIRM") ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, candidate_hex)) {
            dump_excerpt("confirm/status", out);
            goto out;
        }
        where = "managed stable after supervisor restart";
        close(cfd);
        cfd = -1;
        stop_pid(sup);
        sup = -1;
        worker = -1;
        sup = spawn_supervisor_with_storage(carrier, storage_root);
        if (sup < 0)
            goto out;
        worker = wait_child_pid(sup, -1, 10000);
        if (worker <= 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, candidate_hex)) {
            dump_excerpt("restart managed status", out);
            goto out;
        }
        where = "switch supervisor to temp active executable";
        close(cfd);
        cfd = -1;
        stop_pid(sup);
        sup = -1;
        worker = -1;
        if (copy_with_suffix("build/host/lmuxd", active_exe,
                             "LMUX_ACTIVE_SUPERVISOR_A\n") < 0 ||
            sha_file_hex(active_exe, active_hex) < 0)
            goto out;
        sup = spawn_supervisor_exec_storage(active_exe, carrier, storage_root);
        if (sup < 0)
            goto out;
        worker = wait_child_pid(sup, -1, 10000);
        if (worker <= 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "high-level deploy normal";
        if (copy_with_suffix("build/host/lmuxd", deploy_src,
                             "LMUX_P5_DEPLOY_CANDIDATE\n") < 0 ||
            sha_file_hex(deploy_src, deploy_hex) < 0)
            goto out;
        if (snprintf(deploy_final, sizeof(deploy_final), "%s/%s.lmuxd",
                     workers, deploy_hex) <= 0 ||
            strlen(deploy_final) >= sizeof(deploy_final))
            goto out;
        if (run_lmux_deploy(control, deploy_src, 60000, out, sizeof(out)) < 0 ||
            !strstr(out, "OK DEPLOY") ||
            !strstr(out, "deploy complete") ||
            !strstr(out, deploy_hex)) {
            dump_text_full("deploy normal", out);
            goto out;
        }
        if (sha_file_hex(active_exe, active_hex) < 0 ||
            strcmp(active_hex, deploy_hex) != 0) {
            fprintf(stderr, "active supervisor sha after deploy %s expected %s\n",
                    active_hex, deploy_hex);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy status", out);
            goto out;
        }
        where = "high-level deploy already active";
        if (run_lmux_deploy(control, deploy_src, 30000, out, sizeof(out)) < 0 ||
            !strstr(out, "OK DEPLOY") ||
            !strstr(out, "already active") ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy already active", out);
            goto out;
        }
        where = "high-level deploy candidate crash";
        if (run_lmux_deploy(control, bad_worker_src, 30000, out, sizeof(out)) == 0 ||
            !strstr(out, "ERR DEPLOY") ||
            !strstr(out, "candidate rolled back")) {
            dump_excerpt("deploy crash", out);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy crash rollback status", out);
            goto out;
        }
        where = "managed repeated rollback cycles";
        fd_base = count_fds(sup);
        if (fd_base < 0)
            goto out;
        for (k = 0; k < 3; k++) {
            if (run_lmux_boot_activate(control, baseline_hex, 30000,
                                       out, sizeof(out)) < 0 ||
                !strstr(out, "OK ACTIVATE") ||
                !strstr(out, "phase=TESTING") ||
                status_value(out, "activation_id=") == 0) {
                dump_excerpt("cycle activate", out);
                goto out;
            }
            snprintf(activation_id, sizeof(activation_id), "%llu",
                     status_value(out, "activation_id="));
            worker2 = wait_child_pid(sup, worker, 10000);
            if (worker2 <= 0 || worker2 == worker)
                goto out;
            worker = worker2;
            worker2 = -1;
            if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
                goto out;
            if (run_lmux_boot_id("rollback", control, activation_id,
                                 out, sizeof(out)) < 0 ||
                !strstr(out, "OK ROLLBACK")) {
                dump_excerpt("cycle rollback", out);
                goto out;
            }
            worker2 = wait_child_pid(sup, worker, 10000);
            if (worker2 <= 0 || worker2 == worker)
                goto out;
            worker = worker2;
            worker2 = -1;
            if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
                run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
                !strstr(out, "phase=STABLE") ||
                !strstr(out, deploy_hex)) {
                dump_excerpt("cycle status", out);
                goto out;
            }
        }
        fd_end = wait_fd_count(sup, fd_base, 3000);
        if (fd_end != fd_base) {
            fprintf(stderr, "managed fd baseline base=%d end=%d\n", fd_base, fd_end);
            goto out;
        }
        cfd = wait_connect(console, 10000);
        if (cfd < 0)
            goto out;
        usleep(1000000);
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' R PID \"$$\"",
                             "RPID:", out, sizeof(out)) < 0) {
            dump_excerpt("restart managed console", out);
            goto out;
        }
        shell_pid = parse_pid_marker(out, "RPID:");
        if (shell_pid <= 0)
            goto out;
        close(cfd);
        cfd = -1;
        where = "high-level deploy disconnect before activate";
        if (copy_with_suffix("build/host/lmuxd", other_src,
                             "LMUX_P51_DEPLOY_DISCONNECT_PRE_ACTIVATE\n") < 0 ||
            sha_file_hex(other_src, other_hex) < 0 ||
            stat(other_src, &st) < 0)
            goto out;
        if (partial_deploy_disconnect(control, other_src, (uint64_t)st.st_size,
                                      4096, 30000) < 0 ||
            wait_path_absent(upload_tmp, 5000) < 0 ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy disconnect before activate", out);
            goto out;
        }
        where = "high-level deploy after disconnect cleanup";
        if (run_lmux_deploy(control, deploy_src, 30000, out, sizeof(out)) < 0 ||
            !strstr(out, "OK DEPLOY") ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy after disconnect cleanup", out);
            goto out;
        }
        where = "high-level deploy disconnect after activate";
        fd_base = count_fds(host);
        if (fd_base < 0 ||
            deploy_disconnect_after_activate(control, other_src,
                                             (uint64_t)st.st_size, 30000) < 0)
            goto out;
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        {
            long long deadline = now_ms() + 30000;
            int stable_done = 0;
            while (now_ms() < deadline) {
                if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) == 0 &&
                    strstr(out, "phase=STABLE") &&
                    strstr(out, other_hex)) {
                    stable_done = 1;
                    break;
                }
                usleep(50000);
            }
            if (!stable_done) {
                dump_excerpt("deploy disconnect after activate status", out);
                goto out;
            }
        }
        fd_end = wait_fd_count(host, fd_base, 3000);
        if (fd_end != fd_base) {
            fprintf(stderr, "deploy disconnect fd baseline base=%d end=%d\n",
                    fd_base, fd_end);
            goto out;
        }
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        cfd = wait_connect(console, 10000);
        if (cfd < 0)
            goto out;
        usleep(1000000);
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' D PID \"$$\"",
                             "DPID:", out, sizeof(out)) < 0) {
            dump_excerpt("deploy disconnect shell", out);
            goto out;
        }
        shell_pid = parse_pid_marker(out, "DPID:");
        if (shell_pid <= 0)
            goto out;
        close(cfd);
        cfd = -1;
    }

    where = "file through supervisor";
    if (make_file(src, 256 * 1024) < 0 ||
        run_lmux_file("put", file_sock, src, target) < 0 ||
        run_lmux_file("get", file_sock, target, got) < 0 ||
        files_equal(src, got) < 0)
        goto out;

    where = "carrier loss preserves worker and shell";
    close(cfd);
    cfd = -1;
    stop_pid(host);
    host = -1;
    sleep(1);
    if (wait_child_pid(sup, -1, 1000) != worker)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    cfd = wait_connect(console, 10000);
    usleep(2000000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID2 \"$$\"", "SPID2:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_after = parse_pid_marker(out, "SPID2:");
    if (shell_pid_after != shell_pid)
        goto out;

    where = "worker crash restart";
    close(cfd);
    cfd = -1;
    kill(worker, SIGKILL);
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0)
        goto out;
    if (wait_pid_gone(worker, 3000) < 0 || wait_pid_gone((pid_t)shell_pid, 3000) < 0)
        goto out;
    cfd = wait_connect(console, 12000);
    usleep(2000000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID3 \"$$\"", "SPID3:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_new = parse_pid_marker(out, "SPID3:");
    if (shell_pid_new <= 0 || shell_pid_new == shell_pid)
        goto out;
    if (getenv("LMUX_SUP_IT_OUTER")) {
        where = "boot worker_status after restart";
        if (!boot_worker_status_ok(control, "ACTIVE", out, sizeof(out)) ||
            status_value(out, "restart_count=") < 1) {
            dump_excerpt("boot after worker restart", out);
            goto out;
        }
    }
    if (run_lmux_file("put", file_sock, src, target) < 0 ||
        run_lmux_file("get", file_sock, target, got) < 0 ||
        files_equal(src, got) < 0)
        goto out;

    where = "supervisor restart with host alive";
    close(cfd);
    cfd = -1;
    stop_pid(sup);
    sup = -1;
    worker = worker2;
    sup = spawn_supervisor(carrier);
    if (sup < 0)
        goto out;
    worker2 = wait_child_pid(sup, -1, 10000);
    if (worker2 <= 0)
        goto out;
    sleep(3);
    cfd = wait_connect(console, 12000);
    usleep(2000000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID4 \"$$\"", "SPID4:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_sup_restart = parse_pid_marker(out, "SPID4:");
    if (shell_pid_sup_restart <= 0 || shell_pid_sup_restart == shell_pid_new)
        goto out;

    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    unlink(src);
    unlink(target);
    unlink(got);
    unlink(upload_final);
    unlink(candidate_final);
    unlink(other_final);
    unlink(bad_worker_final);
    unlink(deploy_final);
    unlink(candidate_src);
    unlink(other_src);
    unlink(bad_worker_src);
    unlink(deploy_src);
    unlink(upload_tmp);
    rmdir(workers);
    rmdir(storage_root);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int host_first_startup_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-hostfirst-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    pid_t sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[4096];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp host-first");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    if (wait_path_exists(console, 5000) < 0)
        goto out;
    setenv("LMUX_SUP_TEST_UP_ACK_DELAY_MS", "750", 1);
    sup = spawn_supervisor(carrier);
    unsetenv("LMUX_SUP_TEST_UP_ACK_DELAY_MS");
    if (sup < 0)
        goto out;
    where = "host-first startup layers";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") != NULL,
                          12000, out, sizeof(out)) < 0) {
        dump_excerpt("host-first status", out);
        goto out;
    }
    where = "host-first console";
    cfd = wait_connect(console, 12000);
    if (cfd < 0)
        goto out;
    usleep(1000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' H FIRST \"$$\"",
                         "HFIRST:", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "host_first where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int pending_abandon_test(int outer)
{
    char dir[] = "/tmp/lmux-supervisor-pending-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    pid_t host = -1;
    int dummy = -1, base_fds = -1, end_fds = -1, rc = 1, i;
    char out[4096];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp pending");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    host = spawn_host_listen(carrier, control, pty_arg, file_arg, outer);
    if (host < 0)
        goto out;
    if (wait_path_exists(control, 5000) < 0 ||
        wait_path_exists(console, 5000) < 0)
        goto out;
    dummy = wait_connect(carrier, 5000);
    if (dummy < 0)
        goto out;
    {
        long long end = now_ms() + 5000;
        do {
            if (read_status(control, out, sizeof(out)) == 0 &&
                strstr(out, "carrier: open"))
                break;
            usleep(20000);
        } while (now_ms() < end);
        if (!strstr(out, "carrier: open"))
            goto out;
    }
    if (outer) {
        long long end = now_ms() + 5000;
        do {
            if (read_status(control, out, sizeof(out)) == 0 &&
                strstr(out, "outer: WAIT_HELLO"))
                break;
            usleep(20000);
        } while (now_ms() < end);
        if (!strstr(out, "outer: WAIT_HELLO"))
            goto out;
    }
    base_fds = count_fds(host);
    if (base_fds < 0)
        goto out;
    where = outer ? "outer pending abandon" : "raw pending abandon";
    for (i = 0; i < 100; i++) {
        int cfd = wait_connect(console, 1000);
        if (cfd < 0)
            goto out;
        if (wait_pending_state(control, 1, 1000, out, sizeof(out)) < 0) {
            close(cfd);
            dump_excerpt("pending yes status", out);
            goto out;
        }
        close(cfd);
        if (wait_pending_state(control, 0, 1000, out, sizeof(out)) < 0) {
            dump_excerpt("pending no status", out);
            goto out;
        }
    }
    end_fds = wait_fd_count(host, base_fds, 1000);
    if (end_fds != base_fds) {
        fprintf(stderr, "pending fd baseline base=%d end=%d\n", base_fds, end_fds);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "pending where=%s\n", where);
    if (dummy >= 0)
        close(dummy);
    stop_pid(host);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int outer_boot_worker_backoff_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-boot-backoff-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char pty_arg[320], file_arg[320], out[4096];
    pid_t sup = -1, host = -1;
    int rc = 1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp boot backoff");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_bad_worker(carrier);
    if (sup < 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    where = "outer only ready";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_outer_only(control, 10000, out, sizeof(out)) < 0) {
        dump_excerpt("boot backoff status", out);
        goto out;
    }
    where = "boot info during backoff";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("boot backoff info", out);
        goto out;
    }
    where = "boot worker status backoff";
    if (!boot_worker_status_ok(control, "BACKOFF", out, sizeof(out))) {
        dump_excerpt("boot backoff worker", out);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "boot_backoff where=%s\n", where);
    stop_pid(host);
    stop_pid(sup);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int managed_expect_degraded(const char *storage_root)
{
    char dir[] = "/tmp/lmux-supervisor-managed-degraded-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char pty_arg[320], file_arg[320], out[4096];
    pid_t sup = -1, host = -1;
    int rc = 1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp managed degraded case");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_with_storage(carrier, storage_root);
    if (sup < 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    where = "outer degraded ready";
    if (wait_outer_only(control, 10000, out, sizeof(out)) < 0) {
        dump_excerpt("managed degraded outer", out);
        goto out;
    }
    where = "degraded update status";
    if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=DEGRADED")) {
        dump_excerpt("managed degraded update", out);
        goto out;
    }
    where = "degraded no worker";
    if (wait_child_pid(sup, -1, 500) != -1)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "managed_degraded where=%s\n", where);
    stop_pid(host);
    stop_pid(sup);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int managed_corrupt_state_startup_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-managed-state-XXXXXX";
    char root[180], confirmed_path[220], candidate_path[220];
    uint8_t confirmed[32], candidate[32], zero[32];
    int rc = 1, i;

    if (!mkdtemp(dir))
        return fail("mkdtemp managed corrupt state");
    fill_digest(confirmed, 0x11);
    fill_digest(candidate, 0x77);
    memset(zero, 0, sizeof(zero));

    for (i = 0; i < 6; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        if (mkdir(root, 0755) < 0)
            goto out;
        if (i == 0) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 1, 0) < 0)
                goto out;
        } else if (i == 1) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 9, confirmed, zero, 0, 0) < 0)
                goto out;
        } else if (i == 2) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, candidate, 0, 0) < 0)
                goto out;
        } else if (i == 3) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_TESTING,
                                           1, 0, confirmed, candidate, 0, 0) < 0)
                goto out;
        } else if (i == 4) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_TESTING,
                                           1, 7, confirmed, confirmed, 0, 0) < 0)
                goto out;
        } else {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 0, 1) < 0)
                goto out;
        }
        if (managed_expect_degraded(root) < 0)
            goto out;
        snprintf(confirmed_path, sizeof(confirmed_path), "%s/worker.state", root);
        unlink(confirmed_path);
        rmdir(root);
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "managed_corrupt_state case=%d\n", i);
    for (i = 0; i < 6; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        snprintf(confirmed_path, sizeof(confirmed_path), "%s/worker.state", root);
        snprintf(candidate_path, sizeof(candidate_path), "%s/worker.state.tmp", root);
        unlink(confirmed_path);
        unlink(candidate_path);
        rmdir(root);
    }
    rmdir(dir);
    return rc;
}

static int managed_artifact_failure_startup_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-managed-artifact-XXXXXX";
    char root[180], workers[220], path[320], state[220];
    uint8_t confirmed[32], candidate[32], zero[32];
    int rc = 1, i;

    if (!mkdtemp(dir))
        return fail("mkdtemp managed artifact state");
    fill_digest(confirmed, 0x21);
    fill_digest(candidate, 0x91);
    memset(zero, 0, sizeof(zero));

    for (i = 0; i < 3; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        snprintf(workers, sizeof(workers), "%s/workers", root);
        if (mkdir(root, 0755) < 0 || mkdir(workers, 0755) < 0)
            goto out;
        if (i == 0) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 0, 0) < 0)
                goto out;
        } else if (i == 1) {
            if (worker_artifact_path(workers, confirmed, path, sizeof(path)) < 0 ||
                write_small_executable(path, "wrong artifact\n") < 0 ||
                write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 0, 0) < 0)
                goto out;
        } else {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_TESTING,
                                           2, 77, confirmed, candidate, 0, 0) < 0)
                goto out;
        }
        if (managed_expect_degraded(root) < 0)
            goto out;
        snprintf(state, sizeof(state), "%s/worker.state", root);
        unlink(state);
        if (worker_artifact_path(workers, confirmed, path, sizeof(path)) == 0)
            unlink(path);
        if (worker_artifact_path(workers, candidate, path, sizeof(path)) == 0)
            unlink(path);
        rmdir(workers);
        rmdir(root);
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "managed_artifact_failure case=%d\n", i);
    for (i = 0; i < 3; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        snprintf(workers, sizeof(workers), "%s/workers", root);
        snprintf(state, sizeof(state), "%s/worker.state", root);
        unlink(state);
        if (worker_artifact_path(workers, confirmed, path, sizeof(path)) == 0)
            unlink(path);
        if (worker_artifact_path(workers, candidate, path, sizeof(path)) == 0)
            unlink(path);
        rmdir(workers);
        rmdir(root);
    }
    rmdir(dir);
    return rc;
}

static int outer_host_restart_same_physical_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-host-restart-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    int stable[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[4096];
    const char *where = "setup";
    struct saved_env outer_env = {0, NULL};

    if (!mkdtemp(dir))
        return fail("mkdtemp host restart same physical");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0)
        goto out;
    proxy = spawn_persistent_proxy(carrier, stable[0]);
    close(stable[0]);
    stable[0] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    sup = spawn_supervisor_fd(stable[1], 1);
    close(stable[1]);
    stable[1] = -1;
    if (sup < 0)
        goto out;
    if (save_env_var(&outer_env, "LMUX_SUP_IT_OUTER") < 0)
        goto out;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    host = spawn_host(carrier, control, pty_arg, file_arg);
    restore_env_var("LMUX_SUP_IT_OUTER", &outer_env);
    if (host < 0)
        goto out;
    where = "initial host ready";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("host restart initial status", out);
        goto out;
    }
    stop_pid(host);
    host = -1;
    where = "restarted host ready same physical";
    if (save_env_var(&outer_env, "LMUX_SUP_IT_OUTER") < 0)
        goto out;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    host = spawn_host(carrier, control, pty_arg, file_arg);
    restore_env_var("LMUX_SUP_IT_OUTER", &outer_env);
    if (host < 0 ||
        wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("host restart status", out);
        goto out;
    }
    cfd = wait_connect(console, 12000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' HOST RESTART", "HOSTRESTART", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "host_restart_same_physical where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int outer_supervisor_restart_same_host_physical_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-target-restart-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    int stable[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[4096];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp supervisor restart same physical");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0)
        goto out;
    proxy = spawn_persistent_proxy(carrier, stable[0]);
    close(stable[0]);
    stable[0] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd(stable[1], control, pty_arg, file_arg, 1);
    close(stable[1]);
    stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    sup = spawn_supervisor_connect(carrier, 1);
    if (sup < 0)
        goto out;
    where = "initial supervisor ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("supervisor restart initial status", out);
        goto out;
    }
    stop_pid(sup);
    sup = -1;
    where = "restarted supervisor ready same host physical";
    sup = spawn_supervisor_connect(carrier, 1);
    if (sup < 0 ||
        wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("supervisor restart status", out);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "supervisor_restart_same_physical where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int outer_first_contact_lost_hello_test(int target_first)
{
    char dir[] = "/tmp/lmux-supervisor-first-contact-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int stable[2] = {-1, -1};
    pid_t sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    char boot_after[8192];
    char status_after_first_boot[8192];
    const char *drop_dir = target_first ? "TARGET_TO_HOST" : "HOST_TO_TARGET";
    unsigned long long host_local = 0, host_peer = 0;
    unsigned long long before_boot_tx = 0, before_boot_rx = 0;
    unsigned long long after_boot_tx = 0, after_boot_rx = 0;
    unsigned long long before_boot_boot_tx = 0, after_boot_boot_tx = 0;
    unsigned long long before_resync = 0, after_resync = 0;
    unsigned long long before_crc = 0, after_crc = 0;
    unsigned long long before_seq = 0, after_seq = 0;
    unsigned long long before_proto = 0, after_proto = 0;
    unsigned long long before_pre_est = 0, after_pre_est = 0;
    int first_boot_ok = 0, delayed_boot_ok = 0;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp first contact");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0)
        goto out;

    if (target_first) {
        where = "spawn target";
        sup = spawn_supervisor_fd(stable[1], 1);
        close(stable[1]);
        stable[1] = -1;
        if (sup < 0)
            goto out;
        where = "discard target initial hello";
        if (discard_one_cobs_record(stable[0], 5000) < 0)
            goto out;
        where = "spawn host";
        host = spawn_host_fd(stable[0], control, pty_arg, file_arg, 1);
        close(stable[0]);
        stable[0] = -1;
        if (host < 0)
            goto out;
    } else {
        where = "spawn host";
        host = spawn_host_fd(stable[0], control, pty_arg, file_arg, 1);
        close(stable[0]);
        stable[0] = -1;
        if (host < 0)
            goto out;
        where = "discard host initial hello";
        if (discard_one_cobs_record(stable[1], 5000) < 0)
            goto out;
        where = "spawn target";
        sup = spawn_supervisor_fd(stable[1], 1);
        close(stable[1]);
        stable[1] = -1;
        if (sup < 0)
            goto out;
    }

    where = "ready after lost first hello";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0) {
        dump_excerpt("first contact status", out);
        goto out;
    }
    host_local = status_value(out, "local_conn=");
    host_peer = status_value(out, "peer_conn=");
    before_boot_tx = status_value(out, "tx_frames=");
    before_boot_rx = status_value(out, "rx_frames=");
    before_boot_boot_tx = status_value(out, "outer_boot: caps=BOOT_INFO,BOOT_UPLOAD,BOOT_MANAGED,BOOT_SUPERVISOR tx_frames=");
    before_resync = status_value(out, "resync_drops=");
    before_crc = status_value(out, "crc_errors=");
    before_seq = status_value(out, "seq_errors=");
    before_proto = status_value(out, "protocol_errors=");
    before_pre_est = status_value(out, "pre_established_drops=");
    where = "boot after lost first hello";
    first_boot_ok = boot_info_ok(control, out, sizeof(out)) ? 1 : 0;
    status_after_first_boot[0] = 0;
    if (read_status(control, status_after_first_boot,
                    sizeof(status_after_first_boot)) == 0) {
        after_boot_tx = status_value(status_after_first_boot, "tx_frames=");
        after_boot_rx = status_value(status_after_first_boot, "rx_frames=");
        after_boot_boot_tx = status_value(status_after_first_boot, "outer_boot: caps=BOOT_INFO,BOOT_UPLOAD,BOOT_MANAGED,BOOT_SUPERVISOR tx_frames=");
        after_resync = status_value(status_after_first_boot, "resync_drops=");
        after_crc = status_value(status_after_first_boot, "crc_errors=");
        after_seq = status_value(status_after_first_boot, "seq_errors=");
        after_proto = status_value(status_after_first_boot, "protocol_errors=");
        after_pre_est = status_value(status_after_first_boot, "pre_established_drops=");
    }
    delayed_boot_ok = wait_boot_info_ok(control, 5000, boot_after,
                                        sizeof(boot_after)) == 0;
    if (!first_boot_ok && !delayed_boot_ok) {
        dump_excerpt("first contact boot", out);
        dump_excerpt("first contact post-boot status", status_after_first_boot);
        dump_excerpt("first contact delayed boot", boot_after);
        goto out;
    }
    where = "console after lost first hello";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' FIRST CONTACT",
                         "FIRSTCONTACT", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr,
                "outer_first_contact_lost_hello target_first=%d(%s) drop=%s fault_count=1 where=%s host_outer=(%llu,%llu) boot_ok=%d delayed_boot_ok=%d tx_frames=%llu->%llu rx_frames=%llu->%llu boot_tx=%llu->%llu crc=%llu->%llu seq=%llu->%llu resync=%llu->%llu proto=%llu->%llu pre_est=%llu->%llu\n",
                target_first, target_first ? "target starts first" : "host starts first",
                drop_dir, where, host_local, host_peer,
                first_boot_ok, delayed_boot_ok,
                before_boot_tx, after_boot_tx,
                before_boot_rx, after_boot_rx,
                before_boot_boot_tx, after_boot_boot_tx,
                before_crc, after_crc,
                before_seq, after_seq,
                before_resync, after_resync,
                before_proto, after_proto,
                before_pre_est, after_pre_est);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_first_contact_truncated_hello_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-first-trunc-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp first trunc");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(note) < 0)
        goto out;
    proxy = spawn_truncate_first_hello_proxy(host_pair[0], target_pair[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;

    where = "spawn target";
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0)
        goto out;
    where = "truncated target hello";
    if (wait_fault_injected(note[0], 5000) < 0)
        goto out;
    where = "spawn host";
    host = spawn_host_fd(host_pair[1], control, pty_arg, file_arg, 1);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0)
        goto out;

    where = "ready after truncated hello";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 25000, out, sizeof(out)) < 0) {
        dump_excerpt("first trunc status", out);
        goto out;
    }
    where = "boot after truncated hello";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("first trunc boot", out);
        goto out;
    }
    where = "console after truncated hello";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' FIRST TRUNC",
                         "FIRSTTRUNC", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_first_contact_truncated_hello where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_first_contact_lost_ack_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-first-ack-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    char boot_after[8192];
    char status_after_first_boot[8192];
    char drop_dir = 0;
    unsigned long long drop_conn = 0, drop_ack_peer = 0;
    unsigned long long host_local = 0, host_peer = 0;
    unsigned long long before_boot_tx = 0, before_boot_rx = 0;
    unsigned long long after_boot_tx = 0, after_boot_rx = 0;
    unsigned long long before_resync = 0, after_resync = 0;
    int first_boot_ok = 0, delayed_boot_ok = 0;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp first ack");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(note) < 0)
        goto out;
    proxy = spawn_drop_first_ack_proxy(host_pair[0], target_pair[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;

    where = "spawn target";
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0)
        goto out;
    where = "spawn host";
    host = spawn_host_fd(host_pair[1], control, pty_arg, file_arg, 1);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0)
        goto out;
    where = "dropped hello ack";
    if (wait_ack_drop_report(note[0], 5000, &drop_dir,
                             &drop_conn, &drop_ack_peer) < 0)
        goto out;

    where = "ready after lost hello ack";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 25000, out, sizeof(out)) < 0) {
        dump_excerpt("first ack status", out);
        goto out;
    }
    host_local = status_value(out, "local_conn=");
    host_peer = status_value(out, "peer_conn=");
    before_boot_tx = status_value(out, "tx_frames=");
    before_boot_rx = status_value(out, "rx_frames=");
    before_resync = status_value(out, "resync_drops=");
    where = "boot after lost hello ack";
    first_boot_ok = boot_info_ok(control, out, sizeof(out)) ? 1 : 0;
    status_after_first_boot[0] = 0;
    if (read_status(control, status_after_first_boot,
                    sizeof(status_after_first_boot)) == 0) {
        after_boot_tx = status_value(status_after_first_boot, "tx_frames=");
        after_boot_rx = status_value(status_after_first_boot, "rx_frames=");
        after_resync = status_value(status_after_first_boot, "resync_drops=");
    }
    delayed_boot_ok = wait_boot_info_ok(control, 5000, boot_after,
                                        sizeof(boot_after)) == 0;
    if (!first_boot_ok && !delayed_boot_ok) {
        dump_excerpt("first ack boot", out);
        dump_excerpt("first ack post-boot status", status_after_first_boot);
        dump_excerpt("first ack delayed boot", boot_after);
        goto out;
    }
    where = "fault fired once";
    if (wait_no_extra_fault_injected(note[0], 250) < 0)
        goto out;
    where = "console after lost hello ack";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' FIRST ACK",
                         "FIRSTACK", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr,
                "outer_first_contact_lost_ack where=%s drop_dir=%c(%s) drop_conn=%llu drop_ack_peer=%llu host_outer=(%llu,%llu) boot_ok=%d delayed_boot_ok=%d tx_frames=%llu->%llu rx_frames=%llu->%llu resync=%llu->%llu\n",
                where, drop_dir ? drop_dir : '?',
                drop_dir == 'H' ? "HOST_TO_TARGET" :
                (drop_dir == 'T' ? "TARGET_TO_HOST" : "UNKNOWN"),
                drop_conn, drop_ack_peer, host_local, host_peer,
                first_boot_ok, delayed_boot_ok,
                before_boot_tx, after_boot_tx,
                before_boot_rx, after_boot_rx,
                before_resync, after_resync);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_duplicate_hello_burst_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-hello-burst-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp hello burst");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(note) < 0)
        goto out;
    proxy = spawn_duplicate_first_target_hello_proxy(host_pair[0],
                                                     target_pair[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;

    where = "spawn target";
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0)
        goto out;
    where = "spawn host";
    host = spawn_host_fd(host_pair[1], control, pty_arg, file_arg, 1);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0)
        goto out;
    where = "duplicated target hello";
    if (wait_fault_injected(note[0], 5000) < 0)
        goto out;

    where = "ready after duplicate hello burst";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 25000, out, sizeof(out)) < 0) {
        dump_excerpt("hello burst status", out);
        goto out;
    }
    where = "boot after duplicate hello burst";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("hello burst boot", out);
        goto out;
    }
    where = "console after duplicate hello burst";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' HELLO BURST",
                         "HELLOBURST", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_duplicate_hello_burst where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_fault_recovery_test(enum outer_fault_kind kind,
                                     enum outer_fault_dir dir)
{
    char dirbuf[] = "/tmp/lmux-supervisor-fault-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char file_src[160], file_target[160], file_got[160];
    char pty_arg[320], file_arg[320], out[8192];
    int stable[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1, worker = -1;
    int cfd = -1, sfd = -1, rc = 1;
    unsigned long long old_local, old_peer, new_local, new_peer;
    unsigned long long old_crc, old_seq, new_crc, new_seq;
    unsigned long long worker_incarnation = 0, worker_incarnation_after = 0;
    long shell_pid = -1, shell_pid_after = -1;
    const char *where = "setup";

    if (!mkdtemp(dirbuf))
        return fail("mkdtemp outer fault");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dirbuf);
    snprintf(control, sizeof(control), "%s/control.sock", dirbuf);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dirbuf);
    snprintf(console, sizeof(console), "%s/console1.sock", dirbuf);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dirbuf);
    snprintf(file_src, sizeof(file_src), "%s/fault-src.bin", dirbuf);
    snprintf(file_target, sizeof(file_target), "%s/fault-target.bin", dirbuf);
    snprintf(file_got, sizeof(file_got), "%s/fault-got.bin", dirbuf);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy(carrier, stable[0], kind, dir, arm[0], note[1]);
    close(stable[0]); stable[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd_structured(stable[1], control, structured,
                                    pty_arg, file_arg, 1);
    close(stable[1]); stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    sup = spawn_supervisor_connect(carrier, 1);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("fault initial status", out);
        goto out;
    }
    if (wait_exec_service_ready(control, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("fault initial exec", out);
        goto out;
    }
    old_local = status_value(out, "local_conn=");
    old_peer = status_value(out, "peer_conn=");
    old_crc = status_value(out, "crc_errors=");
    old_seq = status_value(out, "seq_errors=");
    if (old_local == 0 || old_peer == 0)
        goto out;
    where = "initial boot status";
    if (run_lmux_boot_cli("worker-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK WORKER_STATUS"))
        goto out;
    worker_incarnation = status_value(out, "worker_incarnation=");
    where = "initial shell";
    cfd = wait_connect(console, 10000);
    if (cfd < 0)
        goto out;
    usleep(2000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' F PID \"$$\"",
                         "FPID:", out, sizeof(out)) < 0)
        goto out;
    shell_pid = parse_pid_marker(out, "FPID:");
    if (shell_pid <= 0)
        goto out;

    where = "arm fault";
    if (write_all(arm[1], "A", 1) < 0)
        goto out;
    if (kind == OUTER_FAULT_SEQ_GAP && dir == OUTER_FAULT_HOST_TO_TARGET) {
        char big[3072];
        memset(big, '#', sizeof(big));
        big[0] = ':';
        big[sizeof(big) - 1] = '\n';
        (void)write_all(cfd, big, sizeof(big));
    } else if (dir == OUTER_FAULT_HOST_TO_TARGET) {
        const char *cmd = "printf 'FAULT_H2T\\n'\n";
        (void)write_all(cfd, cmd, strlen(cmd));
    } else {
        const char *cmd = "printf 'FAULT_T2H\\n'\n";
        (void)write_all(cfd, cmd, strlen(cmd));
    }
    if (wait_fault_injected(note[0], 5000) < 0) {
        dump_excerpt("fault inject wait", out);
        goto out;
    }
    close(cfd);
    cfd = -1;

    where = "fault recovery";
    if (wait_ready_layers_after_outer_change(control, old_local, old_peer,
                                             25000, out, sizeof(out)) < 0) {
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    new_local = status_value(out, "local_conn=");
    new_peer = status_value(out, "peer_conn=");
    new_crc = status_value(out, "crc_errors=");
    new_seq = status_value(out, "seq_errors=");
    if (new_local == old_local || new_peer == old_peer) {
        fprintf(stderr, "outer conn unchanged old=(%llu,%llu) new=(%llu,%llu)\n",
                old_local, old_peer, new_local, new_peer);
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    if (kind == OUTER_FAULT_BAD_CRC && dir == OUTER_FAULT_TARGET_TO_HOST &&
        new_crc <= old_crc) {
        fprintf(stderr, "crc counter did not advance old=%llu new=%llu\n",
                old_crc, new_crc);
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    if (kind == OUTER_FAULT_SEQ_GAP && dir == OUTER_FAULT_TARGET_TO_HOST &&
        new_seq <= old_seq) {
        fprintf(stderr, "seq counter did not advance old=%llu new=%llu\n",
                old_seq, new_seq);
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    if (strstr(out, "protocol_errors=0") == NULL ||
        strstr(out, "stream_errors=0") == NULL ||
        strstr(out, "stale=0") == NULL ||
        strstr(out, "pre_established_drops=0") == NULL) {
        dump_excerpt("fault recovery lmux counters", out);
        goto out;
    }
    if (wait_child_pid(sup, -1, 1000) != worker) {
        fprintf(stderr, "worker pid changed expected=%ld\n", (long)worker);
        goto out;
    }

    where = "post fault boot";
    if (!boot_info_ok(control, out, sizeof(out)) ||
        run_lmux_boot_cli("worker-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK WORKER_STATUS") ||
        !strstr(out, "worker_state=ACTIVE"))
        goto out;
    worker_incarnation_after = status_value(out, "worker_incarnation=");
    if (worker_incarnation_after != worker_incarnation)
        goto out;

    where = "post fault shell";
    cfd = wait_connect(console, 12000);
    if (cfd < 0)
        goto out;
    usleep(2000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' F PID2 \"$$\"",
                         "FPID2:", out, sizeof(out)) < 0) {
        dump_excerpt("post fault shell output", out);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("post fault shell status", out);
        goto out;
    }
    shell_pid_after = parse_pid_marker(out, "FPID2:");
    if (shell_pid_after != shell_pid) {
        fprintf(stderr, "post fault shell pid before=%ld after=%ld\n",
                shell_pid, shell_pid_after);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("post fault shell pid status", out);
        goto out;
    }
    where = "post fault file";
    if (make_file(file_src, 4096) < 0 ||
        run_lmux_file("put", file_sock, file_src, file_target) < 0 ||
        run_lmux_file("get", file_sock, file_target, file_got) < 0 ||
        files_equal(file_src, file_got) < 0)
        goto out;
    where = "post fault structured exec";
    sfd = hc2_connect_hello_retry(structured, 5000);
    if (sfd < 0 || hc2_run_echo(sfd, 9000, "OUTER_RECOVERED") < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0) {
        fprintf(stderr, "outer_fault kind=%s(%d) dir=%s(%d) where=%s\n",
                outer_fault_kind_name(kind), kind,
                outer_fault_dir_name(dir), dir, where);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("outer fault final status", out);
    }
    if (cfd >= 0)
        close(cfd);
    if (sfd >= 0)
        close(sfd);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    unlink(control);
    unlink(structured);
    unlink(console);
    unlink(file_sock);
    unlink(file_src);
    unlink(file_target);
    unlink(file_got);
    unlink(carrier);
    rmdir(dirbuf);
    return rc;
}

static int outer_upload_fault_test(enum outer_fault_kind kind)
{
    char dirbuf[] = "/tmp/lmux-supervisor-upload-fault-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], upload_src[160], upload_src2[160];
    char upload_tmp[512], upload_final[512], upload_final2[512];
    char pty_arg[320], file_arg[320], out[8192], hex[65], hex2[65];
    int stable[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1, worker = -1;
    int rc = 1, upload_rc;
    unsigned long long old_local = 0, old_peer = 0, new_local = 0, new_peer = 0;
    struct stat st;
    const char *where = "setup";
    const char *expect = NULL;

    if (!mkdtemp(dirbuf))
        return fail("mkdtemp upload fault");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dirbuf);
    snprintf(control, sizeof(control), "%s/control.sock", dirbuf);
    snprintf(console, sizeof(console), "%s/console1.sock", dirbuf);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dirbuf);
    snprintf(storage_root, sizeof(storage_root), "%s/custom-storage", dirbuf);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(upload_src, sizeof(upload_src), "%s/upload-worker.bin", dirbuf);
    snprintf(upload_src2, sizeof(upload_src2), "%s/upload-worker-2.bin", dirbuf);
    snprintf(upload_tmp, sizeof(upload_tmp), "%s/.upload.tmp", workers);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy(carrier, stable[0], kind,
                              OUTER_FAULT_HOST_TO_TARGET,
                              arm[0], note[1]);
    close(stable[0]); stable[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd(stable[1], control, pty_arg, file_arg, 1);
    close(stable[1]); stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    if (mkdir(storage_root, 0755) < 0)
        goto out;
    sup = spawn_supervisor_connect_storage(carrier, 1, storage_root);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0)
        goto out;
    old_local = status_value(out, "local_conn=");
    old_peer = status_value(out, "peer_conn=");
    if (make_file(upload_src, 64 * 1024) < 0 ||
        make_file(upload_src2, 32 * 1024 + 17) < 0 ||
        sha_file_hex(upload_src, hex) < 0 ||
        sha_file_hex(upload_src2, hex2) < 0)
        goto out;
    if (snprintf(upload_final, sizeof(upload_final), "%s/%s.lmuxd",
                 workers, hex) <= 0 ||
        strlen(upload_final) >= sizeof(upload_final))
        goto out;
    if (snprintf(upload_final2, sizeof(upload_final2), "%s/%s.lmuxd",
                 workers, hex2) <= 0 ||
        strlen(upload_final2) >= sizeof(upload_final2))
        goto out;

    where = "arm upload fault";
    if (write_all(arm[1], "A", 1) < 0)
        goto out;
    upload_rc = run_lmux_boot_upload(control, upload_src, out, sizeof(out));
    if (wait_fault_injected(note[0], 5000) < 0) {
        dump_excerpt("upload fault inject wait", out);
        goto out;
    }

    if (kind == OUTER_FAULT_BOOT_BAD_HASH)
        expect = "ERR BOOT BAD_HASH";
    else if (kind == OUTER_FAULT_BOOT_BAD_OFFSET)
        expect = "ERR BOOT OFFSET_MISMATCH";

    if (expect) {
        where = "semantic upload error";
        if (upload_rc == 0 || !strstr(out, expect)) {
            dump_excerpt("upload semantic fault", out);
            goto out;
        }
        if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0)
            goto out;
    } else {
        where = "outer upload fault recovery";
        if (upload_rc == 0) {
            dump_excerpt("upload outer fault unexpectedly ok", out);
            goto out;
        }
        if (wait_ready_layers_after_outer_change(control, old_local, old_peer,
                                                 25000, out, sizeof(out)) < 0) {
            dump_excerpt("upload outer fault recovery", out);
            goto out;
        }
        new_local = status_value(out, "local_conn=");
        new_peer = status_value(out, "peer_conn=");
        if (new_local == 0 || new_peer == 0 ||
            new_local == old_local || new_peer == old_peer) {
            fprintf(stderr, "upload fault outer conn old=(%llu,%llu) new=(%llu,%llu)\n",
                    old_local, old_peer, new_local, new_peer);
            dump_excerpt("upload outer fault recovery conn", out);
            goto out;
        }
    }
    where = "fault fired once";
    if (wait_no_extra_fault_injected(note[0], 250) < 0)
        goto out;
    where = "post fault boot info";
    if (wait_boot_info_ok(control, 5000, out, sizeof(out)) < 0) {
        dump_excerpt("post fault boot info", out);
        goto out;
    }

    where = "upload cleanup";
    if (wait_path_absent(upload_tmp, 5000) < 0 ||
        lstat(upload_final, &st) == 0)
        goto out;

    where = "upload after fault";
    if (run_lmux_boot_upload(control, upload_src, out, sizeof(out)) < 0 ||
        !strstr(out, "OK UPLOAD") ||
        !strstr(out, hex) ||
        lstat(upload_tmp, &st) == 0 ||
        lstat(upload_final, &st) < 0 ||
        files_equal(upload_src, upload_final) < 0) {
        dump_excerpt("upload after fault", out);
        goto out;
    }
    where = "second upload after fault";
    if (run_lmux_boot_upload(control, upload_src2, out, sizeof(out)) < 0 ||
        !strstr(out, "OK UPLOAD") ||
        !strstr(out, hex2) ||
        lstat(upload_tmp, &st) == 0 ||
        lstat(upload_final2, &st) < 0 ||
        files_equal(upload_src2, upload_final2) < 0) {
        dump_excerpt("second upload after fault", out);
        goto out;
    }
    where = "fault still fired once";
    if (wait_no_extra_fault_injected(note[0], 250) < 0)
        goto out;
    if (wait_child_pid(sup, -1, 1000) != worker)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_upload_fault kind=%s(%d) where=%s\n",
                outer_fault_kind_name(kind), kind, where);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    unlink(upload_final2);
    unlink(upload_final);
    unlink(upload_tmp);
    unlink(upload_src);
    unlink(upload_src2);
    rmdir(workers);
    rmdir(storage_root);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    unlink(carrier);
    rmdir(dirbuf);
    return rc;
}

static int outer_deploy_confirm_fault_test(enum outer_fault_kind kind)
{
    char dirbuf[] = "/tmp/lmux-supervisor-deploy-fault-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], deploy_src[160], deploy_final[512];
    char pty_arg[320], file_arg[320], out[8192], hex[65];
    int stable[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1, worker = -1, worker2 = -1;
    int rc = 1;
    enum outer_fault_dir dir;
    struct stat st;
    const char *where = "setup";

    if (!mkdtemp(dirbuf))
        return fail("mkdtemp deploy fault");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dirbuf);
    snprintf(control, sizeof(control), "%s/control.sock", dirbuf);
    snprintf(console, sizeof(console), "%s/console1.sock", dirbuf);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dirbuf);
    snprintf(storage_root, sizeof(storage_root), "%s/custom-storage", dirbuf);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(deploy_src, sizeof(deploy_src), "%s/deploy-worker.lmuxd", dirbuf);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    dir = kind == OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC ?
        OUTER_FAULT_TARGET_TO_HOST : OUTER_FAULT_HOST_TO_TARGET;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy(carrier, stable[0], kind, dir, arm[0], note[1]);
    close(stable[0]); stable[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd(stable[1], control, pty_arg, file_arg, 1);
    close(stable[1]); stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    if (mkdir(storage_root, 0755) < 0)
        goto out;
    sup = spawn_supervisor_connect_storage(carrier, 1, storage_root);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0)
        goto out;
    where = "prepare deploy candidate";
    if (copy_with_suffix("build/host/lmuxd", deploy_src,
                         kind == OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC ?
                         "LMUX_P51_CONFIRM_RESPONSE_FAULT\n" :
                         "LMUX_P51_CONFIRM_REQUEST_FAULT\n") < 0 ||
        sha_file_hex(deploy_src, hex) < 0)
        goto out;
    if (snprintf(deploy_final, sizeof(deploy_final), "%s/%s.lmuxd",
                 workers, hex) <= 0 ||
        strlen(deploy_final) >= sizeof(deploy_final))
        goto out;
    where = "arm deploy confirm fault";
    if (write_all(arm[1], "A", 1) < 0)
        goto out;
    if (run_lmux_deploy_wait_fault(control, deploy_src, 30000, note[0],
                                   out, sizeof(out)) < 0 ||
        !strstr(out, "OK DEPLOY") ||
        !strstr(out, hex)) {
        dump_excerpt("deploy confirm fault output", out);
        goto out;
    }
    where = "deploy confirm fault final status";
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0 || worker2 == worker)
        goto out;
    worker = worker2;
    worker2 = -1;
    if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
        run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=STABLE") ||
        !strstr(out, hex) ||
        lstat(deploy_final, &st) < 0) {
        dump_excerpt("deploy confirm fault status", out);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_deploy_confirm_fault kind=%s(%d) where=%s\n",
                outer_fault_kind_name(kind), kind, where);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    unlink(deploy_final);
    unlink(deploy_src);
    rmdir(workers);
    rmdir(storage_root);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    unlink(carrier);
    rmdir(dirbuf);
    return rc;
}

static int supervisor_install_restart_shortcut_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-install-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], active_dir[160], active_exe[220];
    char worker_b[220], worker_c[220], final_b[512], worker_state[220];
    char active_hex[65], b_hex[65], c_hex[65];
    char pty_arg[320], file_arg[320], out[8192], activation_id[32];
    const char *where = "setup";
    pid_t respawner = -1, sup = -1, worker = -1, worker2 = -1, sup2 = -1;
    pid_t host = -1;
    unsigned long long generation_before, generation_after;
    int ready_rc = -1;
    int rc = 1;

    if (!mkdtemp(dir))
        return fail("mkdtemp supervisor install");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(storage_root, sizeof(storage_root), "%s/storage", dir);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(active_dir, sizeof(active_dir), "%s/active", dir);
    snprintf(active_exe, sizeof(active_exe), "%s/lmuxd", active_dir);
    snprintf(worker_b, sizeof(worker_b), "%s/worker-b.lmuxd", dir);
    snprintf(worker_c, sizeof(worker_c), "%s/worker-c.lmuxd", dir);
    snprintf(worker_state, sizeof(worker_state), "%s/worker.state", storage_root);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    where = "setup mkdir storage";
    if (mkdir(storage_root, 0755) < 0)
        goto out;
    where = "setup mkdir active";
    if (mkdir(active_dir, 0755) < 0)
        goto out;
    where = "setup copy active";
    if (copy_with_suffix("build/host/lmuxd", active_exe,
                         "LMUX_SUPERVISOR_INSTALL_A\n") < 0)
        goto out;
    where = "setup copy worker B";
    if (copy_with_suffix("build/host/lmuxd", worker_b,
                         "LMUX_SUPERVISOR_INSTALL_B\n") < 0)
        goto out;
    where = "setup copy worker C";
    if (copy_with_suffix("build/host/lmuxd", worker_c,
                         "LMUX_SUPERVISOR_INSTALL_C\n") < 0)
        goto out;
    where = "setup sha active";
    if (sha_file_hex(active_exe, active_hex) < 0)
        goto out;
    where = "setup sha worker B";
    if (sha_file_hex(worker_b, b_hex) < 0)
        goto out;
    where = "setup sha worker C";
    if (sha_file_hex(worker_c, c_hex) < 0)
        goto out;
    where = "setup distinct hashes";
    if (strcmp(active_hex, b_hex) == 0)
        goto out;
    where = "setup final path";
    if (snprintf(final_b, sizeof(final_b), "%s/%s.lmuxd", workers, b_hex) <= 0 ||
        strlen(final_b) >= sizeof(final_b))
        goto out;

    where = "setup spawn respawner";
    respawner = spawn_supervisor_respawner(active_exe, carrier, storage_root);
    if (respawner <= 0)
        goto out;
    where = "setup wait supervisor";
    sup = wait_child_pid(respawner, -1, 10000);
    if (sup <= 0)
        goto out;
    where = "setup wait worker";
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    where = "setup spawn host";
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    where = "setup wait ready";
    ready_rc = wait_ready_layers(control, 1, 20000, out, sizeof(out));
    if (ready_rc < 0)
        goto out;

    where = "prepare stable worker B without supervisor install";
    if (run_lmux_boot_upload(control, worker_b, out, sizeof(out)) < 0 ||
        lstat(final_b, &(struct stat){0}) < 0)
        goto out;
    if (run_lmux_boot_activate(control, b_hex, 30000, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=TESTING") ||
        status_value(out, "activation_id=") == 0) {
        dump_excerpt("install shortcut activate", out);
        goto out;
    }
    snprintf(activation_id, sizeof(activation_id), "%llu",
             status_value(out, "activation_id="));
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0 || worker2 == worker)
        goto out;
    worker = worker2;
    worker2 = -1;
    if (wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0)
        goto out;
    if (run_lmux_boot_id("confirm", control, activation_id, out, sizeof(out)) < 0 ||
        run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=STABLE") || !strstr(out, b_hex)) {
        dump_excerpt("install shortcut stable", out);
        goto out;
    }
    generation_before = status_value(out, "generation=");
    if (sha_file_hex(active_exe, active_hex) < 0 || strcmp(active_hex, b_hex) == 0)
        goto out;

    where = "restart rejected before install";
    if (run_lmux_boot_sha("supervisor-restart", control, b_hex,
                          out, sizeof(out)) < 0 ||
        !strstr(out, "ERR BOOT")) {
        dump_excerpt("restart rejected before install", out);
        goto out;
    }

    where = "unconfirmed artifact install rejected";
    if (run_lmux_boot_upload(control, worker_c, out, sizeof(out)) < 0 ||
        run_lmux_boot_sha("supervisor-install", control, c_hex,
                          out, sizeof(out)) < 0 ||
        !strstr(out, "ERR BOOT WRONG_STATE"))
        goto out;

    where = "deploy restart shortcut installs and restarts";
    if (run_lmux_deploy_restart_only(control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK DEPLOY") || !strstr(out, b_hex)) {
        dump_excerpt("restart shortcut", out);
        goto out;
    }
    sup2 = wait_child_pid(respawner, sup, 10000);
    if (sup2 <= 0 || sup2 == sup)
        goto out;
    sup = sup2;
    sup2 = -1;
    if (sha_file_hex(active_exe, active_hex) < 0 || strcmp(active_hex, b_hex) != 0)
        goto out;
    if (wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0 ||
        run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=STABLE") || !strstr(out, b_hex)) {
        dump_excerpt("restart shortcut final", out);
        goto out;
    }
    generation_after = status_value(out, "generation=");
    if (generation_after != generation_before)
        goto out;

    where = "already installed restart shortcut";
    if (run_lmux_deploy_restart_only(control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK DEPLOY")) {
        dump_excerpt("restart shortcut already", out);
        goto out;
    }
    sup2 = wait_child_pid(respawner, sup, 10000);
    if (sup2 <= 0 || sup2 == sup)
        goto out;
    rc = 0;
out:
    if (rc) {
        fprintf(stderr,
                "supervisor_install_restart_shortcut where=%s ready_rc=%d respawner=%ld alive=%d sup=%ld alive=%d worker=%ld alive=%d host=%ld alive=%d paths carrier=%d control=%d console=%d file=%d storage=%d workers=%d\n",
                where, ready_rc,
                (long)respawner, pid_alive(respawner),
                (long)sup, pid_alive(sup),
                (long)worker, pid_alive(worker),
                (long)host, pid_alive(host),
                path_exists_now(carrier), path_exists_now(control),
                path_exists_now(console), path_exists_now(file_sock),
                path_exists_now(storage_root), path_exists_now(workers));
        if (out[0])
            dump_excerpt("install shortcut setup status", out);
    }
    stop_pid(host);
    stop_pgrp(respawner);
    unlink(worker_b);
    unlink(worker_c);
    unlink(active_exe);
    unlink(final_b);
    unlink(worker_state);
    unlink_dir_files(workers);
    rmdir(active_dir);
    rmdir(workers);
    rmdir(storage_root);
    unlink(carrier);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int private_exec_basic_test(void)
{
    char dir[] = "/tmp/lmux-private-exec-it-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160], storage[160];
    char pty_arg[360], file_arg[240];
    char out[32768];
    char req[4096];
    char self_path[256], self_hex[sizeof(self_path) * 2 + 1];
    char helper_hex[64], pgsig_hex[32], bp_hex[32], stderr_bp_hex[64];
    char stdin_delay_hex[64], marker_hex[1100], total_hex[32];
    char stdin_hex[3200];
    char marker[512], marker2[1024];
    char *big = NULL;
    ssize_t self_len;
    size_t bp_count = 0;
    pid_t sup = -1, host = -1;
    int rc = 1;
    uint64_t exec_id = 0, old_id = 0;
    uint64_t ids[V2_EXEC_MAX_COMPLETED + 3];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp private exec");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(storage, sizeof(storage), "%s/storage", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (mkdir(storage, 0700) < 0)
        goto out;
    self_len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (self_len <= 0)
        goto out;
    self_path[self_len] = 0;
    if (hex_encode_cstr(self_path, self_hex, sizeof(self_hex)) < 0 ||
        hex_encode_cstr("--exec-helper", helper_hex, sizeof(helper_hex)) < 0 ||
        hex_encode_cstr("pgsig", pgsig_hex, sizeof(pgsig_hex)) < 0 ||
        hex_encode_cstr("stdout-bp", bp_hex, sizeof(bp_hex)) < 0 ||
        hex_encode_cstr("stderr-bp", stderr_bp_hex, sizeof(stderr_bp_hex)) < 0 ||
        hex_encode_cstr("stdin-delay", stdin_delay_hex, sizeof(stdin_delay_hex)) < 0)
        goto out;
    big = malloc(320000);
    if (!big)
        goto out;

    sup = spawn_supervisor_with_storage(carrier, storage);
    if (sup <= 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0)
        goto out;

    where = "ready";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0) {
        dump_excerpt("private exec status", out);
        goto out;
    }

    where = "true";
    if (exec_request_test(control, "EXEC START - 2f62696e2f74727565\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "exit=0") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec true", out);
        goto out;
    }

    where = "false";
    if (exec_request_test(control, "EXEC START - 2f62696e2f66616c7365\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "exit=1") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec false", out);
        goto out;
    }

    where = "exec failure";
    if (exec_request_test(control, "EXEC START - 2f6e6f2f737563682d6c6d75782d65786563\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=START_FAILED") ||
        !strstr(out, "fail=5") ||
        !strstr(out, "errno=2") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec failure", out);
        goto out;
    }

    where = "stdio";
    if (exec_request_test(control,
                          "EXEC START - 2f62696e2f7368 2d63 7072696e746620455845435f4f55543b207072696e746620455845435f455252203e26323b20636174 --stdin 410042ff0a\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STDOUT_HEX 455845435f4f5554") ||
        !strstr(out, "STDERR_HEX 455845435f455252") ||
        !strstr(out, "410042ff0a") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "exit=0") ||
        !strstr(out, "stdout_eof=1") ||
        !strstr(out, "stderr_eof=1") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec stdio", out);
        goto out;
    }

    where = "release completed";
    if (parse_exec_id_line(out, &old_id) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC RELEASE %llu\n",
             (unsigned long long)old_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC RELEASED")) {
        dump_excerpt("private exec release", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)old_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "ERR EXEC remote status=2")) {
        dump_excerpt("private exec released status", out);
        goto out;
    }

    where = "detached reconnect attach";
    if (exec_request_test(control,
                          "EXEC START_DETACHED - 2f62696e2f7368 2d63 736c65657020313b207072696e746620504f5354\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "DETACHED") ||
        parse_exec_id_line(out, &exec_id) < 0) {
        dump_excerpt("private exec detached start", out);
        goto out;
    }
    stop_pid(host);
    host = -1;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec reconnect attach", out);
        goto out;
    }

    where = "competing attach and reconnect frees";
    if (exec_request_test(control,
                          "EXEC START_DETACHED - 2f62696e2f7368 2d63 736c65657020333b20657869742030\n",
                          out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0) {
        dump_excerpt("private exec conflict start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC ATTACH_CONFLICT %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC ATTACH_BUSY")) {
        dump_excerpt("private exec attach conflict", out);
        goto out;
    }
    stop_pid(host);
    host = -1;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec reconnect after conflict", out);
        goto out;
    }

    where = "signal process group";
    snprintf(marker, sizeof(marker), "%s/pg-int", dir);
    if (mkdir(marker, 0700) < 0 ||
        hex_encode_cstr(marker, marker_hex, sizeof(marker_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s %s\n",
             self_hex, helper_hex, pgsig_hex, marker_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/leader-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/child-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0) {
        dump_excerpt("private exec signal start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC SIGNAL %llu INT\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=6")) {
        dump_excerpt("private exec signal", out);
        goto out;
    }
    snprintf(marker2, sizeof(marker2), "%s/leader-sigint", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(marker2, sizeof(marker2), "%s/child-sigint", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec signal drain", out);
        goto out;
    }

    where = "signal term and stale";
    snprintf(marker, sizeof(marker), "%s/pg-term", dir);
    if (mkdir(marker, 0700) < 0 ||
        hex_encode_cstr(marker, marker_hex, sizeof(marker_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s %s\n",
             self_hex, helper_hex, pgsig_hex, marker_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/leader-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/child-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0) {
        dump_excerpt("private exec term start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC SIGNAL %llu TERM\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=6")) {
        dump_excerpt("private exec term", out);
        goto out;
    }
    snprintf(marker2, sizeof(marker2), "%s/leader-sigterm", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(marker2, sizeof(marker2), "%s/child-sigterm", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec term drain", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC RELEASE %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC RELEASED")) {
        dump_excerpt("private exec term release", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC SIGNAL %llu TERM\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "ERR EXEC remote status=2")) {
        dump_excerpt("private exec stale signal", out);
        goto out;
    }

    where = "stdout backpressure";
    snprintf(marker, sizeof(marker), "%s/stdout-blocked", dir);
    if (hex_encode_cstr(marker, marker_hex, sizeof(marker_hex)) < 0 ||
        hex_encode_cstr("16384", total_hex, sizeof(total_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s %s %s\n",
             self_hex, helper_hex, bp_hex, marker_hex, total_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0 ||
        wait_marker(marker, 5000) < 0 ||
        read_size_marker(marker, &bp_count) < 0 ||
        bp_count == 0 || bp_count > 100000) {
        dump_excerpt("private exec stdout bp start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        (!strstr(out, "STATUS state=2") &&
         !strstr(out, "STATUS state=3"))) {
        dump_excerpt("private exec stdout bp status", out);
        goto out;
    }
    if (!boot_info_ok(control, out, sizeof(out)))
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, big, 320000) < 0 ||
        verify_stdout_hex_pattern(big, bp_count) < 0) {
        char diag[2048];
        fprintf(stderr, "stdout bp expected=%llu host_received=%llu\n",
                (unsigned long long)bp_count,
                (unsigned long long)count_stdout_hex_bytes(big));
        snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
                 (unsigned long long)exec_id);
        if (exec_request_test(control, req, diag, sizeof(diag)) == 0)
            dump_excerpt("private exec stdout bp post-status", diag);
        dump_excerpt("private exec stdout bp attach", big);
        goto out;
    }

    where = "stderr independent backpressure";
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s\n",
             self_hex, helper_hex, stderr_bp_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0) {
        dump_excerpt("private exec stderr bp start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC ATTACH_STDOUT_ONCE %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "5354444f55545f4f4b")) {
        dump_excerpt("private exec stderr bp stdout", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC RELEASE %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC RELEASED")) {
        dump_excerpt("private exec stderr bp release", out);
        goto out;
    }

    where = "stdin backpressure";
    if (hex_encode_pattern(1500, stdin_hex, sizeof(stdin_hex)) < 0 ||
        hex_encode_cstr("1500", total_hex, sizeof(total_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START - %s %s %s %s --stdin %s\n",
             self_hex, helper_hex, stdin_delay_hex, total_hex, stdin_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "535444494e5f4f4b") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec stdin backpressure", out);
        goto out;
    }

    where = "stdin epipe";
    if (exec_request_test(control,
                          "EXEC START - 2f62696e2f7368 2d63 6578656320303c262d3b20657869742030 --stdin 00010203040506070809\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec epipe", out);
        goto out;
    }

    where = "exit drain repeated";
    for (int rep = 0; rep < 5; rep++) {
        if (exec_request_test(control,
                              "EXEC START - 2f62696e2f7368 2d63 7072696e7466204f55545f5441494c3b207072696e7466204552525f5441494c203e26323b20657869742030\n",
                              out, sizeof(out)) < 0 ||
            !strstr(out, "4f55545f5441494c") ||
            !strstr(out, "4552525f5441494c") ||
            !strstr(out, "STATUS state=4") ||
            !strstr(out, "\nDONE\n")) {
            dump_excerpt("private exec repeated drain", out);
            goto out;
        }
    }

    where = "completed gc";
    usleep(500000);
    for (int gi = 0; gi < V2_EXEC_MAX_COMPLETED + 2; gi++) {
        if (exec_request_test(control, "EXEC START - 2f62696e2f74727565\n",
                              out, sizeof(out)) < 0 ||
            parse_exec_id_line(out, &ids[gi]) < 0 ||
            (gi > 0 && ids[gi] <= ids[gi - 1]) ||
            !strstr(out, "STATUS state=4")) {
            dump_excerpt("private exec gc start", out);
            if (read_status(control, out, sizeof(out)) == 0)
                dump_excerpt("private exec gc status", out);
            goto out;
        }
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)ids[0]);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "ERR EXEC remote status=2")) {
        dump_excerpt("private exec gc evicted", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)ids[V2_EXEC_MAX_COMPLETED + 1]);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4")) {
        dump_excerpt("private exec gc retained", out);
        goto out;
    }

    where = "exit drain tail";
    if (exec_request_test(control,
                          "EXEC START - 2f62696e2f7368 2d63 693d303b207768696c65205b202469202d6c74203830205d3b20646f207072696e746620303132333435363738396162636465663b20693d242828692b3129293b20646f6e65\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "30313233343536373839616263646566") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec exit drain", out);
        goto out;
    }

    rc = 0;
out:
    if (rc) {
        fprintf(stderr, "private_exec_basic where=%s\n", where);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("private exec final status", out);
    }
    stop_pid(host);
    stop_pid(sup);
    free(big);
    unlink(carrier);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(storage);
    rmdir(dir);
    return rc;
}

static int private_exec_stdin_epipe_test(void)
{
    char dir[] = "/tmp/lmux-private-exec-epipe-it-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160], storage[160];
    char pty_arg[360], file_arg[240];
    char out[32768];
    pid_t sup = -1, host = -1;
    int rc = 1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp private exec epipe");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(storage, sizeof(storage), "%s/storage", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (mkdir(storage, 0700) < 0)
        goto out;
    sup = spawn_supervisor_with_storage(carrier, storage);
    if (sup <= 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0)
        goto out;

    where = "ready";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0) {
        dump_excerpt("private exec epipe status", out);
        goto out;
    }

    where = "stdin epipe";
    if (exec_request_test(control,
                          "EXEC START - 2f62696e2f7368 2d63 6578656320303c262d3b20657869742030 --stdin 00010203040506070809\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec epipe focused", out);
        goto out;
    }
    rc = 0;
out:
    if (rc) {
        fprintf(stderr, "private_exec_stdin_epipe where=%s\n", where);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("private exec epipe final status", out);
    }
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(storage);
    rmdir(dir);
    return rc;
}

static int structured_control_test(void)
{
    char dir[] = "/tmp/lmux-structured-it-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char file_src[160], file_target[160], file_got[160], src_hex[65], got_hex[65];
    char pty_arg[360], file_arg[240], out[32768];
    pid_t sup = -1, host = -1, worker = -1;
    int fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD], stdin_payload[64];
    uint16_t type;
    uint64_t req, ref = 0;
    uint64_t file_op = 0;
    uint32_t len;
    const char *argv1[] = {"/bin/sh", "-c", "printf OUT; printf ERR >&2"};
    const char *argv2[] = {"/bin/cat"};
    const char *argv3[] = {"/bin/sh", "-c", "sleep 2; printf LATE"};
    const char *argv4[] = {"/bin/sh", "-c", "i=0; while [ $i -lt 5 ]; do printf X; i=$((i+1)); sleep 0.1; done"};
    const char *argv5[] = {"/bin/sh", "-c", "printf A; read line; printf B"};
    const char *argv6[] = {"/bin/sh", "-c", "dd of=/dev/null bs=1 count=1 2>/dev/null; printf CTRL; sleep 5"};
    char stdout_seen[64] = {0}, stderr_seen[64] = {0}, cat_seen[64] = {0};
    size_t cat_len = 0;
    int done = 0;
    int worker_stopped = 0;
    const char *where = "setup";
    int file_disconnect_errno = 0;
    int file_disconnect_state = -1;
    uint64_t file_disconnect_expected_req = 0;
    uint64_t file_disconnect_observed_op = 0;

    if (hc2_reader_fragmentation_selftest() < 0) {
        fprintf(stderr, "structured_control where=reader fragmentation\n");
        return 1;
    }
    if (!mkdtemp(dir))
        return fail("mkdtemp structured");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(file_src, sizeof(file_src), "%s/structured-src.bin", dir);
    snprintf(file_target, sizeof(file_target), "%s/structured-target.bin", dir);
    snprintf(file_got, sizeof(file_got), "%s/structured-got.bin", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0)
        goto out;
    where = "ready";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0)
        goto out;
    {
        struct stat st_sock;
        where = "structured socket mode";
        if (lstat(structured, &st_sock) < 0 ||
            (st_sock.st_mode & 0777) != 0777) {
            fprintf(stderr, "structured socket mode=%03o\n",
                    (unsigned)(st_sock.st_mode & 0777));
            goto out;
        }
    }
    if (wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0) {
        fprintf(stderr, "has_exec=%d has_type=%d has_ack=%d\n",
                strstr(out, "__exec") != NULL,
                strstr(out, "type=exec") != NULL,
                strstr(out, "acknowledged=yes") != NULL);
        fprintf(stderr, "%s\n", out);
        dump_excerpt("structured exec service status", out);
        goto out;
    }

    where = "handshake bad magic";
    if (hc2_bad_magic_smoke(structured) < 0)
        goto out;
    where = "handshake malformed";
    if (hc2_malformed_smoke(structured) < 0)
        goto out;
    where = "handshake hello";
    fd = hc2_connect_hello_retry(structured, 3000);
    if (fd < 0)
        goto out;
    where = "handshake second client";
    if (hc2_second_client_rejected(structured) < 0)
        goto out;
    where = "handshake status send";
    if (hc2_send_frame(fd, LMUX_HC2_STATUS_REQUEST, 2, NULL, 0) < 0)
        goto out;
    where = "handshake status tiny read";
    if (hc2_read_frame_tiny(fd, &type, &req, payload, &len, 3000) < 0)
        goto out;
    where = "handshake status frame";
    if (type != LMUX_HC2_STATUS_REPLY || req != 2 || len < 64)
        goto out;
    where = "handshake duplicate hello";
    if (hc2_send_frame(fd, LMUX_HC2_CLIENT_HELLO, 24, NULL, 0) < 0 ||
        hc2_expect_error(fd, 24, LMUX_HC2_E_PROTOCOL) < 0)
        goto out;
    close(fd);
    fd = -1;
    where = "handshake fresh after duplicate";
    fd = hc2_connect_hello_retry(structured, 3000);
    if (fd < 0)
        goto out;
    where = "handshake legacy status";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          3000, out, sizeof(out)) < 0)
        goto out;

    where = "exec stdout stderr";
    if (hc2_exec_start_ready(fd, 3, "", argv1, 3) < 0)
        goto out;
    done = 0;
    while (!done) {
        where = "exec stdout stderr read";
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t n = len - 8;
            if (n + strlen(stdout_seen) >= sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + strlen(stdout_seen), payload + 8, n);
        } else if (type == LMUX_HC2_EXEC_STDERR && len >= 8) {
            size_t n = len - 8;
            if (n + strlen(stderr_seen) >= sizeof(stderr_seen))
                goto out;
            memcpy(stderr_seen + strlen(stderr_seen), payload + 8, n);
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_ERROR) {
            fprintf(stderr, "structured exec stdout/stderr error len=%u code0=%u code8=%u\n",
                    len, len >= 4 ? lmux_get_le32(payload) : 0,
                    len >= 12 ? lmux_get_le32(payload + 8) : 0);
            goto out;
        }
    }
    where = "exec stdout stderr compare";
    if (strcmp(stdout_seen, "OUT") != 0 || strcmp(stderr_seen, "ERR") != 0)
        goto out;

    where = "exec stdin close";
    memset(stdout_seen, 0, sizeof(stdout_seen));
    if (hc2_exec_start(fd, 4, "", argv2, 1) < 0)
        goto out;
    ref = 0;
    for (;;) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STARTED && len >= 32) {
            ref = hc2_test_get_u64(payload);
            break;
        }
    }
    hc2_test_put_u64(stdin_payload, ref);
    memcpy(stdin_payload + 8, "A\0B\n", 4);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_DATA, 5,
                       stdin_payload, 12) < 0 ||
        hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_CLOSE, 6,
                       stdin_payload, 8) < 0)
        goto out;
    done = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            if (cat_len + len - 8 > sizeof(cat_seen))
                goto out;
            memcpy(cat_seen + cat_len, payload + 8, len - 8);
            cat_len += len - 8;
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }
    if (cat_len != 4 || memcmp(cat_seen, "A\0B\n", 4) != 0)
        goto out;

    where = "exec live one-byte stdout";
    memset(stdout_seen, 0, sizeof(stdout_seen));
    cat_len = 0;
    if (hc2_exec_start(fd, 9, "", argv5, 3) < 0)
        goto out;
    ref = 0;
    while (!ref || cat_len < 1) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STARTED && len >= 32) {
            ref = hc2_test_get_u64(payload);
        } else if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            if (cat_len + len - 8 > sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + cat_len, payload + 8, len - 8);
            cat_len += len - 8;
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            goto out;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }
    if (cat_len != 1 || stdout_seen[0] != 'A')
        goto out;
    hc2_test_put_u64(stdin_payload, ref);
    memcpy(stdin_payload + 8, "go\n", 3);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_DATA, 10,
                       stdin_payload, 11) < 0 ||
        hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_CLOSE, 11,
                       stdin_payload, 8) < 0)
        goto out;
    done = 0;
    while (!done || cat_len < 2) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            if (cat_len + len - 8 > sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + cat_len, payload + 8, len - 8);
            cat_len += len - 8;
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }
    if (cat_len != 2 || memcmp(stdout_seen, "AB", 2) != 0)
        goto out;

    where = "exec one-byte binary stdin";
    memset(stdout_seen, 0, sizeof(stdout_seen));
    cat_len = 0;
    if (hc2_exec_start(fd, 12, "", argv6, 3) < 0)
        goto out;
    ref = 0;
    while (!ref) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STARTED && len >= 32) {
            ref = hc2_test_get_u64(payload);
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            goto out;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }
    hc2_test_put_u64(stdin_payload, ref);
    stdin_payload[8] = 0x03;
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_DATA, 13,
                       stdin_payload, 9) < 0)
        goto out;
    while (cat_len < 4) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            if (cat_len + len - 8 > sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + cat_len, payload + 8, len - 8);
            cat_len += len - 8;
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            goto out;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }
    if (cat_len != 4 || memcmp(stdout_seen, "CTRL", 4) != 0)
        goto out;
    hc2_test_put_u64(stdin_payload, ref);
    lmux_put_le32(stdin_payload + 8, LMUX_HC2_SIG_TERM);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_SIGNAL, 14,
                       stdin_payload, 12) < 0)
        goto out;
    done = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
            payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }

    where = "structured file put/get";
    if (make_file(file_src, 1024 * 1024) < 0 ||
        sha_file_hex(file_src, src_hex) < 0)
        goto out;
    memset(stdout_seen, 0, sizeof(stdout_seen));
    if (hc2_exec_start(fd, 40, "", argv4, 3) < 0 ||
        hc2_file_start(fd, LMUX_HC2_FILE_PUT_START, 41, file_src, file_target) < 0)
        goto out;
    done = 0;
    file_op = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_FILE_STARTED && len >= 24) {
            file_op = hc2_test_get_u64(payload);
            if (!file_op)
                goto out;
        } else if (type == LMUX_HC2_FILE_DONE && len >= 48) {
            char done_hex[65];
            if (hc2_test_get_u64(payload) != file_op ||
                hc2_test_get_u64(payload + 8) != 1024 * 1024)
                goto out;
            sha256_hex(payload + 16, done_hex);
            if (strcmp(done_hex, src_hex) != 0)
                goto out;
            done = 1;
        } else if (type == LMUX_HC2_FILE_ERROR) {
            goto out;
        } else if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t old = strlen(stdout_seen), n = len - 8;
            if (old + n >= sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + old, payload + 8, n);
        }
    }
    done = 0;
    while (strlen(stdout_seen) < 5 || !done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t old = strlen(stdout_seen), n = len - 8;
            if (old + n >= sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + old, payload + 8, n);
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_ERROR || type == LMUX_HC2_FILE_ERROR) {
            goto out;
        }
    }
    if (strcmp(stdout_seen, "XXXXX") != 0)
        goto out;

    if (hc2_file_start(fd, LMUX_HC2_FILE_GET_START, 42, file_target, file_got) < 0)
        goto out;
    done = 0;
    file_op = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_FILE_STARTED && len >= 24) {
            file_op = hc2_test_get_u64(payload);
            if (!file_op)
                goto out;
        } else if (type == LMUX_HC2_FILE_DONE && len >= 48) {
            char done_hex[65];
            if (hc2_test_get_u64(payload) != file_op ||
                hc2_test_get_u64(payload + 8) != 1024 * 1024)
                goto out;
            sha256_hex(payload + 16, done_hex);
            if (strcmp(done_hex, src_hex) != 0)
                goto out;
            done = 1;
        } else if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR) {
            goto out;
        }
    }
    if (sha_file_hex(file_got, got_hex) < 0 ||
        strcmp(src_hex, got_hex) != 0 ||
        files_equal(file_src, file_got) < 0)
        goto out;

    where = "structured file lifecycle cancel";
    if (make_file(file_src, 64u * 1024u * 1024u) < 0 ||
        hc2_file_start(fd, LMUX_HC2_FILE_PUT_START, 43,
                       file_src, file_target) < 0)
        goto out;
    file_op = 0;
    while (!file_op) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_FILE_STARTED && len >= 24)
            file_op = hc2_test_get_u64(payload);
        else if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR)
            goto out;
    }
    if (kill(worker, SIGSTOP) < 0)
        goto out;
    worker_stopped = 1;
    if (hc2_file_start(fd, LMUX_HC2_FILE_GET_START, 430,
                       file_target, file_got) < 0)
        goto out;
    for (;;) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_ERROR && req == 430 && len >= 8) {
            if (lmux_get_le32(payload) != LMUX_HC2_E_BUSY ||
                lmux_get_le32(payload + 4) == 0)
                goto out;
            break;
        }
        if (type == LMUX_HC2_FILE_ERROR)
            goto out;
    }
    hc2_test_put_u64(payload, file_op);
    if (hc2_send_frame(fd, LMUX_HC2_FILE_STATUS, 44, payload, 8) < 0)
        goto out;
    for (;;) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_FILE_STATE && req == 44 && len >= 96) {
            if (hc2_test_get_u64(payload) != file_op ||
                payload[53] != LMUX_HC2_FILE_RUNNING)
                goto out;
            break;
        }
        if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR)
            goto out;
    }
    hc2_test_put_u64(payload, file_op);
    if (hc2_send_frame(fd, LMUX_HC2_FILE_CANCEL, 45, payload, 8) < 0)
        goto out;
    for (;;) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_FILE_STATE && req == 45 && len >= 96) {
            if (hc2_test_get_u64(payload) != file_op ||
                payload[53] != LMUX_HC2_FILE_CANCELLED)
                goto out;
            break;
        }
        if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR)
            goto out;
    }
    if (kill(worker, SIGCONT) < 0)
        goto out;
    worker_stopped = 0;

    where = "structured file after cancel";
    if (make_file(file_src, 1024 * 1024) < 0 ||
        hc2_file_start(fd, LMUX_HC2_FILE_PUT_START, 46,
                       file_src, file_target) < 0)
        goto out;
    done = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_FILE_DONE && len >= 48)
            done = 1;
        else if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR)
            goto out;
    }

    where = "structured file disconnect start PUT";
    done = 0;
    if (make_file(file_src, 64u * 1024u * 1024u) < 0 ||
        hc2_file_start(fd, LMUX_HC2_FILE_PUT_START, 47,
                       file_src, file_target) < 0) {
        file_disconnect_errno = errno;
        goto out;
    }
    where = "structured file disconnect wait FILE_STARTED";
    file_disconnect_expected_req = 47;
    file_op = 0;
    while (!file_op) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0) {
            file_disconnect_errno = errno;
            goto out;
        }
        if (type == LMUX_HC2_FILE_STARTED && req == 47 && len >= 24)
            file_op = hc2_test_get_u64(payload);
        else if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR)
            goto out;
    }
    where = "structured file disconnect reconnect HC2";
    close(fd);
    fd = -1;
    fd = hc2_connect_hello(structured);
    if (fd < 0) {
        file_disconnect_errno = errno;
        goto out;
    }
    where = "structured file disconnect send FILE_STATUS";
    file_disconnect_expected_req = 48;
    hc2_test_put_u64(payload, file_op);
    if (hc2_send_frame(fd, LMUX_HC2_FILE_STATUS, 48, payload, 8) < 0) {
        file_disconnect_errno = errno;
        goto out;
    }
    where = "structured file disconnect wait FILE_STATE";
    for (;;) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0) {
            file_disconnect_errno = errno;
            goto out;
        }
        if (type == LMUX_HC2_FILE_STATE && req == 48) {
            where = "structured file disconnect verify FILE_STATE";
            if (len < 96)
                goto out;
            file_disconnect_observed_op = hc2_test_get_u64(payload);
            file_disconnect_state = payload[53];
            if (file_disconnect_observed_op != file_op ||
                (file_disconnect_state != LMUX_HC2_FILE_RUNNING &&
                 file_disconnect_state != LMUX_HC2_FILE_DONE_STATE))
                goto out;
            if (file_disconnect_state == LMUX_HC2_FILE_RUNNING) {
                where = "structured file disconnect send FILE_CANCEL";
                file_disconnect_expected_req = 49;
                hc2_test_put_u64(payload, file_op);
                if (hc2_send_frame(fd, LMUX_HC2_FILE_CANCEL, 49,
                                   payload, 8) < 0) {
                    file_disconnect_errno = errno;
                    goto out;
                }
            } else {
                done = 1;
            }
            break;
        }
        if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR)
            goto out;
    }
    if (!done) {
        where = "structured file disconnect wait CANCELLED";
        file_disconnect_expected_req = 49;
        file_disconnect_observed_op = 0;
        file_disconnect_state = -1;
        for (;;) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0) {
                file_disconnect_errno = errno;
                goto out;
            }
            if (type == LMUX_HC2_FILE_STATE && req == 49) {
                if (len < 96)
                    goto out;
                file_disconnect_observed_op = hc2_test_get_u64(payload);
                file_disconnect_state = payload[53];
                if (file_disconnect_observed_op != file_op ||
                    file_disconnect_state != LMUX_HC2_FILE_CANCELLED)
                    goto out;
                break;
            }
            if (type == LMUX_HC2_FILE_ERROR || type == LMUX_HC2_ERROR)
                goto out;
        }
    }

    {
        const char *request_namespace = "mcp-session-recovery";
        uint64_t discarded_op = 0, recovered_op = 0, duplicate_op = 0;
        uint8_t state = 0;
        int i;

        where = "idempotent start lost response running";
        if (make_file(file_src, 64u * 1024u * 1024u) < 0 ||
            kill(worker, SIGSTOP) < 0)
            goto out;
        worker_stopped = 1;
        if (hc2_file_start_idempotent(
                fd, LMUX_HC2_FILE_PUT_START_IDEMPOTENT, 500,
                request_namespace, "lost-running", file_src,
                file_target) < 0 ||
            hc2_wait_file_state(fd, 500, 10000, &state,
                                &discarded_op) < 0 ||
            state != LMUX_HC2_FILE_RUNNING || !discarded_op)
            goto out;
        close(fd);
        fd = -1;
        fd = hc2_connect_hello_retry(structured, 3000);
        if (fd < 0 ||
            hc2_file_status_token(fd, 501, request_namespace,
                                  "lost-running") < 0 ||
            hc2_wait_file_state(fd, 501, 10000, &state,
                                &recovered_op) < 0 ||
            state != LMUX_HC2_FILE_RUNNING ||
            recovered_op != discarded_op)
            goto out;

        where = "idempotent duplicate running";
        if (hc2_file_start_idempotent(
                fd, LMUX_HC2_FILE_PUT_START_IDEMPOTENT, 502,
                request_namespace, "lost-running", file_src,
                file_target) < 0 ||
            hc2_wait_file_state(fd, 502, 10000, &state,
                                &duplicate_op) < 0 ||
            state != LMUX_HC2_FILE_RUNNING ||
            duplicate_op != recovered_op)
            goto out;
        if (hc2_file_start_idempotent(
                fd, LMUX_HC2_FILE_PUT_START_IDEMPOTENT, 503,
                request_namespace, "lost-running", file_src,
                file_got) < 0 ||
            hc2_expect_error(fd, 503, LMUX_HC2_E_INVALID_ARGUMENT) < 0)
            goto out;

        where = "idempotent recovered cancel";
        hc2_test_put_u64(payload, recovered_op);
        if (hc2_send_frame(fd, LMUX_HC2_FILE_CANCEL, 504,
                           payload, 8) < 0 ||
            hc2_wait_file_state(fd, 504, 10000, &state, NULL) < 0 ||
            state != LMUX_HC2_FILE_CANCELLED ||
            kill(worker, SIGCONT) < 0)
            goto out;
        worker_stopped = 0;

        where = "idempotent start lost response done";
        if (make_file(file_src, 1024u * 1024u) < 0 ||
            hc2_file_start_idempotent(
                fd, LMUX_HC2_FILE_PUT_START_IDEMPOTENT, 510,
                request_namespace, "lost-done", file_src,
                file_target) < 0 ||
            hc2_wait_file_state(fd, 510, 10000, &state,
                                &discarded_op) < 0 ||
            !discarded_op)
            goto out;
        close(fd);
        fd = -1;
        fd = hc2_connect_hello_retry(structured, 3000);
        if (fd < 0)
            goto out;
        if (hc2_file_status_token(fd, 511, request_namespace,
                                  "lost-done") < 0 ||
            hc2_wait_file_state(fd, 511, 10000, &state,
                                &recovered_op) < 0)
            goto out;
        if (state == LMUX_HC2_FILE_RUNNING) {
            if (hc2_wait_file_done(fd, recovered_op, 15000) < 0 ||
                hc2_file_status_token(fd, 512, request_namespace,
                                      "lost-done") < 0 ||
                hc2_wait_file_state(fd, 512, 10000, &state,
                                    &recovered_op) < 0)
                goto out;
        }
        if (state != LMUX_HC2_FILE_DONE_STATE ||
            recovered_op != discarded_op)
            goto out;

        where = "idempotent duplicate terminal";
        if (hc2_file_start_idempotent(
                fd, LMUX_HC2_FILE_PUT_START_IDEMPOTENT, 600,
                request_namespace, "lost-done", file_src,
                file_target) < 0 ||
            hc2_wait_file_state(fd, 600, 10000, &state,
                                &duplicate_op) < 0 ||
            state != LMUX_HC2_FILE_DONE_STATE ||
            duplicate_op != recovered_op)
            goto out;

        where = "idempotent terminal eviction";
        for (i = 0; i < 8; i++) {
            char token[32];
            uint64_t start_req = 610 + (uint64_t)i * 32;
            uint64_t evict_op = 0;
            snprintf(token, sizeof(token), "evict-%d", i);
            if (hc2_file_start_idempotent(
                    fd, LMUX_HC2_FILE_PUT_START_IDEMPOTENT, start_req,
                    request_namespace, token, file_src, file_target) < 0 ||
                hc2_wait_file_state(fd, start_req, 10000, &state,
                                    &evict_op) < 0 ||
                !evict_op)
                goto out;
            if (state == LMUX_HC2_FILE_RUNNING &&
                hc2_wait_file_done(fd, evict_op, 15000) < 0)
                goto out;
            if (hc2_file_status_token(fd, start_req + 1,
                                      request_namespace, token) < 0 ||
                hc2_wait_file_state(fd, start_req + 1, 10000, &state,
                                    NULL) < 0)
                goto out;
            if (state != LMUX_HC2_FILE_DONE_STATE)
                goto out;
        }
        if (hc2_file_status_token(fd, 900, request_namespace,
                                  "lost-done") < 0 ||
            hc2_expect_error(fd, 900, LMUX_HC2_E_NOT_FOUND) < 0)
            goto out;
    }

    where = "disconnect reattach";
    if (hc2_exec_start(fd, 7, "", argv3, 3) < 0)
        goto out;
    ref = 0;
    while (!ref) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STARTED && len >= 32)
            ref = hc2_test_get_u64(payload);
    }
    where = "legacy status during structured exec";
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "__exec")) {
        dump_excerpt("structured legacy status during exec", out);
        goto out;
    }
    where = "disconnect reattach";
    close(fd);
    fd = -1;
    usleep(250000);
    fd = hc2_connect_hello(structured);
    if (fd < 0)
        goto out;
    hc2_test_put_u64(payload, ref);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_ATTACH, 8, payload, 8) < 0)
        goto out;
    memset(stdout_seen, 0, sizeof(stdout_seen));
    done = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t old = strlen(stdout_seen);
            if (old + len - 8 >= sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + old, payload + 8, len - 8);
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_ERROR && req == 8 && len >= 4 &&
                   (lmux_get_le32(payload) == LMUX_HC2_E_BUSY ||
                    lmux_get_le32(payload) == LMUX_HC2_E_NOT_CONNECTED)) {
            hc2_test_put_u64(payload, ref);
            if (hc2_send_frame(fd, LMUX_HC2_EXEC_ATTACH, 8, payload, 8) < 0)
                goto out;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }
    if (strcmp(stdout_seen, "LATE") != 0)
        goto out;

    rc = 0;
out:
    if (rc) {
        fprintf(stderr, "structured_control where=%s\n", where);
        fprintf(stderr, "last=%s(%u) req=%llu len=%u err=%u\n",
                hc2_type_name(type), type, (unsigned long long)req, len,
                (type == LMUX_HC2_ERROR && len >= 4) ? lmux_get_le32(payload) : 0);
        if (strncmp(where, "structured file disconnect", 26) == 0) {
            fprintf(stderr,
                    "file_disconnect errno=%d(%s) expected_req=%llu "
                    "last_req=%llu op=%llu observed_op=%llu state=%d "
                    "file_err=%u\n",
                    file_disconnect_errno,
                    file_disconnect_errno ? strerror(file_disconnect_errno) :
                        "none",
                    (unsigned long long)file_disconnect_expected_req,
                    (unsigned long long)req,
                    (unsigned long long)file_op,
                    (unsigned long long)file_disconnect_observed_op,
                    file_disconnect_state,
                    (type == LMUX_HC2_FILE_ERROR && len >= 12) ?
                        lmux_get_le32(payload + 8) : 0);
        }
    }
    if (fd >= 0)
        close(fd);
    if (worker_stopped)
        kill(worker, SIGCONT);
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier);
    unlink(control);
    unlink(structured);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int file_worker_replacement_torture_test(void)
{
    char dir[] = "/tmp/lmux-file-worker-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char pty_arg[320], file_arg[240], old_src[160], new_src[160];
    char target_path[160], got_path[160], out[8192];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1, put_pid = -1;
    int cfd = -1, rc = 1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp file worker");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(old_src, sizeof(old_src), "%s/old.bin", dir);
    snprintf(new_src, sizeof(new_src), "%s/new.bin", dir);
    snprintf(target_path, sizeof(target_path), "%s/target.bin", dir);
    snprintf(got_path, sizeof(got_path), "%s/got.bin", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0)
        goto out;
    where = "ready";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 1000);
    if (worker <= 0)
        goto out;
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf 'F''ILEWORKER\\n'", "FILEWORKER",
                         out, sizeof(out)) < 0)
        goto out;

    where = "initial file";
    if (make_file(old_src, 64 * 1024) < 0 ||
        make_file(new_src, 8 * 1024 * 1024) < 0 ||
        run_lmux_file("put", file_sock, old_src, target_path) < 0 ||
        files_equal(old_src, target_path) < 0)
        goto out;

    where = "kill worker mid put";
    put_pid = start_lmux_file("put", file_sock, new_src, target_path);
    if (put_pid <= 0 ||
        wait_lmux_temp_at_least(dir, 1, 5000) < 0)
        goto out;
    kill(worker, SIGKILL);
    worker2 = wait_child_pid(sup, worker, 15000);
    if (worker2 <= 0 || wait_pid_gone(worker, 3000) < 0)
        goto out;
    {
        long long end = now_ms() + 10000;
        int st = 0, done = 0;
        while (now_ms() < end) {
            if (waitpid(put_pid, &st, WNOHANG) == put_pid) {
                done = 1;
                break;
            }
            usleep(20000);
        }
        if (!done)
            goto out;
        put_pid = -1;
    }
    if (files_equal(old_src, target_path) < 0)
        goto out;

    where = "fresh file after worker replacement";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        run_lmux_file("put", file_sock, new_src, target_path) < 0 ||
        wait_lmux_temp_count(dir, 0, 5000) < 0 ||
        run_lmux_file("get", file_sock, target_path, got_path) < 0 ||
        files_equal(new_src, got_path) < 0)
        goto out;
    if (shell_cmd_expect(cfd, "printf 'F''ILEWORKER2\\n'", "FILEWORKER2",
                         out, sizeof(out)) < 0) {
        close(cfd);
        cfd = wait_connect(console, 10000);
        if (cfd < 0 ||
            shell_cmd_expect(cfd, "printf 'F''ILEWORKER2\\n'", "FILEWORKER2",
                             out, sizeof(out)) < 0)
            goto out;
    }

    rc = 0;
out:
    if (rc)
        fprintf(stderr, "file_worker_replacement where=%s worker=%ld worker2=%ld temp=%d\n",
                where, (long)worker, (long)worker2, count_lmux_temp_files(dir));
    if (put_pid > 0) {
        kill(put_pid, SIGKILL);
        (void)waitpid(put_pid, NULL, 0);
    }
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier); unlink(control); unlink(console); unlink(file_sock);
    unlink(old_src); unlink(new_src); unlink(target_path); unlink(got_path);
    unlink_dir_files(dir);
    rmdir(dir);
    return rc ? fail(where) : 0;
}

static int marker_bad(const char *buf, const char *own, const char *a,
                      const char *b)
{
    return strstr(buf, own) == NULL || strstr(buf, a) != NULL ||
        strstr(buf, b) != NULL;
}

static int structured_concurrent_exec_test(void)
{
    char dir[] = "/tmp/lmux-structured-concurrent-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out[32768];
    pid_t sup = -1, host = -1;
    int fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req, refs[4] = {0, 0, 0, 0};
    uint32_t len;
    const char *argv_a[] = {"/bin/sh", "-c", "i=0; while [ $i -lt 10 ]; do echo AAA:$i; i=$((i+1)); sleep 1; done"};
    const char *argv_b[] = {"/bin/sh", "-c", "i=0; while [ $i -lt 10 ]; do echo BBB:$i; i=$((i+1)); sleep 1; done"};
    const char *argv_c[] = {"/bin/sh", "-c", "i=0; while [ $i -lt 10 ]; do echo CCC:$i; i=$((i+1)); sleep 1; done"};
    const char *argv_d[] = {"/bin/echo", "ONESHOT"};
    const char *argv_e[] = {"/bin/echo", "EXTRA"};
    const char **argvs[4] = {argv_a, argv_b, argv_c, argv_d};
    char seen[4][512];
    int got_marker[4] = {0, 0, 0, 0};
    int done[4] = {0, 0, 0, 0};
    int over_capacity_seen = 0;
    long long end;
    const char *where = "setup";
    int i, j;

    memset(seen, 0, sizeof(seen));
    if (!mkdtemp(dir))
        return fail("mkdtemp structured concurrent");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0)
        goto out;
    where = "ready";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    fd = hc2_connect_hello_retry(structured, 3000);
    if (fd < 0)
        goto out;

    where = "start A/B/C/D";
    for (i = 0; i < 4; i++)
        if (hc2_exec_start_ready_ref(fd, 100 + (uint64_t)i, "",
                                     argvs[i], i == 3 ? 2 : 3,
                                     &refs[i]) < 0 || refs[i] == 0)
            goto out;
    for (i = 0; i < 4; i++)
        for (j = i + 1; j < 4; j++)
            if (refs[i] == refs[j])
                goto out;

    where = "over capacity";
    if (hc2_exec_start(fd, 200, "", argv_e, 2) < 0)
        goto out;
    end = now_ms() + 3000;
    while (now_ms() < end && !over_capacity_seen) {
        if (hc2_read_frame(fd, &type, &req, payload, &len,
                           (int)(end - now_ms())) < 0)
            goto out;
        if (type == LMUX_HC2_ERROR && req == 200 && len >= 4 &&
            (lmux_get_le32(payload) == LMUX_HC2_E_RESOURCE_LIMIT ||
             lmux_get_le32(payload) == LMUX_HC2_E_BUSY)) {
            over_capacity_seen = 1;
            break;
        }
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 4; i++) {
                if (ref == refs[i]) {
                    size_t old = strlen(seen[i]), n = len - 8;
                    if (old + n >= sizeof(seen[i]))
                        goto out;
                    memcpy(seen[i] + old, payload + 8, n);
                    break;
                }
            }
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 4; i++)
                if (ref == refs[i] && payload[16] == V2_EXEC_STATUS_DONE)
                    done[i] = 1;
        } else if (type == LMUX_HC2_ERROR && req == 200) {
            goto out;
        }
    }
    if (!over_capacity_seen)
        goto out;

    where = "legacy status while running";
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "__exec"))
        goto out;

    where = "collect concurrent output";
    end = now_ms() + 16000;
    while (now_ms() < end) {
        if (!got_marker[0] && strstr(seen[0], "AAA:1"))
            got_marker[0] = 1;
        if (!got_marker[1] && strstr(seen[1], "BBB:1"))
            got_marker[1] = 1;
        if (!got_marker[2] && strstr(seen[2], "CCC:1"))
            got_marker[2] = 1;
        if (!got_marker[3] && strcmp(seen[3], "ONESHOT\n") == 0)
            got_marker[3] = 1;
        if (got_marker[0] && got_marker[1] && got_marker[2] &&
            got_marker[3] && done[3] && !done[0] && !done[1] && !done[2])
            break;
        if (hc2_read_frame(fd, &type, &req, payload, &len,
                           (int)(end - now_ms())) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 4; i++) {
                if (ref == refs[i]) {
                    size_t old = strlen(seen[i]), n = len - 8;
                    if (old + n >= sizeof(seen[i]))
                        goto out;
                    memcpy(seen[i] + old, payload + 8, n);
                    break;
                }
            }
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 4; i++)
                if (ref == refs[i] && payload[16] == V2_EXEC_STATUS_DONE)
                    done[i] = 1;
        } else if (type == LMUX_HC2_ERROR) {
            goto out;
        }
    }
    if (!got_marker[0] || !got_marker[1] || !got_marker[2] ||
        !got_marker[3] || !done[3] || done[0] || done[1] || done[2])
        goto out;
    if (marker_bad(seen[0], "AAA:", "BBB:", "CCC:") ||
        marker_bad(seen[1], "BBB:", "AAA:", "CCC:") ||
        marker_bad(seen[2], "CCC:", "AAA:", "BBB:") ||
        strcmp(seen[3], "ONESHOT\n") != 0)
        goto out;

    rc = 0;
out:
    if (rc) {
        fprintf(stderr, "structured_concurrent where=%s\n", where);
        fprintf(stderr, "refs=%llu,%llu,%llu,%llu done=%d,%d,%d,%d\n",
                (unsigned long long)refs[0], (unsigned long long)refs[1],
                (unsigned long long)refs[2], (unsigned long long)refs[3],
                done[0], done[1], done[2], done[3]);
        fprintf(stderr, "A=%s\nB=%s\nC=%s\nD=%s\n",
                seen[0], seen[1], seen[2], seen[3]);
    }
    if (fd >= 0)
        close(fd);
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier);
    unlink(control);
    unlink(structured);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int structured_lmux_reconnect_test(void)
{
    char dir[] = "/tmp/lmux-structured-reconnect-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out[32768];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int fd = -1, console_fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD], stdin_payload[32];
    uint16_t type;
    uint64_t req, ref = 0;
    uint32_t len;
    const char *argv[] = {"/bin/sh", "-c", "printf PRE; cat; printf SUF"};
    const char *fault_cmd = "printf RECONNECT_FAULT\\n\n";
    char stdout_seen[64] = {0};
    int done = 0;
    int reattached = 0;
    unsigned reconnect_frames = 0, reconnect_state_frames = 0;
    uint16_t reconnect_last_type = 0;
    uint64_t reconnect_last_req = 0;
    uint32_t reconnect_last_len = 0;
    int reconnect_state = -1;
    int reconnect_fd_revents = 0;
    int reconnect_host_alive = 0;
    int reconnect_sup_alive = 0;
    int reconnect_proxy_alive = 0;
    int reconnect_wait_ready_rc = -1;
    long shell_pid = -1, shell_pid_after = -1;
    const char *where = "setup";
    struct saved_env outer_env = {0, NULL};

    if (!mkdtemp(dir))
        return fail("mkdtemp structured reconnect");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    where = "socketpair/pipe";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    where = "proxy";
    proxy = spawn_fault_proxy_fds(host_pair[0], target_pair[0],
                                  OUTER_FAULT_BAD_CRC,
                                  OUTER_FAULT_TARGET_TO_HOST,
                                  arm[0], note[1]);
    close(host_pair[0]);
    host_pair[0] = -1;
    close(target_pair[0]);
    target_pair[0] = -1;
    close(arm[0]);
    arm[0] = -1;
    close(note[1]);
    note[1] = -1;
    if (proxy < 0)
        goto out;
    where = "supervisor";
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]);
    target_pair[1] = -1;
    if (sup < 0)
        goto out;
    where = "host";
    if (save_env_var(&outer_env, "LMUX_SUP_IT_OUTER") < 0)
        goto out;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    host = spawn_host_fd_structured(host_pair[1], control, structured,
                                    pty_arg, file_arg, 1);
    restore_env_var("LMUX_SUP_IT_OUTER", &outer_env);
    close(host_pair[1]);
    host_pair[1] = -1;
    if (host < 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0) {
        dump_excerpt("structured reconnect initial status", out);
        goto out;
    }
    where = "initial exec service";
    if (wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0) {
        dump_excerpt("structured reconnect initial exec", out);
        goto out;
    }
    where = "structured hello";
    fd = hc2_connect_hello(structured);
    where = "structured start";
    if (fd < 0 ||
        hc2_exec_start_ready_ref(fd, 101, "", argv, 3, &ref) < 0 ||
        !ref)
        goto out;
    where = "prefix";
    while (!strstr(stdout_seen, "PRE")) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t old = strlen(stdout_seen), n = len - 8;
            if (old + n >= sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + old, payload + 8, n);
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }

    where = "carrier reconnect";
    console_fd = wait_connect(console, 5000);
    if (console_fd < 0)
        goto out;
    usleep(1000000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' R PID \"$$\"",
                         "RPID:", out, sizeof(out)) < 0)
        goto out;
    shell_pid = parse_pid_marker(out, "RPID:");
    if (shell_pid <= 0)
        goto out;
    if (write_all(arm[1], "A", 1) < 0)
        goto out;
    where = "carrier reconnect trigger";
    if (write_all(console_fd, fault_cmd, strlen(fault_cmd)) < 0 ||
        wait_fault_injected(note[0], 5000) < 0)
        goto out;
    close(console_fd);
    console_fd = -1;
    where = "carrier reconnect wait";
    {
        long long end = now_ms() + 15000;
        while (now_ms() < end && !reattached) {
            int rem = (int)(end - now_ms());
            int rr = hc2_read_frame(fd, &type, &req, payload, &len, rem);
            if (rr < 0)
                break;
            note_hc2_frame(type, req, len, &reconnect_last_type,
                           &reconnect_last_req, &reconnect_last_len,
                           &reconnect_frames);
            if (type == LMUX_HC2_EXEC_STATE && req == 0 && len >= 32) {
                reconnect_state_frames++;
                reconnect_state = payload[16];
                reattached = 1;
            } else if (type == LMUX_HC2_EXEC_STATE) {
                reconnect_state_frames++;
            } else if (type == LMUX_HC2_EXEC_LOST)
                goto out;
            else if (type == LMUX_HC2_ERROR && len >= 4 &&
                     lmux_get_le32(payload) != LMUX_HC2_OK)
                goto out;
        }
    }
    if (!reattached) {
        int sr = read_status(control, out, sizeof(out));
        reconnect_fd_revents =
            wait_fd_event(fd, POLLIN | POLLOUT | POLLHUP | POLLERR | POLLNVAL, 0);
        reconnect_host_alive = pid_alive(host);
        reconnect_sup_alive = pid_alive(sup);
        reconnect_proxy_alive = pid_alive(proxy);
        reconnect_wait_ready_rc =
            wait_ready_layers(control, 1, 5000, out, sizeof(out));
        fprintf(stderr, "structured reconnect no-reattach status_rc=%d\n", sr);
        if (reconnect_wait_ready_rc == 0)
            dump_excerpt("structured reconnect timeout ready status", out);
        else if (sr == 0)
            dump_excerpt("structured reconnect timeout status", out);
        goto out;
    }
    where = "post reconnect pty";
    console_fd = wait_connect(console, 5000);
    if (console_fd < 0)
        goto out;
    usleep(1000000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' R PID2 \"$$\"",
                         "RPID2:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_after = parse_pid_marker(out, "RPID2:");
    if (shell_pid_after != shell_pid)
        goto out;
    close(console_fd);
    console_fd = -1;

    where = "auto reattach stdin";
    hc2_test_put_u64(stdin_payload, ref);
    memcpy(stdin_payload + 8, "MID", 3);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_DATA, 102,
                       stdin_payload, 11) < 0 ||
        hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_CLOSE, 103,
                       stdin_payload, 8) < 0)
        goto out;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t old = strlen(stdout_seen), n = len - 8;
            if (old + n >= sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + old, payload + 8, n);
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }
    if (strcmp(stdout_seen, "PREMIDSUF") != 0)
        goto out;
    where = "post reconnect release";
    hc2_test_put_u64(payload, ref);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_RELEASE, 105, payload, 8) < 0)
        goto out;
    done = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
            goto out;
        reconnect_last_type = type;
        reconnect_last_req = req;
        reconnect_last_len = len;
        if (type == LMUX_HC2_EXEC_STATE && len >= 32)
            reconnect_state = payload[16];
        if (type == LMUX_HC2_EXEC_STATE && req == 105)
            done = 1;
        else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                 hc2_test_get_u64(payload) == ref)
            done = 1;
        else if (type == LMUX_HC2_ERROR)
            goto out;
    }
    where = "post reconnect fresh exec";
    close(fd);
    fd = hc2_connect_hello_retry(structured, 5000);
    if (fd < 0)
        goto out;
    if (hc2_run_echo(fd, 106, "FRESH_AFTER_RECONNECT") < 0)
        goto out;
    rc = 0;
out:
    if (rc) {
        fprintf(stderr,
                "structured_lmux_reconnect where=%s stdout=%s frames=%u states=%u last=%s(%u) req=%llu len=%u state=%d reattached=%d\n",
                where, stdout_seen, reconnect_frames,
                reconnect_state_frames,
                hc2_type_name(reconnect_last_type), reconnect_last_type,
                (unsigned long long)reconnect_last_req, reconnect_last_len,
                reconnect_state, reattached);
        if (strcmp(where, "carrier reconnect wait") == 0)
            fprintf(stderr,
                    "structured_lmux_reconnect fd_revents=0x%x host_alive=%d sup_alive=%d proxy_alive=%d wait_ready_rc=%d\n",
                    reconnect_fd_revents, reconnect_host_alive,
                    reconnect_sup_alive, reconnect_proxy_alive,
                    reconnect_wait_ready_rc);
    }
    if (fd >= 0)
        close(fd);
    if (console_fd >= 0)
        close(console_fd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(carrier);
    unlink(control);
    unlink(structured);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int structured_worker_replacement_test(void)
{
    char dir[] = "/tmp/lmux-structured-worker-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], file_src[160], file_target[160], out[32768];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    int fd = -1, console_fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req, ref = 0, file_op = 0;
    uint64_t fresh_ref = 0;
    uint64_t second_ref = 0;
    uint64_t token_file_op = 0;
    uint8_t token_file_state = 0;
    uint32_t len;
    long shell_pid = -1, shell_pid_after = -1;
    const char *argv[] = {"/bin/sh", "-c", "printf PRE; sleep 30"};
    const char *argv_true[] = {"/bin/true"};
    char stdout_seen[32] = {0};
    char fresh_stdout_seen[32] = {0};
    int lost_seen = 0;
    int done = 0;
    uint16_t last_type = 0;
    uint64_t last_req = 0;
    uint32_t last_len = 0;
    uint32_t last_error = 0;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp structured worker");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(file_src, sizeof(file_src), "%s/file-src.bin", dir);
    snprintf(file_target, sizeof(file_target), "%s/file-target.bin", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    where = "initial pty";
    console_fd = wait_connect(console, 10000);
    if (console_fd < 0)
        goto out;
    usleep(1000000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' W PID \"$$\"",
                         "WPID:", out, sizeof(out)) < 0)
        goto out;
    shell_pid = parse_pid_marker(out, "WPID:");
    if (shell_pid <= 0)
        goto out;
    close(console_fd);
    console_fd = -1;
    fd = hc2_connect_hello(structured);
    if (fd < 0 ||
        hc2_exec_start_ready_ref(fd, 201, "", argv, 3, &ref) < 0 ||
        !ref)
        goto out;
    where = "prefix";
    while (!strstr(stdout_seen, "PRE")) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t old = strlen(stdout_seen), n = len - 8;
            if (old + n >= sizeof(stdout_seen))
                goto out;
            memcpy(stdout_seen + old, payload + 8, n);
        } else if (type == LMUX_HC2_ERROR) {
            goto out;
        }
    }

    where = "start file before worker replacement";
    if (make_file(file_src, 64u * 1024u * 1024u) < 0 ||
        hc2_file_start_idempotent(
            fd, LMUX_HC2_FILE_PUT_START_IDEMPOTENT, 200,
            "worker-replacement", "lost-transfer",
            file_src, file_target) < 0 ||
        hc2_wait_file_state(fd, 200, 7000, NULL, &file_op) < 0 ||
        !file_op)
        goto out;

    where = "replace worker";
    kill(worker, SIGKILL);
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0 || worker2 == worker) {
        fprintf(stderr, "structured worker replacement worker2=%ld old=%ld\n",
                (long)worker2, (long)worker);
        goto out;
    }
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0) {
        dump_excerpt("structured worker replacement ready", out);
        goto out;
    }
    if (wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0) {
        dump_excerpt("structured worker replacement exec", out);
        goto out;
    }
    where = "post replacement pty";
    if (wait_pid_gone((pid_t)shell_pid, 3000) < 0)
        goto out;
    console_fd = wait_connect(console, 10000);
    if (console_fd < 0)
        goto out;
    usleep(1000000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' W PID2 \"$$\"",
                         "WPID2:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_after = parse_pid_marker(out, "WPID2:");
    if (shell_pid_after <= 0 || shell_pid_after == shell_pid)
        goto out;
    close(console_fd);
    console_fd = -1;
    where = "exec lost";
    while (!lost_seen) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_LOST) {
            if (lost_seen || len < 12 || hc2_test_get_u64(payload) != ref)
                goto out;
            lost_seen = 1;
        } else if (type == LMUX_HC2_EXEC_STDOUT) {
            goto out;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) != LMUX_HC2_OK) {
            goto out;
        }
    }

    where = "file lost after worker replacement";
    hc2_test_put_u64(payload, file_op);
    if (hc2_send_frame(fd, LMUX_HC2_FILE_STATUS, 206, payload, 8) < 0)
        goto out;
    for (;;) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_FILE_STATE && req == 206 && len >= 96) {
            if (hc2_test_get_u64(payload) != file_op ||
                payload[53] != LMUX_HC2_FILE_LOST ||
                lmux_get_le32(payload + 48) != LMUX_HC2_E_TARGET_LOST)
                goto out;
            break;
        }
        if (type == LMUX_HC2_ERROR)
            goto out;
    }
    if (hc2_file_status_token(fd, 209, "worker-replacement",
                              "lost-transfer") < 0 ||
        hc2_wait_file_state(fd, 209, 10000, &token_file_state,
                            &token_file_op) < 0 ||
        token_file_op != file_op || token_file_state != LMUX_HC2_FILE_LOST)
        goto out;

    where = "stale ref errors";
    hc2_test_put_u64(payload, ref);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_STATUS, 202, payload, 8) < 0 ||
        hc2_expect_error(fd, 202, LMUX_HC2_E_NOT_FOUND) < 0)
        goto out;
    lmux_put_le32(payload + 8, LMUX_HC2_SIG_TERM);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_SIGNAL, 203, payload, 12) < 0 ||
        hc2_expect_error(fd, 203, LMUX_HC2_E_NOT_FOUND) < 0)
        goto out;
    memcpy(payload + 8, "X", 1);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_DATA, 204, payload, 9) < 0 ||
        hc2_expect_error(fd, 204, LMUX_HC2_E_NOT_FOUND) < 0)
        goto out;
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_ATTACH, 205, payload, 8) < 0 ||
        hc2_expect_error(fd, 205, LMUX_HC2_E_NOT_FOUND) < 0)
        goto out;

    where = "fresh exec without old release";
    if (hc2_exec_start_ready_ref(fd, 207, "", argv_true, 1, &fresh_ref) < 0 ||
        !fresh_ref || fresh_ref == ref)
        goto out;
    done = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        last_type = type;
        last_req = req;
        last_len = len;
        if (type == LMUX_HC2_ERROR && len >= 4)
            last_error = lmux_get_le32(payload);
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            size_t old = strlen(fresh_stdout_seen), n = len - 8;
            if (old + n >= sizeof(fresh_stdout_seen))
                goto out;
            memcpy(fresh_stdout_seen + old, payload + 8, n);
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_EXEC_LOST ||
                   (type == LMUX_HC2_ERROR && len >= 4 &&
                    lmux_get_le32(payload) != LMUX_HC2_OK)) {
            goto out;
        }
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_WORKER_FRESH")) {
        rc = 0;
        goto out;
    }

    where = "release old lost ref";
    hc2_test_put_u64(payload, ref);
    if (hc2_send_frame(fd, LMUX_HC2_EXEC_RELEASE, 208, payload, 8) < 0 ||
        hc2_expect_error(fd, 208, LMUX_HC2_E_NOT_FOUND) < 0)
        goto out;

    where = "fresh exec after old release";
    if (hc2_exec_start_ready_ref(fd, 209, "", argv_true, 1, &second_ref) < 0 ||
        !second_ref || second_ref == ref || second_ref == fresh_ref)
        goto out;
    done = 0;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        last_type = type;
        last_req = req;
        last_len = len;
        if (type == LMUX_HC2_ERROR && len >= 4)
            last_error = lmux_get_le32(payload);
        if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            done = 1;
        } else if (type == LMUX_HC2_EXEC_LOST ||
                   (type == LMUX_HC2_ERROR && len >= 4 &&
                    lmux_get_le32(payload) != LMUX_HC2_OK)) {
            goto out;
        }
    }
    rc = 0;
out:
    if (rc)
        fprintf(stderr,
                "structured_worker_replacement where=%s old_stdout=%s fresh_stdout=%s old_ref=%llu fresh_ref=%llu second_ref=%llu last=%s(%u) req=%llu len=%u err=%u\n",
                where, stdout_seen, fresh_stdout_seen,
                (unsigned long long)ref, (unsigned long long)fresh_ref,
                (unsigned long long)second_ref,
                hc2_type_name(last_type), last_type,
                (unsigned long long)last_req, last_len, last_error);
    if (fd >= 0)
        close(fd);
    if (console_fd >= 0)
        close(console_fd);
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier);
    unlink(control);
    unlink(structured);
    unlink(console);
    unlink(file_sock);
    unlink(file_src);
    unlink(file_target);
    rmdir(dir);
    return rc;
}

static int structured_multi_reconnect_test(void)
{
    char dir[] = "/tmp/lmux-structured-mreconnect-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out[32768];
    int host_pair[2] = {-1, -1}, target_pair[2] = {-1, -1};
    int arm[2] = {-1, -1}, note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int fd = -1, console_fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD], stdin_payload[64];
    uint16_t type;
    uint64_t req, refs[3] = {0, 0, 0};
    uint32_t len;
    const char *argv_a[] = {"/bin/sh", "-c", "printf AAA_PRE; cat; printf AAA_SUF"};
    const char *argv_b[] = {"/bin/sh", "-c", "printf BBB_PRE; cat; printf BBB_SUF"};
    const char *argv_c[] = {"/bin/sh", "-c", "printf CCC_PRE; cat; printf CCC_SUF"};
    const char **argvs[3] = {argv_a, argv_b, argv_c};
    const char *prefix[3] = {"AAA_PRE", "BBB_PRE", "CCC_PRE"};
    const char *mid[3] = {"AAA_MID", "BBB_MID", "CCC_MID"};
    const char *want[3] = {
        "AAA_PREAAA_MIDAAA_SUF",
        "BBB_PREBBB_MIDBBB_SUF",
        "CCC_PRECCC_MIDCCC_SUF"
    };
    char seen[3][128];
    int pref[3] = {0, 0, 0}, reattached[3] = {0, 0, 0}, done[3] = {0, 0, 0};
    int i;
    const char *where = "setup";
    uint16_t last_type = 0;
    uint64_t last_req = 0;
    uint32_t last_len = 0, last_err0 = 0, last_err8 = 0;
    int last_read_rc = 0;
    struct saved_env outer_env = {0, NULL};

    memset(seen, 0, sizeof(seen));
    if (!mkdtemp(dir))
        return fail("mkdtemp structured multi reconnect");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy_fds(host_pair[0], target_pair[0],
                                  OUTER_FAULT_BAD_CRC,
                                  OUTER_FAULT_TARGET_TO_HOST,
                                  arm[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0 || save_env_var(&outer_env, "LMUX_SUP_IT_OUTER") < 0)
        goto out;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    host = spawn_host_fd_structured(host_pair[1], control, structured,
                                    pty_arg, file_arg, 1);
    restore_env_var("LMUX_SUP_IT_OUTER", &outer_env);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0 ||
        wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    fd = hc2_connect_hello(structured);
    if (fd < 0)
        goto out;
    where = "start";
    for (i = 0; i < 3; i++) {
        if (hc2_exec_start_ready_ref(fd, 300 + (uint64_t)i, "",
                                     argvs[i], 3, &refs[i]) < 0 || !refs[i])
            goto out;
        where = "prefix";
        while (!pref[i]) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
                goto out;
            if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
                uint64_t ref = hc2_test_get_u64(payload);
                int j;
                for (j = 0; j < 3; j++) {
                    if (ref == refs[j]) {
                        size_t old = strlen(seen[j]), n = len - 8;
                        if (old + n >= sizeof(seen[j]))
                            goto out;
                        memcpy(seen[j] + old, payload + 8, n);
                        if (strstr(seen[j], prefix[j]))
                            pref[j] = 1;
                    }
                }
            } else if (type == LMUX_HC2_ERROR) {
                goto out;
            }
        }
        where = "start";
    }
    where = "fault";
    console_fd = wait_connect(console, 5000);
    {
        const char *fault_cmd = "printf RECONNECT_FAULT\\n\n";
        if (console_fd < 0 || write_all(arm[1], "A", 1) < 0 ||
            write_all(console_fd, fault_cmd, strlen(fault_cmd)) < 0 ||
        wait_fault_injected(note[0], 5000) < 0)
            goto out;
    }
    close(console_fd); console_fd = -1;
    where = "reattach";
    while (!reattached[0] || !reattached[1] || !reattached[2]) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 15000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STATE && req == 0 && len >= 32) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 3; i++)
                if (ref == refs[i])
                    reattached[i] = 1;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) == LMUX_HC2_OK) {
            continue;
        } else if (type == LMUX_HC2_EXEC_LOST || type == LMUX_HC2_ERROR) {
            goto out;
        }
    }
    where = "stdin";
    for (i = 0; i < 3; i++) {
        size_t n = strlen(mid[i]);
        hc2_test_put_u64(stdin_payload, refs[i]);
        memcpy(stdin_payload + 8, mid[i], n);
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_DATA, 400 + (uint64_t)i,
                           stdin_payload, (uint32_t)(8 + n)) < 0 ||
            hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_CLOSE, 410 + (uint64_t)i,
                           stdin_payload, 8) < 0)
            goto out;
    }
    where = "finish";
    while (!done[0] || !done[1] || !done[2]) {
        last_read_rc = hc2_read_frame(fd, &type, &req, payload, &len, 15000);
        if (last_read_rc < 0)
            goto out;
        last_type = type;
        last_req = req;
        last_len = len;
        last_err0 = len >= 4 ? lmux_get_le32(payload) : 0;
        last_err8 = len >= 12 ? lmux_get_le32(payload + 8) : 0;
        if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 3; i++) {
                if (ref == refs[i]) {
                    size_t old = strlen(seen[i]), n = len - 8;
                    if (old + n >= sizeof(seen[i]))
                        goto out;
                    memcpy(seen[i] + old, payload + 8, n);
                }
            }
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                   payload[16] == V2_EXEC_STATUS_DONE) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 3; i++)
                if (ref == refs[i])
                    done[i] = 1;
        } else if (type == LMUX_HC2_ERROR && len >= 4 &&
                   lmux_get_le32(payload) == LMUX_HC2_OK) {
            continue;
        } else if (type == LMUX_HC2_EXEC_LOST || type == LMUX_HC2_ERROR) {
            goto out;
        }
    }
    for (i = 0; i < 3; i++)
        if (strcmp(seen[i], want[i]) != 0)
            goto out;
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "structured_multi_reconnect where=%s A=%s B=%s C=%s last=%u req=%llu len=%u err0=%u err8=%u read_rc=%d\n",
                where, seen[0], seen[1], seen[2], last_type,
                (unsigned long long)last_req, last_len, last_err0, last_err8,
                last_read_rc);
    if (fd >= 0) close(fd);
    if (console_fd >= 0) close(console_fd);
    stop_pid(host); stop_pid(sup); stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static int structured_multi_worker_replacement_test(void)
{
    char dir[] = "/tmp/lmux-structured-mworker-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out[32768];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    int fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type = 0;
    uint64_t req = 0, refs[3] = {0, 0, 0}, fresh = 0;
    uint32_t len = 0;
    const char *argv_a[] = {"/bin/sh", "-c", "printf AAA; sleep 120"};
    const char *argv_b[] = {"/bin/sh", "-c", "printf BBB; sleep 120"};
    const char *argv_c[] = {"/bin/sh", "-c", "printf CCC; sleep 120"};
    const char *argv_true[] = {"/bin/true"};
    const char **argvs[3] = {argv_a, argv_b, argv_c};
    char seen[3][32];
    int pref[3] = {0, 0, 0}, lost[3] = {0, 0, 0}, done = 0;
    int i;
    const char *where = "setup";
    memset(seen, 0, sizeof(seen));
    if (!mkdtemp(dir))
        return fail("mkdtemp structured multi worker");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (worker <= 0 || host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    fd = hc2_connect_hello(structured);
    if (fd < 0)
        goto out;
    where = "start";
    for (i = 0; i < 3; i++) {
        if (hc2_exec_start_ready_ref(fd, 500 + (uint64_t)i, "",
                                     argvs[i], 3, &refs[i]) < 0 || !refs[i])
            goto out;
        where = "prefix";
        while (!pref[i]) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
                goto out;
            if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
                uint64_t ref = hc2_test_get_u64(payload);
                int j;
                for (j = 0; j < 3; j++) {
                    if (ref == refs[j]) {
                        size_t old = strlen(seen[j]), n = len - 8;
                        if (old + n >= sizeof(seen[j]))
                            goto out;
                        memcpy(seen[j] + old, payload + 8, n);
                        if ((j == 0 && strstr(seen[j], "AAA")) ||
                            (j == 1 && strstr(seen[j], "BBB")) ||
                            (j == 2 && strstr(seen[j], "CCC")))
                            pref[j] = 1;
                    }
                }
            } else if (type == LMUX_HC2_ERROR) {
                goto out;
            }
        }
        where = "start";
    }
    where = "replace";
    kill(worker, SIGKILL);
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0 || worker2 == worker ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    where = "lost";
    while (!lost[0] || !lost[1] || !lost[2]) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 10000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_LOST && len >= 12) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 3; i++) {
                if (ref == refs[i]) {
                    if (lost[i])
                        goto out;
                    lost[i] = 1;
                }
            }
        } else if (type == LMUX_HC2_EXEC_STDOUT || type == LMUX_HC2_ERROR) {
            goto out;
        }
    }
    where = "fresh";
    if (hc2_exec_start_ready_ref(fd, 600, "", argv_true, 1, &fresh) < 0 ||
        !fresh)
        goto out;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
            payload[16] == V2_EXEC_STATUS_DONE)
            done = 1;
        else if (type == LMUX_HC2_EXEC_LOST || type == LMUX_HC2_ERROR)
            goto out;
    }
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "structured_multi_worker where=%s A=%s B=%s C=%s\n",
                where, seen[0], seen[1], seen[2]);
    if (fd >= 0) close(fd);
    stop_pid(host); stop_pid(sup);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static int lifecycle_recovery_loop_test(void)
{
    char dir[] = "/tmp/lmux-lifecycle-loop-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out[32768];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    int fd = -1, console_fd = -1, rc = 1;
    int sup_fd_base = -1, sup_fd_end = -1;
    long shell_pid = -1, shell_pid_after = -1;
    int i = -1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp lifecycle loop");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    sup_fd_base = count_fds(sup);
    if (sup_fd_base < 0)
        goto out;
    fd = hc2_connect_hello_retry(structured, 5000);
    if (fd < 0)
        goto out;
    console_fd = wait_connect(console, 10000);
    if (console_fd < 0)
        goto out;
    usleep(500000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' L PID \"$$\"",
                         "LPID:", out, sizeof(out)) < 0)
        goto out;
    shell_pid = parse_pid_marker(out, "LPID:");
    if (shell_pid <= 0)
        goto out;
    close(console_fd);
    console_fd = -1;

    for (i = 0; i < 20; i++) {
        char marker[64];
        snprintf(marker, sizeof(marker), "LOOP_%02d", i);
        where = "loop echo";
        if (hc2_run_echo(fd, 3000 + (uint64_t)i, marker) < 0)
            goto out;
        where = "loop pty";
        console_fd = wait_connect(console, 10000);
        if (console_fd < 0)
            goto out;
        usleep(200000);
        if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' L PIDX \"$$\"",
                             "LPIDX:", out, sizeof(out)) < 0)
            goto out;
        shell_pid_after = parse_pid_marker(out, "LPIDX:");
        if (shell_pid_after != shell_pid)
            goto out;
        close(console_fd);
        console_fd = -1;

        if ((i % 2) == 0) {
            where = "loop carrier loss";
            close(fd);
            fd = -1;
            stop_pid(host);
            host = -1;
            sleep(1);
            if (wait_child_pid(sup, -1, 1000) != worker)
                goto out;
            host = spawn_host_structured(carrier, control, structured,
                                         pty_arg, file_arg);
            if (host <= 0)
                goto out;
        } else {
            where = "loop worker replacement";
            close(fd);
            fd = -1;
            kill(worker, SIGKILL);
            worker2 = wait_child_pid(sup, worker, 10000);
            if (worker2 <= 0 || worker2 == worker)
                goto out;
            if (wait_pid_gone(worker, 3000) < 0 ||
                wait_pid_gone((pid_t)shell_pid, 3000) < 0)
                goto out;
            worker = worker2;
            worker2 = -1;
        }
        where = "loop reconcile";
        if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                              15000, out, sizeof(out)) < 0 ||
            wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
            goto out;
        fd = hc2_connect_hello_retry(structured, 5000);
        if (fd < 0)
            goto out;
        console_fd = wait_connect(console, 10000);
        if (console_fd < 0)
            goto out;
        usleep(500000);
        if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' L PIDY \"$$\"",
                             "LPIDY:", out, sizeof(out)) < 0)
            goto out;
        shell_pid_after = parse_pid_marker(out, "LPIDY:");
        if ((i % 2) == 0) {
            if (shell_pid_after != shell_pid)
                goto out;
        } else {
            if (shell_pid_after <= 0 || shell_pid_after == shell_pid)
                goto out;
            shell_pid = shell_pid_after;
        }
        close(console_fd);
        console_fd = -1;
        snprintf(marker, sizeof(marker), "POST_%02d", i);
        if (hc2_run_echo(fd, 4000 + (uint64_t)i, marker) < 0)
            goto out;
        if (wait_child_pid(sup, worker, 1000) != -1)
            goto out;
    }
    sup_fd_end = wait_fd_count(sup, sup_fd_base, 5000);
    if (sup_fd_end != sup_fd_base) {
        fprintf(stderr, "lifecycle loop supervisor fds base=%d end=%d\n",
                sup_fd_base, sup_fd_end);
        goto out;
    }
    rc = 0;
out:
    if (rc) {
        fprintf(stderr, "lifecycle_recovery_loop where=%s cycle=%d shell=%ld after=%ld worker=%ld\n",
                where, i, shell_pid, shell_pid_after, (long)worker);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("lifecycle recovery final status", out);
    }
    if (fd >= 0)
        close(fd);
    if (console_fd >= 0)
        close(console_fd);
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static int mixed_service_soak_test(void)
{
    char dir[] = "/tmp/lmux-mixed-soak-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char unix_host[160], unix_target[160], pty_arg[320], file_arg[240], unix_arg[400];
    char src[160], target_path[160], got_path[160], out[32768];
    char producer_cmd[512];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    int fd = -1, console_fd = -1, uh = -1, ut = -1, rc = 1;
    int host_fd_base = -1, host_fd_end = -1;
    long shell_pid = -1, shell_pid_after = -1;
    unsigned long long crc0 = 0, seq0 = 0, proto0 = 0, stream0 = 0, stale0 = 0, drops0 = 0;
    unsigned long long crc1 = 0, seq1 = 0, proto1 = 0, stream1 = 0, stale1 = 0, drops1 = 0;
    int i = -1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp mixed soak");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(unix_host, sizeof(unix_host), "%s/rdv-host.sock", dir);
    snprintf(unix_target, sizeof(unix_target), "%s/rdv-target.sock", dir);
    snprintf(src, sizeof(src), "%s/src.bin", dir);
    snprintf(target_path, sizeof(target_path), "%s/target.bin", dir);
    snprintf(got_path, sizeof(got_path), "%s/got.bin", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    snprintf(unix_arg, sizeof(unix_arg), "rdv:%s:%s", unix_host, unix_target);

    if (make_file(src, 4096) < 0)
        goto out;
    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    host = spawn_host_structured_unix(carrier, control, structured,
                                      pty_arg, file_arg, unix_arg);
    if (host <= 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0 ||
        wait_path_exists(unix_host, 5000) < 0 ||
        wait_path_exists(unix_target, 5000) < 0) {
        dump_excerpt("mixed initial status", out);
        goto out;
    }
    host_fd_base = count_fds(host);
    if (read_status(control, out, sizeof(out)) < 0)
        goto out;
    crc0 = status_value(out, "crc_errors=");
    seq0 = status_value(out, "seq_errors=");
    proto0 = status_value(out, "protocol_errors=");
    stream0 = status_value(out, "stream_errors=");
    stale0 = status_value(out, "stale=");
    drops0 = status_value(out, "pre_established_drops=");

    fd = hc2_connect_hello_retry(structured, 5000);
    console_fd = wait_connect(console, 10000);
    if (fd < 0 || console_fd < 0)
        goto out;
    usleep(500000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' M PID \"$$\"",
                         "MPID:", out, sizeof(out)) < 0)
        goto out;
    shell_pid = parse_pid_marker(out, "MPID:");
    if (shell_pid <= 0)
        goto out;
    where = "start detached PTY producer";
    snprintf(producer_cmd, sizeof(producer_cmd),
             "while [ ! -f %s ]; do printf '\\132'; done\n", target_path);
    if (write_all(console_fd, producer_cmd, strlen(producer_cmd)) < 0 ||
        read_until(console_fd, "ZZZZ", out, sizeof(out), 5000) < 0)
        goto out;
    close(console_fd);
    console_fd = -1;

    for (i = 0; i < 12; i++) {
        char marker[64];
        snprintf(marker, sizeof(marker), "MIX_PRE_%02d", i);
        where = "cycle pre operations";
        if (hc2_run_echo(fd, 10000 + (uint64_t)i, marker) < 0 ||
            supervisor_unix_exchange(unix_host, unix_target, i & 1, (uint8_t)i) < 0 ||
            run_lmux_file("put", file_sock, src, target_path) < 0 ||
            run_lmux_file("get", file_sock, target_path, got_path) < 0 ||
            files_equal(src, got_path) < 0)
            goto out;

        where = "cycle live rendezvous";
        uh = connect_unix_test(unix_host);
        ut = connect_unix_test(unix_target);
        if (uh < 0 || ut < 0 ||
            write_all(uh, "LIVE", 4) < 0 ||
            read_exact_timeout(ut, (uint8_t *)out, 4, 5000) < 0 ||
            memcmp(out, "LIVE", 4) != 0)
            goto out;

        close(fd);
        fd = -1;
        if ((i % 3) == 0) {
            where = "cycle carrier reconnect";
            stop_pid(host);
            host = -1;
            if (wait_child_pid(sup, -1, 1000) != worker)
                goto out;
            host = spawn_host_structured_unix(carrier, control, structured,
                                              pty_arg, file_arg, unix_arg);
            if (host <= 0)
                goto out;
            host_fd_base = -1;
        } else if ((i % 3) == 1) {
            where = "cycle worker replacement";
            kill(worker, SIGKILL);
            worker2 = wait_child_pid(sup, worker, 15000);
            if (worker2 <= 0 || worker2 == worker ||
                wait_pid_gone(worker, 3000) < 0 ||
                wait_pid_gone((pid_t)shell_pid, 3000) < 0)
                goto out;
            worker = worker2;
            worker2 = -1;
            shell_pid = -1;
        } else {
            where = "cycle outer peer restart";
            stop_pid(sup);
            sup = -1;
            worker = -1;
            if (shell_pid > 0 && wait_pid_gone((pid_t)shell_pid, 3000) < 0)
                goto out;
            shell_pid = -1;
            sup = spawn_supervisor_with_storage(carrier, NULL);
            if (sup <= 0)
                goto out;
            worker = wait_child_pid(sup, -1, 15000);
            if (worker <= 0)
                goto out;
        }
        close(uh); uh = -1;
        close(ut); ut = -1;

        where = "cycle reconcile";
        if (wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0 ||
            wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0 ||
            wait_path_exists(unix_host, 5000) < 0 ||
            wait_path_exists(unix_target, 5000) < 0) {
            dump_excerpt("mixed reconcile status", out);
            goto out;
        }
        if (host_fd_base < 0)
            host_fd_base = count_fds(host);
        fd = hc2_connect_hello_retry(structured, 5000);
        console_fd = wait_connect(console, 10000);
        if (fd < 0 || console_fd < 0)
            goto out;
        usleep(500000);
        if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' M PID2 \"$$\"",
                             "MPID2:", out, sizeof(out)) < 0)
            goto out;
        shell_pid_after = parse_pid_marker(out, "MPID2:");
        if ((i % 3) == 0) {
            if (shell_pid_after != shell_pid)
                goto out;
        } else {
            if (shell_pid_after <= 0 || shell_pid_after == shell_pid)
                goto out;
            shell_pid = shell_pid_after;
        }
        close(console_fd);
        console_fd = -1;

        snprintf(marker, sizeof(marker), "MIX_POST_%02d", i);
        where = "cycle post operations";
        if (hc2_run_echo(fd, 11000 + (uint64_t)i, marker) < 0 ||
            supervisor_unix_exchange(unix_host, unix_target, (i + 1) & 1,
                                     (uint8_t)(i + 32)) < 0 ||
            run_lmux_file("put", file_sock, src, target_path) < 0 ||
            run_lmux_file("get", file_sock, target_path, got_path) < 0 ||
            files_equal(src, got_path) < 0)
            goto out;
        if (read_status(control, out, sizeof(out)) == 0 &&
            strstr(out, "pending_fd=yes")) {
            dump_excerpt("mixed pending status", out);
            goto out;
        }
    }

    where = "counter sanity";
    if (read_status(control, out, sizeof(out)) < 0)
        goto out;
    crc1 = status_value(out, "crc_errors=");
    seq1 = status_value(out, "seq_errors=");
    proto1 = status_value(out, "protocol_errors=");
    stream1 = status_value(out, "stream_errors=");
    stale1 = status_value(out, "stale=");
    drops1 = status_value(out, "pre_established_drops=");
    if (crc1 != crc0 || seq1 != seq0 || proto1 != proto0 ||
        stream1 != stream0 || stale1 > stale0 + 12 || drops1 != drops0) {
        dump_excerpt("mixed counter status", out);
        goto out;
    }
    where = "fd leak check";
    if (host > 0 && host_fd_base >= 0) {
        host_fd_end = wait_fd_count(host, host_fd_base, 5000);
        if (host_fd_end < 0 || host_fd_end > host_fd_base + 2) {
            fprintf(stderr, "mixed soak host fds base=%d end=%d\n",
                    host_fd_base, host_fd_end);
            goto out;
        }
    }
    rc = 0;
out:
    if (rc)
        fprintf(stderr,
                "mixed_service_soak where=%s cycle=%d shell=%ld after=%ld worker=%ld counters=%llu/%llu %llu/%llu %llu/%llu %llu/%llu %llu/%llu %llu/%llu\n",
                where, i, shell_pid, shell_pid_after, (long)worker,
                crc0, crc1, seq0, seq1, proto0, proto1, stream0, stream1,
                stale0, stale1, drops0, drops1);
    if (fd >= 0)
        close(fd);
    if (console_fd >= 0)
        close(console_fd);
    if (uh >= 0)
        close(uh);
    if (ut >= 0)
        close(ut);
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); unlink(unix_host); unlink(unix_target);
    unlink(src); unlink(target_path); unlink(got_path);
    rmdir(dir);
    return rc;
}

static int structured_exec_lifecycle_repeat_test(void)
{
    char dir[] = "/tmp/lmux-exec-life-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out[32768];
    pid_t sup = -1, host = -1;
    int fd = -1, rc = 1;
    int fd_base = -1, fd_end = -1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type;
    uint64_t req, ref = 0;
    uint32_t len;
    int i = -1;
    const char *where = "setup";
    const char *argv_io[] = {"/bin/sh", "-c", "printf OUT; printf ERR >&2"};
    const char *argv_cat[] = {"/bin/cat"};

    if (!mkdtemp(dir))
        return fail("mkdtemp exec life");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    sup = spawn_supervisor_with_storage(carrier, NULL);
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (sup <= 0 || host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    fd = hc2_connect_hello_retry(structured, 5000);
    if (fd < 0)
        goto out;
    fd_base = count_fds(host);
    if (fd_base < 0)
        goto out;
    for (i = 0; i < 25; i++) {
        int done = 0, out_seen = 0, err_seen = 0, released = 0;
        where = "io start";
        ref = 0;
        if (hc2_exec_start_ready_ref(fd, 5000 + (uint64_t)i, "",
                                     argv_io, 3, &ref) < 0 || !ref)
            goto out;
        where = "io drain";
        while (!done || !out_seen || !err_seen) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
                goto out;
            if (type == LMUX_HC2_EXEC_STDOUT && len >= 8 &&
                hc2_test_get_u64(payload) == ref) {
                if (len - 8 == 3 && memcmp(payload + 8, "OUT", 3) == 0)
                    out_seen = 1;
            } else if (type == LMUX_HC2_EXEC_STDERR && len >= 8 &&
                       hc2_test_get_u64(payload) == ref) {
                if (len - 8 == 3 && memcmp(payload + 8, "ERR", 3) == 0)
                    err_seen = 1;
            } else if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                       hc2_test_get_u64(payload) == ref &&
                       payload[16] == V2_EXEC_STATUS_DONE) {
                done = 1;
            } else if (type == LMUX_HC2_EXEC_LOST ||
                       (type == LMUX_HC2_ERROR && len >= 4 &&
                        lmux_get_le32(payload) != LMUX_HC2_OK)) {
                goto out;
            }
        }
        where = "io release";
        hc2_test_put_u64(payload, ref);
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_RELEASE,
                           6000 + (uint64_t)i, payload, 8) < 0)
            goto out;
        while (!released) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
                goto out;
            if (type == LMUX_HC2_EXEC_STATE && req == 6000 + (uint64_t)i)
                released = 1;
            else if (type == LMUX_HC2_ERROR && len >= 4 &&
                     lmux_get_le32(payload) != LMUX_HC2_OK)
                goto out;
        }
    }
    for (i = 0; i < 15; i++) {
        int done = 0, released = 0;
        where = "stdin close start";
        ref = 0;
        if (hc2_exec_start_ready_ref(fd, 7000 + (uint64_t)i, "",
                                     argv_cat, 1, &ref) < 0 || !ref)
            goto out;
        hc2_test_put_u64(payload, ref);
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_STDIN_CLOSE,
                           7100 + (uint64_t)i, payload, 8) < 0)
            goto out;
        while (!done) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
                goto out;
            if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
                hc2_test_get_u64(payload) == ref &&
                payload[16] == V2_EXEC_STATUS_DONE)
                done = 1;
            else if (type == LMUX_HC2_EXEC_LOST ||
                     (type == LMUX_HC2_ERROR && len >= 4 &&
                      lmux_get_le32(payload) != LMUX_HC2_OK))
                goto out;
        }
        where = "stdin close release";
        hc2_test_put_u64(payload, ref);
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_RELEASE,
                           7200 + (uint64_t)i, payload, 8) < 0)
            goto out;
        while (!released) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 3000) < 0)
                goto out;
            if (type == LMUX_HC2_EXEC_STATE && req == 7200 + (uint64_t)i)
                released = 1;
            else if (type == LMUX_HC2_ERROR && len >= 4 &&
                     lmux_get_le32(payload) != LMUX_HC2_OK)
                goto out;
        }
    }
    fd_end = wait_fd_count(host, fd_base, 5000);
    if (fd_end != fd_base) {
        fprintf(stderr, "exec lifecycle fds base=%d end=%d\n", fd_base, fd_end);
        goto out;
    }
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "structured_exec_lifecycle_repeat where=%s cycle=%d ref=%llu\n",
                where, i, (unsigned long long)ref);
    if (fd >= 0)
        close(fd);
    stop_pid(host);
    stop_pid(sup);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static int structured_control_scheduler_test(void)
{
    char dir[] = "/tmp/lmux-structured-scheduler-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out[32768];
    pid_t sup = -1, host = -1;
    int fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type = 0;
    uint64_t req = 0, refs[3] = {0, 0, 0};
    uint64_t reqs[9];
    uint32_t len = 0;
    int pending[9];
    int remaining = 9;
    int i, j;
    long long end;
    const char *argv_a[] = {"/bin/sh", "-c", "trap '' INT TERM HUP; printf AAA; sleep 4; printf AAA_DONE"};
    const char *argv_b[] = {"/bin/sh", "-c", "trap '' INT TERM HUP; printf BBB; sleep 4; printf BBB_DONE"};
    const char *argv_c[] = {"/bin/sh", "-c", "trap '' INT TERM HUP; printf CCC; sleep 4; printf CCC_DONE"};
    const char **argvs[3] = {argv_a, argv_b, argv_c};
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp structured scheduler");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    fd = hc2_connect_hello_retry(structured, 3000);
    if (fd < 0)
        goto out;
    where = "start";
    for (i = 0; i < 3; i++)
        if (hc2_exec_start_ready_ref(fd, 700 + (uint64_t)i, "",
                                     argvs[i], 3, &refs[i]) < 0 ||
            !refs[i])
            goto out;
    where = "burst control";
    memset(pending, 1, sizeof(pending));
    for (i = 0; i < 3; i++) {
        uint64_t base = 800 + (uint64_t)i * 3;
        hc2_test_put_u64(payload, refs[i]);
        reqs[i * 3] = base;
        reqs[i * 3 + 1] = base + 1;
        reqs[i * 3 + 2] = base + 2;
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_STATUS, base, payload, 8) < 0)
            goto out;
        payload[8] = LMUX_HC2_SIG_HUP;
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_SIGNAL, base + 1, payload, 9) < 0)
            goto out;
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_STATUS, base + 2, payload, 8) < 0)
            goto out;
    }
    where = "burst replies";
    end = now_ms() + 12000;
    while (remaining > 0 && now_ms() < end) {
        if (hc2_read_frame(fd, &type, &req, payload, &len,
                           (int)(end - now_ms())) < 0)
            goto out;
        if (type == LMUX_HC2_ERROR && len >= 4) {
            uint32_t code = lmux_get_le32(payload);
            if (code == LMUX_HC2_E_BUSY)
                goto out;
            for (j = 0; j < 9; j++) {
                if (pending[j] && req == reqs[j]) {
                    if (code != LMUX_HC2_OK)
                        goto out;
                    pending[j] = 0;
                    remaining--;
                    break;
                }
            }
        } else if (type == LMUX_HC2_EXEC_STATE && len >= 32) {
            for (j = 0; j < 9; j++) {
                if (pending[j] && req == reqs[j]) {
                    pending[j] = 0;
                    remaining--;
                    break;
                }
            }
        } else if (type == LMUX_HC2_EXEC_LOST) {
            goto out;
        }
    }
    if (remaining != 0)
        goto out;
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "structured_control_scheduler where=%s remaining=%d last=%s(%u) req=%llu len=%u err=%u\n",
                where, remaining, hc2_type_name(type), type,
                (unsigned long long)req, len,
                (type == LMUX_HC2_ERROR && len >= 4) ? lmux_get_le32(payload) : 0);
    if (fd >= 0)
        close(fd);
    stop_pid(host); stop_pid(sup);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static int structured_deploy_coexistence_test(void)
{
    char dir[] = "/tmp/lmux-structured-deploy-XXXXXX";
    char storage[200], carrier[200], control[200], structured[200];
    char console[200], file_sock[200], pty_arg[420], file_arg[260];
    char deploy_src[220], out[65536];
    pid_t sup = -1, host = -1;
    int fd = -1, rc = 1;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
    uint16_t type = 0;
    uint64_t req = 0, refs[3] = {0, 0, 0}, fresh = 0;
    uint32_t len = 0;
    const char *argv_a[] = {"/bin/sh", "-c", "printf AAA; sleep 120"};
    const char *argv_b[] = {"/bin/sh", "-c", "printf BBB; sleep 120"};
    const char *argv_c[] = {"/bin/sh", "-c", "printf CCC; sleep 120"};
    const char *argv_true[] = {"/bin/true"};
    const char **argvs[3] = {argv_a, argv_b, argv_c};
    char seen[3][32];
    int pref[3] = {0, 0, 0}, terminal[3] = {0, 0, 0}, done = 0;
    int i;
    const char *where = "setup";

    memset(seen, 0, sizeof(seen));
    if (!mkdtemp(dir))
        return fail("mkdtemp structured deploy");
    snprintf(storage, sizeof(storage), "%s/storage", dir);
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(deploy_src, sizeof(deploy_src), "%s/deploy-worker.lmuxd", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (mkdir(storage, 0700) < 0)
        goto out;
    sup = spawn_supervisor_with_storage(carrier, storage);
    if (sup <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    fd = hc2_connect_hello_retry(structured, 3000);
    if (fd < 0)
        goto out;
    where = "start";
    for (i = 0; i < 3; i++) {
        if (hc2_exec_start_ready_ref(fd, 900 + (uint64_t)i, "",
                                     argvs[i], 3, &refs[i]) < 0 ||
            !refs[i])
            goto out;
        where = "prefix";
        while (!pref[i]) {
            if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
                goto out;
            if (type == LMUX_HC2_EXEC_STDOUT && len >= 8) {
                uint64_t ref = hc2_test_get_u64(payload);
                int j;
                for (j = 0; j < 3; j++) {
                    if (ref == refs[j]) {
                        size_t old = strlen(seen[j]), n = len - 8;
                        if (old + n >= sizeof(seen[j]))
                            goto out;
                        memcpy(seen[j] + old, payload + 8, n);
                        if ((j == 0 && strstr(seen[j], "AAA")) ||
                            (j == 1 && strstr(seen[j], "BBB")) ||
                            (j == 2 && strstr(seen[j], "CCC")))
                            pref[j] = 1;
                    }
                }
            } else if (type == LMUX_HC2_ERROR) {
                goto out;
            }
        }
        where = "start";
    }
    where = "legacy status";
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "__exec"))
        goto out;
    where = "deploy busy";
    if (copy_with_suffix("build/host/lmuxd", deploy_src,
                         "LMUX_STRUCTURED_DEPLOY_CANDIDATE\n") < 0 ||
        run_lmux_deploy(control, deploy_src, 120000, out, sizeof(out)) == 0 ||
        !strstr(out, "ERR DEPLOY BUSY structured-exec-active")) {
        dump_excerpt("structured deploy busy", out);
        goto out;
    }
    where = "status after busy";
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "__exec"))
        goto out;
    where = "kill";
    for (i = 0; i < 3; i++) {
        hc2_test_put_u64(payload, refs[i]);
        payload[8] = LMUX_HC2_SIG_KILL;
        if (hc2_send_frame(fd, LMUX_HC2_EXEC_SIGNAL,
                           960 + (uint64_t)i, payload, 9) < 0)
            goto out;
    }
    where = "terminal";
    while (!terminal[0] || !terminal[1] || !terminal[2]) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 15000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
            payload[16] == V2_EXEC_STATUS_DONE) {
            uint64_t ref = hc2_test_get_u64(payload);
            for (i = 0; i < 3; i++) {
                if (ref == refs[i]) {
                    if (terminal[i])
                        goto out;
                    terminal[i] = 1;
                }
            }
        } else if (type == LMUX_HC2_EXEC_LOST ||
                   (type == LMUX_HC2_ERROR && len >= 4 &&
                    lmux_get_le32(payload) != LMUX_HC2_OK)) {
            goto out;
        }
    }
    where = "status after terminal";
    if (read_status(control, out, sizeof(out)) < 0 ||
        !strstr(out, "connection: ESTABLISHED") ||
        !strstr(out, "__exec"))
        goto out;
    where = "deploy abort cleanup";
    {
        struct stat st;
        if (stat(deploy_src, &st) < 0 ||
            partial_deploy_disconnect(control, deploy_src,
                                      (uint64_t)st.st_size,
                                      st.st_size > 4096 ? 4096 : (size_t)st.st_size,
                                      120000) < 0)
            goto out;
    }
    where = "status after deploy abort";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0)
        goto out;
    where = "deploy after terminal";
    if (run_lmux_deploy(control, deploy_src, 120000, out, sizeof(out)) < 0 &&
        !strstr(out, "activation started")) {
        dump_excerpt("structured deploy after terminal", out);
        goto out;
    }
    where = "status after deploy progress";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0 ||
        wait_exec_service_ready(control, 15000, out, sizeof(out)) < 0)
        goto out;
    where = "fresh";
    if (hc2_exec_start_ready_ref(fd, 950, "", argv_true, 1, &fresh) < 0 ||
        !fresh)
        goto out;
    while (!done) {
        if (hc2_read_frame(fd, &type, &req, payload, &len, 7000) < 0)
            goto out;
        if (type == LMUX_HC2_EXEC_STATE && len >= 32 &&
            payload[16] == V2_EXEC_STATUS_DONE)
            done = 1;
        else if (type == LMUX_HC2_EXEC_LOST ||
                 (type == LMUX_HC2_ERROR && len >= 4 &&
                  lmux_get_le32(payload) != LMUX_HC2_OK))
            goto out;
    }
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "structured_deploy_coexistence where=%s A=%s B=%s C=%s last=%s(%u) req=%llu len=%u err=%u\n",
                where, seen[0], seen[1], seen[2], hc2_type_name(type), type,
                (unsigned long long)req, len,
                (type == LMUX_HC2_ERROR && len >= 4) ? lmux_get_le32(payload) : 0);
    if (fd >= 0)
        close(fd);
    stop_pid(host); stop_pid(sup);
    unlink(deploy_src);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(storage); rmdir(dir);
    return rc;
}

struct stream_proc {
    pid_t pid;
    int in_fd;
    int out_fd;
    int err_fd;
};

static void stream_proc_close(struct stream_proc *p)
{
    if (p->in_fd >= 0) { close(p->in_fd); p->in_fd = -1; }
    if (p->out_fd >= 0) { close(p->out_fd); p->out_fd = -1; }
    if (p->err_fd >= 0) { close(p->err_fd); p->err_fd = -1; }
}

static int spawn_lmux_stream_cli(const char *structured,
                                 const char **target_argv, int target_argc,
                                 struct stream_proc *p)
{
    int inpipe[2] = {-1, -1}, outpipe[2] = {-1, -1}, errpipe[2] = {-1, -1};
    char **args;
    int i;
    memset(p, 0, sizeof(*p));
    p->pid = -1;
    p->in_fd = p->out_fd = p->err_fd = -1;
    if (target_argc < 1 || pipe(inpipe) < 0 || pipe(outpipe) < 0 ||
        pipe(errpipe) < 0)
        goto fail;
    args = calloc((size_t)target_argc + 6, sizeof(char *));
    if (!args)
        goto fail;
    args[0] = "build/host/lmux";
    args[1] = "stream";
    args[2] = "--control";
    args[3] = (char *)structured;
    args[4] = "--";
    for (i = 0; i < target_argc; i++)
        args[5 + i] = (char *)target_argv[i];
    p->pid = fork();
    if (p->pid < 0) {
        free(args);
        goto fail;
    }
    if (p->pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        execv("build/host/lmux", args);
        _exit(127);
    }
    free(args);
    close(inpipe[0]); inpipe[0] = -1;
    close(outpipe[1]); outpipe[1] = -1;
    close(errpipe[1]); errpipe[1] = -1;
    p->in_fd = inpipe[1]; inpipe[1] = -1;
    p->out_fd = outpipe[0]; outpipe[0] = -1;
    p->err_fd = errpipe[0]; errpipe[0] = -1;
    (void)test_set_nonblock(p->in_fd);
    (void)test_set_nonblock(p->out_fd);
    (void)test_set_nonblock(p->err_fd);
    return 0;
fail:
    if (inpipe[0] >= 0) close(inpipe[0]);
    if (inpipe[1] >= 0) close(inpipe[1]);
    if (outpipe[0] >= 0) close(outpipe[0]);
    if (outpipe[1] >= 0) close(outpipe[1]);
    if (errpipe[0] >= 0) close(errpipe[0]);
    if (errpipe[1] >= 0) close(errpipe[1]);
    return -1;
}

static int run_lmux_stream_cli(const char *structured,
                               const char **target_argv, int target_argc,
                               const uint8_t *input, size_t input_len,
                               uint8_t **outp, size_t *out_lenp,
                               uint8_t **errp, size_t *err_lenp,
                               int *exit_codep, int timeout_ms)
{
    struct stream_proc p;
    uint8_t *out = NULL, *err = NULL;
    size_t out_len = 0, out_cap = input_len + 2 * 1024 * 1024;
    size_t err_len = 0, err_cap = 2 * 1024 * 1024;
    size_t in_pos = 0;
    int in_open = 1, out_open = 1, err_open = 1, status = 0;
    const char *fail_reason = "unknown";
    long long end = now_ms() + timeout_ms;
    pid_t waited = 0;
    memset(&p, 0, sizeof(p));
    p.pid = -1;
    p.in_fd = p.out_fd = p.err_fd = -1;
    if (out_cap < 65536)
        out_cap = 65536;
    out = malloc(out_cap);
    err = malloc(err_cap);
    if (!out || !err ||
        spawn_lmux_stream_cli(structured, target_argv, target_argc, &p) < 0) {
        fail_reason = "alloc/spawn";
        goto fail;
    }
    if (input_len == 0) {
        close(p.in_fd);
        p.in_fd = -1;
        in_open = 0;
    }
    while ((out_open || err_open || waited == 0) && now_ms() < end) {
        struct pollfd pfds[3];
        int nfds = 0, in_idx = -1, out_idx = -1, err_idx = -1;
        int rr;
        if (in_open) {
            in_idx = nfds;
            pfds[nfds].fd = p.in_fd;
            pfds[nfds].events = in_pos < input_len ? POLLOUT : 0;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (out_open) {
            out_idx = nfds;
            pfds[nfds].fd = p.out_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (err_open) {
            err_idx = nfds;
            pfds[nfds].fd = p.err_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (waited == 0) {
            pid_t w = waitpid(p.pid, &status, WNOHANG);
            if (w == p.pid)
                waited = w;
        }
        rr = poll(pfds, nfds, 50);
        if (rr < 0 && errno == EINTR)
            continue;
        if (rr < 0) {
            fail_reason = "poll";
            goto fail;
        }
        if (in_idx >= 0 && (pfds[in_idx].revents & POLLOUT)) {
            if (in_pos < input_len) {
                ssize_t w = write(p.in_fd, input + in_pos, input_len - in_pos);
                if (w > 0)
                    in_pos += (size_t)w;
                else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                         errno != EINTR)
                    { fail_reason = "stdin write"; goto fail; }
            }
            if (in_pos == input_len) {
                close(p.in_fd);
                p.in_fd = -1;
                in_open = 0;
            }
        } else if (in_idx >= 0 && (pfds[in_idx].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            close(p.in_fd);
            p.in_fd = -1;
            in_open = 0;
        }
        if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP))) {
            for (;;) {
                ssize_t r;
                if (out_len == out_cap)
                    { fail_reason = "stdout cap"; goto fail; }
                r = read(p.out_fd, out + out_len, out_cap - out_len);
                if (r > 0) {
                    out_len += (size_t)r;
                    continue;
                }
                if (r == 0) {
                    close(p.out_fd);
                    p.out_fd = -1;
                    out_open = 0;
                }
                if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                    errno != EINTR)
                    { fail_reason = "stdout read"; goto fail; }
                break;
            }
        }
        if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP))) {
            for (;;) {
                ssize_t r;
                if (err_len == err_cap)
                    { fail_reason = "stderr cap"; goto fail; }
                r = read(p.err_fd, err + err_len, err_cap - err_len);
                if (r > 0) {
                    err_len += (size_t)r;
                    continue;
                }
                if (r == 0) {
                    close(p.err_fd);
                    p.err_fd = -1;
                    err_open = 0;
                }
                if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                    errno != EINTR)
                    { fail_reason = "stderr read"; goto fail; }
                break;
            }
        }
    }
    if (waited == 0)
        waited = waitpid(p.pid, &status, WNOHANG);
    if (waited != p.pid) {
        fail_reason = "timeout/wait";
        goto fail;
    }
    stream_proc_close(&p);
    *outp = out;
    *out_lenp = out_len;
    *errp = err;
    *err_lenp = err_len;
    *exit_codep = WIFEXITED(status) ? WEXITSTATUS(status) :
        (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
    return 0;
fail:
    fprintf(stderr, "run_lmux_stream_cli fail reason=%s in_pos=%llu out_len=%llu err_len=%llu waited=%d\n",
            fail_reason, (unsigned long long)in_pos,
            (unsigned long long)out_len, (unsigned long long)err_len,
            waited == p.pid);
    if (err && err_len) {
        size_t dump = err_len < 512 ? err_len : 512;
        fprintf(stderr, "run_lmux_stream_cli child stderr: ");
        fwrite(err + err_len - dump, 1, dump, stderr);
        fprintf(stderr, "\n");
    }
    if (p.pid > 0) {
        kill(p.pid, SIGKILL);
        waitpid(p.pid, NULL, 0);
    }
    stream_proc_close(&p);
    free(out);
    free(err);
    return -1;
}

static int stream_cli_basic_test(void)
{
    char dir[] = "/tmp/lmux-stream-it-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out_status[32768], self[512], nbuf[32];
    pid_t sup = -1, host = -1;
    uint8_t *input = NULL, *out = NULL, *err = NULL;
    size_t input_len = 256 * 1024, out_len = 0, err_len = 0, i;
    int code = 0, rc = 1;
    const char *where = "setup";
    const char *cat_argv[] = {"/bin/cat"};
    const char *tail_argv[] = {"/bin/sh", "-c", "cat >/dev/null; printf AFTER_EOF"};
    const char *mark_argv[] = {"/bin/sh", "-c", "printf MARK; cat >/dev/null"};
    const char *exit_argv[] = {"/bin/sh", "-c", "exit 37"};
    const char *split_argv[3];
    const char *slow_argv[4];
    const char *output_argv[4];
    const char *duplex_argv[4];

    if (!mkdtemp(dir))
        return fail("mkdtemp stream");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (!realpath("build/host/test_supervisor_integration", self))
        goto out;
    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0)
        goto out;
    where = "ready";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out_status, sizeof(out_status)) < 0 ||
        wait_exec_service_ready(control, 15000, out_status, sizeof(out_status)) < 0)
        goto out;

    input = malloc(input_len);
    if (!input)
        goto out;
    for (i = 0; i < input_len; i++)
        input[i] = (uint8_t)((i * 131u + 17u) & 255u);
    input[0] = 0x00;
    input[1] = 0xff;
    input[2] = 0x03;
    input[3] = 0x0a;
    input[4] = 0x0d;

    where = "binary cat";
    if (run_lmux_stream_cli(structured, cat_argv, 1, input, input_len,
                            &out, &out_len, &err, &err_len, &code, 20000) < 0 ||
        code != 0 || out_len != input_len || memcmp(out, input, input_len) != 0 ||
        err_len != 0)
        goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "eof tail";
    if (run_lmux_stream_cli(structured, tail_argv, 3, (const uint8_t *)"abc", 3,
                            &out, &out_len, &err, &err_len, &code, 10000) < 0 ||
        code != 0 || out_len != 9 || memcmp(out, "AFTER_EOF", 9) != 0)
        goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "output before stdin";
    if (run_lmux_stream_cli(structured, mark_argv, 3, NULL, 0,
                            &out, &out_len, &err, &err_len, &code, 10000) < 0 ||
        code != 0 || out_len != 4 || memcmp(out, "MARK", 4) != 0)
        goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "stderr separation";
    split_argv[0] = self;
    split_argv[1] = "--exec-helper";
    split_argv[2] = "stream-split";
    {
        const uint8_t want_out[] = {0x00, 0xff, 0x03, 'O', 'U', 'T', '\n'};
        const uint8_t want_err[] = {0x7f, 0x00, 0x03, 'E', 'R', 'R', '\n'};
        if (run_lmux_stream_cli(structured, split_argv, 3, NULL, 0,
                                &out, &out_len, &err, &err_len, &code, 10000) < 0 ||
            code != 0 || out_len != sizeof(want_out) ||
            err_len != sizeof(want_err) ||
            memcmp(out, want_out, sizeof(want_out)) != 0 ||
            memcmp(err, want_err, sizeof(want_err)) != 0)
            goto out;
    }
    free(out); out = NULL; free(err); err = NULL;

    where = "exit status";
    if (run_lmux_stream_cli(structured, exit_argv, 3, NULL, 0,
                            &out, &out_len, &err, &err_len, &code, 10000) < 0 ||
        code != 37)
        goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "slow stdin backpressure";
    for (i = 0; i < input_len; i++)
        input[i] = (uint8_t)(i & 255u);
    snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)input_len);
    slow_argv[0] = self;
    slow_argv[1] = "--exec-helper";
    slow_argv[2] = "stream-slow-stdin";
    slow_argv[3] = nbuf;
    if (run_lmux_stream_cli(structured, slow_argv, 4, input, input_len,
                            &out, &out_len, &err, &err_len, &code, 30000) < 0 ||
        code != 0 || out_len != 13 || memcmp(out, "SLOW_STDIN_OK", 13) != 0)
        goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "stdout backpressure";
    snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)(1024 * 1024));
    output_argv[0] = self;
    output_argv[1] = "--exec-helper";
    output_argv[2] = "stream-output";
    output_argv[3] = nbuf;
    if (run_lmux_stream_cli(structured, output_argv, 4, NULL, 0,
                            &out, &out_len, &err, &err_len, &code, 30000) < 0 ||
        code != 0 || out_len != 1024 * 1024 || err_len != 0)
        goto out;
    for (i = 0; i < out_len; i++)
        if (out[i] != (uint8_t)(i & 255u))
            goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "full duplex";
    snprintf(nbuf, sizeof(nbuf), "%llu", (unsigned long long)(128 * 1024));
    duplex_argv[0] = self;
    duplex_argv[1] = "--exec-helper";
    duplex_argv[2] = "stream-full-duplex";
    duplex_argv[3] = nbuf;
    for (i = 0; i < 128 * 1024; i++)
        input[i] = (uint8_t)(i & 255u);
    if (run_lmux_stream_cli(structured, duplex_argv, 4, input, 128 * 1024,
                            &out, &out_len, &err, &err_len, &code, 30000) < 0 ||
        code != 0 || out_len != 128 * 1024 || err_len != 64 * 1024)
        goto out;
    for (i = 0; i < out_len; i++)
        if (out[i] != (uint8_t)((i * 17u + 3u) & 255u))
            goto out;
    for (i = 0; i < err_len; i++)
        if (err[i] != (uint8_t)((i * 31u + 7u) & 255u))
            goto out;
    free(out); out = NULL; free(err); err = NULL;

    rc = 0;
out:
    if (rc)
        fprintf(stderr, "stream_cli_basic where=%s code=%d out_len=%llu err_len=%llu\n",
                where, code, (unsigned long long)out_len,
                (unsigned long long)err_len);
    if (rc && err && err_len) {
        size_t cap = 256;
        size_t dump = err_len < cap ? err_len : cap;
        fprintf(stderr, "stream_cli_basic stderr head: ");
        fwrite(err, 1, dump, stderr);
        if (err_len > dump) {
            fprintf(stderr, "\nstream_cli_basic stderr tail: ");
            fwrite(err + err_len - dump, 1, dump, stderr);
        }
        fprintf(stderr, "\n");
    }
    free(input);
    free(out);
    free(err);
    stop_pid(host); stop_pid(sup);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static int wait_stream_proc_exit(struct stream_proc *p, int timeout_ms,
                                 int *exit_codep)
{
    long long end = now_ms() + timeout_ms;
    int status = 0;
    while (now_ms() < end) {
        pid_t w = waitpid(p->pid, &status, WNOHANG);
        if (w == p->pid) {
            if (exit_codep)
                *exit_codep = WIFEXITED(status) ? WEXITSTATUS(status) :
                    (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
            p->pid = -1;
            return 0;
        }
        if (w < 0 && errno == ECHILD)
            return -1;
        usleep(20000);
    }
    return -1;
}

static int stream_cli_cleanup_test(void)
{
    char dir[] = "/tmp/lmux-stream-cleanup-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out_status[32768], self[512];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    struct stream_proc p;
    uint8_t buf[4096];
    uint8_t *out = NULL, *err = NULL;
    size_t out_len = 0, err_len = 0;
    int code = 0, rc = 1;
    const char *where = "setup";
    const char *output_loop_argv[3];
    const char *cat_argv[] = {"/bin/cat"};
    const char *sig_argv[] = {"/bin/sh", "-c",
        "trap 'exit 42' INT TERM; while :; do sleep 1; done"};

    memset(&p, 0, sizeof(p));
    p.pid = -1;
    p.in_fd = p.out_fd = p.err_fd = -1;
    if (!mkdtemp(dir))
        return fail("mkdtemp stream cleanup");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (!realpath("build/host/test_supervisor_integration", self))
        goto out;
    sup = spawn_supervisor_with_storage(carrier, NULL);
    if (sup <= 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    host = spawn_host_structured(carrier, control, structured, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out_status, sizeof(out_status)) < 0 ||
        wait_exec_service_ready(control, 15000, out_status, sizeof(out_status)) < 0)
        goto out;

    where = "epipe cleanup";
    output_loop_argv[0] = self;
    output_loop_argv[1] = "--exec-helper";
    output_loop_argv[2] = "stream-output-loop";
    if (spawn_lmux_stream_cli(structured, output_loop_argv, 3, &p) < 0)
        goto out;
    code = -1;
    close(p.in_fd); p.in_fd = -1;
    close(p.out_fd); p.out_fd = -1;
    if (wait_stream_proc_exit(&p, 20000, &code) < 0 || code == 0)
        goto out;
    stream_proc_close(&p);
    if (run_lmux_stream_cli(structured, cat_argv, 1, (const uint8_t *)"OK", 2,
                            &out, &out_len, &err, &err_len, &code, 10000) < 0 ||
        code != 0 || out_len != 2 || memcmp(out, "OK", 2) != 0)
        goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "sigint cleanup";
    if (spawn_lmux_stream_cli(structured, sig_argv, 3, &p) < 0)
        goto out;
    code = -1;
    close(p.in_fd); p.in_fd = -1;
    usleep(500000);
    kill(p.pid, SIGINT);
    if (wait_stream_proc_exit(&p, 10000, &code) < 0)
        goto out;
    stream_proc_close(&p);
    if (run_lmux_stream_cli(structured, cat_argv, 1, (const uint8_t *)"OK2", 3,
                            &out, &out_len, &err, &err_len, &code, 10000) < 0 ||
        code != 0 || out_len != 3 || memcmp(out, "OK2", 3) != 0)
        goto out;
    free(out); out = NULL; free(err); err = NULL;

    where = "worker replacement cleanup";
    if (spawn_lmux_stream_cli(structured, cat_argv, 1, &p) < 0)
        goto out;
    code = -1;
    if (write_all(p.in_fd, "LIVE", 4) < 0 ||
        read_exact_timeout(p.out_fd, buf, 4, 5000) < 0 ||
        memcmp(buf, "LIVE", 4) != 0)
        goto out;
    kill(worker, SIGKILL);
    worker2 = wait_child_pid(sup, worker, 15000);
    if (worker2 <= 0 || worker2 == worker)
        goto out;
    worker = worker2;
    worker2 = -1;
    if (wait_stream_proc_exit(&p, 15000, &code) < 0 || code == 0)
        goto out;
    stream_proc_close(&p);
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out_status, sizeof(out_status)) < 0 ||
        wait_exec_service_ready(control, 15000, out_status, sizeof(out_status)) < 0)
        goto out;
    if (run_lmux_stream_cli(structured, cat_argv, 1, (const uint8_t *)"OK3", 3,
                            &out, &out_len, &err, &err_len, &code, 10000) < 0 ||
        code != 0 || out_len != 3 || memcmp(out, "OK3", 3) != 0)
        goto out;

    rc = 0;
out:
    if (rc)
        fprintf(stderr, "stream_cli_cleanup where=%s code=%d worker=%ld\n",
                where, code, (long)worker);
    if (p.pid > 0) {
        kill(p.pid, SIGKILL);
        waitpid(p.pid, NULL, 0);
    }
    stream_proc_close(&p);
    free(out);
    free(err);
    stop_pid(host); stop_pid(sup);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static int stream_cli_reconnect_test(void)
{
    char dir[] = "/tmp/lmux-stream-reconnect-XXXXXX";
    char carrier[160], control[160], structured[160], console[160], file_sock[160];
    char pty_arg[360], file_arg[240], out_status[32768];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    struct stream_proc p;
    int console_fd = -1, code = -1, rc = 1;
    char got[32];
    long shell_pid = -1, shell_pid_after = -1;
    const char *where = "setup";
    const char *cat_argv[] = {"/bin/cat"};
    const char *fault_cmd = "printf STREAM_RECONNECT_FAULT\\n\n";
    struct saved_env outer_env = {0, NULL};

    memset(&p, 0, sizeof(p));
    p.pid = -1;
    p.in_fd = p.out_fd = p.err_fd = -1;
    if (!mkdtemp(dir))
        return fail("mkdtemp stream reconnect");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(structured, sizeof(structured), "%s/control-v2.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy_fds(host_pair[0], target_pair[0],
                                  OUTER_FAULT_BAD_CRC,
                                  OUTER_FAULT_TARGET_TO_HOST,
                                  arm[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0)
        goto out;
    if (save_env_var(&outer_env, "LMUX_SUP_IT_OUTER") < 0)
        goto out;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    host = spawn_host_fd_structured(host_pair[1], control, structured,
                                    pty_arg, file_arg, 1);
    restore_env_var("LMUX_SUP_IT_OUTER", &outer_env);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 15000, out_status, sizeof(out_status)) < 0 ||
        wait_exec_service_ready(control, 15000, out_status, sizeof(out_status)) < 0)
        goto out;
    where = "spawn stream";
    if (spawn_lmux_stream_cli(structured, cat_argv, 1, &p) < 0)
        goto out;
    where = "pre exchange";
    if (write_all(p.in_fd, "BEFORE", 6) < 0 ||
        read_exact_timeout(p.out_fd, (uint8_t *)got, 6, 5000) < 0 ||
        memcmp(got, "BEFORE", 6) != 0)
        goto out;

    where = "trigger reconnect";
    console_fd = wait_connect(console, 5000);
    if (console_fd < 0)
        goto out;
    usleep(500000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' S PID \"$$\"",
                         "SPID:", out_status, sizeof(out_status)) < 0)
        goto out;
    shell_pid = parse_pid_marker(out_status, "SPID:");
    if (shell_pid <= 0 || write_all(arm[1], "A", 1) < 0 ||
        write_all(console_fd, fault_cmd, strlen(fault_cmd)) < 0 ||
        wait_fault_injected(note[0], 5000) < 0)
        goto out;
    close(console_fd); console_fd = -1;
    where = "reconverge";
    if (wait_ready_layers(control, 1, 15000, out_status, sizeof(out_status)) < 0 ||
        wait_exec_service_ready(control, 15000, out_status, sizeof(out_status)) < 0)
        goto out;
    console_fd = wait_connect(console, 5000);
    if (console_fd < 0)
        goto out;
    usleep(500000);
    if (shell_cmd_expect(console_fd, "printf '%s%s:%s\\n' S PID2 \"$$\"",
                         "SPID2:", out_status, sizeof(out_status)) < 0)
        goto out;
    shell_pid_after = parse_pid_marker(out_status, "SPID2:");
    if (shell_pid_after != shell_pid)
        goto out;
    close(console_fd); console_fd = -1;

    where = "post exchange and eof";
    if (write_all(p.in_fd, "AFTER", 5) < 0)
        goto out;
    close(p.in_fd); p.in_fd = -1;
    if (read_exact_timeout(p.out_fd, (uint8_t *)got, 5, 10000) < 0 ||
        memcmp(got, "AFTER", 5) != 0 ||
        wait_stream_proc_exit(&p, 15000, &code) < 0 ||
        code != 0)
        goto out;
    rc = 0;
out:
    if (rc)
        fprintf(stderr,
                "stream_cli_reconnect where=%s code=%d shell=%ld after=%ld\n",
                where, code, shell_pid, shell_pid_after);
    if (p.pid > 0) {
        kill(p.pid, SIGKILL);
        waitpid(p.pid, NULL, 0);
    }
    stream_proc_close(&p);
    if (console_fd >= 0)
        close(console_fd);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    stop_pid(host); stop_pid(sup); stop_pid(proxy);
    unlink(carrier); unlink(control); unlink(structured);
    unlink(console); unlink(file_sock); rmdir(dir);
    return rc;
}

static char helper_marker_dir[512];
static int helper_is_child;

static void helper_write_marker(const char *name)
{
    char path[640];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", helper_marker_dir, name);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) {
        (void)write(fd, "1\n", 2);
        close(fd);
    }
}

static void helper_signal_handler(int sig)
{
    if (sig == SIGINT)
        helper_write_marker(helper_is_child ? "child-sigint" : "leader-sigint");
    else if (sig == SIGTERM)
        helper_write_marker(helper_is_child ? "child-sigterm" : "leader-sigterm");
    _exit(128 + sig);
}

static int exec_helper_pgsig(const char *dir)
{
    pid_t child;
    if (strlen(dir) >= sizeof(helper_marker_dir))
        return 2;
    strcpy(helper_marker_dir, dir);
    signal(SIGINT, helper_signal_handler);
    signal(SIGTERM, helper_signal_handler);
    child = fork();
    if (child < 0)
        return 2;
    if (child == 0) {
        helper_is_child = 1;
        signal(SIGINT, helper_signal_handler);
        signal(SIGTERM, helper_signal_handler);
        helper_write_marker("child-ready");
        for (;;)
            pause();
    }
    helper_write_marker("leader-ready");
    for (;;)
        pause();
}

static int exec_helper_stdout_bp(const char *marker, size_t total)
{
    static const char pat[] = "0123456789abcdef";
    size_t pos = 0;
    int marked = 0;
#ifdef F_SETPIPE_SZ
    (void)fcntl(STDOUT_FILENO, F_SETPIPE_SZ, 4096);
#endif
    if (test_set_nonblock(STDOUT_FILENO) < 0)
        return 2;
    while (pos < total) {
        char buf[512];
        size_t i, n = sizeof(buf);
        ssize_t w;
        if (n > total - pos)
            n = total - pos;
        for (i = 0; i < n; i++)
            buf[i] = pat[(pos + i) & 15];
        w = write(STDOUT_FILENO, buf, n);
        if (w > 0) {
            pos += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!marked) {
                int fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0600);
                if (fd >= 0) {
                    char tmp[64];
                    int len = snprintf(tmp, sizeof(tmp), "%llu\n",
                                       (unsigned long long)pos);
                    if (len > 0)
                        (void)write(fd, tmp, (size_t)len);
                    close(fd);
                }
                marked = 1;
            }
            return 0;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return 3;
    }
    return marked ? 0 : 4;
}

static int exec_helper_stderr_bp(void)
{
    pid_t err_child, out_child;
    err_child = fork();
    if (err_child < 0)
        return 2;
    if (err_child == 0) {
        char buf[512];
        memset(buf, 'E', sizeof(buf));
        for (;;) {
            ssize_t w = write(STDERR_FILENO, buf, sizeof(buf));
            if (w < 0 && errno != EINTR)
                _exit(0);
        }
    }
    out_child = fork();
    if (out_child < 0)
        return 2;
    if (out_child == 0) {
        usleep(200000);
        (void)write(STDOUT_FILENO, "STDOUT_OK", 9);
        _exit(0);
    }
    for (;;)
        pause();
}

static int exec_helper_stdin_delay(size_t total)
{
    size_t got = 0;
    usleep(300000);
    while (got < total) {
        uint8_t buf[256];
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r > 0) {
            ssize_t i;
            for (i = 0; i < r; i++) {
                if (buf[i] != (uint8_t)((got + (size_t)i) & 255))
                    return 3;
            }
            got += (size_t)r;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        return 4;
    }
    if (got != total)
        return 5;
    if (read(STDIN_FILENO, &(uint8_t){0}, 1) != 0)
        return 6;
    (void)write(STDOUT_FILENO, "STDIN_OK", 8);
    return 0;
}

static int exec_helper_stream_split(void)
{
    const uint8_t out[] = {0x00, 0xff, 0x03, 'O', 'U', 'T', '\n'};
    const uint8_t err[] = {0x7f, 0x00, 0x03, 'E', 'R', 'R', '\n'};
    return write_all(STDOUT_FILENO, out, sizeof(out)) < 0 ||
        write_all(STDERR_FILENO, err, sizeof(err)) < 0 ? 3 : 0;
}

static int exec_helper_stream_slow_stdin(size_t total)
{
    size_t got = 0;
    while (got < total) {
        uint8_t buf[37];
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r > 0) {
            size_t i;
            for (i = 0; i < (size_t)r; i++)
                if (buf[i] != (uint8_t)((got + i) & 255))
                    return 4;
            got += (size_t)r;
            usleep(1000);
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        return 5;
    }
    if (got != total) {
        char msg[96];
        int n = snprintf(msg, sizeof(msg), "GOT=%llu WANT=%llu\n",
                         (unsigned long long)got,
                         (unsigned long long)total);
        if (n > 0)
            (void)write(STDERR_FILENO, msg, (size_t)n);
        return 6;
    }
    return write_all(STDOUT_FILENO, "SLOW_STDIN_OK", 13) < 0 ? 7 : 0;
}

static int exec_helper_stream_output(size_t total)
{
    uint8_t buf[4096];
    size_t done = 0;
    while (done < total) {
        size_t i, n = total - done;
        if (n > sizeof(buf))
            n = sizeof(buf);
        for (i = 0; i < n; i++)
            buf[i] = (uint8_t)((done + i) & 255u);
        if (write_all(STDOUT_FILENO, buf, n) < 0)
            return 8;
        done += n;
    }
    return 0;
}

static int exec_helper_stream_output_loop(void)
{
    uint8_t buf[4096];
    size_t done = 0;
    for (;;) {
        size_t i;
        for (i = 0; i < sizeof(buf); i++)
            buf[i] = (uint8_t)((done + i) & 255u);
        if (write_all(STDOUT_FILENO, buf, sizeof(buf)) < 0)
            return 8;
        done += sizeof(buf);
    }
}

static int exec_helper_stream_full_duplex(size_t total)
{
    uint8_t in[257], out[257], err[113];
    size_t got = 0, out_done = 0, err_done = 0;
    while (got < total || out_done < total || err_done < total / 2) {
        if (got < total) {
            ssize_t r = read(STDIN_FILENO, in, sizeof(in));
            if (r > 0) {
                size_t i;
                for (i = 0; i < (size_t)r; i++)
                    if (in[i] != (uint8_t)((got + i) & 255u))
                        return 9;
                got += (size_t)r;
            } else if (r == 0) {
                if (got != total)
                    return 10;
            } else if (errno != EINTR) {
                return 11;
            }
        }
        if (out_done < total) {
            size_t i, n = total - out_done;
            if (n > sizeof(out))
                n = sizeof(out);
            for (i = 0; i < n; i++)
                out[i] = (uint8_t)(((out_done + i) * 17u + 3u) & 255u);
            if (write_all(STDOUT_FILENO, out, n) < 0)
                return 12;
            out_done += n;
        }
        if (err_done < total / 2) {
            size_t i, n = total / 2 - err_done;
            if (n > sizeof(err))
                n = sizeof(err);
            for (i = 0; i < n; i++)
                err[i] = (uint8_t)(((err_done + i) * 31u + 7u) & 255u);
            if (write_all(STDERR_FILENO, err, n) < 0)
                return 13;
            err_done += n;
        }
    }
    return 0;
}

static int exec_helper_main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[2], "pgsig") == 0)
        return exec_helper_pgsig(argv[3]);
    if (argc >= 5 && strcmp(argv[2], "stdout-bp") == 0)
        return exec_helper_stdout_bp(argv[3], (size_t)strtoull(argv[4], NULL, 10));
    if (argc >= 3 && strcmp(argv[2], "stderr-bp") == 0)
        return exec_helper_stderr_bp();
    if (argc >= 4 && strcmp(argv[2], "stdin-delay") == 0)
        return exec_helper_stdin_delay((size_t)strtoull(argv[3], NULL, 10));
    if (argc >= 3 && strcmp(argv[2], "stream-split") == 0)
        return exec_helper_stream_split();
    if (argc >= 4 && strcmp(argv[2], "stream-slow-stdin") == 0)
        return exec_helper_stream_slow_stdin((size_t)strtoull(argv[3], NULL, 10));
    if (argc >= 4 && strcmp(argv[2], "stream-output") == 0)
        return exec_helper_stream_output((size_t)strtoull(argv[3], NULL, 10));
    if (argc >= 3 && strcmp(argv[2], "stream-output-loop") == 0)
        return exec_helper_stream_output_loop();
    if (argc >= 4 && strcmp(argv[2], "stream-full-duplex") == 0)
        return exec_helper_stream_full_duplex((size_t)strtoull(argv[3], NULL, 10));
    return 2;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--exec-helper") == 0)
        return exec_helper_main(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    if (getenv("LMUX_SUP_IT_ONLY_CARRIER_B1"))
        return carrier_manager_standby_without_usb_test() ||
            carrier_manager_usb_without_uart_test() ||
            legacy_named_primary_status_test() ||
            named_carrier_cli_validation_test() ? 1 : 0;
    if (getenv("LMUX_SUP_IT_ONLY_CARRIER_B2"))
        return carrier_lmc1_state_machine_test() ||
            carrier_raw_switch_roundtrip_test() ||
            carrier_raw_recovery_without_primary_test() ||
            carrier_raw_ambiguous_fault_test() ||
            carrier_raw_old_loss_timing_test() ||
            carrier_raw_unavailable_busy_test() ? 1 : 0;
    if (getenv("LMUX_SUP_IT_ONLY_CARRIER_AMBIGUOUS"))
        return carrier_raw_ambiguous_fault_test() ? 1 : 0;
    if (getenv("LMUX_SUP_IT_ONLY_CARRIER_B4"))
        return carrier_dirty_fault_handover_test() ||
            carrier_dirty_explicit_failure_test() ||
            carrier_dirty_reopen_hold_test() ? 1 : 0;
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_DIRTY"))
        return carrier_dirty_structured_control_test() ? 1 : 0;
    if (getenv("LMUX_SUP_IT_ONLY_DISPATCHER"))
        return carrier_dispatcher_handover_console_test() ? 1 : 0;
    if (getenv("LMUX_SUP_IT_ONLY_USB_REGRESSION")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return outer_first_contact_lost_hello_test(1) ||
            outer_first_contact_lost_hello_test(0) ||
            outer_first_contact_truncated_hello_test() ||
            outer_duplicate_hello_burst_test() ||
            structured_lmux_reconnect_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_RECONNECT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_lmux_reconnect_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_WORKER")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_worker_replacement_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_WORKER_FRESH")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_worker_replacement_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_PRIVATE_EXEC")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return private_exec_basic_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_PRIVATE_EXEC_EPIPE")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return private_exec_stdin_epipe_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_CONTROL")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_control_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_CONCURRENT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_concurrent_exec_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_MULTI_RECONNECT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_multi_reconnect_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_MULTI_WORKER")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_multi_worker_replacement_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_SCHEDULER")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_control_scheduler_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_DEPLOY")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_deploy_coexistence_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STREAM_BASIC")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return stream_cli_basic_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STREAM_CLEANUP")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return stream_cli_cleanup_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STREAM_RECONNECT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return stream_cli_reconnect_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STREAM_RECONNECT_THEN_INSTALL")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return stream_cli_reconnect_test() ||
            supervisor_install_restart_shortcut_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_LIFECYCLE_LOOP")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return lifecycle_recovery_loop_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_EXEC_LIFECYCLE")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_exec_lifecycle_repeat_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_FILE_WORKER")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return file_worker_replacement_torture_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_MIXED_SOAK")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return mixed_service_soak_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_INSTALL_RESTART_SHORTCUT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return supervisor_install_restart_shortcut_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_OUTER_UPLOAD_FAULT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_CRC) ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_OUTER_UPLOAD_FAULT_ADJACENT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_OFFSET) ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_OUTER_FAULT_SEQ_H2T")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return outer_fault_recovery_test(OUTER_FAULT_SEQ_GAP,
                                         OUTER_FAULT_HOST_TO_TARGET) ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_OUTER_FIRST_CONTACT_LOST_ACK")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return outer_first_contact_lost_ack_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_OUTER_FIRST_CONTACT_LOST_HELLO")) {
        const char *v = getenv("LMUX_SUP_IT_OUTER_FIRST_CONTACT_TARGET_FIRST");
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return outer_first_contact_lost_hello_test(v && strcmp(v, "1") == 0) ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_CONTROL_THEN_RECONNECT")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_control_test() || structured_lmux_reconnect_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_LOST_HELLO_THEN_INSTALL")) {
        const char *v = getenv("LMUX_SUP_IT_OUTER_FIRST_CONTACT_TARGET_FIRST");
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return outer_first_contact_lost_hello_test(v && strcmp(v, "1") == 0) ||
            supervisor_install_restart_shortcut_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_WORKER_THEN_INSTALL")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_worker_replacement_test() ||
            supervisor_install_restart_shortcut_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_STRUCTURED_THEN_INSTALL")) {
        setenv("LMUX_SUP_IT_OUTER", "1", 1);
        return structured_control_test() ||
            structured_lmux_reconnect_test() ||
            structured_worker_replacement_test() ||
            supervisor_install_restart_shortcut_test() ? 1 : 0;
    }
    if (getenv("LMUX_SUP_IT_ONLY_HOST_FIRST"))
        return host_first_startup_test() ? 1 : 0;
    if (carrier_manager_standby_without_usb_test() ||
        carrier_manager_usb_without_uart_test() ||
        legacy_named_primary_status_test() ||
        named_carrier_cli_validation_test() ||
        carrier_lmc1_state_machine_test() ||
        carrier_raw_switch_roundtrip_test() ||
        carrier_raw_recovery_without_primary_test() ||
        carrier_raw_ambiguous_fault_test() ||
        carrier_raw_old_loss_timing_test() ||
        carrier_raw_unavailable_busy_test() ||
        carrier_dirty_fault_handover_test() ||
        carrier_dirty_structured_control_test() ||
        carrier_dirty_explicit_failure_test() ||
        carrier_dirty_reopen_hold_test() ||
        invalid_storage_root_test())
        return 1;
    if (pending_abandon_test(0) || pending_abandon_test(1))
        return 1;
    if (outer_host_restart_same_physical_test() ||
        outer_supervisor_restart_same_host_physical_test() ||
        outer_first_contact_lost_hello_test(1) ||
        outer_first_contact_lost_hello_test(0) ||
        outer_first_contact_truncated_hello_test() ||
        outer_first_contact_lost_ack_test() ||
        outer_duplicate_hello_burst_test())
        return 1;
    if (host_first_startup_test() || integration_test())
        return 1;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    if (outer_boot_worker_backoff_test() ||
        managed_corrupt_state_startup_test() ||
        managed_artifact_failure_startup_test() ||
        outer_fault_recovery_test(OUTER_FAULT_BAD_CRC, OUTER_FAULT_TARGET_TO_HOST) ||
        outer_fault_recovery_test(OUTER_FAULT_SEQ_GAP, OUTER_FAULT_HOST_TO_TARGET) ||
        outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_HASH) ||
        outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_OFFSET) ||
        outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_CRC) ||
        outer_deploy_confirm_fault_test(OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC) ||
        outer_deploy_confirm_fault_test(OUTER_FAULT_BOOT_CONFIRM_REQ_BAD_CRC) ||
        private_exec_basic_test() ||
        structured_control_test() ||
        structured_control_scheduler_test() ||
        structured_lmux_reconnect_test() ||
        structured_worker_replacement_test() ||
        file_worker_replacement_torture_test() ||
        mixed_service_soak_test() ||
        lifecycle_recovery_loop_test() ||
        structured_exec_lifecycle_repeat_test() ||
        structured_deploy_coexistence_test() ||
        stream_cli_basic_test() ||
        stream_cli_cleanup_test() ||
        stream_cli_reconnect_test() ||
        supervisor_install_restart_shortcut_test() ||
        host_first_startup_test() || integration_test())
        return 1;
    unsetenv("LMUX_SUP_IT_OUTER");
    puts("supervisor integration selftest ok");
    return 0;
}
