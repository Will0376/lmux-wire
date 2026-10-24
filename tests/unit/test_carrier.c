#include "carrier.h"
#include "carrier_manager.h"
#include "dispatch_ipc.h"
#include "lmc1.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
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

static int test_dispatch_backend_records(void)
{
    struct lmux_carrier c;
    struct lmux_carrier_io io;
    struct lmux_dispatch_ipc_message msg;
    static const uint8_t record[] = "~LMUX-D1:AA==\r\n";
    uint8_t payload[300], packet[512], out[300];
    size_t packet_len, off = 0;
    int sv[2];
    memset(payload, 'A', sizeof(payload));
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sv) < 0)
        return fail("dispatch socketpair");
    lmux_carrier_init(&c);
    c.kind = LMUX_CARRIER_DISPATCH_CONNECT;
    c.fd = sv[0];
    c.owns_fd = 1;
    if (lmux_dispatch_ipc_encode(packet, sizeof(packet),
                                 LMUX_DISPATCH_IPC_RX_RECORD,
                                 payload, sizeof(payload), &packet_len) < 0 ||
        send(sv[1], packet, packet_len, 0) != (ssize_t)packet_len)
        return fail("dispatch RX seed");
    while (off < sizeof(out)) {
        io = lmux_carrier_read(&c, out + off, 64);
        if (io.status != LMUX_CARRIER_IO_PROGRESS || io.n <= 0)
            return fail("dispatch partial RX");
        off += (size_t)io.n;
    }
    if (memcmp(out, payload, sizeof(out)) != 0 ||
        c.rx_bytes != sizeof(out))
        return fail("dispatch RX exact bytes");
    io = lmux_carrier_write(&c, record, sizeof(record) - 1);
    if (io.status != LMUX_CARRIER_IO_PROGRESS ||
        io.n != (ssize_t)(sizeof(record) - 1))
        return fail("dispatch TX write");
    {
        ssize_t got = recv(sv[1], packet, sizeof(packet), 0);
        if (got <= 0 || lmux_dispatch_ipc_decode(packet, (size_t)got,
                                                  &msg) < 0 ||
            msg.type != LMUX_DISPATCH_IPC_TX_RECORD ||
            msg.payload_len != sizeof(record) - 1 ||
            memcmp(msg.payload, record, sizeof(record) - 1) != 0)
            return fail("dispatch TX packet");
    }
    memset(packet, 0, sizeof(packet));
    if (send(sv[1], packet, sizeof(packet), 0) != (ssize_t)sizeof(packet))
        return fail("dispatch truncation seed");
    io = lmux_carrier_read(&c, out, sizeof(out));
    if (io.status != LMUX_CARRIER_IO_FATAL || io.err != EMSGSIZE)
        return fail("dispatch MSG_TRUNC handling");
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

