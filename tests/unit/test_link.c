#include "link_endpoint.h"
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

static int feed_record(struct lmux_link_endpoint *ep, const uint8_t *enc,
                       size_t len, unsigned *events)
{
    if (len == 0 || enc[len - 1] != 0)
        return -1;
    return lmux_link_input_bytes(ep, enc, len, events);
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
        (ev & LMUX_LINK_EV_ESTABLISHED) == 0)
        return fail("supervisor establishes from host hello");
    lmux_link_output_consume(&h, lmux_link_output_len(&h));
    ev = 0;
    if (feed_record(&h, lmux_link_output_ptr(&s), lmux_link_output_len(&s), &ev) < 0 ||
        (ev & LMUX_LINK_EV_ESTABLISHED) == 0)
        return fail("host establishes from supervisor hello");
    lmux_link_output_consume(&s, lmux_link_output_len(&s));

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
    (void)feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev);
    lmux_link_output_consume(&h, lmux_link_output_len(&h));
    ev = 0;
    (void)feed_record(&h, lmux_link_output_ptr(&s), lmux_link_output_len(&s), &ev);
    lmux_link_output_consume(&s, lmux_link_output_len(&s));
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
    (void)feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev);
    lmux_link_output_consume(&h, lmux_link_output_len(&h));
    ev = 0;
    (void)feed_record(&h, lmux_link_output_ptr(&s), lmux_link_output_len(&s), &ev);
    lmux_link_output_consume(&s, lmux_link_output_len(&s));
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
    if (feed_record(&s, lmux_link_output_ptr(&h), lmux_link_output_len(&h), &ev) < 0 ||
        !(ev & LMUX_LINK_EV_ESTABLISHED))
        return fail("restart setup s");
    lmux_link_output_consume(&h, lmux_link_output_len(&h));
    ev = 0;
    if (feed_record(&h, lmux_link_output_ptr(&s), lmux_link_output_len(&s), &ev) < 0 ||
        !(ev & LMUX_LINK_EV_ESTABLISHED))
        return fail("restart setup h");
    lmux_link_output_consume(&s, lmux_link_output_len(&s));

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
        !(ev & LMUX_LINK_EV_ESTABLISHED) ||
        s.local_conn_id != 0x4444 ||
        s.peer_conn_id != 0x3333 ||
        s.next_tx_seq != 1)
        return fail("restart did not establish fresh pair");
    if (lmux_link_decode_frame(lmux_link_output_ptr(&s),
                               lmux_link_output_len(&s) - 1, &fr) < 0 ||
        fr.type != LMUX_LINK_T_HELLO ||
        fr.conn_id != 0x4444 ||
        fr.frame_seq != 0)
        return fail("fresh restart hello encoding");
    return 0;
}

int main(void)
{
    if (test_crc32c() ||
        test_golden_vectors() ||
        test_hello_and_transport() ||
        test_sequence_gap_fails() ||
        test_crc_failure_fails_established() ||
        test_peer_restart_requires_fresh_local_hello())
        return 1;
    puts("link selftest ok");
    return 0;
}
