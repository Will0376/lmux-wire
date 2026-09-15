#define LMUXD_NO_MAIN
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "lmuxd.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>

static int fail(const char *msg)
{
    fprintf(stderr, "lmuxd state test failed: %s\n", msg);
    return 1;
}

static void init_app(struct app *a)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->carrier_fd = 0;
    a->control_listen_fd = -1;
    a->control_fd = -1;
    a->local_session = 0x11111111u;
    a->local_features = LMUX_FEATURE_FILE | LMUX_FEATURE_CWD_QUERY |
                        LMUX_FEATURE_WINSIZE | LMUX_FEATURE_STREAM_GEN;
    a->link_state = LINK_WAIT_HELLO;
    for (i = 0; i < LMUX_CHANNELS; i++) {
        a->ch[i].ep.fd = -1;
        a->ch[i].ep.listen_fd = -1;
        a->ch[i].file.fd = -1;
    }
}

static void make_frame(struct lmux_frame *fr, uint8_t type, uint8_t ch)
{
    memset(fr, 0, sizeof(*fr));
    fr->version = LMUX_VERSION;
    fr->type = type;
    fr->channel = ch;
}

static void make_hello(struct lmux_frame *fr, uint32_t sid)
{
    make_frame(fr, LMUX_T_HELLO, 0);
    fr->payload_len = 8;
    memcpy(fr->payload, "LMX1", 4);
    lmux_put_le32(fr->payload + 4, sid);
}

static void make_hello_features(struct lmux_frame *fr, uint32_t sid, uint32_t features)
{
    const char v[] = "test-peer";
    make_frame(fr, LMUX_T_HELLO, 0);
    fr->payload_len = 13 + sizeof(v) - 1;
    memcpy(fr->payload, "LMX1", 4);
    lmux_put_le32(fr->payload + 4, sid);
    lmux_put_le32(fr->payload + 8, features);
    fr->payload[12] = sizeof(v) - 1;
    memcpy(fr->payload + 13, v, sizeof(v) - 1);
}

static int feed_encoded(struct app *a, const struct lmux_frame *fr)
{
    uint8_t enc[LMUX_MAX_ENCODED];
    size_t enc_len = 0;

    if (lmux_encode_frame(fr, enc, sizeof(enc), &enc_len) < 0)
        return -1;
    process_encoded_record(a, enc, enc_len - 1);
    return 0;
}

static int pop_output_frame(struct app *a, struct lmux_frame *fr)
{
    if (build_next_output(a) <= 0)
        return -1;
    if (a->out_len == 0 || a->out[a->out_len - 1] != 0)
        return -1;
    if (lmux_decode_frame(a->out, a->out_len - 1, fr) < 0)
        return -1;
    a->out_len = 0;
    a->out_pos = 0;
    return 0;
}

static int pump_one_frame(struct app *from, struct app *to, struct lmux_frame *out)
{
    struct lmux_frame fr;
    if (!out)
        out = &fr;
    if (pop_output_frame(from, out) < 0)
        return -1;
    handle_frame(to, out);
    return 0;
}

static int pump_until_frame(struct app *from, struct app *to, uint8_t type, uint8_t channel,
                            struct lmux_frame *out)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (pump_one_frame(from, to, out) < 0)
            return -1;
        if (out->type == type && out->channel == channel)
            return 0;
    }
    return -1;
}

static int legacy_accepts_frame(const struct lmux_frame *fr)
{
    if (fr->version != LMUX_VERSION || fr->flags != 0)
        return 0;
    if (fr->type < LMUX_T_HELLO || fr->type > LMUX_T_RESET)
        return 0;
    if ((fr->type == LMUX_T_HELLO || fr->type == LMUX_T_RESET) && fr->channel != 0)
        return 0;
    if (fr->type != LMUX_T_HELLO && fr->type != LMUX_T_RESET &&
        (fr->channel < 1 || fr->channel > LMUX_CHANNELS))
        return 0;
    if (fr->type == LMUX_T_HELLO &&
        (fr->payload_len != 8 || memcmp(fr->payload, "LMX1", 4) != 0))
        return 0;
    if ((fr->type == LMUX_T_OPEN || fr->type == LMUX_T_CLOSE) && fr->payload_len != 0)
        return 0;
    return 1;
}

static void setup_established_pair(struct app *a, struct app *b)
{
    struct lmux_frame fr;
    init_app(a);
    init_app(b);
    make_hello_features(&fr, 0xaaaabbbb,
                        LMUX_FEATURE_FILE | LMUX_FEATURE_CWD_QUERY |
                        LMUX_FEATURE_WINSIZE | LMUX_FEATURE_STREAM_GEN);
    handle_frame(a, &fr);
    make_hello_features(&fr, 0xbbbbcccc,
                        LMUX_FEATURE_FILE | LMUX_FEATURE_CWD_QUERY |
                        LMUX_FEATURE_WINSIZE | LMUX_FEATURE_STREAM_GEN);
    handle_frame(b, &fr);
    a->pending_hello = 0;
    b->pending_hello = 0;
    a->out_len = a->out_pos = 0;
    b->out_len = b->out_pos = 0;
}

static void setup_legacy_established_pair(struct app *a, struct app *b)
{
    struct lmux_frame fr;
    init_app(a);
    init_app(b);
    make_hello(&fr, 0xaaaabbbbu);
    handle_frame(a, &fr);
    make_hello(&fr, 0xbbbbccccu);
    handle_frame(b, &fr);
    a->pending_hello = 0;
    b->pending_hello = 0;
    a->out_len = a->out_pos = 0;
    b->out_len = b->out_pos = 0;
}

static int test_garbage_before_hello(void)
{
    struct app a;
    struct lmux_frame fr;
    const uint8_t junk[] = { 0x01, 0x02, 0x03 };

    init_app(&a);
    process_encoded_record(&a, junk, sizeof(junk));
    if (a.pending_reset || a.link_state != LINK_WAIT_HELLO)
        return fail("garbage before HELLO emitted RESET");

    make_hello(&fr, 0xaabbcc01u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != 0xaabbcc01u)
        return fail("valid HELLO after garbage did not establish");
    return 0;
}

