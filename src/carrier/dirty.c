#include "dirty.h"

#include "link_protocol.h"
#include "lmux.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define D_MAGIC0 'L'
#define D_MAGIC1 'M'
#define D_MAGIC2 'D'
#define D_MAGIC3 '1'

#define OUT_NONE 0
#define OUT_CONTROL 1
#define OUT_TX_INITIAL 2
#define OUT_TX_RESEND 3

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) |
        ((uint64_t)lmux_get_le32(p + 4) << 32);
}

static void put_le64(uint8_t *p, uint64_t value)
{
    lmux_put_le32(p, (uint32_t)value);
    lmux_put_le32(p + 4, (uint32_t)(value >> 32));
}

static size_t b64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) |
            ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = b64_table[(v >> 18) & 63];
        out[o++] = b64_table[(v >> 12) & 63];
        out[o++] = b64_table[(v >> 6) & 63];
        out[o++] = b64_table[v & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b64_table[(v >> 18) & 63];
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
            out[o++] = b64_table[(v >> 12) & 63];
            out[o++] = b64_table[(v >> 6) & 63];
            out[o++] = '=';
        } else {
            out[o++] = b64_table[(v >> 12) & 63];
            out[o++] = '=';
            out[o++] = '=';
        }
    }
    return o;
}

static int b64_value(uint8_t ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int b64_decode(const uint8_t *in, size_t len, uint8_t *out,
                      size_t cap, size_t *out_len)
{
    size_t i, o = 0;
    if (!len || (len & 3))
        return -1;
    for (i = 0; i < len; i += 4) {
        int a = b64_value(in[i]), b = b64_value(in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : b64_value(in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : b64_value(in[i + 3]);
        uint32_t v;
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) ||
            ((c == -2 || d == -2) && i + 4 != len))
            return -1;
        v = ((uint32_t)a << 18) | ((uint32_t)b << 12);
        if (c >= 0) v |= (uint32_t)c << 6;
        if (d >= 0) v |= (uint32_t)d;
        if (o >= cap) return -1;
        out[o++] = (uint8_t)(v >> 16);
        if (c >= 0) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(v >> 8);
        } else if ((b & 15) != 0) {
            return -1;
        }
        if (d >= 0) {
            if (o >= cap) return -1;
            out[o++] = (uint8_t)v;
        } else if (c >= 0 && (c & 3) != 0) {
            return -1;
        }
    }
    *out_len = o;
    return 0;
}

static int record_type_valid(uint8_t type)
{
    return type >= LMUX_DIRTY_DATA && type <= LMUX_DIRTY_FAIL;
}

static int fail_reason_valid(uint8_t reason)
{
    return reason >= LMUX_DIRTY_FAIL_BAD_RECORD &&
        reason <= LMUX_DIRTY_FAIL_PROTOCOL;
}

static int validate_record_reason(const struct lmux_dirty_record *r,
                                  uint8_t *reason)
{
    unsigned expected_count, expected_len;
    if (reason)
        *reason = LMUX_DIRTY_PARSE_SEMANTIC;
    if (!record_type_valid(r->type) || r->flags || !r->session_id)
        return -1;
    if (r->type == LMUX_DIRTY_DATA) {
        if (!r->packet_len || r->packet_len > LMUX_DIRTY_PACKET_MAX ||
            !r->fragment_count ||
            r->fragment_count > LMUX_DIRTY_FRAGMENTS_MAX ||
            r->fragment_index >= r->fragment_count || r->missing_bitmap ||
            r->reason || !r->payload_len ||
            r->payload_len > LMUX_DIRTY_FRAGMENT_MAX)
            return -1;
        expected_count = (r->packet_len + LMUX_DIRTY_FRAGMENT_MAX - 1) /
            LMUX_DIRTY_FRAGMENT_MAX;
        expected_len = r->fragment_index + 1 < r->fragment_count ?
            LMUX_DIRTY_FRAGMENT_MAX :
            r->packet_len - r->fragment_index * LMUX_DIRTY_FRAGMENT_MAX;
        if (r->fragment_count != expected_count ||
            r->fragment_offset != r->fragment_index * LMUX_DIRTY_FRAGMENT_MAX ||
            r->payload_len != expected_len)
            return -1;
        if (r->fragment_crc != lmux_crc32c(r->payload, r->payload_len)) {
            if (reason)
                *reason = LMUX_DIRTY_PARSE_FRAGMENT_CRC;
            return -1;
        }
        return 0;
    }
    if (r->packet_len || r->fragment_offset || r->fragment_index ||
        r->fragment_count || r->payload_len || r->packet_crc ||
        r->fragment_crc)
        return -1;
    if (r->type == LMUX_DIRTY_ACK)
        return (!r->missing_bitmap && !r->reason) ? 0 : -1;
    if (r->type == LMUX_DIRTY_NACK)
        return (r->missing_bitmap &&
                !(r->missing_bitmap & ~((1u << LMUX_DIRTY_FRAGMENTS_MAX) - 1)) &&
                !r->reason) ? 0 : -1;
    return (!r->missing_bitmap && fail_reason_valid(r->reason)) ? 0 : -1;
}

static int validate_record(const struct lmux_dirty_record *r)
{
    return validate_record_reason(r, NULL);
}

