#include "boot_protocol.h"
#include "lmux.h"

#include <string.h>

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) | ((uint64_t)lmux_get_le32(p + 4) << 32);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)v);
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

int lmux_boot_parse_request(const uint8_t *buf, uint32_t len,
                            struct lmux_boot_request *out)
{
    if (!buf || !out || len < LMUX_BOOT_REQ_HEADER_LEN)
        return -1;
    out->opcode = lmux_get_le16(buf);
    out->flags = lmux_get_le16(buf + 2);
    out->request_id = get_le64(buf + 4);
    out->body = buf + LMUX_BOOT_REQ_HEADER_LEN;
    out->body_len = len - LMUX_BOOT_REQ_HEADER_LEN;
    if (out->request_id == 0)
        return -1;
    return 0;
}

int lmux_boot_make_request(uint8_t *buf, size_t cap, uint32_t *len,
                           uint16_t opcode, uint64_t request_id,
                           const uint8_t *body, uint32_t body_len)
{
    if (!buf || !len || request_id == 0 ||
        cap < LMUX_BOOT_REQ_HEADER_LEN ||
        body_len > cap - LMUX_BOOT_REQ_HEADER_LEN ||
        (body_len && !body))
        return -1;
    lmux_put_le16(buf, opcode);
    lmux_put_le16(buf + 2, 0);
    put_le64(buf + 4, request_id);
    if (body_len)
        memcpy(buf + LMUX_BOOT_REQ_HEADER_LEN, body, body_len);
    *len = LMUX_BOOT_REQ_HEADER_LEN + body_len;
    return 0;
}

int lmux_boot_parse_response(const uint8_t *buf, uint32_t len,
                             struct lmux_boot_response *out)
{
    if (!buf || !out || len < LMUX_BOOT_RESP_HEADER_LEN)
        return -1;
    out->opcode = lmux_get_le16(buf);
    out->status = lmux_get_le16(buf + 2);
    if (lmux_get_le32(buf + 4) != 0)
        return -1;
    out->request_id = get_le64(buf + 8);
    out->body = buf + LMUX_BOOT_RESP_HEADER_LEN;
    out->body_len = len - LMUX_BOOT_RESP_HEADER_LEN;
    if (out->request_id == 0)
        return -1;
    return 0;
}

int lmux_boot_make_response(uint8_t *buf, size_t cap, uint32_t *len,
                            uint16_t opcode, uint16_t status,
                            uint64_t request_id,
                            const uint8_t *body, uint32_t body_len)
{
    if (!buf || !len || request_id == 0 ||
        cap < LMUX_BOOT_RESP_HEADER_LEN ||
        body_len > cap - LMUX_BOOT_RESP_HEADER_LEN ||
        (body_len && !body))
        return -1;
    lmux_put_le16(buf, opcode);
    lmux_put_le16(buf + 2, status);
    lmux_put_le32(buf + 4, 0);
    put_le64(buf + 8, request_id);
    if (body_len)
        memcpy(buf + LMUX_BOOT_RESP_HEADER_LEN, body, body_len);
    *len = LMUX_BOOT_RESP_HEADER_LEN + body_len;
    return 0;
}

int lmux_boot_encode_get_info_response(uint8_t *buf, size_t cap, uint32_t *len,
                                       const struct lmux_boot_info *info)
{
    if (!buf || !len || !info || cap < 20)
        return -1;
    lmux_put_le16(buf, info->outer_version);
    lmux_put_le16(buf + 2, info->boot_version);
    lmux_put_le32(buf + 4, info->capabilities);
    lmux_put_le32(buf + 8, info->max_outer_payload);
    lmux_put_le32(buf + 12, info->max_transport_payload);
    lmux_put_le32(buf + 16, 0);
    *len = 20;
    return 0;
}