static int test_open_credit_before_hello(void)
{
    struct app a;
    struct lmux_frame fr;

    init_app(&a);
    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode OPEN");
    make_frame(&fr, LMUX_T_CREDIT, 1);
    fr.payload_len = 2;
    lmux_put_le16(fr.payload, 7);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode CREDIT");
    if (a.pending_reset || a.link_state != LINK_WAIT_HELLO || a.ch[0].remote_open)
        return fail("pre-HELLO OPEN/CREDIT was not ignored");
    return 0;
}

static int test_reset_stale_open_then_hello(void)
{
    struct app a;
    struct lmux_frame fr;

    init_app(&a);
    make_hello(&fr, 0x1000u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    make_frame(&fr, LMUX_T_RESET, 0);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode RESET");
    if (a.link_state != LINK_WAIT_HELLO || a.pending_reset)
        return fail("received RESET cascaded");

    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode stale OPEN");
    if (a.pending_reset || a.ch[0].remote_open)
        return fail("stale OPEN after RESET was not ignored");

    make_hello(&fr, 0x2000u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode second HELLO");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != 0x2000u)
        return fail("HELLO after RESET did not establish");
    return 0;
}

static int test_duplicate_hello(void)
{
    struct app a;
    struct lmux_frame fr;

    init_app(&a);
    a.ch[0].local_open = 1;
    make_hello(&fr, 0x3333u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode OPEN");
    if (!a.ch[0].active)
        return fail("channel did not activate before duplicate HELLO");
    a.ch[0].remote_credit = 5;

    make_hello(&fr, 0x3333u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode duplicate HELLO");
    if (a.link_state != LINK_ESTABLISHED || !a.ch[0].active ||
        a.ch[0].remote_credit != 5 || a.pending_reset)
        return fail("duplicate HELLO reset established channel");
    return 0;
}

static int test_new_session_hello(void)
{
    struct app a;
    struct lmux_frame fr;
    const uint8_t b = 0x5a;

    init_app(&a);
    a.ch[0].local_open = 1;
    make_hello(&fr, 0x4444u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode HELLO");
    make_frame(&fr, LMUX_T_OPEN, 1);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode OPEN");
    ring_write(&a.ch[0].tx, &b, 1);
    ring_write(&a.ch[0].rx, &b, 1);
    a.ch[0].remote_credit = 9;
    a.ch[0].credit_pending = 3;

    make_hello(&fr, 0x5555u);
    if (feed_encoded(&a, &fr) < 0)
        return fail("encode new HELLO");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != 0x5555u)
        return fail("new-session HELLO did not establish");
    if (a.ch[0].remote_open || a.ch[0].active || a.ch[0].remote_credit ||
        a.ch[0].credit_pending || a.ch[0].tx.len || a.ch[0].rx.len)
        return fail("new-session HELLO did not clear channel transport state");
    if (!a.ch[0].pending_open)
        return fail("new-session HELLO did not re-advertise local OPEN");
    return 0;
}

static int test_empty_cobs_record(void)
{
    struct app a;

    init_app(&a);
    process_encoded_record(&a, NULL, 0);
    if (a.pending_reset || a.link_state != LINK_WAIT_HELLO)
        return fail("empty COBS record was treated as fault");
    return 0;
}

static int test_lost_first_hello_converges(void)
{
    struct app a, b;
    struct lmux_frame fr;

    init_app(&a);
    init_app(&b);
    a.local_session = 0xaaa00001u;
    b.local_session = 0xbbb00001u;
    b.ch[0].local_open = 1;
    b.ch[1].local_open = 1;

    if (pop_output_frame(&b, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("B initial HELLO not generated");
    /* B's first HELLO is intentionally dropped. */

    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("A initial HELLO not generated");
    if (feed_encoded(&b, &fr) < 0)
        return fail("deliver A HELLO to B");
    if (b.link_state != LINK_ESTABLISHED || b.pending_reset)
        return fail("B did not establish after A HELLO");

    if (pop_output_frame(&b, &fr) < 0)
        return fail("B response HELLO not generated");
    if (fr.type != LMUX_T_HELLO)
        return fail("B OPEN overtook HELLO response");
    if (feed_encoded(&a, &fr) < 0)
        return fail("deliver B HELLO response to A");
    if (a.link_state != LINK_ESTABLISHED || a.peer_session != b.local_session || a.pending_reset)
        return fail("A did not establish from B HELLO response");

    if (pop_output_frame(&b, &fr) < 0)
        return fail("B OPEN not generated after HELLO response");
    if (fr.type != LMUX_T_OPEN || fr.channel != 1)
        return fail("B first post-HELLO frame was not channel 1 OPEN");
    if (feed_encoded(&a, &fr) < 0)
        return fail("deliver B OPEN to A");
    if (!a.ch[0].remote_open || a.pending_reset)
        return fail("A did not accept B OPEN after handshake");

    return 0;
}

static int test_simultaneous_startup_hello_response_no_loop(void)
{
    struct app a, b;
    struct lmux_frame a_initial, b_initial, fr;

    init_app(&a);
    init_app(&b);
    a.local_session = 0xaaa00002u;
    b.local_session = 0xbbb00002u;

    if (pop_output_frame(&a, &a_initial) < 0 || a_initial.type != LMUX_T_HELLO)
        return fail("A initial simultaneous HELLO not generated");
    if (pop_output_frame(&b, &b_initial) < 0 || b_initial.type != LMUX_T_HELLO)
        return fail("B initial simultaneous HELLO not generated");

    if (feed_encoded(&a, &b_initial) < 0 || feed_encoded(&b, &a_initial) < 0)
        return fail("deliver simultaneous HELLOs");
    if (a.link_state != LINK_ESTABLISHED || b.link_state != LINK_ESTABLISHED)
        return fail("simultaneous HELLOs did not establish both peers");

    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("A HELLO response not generated");
    if (feed_encoded(&b, &fr) < 0)
        return fail("deliver A HELLO response");
    if (pop_output_frame(&b, &fr) < 0 || fr.type != LMUX_T_HELLO)
        return fail("B HELLO response not generated");
    if (feed_encoded(&a, &fr) < 0)
        return fail("deliver B HELLO response");

    if (a.pending_hello || b.pending_hello || a.pending_reset || b.pending_reset)
        return fail("duplicate HELLO responses created loop/reset");
    if (build_next_output(&a) != 0 || build_next_output(&b) != 0)
        return fail("unexpected extra HELLO after duplicate responses");
    return 0;
}

static void setup_socket_channel(struct app *a, int idx, int fd)
{
    a->ch[idx].ep.type = EP_SOCKET;
    a->ch[idx].ep.fd = fd;
    a->ch[idx].local_open = 1;
    a->ch[idx].remote_open = 1;
    a->ch[idx].local_generation++;
    if (a->ch[idx].local_generation == 0)
        a->ch[idx].local_generation = 1;
    a->ch[idx].remote_generation = 1;
    activate_channel(a, idx);
    a->ch[idx].credit_pending = 0;
    a->ch[idx].remote_credit = LMUX_RX_RING;
}

static int test_legacy_wire_compatibility(void)
{
    struct app a, old_side;
    struct lmux_frame fr;

    init_app(&a);
    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_HELLO ||
        fr.payload_len != 8 || !legacy_accepts_frame(&fr))
        return fail("current initial HELLO is not legacy-compatible");

    make_hello_features(&fr, 0x1234u, LMUX_FEATURE_STREAM_GEN);
    if (legacy_accepts_frame(&fr))
        return fail("legacy fixture accepted extended HELLO unexpectedly");
    make_frame(&fr, LMUX_T_OPEN, 3);
    fr.payload_len = 4;
    lmux_put_le32(fr.payload, 7);
    if (legacy_accepts_frame(&fr))
        return fail("legacy fixture accepted extended OPEN unexpectedly");

    init_app(&a);
    make_hello(&fr, 0x4567u);
    handle_frame(&a, &fr);
    if (a.link_state != LINK_ESTABLISHED || a.pending_reset || !a.pending_hello)
        return fail("current side did not establish with legacy HELLO");
    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_HELLO ||
        fr.payload_len != 8 || !legacy_accepts_frame(&fr))
        return fail("HELLO response to legacy peer is not legacy-compatible");

    setup_legacy_established_pair(&a, &old_side);
    a.ch[2].ep.type = EP_SOCKET;
    a.ch[2].local_open = 1;
    a.ch[2].local_generation = 11;
    a.ch[2].pending_open = 1;
    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_OPEN ||
        fr.payload_len != 0 || !legacy_accepts_frame(&fr))
        return fail("current OPEN is not legacy-compatible");
    a.ch[2].pending_close = 1;
    a.ch[2].pending_close_generation = 11;
    if (pop_output_frame(&a, &fr) < 0 || fr.type != LMUX_T_CLOSE ||
        fr.payload_len != 0 || !legacy_accepts_frame(&fr))
        return fail("current CLOSE is not legacy-compatible");
    return 0;
}

static int test_pending_socket_ignores_peer_close(void)
{
    struct app a;
    struct lmux_frame fr;
    int sp[2];
    uint32_t peer_session;
    unsigned long long resets;

    init_app(&a);
    make_hello(&fr, 0x7777u);
    handle_frame(&a, &fr);
    peer_session = a.peer_session;
    resets = a.resets;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return fail("pending socketpair");
    set_nonblock(sp[0]); set_nonblock(sp[1]);
    a.ch[2].ep.type = EP_SOCKET;
    a.ch[2].ep.fd = sp[0];
    a.ch[2].local_open = 1;
    a.ch[2].local_generation = 1;
    make_frame(&fr, LMUX_T_CLOSE, 3);
    handle_frame(&a, &fr);
    if (a.ch[2].ep.fd < 0 || !a.ch[2].local_open || a.ch[2].active)
        return fail("peer CLOSE killed pending socket client");
    if (a.resets != resets || a.peer_session != peer_session || a.pending_reset)
        return fail("pending socket peer CLOSE changed link state");
    close(sp[1]);
    close_fd(&a.ch[2].ep.fd);
    return 0;
}

static int send_app_bytes(struct app *from, struct app *to, int idx, int app_write_fd,
                          int app_read_fd, const uint8_t *data, size_t len)
{
    struct lmux_frame fr;
    uint8_t got[512];
    ssize_t r;
    if (write(app_write_fd, data, len) != (ssize_t)len)
        return -1;
    collect_tx(from, idx);
    while (from->ch[idx].tx.len) {
        if (pump_one_frame(from, to, &fr) < 0)
            return -1;
        if (fr.type == LMUX_T_CREDIT)
            continue;
        if (fr.type != LMUX_T_DATA)
            return -1;
        deliver_rx(to, idx);
    }
    r = read(app_read_fd, got, sizeof(got));
    if (r != (ssize_t)len || memcmp(got, data, len) != 0)
        return -1;
    return 0;
}

static int test_socket_pending_and_binary_stream(void)
{
    struct app a, b;
    int ap[2], bp[2];
    uint8_t data1[] = { 0x03 };
    uint8_t data2[] = { 0x00, 0x03, 0x14, 0xff, 0x41, 0x42 };
    struct lmux_frame fr;

    setup_established_pair(&a, &b);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ap) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, bp) < 0)
        return fail("socketpair stream");
    set_nonblock(ap[0]); set_nonblock(ap[1]);
    set_nonblock(bp[0]); set_nonblock(bp[1]);

    a.ch[2].ep.type = EP_SOCKET;
    a.ch[2].ep.fd = ap[0];
    a.ch[2].local_open = 1;
    a.ch[2].local_generation = 1;
    a.ch[2].pending_open = 1;
    if (pump_one_frame(&a, &b, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("target-first OPEN");
    if (a.ch[2].ep.fd < 0 || a.ch[2].active || !b.ch[2].remote_open)
        return fail("target-first pending did not wait");

    b.ch[2].ep.type = EP_SOCKET;
    b.ch[2].ep.fd = bp[0];
    b.ch[2].local_open = 1;
    b.ch[2].local_generation = 1;
    b.ch[2].pending_open = 1;
    update_channel_active(&b, 2);
    if (pump_one_frame(&b, &a, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("host-later OPEN");
    if (!a.ch[2].active || !b.ch[2].active)
        return fail("socket session did not activate");
    a.ch[2].credit_pending = 0;
    b.ch[2].credit_pending = 0;
    a.ch[2].remote_credit = LMUX_RX_RING;
    b.ch[2].remote_credit = LMUX_RX_RING;

    if (send_app_bytes(&a, &b, 2, ap[1], bp[1], data1, sizeof(data1)) < 0)
        return fail("raw 0x03 did not cross stream");
    if (send_app_bytes(&b, &a, 2, bp[1], ap[1], data2, sizeof(data2)) < 0)
        return fail("binary control bytes did not cross stream");

    close(ap[1]); close(bp[1]);
    close_fd(&a.ch[2].ep.fd);
    close_fd(&b.ch[2].ep.fd);
    return 0;
}

static int test_socket_host_first_pending(void)
{
    struct app a, b;
    int ap[2], bp[2];
    uint8_t data[] = { 'h', 'o', 's', 't', 0x03 };
    struct lmux_frame fr;

    setup_established_pair(&a, &b);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ap) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, bp) < 0)
        return fail("socketpair host-first");
    set_nonblock(ap[0]); set_nonblock(ap[1]);
    set_nonblock(bp[0]); set_nonblock(bp[1]);

    b.ch[2].ep.type = EP_SOCKET;
    b.ch[2].ep.fd = bp[0];
    b.ch[2].local_open = 1;
    b.ch[2].local_generation = 1;
    b.ch[2].pending_open = 1;
    if (pump_one_frame(&b, &a, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("host-first OPEN");
    if (b.ch[2].ep.fd < 0 || b.ch[2].active || !a.ch[2].remote_open)
        return fail("host-first pending did not wait");

    a.ch[2].ep.type = EP_SOCKET;
    a.ch[2].ep.fd = ap[0];
    a.ch[2].local_open = 1;
    a.ch[2].local_generation = 1;
    a.ch[2].pending_open = 1;
    update_channel_active(&a, 2);
    if (pump_one_frame(&a, &b, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("target-later OPEN");
    if (!a.ch[2].active || !b.ch[2].active)
        return fail("host-first session did not activate");
    a.ch[2].credit_pending = 0;
    b.ch[2].credit_pending = 0;
    a.ch[2].remote_credit = LMUX_RX_RING;
    b.ch[2].remote_credit = LMUX_RX_RING;
    if (send_app_bytes(&b, &a, 2, bp[1], ap[1], data, sizeof(data)) < 0)
        return fail("host-first binary data");
    close(ap[1]); close(bp[1]);
    close_fd(&a.ch[2].ep.fd);
    close_fd(&b.ch[2].ep.fd);
    return 0;
}

static int test_socket_stale_close_reconnect_and_isolation(void)
{
    struct app a, b;
    struct lmux_frame fr;
    int ap[2], bp[2], cp[2];

    setup_established_pair(&a, &b);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ap) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, bp) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, cp) < 0)
        return fail("socketpair stale close");
    set_nonblock(ap[0]); set_nonblock(ap[1]);
    set_nonblock(bp[0]); set_nonblock(bp[1]);
    set_nonblock(cp[0]); set_nonblock(cp[1]);

    setup_socket_channel(&a, 0, cp[0]);
    setup_socket_channel(&b, 0, cp[1]);
    setup_socket_channel(&a, 2, ap[0]);
    setup_socket_channel(&b, 2, bp[0]);
    a.ch[2].remote_generation = 2;
    b.ch[2].remote_generation = 2;

    make_frame(&fr, LMUX_T_CLOSE, 3);
    fr.payload_len = 4;
    lmux_put_le32(fr.payload, 1);
    handle_frame(&a, &fr);
    if (!a.ch[2].remote_open || a.ch[2].ep.fd < 0 || !a.ch[2].active)
        return fail("stale CLOSE killed new session");
    if (!a.ch[0].active || a.ch[0].ep.fd < 0)
        return fail("channel teardown leaked to console channel");

    lmux_put_le32(fr.payload, 2);
    handle_frame(&a, &fr);
    if (a.ch[2].remote_open || a.ch[2].ep.fd >= 0 || a.ch[2].active)
        return fail("matching CLOSE did not end session");
    if (!a.ch[0].active || a.ch[0].ep.fd < 0)
        return fail("matching channel close isolated badly");

    a.ch[2].ep.fd = ap[0];
    a.ch[2].local_open = 1;
    a.ch[2].local_generation = 3;
    a.ch[2].pending_open = 1;
    if (pump_one_frame(&a, &b, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("immediate reconnect OPEN missing");
    close(ap[1]); close(bp[1]);
    close_fd(&a.ch[2].ep.fd);
    close_fd(&b.ch[2].ep.fd);
    close_fd(&a.ch[0].ep.fd);
    close_fd(&b.ch[0].ep.fd);
    return 0;
}

static int test_socket_sequential_sessions(void)
{
    struct app a, b;
    struct lmux_frame fr;
    int i;
    setup_established_pair(&a, &b);
    for (i = 0; i < 3; i++) {
        int ap[2], bp[2];
        uint8_t byte = (uint8_t)('a' + i);
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, ap) < 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, bp) < 0)
            return fail("sequential socketpair");
        set_nonblock(ap[0]); set_nonblock(ap[1]);
        set_nonblock(bp[0]); set_nonblock(bp[1]);
        setup_socket_channel(&a, 2, ap[0]);
        setup_socket_channel(&b, 2, bp[0]);
        a.ch[2].pending_open = 1;
        b.ch[2].pending_open = 1;
        (void)pump_one_frame(&a, &b, &fr);
        (void)pump_one_frame(&b, &a, &fr);
        if (send_app_bytes(&a, &b, 2, ap[1], bp[1], &byte, 1) < 0)
            return fail("sequential session data");
        close_socket_client(&a, 2, 1);
        if (pump_one_frame(&a, &b, &fr) < 0 || fr.type != LMUX_T_CLOSE)
            return fail("sequential CLOSE");
        close(ap[1]); close(bp[1]);
        close_fd(&b.ch[2].ep.fd);
    }
    return 0;
}