int lmux_dirty_record_encode(const struct lmux_dirty_record *r,
                             uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t body[LMUX_DIRTY_BINARY_MAX];
    size_t body_len, encoded_len;
    if (!r || !out || !out_len || validate_record(r) < 0) {
        errno = EINVAL;
        return -1;
    }
    body_len = LMUX_DIRTY_HEADER_LEN + r->payload_len + 4;
    if (out_cap < LMUX_DIRTY_PREFIX_LEN + ((body_len + 2) / 3) * 4 + 2) {
        errno = ENOSPC;
        return -1;
    }
    memset(body, 0, body_len);
    body[0] = D_MAGIC0; body[1] = D_MAGIC1;
    body[2] = D_MAGIC2; body[3] = D_MAGIC3;
    body[4] = LMUX_DIRTY_VERSION;
    body[5] = r->type;
    body[6] = LMUX_DIRTY_HEADER_LEN;
    body[7] = r->flags;
    put_le64(body + 8, r->session_id);
    lmux_put_le32(body + 16, r->packet_seq);
    lmux_put_le16(body + 20, r->packet_len);
    lmux_put_le16(body + 22, r->fragment_offset);
    body[24] = r->fragment_index;
    body[25] = r->fragment_count;
    body[26] = r->missing_bitmap;
    body[27] = r->reason;
    lmux_put_le16(body + 28, r->payload_len);
    lmux_put_le32(body + 32, r->packet_crc);
    lmux_put_le32(body + 36, r->fragment_crc);
    memcpy(body + LMUX_DIRTY_HEADER_LEN, r->payload, r->payload_len);
    lmux_put_le32(body + LMUX_DIRTY_HEADER_LEN + r->payload_len,
                  lmux_crc32c(body, LMUX_DIRTY_HEADER_LEN + r->payload_len));
    memcpy(out, LMUX_DIRTY_PREFIX, LMUX_DIRTY_PREFIX_LEN);
    encoded_len = b64_encode(body, body_len,
                             (char *)out + LMUX_DIRTY_PREFIX_LEN);
    out[LMUX_DIRTY_PREFIX_LEN + encoded_len] = '\r';
    out[LMUX_DIRTY_PREFIX_LEN + encoded_len + 1] = '\n';
    *out_len = LMUX_DIRTY_PREFIX_LEN + encoded_len + 2;
    return 0;
}

