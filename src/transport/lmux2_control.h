#ifndef LMUX2_CONTROL_H
#define LMUX2_CONTROL_H

#include "lmux2_protocol.h"

#include <stddef.h>
#include <stdint.h>

#define V2_CTRL_VERSION 1
#define V2_CTRL_MAX_TLVS 3
#define LMUX2_CONTROL_HEADER_LEN 24

enum v2_ctrl_kind {
    V2_CTRL_FACTORY_REPORT = 1,
    V2_CTRL_SERVICE_ENSURE = 2,
    V2_CTRL_SERVICE_OK = 3,
    V2_CTRL_SERVICE_ERROR = 4,
    V2_CTRL_SERVICE_DELETE = 5,
    V2_CTRL_SERVICE_STATE = 6,
    V2_CTRL_SERVICE_EVENT = 7,
    V2_CTRL_SERVICE_CONTROL = 8
};

enum v2_ctrl_error {
    V2_E_NOTFOUND = 1,
    V2_E_CONFLICT = 2,
    V2_E_LIMIT = 3,
    V2_E_BAD_REQUEST = 4,
    V2_E_IO = 5
};

enum v2_tlv_type {
    V2_TLV_NAME = 1,
    V2_TLV_SERVICE_TYPE = 2,
    V2_TLV_TARGET_PATH = 3,
    V2_TLV_ERROR_CODE = 4,
    V2_TLV_EVENT = 5,
    V2_TLV_ARGV = 6,
    V2_TLV_SUPERVISED = 7,
    V2_TLV_WINSIZE = 8
};

struct lmux2_control_view {
    uint8_t kind;
    uint32_t req_id;
    uint64_t service_id;
    uint64_t token;
    const uint8_t *tlv;
    size_t tlv_len;
};

int lmux2_control_put_tlv(uint8_t *p, size_t cap, size_t *off,
                          uint16_t type, const void *data, uint16_t len);
int lmux2_control_put_u8(uint8_t *p, size_t cap, size_t *off,
                         uint16_t type, uint8_t v);
int lmux2_control_make_frame(uint8_t kind, uint32_t req_id,
                             uint64_t service_id, uint64_t token,
                             const uint8_t *tlv, uint16_t tlv_len,
                             struct lmux2_frame *fr);
int lmux2_control_parse_frame(const struct lmux2_frame *fr,
                              struct lmux2_control_view *cv);
int lmux2_control_tlv_find(const struct lmux2_control_view *cv, uint16_t type,
                           const uint8_t **data, uint16_t *len);
int lmux2_control_tlv_string(const struct lmux2_control_view *cv, uint16_t type,
                             char *out, size_t cap);
int lmux2_control_tlv_u8(const struct lmux2_control_view *cv, uint16_t type,
                         uint8_t *out);
int lmux2_control_tlv_bytes(const struct lmux2_control_view *cv, uint16_t type,
                            const uint8_t **out, uint16_t *len);
int lmux2_control_has_service_type(const struct lmux2_control_view *cv, uint8_t want);

#endif
