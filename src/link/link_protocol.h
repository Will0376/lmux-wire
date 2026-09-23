#ifndef LMUX_LINK_PROTOCOL_H
#define LMUX_LINK_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define LMUX_LINK_MAGIC0 'L'
#define LMUX_LINK_MAGIC1 'M'
#define LMUX_LINK_MAGIC2 'O'
#define LMUX_LINK_MAGIC3 '1'

#define LMUX_LINK_VERSION 1
#define LMUX_LINK_HEADER_LEN 32
#define LMUX_LINK_MAX_HEADER_LEN 64
#define LMUX_LINK_MAX_PAYLOAD 4096
#define LMUX_LINK_TRANSPORT_TARGET 1024
#define LMUX_LINK_MAX_DECODED (LMUX_LINK_MAX_HEADER_LEN + LMUX_LINK_MAX_PAYLOAD + 4)
#define LMUX_LINK_MAX_ENCODED (LMUX_LINK_MAX_DECODED + ((LMUX_LINK_MAX_DECODED + 253) / 254) + 1)

enum lmux_link_type {
    LMUX_LINK_T_HELLO = 1,
    LMUX_LINK_T_TRANSPORT = 2,
    LMUX_LINK_T_BOOT_REQUEST = 3,
    LMUX_LINK_T_BOOT_RESPONSE = 4,
    LMUX_LINK_T_BOOT_EVENT = 5
};

enum lmux_link_flag {
    LMUX_LINK_F_CRITICAL_EXTENSION = 0x01
};

enum lmux_link_role {
    LMUX_LINK_ROLE_HOST = 1,
    LMUX_LINK_ROLE_SUPERVISOR = 2
};

enum lmux_link_cap {
    LMUX_LINK_CAP_TRANSPORT = 0x00000001u,
    LMUX_LINK_CAP_BOOT_INFO = 0x00000002u,
    LMUX_LINK_CAP_BOOT_UPDATE_RESERVED = 0x00000004u,
    LMUX_LINK_CAP_BOOT_WORKER_CONTROL_RESERVED = 0x00000008u
};

struct lmux_link_frame {
    uint8_t type;
    uint8_t flags;
    uint16_t header_len;
    uint32_t payload_len;
    uint64_t conn_id;
    uint64_t frame_seq;
    uint8_t payload[LMUX_LINK_MAX_PAYLOAD];
};

struct lmux_link_hello {
    uint8_t role;
    uint32_t capabilities;
    uint32_t max_outer_payload;
    uint32_t max_transport_payload;
    uint32_t preferred_transport_payload;
};

uint32_t lmux_crc32c(const uint8_t *buf, size_t len);

int lmux_link_encode_frame(const struct lmux_link_frame *fr,
                           uint8_t *out, size_t out_cap, size_t *out_len);
int lmux_link_decode_frame(const uint8_t *in, size_t in_len,
                           struct lmux_link_frame *fr);

int lmux_link_make_hello_payload(const struct lmux_link_hello *h,
                                 uint8_t *out, size_t out_cap,
                                 uint32_t *out_len);
int lmux_link_parse_hello_payload(const uint8_t *payload, uint32_t payload_len,
                                  struct lmux_link_hello *h);

#endif