static int test_socket_lifecycle_no_link_reset_100(void)
{
    struct app a, b;
    struct lmux_frame fr;
    uint32_t a_session, b_session;
    unsigned long long a_resets, b_resets;
    int i;

    setup_legacy_established_pair(&a, &b);
    a_session = a.peer_session;
    b_session = b.peer_session;
    a_resets = a.resets;
    b_resets = b.resets;
    for (i = 0; i < 100; i++) {
        int ap[2], bp[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, ap) < 0 ||
            socketpair(AF_UNIX, SOCK_STREAM, 0, bp) < 0)
            return fail("100-cycle socketpair");
        set_nonblock(ap[0]); set_nonblock(ap[1]);
        set_nonblock(bp[0]); set_nonblock(bp[1]);

        a.ch[2].ep.type = EP_SOCKET;
        a.ch[2].ep.fd = ap[0];
        a.ch[2].local_open = 1;
        a.ch[2].local_generation++;
        a.ch[2].pending_open = 1;
        if (pump_one_frame(&a, &b, &fr) < 0 || fr.type != LMUX_T_OPEN)
            return fail("100-cycle first OPEN");
        if (a.ch[2].active || a.ch[2].local_paired)
            return fail("100-cycle first side did not remain pending");

        b.ch[2].ep.type = EP_SOCKET;
        b.ch[2].ep.fd = bp[0];
        b.ch[2].local_open = 1;
        b.ch[2].local_generation++;
        b.ch[2].pending_open = 1;
        update_channel_active(&b, 2);
        if (pump_one_frame(&b, &a, &fr) < 0 || fr.type != LMUX_T_OPEN)
            return fail("100-cycle second OPEN");
        if (!a.ch[2].active || !b.ch[2].active ||
            !a.ch[2].local_paired || !b.ch[2].local_paired)
            return fail("100-cycle session did not pair");

        close_socket_client(&a, 2, 1);
        if (pump_one_frame(&a, &b, &fr) < 0 || fr.type != LMUX_T_CLOSE)
            return fail("100-cycle CLOSE");
        if (a.resets != a_resets || b.resets != b_resets ||
            a.peer_session != a_session || b.peer_session != b_session)
            return fail("100-cycle app lifecycle changed link state");
        close(ap[1]);
        close(bp[1]);
    }
    return 0;
}