int lmux_boot_decode_get_info_response(const uint8_t *buf, uint32_t len,
                                       struct lmux_boot_info *info)
{
    if (!buf || !info || len != 20 || lmux_get_le32(buf + 16) != 0)
        return -1;
    info->outer_version = lmux_get_le16(buf);
    info->boot_version = lmux_get_le16(buf + 2);
    info->capabilities = lmux_get_le32(buf + 4);
    info->max_outer_payload = lmux_get_le32(buf + 8);
    info->max_transport_payload = lmux_get_le32(buf + 12);
    return 0;
}

int lmux_boot_encode_worker_status(uint8_t *buf, size_t cap, uint32_t *len,
                                   const struct lmux_boot_worker_status *st)
{
    if (!buf || !len || !st || cap < 32)
        return -1;
    lmux_put_le32(buf, st->worker_state);
    lmux_put_le32(buf + 4, st->flags);
    put_le64(buf + 8, st->worker_incarnation);
    put_le64(buf + 16, st->uptime_ms);
    lmux_put_le32(buf + 24, st->restart_count);
    lmux_put_le32(buf + 28, 0);
    *len = 32;
    return 0;
}

int lmux_boot_decode_worker_status(const uint8_t *buf, uint32_t len,
                                   struct lmux_boot_worker_status *st)
{
    if (!buf || !st || len != 32 || lmux_get_le32(buf + 28) != 0)
        return -1;
    st->worker_state = lmux_get_le32(buf);
    st->flags = lmux_get_le32(buf + 4);
    st->worker_incarnation = get_le64(buf + 8);
    st->uptime_ms = get_le64(buf + 16);
    st->restart_count = lmux_get_le32(buf + 24);
    return 0;
}

int lmux_boot_encode_upload_begin(uint8_t *buf, size_t cap, uint32_t *len,
                                  uint64_t declared_size)
{
    if (!buf || !len || cap < 8)
        return -1;
    put_le64(buf, declared_size);
    *len = 8;
    return 0;
}

int lmux_boot_decode_upload_begin(const uint8_t *buf, uint32_t len,
                                  struct lmux_boot_upload_begin *out)
{
    if (!buf || !out || len != 8)
        return -1;
    out->declared_size = get_le64(buf);
    return 0;
}

int lmux_boot_encode_upload_begin_response(uint8_t *buf, size_t cap,
                                           uint32_t *len, uint64_t upload_id,
                                           uint32_t max_chunk)
{
    if (!buf || !len || upload_id == 0 || max_chunk == 0 || cap < 12)
        return -1;
    put_le64(buf, upload_id);
    lmux_put_le32(buf + 8, max_chunk);
    *len = 12;
    return 0;
}

int lmux_boot_decode_upload_begin_response(const uint8_t *buf, uint32_t len,
                                           struct lmux_boot_upload_begin_response *out)
{
    if (!buf || !out || len != 12)
        return -1;
    out->upload_id = get_le64(buf);
    out->max_chunk = lmux_get_le32(buf + 8);
    return out->upload_id == 0 || out->max_chunk == 0 ? -1 : 0;
}

int lmux_boot_encode_upload_data(uint8_t *buf, size_t cap, uint32_t *len,
                                 uint64_t upload_id, uint64_t offset,
                                 const uint8_t *data, uint32_t data_len)
{
    if (!buf || !len || upload_id == 0 || cap < 16 ||
        data_len > cap - 16 || (data_len && !data))
        return -1;
    put_le64(buf, upload_id);
    put_le64(buf + 8, offset);
    if (data_len)
        memcpy(buf + 16, data, data_len);
    *len = 16 + data_len;
    return 0;
}

int lmux_boot_decode_upload_data(const uint8_t *buf, uint32_t len,
                                 struct lmux_boot_upload_data *out)
{
    if (!buf || !out || len < 16)
        return -1;
    out->upload_id = get_le64(buf);
    out->offset = get_le64(buf + 8);
    out->data = buf + 16;
    out->data_len = len - 16;
    return out->upload_id == 0 ? -1 : 0;
}

