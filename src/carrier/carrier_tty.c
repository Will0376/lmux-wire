#include "carrier.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef CRTSCTS
#define CRTSCTS 020000000000
#endif

void lmux_serial_config_default(struct lmux_serial_config *cfg)
{
    cfg->baud = 115200;
    cfg->data_bits = 8;
    cfg->parity = LMUX_SERIAL_PARITY_NONE;
    cfg->stop_bits = 1;
    cfg->flow = LMUX_SERIAL_FLOW_NONE;
}

static speed_t baud_to_speed(unsigned baud)
{
    switch (baud) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
#ifdef B57600
    case 57600: return B57600;
#endif
#ifdef B115200
    case 115200: return B115200;
#endif
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B500000
    case 500000: return B500000;
#endif
#ifdef B576000
    case 576000: return B576000;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B1152000
    case 1152000: return B1152000;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
#ifdef B2500000
    case 2500000: return B2500000;
#endif
#ifdef B3000000
    case 3000000: return B3000000;
#endif
#ifdef B3500000
    case 3500000: return B3500000;
#endif
#ifdef B4000000
    case 4000000: return B4000000;
#endif
    default:
        errno = EINVAL;
        return (speed_t)0;
    }
}

const char *lmux_tty_config_stage_name(enum lmux_tty_config_stage stage)
{
    switch (stage) {
    case LMUX_TTY_CONFIG_NONE: return "none";
    case LMUX_TTY_CONFIG_OPEN: return "open";
    case LMUX_TTY_CONFIG_EXCLUSIVE: return "tioc_excl";
    case LMUX_TTY_CONFIG_BAUD: return "baud_map";
    case LMUX_TTY_CONFIG_ARGUMENTS: return "arguments";
    case LMUX_TTY_CONFIG_TCGETATTR_INITIAL: return "tcgetattr_initial";
    case LMUX_TTY_CONFIG_CFSETISPEED: return "cfsetispeed";
    case LMUX_TTY_CONFIG_CFSETOSPEED: return "cfsetospeed";
    case LMUX_TTY_CONFIG_TCSETATTR: return "tcsetattr";
    case LMUX_TTY_CONFIG_TCGETATTR_VERIFY: return "tcgetattr_verify";
    case LMUX_TTY_CONFIG_VALIDATE: return "validation";
    default: return "unknown";
    }
}

const char *lmux_tty_policy_name(enum lmux_tty_policy policy)
{
    switch (policy) {
    case LMUX_TTY_POLICY_BYTE_STREAM: return "byte-stream";
    case LMUX_TTY_POLICY_STRICT_UART: return "strict-uart";
    default: return "unknown";
    }
}

static void diagnostic_failure(struct lmux_tty_diagnostic *diag,
                               enum lmux_tty_config_stage stage,
                               int syscall_errno)
{
    if (!diag)
        return;
    diag->stage = stage;
    diag->syscall_errno = syscall_errno;
}

static void diagnostic_termios(struct lmux_tty_diagnostic *diag,
                               const struct termios *expected,
                               const struct termios *actual)
{
    if (!diag)
        return;
    diag->expected_ispeed = (unsigned long)cfgetispeed(expected);
    diag->expected_ospeed = (unsigned long)cfgetospeed(expected);
    diag->actual_ispeed = (unsigned long)cfgetispeed(actual);
    diag->actual_ospeed = (unsigned long)cfgetospeed(actual);
    diag->expected_iflag = (unsigned long)expected->c_iflag;
    diag->expected_oflag = (unsigned long)expected->c_oflag;
    diag->expected_cflag = (unsigned long)expected->c_cflag;
    diag->expected_lflag = (unsigned long)expected->c_lflag;
    diag->actual_iflag = (unsigned long)actual->c_iflag;
    diag->actual_oflag = (unsigned long)actual->c_oflag;
    diag->actual_cflag = (unsigned long)actual->c_cflag;
    diag->actual_lflag = (unsigned long)actual->c_lflag;
}

uint32_t lmux_tty_validation_mismatch(
    enum lmux_tty_policy policy, const struct lmux_tty_diagnostic *diag)
{
    uint32_t mismatch = 0;
    const unsigned long control_mask = PARENB | PARODD | CSTOPB | CRTSCTS;
    const unsigned long iflag_mask =
        IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR;
    const unsigned long lflag_mask = ICANON | ECHO | ECHOE | ISIG;
    if (!diag)
        return UINT32_MAX;
    if (policy == LMUX_TTY_POLICY_STRICT_UART) {
        if (diag->actual_ispeed != diag->expected_ispeed)
            mismatch |= LMUX_TTY_MISMATCH_ISPEED;
        if (diag->actual_ospeed != diag->expected_ospeed)
            mismatch |= LMUX_TTY_MISMATCH_OSPEED;
    }
    if (!(diag->actual_cflag & CLOCAL))
        mismatch |= LMUX_TTY_MISMATCH_CLOCAL;
    if (!(diag->actual_cflag & CREAD))
        mismatch |= LMUX_TTY_MISMATCH_CREAD;
    if ((diag->actual_cflag & CSIZE) !=
        (diag->expected_cflag & CSIZE))
        mismatch |= LMUX_TTY_MISMATCH_CSIZE;
    if ((diag->actual_cflag & control_mask) !=
        (diag->expected_cflag & control_mask))
        mismatch |= LMUX_TTY_MISMATCH_CONTROL;
    if ((diag->actual_iflag & iflag_mask) !=
        (diag->expected_iflag & iflag_mask))
        mismatch |= LMUX_TTY_MISMATCH_IFLAG;
    if ((diag->actual_lflag & lflag_mask) !=
        (diag->expected_lflag & lflag_mask))
        mismatch |= LMUX_TTY_MISMATCH_LFLAG;
    if ((diag->actual_oflag & OPOST) !=
        (diag->expected_oflag & OPOST))
        mismatch |= LMUX_TTY_MISMATCH_OFLAG;
    return mismatch;
}