int lmux_dirty_record_decode_diagnostic(const uint8_t *wire, size_t wire_len,
                                        struct lmux_dirty_record *r,
                                        uint8_t *parse_reason)
{
    uint8_t body[LMUX_DIRTY_BINARY_MAX];
    char canonical[LMUX_DIRTY_BASE64_MAX];
    size_t encoded_len, body_len, canonical_len, payload_len;
    uint32_t got_crc;
    if (parse_reason)
        *parse_reason = LMUX_DIRTY_PARSE_NONE;
    if (!wire || !r || wire_len < LMUX_DIRTY_PREFIX_LEN + 6)
        return 1;
    if (memcmp(wire, LMUX_DIRTY_PREFIX, LMUX_DIRTY_PREFIX_LEN) != 0)
        return 1;
    if (wire[wire_len - 2] != '\r' || wire[wire_len - 1] != '\n') {
        if (parse_reason)
            *parse_reason = LMUX_DIRTY_PARSE_BAD_TERMINATOR;
        return -1;
    }
    encoded_len = wire_len - LMUX_DIRTY_PREFIX_LEN - 2;
    if (encoded_len > LMUX_DIRTY_BASE64_MAX ||
        b64_decode(wire + LMUX_DIRTY_PREFIX_LEN, encoded_len, body,
                   sizeof(body), &body_len) < 0 ||
        body_len < LMUX_DIRTY_HEADER_LEN + 4) {
        if (parse_reason)
            *parse_reason = LMUX_DIRTY_PARSE_BASE64;
        return -1;
    }
    canonical_len = b64_encode(body, body_len, canonical);
    if (canonical_len != encoded_len ||
        memcmp(canonical, wire + LMUX_DIRTY_PREFIX_LEN, encoded_len) != 0) {
        if (parse_reason)
            *parse_reason = LMUX_DIRTY_PARSE_CANONICAL;
        return -1;
    }
    if (body[0] != D_MAGIC0 || body[1] != D_MAGIC1 ||
        body[2] != D_MAGIC2 || body[3] != D_MAGIC3 ||
        body[4] != LMUX_DIRTY_VERSION ||
        body[6] != LMUX_DIRTY_HEADER_LEN || body[30] || body[31]) {
        if (parse_reason)
            *parse_reason = LMUX_DIRTY_PARSE_HEADER;
        return -1;
    }
    payload_len = lmux_get_le16(body + 28);
    if (payload_len > LMUX_DIRTY_FRAGMENT_MAX ||
        body_len != LMUX_DIRTY_HEADER_LEN + payload_len + 4) {
        if (parse_reason)
            *parse_reason = LMUX_DIRTY_PARSE_LENGTH;
        return -1;
    }
    got_crc = lmux_get_le32(body + LMUX_DIRTY_HEADER_LEN + payload_len);
    if (got_crc != lmux_crc32c(body, LMUX_DIRTY_HEADER_LEN + payload_len)) {
        if (parse_reason)
            *parse_reason = LMUX_DIRTY_PARSE_RECORD_CRC;
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->type = body[5]; r->flags = body[7];
    r->session_id = get_le64(body + 8);
    r->packet_seq = lmux_get_le32(body + 16);
    r->packet_len = lmux_get_le16(body + 20);
    r->fragment_offset = lmux_get_le16(body + 22);
    r->fragment_index = body[24]; r->fragment_count = body[25];
    r->missing_bitmap = body[26]; r->reason = body[27];
    r->payload_len = (uint16_t)payload_len;
    r->packet_crc = lmux_get_le32(body + 32);
    r->fragment_crc = lmux_get_le32(body + 36);
    memcpy(r->payload, body + LMUX_DIRTY_HEADER_LEN, payload_len);
    return validate_record_reason(r, parse_reason);
}

int lmux_dirty_record_decode(const uint8_t *wire, size_t wire_len,
                             struct lmux_dirty_record *r)
{
    return lmux_dirty_record_decode_diagnostic(wire, wire_len, r, NULL);
}

static uint8_t all_fragments(uint8_t count)
{
    return (uint8_t)((1u << count) - 1u);
}

static void set_failure(struct lmux_dirty *d, uint8_t reason)
{
    if (!d->failed) {
        if (reason == LMUX_DIRTY_FAIL_RETRY_EXHAUSTED)
            d->stats.retry_failures++;
        if (reason == LMUX_DIRTY_FAIL_DEADLINE)
            d->stats.deadline_failures++;
        d->failed = 1;
        d->fail_reason = reason;
        d->pending_fail = 1;
    }
}

static int queue_control(struct lmux_dirty *d, uint8_t type,
                         uint64_t session, uint32_t seq, uint8_t bitmap,
                         uint8_t reason)
{
    unsigned i;
    for (i = 0; i < LMUX_DIRTY_CONTROL_MAX; i++) {
        struct lmux_dirty_control *c = &d->control[i];
        if (c->used && c->session_id == session && c->seq == seq) {
            if (d->output_kind == OUT_CONTROL && d->output_slot == i) {
                if (c->type == type && c->bitmap == bitmap &&
                    c->reason == reason)
                    return 0;
                continue;
            }
            if (type == LMUX_DIRTY_ACK || c->type != LMUX_DIRTY_ACK) {
                c->type = type; c->bitmap = bitmap; c->reason = reason;
            }
            return 0;
        }
    }
    for (i = 0; i < LMUX_DIRTY_CONTROL_MAX; i++) {
        struct lmux_dirty_control *c = &d->control[i];
        if (!c->used) {
            memset(c, 0, sizeof(*c));
            c->used = 1; c->type = type; c->session_id = session;
            c->seq = seq; c->bitmap = bitmap; c->reason = reason;
            return 0;
        }
    }
    set_failure(d, LMUX_DIRTY_FAIL_PROTOCOL);
    return -1;
}

static uint32_t retry_interval(uint32_t baud)
{
    uint64_t wire_ms;
    if (baud < 1200) baud = 1200;
    wire_ms = (uint64_t)LMUX_DIRTY_RECORD_MAX *
        LMUX_DIRTY_FRAGMENTS_MAX * 10000u / baud;
    wire_ms += 100;
    if (wire_ms < 200) wire_ms = 200;
    if (wire_ms > 5000) wire_ms = 5000;
    return (uint32_t)wire_ms;
}

void lmux_dirty_init(struct lmux_dirty *d, uint64_t local_session_id,
                     uint64_t peer_session_id, uint32_t tx_first_seq,
                     uint32_t rx_first_seq, uint32_t baud, uint64_t now_ms)
{
    (void)now_ms;
    memset(d, 0, sizeof(*d));
    d->local_session_id = local_session_id;
    d->peer_session_id = peer_session_id;
    d->tx_next_seq = tx_first_seq;
    d->rx_next_seq = rx_first_seq;
    d->baud = baud;
    d->retry_interval_ms = retry_interval(baud);
    if (!local_session_id || !peer_session_id)
        set_failure(d, LMUX_DIRTY_FAIL_PROTOCOL);
}

void lmux_dirty_reset_peer(struct lmux_dirty *d, uint64_t peer_session_id,
                           uint32_t rx_first_seq, uint64_t now_ms)
{
    unsigned i;
    (void)now_ms;
    d->peer_session_id = peer_session_id;
    d->rx_next_seq = rx_first_seq;
    d->delivered_history = 0;
    memset(d->last_delivered_seq, 0, sizeof(d->last_delivered_seq));
    memset(d->rx, 0, sizeof(d->rx));
    for (i = 0; i < LMUX_DIRTY_CONTROL_MAX; i++)
        if (d->control[i].used &&
            d->control[i].session_id != d->local_session_id)
            memset(&d->control[i], 0, sizeof(d->control[i]));
}

int lmux_dirty_submit(struct lmux_dirty *d, const uint8_t *data, size_t len,
                      uint64_t now_ms, uint32_t *packet_seq)
{
    unsigned i;
    struct lmux_dirty_tx_slot *s = NULL;
    if (!d || (!data && len) || !len || len > LMUX_DIRTY_PACKET_MAX ||
        d->failed) {
        errno = d && d->failed ? EPIPE : EINVAL;
        return -1;
    }
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++)
        if (!d->tx[i].used) { s = &d->tx[i]; break; }
    if (!s) { errno = EAGAIN; return -1; }
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->len = (uint16_t)len;
    s->seq = d->tx_next_seq++;
    s->fragment_count = (uint8_t)((len + LMUX_DIRTY_FRAGMENT_MAX - 1) /
                                  LMUX_DIRTY_FRAGMENT_MAX);
    memcpy(s->data, data, len);
    s->packet_crc = lmux_crc32c(data, len);
    s->started_ms = now_ms;
    s->retry_at_ms = now_ms + d->retry_interval_ms;
    s->deadline_ms = now_ms + LMUX_DIRTY_DEADLINE_MAX_MS;
    if (packet_seq) *packet_seq = s->seq;
    return 0;
}

