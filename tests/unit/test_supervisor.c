#include "supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *msg)
{
    fprintf(stderr, "supervisor test failed: %s\n", msg);
    return 1;
}

static int test_msg_codec(void)
{
    struct lmux_supervisor_msg msg, out;
    uint8_t buf[LMUX_SUP_MSG_LEN];
    static const uint8_t exact[] = {
        'L', 'M', 'S', '1',
        LMUX_SUP_LINK_UP, 0, 0, 0,
        0x88, 0x77, 0x66, 0x55,
        0x44, 0x33, 0x22, 0x11
    };
    msg.type = LMUX_SUP_LINK_UP;
    msg.link_epoch = 0x1122334455667788ULL;
    if (lmux_supervisor_msg_encode(buf, &msg) < 0 ||
        memcmp(buf, exact, sizeof(exact)) != 0)
        return fail("sideband exact encoding");
    if (lmux_supervisor_msg_decode(buf, &out) < 0 ||
        out.type != msg.type || out.link_epoch != msg.link_epoch)
        return fail("sideband decode");
    buf[0] = 'X';
    if (lmux_supervisor_msg_decode(buf, &out) == 0)
        return fail("sideband wrong magic");
    buf[0] = 'L';
    buf[5] = 1;
    if (lmux_supervisor_msg_decode(buf, &out) == 0)
        return fail("sideband nonzero flags");
    buf[5] = 0;
    buf[6] = 1;
    if (lmux_supervisor_msg_decode(buf, &out) == 0)
        return fail("sideband nonzero reserved");
    buf[6] = 0;
    buf[4] = 99;
    if (lmux_supervisor_msg_decode(buf, &out) == 0)
        return fail("sideband unknown type");
    return 0;
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int test_proxy_ring(void)
{
    int sv[2];
    struct lmux_proxy_ring r;
    char out[32];
    ssize_t n;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return fail("socketpair");
    set_nonblock(sv[0]);
    set_nonblock(sv[1]);
    lmux_proxy_ring_clear(&r);
    if (write(sv[0], "abcdef", 6) != 6)
        return fail("proxy seed write");
    n = lmux_proxy_read_into(sv[1], &r);
    if (n != 6 || lmux_proxy_ring_len(&r) != 6)
        return fail("proxy read into ring");
    n = lmux_proxy_write_from(sv[0], &r);
    if (n != 6 || lmux_proxy_ring_len(&r) != 0)
        return fail("proxy write from ring");
    if (read(sv[1], out, sizeof(out)) != 6 || memcmp(out, "abcdef", 6) != 0)
        return fail("proxy bytes exact");
    n = lmux_proxy_read_into(sv[1], &r);
    if (n != -2)
        return fail("proxy EAGAIN");
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int test_proxy_ring_wrap(void)
{
    int sv[2];
    struct lmux_proxy_ring r;
    char got[6];
    size_t cap = sizeof(r.buf);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return fail("wrap socketpair");
    memset(&r, 0, sizeof(r));
    r.r = cap - 3;
    r.w = 3;
    r.len = 6;
    r.buf[cap - 3] = 'A';
    r.buf[cap - 2] = 'B';
    r.buf[cap - 1] = 'C';
    r.buf[0] = 'D';
    r.buf[1] = 'E';
    r.buf[2] = 'F';
    if (lmux_proxy_write_from(sv[0], &r) != 3 || r.len != 3)
        return fail("wrap first linear write");
    if (lmux_proxy_write_from(sv[0], &r) != 3 || r.len != 0)
        return fail("wrap second linear write");
    if (read(sv[1], got, sizeof(got)) != 6 || memcmp(got, "ABCDEF", 6) != 0)
        return fail("wrap byte order");
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int test_spawn_fd_hygiene(void)
{
    int data[2], ctrl[2];
    pid_t pid;
    int status;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, data) < 0 ||
        socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctrl) < 0)
        return fail("spawn socketpairs");
    pid = lmux_supervisor_spawn_worker("/no/such/lmuxd", data[1], ctrl[1], 0);
    if (pid < 0)
        return fail("spawn worker");
    close(data[1]);
    close(ctrl[1]);
    if (waitpid(pid, &status, 0) != pid)
        return fail("wait worker");
    close(data[0]);
    close(ctrl[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 127)
        return fail("exec failure exit status");
    return 0;
}

int main(void)
{
    if (test_msg_codec() ||
        test_proxy_ring() ||
        test_proxy_ring_wrap() ||
        test_spawn_fd_hygiene())
        return 1;
    puts("supervisor selftest ok");
    return 0;
}
