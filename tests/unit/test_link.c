#include "link_endpoint.h"
#include "boot_protocol.h"
#include "link_protocol.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *msg)
{
    fprintf(stderr, "link test failed: %s\n", msg);
    return 1;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int unhex(const char *hex, uint8_t *out, size_t *out_len)
{
    size_t n = strlen(hex), i;
    if (n % 2)
        return -1;
    for (i = 0; i < n / 2; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = n / 2;
    return 0;
}

static int test_crc32c(void)
{
    if (lmux_crc32c((const uint8_t *)"", 0) != 0x00000000u)
        return fail("crc empty");
    if (lmux_crc32c((const uint8_t *)"123456789", 9) != 0xe3069283u)
        return fail("crc 123456789");
    return 0;
}

static int check_encoded(const char *name, const char *hex)
{
    uint8_t buf[LMUX_LINK_MAX_ENCODED];
    size_t len;
    struct lmux_link_frame fr;
    if (unhex(hex, buf, &len) < 0 || len == 0 || buf[len - 1] != 0)
        return fail(name);
    if (lmux_link_decode_frame(buf, len - 1, &fr) < 0)
        return fail(name);
    return 0;
}

static int test_golden_vectors(void)
{
    if (check_encoded("hello golden",
        "064c4d4f310102010220010102180101098877665544332211010101010101010201021802030101010210010102040101020401010101010505aa32e000"))
        return 1;
    if (check_encoded("transport golden",
        "064c4d4f3101020202200101020501010a8877665544332211010101010101010103034106ff8cefd9e400"))
        return 1;
    if (check_encoded("boot request golden",
        "064c4d4f3101020302200101020c01010a887766554433221102010101010101020101010d0807060504030201a62729ab00"))
        return 1;
    if (check_encoded("boot response golden",
        "064c4d4f3101020402200101022401010109ffeeddccbbaa990701010101010102010101010101010a08070605040302010102010203010101021001010204010101010105a82fa19000"))
        return 1;
    return 0;
}

static int test_boot_protocol(void)
{
    uint8_t buf[128], body[128], data[LMUX_BOOT_UPLOAD_MAX_CHUNK];
    uint32_t len = 0, body_len = 0;
    struct lmux_boot_request req;
    struct lmux_boot_response resp;
    struct lmux_boot_info info = {1, 1, 3, 4096, 1024};
    struct lmux_boot_info info2;
    struct lmux_boot_worker_status ws, ws2;
    struct lmux_boot_upload_begin ub;
    struct lmux_boot_upload_begin_response ubr;
    struct lmux_boot_upload_data ud;
    struct lmux_boot_upload_data_response udr;
    struct lmux_boot_upload_end ue;
    struct lmux_boot_upload_end_response uer;
    struct lmux_boot_activate act;
    struct lmux_boot_activate_response ar;
    struct lmux_boot_update_status us, us2;
    struct lmux_boot_supervisor_install_response sir;
    uint8_t sha2[32];
    uint64_t abort_id;
    int i;

    if (lmux_boot_make_request(buf, sizeof(buf), &len, LMUX_BOOT_GET_INFO,
                               0x0102030405060708ULL, NULL, 0) < 0 ||
        len != LMUX_BOOT_REQ_HEADER_LEN ||
        lmux_boot_parse_request(buf, len, &req) < 0 ||
        req.opcode != LMUX_BOOT_GET_INFO ||
        req.request_id != 0x0102030405060708ULL ||
        req.body_len != 0)
        return fail("boot request roundtrip");
    buf[3] = 1;
    if (lmux_boot_parse_request(buf, len, &req) < 0)
        return fail("boot request flags parse");
    if (lmux_boot_parse_request(buf, 4, &req) == 0)
        return fail("boot request truncation");

    if (lmux_boot_encode_get_info_response(body, sizeof(body), &body_len, &info) < 0 ||
        body_len != 20 ||
        lmux_boot_decode_get_info_response(body, body_len, &info2) < 0 ||
        info2.outer_version != 1 || info2.boot_version != 1 ||
        info2.capabilities != 3 || info2.max_outer_payload != 4096 ||
        info2.max_transport_payload != 1024)
        return fail("boot get_info body");
    memset(&ws, 0, sizeof(ws));
    ws.worker_state = LMUX_BOOT_WORKER_ACTIVE;
    ws.worker_incarnation = 7;
    ws.uptime_ms = 1234;
    ws.restart_count = 2;
    if (lmux_boot_encode_worker_status(body, sizeof(body), &body_len, &ws) < 0 ||
        body_len != 32 ||
        lmux_boot_decode_worker_status(body, body_len, &ws2) < 0 ||
        ws2.worker_state != LMUX_BOOT_WORKER_ACTIVE ||
        ws2.worker_incarnation != 7 || ws2.uptime_ms != 1234 ||
        ws2.restart_count != 2)
        return fail("boot worker status body");
    if (lmux_boot_make_response(buf, sizeof(buf), &len, LMUX_BOOT_GET_INFO,
                                LMUX_BOOT_STATUS_OK, req.request_id,
                                body, body_len) < 0 ||
        lmux_boot_parse_response(buf, len, &resp) < 0 ||
        resp.opcode != LMUX_BOOT_GET_INFO ||
        resp.status != LMUX_BOOT_STATUS_OK ||
        resp.request_id != req.request_id ||
        resp.body_len != body_len)
        return fail("boot response roundtrip");
    buf[4] = 1;
    if (lmux_boot_parse_response(buf, len, &resp) == 0)
        return fail("boot response reserved");
    if (lmux_boot_encode_upload_begin(body, sizeof(body), &body_len, 12345) < 0 ||
        body_len != 8 ||
        lmux_boot_decode_upload_begin(body, body_len, &ub) < 0 ||
        ub.declared_size != 12345 ||
        lmux_boot_decode_upload_begin(body, 7, &ub) == 0)
        return fail("boot upload begin");
    if (lmux_boot_encode_upload_begin_response(body, sizeof(body), &body_len,
                                               99, 1024) < 0 ||
        body_len != 12 ||
        lmux_boot_decode_upload_begin_response(body, body_len, &ubr) < 0 ||
        ubr.upload_id != 99 || ubr.max_chunk != 1024)
        return fail("boot upload begin response");
    for (i = 0; i < (int)sizeof(data); i++)
        data[i] = (uint8_t)i;
    if (lmux_boot_encode_upload_data(body, sizeof(body), &body_len, 99, 64,
                                     data, 17) < 0 ||
        body_len != 33 ||
        lmux_boot_decode_upload_data(body, body_len, &ud) < 0 ||
        ud.upload_id != 99 || ud.offset != 64 || ud.data_len != 17 ||
        memcmp(ud.data, data, 17) != 0 ||
        lmux_boot_encode_upload_data(body, 20, &body_len, 99, 64,
                                     data, 17) == 0)
        return fail("boot upload data");
    if (lmux_boot_encode_upload_data_response(body, sizeof(body), &body_len,
                                              81) < 0 ||
        lmux_boot_decode_upload_data_response(body, body_len, &udr) < 0 ||
        udr.next_offset != 81)
        return fail("boot upload data response");
    for (i = 0; i < 32; i++)
        data[i] = (uint8_t)(255 - i);
    if (lmux_boot_encode_upload_end(body, sizeof(body), &body_len, 99, data) < 0 ||
        body_len != 40 ||
        lmux_boot_decode_upload_end(body, body_len, &ue) < 0 ||
        ue.upload_id != 99 || memcmp(ue.expected_sha256, data, 32) != 0 ||
        lmux_boot_decode_upload_end(body, 39, &ue) == 0)
        return fail("boot upload end");
    if (lmux_boot_encode_upload_end_response(body, sizeof(body), &body_len,
                                             12345, data) < 0 ||
        lmux_boot_decode_upload_end_response(body, body_len, &uer) < 0 ||
        uer.size != 12345 || memcmp(uer.sha256, data, 32) != 0)
        return fail("boot upload end response");
    if (lmux_boot_encode_upload_abort(body, sizeof(body), &body_len, 99) < 0 ||
        body_len != 8 ||
        lmux_boot_decode_upload_abort(body, body_len, &abort_id) < 0 ||
        abort_id != 99 ||
        lmux_boot_decode_upload_abort(body, 7, &abort_id) == 0)
        return fail("boot upload abort");
    if (lmux_boot_encode_activate(body, sizeof(body), &body_len, data, 30000) < 0 ||
        lmux_boot_decode_activate(body, body_len, &act) < 0 ||
        memcmp(act.candidate_sha256, data, 32) != 0 ||
        act.confirm_timeout_ms != 30000 ||
        lmux_boot_decode_activate(body, 35, &act) == 0)
        return fail("boot activate");
    if (lmux_boot_encode_activate_response(body, sizeof(body), &body_len,
                                           123, 9, 30000,
                                           LMUX_BOOT_UPDATE_TESTING) < 0 ||
        lmux_boot_decode_activate_response(body, body_len, &ar) < 0 ||
        ar.activation_id != 123 || ar.generation != 9 ||
        ar.confirm_timeout_ms != 30000 ||
        ar.phase != LMUX_BOOT_UPDATE_TESTING)
        return fail("boot activate response");
    if (lmux_boot_encode_activation_id(body, sizeof(body), &body_len, 123) < 0 ||
        lmux_boot_decode_activation_id(body, body_len, &abort_id) < 0 ||
        abort_id != 123 ||
        lmux_boot_encode_activation_id(body, sizeof(body), &body_len, 0) == 0)
        return fail("boot activation id");
    memset(&us, 0, sizeof(us));
    us.phase = LMUX_BOOT_UPDATE_TESTING;
    us.flags = LMUX_BOOT_UPDATE_TIMER_RUNNING;
    us.generation = 7;
    us.activation_id = 123;
    us.confirm_remaining_ms = 456;
    us.last_update_error = LMUX_BOOT_STATUS_OK;
    memcpy(us.confirmed_sha256, data, 32);
    memcpy(us.candidate_sha256, data, 32);
    us.candidate_sha256[0] ^= 0x55;
    memcpy(us.running_sha256, us.candidate_sha256, 32);
    if (lmux_boot_encode_update_status(body, sizeof(body), &body_len, &us) < 0 ||
        body_len != LMUX_BOOT_UPDATE_STATUS_LEN ||
        lmux_boot_decode_update_status(body, body_len, &us2) < 0 ||
        us2.phase != us.phase || us2.flags != us.flags ||
        us2.generation != us.generation ||
        us2.activation_id != us.activation_id ||
        us2.confirm_remaining_ms != us.confirm_remaining_ms ||
        us2.last_update_error != us.last_update_error ||
        memcmp(us2.running_sha256, us.running_sha256, 32) != 0 ||
        lmux_boot_decode_update_status(body, body_len - 1, &us2) == 0)
        return fail("boot update status");
    if (lmux_boot_encode_sha256(body, sizeof(body), &body_len, data) < 0 ||
        body_len != 32 ||
        lmux_boot_decode_sha256(body, body_len, sha2) < 0 ||
        memcmp(sha2, data, 32) != 0 ||
        lmux_boot_decode_sha256(body, 31, sha2) == 0)
        return fail("boot supervisor sha");
    if (lmux_boot_encode_supervisor_install_response(
            body, sizeof(body), &body_len, data,
            LMUX_BOOT_SUPERVISOR_INSTALL_INSTALLED |
            LMUX_BOOT_SUPERVISOR_INSTALL_ALREADY) < 0 ||
        body_len != 40 ||
        lmux_boot_decode_supervisor_install_response(body, body_len, &sir) < 0 ||
        memcmp(sir.sha256, data, 32) != 0 ||
        sir.flags != (LMUX_BOOT_SUPERVISOR_INSTALL_INSTALLED |
                      LMUX_BOOT_SUPERVISOR_INSTALL_ALREADY)) {
        return fail("boot supervisor install response");
    }
    body[36] = 1;
    if (lmux_boot_decode_supervisor_install_response(body, body_len, &sir) == 0)
        return fail("boot supervisor install reserved");
    return 0;
}

static int feed_record(struct lmux_link_endpoint *ep, const uint8_t *enc,
                       size_t len, unsigned *events)
{
    if (len == 0 || enc[len - 1] != 0)
        return -1;
    return lmux_link_input_bytes(ep, enc, len, events);
}

static int transfer_output(struct lmux_link_endpoint *from,
                           struct lmux_link_endpoint *to,
                           unsigned *from_events,
                           unsigned *to_events)
{
    size_t len = lmux_link_output_len(from);
    if (len == 0) {
        (void)lmux_link_build_output(from);
        len = lmux_link_output_len(from);
    }
    if (len == 0)
        return -1;
    if (feed_record(to, lmux_link_output_ptr(from), len, to_events) < 0)
        return -1;
    lmux_link_output_consume_events(from, len, from_events);
    return 0;
}

static int establish_pair(struct lmux_link_endpoint *h,
                          struct lmux_link_endpoint *s)
{
    unsigned hev = 0, sev = 0;
    if (transfer_output(h, s, &hev, &sev) < 0 ||
        (sev & LMUX_LINK_EV_ESTABLISHED))
        return -1;
    hev = sev = 0;
    if (transfer_output(s, h, &sev, &hev) < 0 ||
        (sev & LMUX_LINK_EV_ESTABLISHED) ||
        (hev & LMUX_LINK_EV_ESTABLISHED))
        return -1;
    hev = sev = 0;
    if (transfer_output(h, s, &hev, &sev) < 0 ||
        (sev & LMUX_LINK_EV_ESTABLISHED) ||
        (hev & LMUX_LINK_EV_ESTABLISHED))
        return -1;
    hev = sev = 0;
    if (transfer_output(s, h, &sev, &hev) < 0 ||
        !(hev & LMUX_LINK_EV_ESTABLISHED) ||
        !(sev & LMUX_LINK_EV_ESTABLISHED))
        return -1;
    return h->state == LMUX_LINK_ESTABLISHED &&
        s->state == LMUX_LINK_ESTABLISHED ? 0 : -1;
}

static int test_hello_and_transport(void)
{
    struct lmux_link_endpoint h, s;
    uint8_t tmp[128], got[128];
    unsigned ev = 0;
    size_t n;
    lmux_link_endpoint_init(&h, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&s, LMUX_LINK_ROLE_SUPERVISOR);
    lmux_link_endpoint_begin(&h, 0x1111);
    lmux_link_endpoint_begin(&s, 0x2222);
    if (feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev) < 0 ||
        !(ev & LMUX_LINK_EV_PEER_HELLO) ||
        !(ev & LMUX_LINK_EV_HELLO_ACK_QUEUED) ||
        (ev & LMUX_LINK_EV_ESTABLISHED))
        return fail("peer hello established too early");
    if (lmux_link_transport_write(&s, (const uint8_t *)"x", 1) != 0 ||
        lmux_link_boot_queue_frame(&s, LMUX_LINK_T_BOOT_REQUEST,
                                   (const uint8_t *)"x", 1) == 0)
        return fail("transport boot not gated before hello reply");
    lmux_link_output_consume(&h, lmux_link_output_len(&h));
    ev = 0;
    if (transfer_output(&s, &h, &ev, &ev) < 0 ||
        s.state == LMUX_LINK_ESTABLISHED ||
        h.state == LMUX_LINK_ESTABLISHED)
        return fail("supervisor hello established too early");
    ev = 0;
    if (transfer_output(&h, &s, &ev, &ev) < 0 ||
        s.state == LMUX_LINK_ESTABLISHED ||
        h.state == LMUX_LINK_ESTABLISHED)
        return fail("host ack established too early");
    ev = 0;
    if (transfer_output(&s, &h, &ev, &ev) < 0 ||
        h.state != LMUX_LINK_ESTABLISHED ||
        s.state != LMUX_LINK_ESTABLISHED)
        return fail("hello ack handshake");

    memset(tmp, 0, sizeof(tmp));
    tmp[0] = 0x00;
    tmp[1] = 0x03;
    tmp[2] = 0xff;
    for (n = 3; n < sizeof(tmp); n++)
        tmp[n] = (uint8_t)n;
    if (lmux_link_transport_write(&h, tmp, sizeof(tmp)) != sizeof(tmp))
        return fail("transport write");
    while (lmux_link_build_output(&h) > 0) {
        ev = 0;
        if (feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev) < 0)
            return fail("transport feed");
        lmux_link_output_consume(&h, lmux_link_output_len(&h));
    }
    if (lmux_link_transport_rx_len(&s) != sizeof(tmp))
        return fail("transport rx len");
    if (lmux_link_transport_read(&s, got, sizeof(got)) != sizeof(tmp) ||
        memcmp(tmp, got, sizeof(tmp)) != 0)
        return fail("transport exact");
    return 0;
}

