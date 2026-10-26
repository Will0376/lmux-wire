#include "lmc1.h"

#include "link_protocol.h"
#include "lmux.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define LMC1_MAGIC0 'L'
#define LMC1_MAGIC1 'M'
#define LMC1_MAGIC2 'C'
#define LMC1_MAGIC3 '1'
#define LMC1_CRC_OFF 108

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) |
        ((uint64_t)lmux_get_le32(p + 4) << 32);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)v);
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

static int type_valid(uint8_t type)
{
    return type >= LMUX_LMC1_PREPARE && type <= LMUX_LMC1_RESUMED;
}

static int mode_valid(enum lmux_carrier_mode mode)
{
    return mode == LMUX_CARRIER_MODE_RAW || mode == LMUX_CARRIER_MODE_DIRTY;
}

static int status_valid(uint16_t status)
{
    return status <= LMUX_LMC1_STATUS_UNAVAILABLE;
}

int lmux_lmc1_encode(const struct lmux_lmc1_msg *msg,
                     uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t decoded[LMUX_LMC1_DECODED_LEN];
    size_t name_len, enc_len;
    if (!msg || !out || !out_len || !type_valid(msg->type) ||
        !status_valid(msg->status) || !mode_valid(msg->mode) ||
        !lmux_carrier_name_valid(msg->name)) {
        errno = EINVAL;
        return -1;
    }
    name_len = strlen(msg->name);
    memset(decoded, 0, sizeof(decoded));
    decoded[0] = LMC1_MAGIC0;
    decoded[1] = LMC1_MAGIC1;
    decoded[2] = LMC1_MAGIC2;
    decoded[3] = LMC1_MAGIC3;
    decoded[4] = LMUX_LMC1_VERSION;
    decoded[5] = msg->type;
    lmux_put_le16(decoded + 6, msg->status);
    put_le64(decoded + 8, msg->generation);
    lmux_put_le32(decoded + 16, msg->deadline_ms);
    memcpy(decoded + 20, msg->nonce, LMUX_LMC1_NONCE_LEN);
    memcpy(decoded + 36, msg->cookie, LMUX_LMC1_COOKIE_LEN);
    memcpy(decoded + 52, msg->boot_id, LMUX_LMC1_BOOT_ID_LEN);
    decoded[68] = (uint8_t)msg->mode;
    decoded[69] = (uint8_t)name_len;
    memcpy(decoded + 72, msg->name, name_len);
    lmux_put_le32(decoded + LMC1_CRC_OFF,
                  lmux_crc32c(decoded, LMC1_CRC_OFF));
    if (out_cap < LMUX_LMC1_MAX_ENCODED ||
        lmux_cobs_encode(decoded, sizeof(decoded), out, out_cap - 1,
                         &enc_len) < 0) {
        errno = ENOSPC;
        return -1;
    }
    out[enc_len++] = 0;
    *out_len = enc_len;
    return 0;
}

