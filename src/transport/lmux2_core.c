#include "lmux2_core.h"

#include <stdio.h>
#include <string.h>

static void ring_clear(struct lmux2_ring *r)
{
    r->r = r->w = r->len = 0;
}

static size_t ring_free(const struct lmux2_ring *r)
{
    return sizeof(r->buf) - r->len;
}

static size_t ring_peek_linear(const struct lmux2_ring *r, const uint8_t **p)
{
    *p = r->buf + r->r;
    if (r->len == 0)
        return 0;
    if (r->r + r->len <= sizeof(r->buf))
        return r->len;
    return sizeof(r->buf) - r->r;
}

static void ring_drop(struct lmux2_ring *r, size_t n)
{
    if (n > r->len)
        n = r->len;
    r->r = (r->r + n) % sizeof(r->buf);
    r->len -= n;
    if (r->len == 0)
        r->r = r->w = 0;
}

static size_t ring_write(struct lmux2_ring *r, const uint8_t *buf, size_t n)
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

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (UINT64_MAX - a < b)
        return -1;
    *out = a + b;
    return 0;
}

static int stream_id_is_host(uint64_t id)
{
    return id != 0 && (id & 1u);
}

static int local_stream_id_ok(const struct lmux2_conn *c, uint64_t id)
{
    return c->role == LMUX2_ROLE_HOST ? stream_id_is_host(id) : (id != 0 && !stream_id_is_host(id));
}

static int peer_stream_id_ok(const struct lmux2_conn *c, uint64_t id)
{
    return id != 0 && !local_stream_id_ok(c, id);
}

static void set_conn_error(struct lmux2_conn *c, const char *why)
{
    c->state = LMUX2_CONN_ERROR;
    c->protocol_errors++;
    snprintf(c->last_error, sizeof(c->last_error), "%s", why);
}

static void set_stream_error(struct lmux2_conn *c, struct lmux2_stream *s,
                             uint32_t code, const char *why)
{
    c->stream_errors++;
    if (why)
        snprintf(c->last_error, sizeof(c->last_error), "%s", why);
    if (s) {
        s->local_error = code;
        s->state = LMUX2_STREAM_RESET;
    }
}

static int queue_frame(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    size_t pos;
    if (c->outq_len >= LMUX2_OUTQ_CAP) {
        set_conn_error(c, "output queue full");
        return -1;
    }
    pos = (c->outq_r + c->outq_len) % LMUX2_OUTQ_CAP;
    c->outq[pos] = *fr;
    c->outq_len++;
    return 0;
}

int lmux2_queue_frame(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    if (!c || !fr || c->state == LMUX2_CONN_ERROR)
        return -1;
    return queue_frame(c, fr);
}

static int queue_simple(struct lmux2_conn *c, uint8_t type, uint64_t stream_id,
                        uint64_t value)
{
    struct lmux2_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.type = type;
    fr.stream_id = stream_id;
    fr.value = value;
    return queue_frame(c, &fr);
}

static int queue_hello(struct lmux2_conn *c)
{
    struct lmux2_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_HELLO;
    if (lmux2_make_hello_payload(c->role, c->local_conn_id, LMUX2_CORE_MAX_STREAMS,
                                 LMUX2_STREAM_RX_CAP, LMUX2_CONN_RX_CAP,
                                 fr.payload, &fr.payload_len) < 0) {
        set_conn_error(c, "bad local HELLO");
        return -1;
    }
    c->hello_sent = 1;
    return queue_frame(c, &fr);
}

static void reset_stream(struct lmux2_stream *s)
{
    memset(s, 0, sizeof(*s));
    s->state = LMUX2_STREAM_UNUSED;
}

static int stream_slot_reclaimable(const struct lmux2_stream *s)
{
    if (s->state == LMUX2_STREAM_UNUSED)
        return 1;
    if (s->state == LMUX2_STREAM_TERMINAL && s->rx.len == 0 && s->tx.len == 0)
        return 1;
    if (s->state == LMUX2_STREAM_RESET && s->rx.len == 0 && s->tx.len == 0)
        return 1;
    if (s->local_close_sent && s->rx.len == 0 && s->tx.len == 0 &&
        (s->state == LMUX2_STREAM_LOCAL_CLOSED ||
         s->state == LMUX2_STREAM_REMOTE_CLOSED))
        return 1;
    return 0;
}

