#include "link_endpoint.h"

#include <stdio.h>
#include <string.h>

#define LMUX_LINK_HELLO_RETRY_MS 250

static void set_error(struct lmux_link_endpoint *ep, const char *msg)
{
    snprintf(ep->last_error, sizeof(ep->last_error), "%s", msg);
}

static void ring_clear(struct lmux_link_ring *r)
{
    r->r = r->w = r->len = 0;
}

static void boot_queue_clear(struct lmux_link_boot_queue *q)
{
    q->r = q->w = q->len = 0;
}

static unsigned boot_queue_space(const struct lmux_link_boot_queue *q)
{
    return LMUX_LINK_BOOT_QUEUE_DEPTH - q->len;
}

static int boot_queue_push(struct lmux_link_boot_queue *q, uint8_t type,
                           const uint8_t *payload, uint32_t len)
{
    struct lmux_link_boot_msg *m;
    if (q->len >= LMUX_LINK_BOOT_QUEUE_DEPTH ||
        len > LMUX_LINK_MAX_PAYLOAD || (len && !payload))
        return -1;
    m = &q->msg[q->w];
    m->type = type;
    m->len = len;
    if (len)
        memcpy(m->payload, payload, len);
    q->w = (q->w + 1) % LMUX_LINK_BOOT_QUEUE_DEPTH;
    q->len++;
    return 0;
}

static int boot_queue_pop(struct lmux_link_boot_queue *q, uint8_t *type,
                          uint8_t *payload, size_t cap, uint32_t *len)
{
    struct lmux_link_boot_msg *m;
    if (q->len == 0 || !type || !len)
        return -1;
    m = &q->msg[q->r];
    if (m->len > cap || (m->len && !payload))
        return -1;
    *type = m->type;
    *len = m->len;
    if (m->len)
        memcpy(payload, m->payload, m->len);
    q->r = (q->r + 1) % LMUX_LINK_BOOT_QUEUE_DEPTH;
    q->len--;
    if (q->len == 0)
        q->r = q->w = 0;
    return 0;
}

static size_t ring_space(const struct lmux_link_ring *r)
{
    return sizeof(r->buf) - r->len;
}

