#include "lmux2_core.h"
#include "lmux.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *msg)
{
    fprintf(stderr, "lmux2 test failed: %s\n", msg);
    return 1;
}

static int frame_eq(const struct lmux2_frame *a, const struct lmux2_frame *b)
{
    return a->type == b->type && a->flags == b->flags &&
           a->stream_id == b->stream_id && a->value == b->value &&
           a->payload_len == b->payload_len &&
           memcmp(a->payload, b->payload, a->payload_len) == 0;
}

static int pump_one(struct lmux2_conn *from, struct lmux2_conn *to)
{
    struct lmux2_frame fr;
    int r = lmux2_next_frame(from, &fr);
    if (r <= 0)
        return r;
    return lmux2_handle_frame(to, &fr) < 0 ? -1 : 1;
}

static int pump_all(struct lmux2_conn *a, struct lmux2_conn *b)
{
    int guard;
    for (guard = 0; guard < 256; guard++) {
        int pa = pump_one(a, b);
        int pb = pump_one(b, a);
        if (pa < 0 || pb < 0)
            return -1;
        if (pa == 0 && pb == 0)
            return 0;
    }
    return -1;
}

static int setup_pair(struct lmux2_conn *host, struct lmux2_conn *target)
{
    lmux2_conn_init(host, LMUX2_ROLE_HOST, 0x1111);
    lmux2_conn_init(target, LMUX2_ROLE_TARGET, 0x2222);
    if (lmux2_conn_start(host) < 0 || lmux2_conn_start(target) < 0)
        return -1;
    if (pump_all(host, target) < 0)
        return -1;
    return host->state == LMUX2_CONN_ESTABLISHED &&
           target->state == LMUX2_CONN_ESTABLISHED &&
           host->peer_max_data == LMUX2_CONN_RX_CAP &&
           target->peer_max_data == LMUX2_CONN_RX_CAP ? 0 : -1;
}

static int open_host_stream(struct lmux2_conn *host, struct lmux2_conn *target,
                            uint64_t *id)
{
    struct lmux2_stream *s;
    if (lmux2_open_stream(host, 77, id) < 0)
        return -1;
    if (pump_all(host, target) < 0)
        return -1;
    s = lmux2_find_stream(target, *id);
    if (!s || s->state != LMUX2_STREAM_OPEN_RECEIVED)
        return -1;
    if (lmux2_accept_stream(target, *id) < 0)
        return -1;
    if (pump_all(host, target) < 0)
        return -1;
    s = lmux2_find_stream(host, *id);
    if (!s || s->state != LMUX2_STREAM_OPEN || s->peer_max_stream_data != LMUX2_STREAM_RX_CAP)
        return -1;
    s = lmux2_find_stream(target, *id);
    return s && s->state == LMUX2_STREAM_OPEN && s->peer_max_stream_data == LMUX2_STREAM_RX_CAP ? 0 : -1;
}

static int encode_decoded(const uint8_t *decoded, size_t decoded_len,
                          uint8_t *encoded, size_t *encoded_len)
{
    size_t n;
    if (lmux_cobs_encode(decoded, decoded_len, encoded, LMUX2_MAX_ENCODED, &n) < 0)
        return -1;
    encoded[n++] = 0;
    *encoded_len = n;
    return 0;
}

