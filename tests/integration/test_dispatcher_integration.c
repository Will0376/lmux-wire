#include "dispatch_ipc.h"
#include "dirty.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int connect_deadline(const char *path, long long deadline)
{
    struct sockaddr_un addr;
    int fd;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
        return -1;
    strcpy(addr.sun_path, path);
    while (now_ms() < deadline) {
        fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        if (fd >= 0)
            close(fd);
        if (errno != ENOENT && errno != ECONNREFUSED)
            return -1;
        poll(NULL, 0, 10);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int send_msg(int fd, uint8_t type, const void *payload, size_t len)
{
    uint8_t packet[LMUX_DISPATCH_IPC_MESSAGE_MAX];
    size_t packet_len;
    if (lmux_dispatch_ipc_encode(packet, sizeof(packet), type, payload, len,
                                 &packet_len) < 0)
        return -1;
    return send(fd, packet, packet_len, 0) == (ssize_t)packet_len ? 0 : -1;
}

static ssize_t read_deadline(int fd, uint8_t *buf, size_t cap,
                             long long deadline)
{
    size_t off = 0;
    while (off < cap && now_ms() < deadline) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ms = (int)(deadline - now_ms());
        int rc = poll(&pfd, 1, ms > 0 ? ms : 0);
        if (rc <= 0)
            break;
        if (pfd.revents & POLLIN) {
            ssize_t n = read(fd, buf + off, cap - off);
            if (n > 0)
                off += (size_t)n;
            else if (n < 0 && errno != EAGAIN && errno != EINTR)
                return -1;
        }
    }
    return (ssize_t)off;
}

static int helper_main(void)
{
    static const char message[] = "CONSOLE_OK\n";
    struct termios tio;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &tio) < 0 ||
        !(tio.c_lflag & ICANON) || !(tio.c_oflag & ONLCR) ||
        tcgetsid(STDIN_FILENO) != getsid(0))
        return 3;
    return write(STDOUT_FILENO, message, sizeof(message) - 1) ==
        (ssize_t)(sizeof(message) - 1) ? 0 : 4;
}