static struct lmux_dirty_tx_slot *find_tx(struct lmux_dirty *d, uint32_t seq)
{
    unsigned i;
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++)
        if (d->tx[i].used && d->tx[i].seq == seq)
            return &d->tx[i];
    return NULL;
}

static struct lmux_dirty_rx_slot *find_rx(struct lmux_dirty *d, uint32_t seq)
{
    unsigned i;
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++)
        if (d->rx[i].used && d->rx[i].seq == seq)
            return &d->rx[i];
    return NULL;
}

static int was_delivered(const struct lmux_dirty *d, uint32_t seq)
{
    unsigned i;
    for (i = 0; i < d->delivered_history; i++)
        if (d->last_delivered_seq[i] == seq) return 1;
    return 0;
}

static void handle_data(struct lmux_dirty *d,
                        const struct lmux_dirty_record *r, uint64_t now_ms)
{
    struct lmux_dirty_rx_slot *s;
    uint32_t distance;
    unsigned i;
    uint8_t bit = (uint8_t)(1u << r->fragment_index);
    if (r->session_id != d->peer_session_id) {
        d->stats.stale_records++;
        return;
    }
    if (was_delivered(d, r->packet_seq)) {
        d->stats.stale_records++;
        queue_control(d, LMUX_DIRTY_ACK, r->session_id, r->packet_seq, 0, 0);
        return;
    }
    distance = r->packet_seq - d->rx_next_seq;
    if (distance >= LMUX_DIRTY_WINDOW_MAX) {
        if (distance > UINT32_MAX / 2) {
            d->stats.stale_records++;
            /* rx_next advances only after verified upper-layer consumption. */
            queue_control(d, LMUX_DIRTY_ACK, r->session_id,
                          r->packet_seq, 0, 0);
            return;
        }
        set_failure(d, LMUX_DIRTY_FAIL_RX_WINDOW);
        return;
    }
    s = find_rx(d, r->packet_seq);
    if (!s) {
        for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++)
            if (!d->rx[i].used) { s = &d->rx[i]; break; }
        if (!s) { set_failure(d, LMUX_DIRTY_FAIL_RX_WINDOW); return; }
        memset(s, 0, sizeof(*s));
        s->used = 1; s->seq = r->packet_seq; s->len = r->packet_len;
        s->fragment_count = r->fragment_count;
        s->packet_crc = r->packet_crc;
        s->started_ms = now_ms;
        s->nack_at_ms = now_ms + d->retry_interval_ms;
        s->deadline_ms = now_ms + LMUX_DIRTY_DEADLINE_MAX_MS;
    } else if (s->len != r->packet_len ||
               s->fragment_count != r->fragment_count ||
               s->packet_crc != r->packet_crc) {
        set_failure(d, LMUX_DIRTY_FAIL_PROTOCOL);
        return;
    }
    if (s->received_bitmap & bit) {
        d->stats.duplicate_fragments++;
        return;
    }
    memcpy(s->data + r->fragment_offset, r->payload, r->payload_len);
    s->received_bitmap |= bit;
    d->stats.data_fragments++;
    if (s->received_bitmap == all_fragments(s->fragment_count)) {
        if (lmux_crc32c(s->data, s->len) != s->packet_crc) {
            d->stats.packet_crc_failures++;
            memset(s->data, 0, s->len);
            s->received_bitmap = 0;
            if (++s->nack_rounds > LMUX_DIRTY_RETRY_MAX) {
                set_failure(d, LMUX_DIRTY_FAIL_PACKET_CRC);
                return;
            }
            queue_control(d, LMUX_DIRTY_NACK, r->session_id, s->seq,
                          all_fragments(s->fragment_count), 0);
            s->nack_at_ms = now_ms + d->retry_interval_ms;
        } else {
            s->verified = 1;
        }
    }
}

static void handle_control(struct lmux_dirty *d,
                           const struct lmux_dirty_record *r, uint64_t now_ms)
{
    struct lmux_dirty_tx_slot *s;
    uint8_t mask;
    (void)now_ms;
    if (r->session_id != d->local_session_id) {
        d->stats.stale_records++;
        return;
    }
    if (r->type == LMUX_DIRTY_FAIL) {
        d->failed = 1;
        d->fail_reason = r->reason == LMUX_DIRTY_FAIL_NONE ?
            LMUX_DIRTY_FAIL_PEER : r->reason;
        return;
    }
    s = find_tx(d, r->packet_seq);
    if (!s) {
        d->stats.stale_records++;
        return;
    }
    if (r->type == LMUX_DIRTY_ACK) {
        d->stats.ack_received++;
        unsigned index = (unsigned)(s - d->tx);
        if ((d->output_kind == OUT_TX_INITIAL ||
             d->output_kind == OUT_TX_RESEND) && d->output_slot == index)
            s->acked = 1;
        else
            memset(s, 0, sizeof(*s));
        return;
    }
    d->stats.nack_received++;
    mask = r->missing_bitmap & all_fragments(s->fragment_count);
    if (!mask) { set_failure(d, LMUX_DIRTY_FAIL_PROTOCOL); return; }
    if (!s->resend_bitmap) {
        if (s->retry_rounds >= LMUX_DIRTY_RETRY_MAX) {
            set_failure(d, LMUX_DIRTY_FAIL_RETRY_EXHAUSTED);
            return;
        }
        s->retry_rounds++;
    }
    s->resend_bitmap |= mask;
    s->retry_at_ms = now_ms + d->retry_interval_ms;
}

