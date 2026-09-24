#ifndef LMUX_BOOT_PROTOCOL_H
#define LMUX_BOOT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define LMUX_BOOT_VERSION 1

#define LMUX_BOOT_REQ_HEADER_LEN 12
#define LMUX_BOOT_RESP_HEADER_LEN 16
#define LMUX_BOOT_UPLOAD_SHA256_LEN 32
#define LMUX_BOOT_UPLOAD_MAX_CHUNK 1024
#define LMUX_BOOT_UPDATE_STATUS_LEN 128

enum lmux_boot_opcode {
    LMUX_BOOT_GET_INFO = 1,
    LMUX_BOOT_WORKER_STATUS = 2,
    LMUX_BOOT_UPLOAD_BEGIN = 16,
    LMUX_BOOT_UPLOAD_DATA = 17,
    LMUX_BOOT_UPLOAD_END = 18,
    LMUX_BOOT_UPDATE_STATUS = 19,
    LMUX_BOOT_CANDIDATE_STATUS = 19,
    LMUX_BOOT_ACTIVATE_CANDIDATE = 20,
    LMUX_BOOT_CONFIRM_CANDIDATE = 21,
    LMUX_BOOT_ROLLBACK = 22,
    LMUX_BOOT_RESTART_WORKER = 23,
    LMUX_BOOT_UPLOAD_ABORT = 24,
    LMUX_BOOT_SUPERVISOR_INSTALL = 25,
    LMUX_BOOT_SUPERVISOR_RESTART = 26
};

enum lmux_boot_status {
    LMUX_BOOT_STATUS_OK = 0,
    LMUX_BOOT_STATUS_UNKNOWN_OPCODE = 1,
    LMUX_BOOT_STATUS_BAD_REQUEST = 2,
    LMUX_BOOT_STATUS_BUSY = 3,
    LMUX_BOOT_STATUS_UNSUPPORTED = 4,
    LMUX_BOOT_STATUS_NO_SPACE = 5,
    LMUX_BOOT_STATUS_BAD_HASH = 6,
    LMUX_BOOT_STATUS_NOT_FOUND = 7,
    LMUX_BOOT_STATUS_WRONG_STATE = 8,
    LMUX_BOOT_STATUS_INTERNAL_ERROR = 9,
    LMUX_BOOT_STATUS_ID_MISMATCH = 10,
    LMUX_BOOT_STATUS_OFFSET_MISMATCH = 11,
    LMUX_BOOT_STATUS_SIZE_VIOLATION = 12,
    LMUX_BOOT_STATUS_IO_ERROR = 13,
    LMUX_BOOT_STATUS_ALREADY_ACTIVE = 14,
    LMUX_BOOT_STATUS_ALREADY_CONFIRMED = 15,
    LMUX_BOOT_STATUS_NOT_TESTING = 16
};

enum lmux_boot_worker_state {
    LMUX_BOOT_WORKER_UNKNOWN = 0,
    LMUX_BOOT_WORKER_STOPPED = 1,
    LMUX_BOOT_WORKER_STARTING = 2,
    LMUX_BOOT_WORKER_LINK_DOWN = 3,
    LMUX_BOOT_WORKER_LINK_UP_BARRIER = 4,
    LMUX_BOOT_WORKER_ACTIVE = 5,
    LMUX_BOOT_WORKER_BACKOFF = 6
};

struct lmux_boot_request {
    uint16_t opcode;
    uint16_t flags;
    uint64_t request_id;
    const uint8_t *body;
    uint32_t body_len;
};

struct lmux_boot_response {
    uint16_t opcode;
    uint16_t status;
    uint64_t request_id;
    const uint8_t *body;
    uint32_t body_len;
};

struct lmux_boot_info {
    uint16_t outer_version;
    uint16_t boot_version;
    uint32_t capabilities;
    uint32_t max_outer_payload;
    uint32_t max_transport_payload;
};