static int test_heterogeneous_lifecycle_no_reset(void)
{
    struct app host, target;
    struct lmux_frame fr;
    int sp[2], fp[2];
    uint32_t host_session, target_session;
    unsigned long long host_resets, target_resets;

    setup_legacy_established_pair(&host, &target);
    host_session = host.peer_session;
    target_session = target.peer_session;
    host_resets = host.resets;
    target_resets = target.resets;

    target.ch[0].ep.type = EP_SHELL;
    target.ch[0].local_open = 1;
    target.ch[0].pending_open = 1;
    if (pump_one_frame(&target, &host, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("shell endpoint OPEN");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0)
        return fail("shell/socket socketpair");
    set_nonblock(sp[0]); set_nonblock(sp[1]);
    host.ch[0].ep.type = EP_SOCKET;
    host.ch[0].ep.fd = sp[0];
    host.ch[0].local_open = 1;
    host.ch[0].local_generation = 1;
    host.ch[0].pending_open = 1;
    update_channel_active(&host, 0);
    if (!host.ch[0].active)
        return fail("first socket<->shell connection did not activate");
    if (pump_one_frame(&host, &target, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("socket endpoint OPEN to shell");
    close_socket_client(&host, 0, 1);
    if (pump_one_frame(&host, &target, &fr) < 0 || fr.type != LMUX_T_CLOSE)
        return fail("socket<->shell CLOSE");
    close(sp[1]);

    target.ch[3].ep.type = EP_FILE;
    target.ch[3].local_open = 1;
    target.ch[3].pending_open = 1;
    if (pump_until_frame(&target, &host, LMUX_T_OPEN, 4, &fr) < 0)
        return fail("file endpoint OPEN");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fp) < 0)
        return fail("file/socket socketpair");
    set_nonblock(fp[0]); set_nonblock(fp[1]);
    host.ch[3].ep.type = EP_SOCKET;
    host.ch[3].ep.fd = fp[0];
    host.ch[3].local_open = 1;
    host.ch[3].local_generation = 1;
    host.ch[3].pending_open = 1;
    update_channel_active(&host, 3);
    if (!host.ch[3].active)
        return fail("socket<->file connection did not activate");
    if (pump_one_frame(&host, &target, &fr) < 0 || fr.type != LMUX_T_OPEN)
        return fail("socket endpoint OPEN to file");
    close_socket_client(&host, 3, 1);
    if (pump_one_frame(&host, &target, &fr) < 0 || fr.type != LMUX_T_CLOSE)
        return fail("socket<->file CLOSE");
    close(fp[1]);

    if (host.resets != host_resets || target.resets != target_resets ||
        host.peer_session != host_session || target.peer_session != target_session)
        return fail("heterogeneous lifecycle changed link state");
    return 0;
}

static int test_protocol_fault_still_resets(void)
{
    struct app a;
    struct lmux_frame fr;

    init_app(&a);
    make_hello(&fr, 0x12345678u);
    handle_frame(&a, &fr);
    a.ch[0].local_open = 1;
    a.ch[0].remote_open = 1;
    a.ch[0].active = 1;
    make_frame(&fr, LMUX_T_DATA, 1);
    fr.sequence = 9;
    fr.payload_len = 1;
    fr.payload[0] = 0xaa;
    handle_frame(&a, &fr);
    if (!a.pending_reset || a.resets != 1 || a.local_resets != 1 ||
        strstr(a.last_reset, "DATA sequence mismatch") == NULL)
        return fail("genuine protocol fault did not reset");
    return 0;
}

static int test_late_data_credit_inactive_no_reset(void)
{
    struct app a;
    struct lmux_frame fr;
    uint32_t peer_session;

    init_app(&a);
    make_hello(&fr, 0x91919191u);
    handle_frame(&a, &fr);
    peer_session = a.peer_session;
    make_frame(&fr, LMUX_T_CREDIT, 3);
    fr.payload_len = 2;
    lmux_put_le16(fr.payload, 16);
    handle_frame(&a, &fr);
    if (a.pending_reset || a.resets || a.peer_session != peer_session)
        return fail("inactive CREDIT caused link reset");
    make_frame(&fr, LMUX_T_DATA, 3);
    fr.sequence = 7;
    fr.payload_len = 3;
    fr.payload[0] = 'o';
    fr.payload[1] = 'l';
    fr.payload[2] = 'd';
    handle_frame(&a, &fr);
    if (a.pending_reset || a.resets || a.peer_session != peer_session)
        return fail("inactive DATA caused link reset");
    return 0;
}

static int test_winsize_frame(void)
{
    struct app a;
    struct lmux_frame fr;
    struct winsize ws;
    char p1[128], p2[128];

    init_app(&a);
    a.link_state = LINK_ESTABLISHED;
    snprintf(p1, sizeof(p1), "/tmp/lmux-ws1-%ld", (long)getpid());
    snprintf(p2, sizeof(p2), "/tmp/lmux-ws2-%ld", (long)getpid());
    a.ch[0].ep.type = EP_PTY;
    strcpy(a.ch[0].ep.path, p1);
    a.ch[1].ep.type = EP_PTY;
    strcpy(a.ch[1].ep.path, p2);
    if (create_pty(&a, 0) < 0 || create_pty(&a, 1) < 0)
        return fail("create PTY for winsize");

    make_frame(&fr, LMUX_T_WINSIZE, 1);
    fr.payload_len = 8;
    lmux_put_le16(fr.payload, 24);
    lmux_put_le16(fr.payload + 2, 80);
    lmux_put_le16(fr.payload + 4, 0);
    lmux_put_le16(fr.payload + 6, 0);
    handle_frame(&a, &fr);
    memset(&ws, 0, sizeof(ws));
    if (ioctl(a.ch[0].ep.fd, TIOCGWINSZ, &ws) < 0 || ws.ws_row != 24 || ws.ws_col != 80)
        return fail("80x24 winsize not applied");

    lmux_put_le16(fr.payload, 40);
    lmux_put_le16(fr.payload + 2, 120);
    handle_frame(&a, &fr);
    memset(&ws, 0, sizeof(ws));
    if (ioctl(a.ch[0].ep.fd, TIOCGWINSZ, &ws) < 0 || ws.ws_row != 40 || ws.ws_col != 120)
        return fail("120x40 winsize not applied");
    memset(&ws, 0, sizeof(ws));
    if (ioctl(a.ch[1].ep.fd, TIOCGWINSZ, &ws) < 0 || (ws.ws_row == 40 && ws.ws_col == 120))
        return fail("winsize leaked to channel 2");

    fr.payload_len = 8;
    lmux_put_le16(fr.payload, 0);
    lmux_put_le16(fr.payload + 2, 80);
    handle_frame(&a, &fr);
    if (a.pending_reset)
        return fail("malformed winsize caused protocol reset");
    close_fd(&a.ch[0].ep.fd);
    close_fd(&a.ch[1].ep.fd);
    unlink(p1);
    unlink(p2);
    return 0;
}

static int test_shell_lifecycle_smoke(void)
{
    struct app a;
    char path[128];
    char link_a[128], link_b[128];
    ssize_t la, lb;
    pid_t first, second;
    int loops;

    init_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-test-shell-%ld", (long)getpid());
    a.ch[0].ep.type = EP_SHELL;
    strcpy(a.ch[0].ep.path, path);
    if (create_pty(&a, 0) < 0)
        return fail("shell PTY create");
    la = readlink(path, link_a, sizeof(link_a) - 1);
    if (la <= 0) {
        cleanup(&a);
        return fail("shell symlink missing");
    }
    link_a[la] = 0;
    spawn_shell(&a, 0);
    first = a.ch[0].ep.child_pid;
    if (first <= 0) {
        cleanup(&a);
        return fail("shell did not spawn");
    }
    kill(first, SIGHUP);
    for (loops = 0; loops < 100 && a.ch[0].ep.child_pid == first; loops++) {
        reap_shells(&a);
        usleep(20000);
    }
    if (a.ch[0].ep.child_pid == first) {
        cleanup(&a);
        return fail("shell was not reaped");
    }
    a.ch[0].ep.respawn_at_ms = 0;
    supervise_shells(&a);
    second = a.ch[0].ep.child_pid;
    if (second <= 0 || second == first) {
        cleanup(&a);
        return fail("shell did not respawn");
    }
    lb = readlink(path, link_b, sizeof(link_b) - 1);
    if (lb <= 0) {
        cleanup(&a);
        return fail("shell symlink disappeared");
    }
    link_b[lb] = 0;
    if (strcmp(link_a, link_b) != 0) {
        cleanup(&a);
        return fail("shell symlink changed across respawn");
    }
    cleanup(&a);
    return 0;
}

static void init_file_app(struct app *a)
{
    init_app(a);
    a->ch[0].ep.type = EP_FILE;
    a->ch[0].local_open = 1;
    a->ch[0].remote_open = 1;
    a->ch[0].active = 1;
    a->ch[0].file.fd = -1;
}

static void feed_lxf(struct app *a, uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t rec_len = 0;
    lxf_encode_record(type, 0, payload, len, rec, sizeof(rec), &rec_len);
    ring_write(&a->ch[0].rx, rec, rec_len);
    file_service_step(a, 0);
}

static void feed_lxf_idx(struct app *a, int idx, uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t rec_len = 0;
    lxf_encode_record(type, 0, payload, len, rec, sizeof(rec), &rec_len);
    ring_write(&a->ch[idx].rx, rec, rec_len);
    file_service_step(a, idx);
}

static void feed_lxf_buffered(struct app *a, uint8_t type, const uint8_t *payload, uint32_t len)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t rec_len = 0;
    lxf_encode_record(type, 0, payload, len, rec, sizeof(rec), &rec_len);
    ring_write(&a->ch[0].rx, rec, rec_len);
}