int lmux_lmc1_decode(const uint8_t *encoded, size_t encoded_len,
                     struct lmux_lmc1_msg *msg)
{
    uint8_t decoded[LMUX_LMC1_DECODED_LEN];
    size_t decoded_len, name_len, i;
    uint32_t got_crc;
    if (!encoded || !encoded_len || !msg)
        return -1;
    if (lmux_cobs_decode(encoded, encoded_len, decoded, sizeof(decoded),
                         &decoded_len) < 0)
        return 1;
    if (decoded_len < 4 || decoded[0] != LMC1_MAGIC0 ||
        decoded[1] != LMC1_MAGIC1 || decoded[2] != LMC1_MAGIC2 ||
        decoded[3] != LMC1_MAGIC3)
        return 1;
    if (decoded_len != LMUX_LMC1_DECODED_LEN ||
        decoded[4] != LMUX_LMC1_VERSION || !type_valid(decoded[5]) ||
        !status_valid(lmux_get_le16(decoded + 6)) ||
        decoded[70] != 0 || decoded[71] != 0)
        return -1;
    got_crc = lmux_get_le32(decoded + LMC1_CRC_OFF);
    if (got_crc != lmux_crc32c(decoded, LMC1_CRC_OFF))
        return -1;
    name_len = decoded[69];
    if (name_len == 0 || name_len > LMUX_CARRIER_NAME_MAX)
        return -1;
    for (i = 72 + name_len; i < LMC1_CRC_OFF; i++)
        if (decoded[i] != 0)
            return -1;
    memset(msg, 0, sizeof(*msg));
    msg->type = decoded[5];
    msg->status = lmux_get_le16(decoded + 6);
    msg->generation = get_le64(decoded + 8);
    msg->deadline_ms = lmux_get_le32(decoded + 16);
    memcpy(msg->nonce, decoded + 20, LMUX_LMC1_NONCE_LEN);
    memcpy(msg->cookie, decoded + 36, LMUX_LMC1_COOKIE_LEN);
    memcpy(msg->boot_id, decoded + 52, LMUX_LMC1_BOOT_ID_LEN);
    msg->mode = (enum lmux_carrier_mode)decoded[68];
    memcpy(msg->name, decoded + 72, name_len);
    msg->name[name_len] = 0;
    if (!mode_valid(msg->mode) || !lmux_carrier_name_valid(msg->name))
        return -1;
    return 0;
}

void lmux_lmc1_raw_init(struct lmux_lmc1_raw_io *io)
{
    memset(io, 0, sizeof(*io));
}

int lmux_lmc1_raw_queue(struct lmux_lmc1_raw_io *io,
                        const struct lmux_lmc1_msg *msg)
{
    if (!io || io->tx_pos != io->tx_len) {
        errno = EBUSY;
        return -1;
    }
    io->tx_len = io->tx_pos = 0;
    if (lmux_lmc1_encode(msg, io->tx, sizeof(io->tx), &io->tx_len) < 0)
        return -1;
    io->tx_type = msg->type;
    return 0;
}

static void deliver_record(struct lmux_lmc1_raw_io *io, int allow_outer,
                           lmux_lmc1_control_cb control_cb,
                           lmux_lmc1_outer_cb outer_cb, void *opaque)
{
    struct lmux_lmc1_msg msg;
    int rc = lmux_lmc1_decode(io->rx, io->rx_len, &msg);
    if (rc == 0) {
        io->valid_control_records++;
        if (control_cb)
            control_cb(opaque, &msg);
    } else if (rc < 0) {
        io->invalid_control_records++;
    } else {
        io->foreign_records++;
        if (allow_outer && outer_cb)
            outer_cb(opaque, io->rx, io->rx_len);
    }
}

int lmux_lmc1_raw_feed(struct lmux_lmc1_raw_io *io,
                       const uint8_t *buf, size_t len, int allow_outer,
                       lmux_lmc1_control_cb control_cb,
                       lmux_lmc1_outer_cb outer_cb, void *opaque)
{
    size_t i;
    if (!io || (!buf && len)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < len; i++) {
        if (buf[i] == 0) {
            if (!io->discard_until_delim && io->rx_len)
                deliver_record(io, allow_outer, control_cb, outer_cb, opaque);
            else if (io->discard_until_delim)
                io->invalid_control_records++;
            io->rx_len = 0;
            io->discard_until_delim = 0;
            continue;
        }
        if (io->discard_until_delim)
            continue;
        if (io->rx_len == sizeof(io->rx)) {
            io->rx_len = 0;
            io->discard_until_delim = 1;
            if (allow_outer) {
                errno = EPROTO;
                return -1;
            }
            continue;
        }
        io->rx[io->rx_len++] = buf[i];
    }
    return 0;
}

