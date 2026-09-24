#include "carrier.h"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
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
    default:
        errno = EINVAL;
        return (speed_t)0;
    }
}

int lmux_serial_configure_fd(int fd, const struct lmux_serial_config *cfg)
{
    struct termios tio;
    struct lmux_serial_config def;
    speed_t speed;
    if (!cfg) {
        lmux_serial_config_default(&def);
        cfg = &def;
    }
    speed = baud_to_speed(cfg->baud);
    if (!speed)
        return -1;
    if (cfg->data_bits < 5 || cfg->data_bits > 8 ||
        (cfg->stop_bits != 1 && cfg->stop_bits != 2)) {
        errno = EINVAL;
        return -1;
    }
    if (tcgetattr(fd, &tio) < 0)
        return -1;
    cfmakeraw(&tio);
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
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
        errno = EINVAL;
        return -1;
    }
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    return tcsetattr(fd, TCSANOW, &tio);
}

int lmux_carrier_tty_open(struct lmux_carrier *c)
{
    int fd;
    if (!c->path) {
        errno = EINVAL;
        return -1;
    }
    fd = open(c->path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    (void)lmux_serial_configure_fd(fd, &c->serial);
    c->fd = fd;
    c->owns_fd = 1;
    return 0;
}
