#include "lmux2_control.h"
#include "lmux.h"

#include <string.h>

int lmux2_control_put_tlv(uint8_t *p, size_t cap, size_t *off,
                          uint16_t type, const void *data, uint16_t len)
{
    if (*off + 4u + len > cap)
        return -1;
    lmux_put_le16(p + *off, type);
    lmux_put_le16(p + *off + 2, len);
    if (len)
        memcpy(p + *off + 4, data, len);
    *off += 4u + len;
    return 0;
}

int lmux2_control_put_u8(uint8_t *p, size_t cap, size_t *off,
                         uint16_t type, uint8_t v)
{
    return lmux2_control_put_tlv(p, cap, off, type, &v, 1);
}

int lmux2_control_make_frame(uint8_t kind, uint32_t req_id,
                             uint64_t service_id, uint64_t token,
                             const uint8_t *tlv, uint16_t tlv_len,
                             struct lmux2_frame *fr)
{
    if (tlv_len > LMUX2_MAX_PAYLOAD - LMUX2_CONTROL_HEADER_LEN)
        return -1;
    memset(fr, 0, sizeof(*fr));
    fr->type = LMUX2_T_CONTROL;
    fr->stream_id = 0;
    fr->payload[0] = V2_CTRL_VERSION;
    fr->payload[1] = kind;
    lmux_put_le16(fr->payload + 2, 0);
    lmux_put_le32(fr->payload + 4, req_id);
    lmux2_put_le64(fr->payload + 8, service_id);
    lmux2_put_le64(fr->payload + 16, token);
    if (tlv_len)
        memcpy(fr->payload + LMUX2_CONTROL_HEADER_LEN, tlv, tlv_len);
    fr->payload_len = (uint16_t)(LMUX2_CONTROL_HEADER_LEN + tlv_len);
    return 0;
}

int lmux2_control_parse_frame(const struct lmux2_frame *fr,
                              struct lmux2_control_view *cv)
{
    if (fr->type != LMUX2_T_CONTROL || fr->stream_id != 0 ||
        fr->payload_len < LMUX2_CONTROL_HEADER_LEN ||
        fr->payload[0] != V2_CTRL_VERSION ||
        lmux_get_le16(fr->payload + 2) != 0)
        return -1;
    cv->kind = fr->payload[1];
    cv->req_id = lmux_get_le32(fr->payload + 4);
    cv->service_id = lmux2_get_le64(fr->payload + 8);
    cv->token = lmux2_get_le64(fr->payload + 16);
    cv->tlv = fr->payload + LMUX2_CONTROL_HEADER_LEN;
    cv->tlv_len = fr->payload_len - LMUX2_CONTROL_HEADER_LEN;
    return 0;
}

int lmux2_control_tlv_find(const struct lmux2_control_view *cv, uint16_t type,
                           const uint8_t **data, uint16_t *len)
{
    size_t off = 0;
    while (off + 4 <= cv->tlv_len) {
        uint16_t t = lmux_get_le16(cv->tlv + off);
        uint16_t l = lmux_get_le16(cv->tlv + off + 2);
        off += 4;
        if (off + l > cv->tlv_len)
            return -1;
        if (t == type) {
            *data = cv->tlv + off;
            *len = l;
            return 1;
        }
        off += l;
    }
    return off == cv->tlv_len ? 0 : -1;
}

int lmux2_control_tlv_string(const struct lmux2_control_view *cv, uint16_t type,
                             char *out, size_t cap)
{
    const uint8_t *p;
    uint16_t len;
    int r = lmux2_control_tlv_find(cv, type, &p, &len);
    if (r <= 0 || len == 0 || len >= cap || memchr(p, 0, len) == NULL)
        return -1;
    memcpy(out, p, len);
    out[len - 1] = 0;
    return 0;
}

int lmux2_control_tlv_u8(const struct lmux2_control_view *cv, uint16_t type,
                         uint8_t *out)
{
    const uint8_t *p;
    uint16_t len;
    int r = lmux2_control_tlv_find(cv, type, &p, &len);
    if (r <= 0 || len != 1)
        return -1;
    *out = p[0];
    return 0;
}

int lmux2_control_tlv_bytes(const struct lmux2_control_view *cv, uint16_t type,
                            const uint8_t **out, uint16_t *len)
{
    int r = lmux2_control_tlv_find(cv, type, out, len);
    return r > 0 ? 0 : -1;
}

int lmux2_control_has_service_type(const struct lmux2_control_view *cv, uint8_t want)
{
    size_t off = 0;
    while (off + 4 <= cv->tlv_len) {
        uint16_t t = lmux_get_le16(cv->tlv + off);
        uint16_t l = lmux_get_le16(cv->tlv + off + 2);
        off += 4;
        if (off + l > cv->tlv_len)
            return 0;
        if (t == V2_TLV_SERVICE_TYPE && l == 1 && cv->tlv[off] == want)
            return 1;
        off += l;
    }
    return 0;
}
