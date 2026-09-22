#ifndef LMUX2_PROTOCOL_H
#define LMUX2_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define LMUX2_MAGIC "LMX2"
#define LMUX2_VERSION 2

#define LMUX2_HEADER_LEN 28
#define LMUX2_CRC_LEN 2
#define LMUX2_MAX_PAYLOAD 512
#define LMUX2_MAX_DECODED (LMUX2_HEADER_LEN + LMUX2_MAX_PAYLOAD + LMUX2_CRC_LEN)
#define LMUX2_MAX_ENCODED (LMUX2_MAX_DECODED + (LMUX2_MAX_DECODED / 254) + 2)

#define LMUX2_F_CRITICAL 0x0001u
#define LMUX2_KNOWN_FLAGS 0x0000u

enum lmux2_role {
    LMUX2_ROLE_HOST = 1,
    LMUX2_ROLE_TARGET = 2
};

enum lmux2_type {
    LMUX2_T_HELLO = 1,
    LMUX2_T_STREAM_OPEN = 2,
    LMUX2_T_STREAM_OPEN_OK = 3,
    LMUX2_T_STREAM_OPEN_ERROR = 4,
    LMUX2_T_STREAM_CLOSE = 5,
    LMUX2_T_STREAM_RESET = 6,
    LMUX2_T_DATA = 7,
    LMUX2_T_MAX_STREAM_DATA = 8,
    LMUX2_T_MAX_DATA = 9,
    LMUX2_T_CONTROL = 10
};

enum lmux2_open_error {
    LMUX2_OPEN_E_REFUSED = 1,
    LMUX2_OPEN_E_LIMIT = 2,
    LMUX2_OPEN_E_BAD_STATE = 3
};

struct lmux2_frame {
    uint8_t type;
    uint16_t flags;
    uint64_t stream_id;
    uint64_t value;
    uint16_t payload_len;
    uint8_t payload[LMUX2_MAX_PAYLOAD];
};

struct lmux2_hello {
    enum lmux2_role role;
    uint64_t conn_id;
    uint32_t max_streams;
    uint32_t stream_window;
    uint64_t conn_window;
};

uint64_t lmux2_get_le64(const uint8_t *p);
void lmux2_put_le64(uint8_t *p, uint64_t v);

int lmux2_type_known(uint8_t type);
int lmux2_encode_frame(const struct lmux2_frame *fr, uint8_t *out,
                       size_t out_cap, size_t *out_len);
int lmux2_decode_frame(const uint8_t *encoded, size_t encoded_len,
                       struct lmux2_frame *fr);
int lmux2_make_hello_payload(enum lmux2_role role, uint64_t conn_id,
                             uint32_t max_streams, uint32_t stream_window,
                             uint64_t conn_window, uint8_t *payload,
                             uint16_t *payload_len);
int lmux2_parse_hello_payload(const uint8_t *payload, uint16_t payload_len,
                              struct lmux2_hello *hello);

#endif