int lmux_lmc1_raw_flush(struct lmux_lmc1_raw_io *io,
                        struct lmux_carrier *carrier,
                        uint8_t *completed_type)
{
    if (completed_type)
        *completed_type = 0;
    if (!io || !carrier)
        return -1;
    while (io->tx_pos < io->tx_len) {
        struct lmux_carrier_io r =
            lmux_carrier_write(carrier, io->tx + io->tx_pos,
                               io->tx_len - io->tx_pos);
        if (r.status == LMUX_CARRIER_IO_PROGRESS) {
            io->tx_pos += (size_t)r.n;
            continue;
        }
        if (r.status == LMUX_CARRIER_IO_WOULD_BLOCK)
            return 0;
        return -1;
    }
    if (io->tx_len) {
        if (completed_type)
            *completed_type = io->tx_type;
        io->tx_len = io->tx_pos = 0;
        io->tx_type = 0;
        return 1;
    }
    return 0;
}

int lmux_lmc1_raw_has_output(const struct lmux_lmc1_raw_io *io)
{
    return io && io->tx_pos < io->tx_len;
}

void lmux_lmc1_raw_reset_rx(struct lmux_lmc1_raw_io *io)
{
    if (!io)
        return;
    io->rx_len = 0;
    io->discard_until_delim = 0;
}

static const char lmc1_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int lmc1_b64_value(uint8_t ch)
{
    const char *p = strchr(lmc1_b64, ch);
    return p ? (int)(p - lmc1_b64) : -1;
}

static size_t lmc1_b64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t i, o = 0;
    for (i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        size_t n = len - i;
        if (n > 1) v |= (uint32_t)in[i + 1] << 8;
        if (n > 2) v |= in[i + 2];
        out[o++] = lmc1_b64[(v >> 18) & 63];
        out[o++] = lmc1_b64[(v >> 12) & 63];
        out[o++] = n > 1 ? lmc1_b64[(v >> 6) & 63] : '=';
        out[o++] = n > 2 ? lmc1_b64[v & 63] : '=';
    }
    return o;
}

static int lmc1_b64_decode(const char *in, size_t len, uint8_t *out,
                           size_t cap, size_t *out_len)
{
    size_t i, o = 0;
    if (!len || (len & 3u))
        return -1;
    for (i = 0; i < len; i += 4) {
        int a = lmc1_b64_value((uint8_t)in[i]);
        int b = lmc1_b64_value((uint8_t)in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : lmc1_b64_value((uint8_t)in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : lmc1_b64_value((uint8_t)in[i + 3]);
        uint32_t v;
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) ||
            ((c == -2 || d == -2) && i + 4 != len))
            return -1;
        v = (uint32_t)a << 18 | (uint32_t)b << 12;
        if (c >= 0) v |= (uint32_t)c << 6;
        if (d >= 0) v |= (uint32_t)d;
        if (o >= cap) return -1;
        out[o++] = (uint8_t)(v >> 16);
        if (c >= 0) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(v >> 8);
        }
        if (d >= 0) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)v;
        }
        if ((c == -2 && (v & 0xffffu)) || (d == -2 && (v & 0xffu)))
            return -1;
    }
    *out_len = o;
    return 0;
}

void lmux_lmc1_dirty_init(struct lmux_lmc1_dirty_io *io)
{
    if (io)
        memset(io, 0, sizeof(*io));
}

int lmux_lmc1_dirty_queue(struct lmux_lmc1_dirty_io *io,
                          const struct lmux_lmc1_msg *msg)
{
    uint8_t raw[LMUX_LMC1_MAX_ENCODED];
    size_t raw_len, body_len;
    if (!io || io->tx_pos != io->tx_len) {
        errno = EBUSY;
        return -1;
    }
    if (lmux_lmc1_encode(msg, raw, sizeof(raw), &raw_len) < 0)
        return -1;
    memcpy(io->tx, LMUX_LMC1_DIRTY_PREFIX, LMUX_LMC1_DIRTY_PREFIX_LEN);
    body_len = lmc1_b64_encode(raw, raw_len,
        (char *)io->tx + LMUX_LMC1_DIRTY_PREFIX_LEN);
    io->tx_len = (uint16_t)(LMUX_LMC1_DIRTY_PREFIX_LEN + body_len + 2);
    io->tx[io->tx_len - 2] = '\r';
    io->tx[io->tx_len - 1] = '\n';
    io->tx_pos = 0;
    io->tx_type = msg->type;
    return 0;
}