static int test_codec(void)
{
    struct lmux2_frame fr, out;
    uint8_t enc[LMUX2_MAX_ENCODED], decoded[LMUX2_MAX_DECODED];
    size_t enc_len, decoded_len;
    uint16_t crc;

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_DATA;
    fr.stream_id = 7;
    fr.value = 0x123456789abcdef0ULL;
    fr.payload_len = 6;
    fr.payload[0] = 0;
    fr.payload[1] = 3;
    fr.payload[2] = 0x14;
    fr.payload[3] = 0xff;
    fr.payload[4] = 'A';
    fr.payload[5] = 'Z';
    if (lmux2_encode_frame(&fr, enc, sizeof(enc), &enc_len) < 0 || enc[enc_len - 1] != 0)
        return fail("codec encode delimiter");
    if (lmux2_decode_frame(enc, enc_len - 1, &out) < 0 || !frame_eq(&fr, &out))
        return fail("codec roundtrip");
    enc[3] ^= 0x55;
    if (lmux2_decode_frame(enc, enc_len - 1, &out) == 0)
        return fail("codec CRC/corruption rejection");

    memcpy(decoded, LMUX2_MAGIC, 4);
    decoded[4] = LMUX2_VERSION;
    decoded[5] = LMUX2_T_HELLO;
    lmux_put_le16(decoded + 6, 0);
    memset(decoded + 8, 0, LMUX2_HEADER_LEN - 8);
    decoded_len = LMUX2_HEADER_LEN - 1;
    if (encode_decoded(decoded, decoded_len, enc, &enc_len) < 0 ||
        lmux2_decode_frame(enc, enc_len - 1, &out) == 0)
        return fail("truncated header rejection");

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_DATA;
    fr.payload_len = LMUX2_MAX_PAYLOAD + 1;
    if (lmux2_encode_frame(&fr, enc, sizeof(enc), &enc_len) == 0)
        return fail("oversized payload encode rejection");

    memset(decoded, 0, sizeof(decoded));
    memcpy(decoded, LMUX2_MAGIC, 4);
    decoded[4] = 3;
    decoded[5] = LMUX2_T_HELLO;
    decoded_len = LMUX2_HEADER_LEN;
    crc = lmux_crc16_ccitt_false(decoded, decoded_len);
    lmux_put_le16(decoded + decoded_len, crc);
    decoded_len += 2;
    if (encode_decoded(decoded, decoded_len, enc, &enc_len) < 0 ||
        lmux2_decode_frame(enc, enc_len - 1, &out) == 0)
        return fail("bad protocol version rejection");

    memset(&fr, 0, sizeof(fr));
    fr.type = 42;
    fr.stream_id = 0;
    if (lmux2_encode_frame(&fr, enc, sizeof(enc), &enc_len) < 0 ||
        lmux2_decode_frame(enc, enc_len - 1, &out) < 0 || out.type != 42)
        return fail("unknown non-critical codec handling");
    return 0;
}

static int test_handshake(void)
{
    struct lmux2_conn h, t;
    struct lmux2_frame fr;
    if (setup_pair(&h, &t) < 0)
        return fail("HELLO convergence");

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_HELLO;
    if (lmux2_make_hello_payload(LMUX2_ROLE_TARGET, 0x2222, LMUX2_CORE_MAX_STREAMS,
                                 LMUX2_STREAM_RX_CAP, LMUX2_CONN_RX_CAP,
                                 fr.payload, &fr.payload_len) < 0)
        return fail("make duplicate HELLO");
    if (lmux2_handle_frame(&h, &fr) < 0 || h.state != LMUX2_CONN_ESTABLISHED)
        return fail("duplicate HELLO behavior");
    if (h.local_conn_id != 0x1111 || h.peer_restart_count != 0)
        return fail("duplicate HELLO restarted session");

    if (lmux2_open_stream(&h, 77, &fr.stream_id) < 0 || h.next_local_stream_id != 3)
        return fail("setup peer restart open");
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_HELLO;
    if (lmux2_make_hello_payload(LMUX2_ROLE_TARGET, 0x9999, LMUX2_CORE_MAX_STREAMS,
                                 LMUX2_STREAM_RX_CAP, LMUX2_CONN_RX_CAP,
                                 fr.payload, &fr.payload_len) < 0)
        return fail("make peer restart HELLO");
    if (lmux2_handle_frame(&h, &fr) < 0 ||
        h.state != LMUX2_CONN_ESTABLISHED ||
        h.peer_conn_id != 0x9999 ||
        h.local_conn_id != 0x1112 ||
        h.next_local_stream_id != 1 ||
        lmux2_find_stream(&h, 1) ||
        h.outq_len != 1 ||
        !h.max_data_dirty ||
        h.local_max_data != LMUX2_CONN_RX_CAP ||
        h.peer_restart_count != 1)
        return fail("different HELLO peer id did not replace session");

    lmux2_conn_init(&h, LMUX2_ROLE_HOST, 0x3333);
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_STREAM_OPEN;
    fr.stream_id = 2;
    if (lmux2_handle_frame(&h, &fr) < 0 || lmux2_find_stream(&h, 2) ||
        h.stale_frames != 0 || h.pre_established_drops != 1)
        return fail("stream traffic before HELLO");
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_CONTROL;
    if (lmux2_handle_frame(&h, &fr) < 0 || h.state != LMUX2_CONN_WAIT_HELLO ||
        h.stale_frames != 0 || h.pre_established_drops != 2)
        return fail("control traffic before HELLO");

    if (setup_pair(&h, &t) < 0)
        return fail("setup for reconnect");
    if (open_host_stream(&h, &t, &fr.stream_id) < 0)
        return fail("open before reconnect");
    lmux2_conn_begin_physical(&h, 0x4444);
    if (lmux2_find_stream(&h, fr.stream_id) || h.next_local_stream_id != 1)
        return fail("new physical connection did not reset stream namespace");
    return 0;
}

