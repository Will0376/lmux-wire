#ifndef LMUX_H
#define LMUX_H

#include <stddef.h>
#include <stdint.h>

#define LMUX_PATH_MAX 256

uint16_t lmux_get_le16(const uint8_t *p);
uint32_t lmux_get_le32(const uint8_t *p);
void lmux_put_le16(uint8_t *p, uint16_t v);
void lmux_put_le32(uint8_t *p, uint32_t v);

uint16_t lmux_crc16_ccitt_false(const uint8_t *buf, size_t len);
int lmux_cobs_encode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);
int lmux_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len);

#endif