static int serial_configure_fd(int fd, const struct lmux_serial_config *cfg,
                               enum lmux_tty_policy policy,
                               struct lmux_tty_diagnostic *diag)
{
    struct termios tio, check;
    struct lmux_serial_config def;
    speed_t speed;
    uint32_t mismatch;
    if (diag)
        memset(diag, 0, sizeof(*diag));
    if (!cfg) {
        lmux_serial_config_default(&def);
        cfg = &def;
    }
    speed = baud_to_speed(cfg->baud);
    if (!speed) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_BAUD, 0);
        return -1;
    }
    if (cfg->data_bits < 5 || cfg->data_bits > 8 ||
        (cfg->stop_bits != 1 && cfg->stop_bits != 2)) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_ARGUMENTS, 0);
        errno = EINVAL;
        return -1;
    }
    if (tcgetattr(fd, &tio) < 0) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_TCGETATTR_INITIAL, errno);
        return -1;
    }
    cfmakeraw(&tio);
    if (cfsetispeed(&tio, speed) < 0) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_CFSETISPEED, errno);
        return -1;
    }
    if (cfsetospeed(&tio, speed) < 0) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_CFSETOSPEED, errno);
        return -1;
    }
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
    switch (cfg->data_bits) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }
    if (cfg->parity == LMUX_SERIAL_PARITY_EVEN)
        tio.c_cflag |= PARENB;
    else if (cfg->parity == LMUX_SERIAL_PARITY_ODD)
        tio.c_cflag |= PARENB | PARODD;
    else if (cfg->parity != LMUX_SERIAL_PARITY_NONE) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_ARGUMENTS, 0);
        errno = EINVAL;
        return -1;
    }
    if (cfg->stop_bits == 2)
        tio.c_cflag |= CSTOPB;
    if (cfg->flow == LMUX_SERIAL_FLOW_RTSCTS)
        tio.c_cflag |= CRTSCTS;
    else
        tio.c_cflag &= ~CRTSCTS;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR);
    if (cfg->flow == LMUX_SERIAL_FLOW_XONXOFF)
        tio.c_iflag |= IXON | IXOFF;
    else if (cfg->flow != LMUX_SERIAL_FLOW_NONE &&
             cfg->flow != LMUX_SERIAL_FLOW_RTSCTS) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_ARGUMENTS, 0);
        errno = EINVAL;
        return -1;
    }
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_TCSETATTR, errno);
        return -1;
    }
    if (tcgetattr(fd, &check) < 0) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_TCGETATTR_VERIFY, errno);
        return -1;
    }
    diagnostic_termios(diag, &tio, &check);
    mismatch = lmux_tty_validation_mismatch(policy, diag);
    if (mismatch != 0) {
        diagnostic_failure(diag, LMUX_TTY_CONFIG_VALIDATE, 0);
        if (diag)
            diag->validation_mismatch = mismatch;
        errno = EIO;
        return -1;
    }
    return 0;
}

int lmux_serial_configure_fd(int fd, const struct lmux_serial_config *cfg)
{
    struct lmux_tty_diagnostic diag;
    return serial_configure_fd(fd, cfg, LMUX_TTY_POLICY_STRICT_UART, &diag);
}

int lmux_carrier_tty_open(struct lmux_carrier *c)
{
    int fd;
    memset(&c->tty_diagnostic, 0, sizeof(c->tty_diagnostic));
    if (!c->path) {
        diagnostic_failure(&c->tty_diagnostic, LMUX_TTY_CONFIG_ARGUMENTS, 0);
        errno = EINVAL;
        return -1;
    }
    fd = open(c->path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        diagnostic_failure(&c->tty_diagnostic, LMUX_TTY_CONFIG_OPEN, errno);
        return -1;
    }
    c->exclusive_acquired = 0;
    if (c->exclusive_requested) {
#ifdef TIOCEXCL
        if (ioctl(fd, TIOCEXCL) < 0) {
            int saved = errno;
            diagnostic_failure(&c->tty_diagnostic,
                               LMUX_TTY_CONFIG_EXCLUSIVE, saved);
            close(fd);
            errno = saved;
            return -1;
        }
        c->exclusive_acquired = 1;
#else
        diagnostic_failure(&c->tty_diagnostic,
                           LMUX_TTY_CONFIG_EXCLUSIVE, ENOTSUP);
        close(fd);
        errno = ENOTSUP;
        return -1;
#endif
    }
    if (serial_configure_fd(fd, &c->serial, c->tty_policy,
                            &c->tty_diagnostic) < 0) {
        int saved = errno;
        close(fd);
        c->exclusive_acquired = 0;
        errno = saved;
        return -1;
    }
    c->fd = fd;
    c->owns_fd = 1;
    return 0;
}