static int test_tty_strict_configuration_and_exclusive(void)
{
    struct lmux_carrier c;
    struct termios tio;
    char *path;
    int master = -1, competing = -1;
    int rc = 1;

    memset(&tio, 0, sizeof(tio));
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
        return fail("open test pty");
    path = ptsname(master);
    if (!path)
        goto out;
    lmux_carrier_init(&c);
    if (lmux_carrier_set_tty_path(&c, path) < 0)
        goto out;
    lmux_carrier_set_tty_policy(&c, LMUX_TTY_POLICY_STRICT_UART);
    lmux_carrier_set_exclusive(&c, 1);
    if (lmux_carrier_try_open(&c, 1000) != 1 ||
        !c.exclusive_acquired || tcgetattr(c.fd, &tio) < 0 ||
        c.tty_diagnostic.stage != LMUX_TTY_CONFIG_NONE ||
        c.tty_diagnostic.syscall_errno != 0 ||
        (tio.c_lflag & (ICANON | ECHO | ISIG)) != 0 ||
        (tio.c_cflag & CSIZE) != CS8 || cfgetispeed(&tio) != B115200 ||
        cfgetospeed(&tio) != B115200) {
        fprintf(stderr,
                "tty strict open fd=%d acquired=%d errno=%d cflag=0x%lx lflag=0x%lx ispeed=%lu ospeed=%lu\n",
                c.fd, c.exclusive_acquired, c.last_errno,
                (unsigned long)tio.c_cflag, (unsigned long)tio.c_lflag,
                (unsigned long)cfgetispeed(&tio),
                (unsigned long)cfgetospeed(&tio));
        goto cleanup;
    }
    errno = 0;
    competing = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (competing >= 0 || errno != EBUSY) {
        fprintf(stderr, "tty competing fd=%d errno=%d\n", competing, errno);
        goto cleanup;
    }
    lmux_carrier_cleanup(&c);
    competing = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (competing < 0) {
        fprintf(stderr, "tty reopen after cleanup errno=%d\n", errno);
        goto out;
    }
    close(competing);
    competing = -1;

    lmux_carrier_init(&c);
    if (lmux_carrier_set_tty_path(&c, path) < 0)
        goto out;
    c.serial.baud = 12345;
    if (lmux_carrier_try_open(&c, 2000) >= 0 || c.fd >= 0 ||
        c.open_fail_count != 1 || c.last_errno != EINVAL ||
        c.tty_diagnostic.stage != LMUX_TTY_CONFIG_BAUD ||
        c.tty_diagnostic.syscall_errno != 0) {
        fprintf(stderr,
                "tty invalid baud fd=%d fails=%llu errno=%d stage=%s syscall_errno=%d\n",
                c.fd, c.open_fail_count, c.last_errno,
                lmux_tty_config_stage_name(c.tty_diagnostic.stage),
                c.tty_diagnostic.syscall_errno);
        goto cleanup;
    }
    lmux_carrier_cleanup(&c);

    lmux_carrier_init(&c);
    if (lmux_carrier_set_tty_path(&c, "/dev/null") < 0)
        goto out;
    if (lmux_carrier_try_open(&c, 3000) >= 0 || c.fd >= 0 ||
        c.open_fail_count != 1 || c.last_errno != ENOTTY ||
        c.tty_diagnostic.stage != LMUX_TTY_CONFIG_TCGETATTR_INITIAL ||
        c.tty_diagnostic.syscall_errno != ENOTTY) {
        fprintf(stderr,
                "tty syscall failure fd=%d fails=%llu errno=%d stage=%s syscall_errno=%d\n",
                c.fd, c.open_fail_count, c.last_errno,
                lmux_tty_config_stage_name(c.tty_diagnostic.stage),
                c.tty_diagnostic.syscall_errno);
        goto cleanup;
    }
    lmux_carrier_cleanup(&c);
    rc = 0;
    goto out;

cleanup:
    if (competing >= 0)
        close(competing);
    lmux_carrier_cleanup(&c);
out:
    if (master >= 0)
        close(master);
    return rc ? fail("strict tty configuration/exclusive") : 0;
}

static int test_tty_policy_speed_mismatch(void)
{
    struct lmux_tty_diagnostic diag;
    uint32_t mismatch;

    memset(&diag, 0, sizeof(diag));
    diag.expected_ispeed = B115200;
    diag.expected_ospeed = B115200;
    diag.actual_ispeed = B9600;
    diag.actual_ospeed = B9600;
    diag.expected_cflag = CLOCAL | CREAD | CS8;
    diag.actual_cflag = diag.expected_cflag;

    mismatch = lmux_tty_validation_mismatch(
        LMUX_TTY_POLICY_BYTE_STREAM, &diag);
    if (mismatch != 0)
        return fail("byte-stream rejected baud-only mismatch");

    mismatch = lmux_tty_validation_mismatch(
        LMUX_TTY_POLICY_STRICT_UART, &diag);
    if (mismatch !=
        (LMUX_TTY_MISMATCH_ISPEED | LMUX_TTY_MISMATCH_OSPEED))
        return fail("strict UART accepted baud mismatch");

    diag.actual_lflag |= ECHO;
    mismatch = lmux_tty_validation_mismatch(
        LMUX_TTY_POLICY_BYTE_STREAM, &diag);
    if (mismatch != LMUX_TTY_MISMATCH_LFLAG)
        return fail("byte-stream accepted non-raw lflag");
    diag.actual_lflag = diag.expected_lflag;
    diag.actual_iflag |= IXON;
    mismatch = lmux_tty_validation_mismatch(
        LMUX_TTY_POLICY_BYTE_STREAM, &diag);
    if (mismatch != LMUX_TTY_MISMATCH_IFLAG)
        return fail("byte-stream accepted transforming iflag");
    return 0;
}

