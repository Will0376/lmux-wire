#include "dirty.h"
#include "link_protocol.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message)
{
    fprintf(stderr, "dirty test failed: %s\n", message);
    return 1;
}

static void fill_pattern(uint8_t *buf, size_t len, unsigned seed)
{
    size_t i;
    for (i = 0; i < len; i++)
        buf[i] = (uint8_t)((i * 73u + seed * 29u) ^ (i >> 2));
}

static int transfer_one(struct lmux_dirty *from, struct lmux_dirty *to,
                        uint64_t now, int split, int duplicate)
{
    const uint8_t *wire;
    size_t len;
    int rc = lmux_dirty_output_peek(from, now, &wire, &len);
    if (rc <= 0) return rc;
    if (split && len > 3) {
        size_t a = len / 3, b = len / 2;
        if (lmux_dirty_feed(to, wire, a, now) < 0 ||
            lmux_dirty_feed(to, wire + a, b - a, now) < 0 ||
            lmux_dirty_feed(to, wire + b, len - b, now) < 0)
            return -1;
    } else if (lmux_dirty_feed(to, wire, len, now) < 0) {
        return -1;
    }
    if (duplicate && lmux_dirty_feed(to, wire, len, now) < 0)
        return -1;
    if (lmux_dirty_output_consume(from, len) < 0) return -1;
    return 1;
}

static int transfer_all(struct lmux_dirty *from, struct lmux_dirty *to,
                        uint64_t now, int split)
{
    int count = 0, rc;
    while ((rc = transfer_one(from, to, now, split, 0)) > 0) {
        if (++count > 64) return -1;
    }
    return rc < 0 ? -1 : count;
}

static int expect_packet(struct lmux_dirty *d, const uint8_t *want,
                         size_t want_len, uint32_t want_seq)
{
    const uint8_t *got;
    size_t got_len;
    uint32_t seq;
    if (lmux_dirty_receive_peek(d, &got, &got_len, &seq) != 1 ||
        got_len != want_len || seq != want_seq ||
        memcmp(got, want, want_len) != 0)
        return -1;
    return lmux_dirty_receive_consume(d);
}

static int test_record_codec(void)
{
    struct lmux_dirty_record in, out;
    uint8_t wire[LMUX_DIRTY_RECORD_MAX], bad[LMUX_DIRTY_RECORD_MAX];
    size_t len;
    uint8_t parse_reason;
    memset(&in, 0, sizeof(in));
    in.type = LMUX_DIRTY_DATA;
    in.session_id = UINT64_C(0x0102030405060708);
    in.packet_seq = 0xf1020304u;
    in.packet_len = 200;
    in.fragment_index = 0;
    in.fragment_count = 2;
    in.payload_len = LMUX_DIRTY_FRAGMENT_MAX;
    fill_pattern(in.payload, in.payload_len, 4);
    in.fragment_crc = lmux_crc32c(in.payload, in.payload_len);
    in.packet_crc = 0xabcdef01u;
    if (lmux_dirty_record_encode(&in, wire, sizeof(wire), &len) < 0 ||
        len > LMUX_DIRTY_RECORD_MAX ||
        lmux_dirty_record_decode(wire, len, &out) != 0 ||
        out.type != in.type || out.session_id != in.session_id ||
        out.packet_seq != in.packet_seq || out.payload_len != in.payload_len ||
        memcmp(out.payload, in.payload, in.payload_len) != 0)
        return fail("DATA codec");
    in.fragment_crc ^= 1;
    if (lmux_dirty_record_encode(&in, bad, sizeof(bad), &len) == 0)
        return fail("fragment CRC rejected independently");
    in.fragment_crc ^= 1;

    memcpy(bad, wire, len);
    bad[LMUX_DIRTY_PREFIX_LEN + 7] =
        bad[LMUX_DIRTY_PREFIX_LEN + 7] == 'A' ? 'B' : 'A';
    if (lmux_dirty_record_decode_diagnostic(bad, len, &out,
            &parse_reason) != -1 ||
        parse_reason != LMUX_DIRTY_PARSE_RECORD_CRC)
        return fail("record CRC detects encoded corruption");
    memcpy(bad, wire, len);
    bad[len - 2] = '\n';
    if (lmux_dirty_record_decode(bad, len, &out) != -1)
        return fail("strict CRLF");
    if (lmux_dirty_record_decode((const uint8_t *)"console\r\n", 9, &out) != 1)
        return fail("foreign record classification");

    memset(&in, 0, sizeof(in));
    in.type = LMUX_DIRTY_ACK; in.session_id = 7; in.packet_seq = 9;
    if (lmux_dirty_record_encode(&in, wire, sizeof(wire), &len) < 0 ||
        lmux_dirty_record_decode(wire, len, &out) != 0 ||
        out.type != LMUX_DIRTY_ACK)
        return fail("ACK codec");
    in.type = LMUX_DIRTY_NACK; in.missing_bitmap = 0x25;
    if (lmux_dirty_record_encode(&in, wire, sizeof(wire), &len) < 0 ||
        lmux_dirty_record_decode(wire, len, &out) != 0 ||
        out.missing_bitmap != 0x25)
        return fail("NACK codec");
    in.type = LMUX_DIRTY_FAIL; in.missing_bitmap = 0;
    in.reason = LMUX_DIRTY_FAIL_RETRY_EXHAUSTED;
    if (lmux_dirty_record_encode(&in, wire, sizeof(wire), &len) < 0 ||
        lmux_dirty_record_decode(wire, len, &out) != 0 ||
        out.reason != LMUX_DIRTY_FAIL_RETRY_EXHAUSTED)
        return fail("FAIL codec");

    in.type = LMUX_DIRTY_NACK; in.reason = 0; in.missing_bitmap = 0;
    if (lmux_dirty_record_encode(&in, wire, sizeof(wire), &len) == 0 ||
        errno != EINVAL)
        return fail("invalid NACK rejected");
    return 0;
}