static void lmc1_dirty_deliver(struct lmux_lmc1_dirty_io *io,
                               lmux_lmc1_control_cb cb, void *opaque)
{
    uint8_t raw[LMUX_LMC1_MAX_ENCODED];
    size_t raw_len = 0;
    struct lmux_lmc1_msg msg;
    if (io->invalid ||
        lmc1_b64_decode(io->rx, io->rx_len, raw, sizeof(raw), &raw_len) < 0 ||
        raw_len < 2 || raw[raw_len - 1] != 0 ||
        lmux_lmc1_decode(raw, raw_len - 1, &msg) != 0) {
        io->invalid_control_records++;
        return;
    }
    io->valid_control_records++;
    if (cb)
        cb(opaque, &msg);
}

static void lmc1_dirty_outside(struct lmux_lmc1_dirty_io *io, uint8_t ch)
{
    const char *prefix = LMUX_LMC1_DIRTY_PREFIX;
    if (ch == (uint8_t)prefix[io->prefix_match]) {
        io->prefix_match++;
        if (io->prefix_match == LMUX_LMC1_DIRTY_PREFIX_LEN) {
            io->in_record = 1;
            io->prefix_match = 0;
            io->rx_len = 0;
            io->saw_cr = 0;
            io->invalid = 0;
        }
    } else {
        io->foreign_bytes++;
        io->prefix_match = ch == (uint8_t)prefix[0] ? 1 : 0;
    }
}

int lmux_lmc1_dirty_feed(struct lmux_lmc1_dirty_io *io,
                         const uint8_t *buf, size_t len,
                         lmux_lmc1_control_cb cb, void *opaque)
{
    size_t i;
    if (!io || (!buf && len)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < len; i++) {
        uint8_t ch = buf[i];
        if (!io->in_record) {
            lmc1_dirty_outside(io, ch);
            continue;
        }
        if (ch == '~') {
            io->invalid_control_records++;
            io->in_record = 0;
            io->prefix_match = 1;
            io->rx_len = 0;
            io->saw_cr = 0;
            io->invalid = 0;
            continue;
        }
        if (io->saw_cr) {
            if (ch == '\n')
                lmc1_dirty_deliver(io, cb, opaque);
            else
                io->invalid_control_records++;
            io->in_record = 0;
            io->prefix_match = 0;
            io->rx_len = 0;
            io->saw_cr = 0;
            io->invalid = 0;
            if (ch != '\n')
                lmc1_dirty_outside(io, ch);
            continue;
        }
        if (ch == '\r') {
            io->saw_cr = 1;
            continue;
        }
        if (ch == '\n') {
            io->invalid_control_records++;
            io->in_record = 0;
            io->prefix_match = 0;
            io->rx_len = 0;
            io->invalid = 0;
            continue;
        }
        if (lmc1_b64_value(ch) < 0 && ch != '=')
            io->invalid = 1;
        if (io->rx_len < LMUX_LMC1_DIRTY_BASE64_MAX)
            io->rx[io->rx_len++] = (char)ch;
        else
            io->invalid = 1;
    }
    return 0;
}

int lmux_lmc1_dirty_flush(struct lmux_lmc1_dirty_io *io,
                          struct lmux_carrier *carrier,
                          uint8_t *completed_type)
{
    if (completed_type)
        *completed_type = 0;
    if (!io || !carrier)
        return -1;
    while (io->tx_pos < io->tx_len) {
        struct lmux_carrier_io r = lmux_carrier_write(
            carrier, io->tx + io->tx_pos, io->tx_len - io->tx_pos);
        if (r.status == LMUX_CARRIER_IO_PROGRESS) {
            io->tx_pos += (uint16_t)r.n;
            continue;
        }
        if (r.status == LMUX_CARRIER_IO_WOULD_BLOCK)
            return 0;
        return -1;
    }
    if (io->tx_len) {
        if (completed_type)
            *completed_type = io->tx_type;
        io->tx_len = io->tx_pos = 0;
        io->tx_type = 0;
        return 1;
    }
    return 0;
}

