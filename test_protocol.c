#include "lxf.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *msg)
{
    fprintf(stderr, "selftest failed: %s\n", msg);
    return 1;
}

int main(void)
{
    const uint8_t check[] = "123456789";
    uint8_t all[256];
    uint8_t enc[LMUX_MAX_ENCODED];
    uint8_t dec[LMUX_MAX_DECODED];
    size_t enc_len = 0, dec_len = 0;
    struct lmux_frame fr, out;
    int i;
    struct sha256_ctx sha;
    uint8_t digest[32];
    char hex[65];
    uint8_t rec[LXF_MAX_RECORD], payload[256], type, flags;
    uint32_t plen;

    if (lmux_crc16_ccitt_false(check, 9) != 0x29b1)
        return fail("CRC-16/CCITT-FALSE vector");

    for (i = 0; i < 256; i++)
        all[i] = (uint8_t)i;
    if (lmux_cobs_encode(all, sizeof(all), enc, sizeof(enc), &enc_len) < 0)
        return fail("COBS encode");
    if (lmux_cobs_decode(enc, enc_len, dec, sizeof(dec), &dec_len) < 0)
        return fail("COBS decode");
    if (dec_len != sizeof(all) || memcmp(dec, all, sizeof(all)) != 0)
        return fail("COBS round-trip");

    memset(&fr, 0, sizeof(fr));
    fr.version = LMUX_VERSION;
    fr.type = LMUX_T_DATA;
    fr.channel = 2;
    fr.sequence = 0x1234;
    fr.payload_len = sizeof(all);
    memcpy(fr.payload, all, sizeof(all));
    if (lmux_encode_frame(&fr, enc, sizeof(enc), &enc_len) < 0)
        return fail("frame encode");
    if (enc[enc_len - 1] != 0)
        return fail("frame delimiter");
    if (lmux_decode_frame(enc, enc_len - 1, &out) < 0)
        return fail("frame decode");
    if (out.type != fr.type || out.channel != fr.channel ||
        out.sequence != fr.sequence || out.payload_len != fr.payload_len ||
        memcmp(out.payload, fr.payload, fr.payload_len) != 0)
        return fail("frame round-trip");

    enc[3] ^= 0x55;
    if (lmux_decode_frame(enc, enc_len - 1, &out) == 0)
        return fail("bad CRC rejection");

    sha256_init(&sha);
    sha256_update(&sha, check, 9);
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    if (strcmp(hex, "15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225") != 0)
        return fail("SHA-256 vector");

    for (i = 0; i < 256; i++)
        payload[i] = (uint8_t)i;
    if (lxf_encode_record(LXF_T_DATA, 0, payload, sizeof(payload), rec, sizeof(rec), &enc_len) < 0)
        return fail("LXF encode");
    if (lxf_decode_header(rec, enc_len, &type, &flags, &plen) < 0)
        return fail("LXF decode header");
    if (type != LXF_T_DATA || flags != 0 || plen != sizeof(payload) ||
        memcmp(rec + LXF_HEADER_LEN, payload, sizeof(payload)) != 0)
        return fail("LXF binary payload");
    lmux_put_le32(rec + 8, LXF_MAX_PAYLOAD + 1);
    if (lxf_decode_header(rec, LXF_HEADER_LEN, &type, &flags, &plen) == 0)
        return fail("LXF malformed length rejection");

    puts("protocol selftest ok");
    return 0;
}