static void reset_transient(struct lmux2_conn *c)
{
    int i;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++)
        reset_stream(&c->streams[i]);
    c->next_local_stream_id = c->role == LMUX2_ROLE_HOST ? 1 : 2;
    c->highest_peer_stream_id_seen = 0;
    c->conn_send_offset = 0;
    c->conn_recv_offset = 0;
    c->conn_rx_consumed = 0;
    c->local_max_data = 0;
    c->advertised_max_data = 0;
    c->last_sent_max_data = 0;
    c->peer_max_data = 0;
    c->max_data_dirty = 0;
    c->rr_next = 0;
}

void lmux2_conn_init(struct lmux2_conn *c, enum lmux2_role role, uint64_t conn_id)
{
    memset(c, 0, sizeof(*c));
    c->role = role;
    c->state = LMUX2_CONN_WAIT_HELLO;
    c->local_conn_id = conn_id ? conn_id : 1;
    reset_transient(c);
}

void lmux2_conn_begin_physical(struct lmux2_conn *c, uint64_t conn_id)
{
    c->state = LMUX2_CONN_WAIT_HELLO;
    c->local_conn_id = conn_id ? conn_id : c->local_conn_id + 1;
    if (c->local_conn_id == 0)
        c->local_conn_id = 1;
    c->peer_conn_id = 0;
    c->hello_sent = 0;
    c->outq_r = c->outq_len = 0;
    reset_transient(c);
}

int lmux2_conn_start(struct lmux2_conn *c)
{
    if (c->state == LMUX2_CONN_ERROR)
        return -1;
    return queue_hello(c);
}

struct lmux2_stream *lmux2_find_stream(struct lmux2_conn *c, uint64_t stream_id)
{
    int i;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++)
        if (c->streams[i].state != LMUX2_STREAM_UNUSED &&
            (c->streams[i].state != LMUX2_STREAM_TERMINAL ||
             c->streams[i].rx.len > 0) &&
            c->streams[i].id == stream_id)
            return &c->streams[i];
    return NULL;
}

const struct lmux2_stream *lmux2_find_stream_const(const struct lmux2_conn *c,
                                                   uint64_t stream_id)
{
    int i;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++)
        if (c->streams[i].state != LMUX2_STREAM_UNUSED &&
            (c->streams[i].state != LMUX2_STREAM_TERMINAL ||
             c->streams[i].rx.len > 0) &&
            c->streams[i].id == stream_id)
            return &c->streams[i];
    return NULL;
}

static struct lmux2_stream *alloc_stream_slot(struct lmux2_conn *c)
{
    int i;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++) {
        if (stream_slot_reclaimable(&c->streams[i])) {
            reset_stream(&c->streams[i]);
            return &c->streams[i];
        }
    }
    return NULL;
}

static int advertise_conn_limit(struct lmux2_conn *c)
{
    uint64_t limit;
    if (add_overflow_u64(c->conn_rx_consumed, LMUX2_CONN_RX_CAP, &limit) < 0)
        return -1;
    c->local_max_data = limit;
    if (limit > c->last_sent_max_data)
        c->max_data_dirty = 1;
    return 0;
}

static int advertise_stream_limit(struct lmux2_stream *s)
{
    uint64_t limit;
    if (add_overflow_u64(s->recv_consumed, LMUX2_STREAM_RX_CAP, &limit) < 0)
        return -1;
    s->local_max_stream_data = limit;
    if (limit > s->last_sent_max_stream_data)
        s->max_stream_data_dirty = 1;
    return 0;
}

static int establish_from_hello(struct lmux2_conn *c, const struct lmux2_hello *h)
{
    if (h->role == c->role) {
        set_conn_error(c, "peer role matches local role");
        return -1;
    }
    if (c->state == LMUX2_CONN_ESTABLISHED && c->peer_conn_id == h->conn_id) {
        if (!c->hello_sent && queue_hello(c) < 0)
            return -1;
        return 0;
    }
    if (c->state == LMUX2_CONN_ESTABLISHED && c->peer_conn_id != h->conn_id) {
        c->peer_restart_count++;
        lmux2_conn_begin_physical(c, c->local_conn_id + 1);
    }
    c->state = LMUX2_CONN_ESTABLISHED;
    c->peer_conn_id = h->conn_id;
    if (!c->hello_sent && queue_hello(c) < 0)
        return -1;
    return advertise_conn_limit(c);
}