static int test_stream_ids(void)
{
    struct lmux2_conn h, t;
    struct lmux2_frame fr;
    uint64_t id1, id2, tid;

    if (setup_pair(&h, &t) < 0)
        return fail("stream id setup");
    if (lmux2_open_stream(&h, 1, &id1) < 0 || id1 != 1 ||
        lmux2_open_stream(&h, 1, &id2) < 0 || id2 != 3)
        return fail("host odd monotonic stream ids");
    if (lmux2_open_stream(&t, 1, &tid) < 0 || tid != 2)
        return fail("target even stream ids");

    if (setup_pair(&h, &t) < 0)
        return fail("invalid parity setup");
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_STREAM_OPEN;
    fr.stream_id = 1;
    if (lmux2_handle_frame(&h, &fr) == 0 || h.state != LMUX2_CONN_ERROR)
        return fail("invalid peer parity was not connection error");

    if (setup_pair(&h, &t) < 0)
        return fail("duplicate open setup");
    fr.stream_id = 2;
    if (lmux2_handle_frame(&h, &fr) < 0 ||
        lmux2_handle_frame(&h, &fr) < 0 ||
        h.state != LMUX2_CONN_ESTABLISHED || h.stream_errors == 0)
        return fail("duplicate OPEN id handling");
    return 0;
}

static int test_stream_lifecycle(void)
{
    struct lmux2_conn h, t;
    uint64_t id1, id2;
    struct lmux2_stream *s;

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id1) < 0)
        return fail("OPEN -> OK");

    if (setup_pair(&h, &t) < 0)
        return fail("OPEN -> ERROR setup");
    if (lmux2_open_stream(&h, 77, &id1) < 0 || pump_all(&h, &t) < 0 ||
        lmux2_reject_stream(&t, id1, LMUX2_OPEN_E_REFUSED) < 0 ||
        pump_all(&h, &t) < 0)
        return fail("OPEN -> ERROR exchange");
    s = lmux2_find_stream(&h, id1);
    if (s && s->state != LMUX2_STREAM_TERMINAL)
        return fail("OPEN_ERROR did not terminal stream");

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id1) < 0)
        return fail("close setup");
    if (lmux2_close_stream(&h, id1) < 0 || pump_all(&h, &t) < 0)
        return fail("local close send");
    s = lmux2_find_stream(&t, id1);
    if (!s || s->state != LMUX2_STREAM_REMOTE_CLOSED)
        return fail("peer close state");
    if (lmux2_close_stream(&t, id1) < 0 || pump_all(&h, &t) < 0)
        return fail("peer close reply");
    s = lmux2_find_stream(&h, id1);
    if (s && s->state != LMUX2_STREAM_TERMINAL)
        return fail("graceful close terminal state");

    if (setup_pair(&h, &t) < 0 ||
        open_host_stream(&h, &t, &id1) < 0 ||
        open_host_stream(&h, &t, &id2) < 0)
        return fail("reset isolation setup");
    if (lmux2_reset_stream(&t, id1, 99) < 0 || pump_all(&h, &t) < 0)
        return fail("target reset stream");
    s = lmux2_find_stream(&h, id2);
    if (!s || s->state != LMUX2_STREAM_OPEN || h.state != LMUX2_CONN_ESTABLISHED)
        return fail("unrelated stream did not survive reset");
    if (lmux2_reset_stream(&h, id2, 100) < 0 || pump_all(&h, &t) < 0 ||
        t.state != LMUX2_CONN_ESTABLISHED)
        return fail("host reset stream");
    return 0;
}