static uint32_t diagnostic_hash_part(uint32_t h, const uint8_t *data,
                                     size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= UINT32_C(16777619);
    }
    return h;
}

static uint32_t diagnostic_record_hash(const uint8_t *body, size_t body_len)
{
    static const uint8_t ending[] = "\r\n";
    uint32_t h = UINT32_C(2166136261);
    h = diagnostic_hash_part(h, (const uint8_t *)LMUX_DIRTY_PREFIX,
                             LMUX_DIRTY_PREFIX_LEN);
    h = diagnostic_hash_part(h, body, body_len);
    return diagnostic_hash_part(h, ending, sizeof(ending) - 1);
}

static void diagnostic_sample(struct lmux_dirty *d, const uint8_t *body,
                              size_t body_len, size_t offset)
{
    size_t start, take;
    if (!body || !body_len) {
        d->diagnostic.last_invalid_sample_len = 0;
        return;
    }
    if (offset > body_len)
        offset = body_len;
    start = offset > LMUX_DIRTY_DIAG_SAMPLE_MAX / 2 ?
        offset - LMUX_DIRTY_DIAG_SAMPLE_MAX / 2 : 0;
    if (start + LMUX_DIRTY_DIAG_SAMPLE_MAX > body_len &&
        body_len > LMUX_DIRTY_DIAG_SAMPLE_MAX)
        start = body_len - LMUX_DIRTY_DIAG_SAMPLE_MAX;
    take = body_len - start;
    if (take > LMUX_DIRTY_DIAG_SAMPLE_MAX)
        take = LMUX_DIRTY_DIAG_SAMPLE_MAX;
    memcpy(d->diagnostic.last_invalid_sample, body + start, take);
    d->diagnostic.last_invalid_sample_len = (uint8_t)take;
}

static void note_invalid(struct lmux_dirty *d, uint8_t reason,
                         const uint8_t *body, size_t body_len, size_t offset,
                         uint64_t now_ms)
{
    if (reason <= LMUX_DIRTY_PARSE_NONE ||
        reason >= LMUX_DIRTY_PARSE_REASON_COUNT)
        reason = LMUX_DIRTY_PARSE_SEMANTIC;
    d->stats.invalid_records++;
    d->diagnostic.invalid_reason[reason]++;
    d->diagnostic.last_invalid_ms = now_ms;
    d->diagnostic.last_invalid_reason = reason;
    d->diagnostic.last_invalid_wire_len = (uint16_t)
        (LMUX_DIRTY_PREFIX_LEN + body_len + 2);
    d->diagnostic.last_invalid_offset = (uint16_t)
        (offset > UINT16_MAX ? UINT16_MAX : offset);
    d->diagnostic.last_invalid_wire_hash =
        diagnostic_record_hash(body, body_len);
    diagnostic_sample(d, body, body_len, offset);
}

static void process_record(struct lmux_dirty *d, const uint8_t *wire,
                           size_t wire_len, uint64_t now_ms)
{
    struct lmux_dirty_record r;
    uint8_t reason = LMUX_DIRTY_PARSE_NONE;
    int rc = lmux_dirty_record_decode_diagnostic(wire, wire_len, &r,
                                                 &reason);
    if (rc != 0) {
        note_invalid(d, reason, wire + LMUX_DIRTY_PREFIX_LEN,
                     wire_len - LMUX_DIRTY_PREFIX_LEN - 2, 0, now_ms);
        return;
    }
    d->stats.valid_records++;
    d->diagnostic.last_valid_ms = now_ms;
    d->diagnostic.last_valid_wire_len = (uint16_t)wire_len;
    d->diagnostic.last_valid_type = r.type;
    d->diagnostic.last_valid_session_id = r.session_id;
    d->diagnostic.last_valid_packet_seq = r.packet_seq;
    d->diagnostic.last_valid_wire_hash = diagnostic_hash_part(
        UINT32_C(2166136261), wire, wire_len);
    d->diagnostic.last_valid_fragment_index = r.fragment_index;
    d->diagnostic.last_valid_fragment_count = r.fragment_count;
    if (r.type == LMUX_DIRTY_DATA)
        handle_data(d, &r, now_ms);
    else
        handle_control(d, &r, now_ms);
}

static void foreign_sample_append(struct lmux_dirty *d, uint8_t ch)
{
    uint8_t *len = &d->diagnostic.last_foreign_sample_len;
    if (*len < LMUX_DIRTY_DIAG_SAMPLE_MAX)
        d->diagnostic.last_foreign_sample[(*len)++] = ch;
}

