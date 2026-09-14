#include "lmux.h"

#include <string.h>

uint16_t lmux_get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t lmux_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void lmux_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

void lmux_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

uint16_t lmux_crc16_ccitt_false(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xffff;
    size_t i;

    for (i = 0; i < len; i++) {
        int bit;
        crc ^= (uint16_t)buf[i] << 8;
        for (bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

int lmux_cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t read_index = 0;
    size_t write_index = 1;
    size_t code_index = 0;
    uint8_t code = 1;

    if (out_cap == 0)
        return -1;

    while (read_index < in_len) {
        if (write_index >= out_cap)
            return -1;
        if (in[read_index] == 0) {
            out[code_index] = code;
            code = 1;
            code_index = write_index++;
            read_index++;
        } else {
            out[write_index++] = in[read_index++];
            code++;
            if (code == 0xff) {
                out[code_index] = code;
                code = 1;
                code_index = write_index++;
                if (code_index >= out_cap)
                    return -1;
            }
        }
    }

    if (code_index >= out_cap)
        return -1;
    out[code_index] = code;
    *out_len = write_index;
    return 0;
}

int lmux_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t read_index = 0;
    size_t write_index = 0;

    while (read_index < in_len) {
        uint8_t code = in[read_index++];
        unsigned int i;

        if (code == 0)
            return -1;
        if (read_index + code - 1 > in_len)
            return -1;
        for (i = 1; i < code; i++) {
            if (write_index >= out_cap)
                return -1;
            out[write_index++] = in[read_index++];
        }
        if (code != 0xff && read_index < in_len) {
            if (write_index >= out_cap)
                return -1;
            out[write_index++] = 0;
        }
    }

    *out_len = write_index;
    return 0;
}

int lmux_encode_frame(const struct lmux_frame *fr, uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t decoded[LMUX_MAX_DECODED];
    size_t decoded_len;
    size_t encoded_len;
    uint16_t crc;

    if (fr->payload_len > LMUX_MAX_PAYLOAD)
        return -1;

    decoded[0] = fr->version;
    decoded[1] = fr->type;
    decoded[2] = fr->channel;
    decoded[3] = fr->flags;
    lmux_put_le16(decoded + 4, fr->sequence);
    lmux_put_le16(decoded + 6, fr->payload_len);
    memcpy(decoded + LMUX_HEADER_LEN, fr->payload, fr->payload_len);
    decoded_len = LMUX_HEADER_LEN + fr->payload_len;
    crc = lmux_crc16_ccitt_false(decoded, decoded_len);
    lmux_put_le16(decoded + decoded_len, crc);
    decoded_len += LMUX_CRC_LEN;

    if (lmux_cobs_encode(decoded, decoded_len, out, out_cap, &encoded_len) < 0)
        return -1;
    if (encoded_len + 1 > out_cap)
        return -1;
    out[encoded_len++] = 0;
    *out_len = encoded_len;
    return 0;
}

int lmux_decode_frame(const uint8_t *encoded, size_t encoded_len, struct lmux_frame *fr)
{
    uint8_t decoded[LMUX_MAX_DECODED];
    size_t decoded_len;
    uint16_t have_crc, want_crc;

    if (encoded_len == 0 || encoded_len > LMUX_MAX_ENCODED)
        return -1;
    if (lmux_cobs_decode(encoded, encoded_len, decoded, sizeof(decoded), &decoded_len) < 0)
        return -1;
    if (decoded_len < LMUX_HEADER_LEN + LMUX_CRC_LEN)
        return -1;

    fr->version = decoded[0];
    fr->type = decoded[1];
    fr->channel = decoded[2];
    fr->flags = decoded[3];
    fr->sequence = lmux_get_le16(decoded + 4);
    fr->payload_len = lmux_get_le16(decoded + 6);

    if (fr->version != LMUX_VERSION || fr->flags != 0)
        return -1;
    if (fr->payload_len > LMUX_MAX_PAYLOAD)
        return -1;
    if (decoded_len != (size_t)LMUX_HEADER_LEN + fr->payload_len + LMUX_CRC_LEN)
        return -1;
    if (fr->type < LMUX_T_HELLO || fr->type > LMUX_T_RESET)
        return -1;
    if ((fr->type == LMUX_T_HELLO || fr->type == LMUX_T_RESET) && fr->channel != 0)
        return -1;
    if (fr->type != LMUX_T_HELLO && fr->type != LMUX_T_RESET &&
        (fr->channel < 1 || fr->channel > LMUX_CHANNELS))
        return -1;

    have_crc = lmux_get_le16(decoded + LMUX_HEADER_LEN + fr->payload_len);
    want_crc = lmux_crc16_ccitt_false(decoded, LMUX_HEADER_LEN + fr->payload_len);
    if (have_crc != want_crc)
        return -1;

    memcpy(fr->payload, decoded + LMUX_HEADER_LEN, fr->payload_len);
    return 0;
}