static int test_fragmentation_ack_and_backpressure(void)
{
    struct lmux_dirty a, b;
    uint8_t p0[1024], p1[337], p2[1];
    uint32_t s0, s1;
    fill_pattern(p0, sizeof(p0), 1);
    fill_pattern(p1, sizeof(p1), 2);
    p2[0] = 0;
    lmux_dirty_init(&a, 11, 22, 50, 70, 115200, 0);
    lmux_dirty_init(&b, 22, 11, 70, 50, 115200, 0);
    if (lmux_dirty_submit(&a, p0, sizeof(p0), 0, &s0) < 0 ||
        lmux_dirty_submit(&a, p1, sizeof(p1), 0, &s1) < 0 ||
        s0 != 50 || s1 != 51)
        return fail("two-packet TX window");
    if (lmux_dirty_submit(&a, p2, sizeof(p2), 0, NULL) == 0 ||
        errno != EAGAIN)
        return fail("bounded TX backpressure");
    if (transfer_all(&a, &b, 1, 1) != 8)
        return fail("fragment transfer and split input");
    if (expect_packet(&b, p0, sizeof(p0), s0) < 0)
        return fail("first exact packet");
    if (transfer_all(&b, &a, 2, 0) != 1)
        return fail("ACK after upper consume");
    if (lmux_dirty_submit(&a, p2, sizeof(p2), 2, NULL) < 0)
        return fail("ACK releases exactly one TX slot");
    if (expect_packet(&b, p1, sizeof(p1), s1) < 0)
        return fail("second exact packet");
    return 0;
}

