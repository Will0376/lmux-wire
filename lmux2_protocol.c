#include "lmux2_protocol.h"
#include "lmux.h"

#include <string.h>

#define LMUX2_HELLO_LEN 28

uint64_t lmux2_get_le64(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) | ((uint64_t)lmux_get_le32(p + 4) << 32);
}

void lmux2_put_le64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)(v & 0xffffffffu));
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

int lmux2_type_known(uint8_t type)
{
    return type >= LMUX2_T_HELLO && type <= LMUX2_T_CONTROL;
}

int lmux2_encode_frame(const struct lmux2_frame *fr, uint8_t *out,
                       size_t out_cap, size_t *out_len)
{
    uint8_t decoded[LMUX2_MAX_DECODED];
    size_t decoded_len, encoded_len;
    uint16_t crc;

    if (!fr || !out || !out_len || fr->payload_len > LMUX2_MAX_PAYLOAD)
        return -1;
    if (lmux2_type_known(fr->type) && (fr->flags & ~LMUX2_KNOWN_FLAGS))
        return -1;

    memcpy(decoded, LMUX2_MAGIC, 4);
    decoded[4] = LMUX2_VERSION;
    decoded[5] = fr->type;
    lmux_put_le16(decoded + 6, fr->flags);
    lmux2_put_le64(decoded + 8, fr->stream_id);
    lmux2_put_le64(decoded + 16, fr->value);
    lmux_put_le16(decoded + 24, fr->payload_len);
    lmux_put_le16(decoded + 26, 0);
    if (fr->payload_len)
        memcpy(decoded + LMUX2_HEADER_LEN, fr->payload, fr->payload_len);
    decoded_len = LMUX2_HEADER_LEN + fr->payload_len;
    crc = lmux_crc16_ccitt_false(decoded, decoded_len);
    lmux_put_le16(decoded + decoded_len, crc);
    decoded_len += LMUX2_CRC_LEN;

    if (lmux_cobs_encode(decoded, decoded_len, out, out_cap, &encoded_len) < 0)
        return -1;
    if (encoded_len + 1 > out_cap)
        return -1;
    out[encoded_len++] = 0;
    *out_len = encoded_len;
    return 0;
}

int lmux2_decode_frame(const uint8_t *encoded, size_t encoded_len,
                       struct lmux2_frame *fr)
{
    uint8_t decoded[LMUX2_MAX_DECODED];
    size_t decoded_len;
    uint16_t have_crc, want_crc;

    if (!encoded || !fr || encoded_len == 0 || encoded_len > LMUX2_MAX_ENCODED)
        return -1;
    if (lmux_cobs_decode(encoded, encoded_len, decoded, sizeof(decoded), &decoded_len) < 0)
        return -1;
    if (decoded_len < LMUX2_HEADER_LEN + LMUX2_CRC_LEN)
        return -1;
    if (memcmp(decoded, LMUX2_MAGIC, 4) != 0 || decoded[4] != LMUX2_VERSION)
        return -1;

    fr->type = decoded[5];
    fr->flags = lmux_get_le16(decoded + 6);
    fr->stream_id = lmux2_get_le64(decoded + 8);
    fr->value = lmux2_get_le64(decoded + 16);
    fr->payload_len = lmux_get_le16(decoded + 24);
    if (lmux_get_le16(decoded + 26) != 0)
        return -1;
    if (fr->payload_len > LMUX2_MAX_PAYLOAD)
        return -1;
    if (decoded_len != (size_t)LMUX2_HEADER_LEN + fr->payload_len + LMUX2_CRC_LEN)
        return -1;
    if (lmux2_type_known(fr->type) && (fr->flags & ~LMUX2_KNOWN_FLAGS))
        return -1;

    have_crc = lmux_get_le16(decoded + LMUX2_HEADER_LEN + fr->payload_len);
    want_crc = lmux_crc16_ccitt_false(decoded, LMUX2_HEADER_LEN + fr->payload_len);
    if (have_crc != want_crc)
        return -1;

    if (fr->payload_len)
        memcpy(fr->payload, decoded + LMUX2_HEADER_LEN, fr->payload_len);
    return 0;
}

int lmux2_make_hello_payload(enum lmux2_role role, uint64_t conn_id,
                             uint32_t max_streams, uint32_t stream_window,
                             uint64_t conn_window, uint8_t *payload,
                             uint16_t *payload_len)
{
    if (!payload || !payload_len || (role != LMUX2_ROLE_HOST && role != LMUX2_ROLE_TARGET) ||
        conn_id == 0 || max_streams == 0 || stream_window == 0 || conn_window == 0)
        return -1;
    payload[0] = (uint8_t)role;
    payload[1] = payload[2] = payload[3] = 0;
    lmux2_put_le64(payload + 4, conn_id);
    lmux_put_le32(payload + 12, max_streams);
    lmux_put_le32(payload + 16, stream_window);
    lmux2_put_le64(payload + 20, conn_window);
    *payload_len = LMUX2_HELLO_LEN;
    return 0;
}

int lmux2_parse_hello_payload(const uint8_t *payload, uint16_t payload_len,
                              struct lmux2_hello *hello)
{
    if (!payload || !hello || payload_len != LMUX2_HELLO_LEN)
        return -1;
    if (payload[1] || payload[2] || payload[3])
        return -1;
    memset(hello, 0, sizeof(*hello));
    hello->role = (enum lmux2_role)payload[0];
    hello->conn_id = lmux2_get_le64(payload + 4);
    hello->max_streams = lmux_get_le32(payload + 12);
    hello->stream_window = lmux_get_le32(payload + 16);
    hello->conn_window = lmux2_get_le64(payload + 20);
    if ((hello->role != LMUX2_ROLE_HOST && hello->role != LMUX2_ROLE_TARGET) ||
        hello->conn_id == 0 || hello->max_streams == 0 ||
        hello->stream_window == 0 || hello->conn_window == 0)
        return -1;
    return 0;
}
