#ifndef LMUX_UART_DISPATCHER_H
#define LMUX_UART_DISPATCHER_H

#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#include "dirty.h"

#define LMUX_DISPATCH_ESCAPE "~LMUX CONSOLE"
#define LMUX_DISPATCH_RECORD_MAX LMUX_DIRTY_RECORD_MAX

struct lmux_dispatch_parser {
    uint8_t record[LMUX_DISPATCH_RECORD_MAX];
    size_t record_len;
    char line[sizeof(LMUX_DISPATCH_ESCAPE)];
    size_t line_len;
    int line_possible;
    int pending_cr;
    int escape_pending;
    int in_record;
    unsigned long long foreign_bytes;
    unsigned long long invalid_records;
};

typedef int (*lmux_dispatch_record_cb)(void *opaque,
                                       const uint8_t *record, size_t len);

void lmux_dispatch_parser_init(struct lmux_dispatch_parser *p);
/* Returns 1 when the emergency escape was consumed, 0 otherwise, -1 on cb. */
int lmux_dispatch_parser_feed(struct lmux_dispatch_parser *p,
                              const uint8_t *buf, size_t len,
                              lmux_dispatch_record_cb cb, void *opaque);
/* Resolve a CR-only terminator after the dispatcher grace interval. */
int lmux_dispatch_parser_finish_cr(struct lmux_dispatch_parser *p);

/* Build bytes which, after the current tty output processing, appear exactly
 * as `logical` on the physical wire. No termios mutation is performed. */
int lmux_dispatch_tty_encode(const struct termios *tio,
                             const uint8_t *logical, size_t logical_len,
                             uint8_t *out, size_t out_cap, size_t *out_len);

int lmux_uart_dispatch_main(int argc, char **argv);

#endif
