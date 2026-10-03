#ifndef LMUX2_CORE_H
#define LMUX2_CORE_H

#include "lmux2_protocol.h"

#include <stddef.h>
#include <stdint.h>

#define LMUX2_CORE_MAX_STREAMS 32
#define LMUX2_STREAM_RX_CAP 1024
#define LMUX2_STREAM_TX_CAP 1024
#define LMUX2_CONN_RX_CAP 4096
#define LMUX2_OUTQ_CAP 32

enum lmux2_conn_state {
    LMUX2_CONN_WAIT_HELLO = 0,
    LMUX2_CONN_ESTABLISHED,
    LMUX2_CONN_ERROR
};

enum lmux2_stream_state {
    LMUX2_STREAM_UNUSED = 0,
    LMUX2_STREAM_OPEN_SENT,
    LMUX2_STREAM_OPEN_RECEIVED,
    LMUX2_STREAM_OPEN,
    LMUX2_STREAM_LOCAL_CLOSED,
    LMUX2_STREAM_REMOTE_CLOSED,
    LMUX2_STREAM_RESET,
    LMUX2_STREAM_TERMINAL
};

struct lmux2_ring {
    uint8_t buf[LMUX2_STREAM_RX_CAP];
    size_t r;
    size_t w;
    size_t len;
};

struct lmux2_stream {
    enum lmux2_stream_state state;
    uint64_t id;
    uint64_t service_id;
    uint64_t open_token;
    uint64_t send_offset;
    uint64_t expected_recv_offset;
    uint64_t recv_consumed;
    uint64_t local_max_stream_data;
    uint64_t advertised_max_stream_data;
    uint64_t last_sent_max_stream_data;
    uint64_t peer_max_stream_data;
    int max_stream_data_dirty;
    uint32_t local_error;
    uint32_t remote_error;
    int local_close_requested;
    int local_close_sent;
    struct lmux2_ring rx;
    struct lmux2_ring tx;
};

struct lmux2_conn {
    enum lmux2_role role;
    enum lmux2_conn_state state;
    uint64_t local_conn_id;
    uint64_t peer_conn_id;
    uint64_t next_local_stream_id;
    uint64_t highest_peer_stream_id_seen;
    uint64_t conn_send_offset;
    uint64_t conn_recv_offset;
    uint64_t conn_rx_consumed;
    uint64_t local_max_data;
    uint64_t advertised_max_data;
    uint64_t last_sent_max_data;
    uint64_t peer_max_data;
    int max_data_dirty;
    int hello_sent;
    int rr_next;
    unsigned long long frames_tx;
    unsigned long long frames_rx;
    unsigned long long protocol_errors;
    unsigned long long stream_errors;
    unsigned long long stale_frames;
    unsigned long long pre_established_drops;
    unsigned long long peer_restart_count;
    char last_error[96];
    struct lmux2_stream streams[LMUX2_CORE_MAX_STREAMS];
    struct lmux2_frame outq[LMUX2_OUTQ_CAP];
    size_t outq_r;
    size_t outq_len;
};

void lmux2_conn_init(struct lmux2_conn *c, enum lmux2_role role, uint64_t conn_id);
void lmux2_conn_begin_physical(struct lmux2_conn *c, uint64_t conn_id);
int lmux2_conn_start(struct lmux2_conn *c);
int lmux2_handle_frame(struct lmux2_conn *c, const struct lmux2_frame *fr);
int lmux2_next_frame(struct lmux2_conn *c, struct lmux2_frame *fr);
int lmux2_queue_frame(struct lmux2_conn *c, const struct lmux2_frame *fr);

int lmux2_open_stream(struct lmux2_conn *c, uint64_t service_id, uint64_t *stream_id);
int lmux2_open_stream_ex(struct lmux2_conn *c, uint64_t service_id,
                         uint64_t pending_token, uint64_t *stream_id);
int lmux2_accept_stream(struct lmux2_conn *c, uint64_t stream_id);
int lmux2_reject_stream(struct lmux2_conn *c, uint64_t stream_id, uint32_t error_code);
int lmux2_close_stream(struct lmux2_conn *c, uint64_t stream_id);
int lmux2_reset_stream(struct lmux2_conn *c, uint64_t stream_id, uint32_t error_code);

size_t lmux2_stream_write(struct lmux2_conn *c, uint64_t stream_id,
                          const uint8_t *buf, size_t len);
size_t lmux2_stream_read(struct lmux2_conn *c, uint64_t stream_id,
                         uint8_t *buf, size_t len);

struct lmux2_stream *lmux2_find_stream(struct lmux2_conn *c, uint64_t stream_id);
const struct lmux2_stream *lmux2_find_stream_const(const struct lmux2_conn *c,
                                                   uint64_t stream_id);
size_t lmux2_stream_rx_len(const struct lmux2_conn *c, uint64_t stream_id);
size_t lmux2_stream_tx_len(const struct lmux2_conn *c, uint64_t stream_id);

#endif
