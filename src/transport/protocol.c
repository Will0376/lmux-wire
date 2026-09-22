#include "lmux.h"

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