static void handle_hello(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_hello h;
    if (fr->stream_id != 0 || fr->value != 0 ||
        lmux2_parse_hello_payload(fr->payload, fr->payload_len, &h) < 0) {
        set_conn_error(c, "bad HELLO");
        return;
    }
    (void)establish_from_hello(c, &h);
}

static void handle_stream_open(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_stream *s;
    uint64_t token = 0;
    if (fr->stream_id == 0 || !peer_stream_id_ok(c, fr->stream_id)) {
        set_conn_error(c, "bad peer stream id");
        return;
    }
    if (fr->payload_len != 0 && fr->payload_len != 8) {
        set_conn_error(c, "bad STREAM_OPEN payload");
        return;
    }
    if (fr->payload_len == 8)
        token = lmux2_get_le64(fr->payload);
    if (fr->stream_id <= c->highest_peer_stream_id_seen || lmux2_find_stream(c, fr->stream_id)) {
        c->stream_errors++;
        (void)queue_simple(c, LMUX2_T_STREAM_OPEN_ERROR, fr->stream_id, LMUX2_OPEN_E_BAD_STATE);
        return;
    }
    s = alloc_stream_slot(c);
    if (!s) {
        c->stream_errors++;
        (void)queue_simple(c, LMUX2_T_STREAM_OPEN_ERROR, fr->stream_id, LMUX2_OPEN_E_LIMIT);
        return;
    }
    c->highest_peer_stream_id_seen = fr->stream_id;
    s->state = LMUX2_STREAM_OPEN_RECEIVED;
    s->id = fr->stream_id;
    s->service_id = fr->value;
    s->open_token = token;
}

static void handle_stream_open_ok(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_stream *s = lmux2_find_stream(c, fr->stream_id);
    if (!s) {
        c->stale_frames++;
        return;
    }
    if (s->state != LMUX2_STREAM_OPEN_SENT) {
        set_stream_error(c, s, LMUX2_OPEN_E_BAD_STATE, "OPEN_OK in bad stream state");
        return;
    }
    s->state = LMUX2_STREAM_OPEN;
    (void)advertise_stream_limit(s);
}

static void handle_stream_open_error(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_stream *s = lmux2_find_stream(c, fr->stream_id);
    if (!s) {
        c->stale_frames++;
        return;
    }
    if (s->state != LMUX2_STREAM_OPEN_SENT) {
        set_stream_error(c, s, LMUX2_OPEN_E_BAD_STATE, "OPEN_ERROR in bad stream state");
        return;
    }
    s->remote_error = (uint32_t)fr->value;
    s->state = LMUX2_STREAM_TERMINAL;
}

static void handle_stream_close(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_stream *s = lmux2_find_stream(c, fr->stream_id);
    if (!s) {
        c->stale_frames++;
        return;
    }
    if (s->state == LMUX2_STREAM_REMOTE_CLOSED || s->state == LMUX2_STREAM_TERMINAL ||
        s->state == LMUX2_STREAM_RESET)
        return;
    if (s->state == LMUX2_STREAM_LOCAL_CLOSED)
        s->state = LMUX2_STREAM_TERMINAL;
    else if (s->state == LMUX2_STREAM_OPEN)
        s->state = LMUX2_STREAM_REMOTE_CLOSED;
    else
        s->state = LMUX2_STREAM_TERMINAL;
}

static void handle_stream_reset(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_stream *s = lmux2_find_stream(c, fr->stream_id);
    if (!s) {
        c->stale_frames++;
        return;
    }
    s->remote_error = (uint32_t)fr->value;
    s->state = LMUX2_STREAM_TERMINAL;
    ring_clear(&s->rx);
    ring_clear(&s->tx);
}

static void handle_max_stream_data(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_stream *s = lmux2_find_stream(c, fr->stream_id);
    if (!s) {
        c->stale_frames++;
        return;
    }
    if (fr->value > s->peer_max_stream_data)
        s->peer_max_stream_data = fr->value;
}

static void handle_max_data(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    if (fr->stream_id != 0 || fr->payload_len != 0) {
        set_conn_error(c, "bad MAX_DATA");
        return;
    }
    if (fr->value > c->peer_max_data)
        c->peer_max_data = fr->value;
}

static int stream_can_recv_data(enum lmux2_stream_state st)
{
    return st == LMUX2_STREAM_OPEN || st == LMUX2_STREAM_LOCAL_CLOSED;
}