static void parser_outside(struct lmux_dirty *d, uint8_t ch, uint64_t now_ms)
{
    struct lmux_dirty_parser *p = &d->parser;
    const char *prefix = LMUX_DIRTY_PREFIX;
    if (ch == (uint8_t)prefix[p->prefix_match]) {
        p->prefix_match++;
        if (p->prefix_match == LMUX_DIRTY_PREFIX_LEN) {
            p->in_record = 1; p->prefix_match = 0;
            p->body_len = 0; p->saw_cr = 0; p->invalid = 0;
            p->invalid_reason = LMUX_DIRTY_PARSE_NONE;
            p->invalid_offset = 0;
            p->foreign_active = 0;
        }
    } else {
        unsigned matched = p->prefix_match;
        unsigned i;
        int restart = ch == (uint8_t)prefix[0];
        if (!p->foreign_active) {
            p->foreign_active = 1;
            d->diagnostic.foreign_runs++;
            d->diagnostic.last_foreign_sample_len = 0;
        }
        d->diagnostic.last_foreign_ms = now_ms;
        for (i = 0; i < matched; i++)
            foreign_sample_append(d, (uint8_t)prefix[i]);
        d->stats.foreign_bytes += matched;
        if (!restart) {
            d->stats.foreign_bytes++;
            foreign_sample_append(d, ch);
        }
        p->prefix_match = restart ? 1 : 0;
    }
}

int lmux_dirty_feed(struct lmux_dirty *d, const uint8_t *data, size_t len,
                    uint64_t now_ms)
{
    size_t i;
    if (!d || (!data && len)) { errno = EINVAL; return -1; }
    for (i = 0; i < len; i++) {
        struct lmux_dirty_parser *p = &d->parser;
        uint8_t ch = data[i];
        if (!p->in_record) { parser_outside(d, ch, now_ms); continue; }
        if (ch == '~') {
            note_invalid(d, LMUX_DIRTY_PARSE_RESTART_PREFIX,
                         (const uint8_t *)p->body, p->body_len,
                         p->body_len, now_ms);
            p->in_record = 0; p->prefix_match = 1;
            continue;
        }
        if (p->saw_cr) {
            if (ch == '\n' && !p->invalid) {
                uint8_t wire[LMUX_DIRTY_RECORD_MAX];
                size_t wire_len = LMUX_DIRTY_PREFIX_LEN + p->body_len + 2;
                memcpy(wire, LMUX_DIRTY_PREFIX, LMUX_DIRTY_PREFIX_LEN);
                memcpy(wire + LMUX_DIRTY_PREFIX_LEN, p->body, p->body_len);
                wire[wire_len - 2] = '\r'; wire[wire_len - 1] = '\n';
                process_record(d, wire, wire_len, now_ms);
            } else {
                note_invalid(d, p->invalid ? p->invalid_reason :
                             LMUX_DIRTY_PARSE_BAD_TERMINATOR,
                             (const uint8_t *)p->body, p->body_len,
                             p->invalid ? p->invalid_offset : p->body_len,
                             now_ms);
            }
            p->in_record = 0; p->body_len = 0; p->saw_cr = 0;
            p->invalid = 0; p->invalid_reason = LMUX_DIRTY_PARSE_NONE;
            p->prefix_match = 0;
            if (ch != '\n') parser_outside(d, ch, now_ms);
            continue;
        }
        if (ch == '\r') { p->saw_cr = 1; continue; }
        if (ch == '\n') {
            note_invalid(d, p->invalid ? p->invalid_reason :
                         LMUX_DIRTY_PARSE_EARLY_LF,
                         (const uint8_t *)p->body, p->body_len,
                         p->invalid ? p->invalid_offset : p->body_len,
                         now_ms);
            p->in_record = 0; p->body_len = 0; p->invalid = 0;
            p->invalid_reason = LMUX_DIRTY_PARSE_NONE;
            continue;
        }
        if (b64_value(ch) < 0 && ch != '=' && !p->invalid) {
            p->invalid = 1;
            p->invalid_reason = LMUX_DIRTY_PARSE_BAD_ALPHABET;
            p->invalid_offset = p->body_len;
        }
        if (p->body_len < LMUX_DIRTY_BASE64_MAX) {
            p->body[p->body_len++] = (char)ch;
        } else if (!p->invalid) {
            p->invalid = 1;
            p->invalid_reason = LMUX_DIRTY_PARSE_OVERSIZE;
            p->invalid_offset = p->body_len;
        }
    }
    return d->failed ? -1 : 0;
}

int lmux_dirty_tick(struct lmux_dirty *d, uint64_t now_ms)
{
    unsigned i;
    if (!d) { errno = EINVAL; return -1; }
    if (d->failed) { errno = EPIPE; return -1; }
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++) {
        struct lmux_dirty_tx_slot *s = &d->tx[i];
        if (!s->used) continue;
        if (now_ms >= s->deadline_ms) {
            set_failure(d, LMUX_DIRTY_FAIL_DEADLINE); break;
        }
        if (s->sent_bitmap == all_fragments(s->fragment_count) &&
            !s->resend_bitmap && now_ms >= s->retry_at_ms) {
            if (s->retry_rounds >= LMUX_DIRTY_RETRY_MAX) {
                set_failure(d, LMUX_DIRTY_FAIL_RETRY_EXHAUSTED); break;
            }
            s->retry_rounds++;
            s->resend_bitmap = all_fragments(s->fragment_count);
            s->retry_at_ms = now_ms + d->retry_interval_ms;
        }
    }
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX && !d->failed; i++) {
        struct lmux_dirty_rx_slot *s = &d->rx[i];
        uint8_t missing;
        if (!s->used || s->verified) continue;
        if (now_ms >= s->deadline_ms) {
            set_failure(d, LMUX_DIRTY_FAIL_DEADLINE); break;
        }
        if (now_ms < s->nack_at_ms) continue;
        if (s->nack_rounds >= LMUX_DIRTY_RETRY_MAX) {
            set_failure(d, LMUX_DIRTY_FAIL_RETRY_EXHAUSTED); break;
        }
        missing = all_fragments(s->fragment_count) & ~s->received_bitmap;
        s->nack_rounds++;
        queue_control(d, LMUX_DIRTY_NACK, d->peer_session_id, s->seq,
                      missing, 0);
        s->nack_at_ms = now_ms + d->retry_interval_ms;
    }
    return d->failed ? -1 : 0;
}

