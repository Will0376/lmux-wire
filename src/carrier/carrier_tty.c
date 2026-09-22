#include "carrier.h"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#ifndef CRTSCTS
#define CRTSCTS 020000000000
#endif

static int configure_tty(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
        return 0;
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR);
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
    (void)configure_tty(fd);
    c->fd = fd;
    c->owns_fd = 1;
    return 0;
}