static int test_carrier_manager_reopen_hold(void)
{
    struct lmux_carrier_manager m;
    struct lmux_carrier_endpoint *ep = NULL;
    char *path;
    int master = -1;
    int rc = 1;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
        !(path = ptsname(master)))
        return fail("manager hold PTY");
    lmux_carrier_manager_init(&m);
    if (lmux_carrier_manager_add(&m, "rescue", 1, &ep) < 0 || !ep ||
        lmux_carrier_set_tty_path(&ep->carrier, path) < 0)
        goto out;
    if (lmux_carrier_manager_try_open_except(&m, 1000, 1u) != 0 ||
        lmux_carrier_fd(&ep->carrier) >= 0 || ep->carrier.open_count != 0)
        goto out;
    if (lmux_carrier_manager_try_open(&m, 1000) != 1 ||
        lmux_carrier_fd(&ep->carrier) < 0 || ep->carrier.open_count != 1)
        goto out;
    rc = 0;
out:
    lmux_carrier_manager_cleanup(&m);
    close(master);
    return rc ? fail("manager reopen hold mask") : 0;
}

static int test_carrier_manager_dual_endpoints(void)
{
    struct lmux_carrier_manager m;
    struct lmux_carrier_endpoint *primary, *rescue;
    int a[2], b[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, a) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, b) < 0)
        return fail("manager socketpairs");
    lmux_carrier_manager_init(&m);
    if (lmux_carrier_manager_add(&m, "primary", 1, &primary) < 0 ||
        lmux_carrier_manager_add(&m, "rescue", 0, &rescue) < 0 ||
        lmux_carrier_set_fd(&primary->carrier, a[0]) < 0 ||
        lmux_carrier_set_fd(&rescue->carrier, b[0]) < 0 ||
        lmux_carrier_manager_active(&m) != &primary->carrier ||
        !lmux_carrier_manager_is_active(&m, primary) ||
        lmux_carrier_manager_is_active(&m, rescue) ||
        lmux_carrier_manager_find(&m, "primary") != primary ||
        lmux_carrier_manager_find(&m, "rescue") != rescue ||
        strcmp(primary->name, "primary") != 0 ||
        strcmp(rescue->name, "rescue") != 0 ||
        m.configured_count != 2 || m.generation != 1) {
        lmux_carrier_manager_cleanup(&m);
        close(a[1]);
        close(b[1]);
        return fail("manager endpoint authority");
    }
    if (lmux_carrier_manager_set_active(&m, "rescue",
                                        LMUX_CARRIER_MODE_RAW, 2) < 0 ||
        lmux_carrier_manager_active(&m) != &rescue->carrier ||
        lmux_carrier_manager_is_active(&m, primary) ||
        !lmux_carrier_manager_is_active(&m, rescue) || m.generation != 2 ||
        lmux_carrier_manager_set_active(&m, "missing",
                                        LMUX_CARRIER_MODE_RAW, 3) == 0 ||
        m.generation != 2) {
        lmux_carrier_manager_cleanup(&m);
        close(a[1]);
        close(b[1]);
        return fail("manager authority switch");
    }
    lmux_carrier_manager_cleanup(&m);
    close(a[1]);
    close(b[1]);
    return 0;
}