int lmux_lmc1_dirty_has_output(const struct lmux_lmc1_dirty_io *io)
{
    return io && io->tx_pos < io->tx_len;
}

int lmux_lmc1_dirty_receiving(const struct lmux_lmc1_dirty_io *io)
{
    return io && io->in_record;
}

void lmux_lmc1_dirty_reset_rx(struct lmux_lmc1_dirty_io *io)
{
    if (!io)
        return;
    io->rx_len = 0;
    io->prefix_match = 0;
    io->in_record = 0;
    io->saw_cr = 0;
    io->invalid = 0;
}

static uint64_t lmc1_hash64(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

void lmux_lmc1_dirty_session_ids(const uint8_t boot_id[LMUX_LMC1_BOOT_ID_LEN],
                                 uint64_t generation, const char *name,
                                 uint64_t *host_session,
                                 uint64_t *target_session)
{
    uint64_t h = UINT64_C(1469598103934665603);
    uint8_t gen[8];
    put_le64(gen, generation);
    h = lmc1_hash64(h, "DIRTY1", 6);
    h = lmc1_hash64(h, boot_id, LMUX_LMC1_BOOT_ID_LEN);
    h = lmc1_hash64(h, gen, sizeof(gen));
    h = lmc1_hash64(h, name, strlen(name));
    if (host_session) {
        *host_session = lmc1_hash64(h, "host", 4);
        if (!*host_session) *host_session = 1;
    }
    if (target_session) {
        *target_session = lmc1_hash64(h, "target", 6);
        if (!*target_session) *target_session = 1;
    }
}

const char *lmux_lmc1_type_name(uint8_t type)
{
    switch (type) {
    case LMUX_LMC1_PREPARE: return "PREPARE";
    case LMUX_LMC1_READY: return "READY";
    case LMUX_LMC1_COMMIT: return "COMMIT";
    case LMUX_LMC1_COMMITTED: return "COMMITTED";
    case LMUX_LMC1_QUERY: return "QUERY";
    case LMUX_LMC1_QUERY_REPLY: return "QUERY_REPLY";
    case LMUX_LMC1_ERROR: return "ERROR";
    case LMUX_LMC1_RELEASE: return "RELEASE";
    case LMUX_LMC1_RELEASED: return "RELEASED";
    case LMUX_LMC1_RESUME: return "RESUME";
    case LMUX_LMC1_RESUMED: return "RESUMED";
    }
    return "UNKNOWN";
}

const char *lmux_lmc1_status_name(uint16_t status)
{
    switch (status) {
    case LMUX_LMC1_STATUS_OK: return "OK";
    case LMUX_LMC1_STATUS_BUSY: return "BUSY";
    case LMUX_LMC1_STATUS_UNKNOWN_CARRIER: return "UNKNOWN_CARRIER";
    case LMUX_LMC1_STATUS_UNSUPPORTED_MODE: return "UNSUPPORTED_MODE";
    case LMUX_LMC1_STATUS_STALE: return "STALE";
    case LMUX_LMC1_STATUS_BAD_COOKIE: return "BAD_COOKIE";
    case LMUX_LMC1_STATUS_EXPIRED: return "EXPIRED";
    case LMUX_LMC1_STATUS_BAD_REQUEST: return "BAD_REQUEST";
    case LMUX_LMC1_STATUS_UNAVAILABLE: return "UNAVAILABLE";
    }
    return "UNKNOWN";
}

int lmux_lmc1_nonce_equal(const uint8_t a[LMUX_LMC1_NONCE_LEN],
                          const uint8_t b[LMUX_LMC1_NONCE_LEN])
{
    return memcmp(a, b, LMUX_LMC1_NONCE_LEN) == 0;
}