static int test_data(void)
{
    struct lmux2_conn h, t;
    struct lmux2_frame fr;
    uint64_t id;
    uint8_t data[] = { 'A', 0x00, 0x03, 0x14, 0xff, 'Z' };
    uint8_t got[sizeof(data)];
    struct lmux2_stream *s;

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id) < 0)
        return fail("data setup");
    if (lmux2_stream_write(&h, id, data, sizeof(data)) != sizeof(data) ||
        pump_all(&h, &t) < 0 ||
        lmux2_stream_read(&t, id, got, sizeof(got)) != sizeof(got) ||
        memcmp(got, data, sizeof(data)) != 0)
        return fail("binary-clean DATA");

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_DATA;
    fr.stream_id = id;
    fr.value = 999;
    fr.payload_len = 1;
    fr.payload[0] = 'x';
    if (lmux2_handle_frame(&t, &fr) < 0 || t.state != LMUX2_CONN_ESTABLISHED)
        return fail("out-of-order DATA became connection error");
    s = lmux2_find_stream(&t, id);
    if (!s || s->state != LMUX2_STREAM_RESET)
        return fail("out-of-order DATA was not stream-scoped");

    if (setup_pair(&h, &t) < 0)
        return fail("DATA cannot create setup");
    if (lmux2_handle_frame(&h, &fr) < 0 || lmux2_find_stream(&h, id) ||
        h.stale_frames == 0)
        return fail("DATA created stream");

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id) < 0)
        return fail("late DATA setup");
    if (lmux2_reset_stream(&t, id, 1) < 0 || pump_all(&h, &t) < 0)
        return fail("late DATA reset setup");
    if (lmux2_handle_frame(&t, &fr) < 0 || lmux2_stream_rx_len(&t, id) != 0)
        return fail("late DATA affected terminal stream");
    return 0;
}

