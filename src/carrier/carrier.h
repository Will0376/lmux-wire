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
    LMUX_CARRIER_UNIX_CONNECT,
    LMUX_CARRIER_DISPATCH_CONNECT
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

enum lmux_tty_policy {
    LMUX_TTY_POLICY_BYTE_STREAM = 0,
    LMUX_TTY_POLICY_STRICT_UART
};

struct lmux_serial_config {
    unsigned baud;
    unsigned data_bits;
    enum lmux_serial_parity parity;
    unsigned stop_bits;
    enum lmux_serial_flow flow;
};

enum lmux_tty_config_stage {
    LMUX_TTY_CONFIG_NONE = 0,
    LMUX_TTY_CONFIG_OPEN,
    LMUX_TTY_CONFIG_EXCLUSIVE,
    LMUX_TTY_CONFIG_BAUD,
    LMUX_TTY_CONFIG_ARGUMENTS,
    LMUX_TTY_CONFIG_TCGETATTR_INITIAL,
    LMUX_TTY_CONFIG_CFSETISPEED,
    LMUX_TTY_CONFIG_CFSETOSPEED,
    LMUX_TTY_CONFIG_TCSETATTR,
    LMUX_TTY_CONFIG_TCGETATTR_VERIFY,
    LMUX_TTY_CONFIG_VALIDATE
};

enum lmux_tty_validation_mismatch {
    LMUX_TTY_MISMATCH_ISPEED = 1u << 0,
    LMUX_TTY_MISMATCH_OSPEED = 1u << 1,
    LMUX_TTY_MISMATCH_CLOCAL = 1u << 2,
    LMUX_TTY_MISMATCH_CREAD = 1u << 3,
    LMUX_TTY_MISMATCH_CSIZE = 1u << 4,
    LMUX_TTY_MISMATCH_CONTROL = 1u << 5,
    LMUX_TTY_MISMATCH_LFLAG = 1u << 6,
    LMUX_TTY_MISMATCH_OFLAG = 1u << 7,
    LMUX_TTY_MISMATCH_IFLAG = 1u << 8
};

struct lmux_tty_diagnostic {
    enum lmux_tty_config_stage stage;
    int syscall_errno;
    uint32_t validation_mismatch;
    unsigned long expected_ispeed;
    unsigned long expected_ospeed;
    unsigned long actual_ispeed;
    unsigned long actual_ospeed;
    unsigned long expected_iflag;
    unsigned long expected_oflag;
    unsigned long expected_cflag;
    unsigned long expected_lflag;
    unsigned long actual_iflag;
    unsigned long actual_oflag;
    unsigned long actual_cflag;
    unsigned long actual_lflag;
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
    int exclusive_requested;
    int exclusive_acquired;
    enum lmux_tty_policy tty_policy;
    struct lmux_serial_config serial;
    struct lmux_tty_diagnostic tty_diagnostic;
    uint8_t dispatch_tx_record[384];
    size_t dispatch_tx_len;
    size_t dispatch_tx_unreported;
    uint8_t dispatch_control;
    uint8_t dispatch_rx_record[384];
    size_t dispatch_rx_len;
    size_t dispatch_rx_pos;
};

void lmux_serial_config_default(struct lmux_serial_config *cfg);
int lmux_serial_configure_fd(int fd, const struct lmux_serial_config *cfg);
const char *lmux_tty_config_stage_name(enum lmux_tty_config_stage stage);
const char *lmux_tty_policy_name(enum lmux_tty_policy policy);
uint32_t lmux_tty_validation_mismatch(
    enum lmux_tty_policy policy, const struct lmux_tty_diagnostic *diag);

void lmux_carrier_init(struct lmux_carrier *c);
void lmux_carrier_cleanup(struct lmux_carrier *c);
int lmux_carrier_set_tty_path(struct lmux_carrier *c, const char *path);
int lmux_carrier_set_fd(struct lmux_carrier *c, int fd);
int lmux_carrier_set_unix_listen(struct lmux_carrier *c, const char *path);
int lmux_carrier_set_unix_connect(struct lmux_carrier *c, const char *path);
int lmux_carrier_set_dispatch_connect(struct lmux_carrier *c,
                                      const char *path);
void lmux_carrier_set_exclusive(struct lmux_carrier *c, int required);
void lmux_carrier_set_tty_policy(struct lmux_carrier *c,
                                 enum lmux_tty_policy policy);
int lmux_carrier_start(struct lmux_carrier *c);
int lmux_carrier_try_open(struct lmux_carrier *c, long long now_ms);
int lmux_carrier_accept(struct lmux_carrier *c);
void lmux_carrier_mark_lost(struct lmux_carrier *c, long long now_ms);

int lmux_carrier_fd(const struct lmux_carrier *c);
int lmux_carrier_listen_fd(const struct lmux_carrier *c);
const char *lmux_carrier_label(const struct lmux_carrier *c);
int lmux_carrier_is_configured(const struct lmux_carrier *c);
int lmux_carrier_can_reopen(const struct lmux_carrier *c);
int lmux_carrier_wants_write(const struct lmux_carrier *c);
int lmux_carrier_dispatch_control(struct lmux_carrier *c, uint8_t type);
int lmux_carrier_flush_pending(struct lmux_carrier *c);
void lmux_carrier_dispatch_abort(struct lmux_carrier *c);

struct lmux_carrier_io lmux_carrier_read(struct lmux_carrier *c,
                                         void *buf, size_t len);
struct lmux_carrier_io lmux_carrier_write(struct lmux_carrier *c,
                                          const void *buf, size_t len);

#endif