int lmux_boot_encode_upload_data_response(uint8_t *buf, size_t cap,
                                          uint32_t *len, uint64_t next_offset)
{
    if (!buf || !len || cap < 8)
        return -1;
    put_le64(buf, next_offset);
    *len = 8;
    return 0;
}

int lmux_boot_decode_upload_data_response(const uint8_t *buf, uint32_t len,
                                          struct lmux_boot_upload_data_response *out)
{
    if (!buf || !out || len != 8)
        return -1;
    out->next_offset = get_le64(buf);
    return 0;
}

int lmux_boot_encode_upload_end(uint8_t *buf, size_t cap, uint32_t *len,
                                uint64_t upload_id,
                                const uint8_t expected_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN])
{
    if (!buf || !len || !expected_sha256 || upload_id == 0 || cap < 40)
        return -1;
    put_le64(buf, upload_id);
    memcpy(buf + 8, expected_sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    *len = 40;
    return 0;
}

int lmux_boot_decode_upload_end(const uint8_t *buf, uint32_t len,
                                struct lmux_boot_upload_end *out)
{
    if (!buf || !out || len != 40)
        return -1;
    out->upload_id = get_le64(buf);
    memcpy(out->expected_sha256, buf + 8, LMUX_BOOT_UPLOAD_SHA256_LEN);
    return out->upload_id == 0 ? -1 : 0;
}

int lmux_boot_encode_upload_end_response(uint8_t *buf, size_t cap,
                                         uint32_t *len, uint64_t size,
                                         const uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN])
{
    if (!buf || !len || !sha256 || cap < 40)
        return -1;
    put_le64(buf, size);
    memcpy(buf + 8, sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    *len = 40;
    return 0;
}

int lmux_boot_decode_upload_end_response(const uint8_t *buf, uint32_t len,
                                         struct lmux_boot_upload_end_response *out)
{
    if (!buf || !out || len != 40)
        return -1;
    out->size = get_le64(buf);
    memcpy(out->sha256, buf + 8, LMUX_BOOT_UPLOAD_SHA256_LEN);
    return 0;
}

int lmux_boot_encode_upload_abort(uint8_t *buf, size_t cap, uint32_t *len,
                                  uint64_t upload_id)
{
    if (!buf || !len || upload_id == 0 || cap < 8)
        return -1;
    put_le64(buf, upload_id);
    *len = 8;
    return 0;
}

int lmux_boot_decode_upload_abort(const uint8_t *buf, uint32_t len,
                                  uint64_t *upload_id)
{
    if (!buf || !upload_id || len != 8)
        return -1;
    *upload_id = get_le64(buf);
    return *upload_id == 0 ? -1 : 0;
}

int lmux_boot_encode_activate(uint8_t *buf, size_t cap, uint32_t *len,
                              const uint8_t candidate_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN],
                              uint32_t confirm_timeout_ms)
{
    if (!buf || !len || !candidate_sha256 || cap < 36)
        return -1;
    memcpy(buf, candidate_sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    lmux_put_le32(buf + 32, confirm_timeout_ms);
    *len = 36;
    return 0;
}

int lmux_boot_decode_activate(const uint8_t *buf, uint32_t len,
                              struct lmux_boot_activate *out)
{
    if (!buf || !out || len != 36)
        return -1;
    memcpy(out->candidate_sha256, buf, LMUX_BOOT_UPLOAD_SHA256_LEN);
    out->confirm_timeout_ms = lmux_get_le32(buf + 32);
    return 0;
}

int lmux_boot_encode_activate_response(uint8_t *buf, size_t cap, uint32_t *len,
                                       uint64_t activation_id,
                                       uint64_t generation,
                                       uint32_t confirm_timeout_ms,
                                       uint32_t phase)
{
    if (!buf || !len || cap < 24)
        return -1;
    put_le64(buf, activation_id);
    put_le64(buf + 8, generation);
    lmux_put_le32(buf + 16, confirm_timeout_ms);
    lmux_put_le32(buf + 20, phase);
    *len = 24;
    return 0;
}

int lmux_boot_decode_activate_response(const uint8_t *buf, uint32_t len,
                                       struct lmux_boot_activate_response *out)
{
    if (!buf || !out || len != 24)
        return -1;
    out->activation_id = get_le64(buf);
    out->generation = get_le64(buf + 8);
    out->confirm_timeout_ms = lmux_get_le32(buf + 16);
    out->phase = lmux_get_le32(buf + 20);
    return 0;
}

int lmux_boot_encode_activation_id(uint8_t *buf, size_t cap, uint32_t *len,
                                   uint64_t activation_id)
{
    if (!buf || !len || activation_id == 0 || cap < 8)
        return -1;
    put_le64(buf, activation_id);
    *len = 8;
    return 0;
}

int lmux_boot_decode_activation_id(const uint8_t *buf, uint32_t len,
                                   uint64_t *activation_id)
{
    if (!buf || !activation_id || len != 8)
        return -1;
    *activation_id = get_le64(buf);
    return *activation_id == 0 ? -1 : 0;
}

int lmux_boot_encode_update_status(uint8_t *buf, size_t cap, uint32_t *len,
                                   const struct lmux_boot_update_status *st)
{
    if (!buf || !len || !st || cap < LMUX_BOOT_UPDATE_STATUS_LEN)
        return -1;
    memset(buf, 0, LMUX_BOOT_UPDATE_STATUS_LEN);
    lmux_put_le32(buf, st->phase);
    lmux_put_le32(buf + 4, st->flags);
    put_le64(buf + 8, st->generation);
    put_le64(buf + 16, st->activation_id);
    lmux_put_le32(buf + 24, st->confirm_remaining_ms);
    lmux_put_le32(buf + 28, st->last_update_error);
    memcpy(buf + 32, st->confirmed_sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    memcpy(buf + 64, st->candidate_sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    memcpy(buf + 96, st->running_sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    *len = LMUX_BOOT_UPDATE_STATUS_LEN;
    return 0;
}

int lmux_boot_decode_update_status(const uint8_t *buf, uint32_t len,
                                   struct lmux_boot_update_status *st)
{
    if (!buf || !st || len != LMUX_BOOT_UPDATE_STATUS_LEN)
        return -1;
    st->phase = lmux_get_le32(buf);
    st->flags = lmux_get_le32(buf + 4);
    st->generation = get_le64(buf + 8);
    st->activation_id = get_le64(buf + 16);
    st->confirm_remaining_ms = lmux_get_le32(buf + 24);
    st->last_update_error = lmux_get_le32(buf + 28);
    memcpy(st->confirmed_sha256, buf + 32, LMUX_BOOT_UPLOAD_SHA256_LEN);
    memcpy(st->candidate_sha256, buf + 64, LMUX_BOOT_UPLOAD_SHA256_LEN);
    memcpy(st->running_sha256, buf + 96, LMUX_BOOT_UPLOAD_SHA256_LEN);
    return 0;
}

int lmux_boot_encode_sha256(uint8_t *buf, size_t cap, uint32_t *len,
                            const uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN])
{
    if (!buf || !len || !sha256 || cap < LMUX_BOOT_UPLOAD_SHA256_LEN)
        return -1;
    memcpy(buf, sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    *len = LMUX_BOOT_UPLOAD_SHA256_LEN;
    return 0;
}

int lmux_boot_decode_sha256(const uint8_t *buf, uint32_t len,
                            uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN])
{
    if (!buf || !sha256 || len != LMUX_BOOT_UPLOAD_SHA256_LEN)
        return -1;
    memcpy(sha256, buf, LMUX_BOOT_UPLOAD_SHA256_LEN);
    return 0;
}

int lmux_boot_encode_supervisor_install_response(uint8_t *buf, size_t cap,
                                                 uint32_t *len,
                                                 const uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN],
                                                 uint32_t flags)
{
    if (!buf || !len || !sha256 || cap < 40)
        return -1;
    memcpy(buf, sha256, LMUX_BOOT_UPLOAD_SHA256_LEN);
    lmux_put_le32(buf + 32, flags);
    lmux_put_le32(buf + 36, 0);
    *len = 40;
    return 0;
}

int lmux_boot_decode_supervisor_install_response(const uint8_t *buf, uint32_t len,
                                                 struct lmux_boot_supervisor_install_response *out)
{
    if (!buf || !out || len != 40 || lmux_get_le32(buf + 36) != 0)
        return -1;
    memcpy(out->sha256, buf, LMUX_BOOT_UPLOAD_SHA256_LEN);
    out->flags = lmux_get_le32(buf + 32);
    return 0;
}

const char *lmux_boot_status_name(uint16_t status)
{
    switch (status) {
    case LMUX_BOOT_STATUS_OK: return "OK";
    case LMUX_BOOT_STATUS_UNKNOWN_OPCODE: return "UNKNOWN_OPCODE";
    case LMUX_BOOT_STATUS_BAD_REQUEST: return "BAD_REQUEST";
    case LMUX_BOOT_STATUS_BUSY: return "BUSY";
    case LMUX_BOOT_STATUS_UNSUPPORTED: return "UNSUPPORTED";
    case LMUX_BOOT_STATUS_NO_SPACE: return "NO_SPACE";
    case LMUX_BOOT_STATUS_BAD_HASH: return "BAD_HASH";
    case LMUX_BOOT_STATUS_NOT_FOUND: return "NOT_FOUND";
    case LMUX_BOOT_STATUS_WRONG_STATE: return "WRONG_STATE";
    case LMUX_BOOT_STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case LMUX_BOOT_STATUS_ID_MISMATCH: return "ID_MISMATCH";
    case LMUX_BOOT_STATUS_OFFSET_MISMATCH: return "OFFSET_MISMATCH";
    case LMUX_BOOT_STATUS_SIZE_VIOLATION: return "SIZE_VIOLATION";
    case LMUX_BOOT_STATUS_IO_ERROR: return "IO_ERROR";
    case LMUX_BOOT_STATUS_ALREADY_ACTIVE: return "ALREADY_ACTIVE";
    case LMUX_BOOT_STATUS_ALREADY_CONFIRMED: return "ALREADY_CONFIRMED";
    case LMUX_BOOT_STATUS_NOT_TESTING: return "NOT_TESTING";
    default: return "UNKNOWN_STATUS";
    }
}

const char *lmux_boot_worker_state_name(uint32_t state)
{
    switch (state) {
    case LMUX_BOOT_WORKER_UNKNOWN: return "UNKNOWN";
    case LMUX_BOOT_WORKER_STOPPED: return "STOPPED";
    case LMUX_BOOT_WORKER_STARTING: return "STARTING";
    case LMUX_BOOT_WORKER_LINK_DOWN: return "LINK_DOWN";
    case LMUX_BOOT_WORKER_LINK_UP_BARRIER: return "LINK_UP_BARRIER";
    case LMUX_BOOT_WORKER_ACTIVE: return "ACTIVE";
    case LMUX_BOOT_WORKER_BACKOFF: return "BACKOFF";
    default: return "UNKNOWN";
    }
}

const char *lmux_boot_update_phase_name(uint32_t phase)
{
    switch (phase) {
    case LMUX_BOOT_UPDATE_UNMANAGED: return "UNMANAGED";
    case LMUX_BOOT_UPDATE_STABLE: return "STABLE";
    case LMUX_BOOT_UPDATE_TESTING: return "TESTING";
    case LMUX_BOOT_UPDATE_DEGRADED: return "DEGRADED";
    default: return "UNKNOWN";
    }
}