static int test_carrier_manager_names(void)
{
    struct lmux_carrier_manager m;
    struct lmux_carrier_endpoint *primary, *rescue;
    char long_name[LMUX_CARRIER_NAME_MAX + 2];
    const char *value = NULL;
    char parsed[LMUX_CARRIER_NAME_MAX + 1];

    memset(long_name, 'a', sizeof(long_name));
    long_name[sizeof(long_name) - 1] = '\0';
    lmux_carrier_manager_init(&m);
    if (lmux_carrier_manager_add(&m, "primary", 1, &primary) < 0 ||
        lmux_carrier_manager_add(&m, "rescue", 0, &rescue) < 0)
        goto bad;
    errno = 0;
    if (lmux_carrier_manager_add(&m, "rescue", 0, NULL) == 0 ||
        errno != EEXIST)
        goto bad;
    errno = 0;
    if (lmux_carrier_manager_add(&m, "overflow", 0, NULL) == 0 ||
        errno != ENOSPC)
        goto bad;
    errno = 0;
    if (lmux_carrier_manager_add(&m, "", 0, NULL) == 0 ||
        errno != EINVAL)
        goto bad;
    errno = 0;
    if (lmux_carrier_manager_find(&m, "missing") != NULL || errno != ENOENT)
        goto bad;
    if (lmux_carrier_name_valid("") ||
        lmux_carrier_name_valid("-bad") ||
        lmux_carrier_name_valid("bad/name") ||
        lmux_carrier_name_valid("bad name") ||
        lmux_carrier_name_valid(long_name))
        goto bad;
    if (!lmux_carrier_name_valid("rack-1.primary_2") ||
        lmux_carrier_named_value_parse("rescue=/dev/ttyS0", parsed,
                                       &value) < 0 ||
        strcmp(parsed, "rescue") != 0 || strcmp(value, "/dev/ttyS0") != 0 ||
        lmux_carrier_named_value_parse("=/dev/ttyS0", parsed, &value) == 0 ||
        lmux_carrier_named_value_parse("rescue=", parsed, &value) == 0)
        goto bad;
    if (lmux_carrier_manager_rename(&m, primary, "main") < 0 ||
        lmux_carrier_manager_find(&m, "main") != primary ||
        lmux_carrier_manager_rename(&m, primary, "rescue") == 0)
        goto bad;
    lmux_carrier_manager_cleanup(&m);
    return 0;
bad:
    lmux_carrier_manager_cleanup(&m);
    return fail("carrier manager names");
}

static int test_matching_names_different_paths(void)
{
    struct lmux_carrier_manager host, target;
    struct lmux_carrier_endpoint *hp, *hr, *tp, *tr;
    int rc = 1;
    lmux_carrier_manager_init(&host);
    lmux_carrier_manager_init(&target);
    if (lmux_carrier_manager_add(&host, "primary", 1, &hp) < 0 ||
        lmux_carrier_manager_add(&host, "rescue", 0, &hr) < 0 ||
        lmux_carrier_manager_add(&target, "primary", 1, &tp) < 0 ||
        lmux_carrier_manager_add(&target, "rescue", 0, &tr) < 0 ||
        lmux_carrier_set_tty_path(&hp->carrier, "/dev/ttyACM0") < 0 ||
        lmux_carrier_set_tty_path(&hr->carrier, "/dev/ttyUSB0") < 0 ||
        lmux_carrier_set_tty_path(&tp->carrier, "/dev/ttyGS0") < 0 ||
        lmux_carrier_set_tty_path(&tr->carrier, "/dev/ttyS0") < 0 ||
        strcmp(hp->name, tp->name) != 0 || strcmp(hr->name, tr->name) != 0 ||
        strcmp(hp->carrier.path, tp->carrier.path) == 0 ||
        strcmp(hr->carrier.path, tr->carrier.path) == 0)
        goto out;
    rc = 0;
out:
    lmux_carrier_manager_cleanup(&host);
    lmux_carrier_manager_cleanup(&target);
    return rc ? fail("matching names different paths") : 0;
}

struct lmc_capture {
    int control_count;
    int outer_count;
    struct lmux_lmc1_msg msg;
    uint8_t outer[64];
    size_t outer_len;
};

static void capture_control(void *opaque, const struct lmux_lmc1_msg *msg)
{
    struct lmc_capture *c = opaque;
    c->control_count++;
    c->msg = *msg;
}

