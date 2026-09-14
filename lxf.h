#ifndef LXF_H
#define LXF_H

#include "lmux.h"

#include <stddef.h>
#include <stdint.h>

#define LXF_MAGIC "LXF1"
#define LXF_HEADER_LEN 12
#define LXF_MAX_PAYLOAD 4096
#define LXF_MAX_RECORD (LXF_HEADER_LEN + LXF_MAX_PAYLOAD)
#define LXF_PATH_MAX 256
#define LXF_DEFAULT_SOCKET "/run/lmux/file.sock"

#define LXF_F_SIZE_KNOWN 0x00000001u

enum lxf_type {
    LXF_T_PUT_REQUEST = 1,
    LXF_T_GET_REQUEST = 2,
    LXF_T_OK = 3,
    LXF_T_ERROR = 4,
    LXF_T_DATA = 5,
    LXF_T_DONE = 6,
    LXF_T_CWD_REQUEST = 7,
    LXF_T_CWD_REPLY = 8
};

enum lxf_error {
    LXF_E_PROTOCOL = 1,
    LXF_E_IO = 2,
    LXF_E_NOTFOUND = 3,
    LXF_E_DENIED = 4,
    LXF_E_BADPATH = 5,
    LXF_E_NOTREG = 6,
    LXF_E_HASH = 7,
    LXF_E_NOSPACE = 8
};

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;
    uint8_t block[64];
    size_t used;
};

void lmux_put_le64(uint8_t *p, uint64_t v);
uint64_t lmux_get_le64(const uint8_t *p);

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[32]);
void sha256_hex(const uint8_t digest[32], char out[65]);

int lxf_encode_record(uint8_t type, uint8_t flags, const uint8_t *payload,
                      uint32_t payload_len, uint8_t *out, size_t out_cap,
                      size_t *out_len);
int lxf_decode_header(const uint8_t *buf, size_t len, uint8_t *type,
                      uint8_t *flags, uint32_t *payload_len);
const char *lxf_strerror_code(uint32_t code);

#endif
