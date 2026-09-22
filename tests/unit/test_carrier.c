#include "carrier.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fail(const char *msg)
{
    fprintf(stderr, "carrier test failed: %s\n", msg);
    return 1;
}

static int wait_connect_unix(const char *path)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

static int wait_carrier_accept(struct lmux_carrier *c)
{
    int i;
    for (i = 0; i < 50; i++) {
        int rc = lmux_carrier_accept(c);
        if (rc != 0)
            return rc;
        usleep(10000);
    }
    return 0;
}

static int test_fd_binary_and_loss(void)
{
    struct lmux_carrier c;
    struct lmux_carrier_io io;
    int sv[2];
    uint8_t in[] = { 0x00, 0x03, 0x14, 0xff, 'L', 'M' };
    uint8_t out[sizeof(in)];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return fail("socketpair fd");
    lmux_carrier_init(&c);
    if (lmux_carrier_set_fd(&c, sv[0]) < 0)
        return fail("set fd");
    if (write(sv[1], in, sizeof(in)) != (ssize_t)sizeof(in))
        return fail("peer write");
    io = lmux_carrier_read(&c, out, sizeof(out));
    if (io.status != LMUX_CARRIER_IO_PROGRESS || io.n != (ssize_t)sizeof(out) ||
        memcmp(in, out, sizeof(in)) != 0 || c.rx_bytes != sizeof(in))
        return fail("fd binary read");
    io = lmux_carrier_write(&c, in, sizeof(in));
    if (io.status != LMUX_CARRIER_IO_PROGRESS || io.n != (ssize_t)sizeof(in) ||
        c.tx_bytes != sizeof(in))
        return fail("fd binary write");
    memset(out, 0, sizeof(out));
    if (read(sv[1], out, sizeof(out)) != (ssize_t)sizeof(out) ||
        memcmp(in, out, sizeof(in)) != 0)
        return fail("peer read");
    close(sv[1]);
    io = lmux_carrier_read(&c, out, sizeof(out));
    if (io.status != LMUX_CARRIER_IO_LOST)
        return fail("fd EOF loss");
    lmux_carrier_cleanup(&c);
    return 0;
}

static int test_would_block_and_partial_write(void)
{
    struct lmux_carrier c;
    struct lmux_carrier_io io;
    int sv[2];
    uint8_t buf[65536];
    int saw_progress = 0, saw_would_block = 0, saw_partial = 0;
    memset(buf, 0xa5, sizeof(buf));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return fail("socketpair would-block");
    lmux_carrier_init(&c);
    if (lmux_carrier_set_fd(&c, sv[0]) < 0)
        return fail("set fd would-block");
    for (;;) {
        io = lmux_carrier_write(&c, buf, sizeof(buf));
        if (io.status == LMUX_CARRIER_IO_PROGRESS) {
            saw_progress = 1;
            if (io.n > 0 && io.n < (ssize_t)sizeof(buf))
                saw_partial = 1;
            continue;
        }
        if (io.status == LMUX_CARRIER_IO_WOULD_BLOCK) {
            saw_would_block = 1;
            break;
        }
        return fail("unexpected write status");
    }
    if (!saw_progress || !saw_would_block)
        return fail("write progress/backpressure");
    if (saw_partial && c.tx_bytes == 0)
        return fail("partial write accounting");
    lmux_carrier_cleanup(&c);
    close(sv[1]);
    return 0;
}

static int test_unix_connect_retry_and_reconnect(void)
{
    char dir[] = "/tmp/lmux-carrier-test-XXXXXX";
    char path[160];
    struct lmux_carrier listen_c, connect_c;
    int client = -1, client2 = -1;
    uint8_t b = 'x';
    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(path, sizeof(path), "%s/carrier.sock", dir);
    lmux_carrier_init(&listen_c);
    lmux_carrier_init(&connect_c);
    if (lmux_carrier_set_unix_connect(&connect_c, path) < 0)
        return fail("set unix connect");
    if (lmux_carrier_try_open(&connect_c, 1000) >= 0 ||
        connect_c.open_fail_count == 0)
        return fail("connect retry before listener");
    if (lmux_carrier_set_unix_listen(&listen_c, path) < 0 ||
        lmux_carrier_start(&listen_c) < 0) {
        fprintf(stderr, "unix listener start errno=%d path=%s\n", errno, path);
        return fail("unix listener start");
    }
    {
        int open_rc = lmux_carrier_try_open(&connect_c, 3000);
        int accept_rc = wait_carrier_accept(&listen_c);
        if (open_rc != 1 || accept_rc != 1) {
            fprintf(stderr, "unix open_rc=%d accept_rc=%d open_fail=%llu errno=%d listen_fd=%d conn_fd=%d\n",
                    open_rc, accept_rc, connect_c.open_fail_count, connect_c.last_errno,
                    lmux_carrier_listen_fd(&listen_c), lmux_carrier_fd(&connect_c));
        return fail("unix connect/listen open");
        }
    }
    if (lmux_carrier_fd(&listen_c) < 0 || lmux_carrier_fd(&connect_c) < 0)
        return fail("unix fds open");
    if (lmux_carrier_write(&connect_c, &b, 1).status != LMUX_CARRIER_IO_PROGRESS ||
        lmux_carrier_read(&listen_c, &b, 1).status != LMUX_CARRIER_IO_PROGRESS ||
        b != 'x')
        return fail("unix carrier data");
    client = wait_connect_unix(path);
    if (client < 0 || wait_carrier_accept(&listen_c) != 1)
        return fail("unix listener reconnect accept");
    close(client);
    lmux_carrier_mark_lost(&connect_c, 4000);
    if (lmux_carrier_try_open(&connect_c, 5000) != 1)
        return fail("unix reconnect open");
    client2 = lmux_carrier_fd(&connect_c);
    (void)client2;
    lmux_carrier_cleanup(&connect_c);
    lmux_carrier_cleanup(&listen_c);
    unlink(path);
    rmdir(dir);
    return 0;
}

int main(void)
{
    if (test_fd_binary_and_loss() ||
        test_would_block_and_partial_write() ||
        test_unix_connect_retry_and_reconnect())
        return 1;
    puts("carrier selftest ok");
    return 0;
}