static int pop_lxf_idx(struct app *a, int idx, uint8_t *type, uint8_t *payload, uint32_t *len);

static int pop_lxf(struct app *a, uint8_t *type, uint8_t *payload, uint32_t *len)
{
    return pop_lxf_idx(a, 0, type, payload, len);
}

static int pop_lxf_idx(struct app *a, int idx, uint8_t *type, uint8_t *payload, uint32_t *len)
{
    struct channel *c = &a->ch[idx];
    uint8_t rec[LXF_MAX_RECORD];
    uint8_t flags;
    const uint8_t *p;
    size_t n;

    int guard = 0;
    file_service_step(a, idx);
    while (c->tx.len < LXF_HEADER_LEN && guard++ < 64)
        file_service_step(a, idx);
    if (c->tx.len < LXF_HEADER_LEN)
        return -1;
    n = ring_peek_linear(&c->tx, &p);
    if (n >= LXF_HEADER_LEN) {
        memcpy(rec, p, LXF_HEADER_LEN);
        ring_drop(&c->tx, LXF_HEADER_LEN);
    } else {
        memcpy(rec, p, n);
        ring_drop(&c->tx, n);
        memcpy(rec + n, c->tx.buf + c->tx.r, LXF_HEADER_LEN - n);
        ring_drop(&c->tx, LXF_HEADER_LEN - n);
    }
    if (lxf_decode_header(rec, LXF_HEADER_LEN, type, &flags, len) < 0)
        return -1;
    guard = 0;
    while (c->tx.len < *len && guard++ < 64)
        file_service_step(a, idx);
    if (c->tx.len < *len)
        return -1;
    if (*len) {
        n = ring_peek_linear(&c->tx, &p);
        if (n >= *len) {
            memcpy(payload, p, *len);
            ring_drop(&c->tx, *len);
        } else {
            memcpy(payload, p, n);
            ring_drop(&c->tx, n);
            memcpy(payload + n, c->tx.buf + c->tx.r, *len - n);
            ring_drop(&c->tx, *len - n);
        }
    }
    return 0;
}

