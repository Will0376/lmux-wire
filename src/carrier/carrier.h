#ifndef LMUX_CARRIER_H
#define LMUX_CARRIER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum lmux_carrier_kind {
    LMUX_CARRIER_NONE = 0,
    LMUX_CARRIER_TTY_PATH,
    LMUX_CARRIER_FD,
    LMUX_CARRIER_UNIX_LISTEN,
    LMUX_CARRIER_UNIX_CONNECT
};

enum lmux_carrier_io_status {
    LMUX_CARRIER_IO_PROGRESS = 0,
    LMUX_CARRIER_IO_WOULD_BLOCK,
    LMUX_CARRIER_IO_LOST,
    LMUX_CARRIER_IO_FATAL
};

enum lmux_serial_parity {
    LMUX_SERIAL_PARITY_NONE = 0,
    LMUX_SERIAL_PARITY_EVEN,
    LMUX_SERIAL_PARITY_ODD
};

enum lmux_serial_flow {
    LMUX_SERIAL_FLOW_NONE = 0,
    LMUX_SERIAL_FLOW_RTSCTS,
    LMUX_SERIAL_FLOW_XONXOFF
};

struct lmux_serial_config {
    unsigned baud;
    unsigned data_bits;
    enum lmux_serial_parity parity;
    unsigned stop_bits;
    enum lmux_serial_flow flow;
};

struct lmux_carrier_io {
    enum lmux_carrier_io_status status;
    ssize_t n;
    int err;
};

struct lmux_carrier {
    enum lmux_carrier_kind kind;
    int fd;
    int listen_fd;
    int owns_fd;
    int created_path;
    const char *path;
    const char *label;
    long long next_attempt_ms;
    unsigned long long open_count;
    unsigned long long lost_count;
    unsigned long long open_fail_count;
    unsigned long long rx_bytes;
    unsigned long long tx_bytes;
    int last_errno;
    struct lmux_serial_config serial;
};

void lmux_serial_config_default(struct lmux_serial_config *cfg);
int lmux_serial_configure_fd(int fd, const struct lmux_serial_config *cfg);

void lmux_carrier_init(struct lmux_carrier *c);
void lmux_carrier_cleanup(struct lmux_carrier *c);
int lmux_carrier_set_tty_path(struct lmux_carrier *c, const char *path);
int lmux_carrier_set_fd(struct lmux_carrier *c, int fd);
int lmux_carrier_set_unix_listen(struct lmux_carrier *c, const char *path);
int lmux_carrier_set_unix_connect(struct lmux_carrier *c, const char *path);
int lmux_carrier_start(struct lmux_carrier *c);
int lmux_carrier_try_open(struct lmux_carrier *c, long long now_ms);
int lmux_carrier_accept(struct lmux_carrier *c);
void lmux_carrier_mark_lost(struct lmux_carrier *c, long long now_ms);

int lmux_carrier_fd(const struct lmux_carrier *c);
int lmux_carrier_listen_fd(const struct lmux_carrier *c);
const char *lmux_carrier_label(const struct lmux_carrier *c);
int lmux_carrier_is_configured(const struct lmux_carrier *c);
int lmux_carrier_can_reopen(const struct lmux_carrier *c);

struct lmux_carrier_io lmux_carrier_read(struct lmux_carrier *c,
                                         void *buf, size_t len);
struct lmux_carrier_io lmux_carrier_write(struct lmux_carrier *c,
                                          const void *buf, size_t len);

#endif
