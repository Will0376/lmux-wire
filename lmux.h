#ifndef LMUX_H
#define LMUX_H

#include <stddef.h>
#include <stdint.h>

#define LMUX_VERSION 1
#define LMUX_CHANNELS 4
#define LMUX_MAX_PAYLOAD 256
#define LMUX_RX_RING 1024
#define LMUX_TX_RING 1024
#define LMUX_HEADER_LEN 8
#define LMUX_CRC_LEN 2
#define LMUX_MAX_DECODED (LMUX_HEADER_LEN + LMUX_MAX_PAYLOAD + LMUX_CRC_LEN)
#define LMUX_MAX_ENCODED (LMUX_MAX_DECODED + (LMUX_MAX_DECODED / 254) + 2)
#define LMUX_PATH_MAX 256
#define LMUX_FEATURE_FILE       0x00000001u
#define LMUX_FEATURE_CWD_QUERY  0x00000002u
#define LMUX_FEATURE_WINSIZE    0x00000004u
#define LMUX_FEATURE_STREAM_GEN 0x00000008u

enum lmux_type {
    LMUX_T_HELLO = 1,
    LMUX_T_OPEN = 2,
    LMUX_T_CLOSE = 3,
    LMUX_T_DATA = 4,
    LMUX_T_CREDIT = 5,
    LMUX_T_RESET = 6,
    LMUX_T_WINSIZE = 7
};

struct lmux_frame {
    uint8_t version;
    uint8_t type;
    uint8_t channel;
    uint8_t flags;
    uint16_t sequence;
    uint16_t payload_len;
    uint8_t payload[LMUX_MAX_PAYLOAD];
};

uint16_t lmux_get_le16(const uint8_t *p);
uint32_t lmux_get_le32(const uint8_t *p);
void lmux_put_le16(uint8_t *p, uint16_t v);
void lmux_put_le32(uint8_t *p, uint32_t v);

uint16_t lmux_crc16_ccitt_false(const uint8_t *buf, size_t len);
int lmux_cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
int lmux_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
int lmux_encode_frame(const struct lmux_frame *fr, uint8_t *out, size_t out_cap, size_t *out_len);
int lmux_decode_frame(const uint8_t *encoded, size_t encoded_len, struct lmux_frame *fr);

#endif