void lmux_dirty_peer_lost(struct lmux_dirty *d)
{
    if (d)
        set_failure(d, LMUX_DIRTY_FAIL_PEER);
}

static int build_control_output(struct lmux_dirty *d, unsigned index)
{
    struct lmux_dirty_control *c = &d->control[index];
    struct lmux_dirty_record r;
    size_t out_len;
    memset(&r, 0, sizeof(r));
    r.type = c->type; r.session_id = c->session_id; r.packet_seq = c->seq;
    r.missing_bitmap = c->bitmap; r.reason = c->reason;
    if (lmux_dirty_record_encode(&r, d->output, sizeof(d->output),
                                 &out_len) < 0)
        return -1;
    d->output_len = (uint16_t)out_len;
    d->output_pos = 0; d->output_kind = OUT_CONTROL;
    d->output_slot = (uint8_t)index;
    if (c->type == LMUX_DIRTY_ACK)
        d->stats.ack_sent++;
    else if (c->type == LMUX_DIRTY_NACK)
        d->stats.nack_sent++;
    return 0;
}

static int build_tx_output(struct lmux_dirty *d, unsigned index,
                           uint8_t bit, int resend)
{
    struct lmux_dirty_tx_slot *s = &d->tx[index];
    struct lmux_dirty_record r;
    unsigned fragment_index = 0;
    size_t offset, len, out_len;
    while (((uint8_t)(1u << fragment_index) & bit) == 0) fragment_index++;
    offset = fragment_index * LMUX_DIRTY_FRAGMENT_MAX;
    len = s->len - offset;
    if (len > LMUX_DIRTY_FRAGMENT_MAX) len = LMUX_DIRTY_FRAGMENT_MAX;
    memset(&r, 0, sizeof(r));
    r.type = LMUX_DIRTY_DATA; r.session_id = d->local_session_id;
    r.packet_seq = s->seq; r.packet_len = s->len;
    r.fragment_offset = (uint16_t)offset;
    r.fragment_index = (uint8_t)fragment_index;
    r.fragment_count = s->fragment_count; r.payload_len = (uint16_t)len;
    r.packet_crc = s->packet_crc;
    memcpy(r.payload, s->data + offset, len);
    r.fragment_crc = lmux_crc32c(r.payload, len);
    if (lmux_dirty_record_encode(&r, d->output, sizeof(d->output), &out_len) < 0)
        return -1;
    d->output_len = (uint16_t)out_len; d->output_pos = 0;
    d->output_kind = resend ? OUT_TX_RESEND : OUT_TX_INITIAL;
    d->output_slot = (uint8_t)index; d->output_bit = bit;
    return 0;
}

static int seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static int schedule_output(struct lmux_dirty *d)
{
    unsigned i, pass;
    int order[LMUX_DIRTY_WINDOW_MAX] = { -1, -1 };
    if (d->pending_fail) {
        struct lmux_dirty_record r;
        size_t out_len;
        memset(&r, 0, sizeof(r));
        r.type = LMUX_DIRTY_FAIL; r.session_id = d->local_session_id;
        r.reason = d->fail_reason;
        if (lmux_dirty_record_encode(&r, d->output, sizeof(d->output),
                                     &out_len) < 0)
            return -1;
        d->output_len = (uint16_t)out_len; d->output_pos = 0;
        d->output_kind = OUT_CONTROL; d->output_slot = UCHAR_MAX;
        return 0;
    }
    for (i = 0; i < LMUX_DIRTY_CONTROL_MAX; i++)
        if (d->control[i].used) return build_control_output(d, i);
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++) if (d->tx[i].used) {
        if (order[0] < 0 || seq_before(d->tx[i].seq, d->tx[order[0]].seq)) {
            order[1] = order[0]; order[0] = (int)i;
        } else order[1] = (int)i;
    }
    for (pass = 0; pass < LMUX_DIRTY_WINDOW_MAX; pass++) {
        struct lmux_dirty_tx_slot *s;
        uint8_t available, bit;
        if (order[pass] < 0) continue;
        s = &d->tx[order[pass]];
        available = s->resend_bitmap;
        if (available) {
            bit = available & (uint8_t)(-(int)available);
            return build_tx_output(d, (unsigned)order[pass], bit, 1);
        }
        available = all_fragments(s->fragment_count) & ~s->sent_bitmap;
        if (available) {
            bit = available & (uint8_t)(-(int)available);
            return build_tx_output(d, (unsigned)order[pass], bit, 0);
        }
    }
    return 0;
}