static int test_parser_resynchronization(void)
{
    struct lmux_dirty d;
    struct lmux_dirty_record r;
    uint8_t wire[LMUX_DIRTY_RECORD_MAX];
    uint8_t stream[LMUX_DIRTY_RECORD_MAX * 2];
    size_t len, n = 0;
    unsigned i;
    memset(&r, 0, sizeof(r));
    r.type = LMUX_DIRTY_ACK; r.session_id = 90; r.packet_seq = 3;
    if (lmux_dirty_record_encode(&r, wire, sizeof(wire), &len) < 0)
        return fail("resync source encode");
    lmux_dirty_init(&d, 90, 91, 0, 0, 115200, 0);
    memcpy(stream + n, "noise", 5); n += 5;
    memcpy(stream + n, wire, len / 2); n += len / 2;
    memcpy(stream + n, "foreign-inside", 14); n += 14;
    memcpy(stream + n, wire, len); n += len;
    if (lmux_dirty_feed(&d, stream, n, 0) < 0 ||
        d.stats.valid_records != 1 || d.stats.invalid_records == 0 ||
        d.diagnostic.invalid_reason[LMUX_DIRTY_PARSE_RESTART_PREFIX] == 0 ||
        d.diagnostic.last_valid_type != LMUX_DIRTY_ACK)
        return fail("new prefix resynchronizes truncated candidate");

    /* Oversized candidates retain no unbounded line state. */
    if (lmux_dirty_feed(&d, (const uint8_t *)LMUX_DIRTY_PREFIX,
                        LMUX_DIRTY_PREFIX_LEN, 0) < 0)
        return fail("oversize prefix feed");
    for (i = 0; i < LMUX_DIRTY_BASE64_MAX + 80; i++)
        if (lmux_dirty_feed(&d, (const uint8_t *)"A", 1, 0) < 0)
            return fail("oversize body feed");
    if (lmux_dirty_feed(&d, (const uint8_t *)"\r\n", 2, 0) < 0 ||
        d.parser.body_len != 0 || d.parser.in_record ||
        d.stats.invalid_records < 2)
        return fail("oversize record discarded and bounded");

    memset(&r, 0, sizeof(r));
    r.type = LMUX_DIRTY_DATA; r.session_id = 1; r.packet_seq = 1;
    r.packet_len = 10; r.fragment_count = 0; r.payload_len = 10;
    memset(r.payload, 1, 10); r.packet_crc = 1;
    r.fragment_crc = lmux_crc32c(r.payload, 10);
    if (lmux_dirty_record_encode(&r, wire, sizeof(wire), &len) == 0)
        return fail("invalid fragment count rejected");
    r.fragment_count = 1; r.packet_len = LMUX_DIRTY_PACKET_MAX + 1;
    if (lmux_dirty_record_encode(&r, wire, sizeof(wire), &len) == 0)
        return fail("impossible packet length rejected");
    return 0;
}

static int test_ordering_and_sequence_wrap(void)
{
    struct lmux_dirty a, b;
    struct lmux_dirty_record records[3];
    uint8_t wire[3][LMUX_DIRTY_RECORD_MAX];
    uint8_t first[100], second[300];
    const uint8_t *out;
    size_t lens[3], out_len;
    unsigned i;
    fill_pattern(first, sizeof(first), 8);
    fill_pattern(second, sizeof(second), 9);
    lmux_dirty_init(&a, 101, 202, UINT32_MAX, 40, 115200, 0);
    lmux_dirty_init(&b, 202, 101, 40, UINT32_MAX, 115200, 0);
    if (lmux_dirty_submit(&a, first, sizeof(first), 0, NULL) < 0 ||
        lmux_dirty_submit(&a, second, sizeof(second), 0, NULL) < 0)
        return fail("wrap submit");
    for (i = 0; i < 3; i++) {
        if (lmux_dirty_output_peek(&a, 0, &out, &out_len) != 1 ||
            out_len > sizeof(wire[i])) return fail("capture records");
        memcpy(wire[i], out, out_len); lens[i] = out_len;
        if (lmux_dirty_record_decode(wire[i], lens[i], &records[i]) != 0 ||
            lmux_dirty_output_consume(&a, out_len) < 0)
            return fail("decode captured records");
    }
    /* Packet zero arrives and verifies before UINT32_MAX. */
    if (lmux_dirty_feed(&b, wire[1], lens[1], 1) < 0 ||
        lmux_dirty_feed(&b, wire[2], lens[2], 1) < 0 ||
        lmux_dirty_receive_peek(&b, &out, &out_len, NULL) != 0)
        return fail("verified future packet remains held");
    if (lmux_dirty_feed(&b, wire[0], lens[0], 1) < 0 ||
        expect_packet(&b, first, sizeof(first), UINT32_MAX) < 0 ||
        expect_packet(&b, second, sizeof(second), 0) < 0)
        return fail("ordered delivery across sequence wrap");
    return 0;
}

