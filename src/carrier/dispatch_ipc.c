#include "dispatch_ipc.h"

#include <errno.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int lmux_dispatch_ipc_type_valid(uint8_t type)
{
    return type >= LMUX_DISPATCH_IPC_RX_RECORD &&
        type <= LMUX_DISPATCH_IPC_RELEASE;
}

int lmux_dispatch_ipc_encode(uint8_t *out, size_t cap,
                             uint8_t type, const void *payload,
                             size_t payload_len, size_t *out_len)
{
    if (!out || !out_len || !lmux_dispatch_ipc_type_valid(type) ||
        payload_len > LMUX_DISPATCH_IPC_PAYLOAD_MAX ||
        (payload_len && !payload) ||
        (type >= LMUX_DISPATCH_IPC_ACTIVE && payload_len != 0) ||
        cap < LMUX_DISPATCH_IPC_HEADER_LEN + payload_len) {
        errno = EINVAL;
        return -1;
    }
    put32(out, LMUX_DISPATCH_IPC_MAGIC);
    out[4] = LMUX_DISPATCH_IPC_VERSION;
    out[5] = type;
    put16(out + 6, (uint16_t)payload_len);
    if (payload_len)
        memcpy(out + LMUX_DISPATCH_IPC_HEADER_LEN, payload, payload_len);
    *out_len = LMUX_DISPATCH_IPC_HEADER_LEN + payload_len;
    return 0;
}

int lmux_dispatch_ipc_decode(const void *buf, size_t len,
                             struct lmux_dispatch_ipc_message *out)
{
    const uint8_t *p = buf;
    uint16_t payload_len;
    if (!buf || !out || len < LMUX_DISPATCH_IPC_HEADER_LEN ||
        get32(p) != LMUX_DISPATCH_IPC_MAGIC ||
        p[4] != LMUX_DISPATCH_IPC_VERSION ||
        !lmux_dispatch_ipc_type_valid(p[5])) {
        errno = EPROTO;
        return -1;
    }
    payload_len = get16(p + 6);
    if (payload_len > LMUX_DISPATCH_IPC_PAYLOAD_MAX ||
        len != LMUX_DISPATCH_IPC_HEADER_LEN + payload_len ||
        (p[5] >= LMUX_DISPATCH_IPC_ACTIVE && payload_len != 0)) {
        errno = EPROTO;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->type = p[5];
    out->payload_len = payload_len;
    if (payload_len)
        memcpy(out->payload, p + LMUX_DISPATCH_IPC_HEADER_LEN, payload_len);
    return 0;
}
