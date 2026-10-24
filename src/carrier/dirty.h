#ifndef LMUX_DIRTY_H
#define LMUX_DIRTY_H

#include <stddef.h>
#include <stdint.h>

#define LMUX_DIRTY_VERSION 1
#define LMUX_DIRTY_PREFIX "~LMUX-D1:"
#define LMUX_DIRTY_PREFIX_LEN 9

#define LMUX_DIRTY_PACKET_MAX 1024
#define LMUX_DIRTY_FRAGMENT_MAX 192
#define LMUX_DIRTY_FRAGMENTS_MAX 6
#define LMUX_DIRTY_WINDOW_MAX 2
#define LMUX_DIRTY_CONTROL_MAX (LMUX_DIRTY_WINDOW_MAX + 1)
#define LMUX_DIRTY_RETRY_MAX 5
#define LMUX_DIRTY_DEADLINE_MAX_MS 30000u
#define LMUX_DIRTY_DIAG_SAMPLE_MAX 32u

#define LMUX_DIRTY_HEADER_LEN 40
#define LMUX_DIRTY_BINARY_MAX \
    (LMUX_DIRTY_HEADER_LEN + LMUX_DIRTY_FRAGMENT_MAX + 4)
#define LMUX_DIRTY_BASE64_MAX (((LMUX_DIRTY_BINARY_MAX + 2) / 3) * 4)
#define LMUX_DIRTY_RECORD_MAX \
    (LMUX_DIRTY_PREFIX_LEN + LMUX_DIRTY_BASE64_MAX + 2)

enum lmux_dirty_type {
    LMUX_DIRTY_DATA = 1,
    LMUX_DIRTY_ACK = 2,
    LMUX_DIRTY_NACK = 3,
    LMUX_DIRTY_FAIL = 4
};

enum lmux_dirty_fail_reason {
    LMUX_DIRTY_FAIL_NONE = 0,
    LMUX_DIRTY_FAIL_BAD_RECORD = 1,
    LMUX_DIRTY_FAIL_RX_WINDOW = 2,
    LMUX_DIRTY_FAIL_PACKET_CRC = 3,
    LMUX_DIRTY_FAIL_RETRY_EXHAUSTED = 4,
    LMUX_DIRTY_FAIL_DEADLINE = 5,
    LMUX_DIRTY_FAIL_PEER = 6,
    LMUX_DIRTY_FAIL_PROTOCOL = 7
};

enum lmux_dirty_parse_reason {
    LMUX_DIRTY_PARSE_NONE = 0,
    LMUX_DIRTY_PARSE_RESTART_PREFIX = 1,
    LMUX_DIRTY_PARSE_BAD_TERMINATOR = 2,
    LMUX_DIRTY_PARSE_EARLY_LF = 3,
    LMUX_DIRTY_PARSE_BAD_ALPHABET = 4,
    LMUX_DIRTY_PARSE_OVERSIZE = 5,
    LMUX_DIRTY_PARSE_BASE64 = 6,
    LMUX_DIRTY_PARSE_CANONICAL = 7,
    LMUX_DIRTY_PARSE_HEADER = 8,
    LMUX_DIRTY_PARSE_LENGTH = 9,
    LMUX_DIRTY_PARSE_RECORD_CRC = 10,
    LMUX_DIRTY_PARSE_SEMANTIC = 11,
    LMUX_DIRTY_PARSE_FRAGMENT_CRC = 12,
    LMUX_DIRTY_PARSE_REASON_COUNT = 13
};

struct lmux_dirty_record {
    uint8_t type;
    uint8_t flags;
    uint64_t session_id;
    uint32_t packet_seq;
    uint16_t packet_len;
    uint16_t fragment_offset;
    uint8_t fragment_index;
    uint8_t fragment_count;
    uint8_t missing_bitmap;
    uint8_t reason;
    uint16_t payload_len;
    uint32_t packet_crc;
    uint32_t fragment_crc;
    uint8_t payload[LMUX_DIRTY_FRAGMENT_MAX];
};

struct lmux_dirty_tx_slot {
    uint8_t used;
    uint8_t fragment_count;
    uint8_t sent_bitmap;
    uint8_t resend_bitmap;
    uint8_t retry_rounds;
    uint8_t acked;
    uint16_t len;
    uint32_t seq;
    uint32_t packet_crc;
    uint64_t started_ms;
    uint64_t retry_at_ms;
    uint64_t deadline_ms;
    uint8_t data[LMUX_DIRTY_PACKET_MAX];
};

struct lmux_dirty_rx_slot {
    uint8_t used;
    uint8_t verified;
    uint8_t fragment_count;
    uint8_t received_bitmap;
    uint8_t nack_rounds;
    uint16_t len;
    uint32_t seq;
    uint32_t packet_crc;
    uint64_t started_ms;
    uint64_t nack_at_ms;
    uint64_t deadline_ms;
    uint8_t data[LMUX_DIRTY_PACKET_MAX];
};

struct lmux_dirty_control {
    uint8_t used;
    uint8_t type;
    uint8_t bitmap;
    uint8_t reason;
    uint64_t session_id;
    uint32_t seq;
};