static void reset_stream_due_to_peer(struct lmux2_conn *c, struct lmux2_stream *s,
                                     const char *why)
{
    set_stream_error(c, s, LMUX2_OPEN_E_BAD_STATE, why);
    (void)queue_simple(c, LMUX2_T_STREAM_RESET, s->id, LMUX2_OPEN_E_BAD_STATE);
    ring_clear(&s->rx);
    ring_clear(&s->tx);
}

static void handle_data(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    struct lmux2_stream *s = lmux2_find_stream(c, fr->stream_id);
    uint64_t end, conn_end;
    if (!s) {
        c->stale_frames++;
        return;
    }
    if (fr->payload_len == 0 || !stream_can_recv_data(s->state)) {
        reset_stream_due_to_peer(c, s, "DATA in bad stream state");
        return;
    }
    if (fr->value != s->expected_recv_offset ||
        add_overflow_u64(fr->value, fr->payload_len, &end) < 0) {
        reset_stream_due_to_peer(c, s, "DATA offset mismatch");
        return;
    }
    if (end > s->local_max_stream_data) {
        reset_stream_due_to_peer(c, s, "stream flow-control violation");
        return;
    }
    if (add_overflow_u64(c->conn_recv_offset, fr->payload_len, &conn_end) < 0 ||
        conn_end > c->local_max_data) {
        set_conn_error(c, "connection flow-control violation");
        return;
    }
    if (ring_free(&s->rx) < fr->payload_len) {
        reset_stream_due_to_peer(c, s, "stream receive buffer overflow");
        return;
    }
    ring_write(&s->rx, fr->payload, fr->payload_len);
    s->expected_recv_offset = end;
    c->conn_recv_offset = conn_end;
}

int lmux2_handle_frame(struct lmux2_conn *c, const struct lmux2_frame *fr)
{
    if (!c || !fr || c->state == LMUX2_CONN_ERROR)
        return -1;
    c->frames_rx++;
    if (!lmux2_type_known(fr->type)) {
        if (fr->flags & LMUX2_F_CRITICAL)
            set_conn_error(c, "unknown critical frame");
        else
            c->stale_frames++;
        return c->state == LMUX2_CONN_ERROR ? -1 : 0;
    }
    if (fr->flags != 0) {
        set_conn_error(c, "known frame with flags");
        return -1;
    }
    if (fr->type != LMUX2_T_HELLO && c->state != LMUX2_CONN_ESTABLISHED) {
        c->pre_established_drops++;
        return 0;
    }

    switch (fr->type) {
    case LMUX2_T_HELLO:
        handle_hello(c, fr);
        break;
    case LMUX2_T_STREAM_OPEN:
        handle_stream_open(c, fr);
        break;
    case LMUX2_T_STREAM_OPEN_OK:
        handle_stream_open_ok(c, fr);
        break;
    case LMUX2_T_STREAM_OPEN_ERROR:
        handle_stream_open_error(c, fr);
        break;
    case LMUX2_T_STREAM_CLOSE:
        handle_stream_close(c, fr);
        break;
    case LMUX2_T_STREAM_RESET:
        handle_stream_reset(c, fr);
        break;
    case LMUX2_T_DATA:
        handle_data(c, fr);
        break;
    case LMUX2_T_MAX_STREAM_DATA:
        handle_max_stream_data(c, fr);
        break;
    case LMUX2_T_MAX_DATA:
        handle_max_data(c, fr);
        break;
    case LMUX2_T_CONTROL:
        if (fr->stream_id != 0)
            set_conn_error(c, "CONTROL on nonzero stream");
        break;
    default:
        break;
    }
    return c->state == LMUX2_CONN_ERROR ? -1 : 0;
}

