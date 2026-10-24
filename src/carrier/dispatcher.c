#include "dispatcher.h"
#include "lmc1.h"

#include <errno.h>
#include <string.h>

static const uint8_t common_prefix[] = "~LMUX-";

void lmux_dispatch_parser_init(struct lmux_dispatch_parser *p)
{
    memset(p, 0, sizeof(*p));
    p->line_possible = 1;
}

static void line_byte(struct lmux_dispatch_parser *p, uint8_t ch)
{
    static const char escape[] = LMUX_DISPATCH_ESCAPE;
    if (ch == '\r' || ch == '\n')
        return;
    if (!p->line_possible)
        return;
    if (p->line_len >= sizeof(escape) - 1 ||
        ch != (uint8_t)escape[p->line_len]) {
        p->line_possible = 0;
        return;
    }
    p->line[p->line_len++] = (char)ch;
}

static int line_end(struct lmux_dispatch_parser *p, uint8_t ch)
{
    int match = p->line_possible &&
        p->line_len == sizeof(LMUX_DISPATCH_ESCAPE) - 1;
    p->line_len = 0;
    p->line_possible = 1;
    p->pending_cr = ch == '\r';
    p->escape_pending = p->pending_cr && match;
    return ch == '\n' && match;
}

static int feed_one(struct lmux_dispatch_parser *p, uint8_t ch,
                    lmux_dispatch_record_cb cb, void *opaque)
{
    if (p->pending_cr) {
        int escape = p->escape_pending;
        p->pending_cr = 0;
        p->escape_pending = 0;
        if (ch == '\n')
            return escape; /* consume CRLF as one physical line ending */
        if (escape)
            return 1;
    }
    if (p->in_record) {
        if (p->record_len >= sizeof(p->record)) {
            p->invalid_records++;
            p->in_record = 0;
            p->record_len = 0;
            return 0;
        }
        p->record[p->record_len++] = ch;
        if (ch == '\n') {
            int rc = cb ? cb(opaque, p->record, p->record_len) : 0;
            p->in_record = 0;
            p->record_len = 0;
            p->line_len = 0;
            p->line_possible = 1;
            return rc < 0 ? -1 : 0;
        }
        return 0;
    }
    line_byte(p, ch);
    if (ch == '\r' || ch == '\n') {
        int escape = line_end(p, ch);
        if (escape)
            return 1;
        return 0;
    }
    if (p->record_len) {
        int match = 0;
        if (p->record_len < sizeof(common_prefix) - 1)
            match = ch == common_prefix[p->record_len];
        else if (p->record_len == 6)
            match = ch == 'C' || ch == 'D';
        else if (p->record_len == 7)
            match = ch == '1';
        else if (p->record_len == 8)
            match = ch == ':';
        if (match) {
            p->record[p->record_len++] = ch;
            if (p->record_len == 9)
                p->in_record = 1;
            return 0;
        }
        p->record_len = 0;
    }
    if (ch == common_prefix[0]) {
        p->record[0] = ch;
        p->record_len = 1;
        return 0;
    }
    p->foreign_bytes++;
    return 0;
}

int lmux_dispatch_parser_feed(struct lmux_dispatch_parser *p,
                              const uint8_t *buf, size_t len,
                              lmux_dispatch_record_cb cb, void *opaque)
{
    size_t i;
    if (!p || (!buf && len)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < len; i++) {
        uint8_t ch = buf[i];
        int rc;
        rc = feed_one(p, ch, cb, opaque);
        if (rc != 0)
            return rc;
    }
    return 0;
}

int lmux_dispatch_parser_finish_cr(struct lmux_dispatch_parser *p)
{
    int escape;
    if (!p || !p->pending_cr)
        return 0;
    escape = p->escape_pending;
    p->pending_cr = 0;
    p->escape_pending = 0;
    return escape;
}

static tcflag_t unsafe_output_flags(void)
{
    tcflag_t flags = 0;
#ifdef OLCUC
    flags |= OLCUC;
#endif
#ifdef OCRNL
    flags |= OCRNL;
#endif
#ifdef ONOCR
    flags |= ONOCR;
#endif
#ifdef ONLRET
    flags |= ONLRET;
#endif
#ifdef OFILL
    flags |= OFILL;
#endif
#ifdef NLDLY
    flags |= NLDLY;
#endif
#ifdef CRDLY
    flags |= CRDLY;
#endif
#ifdef TABDLY
    flags |= TABDLY;
#endif
#ifdef BSDLY
    flags |= BSDLY;
#endif
#ifdef VTDLY
    flags |= VTDLY;
#endif
#ifdef FFDLY
    flags |= FFDLY;
#endif
    return flags;
}

int lmux_dispatch_tty_encode(const struct termios *tio,
                             const uint8_t *logical, size_t logical_len,
                             uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t i;
    int onlcr;
    if (!tio || !logical || !out || !out_len || logical_len > out_cap) {
        errno = EINVAL;
        return -1;
    }
    if ((tio->c_oflag & OPOST) && (tio->c_oflag & unsafe_output_flags())) {
        errno = ENOTSUP;
        return -1;
    }
    onlcr = (tio->c_oflag & OPOST) && (tio->c_oflag & ONLCR);
    if (!onlcr) {
        memcpy(out, logical, logical_len);
        *out_len = logical_len;
        return 0;
    }
    if (logical_len < 2 || logical[logical_len - 2] != '\r' ||
        logical[logical_len - 1] != '\n') {
        errno = EPROTO;
        return -1;
    }
    for (i = 0; i + 2 < logical_len; i++)
        if (logical[i] == '\r' || logical[i] == '\n') {
            errno = EPROTO;
            return -1;
        }
    memcpy(out, logical, logical_len - 2);
    out[logical_len - 2] = '\n';
    *out_len = logical_len - 1;
    return 0;
}