struct lmux_dirty_parser {
    char body[LMUX_DIRTY_BASE64_MAX + 1];
    uint16_t body_len;
    uint8_t prefix_match;
    uint8_t in_record;
    uint8_t saw_cr;
    uint8_t invalid;
    uint8_t invalid_reason;
    uint8_t foreign_active;
    uint16_t invalid_offset;
};

struct lmux_dirty_diagnostic {
    uint64_t invalid_reason[LMUX_DIRTY_PARSE_REASON_COUNT];
    uint64_t foreign_runs;
    uint64_t last_valid_ms;
    uint64_t last_invalid_ms;
    uint64_t last_foreign_ms;
    uint64_t last_valid_session_id;
    uint32_t last_valid_packet_seq;
    uint32_t last_valid_wire_hash;
    uint32_t last_invalid_wire_hash;
    uint16_t last_valid_wire_len;
    uint16_t last_invalid_wire_len;
    uint16_t last_invalid_offset;
    uint8_t last_valid_type;
    uint8_t last_valid_fragment_index;
    uint8_t last_valid_fragment_count;
    uint8_t last_invalid_reason;
    uint8_t last_invalid_sample_len;
    uint8_t last_foreign_sample_len;
    uint8_t last_invalid_sample[LMUX_DIRTY_DIAG_SAMPLE_MAX];
    uint8_t last_foreign_sample[LMUX_DIRTY_DIAG_SAMPLE_MAX];
};

struct lmux_dirty_stats {
    uint64_t valid_records;
    uint64_t invalid_records;
    uint64_t foreign_bytes;
    uint64_t data_fragments;
    uint64_t duplicate_fragments;
    uint64_t stale_records;
    uint64_t retransmitted_fragments;
    uint64_t delivered_packets;
    uint64_t packet_crc_failures;
    uint64_t ack_sent;
    uint64_t ack_received;
    uint64_t nack_sent;
    uint64_t nack_received;
    uint64_t retry_failures;
    uint64_t deadline_failures;
};

struct lmux_dirty {
    uint64_t local_session_id;
    uint64_t peer_session_id;
    uint32_t tx_next_seq;
    uint32_t rx_next_seq;
    uint32_t last_delivered_seq[LMUX_DIRTY_WINDOW_MAX];
    uint8_t delivered_history;
    uint32_t baud;
    uint32_t retry_interval_ms;
    struct lmux_dirty_tx_slot tx[LMUX_DIRTY_WINDOW_MAX];
    struct lmux_dirty_rx_slot rx[LMUX_DIRTY_WINDOW_MAX];
    struct lmux_dirty_control control[LMUX_DIRTY_CONTROL_MAX];
    struct lmux_dirty_parser parser;
    uint8_t output[LMUX_DIRTY_RECORD_MAX];
    uint16_t output_len;
    uint16_t output_pos;
    uint8_t output_kind;
    uint8_t output_slot;
    uint8_t output_bit;
    uint8_t pending_fail;
    uint8_t failed;
    uint8_t fail_reason;
    struct lmux_dirty_stats stats;
    struct lmux_dirty_diagnostic diagnostic;
};

int lmux_dirty_record_encode(const struct lmux_dirty_record *record,
                             uint8_t *out, size_t out_cap, size_t *out_len);
/* 0: valid DIRTY record, 1: foreign/non-DIRTY, -1: malformed DIRTY. */
int lmux_dirty_record_decode(const uint8_t *wire, size_t wire_len,
                             struct lmux_dirty_record *record);
int lmux_dirty_record_decode_diagnostic(const uint8_t *wire, size_t wire_len,
                                        struct lmux_dirty_record *record,
                                        uint8_t *parse_reason);

void lmux_dirty_init(struct lmux_dirty *dirty, uint64_t local_session_id,
                     uint64_t peer_session_id, uint32_t tx_first_seq,
                     uint32_t rx_first_seq, uint32_t baud, uint64_t now_ms);
void lmux_dirty_reset_peer(struct lmux_dirty *dirty, uint64_t peer_session_id,
                           uint32_t rx_first_seq, uint64_t now_ms);

/* One submitted chunk is one packet. EAGAIN means bounded TX backpressure. */
int lmux_dirty_submit(struct lmux_dirty *dirty, const uint8_t *data, size_t len,
                      uint64_t now_ms, uint32_t *packet_seq);
int lmux_dirty_feed(struct lmux_dirty *dirty, const uint8_t *data, size_t len,
                    uint64_t now_ms);
int lmux_dirty_tick(struct lmux_dirty *dirty, uint64_t now_ms);
void lmux_dirty_peer_lost(struct lmux_dirty *dirty);

int lmux_dirty_output_peek(struct lmux_dirty *dirty, uint64_t now_ms,
                           const uint8_t **data, size_t *len);
int lmux_dirty_output_consume(struct lmux_dirty *dirty, size_t len);

int lmux_dirty_receive_peek(const struct lmux_dirty *dirty,
                            const uint8_t **data, size_t *len,
                            uint32_t *packet_seq);
int lmux_dirty_receive_consume(struct lmux_dirty *dirty);

int lmux_dirty_failed(const struct lmux_dirty *dirty);
const char *lmux_dirty_fail_reason_name(uint8_t reason);
const char *lmux_dirty_parse_reason_name(uint8_t reason);
size_t lmux_dirty_memory_footprint(void);

#endif