static void empty_sha(uint8_t digest[32])
{
    struct sha256_ctx sha;
    sha256_init(&sha);
    sha256_final(&sha, digest);
}

static int test_file_zero_put(void)
{
    struct app a;
    char path[128];
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], digest[32], type;
    uint32_t len;
    struct stat st;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-zero-put-%ld", (long)getpid());
    unlink(path);
    lmux_put_le32(p, 0600);
    lmux_put_le64(p + 4, 0);
    lmux_put_le32(p + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)p + 16, path);
    feed_lxf(&a, LXF_T_PUT_REQUEST, p, (uint32_t)(16 + strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("zero PUT request not accepted");
    empty_sha(digest);
    lmux_put_le64(p, 0);
    memcpy(p + 8, digest, 32);
    feed_lxf(&a, LXF_T_DONE, p, 40);
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("zero PUT did not complete");
    if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != 0) {
        unlink(path);
        return fail("zero PUT file missing");
    }
    unlink(path);
    return 0;
}

static int test_file_bad_hash_preserves_existing(void)
{
    struct app a;
    char path[128];
    int fd;
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    char buf[8] = {0};

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-badhash-%ld", (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("create badhash base");
    write(fd, "old", 3);
    close(fd);
    lmux_put_le32(p, 0600);
    lmux_put_le64(p + 4, 3);
    lmux_put_le32(p + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)p + 16, path);
    feed_lxf(&a, LXF_T_PUT_REQUEST, p, (uint32_t)(16 + strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("bad hash PUT request not accepted");
    memcpy(p, "new", 3);
    feed_lxf(&a, LXF_T_DATA, p, 3);
    memset(p, 0, 40);
    lmux_put_le64(p, 3);
    feed_lxf(&a, LXF_T_DONE, p, 40);
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_ERROR) {
        unlink(path);
        return fail("bad hash was not rejected");
    }
    fd = open(path, O_RDONLY);
    read(fd, buf, 3);
    close(fd);
    unlink(path);
    if (memcmp(buf, "old", 3) != 0)
        return fail("bad hash replaced existing file");
    return 0;
}

