#ifndef LMUX_ZEROMUX_SESSION_H
#define LMUX_ZEROMUX_SESSION_H

#include "carrier.h"

#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#define LMUX_ZMX_NONCE_MAX 40
#define LMUX_ZMX_RESPONSE_MAX 8192

enum lmux_zmx_error {
    LMUX_ZMX_OK = 0,
    LMUX_ZMX_AUTH_REQUIRED,
    LMUX_ZMX_NO_USABLE_SHELL,
    LMUX_ZMX_TIMEOUT,
    LMUX_ZMX_CARRIER_LOST,
    LMUX_ZMX_PROTOCOL_ERROR,
    LMUX_ZMX_IO_ERROR
};

struct lmux_zmx_session {
    int fd;
    int owns_fd;
    int restore_termios;
    struct termios saved_termios;
    unsigned timeout_ms;
    uint64_t nonce_counter;
    enum lmux_zmx_error last_error;
    char last_phase[32];
};

struct lmux_zmx_command_result {
    char output[LMUX_ZMX_RESPONSE_MAX];
    size_t output_len;
    int exit_status;
};

struct lmux_zmx_stream;
typedef int (*lmux_zmx_stream_payload_fn)(struct lmux_zmx_stream *st,
                                          void *ctx);

void lmux_zmx_session_init(struct lmux_zmx_session *s);
int lmux_zmx_session_open_tty(struct lmux_zmx_session *s, const char *path,
                              const struct lmux_serial_config *serial,
                              unsigned timeout_ms);
int lmux_zmx_session_use_fd(struct lmux_zmx_session *s, int fd,
                            int owns_fd, unsigned timeout_ms);
void lmux_zmx_session_close(struct lmux_zmx_session *s);
const char *lmux_zmx_error_name(enum lmux_zmx_error err);

int lmux_zmx_session_establish(struct lmux_zmx_session *s);
int lmux_zmx_session_command(struct lmux_zmx_session *s, const char *command,
                             struct lmux_zmx_command_result *res);
int lmux_zmx_session_stream_command(struct lmux_zmx_session *s,
                                    const char *before_payload,
                                    lmux_zmx_stream_payload_fn payload,
                                    void *payload_ctx,
                                    struct lmux_zmx_command_result *res);
int lmux_zmx_session_stream_foreground(struct lmux_zmx_session *s,
                                       const char *start_fmt,
                                       const char *command_arg,
                                       lmux_zmx_stream_payload_fn payload,
                                       void *payload_ctx,
                                       struct lmux_zmx_command_result *res);
int lmux_zmx_stream_write(struct lmux_zmx_stream *st,
                          const char *buf, size_t len);

#endif
