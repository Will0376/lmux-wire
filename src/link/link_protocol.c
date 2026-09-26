#include "link_protocol.h"
#include "lmux.h"

#include <string.h>

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void put_le64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

uint32_t lmux_crc32c(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xffffffffu;
    size_t i;
    for (i = 0; i < len; i++) {
        int bit;
        crc ^= buf[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? (crc >> 1) ^ 0x82f63b78u : crc >> 1;
    }
    return crc ^ 0xffffffffu;
}

static int validate_type(uint8_t type)
{
    return type >= LMUX_LINK_T_HELLO && type <= LMUX_LINK_T_HELLO_ACK;
}

int lmux_link_encode_frame(const struct lmux_link_frame *fr,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t decoded[LMUX_LINK_MAX_DECODED];
    uint32_t crc;
    size_t decoded_len, enc_len;

    if (!fr || !out || !out_len || !validate_type(fr->type) ||
        fr->flags & ~LMUX_LINK_F_CRITICAL_EXTENSION ||
        fr->header_len != LMUX_LINK_HEADER_LEN ||
        fr->payload_len > LMUX_LINK_MAX_PAYLOAD ||
        out_cap < LMUX_LINK_MAX_ENCODED)
        return -1;

    decoded[0] = LMUX_LINK_MAGIC0;
    decoded[1] = LMUX_LINK_MAGIC1;
    decoded[2] = LMUX_LINK_MAGIC2;
    decoded[3] = LMUX_LINK_MAGIC3;
    lmux_put_le16(decoded + 4, LMUX_LINK_VERSION);
    decoded[6] = fr->type;
    decoded[7] = fr->flags;
    lmux_put_le16(decoded + 8, LMUX_LINK_HEADER_LEN);
    lmux_put_le16(decoded + 10, 0);
    lmux_put_le32(decoded + 12, fr->payload_len);
    put_le64(decoded + 16, fr->conn_id);
    put_le64(decoded + 24, fr->frame_seq);
    if (fr->payload_len)
        memcpy(decoded + LMUX_LINK_HEADER_LEN, fr->payload, fr->payload_len);
    decoded_len = LMUX_LINK_HEADER_LEN + fr->payload_len;
    crc = lmux_crc32c(decoded, decoded_len);
    lmux_put_le32(decoded + decoded_len, crc);
    decoded_len += 4;

    if (lmux_cobs_encode(decoded, decoded_len, out, out_cap - 1, &enc_len) < 0)
        return -1;
    out[enc_len++] = 0;
    *out_len = enc_len;
    return 0;
}

int lmux_link_decode_frame(const uint8_t *in, size_t in_len,
                           struct lmux_link_frame *fr)
{
    uint8_t decoded[LMUX_LINK_MAX_DECODED];
    size_t decoded_len;
    uint16_t version, header_len, reserved;
    uint32_t payload_len, got_crc, want_crc;

    if (!in || !fr || in_len == 0 ||
        lmux_cobs_decode(in, in_len, decoded, sizeof(decoded), &decoded_len) < 0)
        return -1;
    if (decoded_len < LMUX_LINK_HEADER_LEN + 4)
        return -1;
    if (decoded[0] != LMUX_LINK_MAGIC0 || decoded[1] != LMUX_LINK_MAGIC1 ||
        decoded[2] != LMUX_LINK_MAGIC2 || decoded[3] != LMUX_LINK_MAGIC3)
        return -1;
    version = lmux_get_le16(decoded + 4);
    if (version != LMUX_LINK_VERSION)
        return -1;
    if (decoded[7] & ~LMUX_LINK_F_CRITICAL_EXTENSION)
        return -1;
    header_len = lmux_get_le16(decoded + 8);
    reserved = lmux_get_le16(decoded + 10);
    payload_len = lmux_get_le32(decoded + 12);
    if (reserved != 0 || header_len < LMUX_LINK_HEADER_LEN ||
        header_len > LMUX_LINK_MAX_HEADER_LEN ||
        payload_len > LMUX_LINK_MAX_PAYLOAD ||
        decoded_len != (size_t)header_len + payload_len + 4)
        return -1;
    if (header_len > LMUX_LINK_HEADER_LEN &&
        (decoded[7] & LMUX_LINK_F_CRITICAL_EXTENSION))
        return -1;
    got_crc = lmux_get_le32(decoded + header_len + payload_len);
    want_crc = lmux_crc32c(decoded, (size_t)header_len + payload_len);
    if (got_crc != want_crc)
        return -1;

    memset(fr, 0, sizeof(*fr));
    fr->type = decoded[6];
    fr->flags = decoded[7];
    fr->header_len = header_len;
    fr->payload_len = payload_len;
    fr->conn_id = get_le64(decoded + 16);
    fr->frame_seq = get_le64(decoded + 24);
    if (payload_len)
        memcpy(fr->payload, decoded + header_len, payload_len);
    return 0;
}

int lmux_link_make_hello_payload(const struct lmux_link_hello *h,
                                 uint8_t *out, size_t out_cap,
                                 uint32_t *out_len)
{
    if (!h || !out || !out_len || out_cap < 24 ||
        (h->role != LMUX_LINK_ROLE_HOST &&
         h->role != LMUX_LINK_ROLE_SUPERVISOR) ||
        h->preferred_transport_payload > h->max_transport_payload)
        return -1;
    memset(out, 0, 24);
    out[0] = h->role;
    out[1] = 0;
    lmux_put_le16(out + 2, 24);
    lmux_put_le32(out + 4, h->capabilities);
    lmux_put_le32(out + 8, h->max_outer_payload);
    lmux_put_le32(out + 12, h->max_transport_payload);
    lmux_put_le32(out + 16, h->preferred_transport_payload);
    lmux_put_le32(out + 20, 0);
    *out_len = 24;
    return 0;
}

int lmux_link_parse_hello_payload(const uint8_t *payload, uint32_t payload_len,
                                  struct lmux_link_hello *h)
{
    uint16_t hello_len;
    if (!payload || !h || payload_len < 24)
        return -1;
    hello_len = lmux_get_le16(payload + 2);
    if ((payload[0] != LMUX_LINK_ROLE_HOST &&
         payload[0] != LMUX_LINK_ROLE_SUPERVISOR) ||
        payload[1] != 0 || hello_len < 24 || hello_len > payload_len ||
        lmux_get_le32(payload + 20) != 0)
        return -1;
    memset(h, 0, sizeof(*h));
    h->role = payload[0];
    h->capabilities = lmux_get_le32(payload + 4);
    h->max_outer_payload = lmux_get_le32(payload + 8);
    h->max_transport_payload = lmux_get_le32(payload + 12);
    h->preferred_transport_payload = lmux_get_le32(payload + 16);
    if (!h->max_outer_payload || !h->max_transport_payload ||
        h->max_outer_payload > LMUX_LINK_MAX_PAYLOAD ||
        h->max_transport_payload > h->max_outer_payload ||
        h->preferred_transport_payload > h->max_transport_payload)
        return -1;
    return 0;
}

int lmux_link_make_hello_ack_payload(uint64_t ack_peer_conn_id,
                                     uint8_t *out, size_t out_cap,
                                     uint32_t *out_len)
{
    if (!out || !out_len || out_cap < 8 || ack_peer_conn_id == 0)
        return -1;
    put_le64(out, ack_peer_conn_id);
    *out_len = 8;
    return 0;
}

int lmux_link_parse_hello_ack_payload(const uint8_t *payload,
                                      uint32_t payload_len,
                                      uint64_t *ack_peer_conn_id)
{
    if (!payload || !ack_peer_conn_id || payload_len != 8)
        return -1;
    *ack_peer_conn_id = get_le64(payload);
    return *ack_peer_conn_id ? 0 : -1;
}