static int test_file_get_roundtrip(void)
{
    struct app a;
    char path[128];
    int fd;
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    int saw_data = 0, saw_done = 0;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-get-%ld", (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("create GET source");
    write(fd, "abc", 3);
    close(fd);
    strcpy((char *)p, path);
    feed_lxf(&a, LXF_T_GET_REQUEST, p, (uint32_t)(strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK || lmux_get_le64(out + 4) != 3) {
        unlink(path);
        return fail("GET metadata bad");
    }
    if (pop_lxf(&a, &type, out, &len) == 0 && type == LXF_T_DATA && len == 3 && memcmp(out, "abc", 3) == 0)
        saw_data = 1;
    if (pop_lxf(&a, &type, out, &len) == 0 && type == LXF_T_DONE && lmux_get_le64(out) == 3)
        saw_done = 1;
    unlink(path);
    if (!saw_data || !saw_done)
        return fail("GET data/DONE missing");
    return 0;
}

static int run_buffered_put_size(size_t data_len, int split_done)
{
    struct app a;
    char path[128];
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type, digest[32];
    uint32_t len;
    struct sha256_ctx sha;
    struct stat st;
    size_t i, off;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-buffered-put-%ld-%lu", (long)getpid(), (unsigned long)data_len);
    unlink(path);
    lmux_put_le32(p, 0600);
    lmux_put_le64(p + 4, data_len);
    lmux_put_le32(p + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)p + 16, path);
    feed_lxf(&a, LXF_T_PUT_REQUEST, p, (uint32_t)(16 + strlen(path) + 1));
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK)
        return fail("buffered PUT request not accepted");
    if (a.ch[0].file.state != FS_PUT || a.ch[0].tx.len != 0)
        return fail("buffered PUT request left bad state");

    sha256_init(&sha);
    for (i = 0; i < data_len; i++)
        p[i % sizeof(p)] = (uint8_t)(i * 17 + 3);
    for (off = 0; off < data_len; ) {
        size_t n = data_len - off;
        if (n > 400)
            n = 400;
        for (i = 0; i < n; i++)
            p[i] = (uint8_t)((off + i) * 17 + 3);
        sha256_update(&sha, p, n);
        feed_lxf_buffered(&a, LXF_T_DATA, p, (uint32_t)n);
        if (split_done)
            (void)drain_internal_work(&a);
        off += n;
    }
    sha256_final(&sha, digest);
    lmux_put_le64(p, data_len);
    memcpy(p + 8, digest, 32);
    feed_lxf_buffered(&a, LXF_T_DONE, p, 40);
    if (!drain_internal_work(&a)) {
        unlink(path);
        return fail("buffered PUT produced no internal progress");
    }
    if (pop_lxf(&a, &type, out, &len) < 0 || type != LXF_T_OK || len < 40) {
        unlink(path);
        return fail("buffered PUT did not send OK");
    }
    if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size != data_len) {
        unlink(path);
        return fail("buffered PUT final file bad");
    }
    unlink(path);
    return 0;
}