int lmux2_next_frame(struct lmux2_conn *c, struct lmux2_frame *fr)
{
    int n;
    if (!c || !fr || c->state == LMUX2_CONN_ERROR)
        return -1;
    if (c->outq_len) {
        *fr = c->outq[c->outq_r];
        c->outq_r = (c->outq_r + 1) % LMUX2_OUTQ_CAP;
        c->outq_len--;
        c->frames_tx++;
        return 1;
    }
    if (c->state != LMUX2_CONN_ESTABLISHED)
        return 0;
    if (c->max_data_dirty && c->local_max_data > c->last_sent_max_data) {
        memset(fr, 0, sizeof(*fr));
        fr->type = LMUX2_T_MAX_DATA;
        fr->value = c->local_max_data;
        c->advertised_max_data = c->local_max_data;
        c->last_sent_max_data = c->local_max_data;
        c->max_data_dirty = 0;
        c->frames_tx++;
        return 1;
    }
    for (n = 0; n < LMUX2_CORE_MAX_STREAMS; n++) {
        int idx = (c->rr_next + n) % LMUX2_CORE_MAX_STREAMS;
        struct lmux2_stream *s = &c->streams[idx];
        if (s->state == LMUX2_STREAM_UNUSED || s->state == LMUX2_STREAM_TERMINAL)
            continue;
        if (!s->max_stream_data_dirty ||
            s->local_max_stream_data <= s->last_sent_max_stream_data)
            continue;
        memset(fr, 0, sizeof(*fr));
        fr->type = LMUX2_T_MAX_STREAM_DATA;
        fr->stream_id = s->id;
        fr->value = s->local_max_stream_data;
        s->advertised_max_stream_data = s->local_max_stream_data;
        s->last_sent_max_stream_data = s->local_max_stream_data;
        s->max_stream_data_dirty = 0;
        c->rr_next = (idx + 1) % LMUX2_CORE_MAX_STREAMS;
        c->frames_tx++;
        return 1;
    }
    for (n = 0; n < LMUX2_CORE_MAX_STREAMS; n++) {
        int idx = (c->rr_next + n) % LMUX2_CORE_MAX_STREAMS;
        struct lmux2_stream *s = &c->streams[idx];
        const uint8_t *p;
        size_t avail;
        uint64_t stream_room, conn_room;
        if ((s->state != LMUX2_STREAM_OPEN &&
             s->state != LMUX2_STREAM_REMOTE_CLOSED &&
             s->state != LMUX2_STREAM_LOCAL_CLOSED) ||
            s->tx.len == 0)
            continue;
        if (s->send_offset >= s->peer_max_stream_data ||
            c->conn_send_offset >= c->peer_max_data)
            continue;
        stream_room = s->peer_max_stream_data - s->send_offset;
        conn_room = c->peer_max_data - c->conn_send_offset;
        avail = ring_peek_linear(&s->tx, &p);
        if (avail > LMUX2_MAX_PAYLOAD)
            avail = LMUX2_MAX_PAYLOAD;
        if (avail > stream_room)
            avail = (size_t)stream_room;
        if (avail > conn_room)
            avail = (size_t)conn_room;
        if (avail == 0)
            continue;
        memset(fr, 0, sizeof(*fr));
        fr->type = LMUX2_T_DATA;
        fr->stream_id = s->id;
        fr->value = s->send_offset;
        fr->payload_len = (uint16_t)avail;
        memcpy(fr->payload, p, avail);
        ring_drop(&s->tx, avail);
        s->send_offset += avail;
        c->conn_send_offset += avail;
        c->rr_next = (idx + 1) % LMUX2_CORE_MAX_STREAMS;
        c->frames_tx++;
        return 1;
    }
    for (n = 0; n < LMUX2_CORE_MAX_STREAMS; n++) {
        int idx = (c->rr_next + n) % LMUX2_CORE_MAX_STREAMS;
        struct lmux2_stream *s = &c->streams[idx];
        if (s->state == LMUX2_STREAM_UNUSED || s->state == LMUX2_STREAM_TERMINAL ||
            s->state == LMUX2_STREAM_RESET)
            continue;
        if (!s->local_close_requested || s->local_close_sent || s->tx.len)
            continue;
        memset(fr, 0, sizeof(*fr));
        fr->type = LMUX2_T_STREAM_CLOSE;
        fr->stream_id = s->id;
        s->local_close_sent = 1;
        if (s->state == LMUX2_STREAM_REMOTE_CLOSED)
            s->state = LMUX2_STREAM_TERMINAL;
        c->rr_next = (idx + 1) % LMUX2_CORE_MAX_STREAMS;
        c->frames_tx++;
        return 1;
    }
    return 0;
}

int lmux2_open_stream(struct lmux2_conn *c, uint64_t service_id, uint64_t *stream_id)
{
    return lmux2_open_stream_ex(c, service_id, 0, stream_id);
}

