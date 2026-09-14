#include "lxf.h"

#include <stdio.h>
#include <string.h>

uint64_t lmux_get_le64(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) | ((uint64_t)lmux_get_le32(p + 4) << 32);
}

void lmux_put_le64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)(v & 0xffffffffu));
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t ror32(uint32_t x, unsigned int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256_ctx *ctx, const uint8_t in[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)in[i * 4] << 24) | ((uint32_t)in[i * 4 + 1] << 16) |
               ((uint32_t)in[i * 4 + 2] << 8) | in[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void sha256_init(struct sha256_ctx *ctx)
{
    static const uint32_t init[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(ctx->h, init, sizeof(init));
    ctx->len = 0;
    ctx->used = 0;
}

void sha256_update(struct sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    ctx->len += (uint64_t)len;
    while (len > 0) {
        size_t n = 64 - ctx->used;
        if (n > len)
            n = len;
        memcpy(ctx->block + ctx->used, data, n);
        ctx->used += n;
        data += n;
        len -= n;
        if (ctx->used == 64) {
            sha256_block(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *ctx, uint8_t out[32])
{
    uint64_t bit_len = ctx->len * 8;
    int i;
    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        while (ctx->used < 64)
            ctx->block[ctx->used++] = 0;
        sha256_block(ctx, ctx->block);
        ctx->used = 0;
    }
    while (ctx->used < 56)
        ctx->block[ctx->used++] = 0;
    for (i = 7; i >= 0; i--)
        ctx->block[ctx->used++] = (uint8_t)(bit_len >> (i * 8));
    sha256_block(ctx, ctx->block);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->h[i];
    }
}

void sha256_hex(const uint8_t digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
}

int lxf_encode_record(uint8_t type, uint8_t flags, const uint8_t *payload,
                      uint32_t payload_len, uint8_t *out, size_t out_cap,
                      size_t *out_len)
{
    if (payload_len > LXF_MAX_PAYLOAD || out_cap < LXF_HEADER_LEN + payload_len)
        return -1;
    memcpy(out, LXF_MAGIC, 4);
    out[4] = type;
    out[5] = flags;
    out[6] = 0;
    out[7] = 0;
    lmux_put_le32(out + 8, payload_len);
    if (payload_len)
        memcpy(out + LXF_HEADER_LEN, payload, payload_len);
    *out_len = LXF_HEADER_LEN + payload_len;
    return 0;
}

int lxf_decode_header(const uint8_t *buf, size_t len, uint8_t *type,
                      uint8_t *flags, uint32_t *payload_len)
{
    if (len < LXF_HEADER_LEN || memcmp(buf, LXF_MAGIC, 4) != 0)
        return -1;
    if (buf[6] || buf[7])
        return -1;
    *type = buf[4];
    *flags = buf[5];
    *payload_len = lmux_get_le32(buf + 8);
    if (*payload_len > LXF_MAX_PAYLOAD)
        return -1;
    return 0;
}

const char *lxf_strerror_code(uint32_t code)
{
    switch (code) {
    case LXF_E_PROTOCOL: return "protocol error";
    case LXF_E_IO: return "I/O error";
    case LXF_E_NOTFOUND: return "no such file";
    case LXF_E_DENIED: return "permission denied";
    case LXF_E_BADPATH: return "bad path";
    case LXF_E_NOTREG: return "not a regular file";
    case LXF_E_HASH: return "hash mismatch";
    case LXF_E_NOSPACE: return "no space left";
    default: return "file transfer error";
    }
}