static int test_file_buffered_data_done(void)
{
    return run_buffered_put_size(32, 0);
}

static int test_file_exact_1134_put(void)
{
    return run_buffered_put_size(1134, 1);
}

static int test_file_no_event_after_done(void)
{
    return run_buffered_put_size(257, 1);
}

static int test_file_get_drain_no_stall(void)
{
    struct app a;
    char path[128];
    int fd;
    uint8_t p[LXF_MAX_PAYLOAD], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    int saw_done = 0, guard;

    init_file_app(&a);
    snprintf(path, sizeof(path), "/tmp/lmux-get-drain-%ld", (long)getpid());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fail("create GET drain source");
    write(fd, "abcdef", 6);
    close(fd);
    strcpy((char *)p, path);
    feed_lxf_buffered(&a, LXF_T_GET_REQUEST, p, (uint32_t)(strlen(path) + 1));
    for (guard = 0; guard < 16; guard++)
        (void)drain_internal_work(&a);
    while (pop_lxf(&a, &type, out, &len) == 0) {
        if (type == LXF_T_DONE) {
            saw_done = 1;
            break;
        }
        if (a.ch[0].tx.len == 0) {
            for (guard = 0; guard < 16; guard++)
                (void)drain_internal_work(&a);
        }
    }
    unlink(path);
    if (!saw_done)
        return fail("GET drain did not produce DONE");
    return 0;
}

static int spawn_cwd_child(const char *dir)
{
    int pipefd[2];
    char ready;
    pid_t pid;
    if (pipe(pipefd) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (chdir(dir) < 0)
            _exit(111);
        ready = 'x';
        (void)write(pipefd[1], &ready, 1);
        close(pipefd[1]);
        for (;;)
            pause();
    }
    close(pipefd[1]);
    if (read(pipefd[0], &ready, 1) != 1) {
        close(pipefd[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    close(pipefd[0]);
    return (int)pid;
}

static void stop_child(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
}

static void init_cwd_app(struct app *a)
{
    init_app(a);
    a->ch[3].ep.type = EP_FILE;
    a->ch[3].local_open = 1;
    a->ch[3].remote_open = 1;
    a->ch[3].active = 1;
    a->ch[3].file.fd = -1;
}

static int test_cwd_request_shell_channels(void)
{
    struct app a;
    char dir1[128], dir2[128];
    uint8_t req[1], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    pid_t p1 = -1, p2 = -1;
    int rc = 1;

    snprintf(dir1, sizeof(dir1), "/tmp/lmux-cwd1-%ld", (long)getpid());
    snprintf(dir2, sizeof(dir2), "/tmp/lmux-cwd2-%ld", (long)getpid());
    mkdir(dir1, 0700);
    mkdir(dir2, 0700);
    p1 = spawn_cwd_child(dir1);
    p2 = spawn_cwd_child(dir2);
    if (p1 <= 0 || p2 <= 0)
        goto out;

    init_cwd_app(&a);
    a.ch[0].ep.type = EP_SHELL;
    a.ch[0].ep.child_pid = p1;
    a.ch[1].ep.type = EP_SHELL;
    a.ch[1].ep.child_pid = p2;

    req[0] = 1;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_CWD_REPLY ||
        strcmp((char *)out, dir1) != 0)
        goto out;
    req[0] = 2;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_CWD_REPLY ||
        strcmp((char *)out, dir2) != 0)
        goto out;
    rc = 0;
out:
    stop_child(p1);
    stop_child(p2);
    rmdir(dir1);
    rmdir(dir2);
    return rc ? fail("CWD request shell channels") : 0;
}

static int test_cwd_request_errors(void)
{
    struct app a;
    uint8_t req[1], out[LXF_MAX_PAYLOAD], type;
    uint32_t len;

    init_cwd_app(&a);
    req[0] = 3;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_ERROR)
        return fail("CWD invalid channel error");

    init_cwd_app(&a);
    a.ch[0].ep.type = EP_SHELL;
    a.ch[0].ep.child_pid = -1;
    req[0] = 1;
    feed_lxf_idx(&a, 3, LXF_T_CWD_REQUEST, req, 1);
    if (pop_lxf_idx(&a, 3, &type, out, &len) < 0 || type != LXF_T_ERROR)
        return fail("CWD absent shell error");
    return 0;
}

int main(void)
{
    if (test_garbage_before_hello() ||
        test_open_credit_before_hello() ||
        test_reset_stale_open_then_hello() ||
        test_duplicate_hello() ||
        test_new_session_hello() ||
        test_empty_cobs_record() ||
        test_lost_first_hello_converges() ||
        test_simultaneous_startup_hello_response_no_loop() ||
        test_legacy_wire_compatibility() ||
        test_pending_socket_ignores_peer_close() ||
        test_socket_pending_and_binary_stream() ||
        test_socket_host_first_pending() ||
        test_socket_stale_close_reconnect_and_isolation() ||
        test_socket_sequential_sessions() ||
        test_socket_lifecycle_no_link_reset_100() ||
        test_heterogeneous_lifecycle_no_reset() ||
        test_protocol_fault_still_resets() ||
        test_late_data_credit_inactive_no_reset() ||
        test_winsize_frame() ||
        test_shell_lifecycle_smoke() ||
        test_file_zero_put() ||
        test_file_bad_hash_preserves_existing() ||
        test_file_get_roundtrip() ||
        test_file_buffered_data_done() ||
        test_file_exact_1134_put() ||
        test_file_no_event_after_done() ||
        test_file_get_drain_no_stall() ||
        test_cwd_request_shell_channels() ||
        test_cwd_request_errors())
        return 1;
    puts("lmuxd state selftest ok");
    return 0;
}
