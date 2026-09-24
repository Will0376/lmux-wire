#include "sender.h"

#include <stdio.h>
#include <string.h>

const char *lmux_zmx_sender_status_name(enum lmux_zmx_sender_status st)
{
    switch (st) {
    case LMUX_ZMX_SENDER_OK: return "OK";
    case LMUX_ZMX_SENDER_UNSUPPORTED: return "UNSUPPORTED";
    case LMUX_ZMX_SENDER_SESSION_LOST: return "SESSION_LOST";
    case LMUX_ZMX_SENDER_TIMEOUT: return "TIMEOUT";
    case LMUX_ZMX_SENDER_REMOTE_FAILED: return "REMOTE_FAILED";
    case LMUX_ZMX_SENDER_TRANSFER_FAILED: return "TRANSFER_FAILED";
    case LMUX_ZMX_SENDER_VERIFY_FAILED: return "VERIFY_FAILED";
    case LMUX_ZMX_SENDER_LOCAL_IO: return "LOCAL_IO";
    case LMUX_ZMX_SENDER_BAD_PATH: return "BAD_PATH";
    }
    return "?";
}

void lmux_zmx_sender_result_init(struct lmux_zmx_sender_result *r)
{
    memset(r, 0, sizeof(*r));
    r->status = LMUX_ZMX_SENDER_OK;
}

enum lmux_zmx_tri lmux_zmx_probe_tool_state(const struct lmux_zmx_probe_result *r,
                                            const char *name)
{
    size_t i;
    if (!r || !name)
        return LMUX_ZMX_UNKNOWN;
    for (i = 0; i < LMUX_ZMX_TOOL_COUNT; i++) {
        if (r->tools[i].name && strcmp(r->tools[i].name, name) == 0)
            return r->tools[i].state;
    }
    return LMUX_ZMX_UNKNOWN;
}

int lmux_zmx_shell_quote(const char *in, char *out, size_t cap)
{
    size_t used = 0;
    const char *p;
    if (!in || !*in || !out || cap < 3)
        return -1;
    out[used++] = '\'';
    for (p = in; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7f)
            return -1;
        if (ch == '\'') {
            if (used + 4 >= cap)
                return -1;
            memcpy(out + used, "'\\''", 4);
            used += 4;
        } else {
            if (used + 1 >= cap)
                return -1;
            out[used++] = (char)ch;
        }
    }
    if (used + 2 > cap)
        return -1;
    out[used++] = '\'';
    out[used] = 0;
    return 0;
}

int lmux_zmx_sender_upload_file(struct lmux_zmx_session *s,
                                const struct lmux_zmx_probe_result *probe,
                                const char *local_path,
                                const char *target_temp,
                                struct lmux_zmx_sender_result *res)
{
    int lmux_zmx_sender_uudecode_upload(struct lmux_zmx_session *,
                                        const struct lmux_zmx_probe_result *,
                                        const char *, const char *,
                                        struct lmux_zmx_sender_result *);
    lmux_zmx_sender_result_init(res);
    if (lmux_zmx_probe_tool_state(probe, "uudecode") != LMUX_ZMX_YES) {
        res->status = LMUX_ZMX_SENDER_UNSUPPORTED;
        snprintf(res->detail, sizeof(res->detail), "uudecode unavailable");
        return -1;
    }
    return lmux_zmx_sender_uudecode_upload(s, probe, local_path,
                                           target_temp, res);
}