int lmux2_open_stream_ex(struct lmux2_conn *c, uint64_t service_id,
                         uint64_t pending_token, uint64_t *stream_id)
{
    struct lmux2_stream *s;
    uint64_t id;
    struct lmux2_frame fr;
    if (!c || c->state != LMUX2_CONN_ESTABLISHED)
        return -1;
    id = c->next_local_stream_id;
    if (id == 0 || UINT64_MAX - id < 2)
        return -1;
    s = alloc_stream_slot(c);
    if (!s)
        return -1;
    s->state = LMUX2_STREAM_OPEN_SENT;
    s->id = id;
    s->service_id = service_id;
    s->open_token = pending_token;
    c->next_local_stream_id += 2;
    if (stream_id)
        *stream_id = id;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_STREAM_OPEN;
    fr.stream_id = id;
    fr.value = service_id;
    if (pending_token) {
        fr.payload_len = 8;
        lmux2_put_le64(fr.payload, pending_token);
    }
    return queue_frame(c, &fr);
}

int lmux2_accept_stream(struct lmux2_conn *c, uint64_t stream_id)
{
    struct lmux2_stream *s = lmux2_find_stream(c, stream_id);
    if (!s || s->state != LMUX2_STREAM_OPEN_RECEIVED)
        return -1;
    s->state = LMUX2_STREAM_OPEN;
    if (queue_simple(c, LMUX2_T_STREAM_OPEN_OK, stream_id, 0) < 0)
        return -1;
    return advertise_stream_limit(s);
}

int lmux2_reject_stream(struct lmux2_conn *c, uint64_t stream_id, uint32_t error_code)
{
    struct lmux2_stream *s = lmux2_find_stream(c, stream_id);
    if (!s || s->state != LMUX2_STREAM_OPEN_RECEIVED)
        return -1;
    s->local_error = error_code;
    s->state = LMUX2_STREAM_TERMINAL;
    return queue_simple(c, LMUX2_T_STREAM_OPEN_ERROR, stream_id, error_code);
}

int lmux2_close_stream(struct lmux2_conn *c, uint64_t stream_id)
{
    struct lmux2_stream *s = lmux2_find_stream(c, stream_id);
    if (!s)
        return 0;
    if (s->state == LMUX2_STREAM_TERMINAL || s->state == LMUX2_STREAM_RESET)
        return 0;
    s->local_close_requested = 1;
    if (s->state == LMUX2_STREAM_REMOTE_CLOSED)
        return 0;
    else if (s->state != LMUX2_STREAM_LOCAL_CLOSED)
        s->state = LMUX2_STREAM_LOCAL_CLOSED;
    return 0;
}

int lmux2_reset_stream(struct lmux2_conn *c, uint64_t stream_id, uint32_t error_code)
{
    struct lmux2_stream *s = lmux2_find_stream(c, stream_id);
    if (!s)
        return 0;
    if (queue_simple(c, LMUX2_T_STREAM_RESET, stream_id, error_code) < 0)
        return -1;
    s->local_error = error_code;
    s->state = LMUX2_STREAM_TERMINAL;
    ring_clear(&s->rx);
    ring_clear(&s->tx);
    return 0;
}

size_t lmux2_stream_write(struct lmux2_conn *c, uint64_t stream_id,
                          const uint8_t *buf, size_t len)
{
    struct lmux2_stream *s = lmux2_find_stream(c, stream_id);
    if (!s || !buf || (s->state != LMUX2_STREAM_OPEN && s->state != LMUX2_STREAM_REMOTE_CLOSED))
        return 0;
    return ring_write(&s->tx, buf, len);
}

size_t lmux2_stream_read(struct lmux2_conn *c, uint64_t stream_id,
                         uint8_t *buf, size_t len)
{
    struct lmux2_stream *s = lmux2_find_stream(c, stream_id);
    size_t done = 0;
    if (!s || !buf)
        return 0;
    while (done < len && s->rx.len) {
        const uint8_t *p;
        size_t n = ring_peek_linear(&s->rx, &p);
        if (n > len - done)
            n = len - done;
        memcpy(buf + done, p, n);
        ring_drop(&s->rx, n);
        done += n;
        s->recv_consumed += n;
        c->conn_rx_consumed += n;
    }
    if (done) {
        (void)advertise_stream_limit(s);
        (void)advertise_conn_limit(c);
    }
    return done;
}

size_t lmux2_stream_rx_len(const struct lmux2_conn *c, uint64_t stream_id)
{
    const struct lmux2_stream *s = lmux2_find_stream_const(c, stream_id);
    return s ? s->rx.len : 0;
}

size_t lmux2_stream_tx_len(const struct lmux2_conn *c, uint64_t stream_id)
{
    const struct lmux2_stream *s = lmux2_find_stream_const(c, stream_id);
    return s ? s->tx.len : 0;
}