static int test_sequence_gap_fails(void)
{
    struct lmux_link_endpoint h, s;
    unsigned ev = 0;
    struct lmux_link_frame fr;
    lmux_link_endpoint_init(&h, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&s, LMUX_LINK_ROLE_SUPERVISOR);
    lmux_link_endpoint_begin(&h, 0x1111);
    lmux_link_endpoint_begin(&s, 0x2222);
    if (establish_pair(&h, &s) < 0)
        return fail("gap setup");
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX_LINK_T_TRANSPORT;
    fr.header_len = LMUX_LINK_HEADER_LEN;
    fr.conn_id = h.local_conn_id;
    fr.frame_seq = 2;
    fr.payload[0] = 1;
    fr.payload_len = 1;
    if (lmux_link_encode_frame(&fr, h.out, sizeof(h.out), &h.out_len) < 0)
        return fail("gap encode");
    ev = 0;
    (void)feed_record(&s, h.out, h.out_len, &ev);
    if (!(ev & LMUX_LINK_EV_DOWN) || s.state != LMUX_LINK_ERROR_RESETTING)
        return fail("gap did not fail session");
    return 0;
}

static int test_crc_failure_fails_established(void)
{
    struct lmux_link_endpoint h, s;
    unsigned ev = 0;
    lmux_link_endpoint_init(&h, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&s, LMUX_LINK_ROLE_SUPERVISOR);
    lmux_link_endpoint_begin(&h, 0x1111);
    lmux_link_endpoint_begin(&s, 0x2222);
    if (establish_pair(&h, &s) < 0)
        return fail("crc setup handshake");
    if (lmux_link_transport_write(&h, (const uint8_t *)"x", 1) != 1 ||
        lmux_link_build_output(&h) <= 0)
        return fail("crc setup");
    h.out[h.out_len > 5 ? h.out_len - 5 : 0] ^= 0x55;
    ev = 0;
    (void)feed_record(&s, h.out, h.out_len, &ev);
    if (!(ev & LMUX_LINK_EV_DOWN) || s.state != LMUX_LINK_ERROR_RESETTING)
        return fail("crc failure did not fail session");
    return 0;
}