struct lmux_boot_worker_status {
    uint32_t worker_state;
    uint32_t flags;
    uint64_t worker_incarnation;
    uint64_t uptime_ms;
    uint32_t restart_count;
};

struct lmux_boot_upload_begin {
    uint64_t declared_size;
};

struct lmux_boot_upload_begin_response {
    uint64_t upload_id;
    uint32_t max_chunk;
};

struct lmux_boot_upload_data {
    uint64_t upload_id;
    uint64_t offset;
    const uint8_t *data;
    uint32_t data_len;
};

struct lmux_boot_upload_data_response {
    uint64_t next_offset;
};

struct lmux_boot_upload_end {
    uint64_t upload_id;
    uint8_t expected_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN];
};

struct lmux_boot_upload_end_response {
    uint64_t size;
    uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN];
};

enum lmux_boot_update_phase {
    LMUX_BOOT_UPDATE_UNMANAGED = 0,
    LMUX_BOOT_UPDATE_STABLE = 1,
    LMUX_BOOT_UPDATE_TESTING = 2,
    LMUX_BOOT_UPDATE_DEGRADED = 3
};

enum lmux_boot_update_flag {
    LMUX_BOOT_UPDATE_TIMER_RUNNING = 0x00000001u,
    LMUX_BOOT_UPDATE_TIMER_PAUSED = 0x00000002u
};

struct lmux_boot_activate {
    uint8_t candidate_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN];
    uint32_t confirm_timeout_ms;
};

struct lmux_boot_activate_response {
    uint64_t activation_id;
    uint64_t generation;
    uint32_t confirm_timeout_ms;
    uint32_t phase;
};

struct lmux_boot_update_status {
    uint32_t phase;
    uint32_t flags;
    uint64_t generation;
    uint64_t activation_id;
    uint32_t confirm_remaining_ms;
    uint32_t last_update_error;
    uint8_t confirmed_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN];
    uint8_t candidate_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN];
    uint8_t running_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN];
};

enum lmux_boot_supervisor_install_flag {
    LMUX_BOOT_SUPERVISOR_INSTALL_INSTALLED = 0x00000001u,
    LMUX_BOOT_SUPERVISOR_INSTALL_ALREADY = 0x00000002u
};

struct lmux_boot_supervisor_install_response {
    uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN];
    uint32_t flags;
};

int lmux_boot_parse_request(const uint8_t *buf, uint32_t len,
                            struct lmux_boot_request *out);
int lmux_boot_make_request(uint8_t *buf, size_t cap, uint32_t *len,
                           uint16_t opcode, uint64_t request_id,
                           const uint8_t *body, uint32_t body_len);
int lmux_boot_parse_response(const uint8_t *buf, uint32_t len,
                             struct lmux_boot_response *out);
int lmux_boot_make_response(uint8_t *buf, size_t cap, uint32_t *len,
                            uint16_t opcode, uint16_t status,
                            uint64_t request_id,
                            const uint8_t *body, uint32_t body_len);

int lmux_boot_encode_get_info_response(uint8_t *buf, size_t cap, uint32_t *len,
                                       const struct lmux_boot_info *info);
int lmux_boot_decode_get_info_response(const uint8_t *buf, uint32_t len,
                                       struct lmux_boot_info *info);
int lmux_boot_encode_worker_status(uint8_t *buf, size_t cap, uint32_t *len,
                                   const struct lmux_boot_worker_status *st);
int lmux_boot_decode_worker_status(const uint8_t *buf, uint32_t len,
                                   struct lmux_boot_worker_status *st);

int lmux_boot_encode_upload_begin(uint8_t *buf, size_t cap, uint32_t *len,
                                  uint64_t declared_size);
int lmux_boot_decode_upload_begin(const uint8_t *buf, uint32_t len,
                                  struct lmux_boot_upload_begin *out);
