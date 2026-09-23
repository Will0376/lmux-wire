#include "supervisor.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

int lmux_supervisor_msg_encode(uint8_t out[LMUX_SUP_MSG_LEN],
                               const struct lmux_supervisor_msg *msg)
{
    if (!out || !msg || msg->type < LMUX_SUP_LINK_DOWN ||
        msg->type > LMUX_SUP_SHUTDOWN)
        return -1;
    memset(out, 0, LMUX_SUP_MSG_LEN);
    out[0] = 'L';
    out[1] = 'M';
    out[2] = 'S';
    out[3] = '1';
    out[4] = msg->type;
    out[5] = 0;
    put_le16(out + 6, 0);
    put_le64(out + 8, msg->link_epoch);
    return 0;
}

int lmux_supervisor_msg_decode(const uint8_t in[LMUX_SUP_MSG_LEN],
                               struct lmux_supervisor_msg *msg)
{
    if (!in || !msg ||
        in[0] != 'L' || in[1] != 'M' || in[2] != 'S' || in[3] != '1' ||
        in[5] != 0 || get_le16(in + 6) != 0 ||
        in[4] < LMUX_SUP_LINK_DOWN || in[4] > LMUX_SUP_SHUTDOWN)
        return -1;
    msg->type = in[4];
    msg->link_epoch = get_le64(in + 8);
    return 0;
}

void lmux_proxy_ring_clear(struct lmux_proxy_ring *r)
{
    r->r = r->w = r->len = 0;
}

size_t lmux_proxy_ring_len(const struct lmux_proxy_ring *r)
{
    return r->len;
}

size_t lmux_proxy_ring_space(const struct lmux_proxy_ring *r)
{
    return sizeof(r->buf) - r->len;
}

static size_t ring_write(struct lmux_proxy_ring *r, const uint8_t *buf, size_t n)
{
    size_t done = 0;
    if (n > lmux_proxy_ring_space(r))
        n = lmux_proxy_ring_space(r);
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

static size_t ring_linear(const struct lmux_proxy_ring *r, const uint8_t **p)
{
    *p = r->buf + r->r;
    if (r->len == 0)
        return 0;
    if (r->r + r->len <= sizeof(r->buf))
        return r->len;
    return sizeof(r->buf) - r->r;
}

static void ring_drop(struct lmux_proxy_ring *r, size_t n)
{
    if (n > r->len)
        n = r->len;
    r->r = (r->r + n) % sizeof(r->buf);
    r->len -= n;
    if (r->len == 0)
        r->r = r->w = 0;
}

ssize_t lmux_proxy_read_into(int fd, struct lmux_proxy_ring *r)
{
    uint8_t tmp[512];
    size_t room = lmux_proxy_ring_space(r);
    ssize_t n;
    if (room == 0) {
        errno = EAGAIN;
        return -2;
    }
    if (room > sizeof(tmp))
        room = sizeof(tmp);
    n = read(fd, tmp, room);
    if (n > 0) {
        (void)ring_write(r, tmp, (size_t)n);
        return n;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return -2;
    return n == 0 ? 0 : -1;
}

ssize_t lmux_proxy_write_from(int fd, struct lmux_proxy_ring *r)
{
    const uint8_t *p;
    size_t n = ring_linear(r, &p);
    ssize_t w;
    if (n == 0) {
        errno = EAGAIN;
        return -2;
    }
    w = write(fd, p, n);
    if (w > 0) {
        ring_drop(r, (size_t)w);
        return w;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return -2;
    return -1;
}

int lmux_supervisor_drain_fd(int fd)
{
    uint8_t tmp[512];
    for (;;) {
        ssize_t r = read(fd, tmp, sizeof(tmp));
        if (r > 0)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return 0;
        return r == 0 ? 1 : -1;
    }
}
