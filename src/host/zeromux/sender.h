#ifndef LMUX_ZEROMUX_SENDER_H
#define LMUX_ZEROMUX_SENDER_H

#include "probe.h"

#include <stdint.h>

enum lmux_zmx_sender_status {
    LMUX_ZMX_SENDER_OK = 0,
    LMUX_ZMX_SENDER_UNSUPPORTED,
    LMUX_ZMX_SENDER_SESSION_LOST,
    LMUX_ZMX_SENDER_TIMEOUT,
    LMUX_ZMX_SENDER_REMOTE_FAILED,
    LMUX_ZMX_SENDER_TRANSFER_FAILED,
    LMUX_ZMX_SENDER_VERIFY_FAILED,
    LMUX_ZMX_SENDER_LOCAL_IO,
    LMUX_ZMX_SENDER_BAD_PATH
};

struct lmux_zmx_sender_result {
    enum lmux_zmx_sender_status status;
    const char *backend;
    uint64_t bytes;
    uint8_t sha256[32];
    int verified;
    int cleanup_attempted;
    int cleanup_ok;
    char detail[128];
};

const char *lmux_zmx_sender_status_name(enum lmux_zmx_sender_status st);
void lmux_zmx_sender_result_init(struct lmux_zmx_sender_result *r);
enum lmux_zmx_tri lmux_zmx_probe_tool_state(const struct lmux_zmx_probe_result *r,
                                            const char *name);
int lmux_zmx_shell_quote(const char *in, char *out, size_t cap);
int lmux_zmx_sender_upload_file(struct lmux_zmx_session *s,
                                const struct lmux_zmx_probe_result *probe,
                                const char *local_path,
                                const char *target_temp,
                                struct lmux_zmx_sender_result *res);
int lmux_zmx_uuencode_line(const uint8_t *data, size_t len,
                           char *out, size_t cap, size_t *out_len);

#endif
