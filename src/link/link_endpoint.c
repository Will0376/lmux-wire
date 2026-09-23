#include "link_endpoint.h"

#include <stdio.h>
#include <string.h>

static void set_error(struct lmux_link_endpoint *ep, const char *msg)
{
    snprintf(ep->last_error, sizeof(ep->last_error), "%s", msg);
}

static void ring_clear(struct lmux_link_ring *r)
{
    r->r = r->w = r->len = 0;
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

static void apply_peer_hello(struct lmux_link_endpoint *ep, uint64_t conn_id,
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
    if (ep->local_hello_sent && ep->peer_hello_accepted &&
        ep->state != LMUX_LINK_ESTABLISHED) {
        ep->state = LMUX_LINK_ESTABLISHED;
        if (events)
            *events |= LMUX_LINK_EV_ESTABLISHED;
    }
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
    memcpy(ep->last_error, err, sizeof(err));
}

void lmux_link_endpoint_begin(struct lmux_link_endpoint *ep,
                              uint64_t conn_id)
{
    struct lmux_link_frame fr;
    struct lmux_link_hello hello;
    uint32_t len = 0;
    lmux_link_endpoint_down(ep);
    ep->local_conn_id = conn_id ? conn_id : 1;
    ep->state = LMUX_LINK_WAIT_HELLO;
    ep->next_tx_seq = 1;
    ep->expected_rx_seq = 0;
    ep->pending_peer_hello.valid = 0;

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
        return;
    fr.payload_len = len;
    if (lmux_link_encode_frame(&fr, ep->out, sizeof(ep->out), &ep->out_len) == 0)
        ep->local_hello_sent = 1;
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

int lmux_link_has_pending_output(const struct lmux_link_endpoint *ep)
{
    return ep->out_pos < ep->out_len;
}

int lmux_link_wants_physical_write(const struct lmux_link_endpoint *ep)
{
    return lmux_link_has_pending_output(ep) || ep->tx_plain.len > 0;
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
    if (ep->out_pos < ep->out_len)
        return 0;
    ep->out_pos = ep->out_len = 0;
    if (ep->state != LMUX_LINK_ESTABLISHED || ep->tx_plain.len == 0)
        return 0;
    chunk = ep->effective_transport_payload;
    if (chunk == 0 || chunk > LMUX_LINK_TRANSPORT_TARGET)
        chunk = LMUX_LINK_TRANSPORT_TARGET;
    if (chunk > ep->tx_plain.len)
        chunk = ep->tx_plain.len;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX_LINK_T_TRANSPORT;
    fr.header_len = LMUX_LINK_HEADER_LEN;
    fr.conn_id = ep->local_conn_id;
    fr.frame_seq = ep->next_tx_seq++;
    fr.payload_len = (uint32_t)ring_read(&ep->tx_plain, fr.payload, chunk);
    if (lmux_link_encode_frame(&fr, ep->out, sizeof(ep->out), &ep->out_len) < 0) {
        ep->state = LMUX_LINK_ERROR_RESETTING;
        set_error(ep, "encode");
        return -1;
    }
    ep->tx_frames++;
    ep->transport_tx_bytes += fr.payload_len;
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
    if (n > ep->out_len - ep->out_pos)
        n = ep->out_len - ep->out_pos;
    ep->out_pos += n;
    if (ep->out_pos == ep->out_len)
        ep->out_pos = ep->out_len = 0;
}

static void fail_session(struct lmux_link_endpoint *ep, unsigned *events,
                         const char *reason)
{
    ep->state = LMUX_LINK_ERROR_RESETTING;
    ep->protocol_errors++;
    set_error(ep, reason);
    ring_clear(&ep->tx_plain);
    ring_clear(&ep->rx_plain);
    ep->out_len = ep->out_pos = 0;
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
        if (fr->conn_id == ep->peer_conn_id)
            return 0;
        ep->pending_peer_hello.valid = 1;
        ep->pending_peer_hello.conn_id = fr->conn_id;
        ep->pending_peer_hello.hello = h;
        ep->state = LMUX_LINK_ERROR_RESETTING;
        set_error(ep, "peer restart");
        ring_clear(&ep->tx_plain);
        ring_clear(&ep->rx_plain);
        ep->out_len = ep->out_pos = 0;
        *events |= LMUX_LINK_EV_PEER_RESTART | LMUX_LINK_EV_DOWN;
        return 0;
    }
    apply_peer_hello(ep, fr->conn_id, &h, events);
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
    apply_peer_hello(ep, pending.conn_id, &pending.hello, events);
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
        if (fr->flags & LMUX_LINK_F_CRITICAL_EXTENSION)
            fail_session(ep, events, "unsupported critical boot");
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