int lmux_boot_encode_upload_begin_response(uint8_t *buf, size_t cap,
                                           uint32_t *len, uint64_t upload_id,
                                           uint32_t max_chunk);
int lmux_boot_decode_upload_begin_response(const uint8_t *buf, uint32_t len,
                                           struct lmux_boot_upload_begin_response *out);
int lmux_boot_encode_upload_data(uint8_t *buf, size_t cap, uint32_t *len,
                                 uint64_t upload_id, uint64_t offset,
                                 const uint8_t *data, uint32_t data_len);
int lmux_boot_decode_upload_data(const uint8_t *buf, uint32_t len,
                                 struct lmux_boot_upload_data *out);
int lmux_boot_encode_upload_data_response(uint8_t *buf, size_t cap,
                                          uint32_t *len, uint64_t next_offset);
int lmux_boot_decode_upload_data_response(const uint8_t *buf, uint32_t len,
                                          struct lmux_boot_upload_data_response *out);
int lmux_boot_encode_upload_end(uint8_t *buf, size_t cap, uint32_t *len,
                                uint64_t upload_id,
                                const uint8_t expected_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN]);
int lmux_boot_decode_upload_end(const uint8_t *buf, uint32_t len,
                                struct lmux_boot_upload_end *out);
int lmux_boot_encode_upload_end_response(uint8_t *buf, size_t cap,
                                         uint32_t *len, uint64_t size,
                                         const uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN]);
int lmux_boot_decode_upload_end_response(const uint8_t *buf, uint32_t len,
                                         struct lmux_boot_upload_end_response *out);
int lmux_boot_encode_upload_abort(uint8_t *buf, size_t cap, uint32_t *len,
                                  uint64_t upload_id);
int lmux_boot_decode_upload_abort(const uint8_t *buf, uint32_t len,
                                  uint64_t *upload_id);
int lmux_boot_encode_activate(uint8_t *buf, size_t cap, uint32_t *len,
                              const uint8_t candidate_sha256[LMUX_BOOT_UPLOAD_SHA256_LEN],
                              uint32_t confirm_timeout_ms);
int lmux_boot_decode_activate(const uint8_t *buf, uint32_t len,
                              struct lmux_boot_activate *out);
int lmux_boot_encode_activate_response(uint8_t *buf, size_t cap, uint32_t *len,
                                       uint64_t activation_id,
                                       uint64_t generation,
                                       uint32_t confirm_timeout_ms,
                                       uint32_t phase);
int lmux_boot_decode_activate_response(const uint8_t *buf, uint32_t len,
                                       struct lmux_boot_activate_response *out);
int lmux_boot_encode_activation_id(uint8_t *buf, size_t cap, uint32_t *len,
                                   uint64_t activation_id);
int lmux_boot_decode_activation_id(const uint8_t *buf, uint32_t len,
                                   uint64_t *activation_id);
int lmux_boot_encode_update_status(uint8_t *buf, size_t cap, uint32_t *len,
                                   const struct lmux_boot_update_status *st);
int lmux_boot_decode_update_status(const uint8_t *buf, uint32_t len,
                                   struct lmux_boot_update_status *st);
int lmux_boot_encode_sha256(uint8_t *buf, size_t cap, uint32_t *len,
                            const uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN]);
int lmux_boot_decode_sha256(const uint8_t *buf, uint32_t len,
                            uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN]);
int lmux_boot_encode_supervisor_install_response(uint8_t *buf, size_t cap,
                                                 uint32_t *len,
                                                 const uint8_t sha256[LMUX_BOOT_UPLOAD_SHA256_LEN],
                                                 uint32_t flags);
int lmux_boot_decode_supervisor_install_response(const uint8_t *buf, uint32_t len,
                                                 struct lmux_boot_supervisor_install_response *out);

const char *lmux_boot_status_name(uint16_t status);
const char *lmux_boot_worker_state_name(uint32_t state);
const char *lmux_boot_update_phase_name(uint32_t phase);

#endif