static void capture_outer(void *opaque, const uint8_t *encoded, size_t len)
{
    struct lmc_capture *c = opaque;
    c->outer_count++;
    if (len <= sizeof(c->outer)) {
        memcpy(c->outer, encoded, len);
        c->outer_len = len;
    }
}

static int test_lmc1_codec_and_raw_demux(void)
{
    struct lmux_lmc1_msg in, out;
    struct lmux_lmc1_raw_io io;
    struct lmc_capture capture;
    uint8_t encoded[LMUX_LMC1_MAX_ENCODED];
    uint8_t foreign[] = { 5, 'L', 'M', 'O', '1', 0 };
    size_t encoded_len;
    int i;
    memset(&in, 0, sizeof(in));
    in.type = LMUX_LMC1_PREPARE;
    in.mode = LMUX_CARRIER_MODE_RAW;
    in.generation = 17;
    in.deadline_ms = 5000;
    strcpy(in.name, "rescue");
    for (i = 0; i < LMUX_LMC1_NONCE_LEN; i++) {
        in.nonce[i] = (uint8_t)i;
        in.cookie[i] = (uint8_t)(i + 16);
        in.boot_id[i] = (uint8_t)(i + 32);
    }
    if (lmux_lmc1_encode(&in, encoded, sizeof(encoded), &encoded_len) < 0 ||
        encoded_len == 0 || encoded[encoded_len - 1] != 0 ||
        lmux_lmc1_decode(encoded, encoded_len - 1, &out) != 0 ||
        out.type != in.type || out.mode != in.mode ||
        out.generation != in.generation || out.deadline_ms != in.deadline_ms ||
        strcmp(out.name, in.name) != 0 ||
        memcmp(out.nonce, in.nonce, sizeof(in.nonce)) != 0 ||
        memcmp(out.cookie, in.cookie, sizeof(in.cookie)) != 0 ||
        memcmp(out.boot_id, in.boot_id, sizeof(in.boot_id)) != 0)
        return fail("LMC1 codec roundtrip");
    encoded[encoded_len / 2] ^= 0x20;
    if (lmux_lmc1_decode(encoded, encoded_len - 1, &out) == 0)
        return fail("LMC1 CRC corruption accepted");
    encoded[encoded_len / 2] ^= 0x20;
    in.status = LMUX_LMC1_STATUS_UNAVAILABLE + 1;
    if (lmux_lmc1_encode(&in, encoded, sizeof(encoded), &encoded_len) == 0)
        return fail("LMC1 invalid status encoded");
    in.status = LMUX_LMC1_STATUS_OK;
    if (lmux_lmc1_encode(&in, encoded, sizeof(encoded), &encoded_len) < 0)
        return fail("LMC1 re-encode after invalid status");

    memset(&capture, 0, sizeof(capture));
    lmux_lmc1_raw_init(&io);
    if (lmux_lmc1_raw_feed(&io, encoded, 3, 1, capture_control,
                           capture_outer, &capture) < 0 ||
        lmux_lmc1_raw_feed(&io, encoded + 3, encoded_len - 3, 1,
                           capture_control, capture_outer, &capture) < 0 ||
        capture.control_count != 1 || capture.outer_count != 0 ||
        strcmp(capture.msg.name, "rescue") != 0)
        return fail("LMC1 split raw demux");
    if (lmux_lmc1_raw_feed(&io, foreign, sizeof(foreign), 1,
                           capture_control, capture_outer, &capture) < 0 ||
        capture.outer_count != 1 || capture.outer_len != sizeof(foreign) - 1 ||
        memcmp(capture.outer, foreign, sizeof(foreign) - 1) != 0)
        return fail("OUTER record raw demux");
    if (lmux_lmc1_raw_feed(&io, foreign, sizeof(foreign), 0,
                           capture_control, capture_outer, &capture) < 0 ||
        capture.outer_count != 1)
        return fail("standby foreign record delivered");

    /* A control record may only begin at a physical record boundary.  If it
     * follows an unfinished OUTER record, the combined record is foreign or
     * malformed and must never reach the carrier-control state machine. */
    memset(&capture, 0, sizeof(capture));
    lmux_lmc1_raw_init(&io);
    if (lmux_lmc1_raw_feed(&io, foreign, sizeof(foreign) - 1, 1,
                           capture_control, capture_outer, &capture) < 0 ||
        lmux_lmc1_raw_feed(&io, encoded, encoded_len, 1,
                           capture_control, capture_outer, &capture) < 0 ||
        capture.control_count != 0)
        return fail("LMC1 accepted inside partial OUTER record");
    return 0;
}

