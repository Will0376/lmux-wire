#ifndef LMUX_DISPATCH_IPC_H
#define LMUX_DISPATCH_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "dirty.h"

#define LMUX_DISPATCH_IPC_MAGIC 0x3150444cu /* "LDP1", little endian */
#define LMUX_DISPATCH_IPC_VERSION 1u
#define LMUX_DISPATCH_IPC_HEADER_LEN 8u
#define LMUX_DISPATCH_IPC_PAYLOAD_MAX LMUX_DIRTY_RECORD_MAX
#define LMUX_DISPATCH_IPC_MESSAGE_MAX \
    (LMUX_DISPATCH_IPC_HEADER_LEN + LMUX_DISPATCH_IPC_PAYLOAD_MAX)

enum lmux_dispatch_ipc_type {
    LMUX_DISPATCH_IPC_RX_RECORD = 1,
    LMUX_DISPATCH_IPC_TX_RECORD = 2,
    LMUX_DISPATCH_IPC_ACTIVE = 3,
    LMUX_DISPATCH_IPC_IDLE = 4,
    LMUX_DISPATCH_IPC_RELEASE = 5
};

struct lmux_dispatch_ipc_message {
    uint8_t type;
    uint16_t payload_len;
    uint8_t payload[LMUX_DISPATCH_IPC_PAYLOAD_MAX];
};

int lmux_dispatch_ipc_encode(uint8_t *out, size_t cap,
                             uint8_t type, const void *payload,
                             size_t payload_len, size_t *out_len);
int lmux_dispatch_ipc_decode(const void *buf, size_t len,
                             struct lmux_dispatch_ipc_message *out);
int lmux_dispatch_ipc_type_valid(uint8_t type);

#endif