static int test_peer_restart_requires_fresh_local_hello(void)
{
    struct lmux_link_endpoint h, s, hdup, h2;
    struct lmux_link_frame fr;
    unsigned ev = 0;
    lmux_link_endpoint_init(&h, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&s, LMUX_LINK_ROLE_SUPERVISOR);
    lmux_link_endpoint_init(&hdup, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&h2, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_begin(&h, 0x1111);
    lmux_link_endpoint_begin(&s, 0x2222);
    if (establish_pair(&h, &s) < 0)
        return fail("restart setup s");

    lmux_link_endpoint_begin(&hdup, 0x1111);
    ev = 0;
    if (feed_record(&s, lmux_link_output_ptr(&hdup), lmux_link_output_len(&hdup), &ev) < 0 ||
        (ev & LMUX_LINK_EV_PEER_RESTART))
        return fail("duplicate same-peer hello restarted");

    lmux_link_endpoint_begin(&h2, 0x3333);
    ev = 0;
    if (feed_record(&s, lmux_link_output_ptr(&h2), lmux_link_output_len(&h2), &ev) < 0 ||
        !(ev & LMUX_LINK_EV_PEER_RESTART) ||
        !s.pending_peer_hello.valid ||
        s.pending_peer_hello.conn_id != 0x3333)
        return fail("new peer hello did not request restart");
    if (lmux_link_endpoint_restart_from_pending(&s, 0x4444, &ev) < 0 ||
        (ev & LMUX_LINK_EV_ESTABLISHED) ||
        !(ev & LMUX_LINK_EV_HELLO_ACK_QUEUED) ||
        s.local_conn_id != 0x4444 ||
        s.peer_conn_id != 0x3333 ||
        s.next_tx_seq != 1)
        return fail("restart did not queue fresh pair");
    if (lmux_link_decode_frame(lmux_link_output_ptr(&s),
                               lmux_link_output_len(&s) - 1, &fr) < 0 ||
        fr.type != LMUX_LINK_T_HELLO ||
        fr.conn_id != 0x4444 ||
        fr.frame_seq != 0)
        return fail("fresh restart hello encoding");
    lmux_link_output_consume(&s, lmux_link_output_len(&s));
    if (lmux_link_build_output(&s) < 0 ||
        lmux_link_decode_frame(lmux_link_output_ptr(&s),
                               lmux_link_output_len(&s) - 1, &fr) < 0 ||
        fr.type != LMUX_LINK_T_HELLO_ACK ||
        fr.conn_id != 0x4444 ||
        fr.frame_seq != 0)
        return fail("fresh restart ack encoding");
    return 0;
}

static int test_hello_ack_payload_and_lost_ack(void)
{
    struct lmux_link_endpoint h, s;
    uint8_t payload[16];
    uint32_t payload_len = 0;
    uint64_t ack = 0;
    unsigned hev = 0, sev = 0;

    if (lmux_link_make_hello_ack_payload(0x1122334455667788ULL,
                                         payload, sizeof(payload),
                                         &payload_len) < 0 ||
        payload_len != 8 ||
        lmux_link_parse_hello_ack_payload(payload, payload_len, &ack) < 0 ||
        ack != 0x1122334455667788ULL)
        return fail("hello ack payload roundtrip");

    lmux_link_endpoint_init(&h, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&s, LMUX_LINK_ROLE_SUPERVISOR);
    lmux_link_endpoint_begin(&h, 0x1111);
    lmux_link_endpoint_begin(&s, 0x2222);

    if (transfer_output(&h, &s, &hev, &sev) < 0 ||
        h.state != LMUX_LINK_WAIT_HELLO ||
        s.state != LMUX_LINK_WAIT_HELLO)
        return fail("lost ack setup host hello");
    hev = sev = 0;
    if (transfer_output(&s, &h, &sev, &hev) < 0 ||
        h.state != LMUX_LINK_WAIT_HELLO ||
        s.state != LMUX_LINK_WAIT_HELLO)
        return fail("lost ack setup supervisor hello");

    /* Drop host->supervisor ACK after it is physically committed locally. */
    if (lmux_link_build_output(&h) < 0 ||
        lmux_link_output_len(&h) == 0)
        return fail("lost ack host ack build");
    lmux_link_output_consume_events(&h, lmux_link_output_len(&h), &hev);
    if (!(hev & LMUX_LINK_EV_HELLO_ACK_COMMITTED) ||
        h.state != LMUX_LINK_WAIT_HELLO ||
        s.state != LMUX_LINK_WAIT_HELLO)
        return fail("lost ack host ack drop");

    hev = sev = 0;
    if (transfer_output(&s, &h, &sev, &hev) < 0 ||
        h.state != LMUX_LINK_ESTABLISHED ||
        s.state != LMUX_LINK_WAIT_HELLO)
        return fail("lost ack one side established setup");

    if (lmux_link_endpoint_tick(&s, 1000) < 0 ||
        lmux_link_endpoint_tick(&s, 1300) <= 0)
        return fail("lost ack retry hello");
    hev = sev = 0;
    if (transfer_output(&s, &h, &sev, &hev) < 0 ||
        (hev & LMUX_LINK_EV_PEER_RESTART) ||
        h.state != LMUX_LINK_ESTABLISHED ||
        s.state != LMUX_LINK_WAIT_HELLO)
        return fail("established duplicate hello handling");
    if (lmux_link_build_output(&h) < 0 ||
        lmux_link_output_len(&h) == 0)
        return fail("established duplicate ack build");
    hev = sev = 0;
    if (transfer_output(&h, &s, &hev, &sev) < 0 ||
        h.state != LMUX_LINK_ESTABLISHED ||
        s.state != LMUX_LINK_ESTABLISHED)
        return fail("lost ack did not converge");
    if (lmux_link_build_output(&h) != 0 || lmux_link_output_len(&h) != 0 ||
        lmux_link_build_output(&s) != 0 || lmux_link_output_len(&s) != 0)
        return fail("hello ack ping pong");
    return 0;
}

static int test_duplicate_hello_ack_coalescing(void)
{
    struct lmux_link_endpoint h, s, hdup;
    struct lmux_link_frame fr;
    unsigned ev = 0;
    size_t ack_len;
    int i;

    lmux_link_endpoint_init(&h, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&s, LMUX_LINK_ROLE_SUPERVISOR);
    lmux_link_endpoint_init(&hdup, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_begin(&h, 0x1111);
    lmux_link_endpoint_begin(&s, 0x2222);
    if (establish_pair(&h, &s) < 0)
        return fail("coalesce setup");

    lmux_link_endpoint_begin(&hdup, 0x1111);
    for (i = 0; i < 8; i++) {
        ev = 0;
        if (feed_record(&s, lmux_link_output_ptr(&hdup),
                        lmux_link_output_len(&hdup), &ev) < 0 ||
            (ev & LMUX_LINK_EV_PEER_RESTART) ||
            s.state != LMUX_LINK_ESTABLISHED)
            return fail("coalesce duplicate hello feed");
        if (i == 0 && !(ev & LMUX_LINK_EV_HELLO_ACK_QUEUED))
            return fail("coalesce first ack not queued");
        if (i > 0 && (ev & LMUX_LINK_EV_HELLO_ACK_QUEUED))
            return fail("coalesce queued duplicate ack");
    }
    ack_len = lmux_link_output_len(&s);
    if (ack_len == 0 ||
        lmux_link_decode_frame(lmux_link_output_ptr(&s), ack_len - 1, &fr) < 0 ||
        fr.type != LMUX_LINK_T_HELLO_ACK ||
        s.pending_ack)
        return fail("coalesce one in-flight ack");
    lmux_link_output_consume(&s, ack_len);
    if (lmux_link_output_len(&s) != 0 || s.pending_ack)
        return fail("coalesce ack not cleared");

    ev = 0;
    if (feed_record(&s, lmux_link_output_ptr(&hdup),
                    lmux_link_output_len(&hdup), &ev) < 0 ||
        !(ev & LMUX_LINK_EV_HELLO_ACK_QUEUED) ||
        lmux_link_output_len(&s) == 0)
        return fail("coalesce later duplicate fresh ack");
    return 0;
}

static int test_boot_interleaving_and_reset(void)
{
    struct lmux_link_endpoint h, s;
    unsigned ev = 0;
    uint8_t req[32], got[32], type;
    uint32_t req_len = 0, got_len = 0;
    const uint8_t a[] = {'A'};
    const uint8_t b[] = {'B'};
    lmux_link_endpoint_init(&h, LMUX_LINK_ROLE_HOST);
    lmux_link_endpoint_init(&s, LMUX_LINK_ROLE_SUPERVISOR);
    h.local_caps |= LMUX_LINK_CAP_BOOT_INFO;
    s.local_caps |= LMUX_LINK_CAP_BOOT_INFO;
    lmux_link_endpoint_begin(&h, 0x1111);
    lmux_link_endpoint_begin(&s, 0x2222);
    if (establish_pair(&h, &s) < 0)
        return fail("boot interleave hello");
    if (!(h.negotiated_caps & LMUX_LINK_CAP_BOOT_INFO) ||
        !(s.negotiated_caps & LMUX_LINK_CAP_BOOT_INFO))
        return fail("boot cap negotiation");
    if (lmux_boot_make_request(req, sizeof(req), &req_len, LMUX_BOOT_GET_INFO,
                               9, NULL, 0) < 0)
        return fail("boot request make interleave");
    if (lmux_link_transport_write(&h, a, sizeof(a)) != sizeof(a))
        return fail("boot interleave queue");
    if (lmux_link_build_output(&h) <= 0)
        return fail("boot interleave first transport");
    ev = 0;
    if (feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev) < 0)
        return fail("boot interleave first feed");
    lmux_link_output_consume(&h, lmux_link_output_len(&h));
    if (lmux_link_boot_queue_frame(&h, LMUX_LINK_T_BOOT_REQUEST, req, req_len) < 0 ||
        lmux_link_transport_write(&h, b, sizeof(b)) != sizeof(b))
        return fail("boot interleave queue rest");
    while (lmux_link_build_output(&h) > 0) {
        ev = 0;
        if (feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev) < 0)
            return fail("boot interleave feed");
        lmux_link_output_consume(&h, lmux_link_output_len(&h));
    }
    if (lmux_link_transport_read(&s, got, sizeof(got)) != 2 ||
        got[0] != 'A' || got[1] != 'B')
        return fail("boot interleave transport order");
    if (lmux_link_boot_read_frame(&s, &type, got, sizeof(got), &got_len) < 0 ||
        type != LMUX_LINK_T_BOOT_REQUEST || got_len != req_len ||
        memcmp(got, req, req_len) != 0)
        return fail("boot interleave boot frame");
    if (s.expected_rx_seq != 4 || s.rx_frames != 3)
        return fail("boot interleave seq count");
    if (lmux_link_boot_queue_frame(&h, LMUX_LINK_T_BOOT_REQUEST, req, req_len) < 0)
        return fail("boot reset queue");
    lmux_link_endpoint_down(&h);
    if (lmux_link_boot_tx_len(&h) != 0 || lmux_link_boot_rx_len(&h) != 0)
        return fail("boot reset clear");
    lmux_link_endpoint_begin(&h, 0x5555);
    lmux_link_endpoint_begin(&s, 0x6666);
    if (establish_pair(&h, &s) < 0)
        return fail("boot malformed setup");
    req[0] = 0xaa;
    if (lmux_link_boot_queue_frame(&h, LMUX_LINK_T_BOOT_REQUEST, req, 1) < 0 ||
        lmux_link_build_output(&h) <= 0)
        return fail("boot malformed queue");
    ev = 0;
    if (feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev) < 0 ||
        s.state != LMUX_LINK_ESTABLISHED ||
        lmux_link_boot_rx_len(&s) != 1)
        return fail("boot malformed became outer failure");
    return 0;
}

int main(void)
{
    if (test_crc32c() ||
        test_golden_vectors() ||
        test_boot_protocol() ||
        test_hello_and_transport() ||
        test_sequence_gap_fails() ||
        test_crc_failure_fails_established() ||
        test_peer_restart_requires_fresh_local_hello() ||
        test_hello_ack_payload_and_lost_ack() ||
        test_duplicate_hello_ack_coalescing() ||
        test_boot_interleaving_and_reset())
        return 1;
    puts("link selftest ok");
    return 0;
}