static int test_flow_control(void)
{
    struct lmux2_conn h, t;
    struct lmux2_frame fr;
    struct lmux2_stream *s1, *s2;
    uint64_t id1, id2;
    uint8_t data[32], got[32];
    int i;

    for (i = 0; i < (int)sizeof(data); i++)
        data[i] = (uint8_t)i;

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id1) < 0)
        return fail("flow setup");
    s1 = lmux2_find_stream(&h, id1);
    s1->peer_max_stream_data = 3;
    if (lmux2_stream_write(&h, id1, data, 10) != 10)
        return fail("flow write");
    if (pump_one(&h, &t) < 0 || lmux2_stream_rx_len(&t, id1) != 3)
        return fail("per-stream limit");
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_MAX_STREAM_DATA;
    fr.stream_id = id1;
    fr.value = 2;
    if (lmux2_handle_frame(&h, &fr) < 0 || s1->peer_max_stream_data != 3)
        return fail("smaller stream limit was not idempotent");
    fr.value = 10;
    if (lmux2_handle_frame(&h, &fr) < 0 || s1->peer_max_stream_data != 10 ||
        pump_all(&h, &t) < 0 || lmux2_stream_rx_len(&t, id1) != 10)
        return fail("limit increase");
    if (lmux2_stream_read(&t, id1, got, 10) != 10)
        return fail("flow read");

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id1) < 0)
        return fail("connection limit setup");
    s1 = lmux2_find_stream(&h, id1);
    h.peer_max_data = 2;
    s1->peer_max_stream_data = 10;
    if (lmux2_stream_write(&h, id1, data, 10) != 10 ||
        pump_one(&h, &t) < 0 || lmux2_stream_rx_len(&t, id1) != 2 ||
        lmux2_next_frame(&h, &fr) != 0)
        return fail("connection limit exhaustion");

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_MAX_DATA;
    fr.value = 1;
    if (lmux2_handle_frame(&h, &fr) < 0 || h.peer_max_data != 2)
        return fail("smaller connection limit was not idempotent");
    fr.value = 12;
    if (lmux2_handle_frame(&h, &fr) < 0 || h.peer_max_data != 12 ||
        pump_all(&h, &t) < 0 || lmux2_stream_rx_len(&t, id1) != 10)
        return fail("connection limit increase");

    if (setup_pair(&h, &t) < 0 ||
        open_host_stream(&h, &t, &id1) < 0 ||
        open_host_stream(&h, &t, &id2) < 0)
        return fail("blocked stream setup");
    s1 = lmux2_find_stream(&h, id1);
    s2 = lmux2_find_stream(&h, id2);
    s1->peer_max_stream_data = 0;
    s2->peer_max_stream_data = 8;
    if (lmux2_stream_write(&h, id1, data, 8) != 8 ||
        lmux2_stream_write(&h, id2, data, 8) != 8 ||
        pump_one(&h, &t) < 0 || lmux2_stream_rx_len(&t, id2) != 8)
        return fail("one blocked stream blocked another");

    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_DATA;
    fr.stream_id = id2;
    fr.value = UINT64_MAX - 1;
    fr.payload_len = 2;
    fr.payload[0] = 'x';
    fr.payload[1] = 'y';
    if (lmux2_handle_frame(&t, &fr) < 0 || t.state != LMUX2_CONN_ESTABLISHED)
        return fail("overflow DATA became connection error");
    s2 = lmux2_find_stream(&t, id2);
    if (!s2 || s2->state != LMUX2_STREAM_RESET)
        return fail("overflow DATA was not stream-scoped");
    return 0;
}

static uint8_t bulk_byte(size_t off)
{
    return (uint8_t)((off * 33u + (off >> 5) + 0x5au) & 0xffu);
}

static void fill_sender_tx(struct lmux2_conn *c, uint64_t id, size_t *produced,
                           size_t total)
{
    uint8_t buf[257];
    while (*produced < total && lmux2_stream_tx_len(c, id) < LMUX2_STREAM_TX_CAP) {
        size_t n = total - *produced;
        size_t room = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(c, id);
        size_t j, w;
        if (n > room)
            n = room;
        if (n > sizeof(buf))
            n = sizeof(buf);
        for (j = 0; j < n; j++)
            buf[j] = bulk_byte(*produced + j);
        w = lmux2_stream_write(c, id, buf, n);
        if (w == 0)
            return;
        *produced += w;
    }
}

