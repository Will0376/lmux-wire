#ifndef LMUX_LINK_ENDPOINT_H
#define LMUX_LINK_ENDPOINT_H

#include "link_protocol.h"

#include <stddef.h>
#include <stdint.h>

#define LMUX_LINK_TRANSPORT_RING_CAP 4096

enum lmux_link_state {
    LMUX_LINK_DOWN = 0,
    LMUX_LINK_WAIT_HELLO,
    LMUX_LINK_ESTABLISHED,
    LMUX_LINK_ERROR_RESETTING
};

enum lmux_link_event {
    LMUX_LINK_EV_NONE = 0,
    LMUX_LINK_EV_ESTABLISHED = 0x01,
    LMUX_LINK_EV_DOWN = 0x02,
    LMUX_LINK_EV_PEER_RESTART = 0x04
};

struct lmux_link_ring {
    uint8_t buf[LMUX_LINK_TRANSPORT_RING_CAP];
    size_t r;
    size_t w;
    size_t len;
};

struct lmux_link_pending_hello {
    int valid;
    uint64_t conn_id;
    struct lmux_link_hello hello;
};

struct lmux_link_endpoint {
    enum lmux_link_role local_role;
    enum lmux_link_role peer_role;
    enum lmux_link_state state;
    uint64_t local_conn_id;
    uint64_t peer_conn_id;
    uint64_t next_tx_seq;
    uint64_t expected_rx_seq;
    uint32_t local_caps;
    uint32_t peer_caps;
    uint32_t negotiated_caps;
    uint32_t local_max_outer_payload;
    uint32_t local_max_transport_payload;
    uint32_t local_preferred_transport_payload;
    uint32_t peer_max_outer_payload;
    uint32_t peer_max_transport_payload;
    uint32_t effective_transport_payload;
    int local_hello_sent;
    int peer_hello_accepted;
    uint8_t enc_in[LMUX_LINK_MAX_ENCODED];
    size_t enc_in_len;
    int discard_until_delim;
    uint8_t out[LMUX_LINK_MAX_ENCODED];
    size_t out_len;
    size_t out_pos;
    struct lmux_link_ring tx_plain;
    struct lmux_link_ring rx_plain;
    struct lmux_link_pending_hello pending_peer_hello;
    unsigned long long tx_frames;
    unsigned long long rx_frames;
    unsigned long long crc_errors;
    unsigned long long seq_errors;
    unsigned long long resync_drops;
    unsigned long long protocol_errors;
    unsigned long long transport_tx_bytes;
    unsigned long long transport_rx_bytes;
    char last_error[64];
};

void lmux_link_endpoint_init(struct lmux_link_endpoint *ep,
                             enum lmux_link_role role);
void lmux_link_endpoint_begin(struct lmux_link_endpoint *ep,
                              uint64_t conn_id);
void lmux_link_endpoint_down(struct lmux_link_endpoint *ep);
int lmux_link_endpoint_restart_from_pending(struct lmux_link_endpoint *ep,
                                            uint64_t new_local_conn_id,
                                            unsigned *events);

size_t lmux_link_transport_write(struct lmux_link_endpoint *ep,
                                 const uint8_t *buf, size_t len);
size_t lmux_link_transport_read(struct lmux_link_endpoint *ep,
                                uint8_t *buf, size_t len);
size_t lmux_link_transport_tx_len(const struct lmux_link_endpoint *ep);
size_t lmux_link_transport_tx_space(const struct lmux_link_endpoint *ep);
size_t lmux_link_transport_rx_len(const struct lmux_link_endpoint *ep);
size_t lmux_link_transport_rx_space(const struct lmux_link_endpoint *ep);

int lmux_link_wants_physical_write(const struct lmux_link_endpoint *ep);
int lmux_link_can_accept_physical(const struct lmux_link_endpoint *ep);
int lmux_link_has_pending_output(const struct lmux_link_endpoint *ep);
int lmux_link_build_output(struct lmux_link_endpoint *ep);
size_t lmux_link_output_len(const struct lmux_link_endpoint *ep);
const uint8_t *lmux_link_output_ptr(const struct lmux_link_endpoint *ep);
void lmux_link_output_consume(struct lmux_link_endpoint *ep, size_t n);

int lmux_link_input_bytes(struct lmux_link_endpoint *ep,
                          const uint8_t *buf, size_t len,
                          unsigned *events);

const char *lmux_link_state_name(enum lmux_link_state st);

#endif
