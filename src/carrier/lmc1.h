#ifndef LMUX_LMC1_H
#define LMUX_LMC1_H

#include "carrier.h"
#include "carrier_manager.h"

#include <stddef.h>
#include <stdint.h>

#define LMUX_LMC1_VERSION 1
#define LMUX_LMC1_NONCE_LEN 16
#define LMUX_LMC1_COOKIE_LEN 16
#define LMUX_LMC1_BOOT_ID_LEN 16
#define LMUX_LMC1_DECODED_LEN 112
#define LMUX_LMC1_MAX_ENCODED 114
#define LMUX_LMC1_RAW_RECORD_MAX 4352
#define LMUX_LMC1_DEFAULT_DEADLINE_MS 5000u
#define LMUX_LMC1_DIRTY_PREFIX "~LMUX-C1:"
#define LMUX_LMC1_DIRTY_PREFIX_LEN 9
#define LMUX_LMC1_DIRTY_BASE64_MAX (((LMUX_LMC1_MAX_ENCODED + 2) / 3) * 4)
#define LMUX_LMC1_DIRTY_RECORD_MAX \
    (LMUX_LMC1_DIRTY_PREFIX_LEN + LMUX_LMC1_DIRTY_BASE64_MAX + 2)

enum lmux_lmc1_codec {
    LMUX_LMC1_CODEC_RAW = 1,
    LMUX_LMC1_CODEC_DIRTY_BOOTSTRAP = 2
};

enum lmux_lmc1_type {
    LMUX_LMC1_PREPARE = 1,
    LMUX_LMC1_READY = 2,
    LMUX_LMC1_COMMIT = 3,
    LMUX_LMC1_COMMITTED = 4,
    LMUX_LMC1_QUERY = 5,
    LMUX_LMC1_QUERY_REPLY = 6,
    LMUX_LMC1_ERROR = 7,
    LMUX_LMC1_RELEASE = 8,
    LMUX_LMC1_RELEASED = 9,
    LMUX_LMC1_RESUME = 10,
    LMUX_LMC1_RESUMED = 11
};

enum lmux_lmc1_status {
    LMUX_LMC1_STATUS_OK = 0,
    LMUX_LMC1_STATUS_BUSY = 1,
    LMUX_LMC1_STATUS_UNKNOWN_CARRIER = 2,
    LMUX_LMC1_STATUS_UNSUPPORTED_MODE = 3,
    LMUX_LMC1_STATUS_STALE = 4,
    LMUX_LMC1_STATUS_BAD_COOKIE = 5,
    LMUX_LMC1_STATUS_EXPIRED = 6,
    LMUX_LMC1_STATUS_BAD_REQUEST = 7,
    LMUX_LMC1_STATUS_UNAVAILABLE = 8
};

struct lmux_lmc1_msg {
    uint8_t type;
    uint16_t status;
    uint64_t generation;
    uint32_t deadline_ms;
    uint8_t nonce[LMUX_LMC1_NONCE_LEN];
    uint8_t cookie[LMUX_LMC1_COOKIE_LEN];
    uint8_t boot_id[LMUX_LMC1_BOOT_ID_LEN];
    enum lmux_carrier_mode mode;
    char name[LMUX_CARRIER_NAME_MAX + 1];
};

struct lmux_lmc1_raw_io {
    uint8_t rx[LMUX_LMC1_RAW_RECORD_MAX];
    size_t rx_len;
    int discard_until_delim;
    uint8_t tx[LMUX_LMC1_MAX_ENCODED];
    size_t tx_len;
    size_t tx_pos;
    uint8_t tx_type;
    unsigned long long valid_control_records;
    unsigned long long invalid_control_records;
    unsigned long long foreign_records;
};

struct lmux_lmc1_dirty_io {
    char rx[LMUX_LMC1_DIRTY_BASE64_MAX + 1];
    uint16_t rx_len;
    uint8_t prefix_match;
    uint8_t in_record;
    uint8_t saw_cr;
    uint8_t invalid;
    uint8_t tx[LMUX_LMC1_DIRTY_RECORD_MAX];
    uint16_t tx_len;
    uint16_t tx_pos;
    uint8_t tx_type;
    unsigned long long valid_control_records;
    unsigned long long invalid_control_records;
    unsigned long long foreign_bytes;
};

typedef void (*lmux_lmc1_control_cb)(void *opaque,
                                     const struct lmux_lmc1_msg *msg);
typedef void (*lmux_lmc1_outer_cb)(void *opaque,
                                   const uint8_t *encoded, size_t len);

int lmux_lmc1_encode(const struct lmux_lmc1_msg *msg,
                     uint8_t *out, size_t out_cap, size_t *out_len);

/* 0: valid LMC1, 1: a different record magic, -1: malformed LMC1. */
int lmux_lmc1_decode(const uint8_t *encoded, size_t encoded_len,
                     struct lmux_lmc1_msg *msg);

void lmux_lmc1_raw_init(struct lmux_lmc1_raw_io *io);
int lmux_lmc1_raw_queue(struct lmux_lmc1_raw_io *io,
                        const struct lmux_lmc1_msg *msg);
int lmux_lmc1_raw_feed(struct lmux_lmc1_raw_io *io,
                       const uint8_t *buf, size_t len, int allow_outer,
                       lmux_lmc1_control_cb control_cb,
                       lmux_lmc1_outer_cb outer_cb, void *opaque);
int lmux_lmc1_raw_flush(struct lmux_lmc1_raw_io *io,
                        struct lmux_carrier *carrier,
                        uint8_t *completed_type);
int lmux_lmc1_raw_has_output(const struct lmux_lmc1_raw_io *io);
void lmux_lmc1_raw_reset_rx(struct lmux_lmc1_raw_io *io);

void lmux_lmc1_dirty_init(struct lmux_lmc1_dirty_io *io);
int lmux_lmc1_dirty_queue(struct lmux_lmc1_dirty_io *io,
                          const struct lmux_lmc1_msg *msg);
int lmux_lmc1_dirty_feed(struct lmux_lmc1_dirty_io *io,
                         const uint8_t *buf, size_t len,
                         lmux_lmc1_control_cb control_cb, void *opaque);
int lmux_lmc1_dirty_flush(struct lmux_lmc1_dirty_io *io,
                          struct lmux_carrier *carrier,
                          uint8_t *completed_type);
int lmux_lmc1_dirty_has_output(const struct lmux_lmc1_dirty_io *io);
int lmux_lmc1_dirty_receiving(const struct lmux_lmc1_dirty_io *io);
void lmux_lmc1_dirty_reset_rx(struct lmux_lmc1_dirty_io *io);

/* Deterministic per-generation DIRTY namespaces, not authentication. */
void lmux_lmc1_dirty_session_ids(const uint8_t boot_id[LMUX_LMC1_BOOT_ID_LEN],
                                 uint64_t generation, const char *name,
                                 uint64_t *host_session,
                                 uint64_t *target_session);

const char *lmux_lmc1_type_name(uint8_t type);
const char *lmux_lmc1_status_name(uint16_t status);
int lmux_lmc1_nonce_equal(const uint8_t a[LMUX_LMC1_NONCE_LEN],
                          const uint8_t b[LMUX_LMC1_NONCE_LEN]);

#endif
