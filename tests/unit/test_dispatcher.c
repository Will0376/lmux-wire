#include "dispatcher.h"
#include "dispatch_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct capture {
    uint8_t data[LMUX_DISPATCH_RECORD_MAX];
    size_t len;
    unsigned count;
};

static int capture_record(void *opaque, const uint8_t *data, size_t len)
{
    struct capture *c = opaque;
    if (len > sizeof(c->data))
        return -1;
    memcpy(c->data, data, len);
    c->len = len;
    c->count++;
    return 0;
}

static int test_ipc(void)
{
    uint8_t wire[LMUX_DISPATCH_IPC_MESSAGE_MAX];
    const uint8_t payload[] = "~LMUX-D1:AA==\r\n";
    struct lmux_dispatch_ipc_message msg;
    size_t len;
    if (lmux_dispatch_ipc_encode(wire, sizeof(wire),
                                 LMUX_DISPATCH_IPC_RX_RECORD,
                                 payload, sizeof(payload) - 1, &len) < 0 ||
        lmux_dispatch_ipc_decode(wire, len, &msg) < 0 ||
        msg.type != LMUX_DISPATCH_IPC_RX_RECORD ||
        msg.payload_len != sizeof(payload) - 1 ||
        memcmp(msg.payload, payload, sizeof(payload) - 1) != 0)
        return -1;
    if (lmux_dispatch_ipc_decode(wire, len - 1, &msg) == 0)
        return -1;
    if (lmux_dispatch_ipc_encode(wire, sizeof(wire),
                                 LMUX_DISPATCH_IPC_ACTIVE,
                                 payload, 1, &len) == 0)
        return -1;
    return 0;
}

static int test_parser(void)
{
    static const uint8_t record[] = "noise~LMUX-C1:QUJDRA==\r\n";
    static const uint8_t embedded[] =
        "~LMUX-D1:fkxNVVggQ09OU09MRQ==\r\n";
    struct lmux_dispatch_parser p;
    struct capture c;
    size_t i;
    memset(&c, 0, sizeof(c));
    lmux_dispatch_parser_init(&p);
    for (i = 0; i < sizeof(record) - 1; i++)
        if (lmux_dispatch_parser_feed(&p, record + i, 1,
                                      capture_record, &c) != 0) {
            fprintf(stderr, "parser unexpected result at byte %zu\n", i);
            return -1;
        }
    if (c.count != 1 || c.len != sizeof(record) - 1 - 5 ||
        memcmp(c.data, record + 5, c.len) != 0) {
        fprintf(stderr, "parser capture count=%u len=%zu expected=%zu\n",
                c.count, c.len, sizeof(record) - 1 - 5);
        return -1;
    }
    if (lmux_dispatch_parser_feed(&p, embedded, sizeof(embedded) - 1,
                                  capture_record, &c) != 0 || c.count != 2) {
        fprintf(stderr, "parser embedded count=%u\n", c.count);
        return -1;
    }
    lmux_dispatch_parser_init(&p);
    if (lmux_dispatch_parser_feed(&p,
            (const uint8_t *)"~LMUX CONSOLE\n", 14,
            capture_record, &c) != 1) {
        fprintf(stderr, "parser LF escape failed\n");
        return -1;
    }
    lmux_dispatch_parser_init(&p);
    if (lmux_dispatch_parser_feed(&p,
            (const uint8_t *)"~LMUX CONSOLE\r\n", 15,
            capture_record, &c) != 1 || p.pending_cr) {
        fprintf(stderr, "parser CRLF escape failed pending=%d\n", p.pending_cr);
        return -1;
    }
    lmux_dispatch_parser_init(&p);
    if (lmux_dispatch_parser_feed(&p,
            (const uint8_t *)"~LMUX CONSOLE\r", 14,
            capture_record, &c) != 0 ||
        lmux_dispatch_parser_finish_cr(&p) != 1) {
        fprintf(stderr, "parser CR escape failed pending=%d\n", p.pending_cr);
        return -1;
    }
    lmux_dispatch_parser_init(&p);
    if (lmux_dispatch_parser_feed(&p,
            (const uint8_t *)"x~LMUX CONSOLE\n", 15,
            capture_record, &c) != 0) {
        fprintf(stderr, "parser false escape\n");
        return -1;
    }
    return 0;
}

static int test_tty_encoding(void)
{
    static const uint8_t record[] = "~LMUX-D1:AA==\r\n";
    uint8_t out[sizeof(record)];
    struct termios tio;
    size_t len;
    memset(&tio, 0, sizeof(tio));
    if (lmux_dispatch_tty_encode(&tio, record, sizeof(record) - 1,
                                 out, sizeof(out), &len) < 0 ||
        len != sizeof(record) - 1 || memcmp(out, record, len) != 0)
        return -1;
    tio.c_oflag = OPOST | ONLCR;
    if (lmux_dispatch_tty_encode(&tio, record, sizeof(record) - 1,
                                 out, sizeof(out), &len) < 0 ||
        len != sizeof(record) - 2 || out[len - 1] != '\n' ||
        memchr(out, '\r', len) != NULL)
        return -1;
#ifdef OCRNL
    tio.c_oflag |= OCRNL;
    errno = 0;
    if (lmux_dispatch_tty_encode(&tio, record, sizeof(record) - 1,
                                 out, sizeof(out), &len) == 0 ||
        errno != ENOTSUP)
        return -1;
#endif
    return 0;
}

int main(void)
{
    if (test_ipc() < 0) {
        fprintf(stderr, "dispatcher IPC unit test failed\n");
        return 1;
    }
    if (test_parser() < 0) {
        fprintf(stderr, "dispatcher parser unit test failed\n");
        return 1;
    }
    if (test_tty_encoding() < 0) {
        fprintf(stderr, "dispatcher tty encoding unit test failed\n");
        return 1;
    }
    printf("dispatcher unit tests passed\n");
    return 0;
}