int main(int argc, char **argv)
{
    char temp[] = "/tmp/lmux-dispatch-it-XXXXXX";
    char socket_path[PATH_MAX];
    char trace_path[PATH_MAX];
    char self[PATH_MAX];
    char *slave_name;
    struct termios tio;
    uint8_t tx_record[LMUX_DIRTY_RECORD_MAX];
    size_t tx_record_len = 0;
    struct lmux_dirty_record tx_desc;
    const uint8_t rx_record[] = "~LMUX-C1:QUJDRA==\r\n";
    uint8_t buf[512];
    struct lmux_dispatch_ipc_message msg;
    int master = -1, slave = -1, peer = -1, second = -1, status;
    pid_t child = -1;
    ssize_t n;
    const char *stage = "init";
    if (argc > 1 && strcmp(argv[1], "--helper") == 0)
        return helper_main();
    stage = "mkdtemp";
    if (!mkdtemp(temp) || !realpath(argv[0], self))
        goto fail;
    snprintf(socket_path, sizeof(socket_path), "%s/dispatcher.sock", temp);
    snprintf(trace_path, sizeof(trace_path), "%s/dispatcher.trace", temp);
    memset(&tx_desc, 0, sizeof(tx_desc));
    tx_desc.type = LMUX_DIRTY_ACK;
    tx_desc.session_id = UINT64_C(0x1234);
    tx_desc.packet_seq = 7;
    if (lmux_dirty_record_encode(&tx_desc, tx_record, sizeof(tx_record),
                                 &tx_record_len) < 0)
        goto fail;
    stage = "posix_openpt";
    master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
        !(slave_name = ptsname(master)))
        goto fail;
    stage = "open-slave";
    slave = open(slave_name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0 || tcgetattr(slave, &tio) < 0)
        goto fail;
    tio.c_oflag |= OPOST | ONLCR;
    tio.c_lflag |= ICANON | ECHO | ISIG;
    if (tcsetattr(slave, TCSANOW, &tio) < 0)
        goto fail;
    close(slave);
    slave = -1;
    stage = "fork";
    child = fork();
    if (child < 0)
        goto fail;
    if (child == 0) {
        execl("build/host/lmux-uart-dispatch", "lmux-uart-dispatch",
              "--tty", slave_name, "--ipc-listen", socket_path,
              "--acquire-ctty",
              "--trace-file", trace_path,
              "--console-exec", self, "--helper", (char *)NULL);
        _exit(127);
    }
    stage = "connect";
    peer = connect_deadline(socket_path, now_ms() + 3000);
    if (peer < 0)
        goto fail;
    stage = "idle-disconnect";
    close(peer);
    peer = -1;
    poll(NULL, 0, 50);
    if (kill(child, 0) < 0)
        goto fail;
    peer = connect_deadline(socket_path, now_ms() + 3000);
    if (peer < 0)
        goto fail;
    stage = "second-peer-reject";
    second = connect_deadline(socket_path, now_ms() + 1000);
    if (second < 0)
        goto fail;
    {
        struct pollfd pfd = { second, POLLIN | POLLHUP, 0 };
        if (poll(&pfd, 1, 1000) <= 0 ||
            !(pfd.revents & (POLLIN | POLLHUP)))
            goto fail;
    }
    close(second);
    second = -1;
    stage = "send-tx";
    if (send_msg(peer, LMUX_DISPATCH_IPC_TX_RECORD,
                 tx_record, tx_record_len) < 0)
        goto fail;
    stage = "read-physical";
    n = read_deadline(master, buf, tx_record_len, now_ms() + 2000);
    if (n != (ssize_t)tx_record_len ||
        memcmp(buf, tx_record, tx_record_len) != 0) {
        fprintf(stderr, "dispatcher PTY ONLCR output mismatch n=%zd\n", n);
        goto fail;
    }
    stage = "write-physical";
    if (write(master, rx_record, sizeof(rx_record) - 1) !=
        (ssize_t)(sizeof(rx_record) - 1))
        goto fail;
    stage = "recv-ipc";
    n = recv(peer, buf, sizeof(buf), 0);
    if (n <= 0 || lmux_dispatch_ipc_decode(buf, (size_t)n, &msg) < 0 ||
        msg.type != LMUX_DISPATCH_IPC_RX_RECORD ||
        msg.payload_len != sizeof(rx_record) - 1 ||
        memcmp(msg.payload, rx_record, sizeof(rx_record) - 1) != 0)
        goto fail;
    stage = "send-active";
    if (send_msg(peer, LMUX_DISPATCH_IPC_ACTIVE, NULL, 0) < 0)
        goto fail;
    close(peer);
    peer = -1;
    stage = "read-console";
    n = read_deadline(master, buf, 12, now_ms() + 3000);
    if (n != 12 || memcmp(buf, "CONSOLE_OK\r\n", 12) != 0) {
        fprintf(stderr, "dispatcher console restoration mismatch n=%zd\n", n);
        goto fail;
    }
    stage = "wait-console";
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        goto fail;
    child = -1;
    stage = "trace-file";
    {
        char trace[4096];
        int fd = open(trace_path, O_RDONLY | O_CLOEXEC);
        ssize_t got = fd >= 0 ? read(fd, trace, sizeof(trace) - 1) : -1;
        if (fd >= 0)
            close(fd);
        if (got <= 0)
            goto fail;
        trace[got] = '\0';
        if (!strstr(trace, "kind=D1 type=2") ||
            !strstr(trace, "session=4660 seq=7 frag=0/0") ||
            !strstr(trace, "tx_complete id=1"))
            goto fail;
    }
    close(master);
    unlink(socket_path);
    unlink(trace_path);
    rmdir(temp);
    printf("dispatcher process integration passed\n");
    return 0;
fail:
    fprintf(stderr, "dispatcher process integration stage=%s: %s\n",
            stage, strerror(errno));
    if (peer >= 0)
        close(peer);
    if (second >= 0)
        close(second);
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (slave >= 0)
        close(slave);
    if (master >= 0)
        close(master);
    unlink(socket_path);
    unlink(trace_path);
    rmdir(temp);
    return 1;
}