static int test_flow_control_liveness(void)
{
    struct lmux2_conn h, t;
    struct lmux2_frame fr;
    struct lmux2_stream *hs, *ts;
    uint64_t id;
    size_t produced = 0, consumed = 0;
    unsigned stream_updates = 0, conn_updates = 0, blocked_cycles = 0;
    enum { TOTAL = 1024 * 1024 };
    uint8_t buf[257], got[333];
    int i;

    for (i = 0; i < (int)sizeof(buf); i++)
        buf[i] = bulk_byte((size_t)i);
    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id) < 0)
        return fail("flow liveness setup");
    hs = lmux2_find_stream(&h, id);
    ts = lmux2_find_stream(&t, id);
    if (!hs || !ts)
        return fail("flow liveness stream lookup");
    hs->peer_max_stream_data = 256;
    h.peer_max_data = 256;

    while (consumed < TOTAL) {
        fill_sender_tx(&h, id, &produced, TOTAL);

        for (;;) {
            int r = lmux2_next_frame(&h, &fr);
            if (r < 0)
                return fail("flow liveness sender next");
            if (r == 0)
                break;
            if (fr.type == LMUX2_T_DATA && lmux2_handle_frame(&t, &fr) < 0)
                return fail("flow liveness receiver handle DATA");
            else if (fr.type != LMUX2_T_DATA && lmux2_handle_frame(&t, &fr) < 0)
                return fail("flow liveness receiver handle non-DATA");
            if (fr.type == LMUX2_T_DATA)
                fill_sender_tx(&h, id, &produced, TOTAL);
        }

        if (lmux2_stream_tx_len(&h, id) > 0 &&
            (hs->send_offset >= hs->peer_max_stream_data ||
             h.conn_send_offset >= h.peer_max_data))
            blocked_cycles++;

        while (lmux2_stream_rx_len(&t, id) > 0) {
            size_t n = lmux2_stream_read(&t, id, got, sizeof(got));
            size_t j;
            if (n == 0)
                return fail("flow liveness read zero");
            for (j = 0; j < n; j++)
                if (got[j] != bulk_byte(consumed + j))
                    return fail("flow liveness data mismatch");
            consumed += n;

            for (;;) {
                int r = lmux2_next_frame(&t, &fr);
                if (r < 0)
                    return fail("flow liveness receiver next");
                if (r == 0)
                    break;
                if (fr.type == LMUX2_T_MAX_STREAM_DATA)
                    stream_updates++;
                if (fr.type == LMUX2_T_MAX_DATA)
                    conn_updates++;
                if (lmux2_handle_frame(&h, &fr) < 0)
                    return fail("flow liveness sender handle limit");
            }
        }
    }

    if (produced != TOTAL || consumed != TOTAL)
        return fail("flow liveness incomplete");
    if (blocked_cycles < 100 || stream_updates < 100 || conn_updates < 100) {
        fprintf(stderr, "flow liveness counts: blocked=%u stream_updates=%u conn_updates=%u produced=%zu consumed=%zu\n",
                blocked_cycles, stream_updates, conn_updates, produced, consumed);
        return fail("flow liveness did not cycle limits");
    }
    if (ts->local_max_stream_data != ts->last_sent_max_stream_data ||
        t.local_max_data != t.last_sent_max_data ||
        ts->max_stream_data_dirty || t.max_data_dirty)
        return fail("flow liveness dirty limits not flushed");

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id) < 0)
        return fail("flow dirty queue setup");
    for (i = 0; i < LMUX2_OUTQ_CAP; i++) {
        memset(&fr, 0, sizeof(fr));
        fr.type = LMUX2_T_CONTROL;
        if (lmux2_queue_frame(&t, &fr) < 0)
            return fail("flow dirty queue fill");
    }
    if (lmux2_stream_write(&h, id, buf, sizeof(buf)) != sizeof(buf) ||
        pump_one(&h, &t) < 0)
        return fail("flow dirty queue data");
    if (lmux2_stream_read(&t, id, got, sizeof(buf)) != sizeof(buf))
        return fail("flow dirty queue read");
    ts = lmux2_find_stream(&t, id);
    if (!ts || !ts->max_stream_data_dirty || !t.max_data_dirty ||
        t.state != LMUX2_CONN_ESTABLISHED)
        return fail("flow dirty queue lost dirty state");
    for (i = 0; i < LMUX2_OUTQ_CAP; i++) {
        if (lmux2_next_frame(&t, &fr) != 1 || fr.type != LMUX2_T_CONTROL ||
            lmux2_handle_frame(&h, &fr) < 0)
            return fail("flow dirty queue drain");
    }
    if (lmux2_next_frame(&t, &fr) != 1 || fr.type != LMUX2_T_MAX_DATA ||
        lmux2_handle_frame(&h, &fr) < 0)
        return fail("flow dirty queue max data");
    if (lmux2_next_frame(&t, &fr) != 1 || fr.type != LMUX2_T_MAX_STREAM_DATA ||
        lmux2_handle_frame(&h, &fr) < 0)
        return fail("flow dirty queue max stream data");
    return 0;
}