static int test_selective_retransmission_and_noise(void)
{
    struct lmux_dirty a, b;
    struct lmux_dirty_record r;
    uint8_t payload[500], corrupt[LMUX_DIRTY_RECORD_MAX];
    const uint8_t *wire;
    size_t len;
    unsigned sent = 0;
    uint64_t retry_at;
    fill_pattern(payload, sizeof(payload), 17);
    lmux_dirty_init(&a, 31, 41, 5, 8, 115200, 0);
    lmux_dirty_init(&b, 41, 31, 8, 5, 115200, 0);
    if (lmux_dirty_submit(&a, payload, sizeof(payload), 0, NULL) < 0)
        return fail("selective submit");
    while (lmux_dirty_output_peek(&a, 0, &wire, &len) == 1) {
        if (lmux_dirty_record_decode(wire, len, &r) != 0)
            return fail("selective source record");
        if (r.fragment_index == 1) {
            memcpy(corrupt, wire, len);
            corrupt[len / 2] = '!';
            if (lmux_dirty_feed(&b, corrupt, len / 2, 0) < 0 ||
                lmux_dirty_feed(&b, corrupt + len / 2, len - len / 2, 0) < 0)
                return fail("corrupt split record feed");
        } else {
            static const uint8_t noise[] = "console: hello\n<garbage>";
            if (lmux_dirty_feed(&b, noise, sizeof(noise) - 1, 0) < 0 ||
                lmux_dirty_feed(&b, wire, len, 0) < 0)
                return fail("foreign bytes between records");
        }
        if (lmux_dirty_output_consume(&a, len) < 0) return fail("source consume");
        sent++;
    }
    if (sent != 3 || b.stats.invalid_records == 0 ||
        b.diagnostic.invalid_reason[LMUX_DIRTY_PARSE_BAD_ALPHABET] == 0 ||
        b.diagnostic.last_invalid_reason != LMUX_DIRTY_PARSE_BAD_ALPHABET ||
        lmux_dirty_receive_peek(&b, &wire, &len, NULL) != 0)
        return fail("damaged fragment withheld");
    retry_at = b.retry_interval_ms + 1;
    if (lmux_dirty_tick(&b, retry_at) < 0 ||
        transfer_all(&b, &a, retry_at, 1) != 1)
        return fail("selective NACK");
    if (transfer_all(&a, &b, retry_at, 1) != 1 ||
        a.stats.retransmitted_fragments != 1 ||
        expect_packet(&b, payload, sizeof(payload), 5) < 0)
        return fail("one-fragment retransmission and exact recovery");
    return 0;
}

static int test_packet_crc_full_retransmit(void)
{
    struct lmux_dirty a, b;
    struct lmux_dirty_record r;
    uint8_t payload[250], wire[LMUX_DIRTY_RECORD_MAX];
    size_t len;
    unsigned i;
    fill_pattern(payload, sizeof(payload), 21);
    lmux_dirty_init(&a, 51, 61, 0, 0, 115200, 0);
    lmux_dirty_init(&b, 61, 51, 0, 0, 115200, 0);
    for (i = 0; i < 2; i++) {
        size_t off = i * LMUX_DIRTY_FRAGMENT_MAX;
        size_t n = sizeof(payload) - off;
        if (n > LMUX_DIRTY_FRAGMENT_MAX) n = LMUX_DIRTY_FRAGMENT_MAX;
        memset(&r, 0, sizeof(r));
        r.type = LMUX_DIRTY_DATA; r.session_id = 51; r.packet_seq = 0;
        r.packet_len = sizeof(payload); r.fragment_offset = (uint16_t)off;
        r.fragment_index = (uint8_t)i; r.fragment_count = 2;
        r.payload_len = (uint16_t)n;
        r.packet_crc = lmux_crc32c(payload, sizeof(payload));
        memcpy(r.payload, payload + off, n);
        if (i == 0) r.payload[17] ^= 0x80;
        r.fragment_crc = lmux_crc32c(r.payload, n);
        if (lmux_dirty_record_encode(&r, wire, sizeof(wire), &len) < 0 ||
            lmux_dirty_feed(&b, wire, len, 0) < 0)
            return fail("bad packet CRC records");
    }
    {
        const uint8_t *out;
        size_t out_len;
        if (lmux_dirty_output_peek(&b, 0, &out, &out_len) != 1 ||
            lmux_dirty_record_decode(out, out_len, &r) != 0 ||
            r.type != LMUX_DIRTY_NACK || r.missing_bitmap != 3)
            return fail("full-packet retransmit bitmap");
    }
    /* A complete clean retransmission replaces, rather than mixes with, bad data. */
    for (i = 0; i < 2; i++) {
        size_t off = i * LMUX_DIRTY_FRAGMENT_MAX;
        size_t n = sizeof(payload) - off;
        if (n > LMUX_DIRTY_FRAGMENT_MAX) n = LMUX_DIRTY_FRAGMENT_MAX;
        memset(&r, 0, sizeof(r));
        r.type = LMUX_DIRTY_DATA; r.session_id = 51; r.packet_seq = 0;
        r.packet_len = sizeof(payload); r.fragment_offset = (uint16_t)off;
        r.fragment_index = (uint8_t)i; r.fragment_count = 2;
        r.payload_len = (uint16_t)n;
        r.packet_crc = lmux_crc32c(payload, sizeof(payload));
        memcpy(r.payload, payload + off, n);
        r.fragment_crc = lmux_crc32c(r.payload, n);
        if (lmux_dirty_record_encode(&r, wire, sizeof(wire), &len) < 0 ||
            lmux_dirty_feed(&b, wire, len, 1) < 0)
            return fail("clean full-packet retransmission");
    }
    if (expect_packet(&b, payload, sizeof(payload), 0) < 0)
        return fail("packet CRC recovery exact delivery");
    return 0;
}