int lmux_dirty_output_peek(struct lmux_dirty *d, uint64_t now_ms,
                           const uint8_t **data, size_t *len)
{
    if (!d || !data || !len) { errno = EINVAL; return -1; }
    if (!d->failed) lmux_dirty_tick(d, now_ms);
    if (d->output_pos == d->output_len) {
        d->output_pos = d->output_len = 0; d->output_kind = OUT_NONE;
        if (schedule_output(d) < 0) return -1;
    }
    *data = d->output + d->output_pos;
    *len = d->output_len - d->output_pos;
    return *len ? 1 : 0;
}

int lmux_dirty_output_consume(struct lmux_dirty *d, size_t len)
{
    if (!d || len > (size_t)(d->output_len - d->output_pos)) {
        errno = EINVAL; return -1;
    }
    d->output_pos += (uint16_t)len;
    if (d->output_pos != d->output_len) return 0;
    if (d->output_kind == OUT_CONTROL) {
        if (d->output_slot == UCHAR_MAX)
            d->pending_fail = 0;
        else
            memset(&d->control[d->output_slot], 0,
                   sizeof(d->control[d->output_slot]));
    } else if (d->output_kind == OUT_TX_INITIAL) {
        if (d->tx[d->output_slot].acked)
            memset(&d->tx[d->output_slot], 0,
                   sizeof(d->tx[d->output_slot]));
        else
            d->tx[d->output_slot].sent_bitmap |= d->output_bit;
    } else if (d->output_kind == OUT_TX_RESEND) {
        if (d->tx[d->output_slot].acked)
            memset(&d->tx[d->output_slot], 0,
                   sizeof(d->tx[d->output_slot]));
        else
            d->tx[d->output_slot].resend_bitmap &= (uint8_t)~d->output_bit;
        d->stats.retransmitted_fragments++;
    }
    d->output_len = d->output_pos = 0; d->output_kind = OUT_NONE;
    return 0;
}

int lmux_dirty_receive_peek(const struct lmux_dirty *d,
                            const uint8_t **data, size_t *len,
                            uint32_t *packet_seq)
{
    unsigned i;
    if (!d || !data || !len) { errno = EINVAL; return -1; }
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++) {
        const struct lmux_dirty_rx_slot *s = &d->rx[i];
        if (s->used && s->verified && s->seq == d->rx_next_seq) {
            *data = s->data; *len = s->len;
            if (packet_seq) *packet_seq = s->seq;
            return 1;
        }
    }
    *data = NULL; *len = 0;
    return 0;
}

int lmux_dirty_receive_consume(struct lmux_dirty *d)
{
    unsigned i;
    if (!d) { errno = EINVAL; return -1; }
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++) {
        struct lmux_dirty_rx_slot *s = &d->rx[i];
        if (s->used && s->verified && s->seq == d->rx_next_seq) {
            if (queue_control(d, LMUX_DIRTY_ACK, d->peer_session_id,
                              s->seq, 0, 0) < 0)
                return -1;
            if (d->delivered_history < LMUX_DIRTY_WINDOW_MAX)
                d->delivered_history++;
            if (d->delivered_history > 1)
                d->last_delivered_seq[1] = d->last_delivered_seq[0];
            d->last_delivered_seq[0] = s->seq;
            memset(s, 0, sizeof(*s));
            d->rx_next_seq++;
            d->stats.delivered_packets++;
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

int lmux_dirty_failed(const struct lmux_dirty *d)
{
    return d && d->failed;
}

const char *lmux_dirty_fail_reason_name(uint8_t reason)
{
    switch (reason) {
    case LMUX_DIRTY_FAIL_NONE: return "none";
    case LMUX_DIRTY_FAIL_BAD_RECORD: return "bad-record";
    case LMUX_DIRTY_FAIL_RX_WINDOW: return "rx-window";
    case LMUX_DIRTY_FAIL_PACKET_CRC: return "packet-crc";
    case LMUX_DIRTY_FAIL_RETRY_EXHAUSTED: return "retry-exhausted";
    case LMUX_DIRTY_FAIL_DEADLINE: return "deadline";
    case LMUX_DIRTY_FAIL_PEER: return "peer-failure";
    case LMUX_DIRTY_FAIL_PROTOCOL: return "protocol";
    }
    return "unknown";
}

const char *lmux_dirty_parse_reason_name(uint8_t reason)
{
    switch (reason) {
    case LMUX_DIRTY_PARSE_NONE: return "none";
    case LMUX_DIRTY_PARSE_RESTART_PREFIX: return "restart-prefix";
    case LMUX_DIRTY_PARSE_BAD_TERMINATOR: return "bad-terminator";
    case LMUX_DIRTY_PARSE_EARLY_LF: return "early-lf";
    case LMUX_DIRTY_PARSE_BAD_ALPHABET: return "bad-alphabet";
    case LMUX_DIRTY_PARSE_OVERSIZE: return "oversize";
    case LMUX_DIRTY_PARSE_BASE64: return "base64";
    case LMUX_DIRTY_PARSE_CANONICAL: return "canonical";
    case LMUX_DIRTY_PARSE_HEADER: return "header";
    case LMUX_DIRTY_PARSE_LENGTH: return "length";
    case LMUX_DIRTY_PARSE_RECORD_CRC: return "record-crc";
    case LMUX_DIRTY_PARSE_SEMANTIC: return "semantic";
    case LMUX_DIRTY_PARSE_FRAGMENT_CRC: return "fragment-crc";
    }
    return "unknown";
}

size_t lmux_dirty_memory_footprint(void)
{
    return sizeof(struct lmux_dirty);
}