static size_t ring_write(struct lmux_link_ring *r, const uint8_t *buf, size_t len)
{
    size_t done = 0;
    if (len > ring_space(r))
        len = ring_space(r);
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

static size_t ring_read(struct lmux_link_ring *r, uint8_t *buf, size_t len)
{
    size_t done = 0;
    if (len > r->len)
        len = r->len;
    while (done < len) {
        size_t chunk = sizeof(r->buf) - r->r;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(buf + done, r->buf + r->r, chunk);
        r->r = (r->r + chunk) % sizeof(r->buf);
        r->len -= chunk;
        done += chunk;
    }
    if (r->len == 0)
        r->r = r->w = 0;
    return done;
}

static int min_u32(uint32_t a, uint32_t b)
{
    return a < b ? (int)a : (int)b;
}

static int encode_local_hello(struct lmux_link_endpoint *ep)
{
    struct lmux_link_frame fr;
    struct lmux_link_hello hello;
    uint32_t len = 0;

    memset(&hello, 0, sizeof(hello));
    hello.role = ep->local_role;
    hello.capabilities = ep->local_caps;
    hello.max_outer_payload = ep->local_max_outer_payload;
    hello.max_transport_payload = ep->local_max_transport_payload;
    hello.preferred_transport_payload = ep->local_preferred_transport_payload;

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX_LINK_T_HELLO;
    fr.header_len = LMUX_LINK_HEADER_LEN;
    fr.conn_id = ep->local_conn_id;
    fr.frame_seq = 0;
    if (lmux_link_make_hello_payload(&hello, fr.payload, sizeof(fr.payload), &len) < 0)
        return -1;
    fr.payload_len = len;

    ep->out_len = ep->out_pos = 0;
    if (lmux_link_encode_frame(&fr, ep->out, sizeof(ep->out), &ep->out_len) < 0)
        return -1;
    ep->local_hello_sent = 1;
    ep->pending_hello = 0;
    ep->next_hello_retry_ms = 0;
    return 0;
}

static int queue_local_hello(struct lmux_link_endpoint *ep)
{
    if (ep->out_pos < ep->out_len) {
        ep->pending_hello = 1;
        return 0;
    }
    return encode_local_hello(ep);
}

static int encode_hello_ack(struct lmux_link_endpoint *ep, uint64_t peer_conn_id)
{
    struct lmux_link_frame fr;
    uint32_t len = 0;

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX_LINK_T_HELLO_ACK;
    fr.header_len = LMUX_LINK_HEADER_LEN;
    fr.conn_id = ep->local_conn_id;
    fr.frame_seq = 0;
    if (lmux_link_make_hello_ack_payload(peer_conn_id, fr.payload,
                                         sizeof(fr.payload), &len) < 0)
        return -1;
    fr.payload_len = len;

    ep->out_len = ep->out_pos = 0;
    if (lmux_link_encode_frame(&fr, ep->out, sizeof(ep->out), &ep->out_len) < 0)
        return -1;
    ep->pending_ack = 0;
    ep->out_is_ack = 1;
    ep->out_ack_peer_conn_id = peer_conn_id;
    return 0;
}

static int queue_hello_ack(struct lmux_link_endpoint *ep, uint64_t peer_conn_id)
{
    if ((ep->pending_ack && ep->pending_ack_peer_conn_id == peer_conn_id) ||
        (ep->out_is_ack && ep->out_ack_peer_conn_id == peer_conn_id))
        return 0;
    ep->pending_ack = 1;
    ep->pending_ack_peer_conn_id = peer_conn_id;
    if (ep->out_pos < ep->out_len)
        return 1;
    return encode_hello_ack(ep, peer_conn_id) < 0 ? -1 : 1;
}

static int build_pending_handshake_output(struct lmux_link_endpoint *ep)
{
    if (ep->out_pos < ep->out_len)
        return 0;
    ep->out_pos = ep->out_len = 0;
    ep->out_is_ack = 0;
    ep->out_ack_peer_conn_id = 0;
    if (ep->pending_ack)
        return encode_hello_ack(ep, ep->pending_ack_peer_conn_id);
    if (ep->pending_hello)
        return encode_local_hello(ep);
    return 0;
}

static void maybe_establish(struct lmux_link_endpoint *ep, unsigned *events)
{
    if (ep->state == LMUX_LINK_WAIT_HELLO &&
        ep->peer_hello_accepted &&
        ep->local_ack_committed &&
        ep->peer_ack_received) {
        ep->state = LMUX_LINK_ESTABLISHED;
        if (events)
            *events |= LMUX_LINK_EV_ESTABLISHED;
    }
}

static int apply_peer_hello(struct lmux_link_endpoint *ep, uint64_t conn_id,
                            const struct lmux_link_hello *h,
                            unsigned *events)
{
    uint32_t eff_outer, eff_transport;
    ep->peer_role = h->role;
    ep->peer_conn_id = conn_id;
    ep->peer_caps = h->capabilities;
    ep->peer_max_outer_payload = h->max_outer_payload;
    ep->peer_max_transport_payload = h->max_transport_payload;
    ep->expected_rx_seq = 1;
    ep->peer_hello_accepted = 1;

    eff_outer = (uint32_t)min_u32(ep->local_max_outer_payload,
                                  ep->peer_max_outer_payload);
    if (eff_outer > LMUX_LINK_MAX_PAYLOAD)
        eff_outer = LMUX_LINK_MAX_PAYLOAD;
    eff_transport = (uint32_t)min_u32(ep->local_max_transport_payload,
                                      ep->peer_max_transport_payload);
    if (eff_transport > eff_outer)
        eff_transport = eff_outer;
    if (eff_transport == 0)
        eff_transport = LMUX_LINK_TRANSPORT_TARGET;
    ep->effective_transport_payload = eff_transport;
    ep->negotiated_caps = ep->local_caps & ep->peer_caps;
    if (events)
        *events |= LMUX_LINK_EV_PEER_HELLO;
    {
        int queued = queue_hello_ack(ep, conn_id);
        if (queued < 0) {
            ep->state = LMUX_LINK_ERROR_RESETTING;
            ep->protocol_errors++;
            set_error(ep, "hello ack encode");
            if (events)
                *events |= LMUX_LINK_EV_DOWN;
            return -1;
        }
        if (queued && events)
            *events |= LMUX_LINK_EV_HELLO_ACK_QUEUED;
    }
    maybe_establish(ep, events);
    return 0;
}

void lmux_link_endpoint_init(struct lmux_link_endpoint *ep,
                             enum lmux_link_role role)
{
    memset(ep, 0, sizeof(*ep));
    ep->local_role = role;
    ep->state = LMUX_LINK_DOWN;
    ep->local_caps = LMUX_LINK_CAP_TRANSPORT;
    ep->local_max_outer_payload = LMUX_LINK_MAX_PAYLOAD;
    ep->local_max_transport_payload = LMUX_LINK_TRANSPORT_TARGET;
    ep->local_preferred_transport_payload = LMUX_LINK_TRANSPORT_TARGET;
    ep->effective_transport_payload = LMUX_LINK_TRANSPORT_TARGET;
}

void lmux_link_endpoint_down(struct lmux_link_endpoint *ep)
{
    uint64_t local = ep->local_conn_id;
    enum lmux_link_role role = ep->local_role;
    uint32_t caps = ep->local_caps;
    uint32_t max_outer = ep->local_max_outer_payload;
    uint32_t max_transport = ep->local_max_transport_payload;
    uint32_t preferred = ep->local_preferred_transport_payload;
    unsigned long long tx_frames = ep->tx_frames;
    unsigned long long rx_frames = ep->rx_frames;
    unsigned long long crc_errors = ep->crc_errors;
    unsigned long long seq_errors = ep->seq_errors;
    unsigned long long resync_drops = ep->resync_drops;
    unsigned long long protocol_errors = ep->protocol_errors;
    unsigned long long ttx = ep->transport_tx_bytes;
    unsigned long long trx = ep->transport_rx_bytes;
    unsigned long long btf = ep->boot_tx_frames;
    unsigned long long brf = ep->boot_rx_frames;
    unsigned long long btb = ep->boot_tx_bytes;
    unsigned long long brb = ep->boot_rx_bytes;
    char err[sizeof(ep->last_error)];
    memcpy(err, ep->last_error, sizeof(err));
    memset(ep, 0, sizeof(*ep));
    ep->local_role = role;
    ep->state = LMUX_LINK_DOWN;
    ep->local_conn_id = local;
    ep->local_caps = caps;
    ep->local_max_outer_payload = max_outer;
    ep->local_max_transport_payload = max_transport;
    ep->local_preferred_transport_payload = preferred;
    ep->effective_transport_payload = preferred ? preferred : LMUX_LINK_TRANSPORT_TARGET;
    ep->tx_frames = tx_frames;
    ep->rx_frames = rx_frames;
    ep->crc_errors = crc_errors;
    ep->seq_errors = seq_errors;
    ep->resync_drops = resync_drops;
    ep->protocol_errors = protocol_errors;
    ep->transport_tx_bytes = ttx;
    ep->transport_rx_bytes = trx;
    ep->boot_tx_frames = btf;
    ep->boot_rx_frames = brf;
    ep->boot_tx_bytes = btb;
    ep->boot_rx_bytes = brb;
    memcpy(ep->last_error, err, sizeof(err));
}

void lmux_link_endpoint_begin(struct lmux_link_endpoint *ep,
                              uint64_t conn_id)
{
    lmux_link_endpoint_down(ep);
    ep->local_conn_id = conn_id ? conn_id : 1;
    ep->state = LMUX_LINK_WAIT_HELLO;
    ep->next_tx_seq = 1;
    ep->expected_rx_seq = 0;
    ep->pending_peer_hello.valid = 0;
    ep->next_hello_retry_ms = 0;

    if (queue_local_hello(ep) < 0)
        set_error(ep, "hello encode");
}

size_t lmux_link_transport_write(struct lmux_link_endpoint *ep,
                                 const uint8_t *buf, size_t len)
{
    if (ep->state != LMUX_LINK_ESTABLISHED)
        return 0;
    return ring_write(&ep->tx_plain, buf, len);
}

size_t lmux_link_transport_read(struct lmux_link_endpoint *ep,
                                uint8_t *buf, size_t len)
{
    return ring_read(&ep->rx_plain, buf, len);
}

size_t lmux_link_transport_tx_len(const struct lmux_link_endpoint *ep)
{
    return ep->tx_plain.len;
}

size_t lmux_link_transport_tx_space(const struct lmux_link_endpoint *ep)
{
    return ring_space(&ep->tx_plain);
}

size_t lmux_link_transport_rx_len(const struct lmux_link_endpoint *ep)
{
    return ep->rx_plain.len;
}

size_t lmux_link_transport_rx_space(const struct lmux_link_endpoint *ep)
{
    return ring_space(&ep->rx_plain);
}

int lmux_link_boot_queue_frame(struct lmux_link_endpoint *ep, uint8_t type,
                               const uint8_t *payload, uint32_t len)
{
    if (ep->state != LMUX_LINK_ESTABLISHED ||
        type < LMUX_LINK_T_BOOT_REQUEST || type > LMUX_LINK_T_BOOT_EVENT)
        return -1;
    return boot_queue_push(&ep->tx_boot, type, payload, len);
}

int lmux_link_boot_read_frame(struct lmux_link_endpoint *ep, uint8_t *type,
                              uint8_t *payload, size_t cap, uint32_t *len)
{
    return boot_queue_pop(&ep->rx_boot, type, payload, cap, len);
}

unsigned lmux_link_boot_tx_len(const struct lmux_link_endpoint *ep)
{
    return ep->tx_boot.len;
}

unsigned lmux_link_boot_tx_space(const struct lmux_link_endpoint *ep)
{
    return boot_queue_space(&ep->tx_boot);
}

unsigned lmux_link_boot_rx_len(const struct lmux_link_endpoint *ep)
{
    return ep->rx_boot.len;
}

unsigned lmux_link_boot_rx_space(const struct lmux_link_endpoint *ep)
{
    return boot_queue_space(&ep->rx_boot);
}

int lmux_link_has_pending_output(const struct lmux_link_endpoint *ep)
{
    return ep->out_pos < ep->out_len;
}

int lmux_link_wants_physical_write(const struct lmux_link_endpoint *ep)
{
    return lmux_link_has_pending_output(ep) ||
           ep->pending_ack || ep->pending_hello ||
           ep->tx_plain.len > 0 || ep->tx_boot.len > 0;
}

int lmux_link_can_accept_physical(const struct lmux_link_endpoint *ep)
{
    if (ep->discard_until_delim)
        return 1;
    return ep->enc_in_len < sizeof(ep->enc_in);
}

int lmux_link_build_output(struct lmux_link_endpoint *ep)
{
    struct lmux_link_frame fr;
    size_t chunk;
    int send_boot;
    if (ep->out_pos < ep->out_len)
        return 0;
    if (ep->pending_ack || ep->pending_hello)
        return build_pending_handshake_output(ep);
    ep->out_pos = ep->out_len = 0;
    if (ep->state != LMUX_LINK_ESTABLISHED ||
        (ep->tx_plain.len == 0 && ep->tx_boot.len == 0))
        return 0;
    memset(&fr, 0, sizeof(fr));
    fr.header_len = LMUX_LINK_HEADER_LEN;
    fr.conn_id = ep->local_conn_id;
    fr.frame_seq = ep->next_tx_seq++;
    send_boot = ep->tx_boot.len > 0 &&
        (ep->tx_plain.len == 0 || ep->prefer_boot_tx);
    if (send_boot) {
        const struct lmux_link_boot_msg *m = &ep->tx_boot.msg[ep->tx_boot.r];
        fr.type = m->type;
        fr.payload_len = m->len;
        if (m->len)
            memcpy(fr.payload, m->payload, m->len);
    } else {
        chunk = ep->effective_transport_payload;
        if (chunk == 0 || chunk > LMUX_LINK_TRANSPORT_TARGET)
            chunk = LMUX_LINK_TRANSPORT_TARGET;
        if (chunk > ep->tx_plain.len)
            chunk = ep->tx_plain.len;
        fr.type = LMUX_LINK_T_TRANSPORT;
        fr.payload_len = (uint32_t)ring_read(&ep->tx_plain, fr.payload, chunk);
    }
    if (lmux_link_encode_frame(&fr, ep->out, sizeof(ep->out), &ep->out_len) < 0) {
        if (!send_boot && fr.payload_len)
            (void)ring_write(&ep->tx_plain, fr.payload, fr.payload_len);
        ep->next_tx_seq--;
        ep->state = LMUX_LINK_ERROR_RESETTING;
        set_error(ep, "encode");
        return -1;
    }
    if (send_boot) {
        ep->tx_boot.r = (ep->tx_boot.r + 1) % LMUX_LINK_BOOT_QUEUE_DEPTH;
        ep->tx_boot.len--;
        if (ep->tx_boot.len == 0)
            ep->tx_boot.r = ep->tx_boot.w = 0;
        ep->boot_tx_frames++;
        ep->boot_tx_bytes += fr.payload_len;
        ep->prefer_boot_tx = 0;
    } else {
        ep->transport_tx_bytes += fr.payload_len;
        ep->prefer_boot_tx = 1;
    }
    ep->tx_frames++;
    return 1;
}

size_t lmux_link_output_len(const struct lmux_link_endpoint *ep)
{
    return ep->out_len - ep->out_pos;
}

const uint8_t *lmux_link_output_ptr(const struct lmux_link_endpoint *ep)
{
    return ep->out + ep->out_pos;
}

void lmux_link_output_consume(struct lmux_link_endpoint *ep, size_t n)
{
    lmux_link_output_consume_events(ep, n, NULL);
}

void lmux_link_output_consume_events(struct lmux_link_endpoint *ep, size_t n,
                                     unsigned *events)
{
    if (n > ep->out_len - ep->out_pos)
        n = ep->out_len - ep->out_pos;
    ep->out_pos += n;
    if (ep->out_pos == ep->out_len) {
        ep->out_pos = ep->out_len = 0;
        if (ep->out_is_ack) {
            ep->out_is_ack = 0;
            if (ep->out_ack_peer_conn_id == ep->peer_conn_id) {
                ep->local_ack_committed = 1;
                if (events)
                    *events |= LMUX_LINK_EV_HELLO_ACK_COMMITTED;
                maybe_establish(ep, events);
            }
            ep->out_ack_peer_conn_id = 0;
        }
    }
}

int lmux_link_endpoint_tick(struct lmux_link_endpoint *ep, long long now_ms)
{
    if (ep->state != LMUX_LINK_WAIT_HELLO || ep->out_pos < ep->out_len ||
        ep->pending_ack || ep->pending_hello)
        return 0;
    if (!ep->next_hello_retry_ms) {
        ep->next_hello_retry_ms = now_ms + LMUX_LINK_HELLO_RETRY_MS;
        return 0;
    }
    if (now_ms < ep->next_hello_retry_ms)
        return 0;
    if (queue_local_hello(ep) < 0) {
        ep->state = LMUX_LINK_ERROR_RESETTING;
        ep->protocol_errors++;
        set_error(ep, "hello retry encode");
        return -1;
    }
    ep->hello_retry_count++;
    ep->next_hello_retry_ms = now_ms + LMUX_LINK_HELLO_RETRY_MS;
    return 1;
}

static void fail_session(struct lmux_link_endpoint *ep, unsigned *events,
                         const char *reason)
{
    ep->state = LMUX_LINK_ERROR_RESETTING;
    ep->protocol_errors++;
    set_error(ep, reason);
    ring_clear(&ep->tx_plain);
    ring_clear(&ep->rx_plain);
    boot_queue_clear(&ep->tx_boot);
    boot_queue_clear(&ep->rx_boot);
    ep->out_len = ep->out_pos = 0;
    ep->pending_hello = 0;
    ep->pending_ack = 0;
    ep->pending_ack_peer_conn_id = 0;
    ep->out_is_ack = 0;
    ep->out_ack_peer_conn_id = 0;
    if (events)
        *events |= LMUX_LINK_EV_DOWN;
}

static int accept_hello(struct lmux_link_endpoint *ep,
                        const struct lmux_link_frame *fr,
                        unsigned *events)
{
    struct lmux_link_hello h;
    if (fr->frame_seq != 0 || fr->conn_id == 0 ||
        lmux_link_parse_hello_payload(fr->payload, fr->payload_len, &h) < 0 ||
        h.role == ep->local_role) {
        if (ep->state == LMUX_LINK_ESTABLISHED)
            fail_session(ep, events, "bad hello");
        else
            ep->resync_drops++;
        return -1;
    }
    if (ep->state == LMUX_LINK_ESTABLISHED) {
        if (fr->conn_id == ep->peer_conn_id) {
            int queued;
            if (events)
                *events |= LMUX_LINK_EV_PEER_HELLO;
            queued = queue_hello_ack(ep, fr->conn_id);
            if (queued < 0) {
                fail_session(ep, events, "hello ack encode");
                return -1;
            }
            if (queued && events)
                *events |= LMUX_LINK_EV_HELLO_ACK_QUEUED;
            return 0;
        }
        ep->pending_peer_hello.valid = 1;
        ep->pending_peer_hello.conn_id = fr->conn_id;
        ep->pending_peer_hello.hello = h;
        ep->state = LMUX_LINK_ERROR_RESETTING;
        set_error(ep, "peer restart");
        ring_clear(&ep->tx_plain);
        ring_clear(&ep->rx_plain);
        boot_queue_clear(&ep->tx_boot);
        boot_queue_clear(&ep->rx_boot);
        ep->out_len = ep->out_pos = 0;
        ep->pending_hello = 0;
        ep->pending_ack = 0;
        ep->pending_ack_peer_conn_id = 0;
        ep->out_is_ack = 0;
        ep->out_ack_peer_conn_id = 0;
        *events |= LMUX_LINK_EV_PEER_RESTART | LMUX_LINK_EV_DOWN;
        return 0;
    }
    if (ep->peer_hello_accepted && fr->conn_id != ep->peer_conn_id) {
        ep->peer_ack_received = 0;
        ep->local_ack_committed = 0;
        ep->pending_ack = 0;
        ep->out_is_ack = 0;
        ep->out_ack_peer_conn_id = 0;
    }
    return apply_peer_hello(ep, fr->conn_id, &h, events);
}

static int accept_hello_ack(struct lmux_link_endpoint *ep,
                            const struct lmux_link_frame *fr,
                            unsigned *events)
{
    uint64_t ack_peer_conn_id = 0;
    if (fr->frame_seq != 0 || fr->conn_id == 0 ||
        lmux_link_parse_hello_ack_payload(fr->payload, fr->payload_len,
                                          &ack_peer_conn_id) < 0) {
        if (ep->state == LMUX_LINK_ESTABLISHED)
            fail_session(ep, events, "bad hello ack");
        else
            ep->resync_drops++;
        return -1;
    }
    if (!ep->peer_hello_accepted ||
        fr->conn_id != ep->peer_conn_id ||
        ack_peer_conn_id != ep->local_conn_id)
        return 0;
    ep->peer_ack_received = 1;
    if (events)
        *events |= LMUX_LINK_EV_PEER_HELLO_ACK;
    maybe_establish(ep, events);
    return 0;
}

int lmux_link_endpoint_restart_from_pending(struct lmux_link_endpoint *ep,
                                            uint64_t new_local_conn_id,
                                            unsigned *events)
{
    struct lmux_link_pending_hello pending = ep->pending_peer_hello;
    if (events)
        *events = 0;
    if (!pending.valid || new_local_conn_id == 0)
        return -1;
    lmux_link_endpoint_begin(ep, new_local_conn_id);
    if (!ep->local_hello_sent)
        return -1;
    if (apply_peer_hello(ep, pending.conn_id, &pending.hello, events) < 0)
        return -1;
    ep->pending_peer_hello.valid = 0;
    return 0;
}

static void handle_frame(struct lmux_link_endpoint *ep,
                         const struct lmux_link_frame *fr,
                         unsigned *events)
{
    if (fr->type == LMUX_LINK_T_HELLO) {
        (void)accept_hello(ep, fr, events);
        return;
    }
    if (fr->type == LMUX_LINK_T_HELLO_ACK) {
        (void)accept_hello_ack(ep, fr, events);
        return;
    }
    if (ep->state != LMUX_LINK_ESTABLISHED) {
        ep->resync_drops++;
        return;
    }
    if (fr->conn_id != ep->peer_conn_id || fr->frame_seq != ep->expected_rx_seq) {
        ep->seq_errors++;
        fail_session(ep, events, "seq");
        return;
    }
    ep->expected_rx_seq++;
    ep->rx_frames++;
    if (fr->type == LMUX_LINK_T_TRANSPORT) {
        if (fr->payload_len > ring_space(&ep->rx_plain)) {
            fail_session(ep, events, "transport rx full");
            return;
        }
        (void)ring_write(&ep->rx_plain, fr->payload, fr->payload_len);
        ep->transport_rx_bytes += fr->payload_len;
        return;
    }
    if (fr->type >= LMUX_LINK_T_BOOT_REQUEST &&
        fr->type <= LMUX_LINK_T_BOOT_EVENT) {
        if (fr->flags & LMUX_LINK_F_CRITICAL_EXTENSION) {
            fail_session(ep, events, "unsupported critical boot");
            return;
        }
        if (boot_queue_push(&ep->rx_boot, fr->type, fr->payload,
                            fr->payload_len) < 0) {
            fail_session(ep, events, "boot rx full");
            return;
        }
        ep->boot_rx_frames++;
        ep->boot_rx_bytes += fr->payload_len;
        return;
    }
    if (fr->flags & LMUX_LINK_F_CRITICAL_EXTENSION)
        fail_session(ep, events, "unknown critical");
}

int lmux_link_input_bytes(struct lmux_link_endpoint *ep,
                          const uint8_t *buf, size_t len,
                          unsigned *events)
{
    size_t i;
    if (events)
        *events = 0;
    for (i = 0; i < len; i++) {
        uint8_t ch = buf[i];
        if (ch == 0) {
            if (!ep->discard_until_delim && ep->enc_in_len) {
                struct lmux_link_frame fr;
                if (lmux_link_decode_frame(ep->enc_in, ep->enc_in_len, &fr) == 0) {
                    handle_frame(ep, &fr, events);
                } else if (ep->state == LMUX_LINK_ESTABLISHED) {
                    ep->crc_errors++;
                    fail_session(ep, events, "decode");
                } else {
                    ep->resync_drops++;
                }
            }
            ep->enc_in_len = 0;
            ep->discard_until_delim = 0;
            continue;
        }
        if (ep->discard_until_delim)
            continue;
        if (ep->enc_in_len >= sizeof(ep->enc_in)) {
            ep->discard_until_delim = 1;
            ep->enc_in_len = 0;
            if (ep->state == LMUX_LINK_ESTABLISHED)
                fail_session(ep, events, "oversize");
            else
                ep->resync_drops++;
            continue;
        }
        ep->enc_in[ep->enc_in_len++] = ch;
    }
    return 0;
}

const char *lmux_link_state_name(enum lmux_link_state st)
{
    switch (st) {
    case LMUX_LINK_DOWN: return "DOWN";
    case LMUX_LINK_WAIT_HELLO: return "WAIT_HELLO";
    case LMUX_LINK_ESTABLISHED: return "ESTABLISHED";
    case LMUX_LINK_ERROR_RESETTING: return "ERROR_RESETTING";
    }
    return "?";
}