static int test_partial_output_interlocks(void)
{
    struct lmux_dirty a, b;
    struct lmux_dirty_record record;
    uint8_t payload[300], data_wire[2][LMUX_DIRTY_RECORD_MAX];
    uint8_t nack_wire[LMUX_DIRTY_RECORD_MAX], ack_wire[LMUX_DIRTY_RECORD_MAX];
    const uint8_t *out;
    size_t data_len[2], len, nack_len, ack_len;
    unsigned i;
    fill_pattern(payload, sizeof(payload), 33);
    lmux_dirty_init(&a, 301, 302, 4, 4, 115200, 0);
    lmux_dirty_init(&b, 302, 301, 4, 4, 115200, 0);
    if (lmux_dirty_submit(&a, payload, sizeof(payload), 0, NULL) < 0)
        return fail("interlock submit");
    for (i = 0; i < 2; i++) {
        if (lmux_dirty_output_peek(&a, 0, &out, &len) != 1)
            return fail("interlock capture DATA");
        memcpy(data_wire[i], out, len); data_len[i] = len;
        if (lmux_dirty_output_consume(&a, len) < 0)
            return fail("interlock consume DATA");
    }
    if (lmux_dirty_feed(&b, data_wire[0], data_len[0], 0) < 0 ||
        lmux_dirty_tick(&b, b.retry_interval_ms + 1) < 0 ||
        lmux_dirty_output_peek(&b, b.retry_interval_ms + 1,
                               &out, &nack_len) != 1 ||
        lmux_dirty_record_decode(out, nack_len, &record) != 0 ||
        record.type != LMUX_DIRTY_NACK)
        return fail("partial NACK prepared");
    memcpy(nack_wire, out, nack_len);
    /* NACK is physically in progress while the missing fragment completes. */
    if (lmux_dirty_feed(&b, data_wire[1], data_len[1],
                        b.retry_interval_ms + 1) < 0 ||
        expect_packet(&b, payload, sizeof(payload), 4) < 0 ||
        lmux_dirty_output_consume(&b, nack_len) < 0 ||
        lmux_dirty_output_peek(&b, b.retry_interval_ms + 1,
                               &out, &ack_len) != 1 ||
        lmux_dirty_record_decode(out, ack_len, &record) != 0 ||
        record.type != LMUX_DIRTY_ACK)
        return fail("ACK survives partial NACK completion");
    memcpy(ack_wire, out, ack_len);
    if (lmux_dirty_output_consume(&b, ack_len) < 0 ||
        lmux_dirty_feed(&a, nack_wire, nack_len,
                        b.retry_interval_ms + 1) < 0 ||
        lmux_dirty_output_peek(&a, b.retry_interval_ms + 1,
                               &out, &len) != 1)
        return fail("partial retransmission prepared");
    /* A delayed ACK may arrive while a now-unneeded retransmit is partial. */
    if (lmux_dirty_feed(&a, ack_wire, ack_len,
                        b.retry_interval_ms + 1) < 0 ||
        !a.tx[0].used || !a.tx[0].acked ||
        lmux_dirty_output_consume(&a, len) < 0 || a.tx[0].used)
        return fail("ACK defers TX slot reuse until partial record completes");
    return 0;
}