static int test_stale_traffic(void)
{
    struct lmux2_conn h, t;
    struct lmux2_frame fr;
    uint64_t id;
    unsigned long long stale;

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id) < 0)
        return fail("stale setup");
    if (lmux2_reset_stream(&h, id, 1) < 0 || pump_all(&h, &t) < 0)
        return fail("reset before stale");
    stale = h.stale_frames;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_MAX_STREAM_DATA;
    fr.stream_id = id;
    fr.value = 999;
    if (lmux2_handle_frame(&h, &fr) < 0 || h.stale_frames <= stale)
        return fail("late MAX_STREAM_DATA terminal stream");
    fr.type = LMUX2_T_STREAM_CLOSE;
    if (lmux2_handle_frame(&h, &fr) < 0)
        return fail("duplicate CLOSE");
    fr.type = LMUX2_T_STREAM_RESET;
    if (lmux2_handle_frame(&h, &fr) < 0)
        return fail("duplicate RESET");
    fr.type = LMUX2_T_STREAM_OPEN;
    fr.stream_id = id;
    if (lmux2_handle_frame(&t, &fr) < 0 || t.state != LMUX2_CONN_ESTABLISHED)
        return fail("OPEN using already-consumed stream ID");
    return 0;
}

static int test_resource_limits(void)
{
    struct lmux2_conn h, t;
    uint64_t id;
    uint8_t data[LMUX2_STREAM_TX_CAP + 8];
    int i;

    if (setup_pair(&h, &t) < 0)
        return fail("resource setup");
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++)
        if (lmux2_open_stream(&h, 1, &id) < 0)
            return fail("stream table fill");
    if (lmux2_open_stream(&h, 1, &id) == 0)
        return fail("stream table exhaustion");

    if (setup_pair(&h, &t) < 0 || open_host_stream(&h, &t, &id) < 0)
        return fail("buffer setup");
    memset(data, 0xa5, sizeof(data));
    if (lmux2_stream_write(&h, id, data, sizeof(data)) != LMUX2_STREAM_TX_CAP ||
        lmux2_stream_tx_len(&h, id) != LMUX2_STREAM_TX_CAP)
        return fail("bounded TX buffer");
    return 0;
}

static int test_unknown_frames(void)
{
    struct lmux2_conn h, t;
    struct lmux2_frame fr;
    if (setup_pair(&h, &t) < 0)
        return fail("unknown setup");
    memset(&fr, 0, sizeof(fr));
    fr.type = 99;
    if (lmux2_handle_frame(&h, &fr) < 0 || h.state != LMUX2_CONN_ESTABLISHED)
        return fail("unknown non-critical frame");
    fr.flags = LMUX2_F_CRITICAL;
    if (lmux2_handle_frame(&h, &fr) == 0 || h.state != LMUX2_CONN_ERROR)
        return fail("unknown critical frame");
    return 0;
}

int main(void)
{
    if (test_codec() ||
        test_handshake() ||
        test_stream_ids() ||
        test_stream_lifecycle() ||
        test_data() ||
        test_flow_control() ||
        test_flow_control_liveness() ||
        test_stale_traffic() ||
        test_resource_limits() ||
        test_unknown_frames())
        return 1;
    puts("lmux2 selftest ok");
    return 0;
}