static int test_lmc1_dirty_bootstrap(void)
{
    struct lmux_lmc1_msg in;
    struct lmux_lmc1_dirty_io io;
    struct lmc_capture capture;
    uint64_t host1, target1, host2, target2;
    size_t split;
    int i;
    memset(&in, 0, sizeof(in));
    in.type = LMUX_LMC1_PREPARE;
    in.mode = LMUX_CARRIER_MODE_DIRTY;
    in.generation = 23;
    in.deadline_ms = 5000;
    strcpy(in.name, "rescue");
    for (i = 0; i < LMUX_LMC1_NONCE_LEN; i++) {
        in.nonce[i] = (uint8_t)(0x10 + i);
        in.cookie[i] = (uint8_t)(0x30 + i);
        in.boot_id[i] = (uint8_t)(0x50 + i);
    }
    lmux_lmc1_dirty_init(&io);
    if (lmux_lmc1_dirty_queue(&io, &in) < 0 ||
        io.tx_len > LMUX_LMC1_DIRTY_RECORD_MAX ||
        memcmp(io.tx, LMUX_LMC1_DIRTY_PREFIX,
               LMUX_LMC1_DIRTY_PREFIX_LEN) != 0)
        return fail("LMC1 dirty bootstrap encode");
    memset(&capture, 0, sizeof(capture));
    split = io.tx_len / 2;
    if (lmux_lmc1_dirty_feed(&io, (const uint8_t *)"console noise\r\n", 15,
                             capture_control, &capture) < 0 ||
        lmux_lmc1_dirty_feed(&io, io.tx, split,
                             capture_control, &capture) < 0 ||
        lmux_lmc1_dirty_feed(&io, io.tx + split, io.tx_len - split,
                             capture_control, &capture) < 0 ||
        capture.control_count != 1 ||
        capture.msg.mode != LMUX_CARRIER_MODE_DIRTY ||
        strcmp(capture.msg.name, "rescue") != 0)
        return fail("LMC1 dirty bootstrap split/noise");
    io.tx[LMUX_LMC1_DIRTY_PREFIX_LEN + 3] ^= 1;
    if (lmux_lmc1_dirty_feed(&io, io.tx, io.tx_len,
                             capture_control, &capture) < 0 ||
        capture.control_count != 1 || io.invalid_control_records == 0)
        return fail("LMC1 dirty bootstrap corruption accepted");
    lmux_lmc1_dirty_session_ids(in.boot_id, 24, in.name, &host1, &target1);
    lmux_lmc1_dirty_session_ids(in.boot_id, 24, in.name, &host2, &target2);
    if (!host1 || !target1 || host1 == target1 ||
        host1 != host2 || target1 != target2)
        return fail("LMC1 dirty session derivation");
    return 0;
}

int main(void)
{
    if (test_fd_binary_and_loss() ||
        test_would_block_and_partial_write() ||
        test_dispatch_backend_records() ||
        test_unix_connect_retry_and_reconnect() ||
        test_tty_strict_configuration_and_exclusive() ||
        test_tty_policy_speed_mismatch() ||
        test_carrier_manager_reopen_hold() ||
        test_carrier_manager_dual_endpoints() ||
        test_carrier_manager_names() ||
        test_matching_names_different_paths() ||
        test_lmc1_codec_and_raw_demux() ||
        test_lmc1_dirty_bootstrap())
        return 1;
    puts("carrier selftest ok");
    return 0;
}