static int test_retry_deadline_session_and_bounds(void)
{
    struct lmux_dirty a, b;
    uint8_t payload[20], garbage[4096], stale_wire[LMUX_DIRTY_RECORD_MAX];
    const uint8_t *wire;
    size_t len, stale_len;
    unsigned round;
    fill_pattern(payload, sizeof(payload), 3);
    memset(garbage, 'x', sizeof(garbage));
    lmux_dirty_init(&a, 71, 81, 1, 1, 9600, 0);
    lmux_dirty_init(&b, 81, 99, 1, 1, 9600, 0);
    if (lmux_dirty_feed(&b, garbage, sizeof(garbage), 0) < 0 ||
        b.stats.foreign_bytes < sizeof(garbage) - LMUX_DIRTY_PREFIX_LEN)
        return fail("continuous garbage is bounded and harmless");
    if (lmux_dirty_submit(&a, payload, sizeof(payload), 0, NULL) < 0 ||
        lmux_dirty_output_peek(&a, 0, &wire, &len) != 1 ||
        len > sizeof(stale_wire))
        return fail("stale session source");
    memcpy(stale_wire, wire, len);
    stale_len = len;
    if (lmux_dirty_feed(&b, wire, len, 0) < 0 ||
        lmux_dirty_output_consume(&a, len) < 0 ||
        lmux_dirty_receive_peek(&b, &wire, &len, NULL) != 0 ||
        b.stats.stale_records != 1)
        return fail("stale session rejected without delivery");
    lmux_dirty_reset_peer(&b, 71, 1, 1);
    if (lmux_dirty_feed(&b, stale_wire, stale_len, 1) < 0 ||
        expect_packet(&b, payload, sizeof(payload), 1) < 0)
        return fail("explicit session replacement accepts exact new session");
    /* Let every complete retry record disappear. */
    for (round = 0; round <= LMUX_DIRTY_RETRY_MAX; round++) {
        uint64_t now = (uint64_t)(round + 1) * a.retry_interval_ms + round;
        lmux_dirty_tick(&a, now);
        while (!lmux_dirty_failed(&a) &&
               lmux_dirty_output_peek(&a, now, &wire, &len) == 1)
            if (lmux_dirty_output_consume(&a, len) < 0)
                return fail("drop retransmit output");
    }
    if (!lmux_dirty_failed(&a) ||
        a.fail_reason != LMUX_DIRTY_FAIL_RETRY_EXHAUSTED)
        return fail("bounded retry exhaustion");

    lmux_dirty_init(&a, 72, 82, 1, 1, 1200, 0);
    if (lmux_dirty_submit(&a, payload, sizeof(payload), 0, NULL) < 0 ||
        lmux_dirty_tick(&a, LMUX_DIRTY_DEADLINE_MAX_MS + 1) == 0 ||
        a.fail_reason != LMUX_DIRTY_FAIL_DEADLINE)
        return fail("absolute hard deadline");
    if (lmux_dirty_memory_footprint() != sizeof(struct lmux_dirty) ||
        sizeof(struct lmux_dirty) > 8192)
        return fail("fixed adapter memory bound");
    return 0;
}

int main(void)
{
    if (test_record_codec() ||
        test_fragmentation_ack_and_backpressure() ||
        test_parser_resynchronization() ||
        test_ordering_and_sequence_wrap() ||
        test_selective_retransmission_and_noise() ||
        test_packet_crc_full_retransmit() ||
        test_partial_output_interlocks() ||
        test_retry_deadline_session_and_bounds())
        return 1;
    printf("dirty tests: ok (adapter=%zu bytes, record=%u bytes)\n",
           sizeof(struct lmux_dirty), (unsigned)LMUX_DIRTY_RECORD_MAX);
    return 0;
}
