#include "lmux2_core.h"
#include "lmux.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *msg)
{
    fprintf(stderr, "lmux2 integration failed: %s\n", msg);
    return 1;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd_event(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    return poll(&pfd, 1, timeout_ms) > 0 ? pfd.revents : 0;
}

static int remaining_timeout(long long end)
{
    long long left = end - now_ms();
    if (left <= 0)
        return 0;
    return left > 30000 ? 30000 : (int)left;
}

static int connect_unix_test(const char *path)
{
    int fd, err = 0;
    socklen_t errlen = sizeof(err);
    struct sockaddr_un sa;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (errno != EINPROGRESS ||
            !(wait_fd_event(fd, POLLOUT | POLLERR | POLLHUP | POLLNVAL, 1000) & POLLOUT) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static int wait_connect(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int fd = connect_unix_test(path);
        if (fd >= 0)
            return fd;
        usleep(20000);
    }
    return -1;
}

static int fd_has_hup(int fd, int timeout_ms)
{
    int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, timeout_ms);
    return (re & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

static int write_all_timeout(int fd, const uint8_t *buf, size_t len, int timeout_ms)
{
    size_t off = 0;
    long long end = now_ms() + timeout_ms;
    while (off < len && now_ms() < end) {
        int re = wait_fd_event(fd, POLLOUT | POLLHUP | POLLERR | POLLNVAL,
                               remaining_timeout(end));
        ssize_t w;
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
        if (!(re & POLLOUT))
            continue;
        w = write(fd, buf + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
        return -1;
    }
    return off == len ? 0 : -1;
}

static int read_exact_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t off = 0;
    long long end = now_ms() + timeout_ms;
    while (off < len && now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL,
                               remaining_timeout(end));
        ssize_t r;
        if (!(re & POLLIN)) {
            if (re & (POLLHUP | POLLERR | POLLNVAL))
                return -1;
            continue;
        }
        r = read(fd, buf + off, len - off);
        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        if (r == 0)
            return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
        return -1;
    }
    return off == len ? 0 : -1;
}

static int exchange_pair(int left, int right, const uint8_t *a, size_t alen,
                         const uint8_t *b, size_t blen)
{
    uint8_t got[128];
    if (alen > sizeof(got) || blen > sizeof(got))
        return -1;
    if (write_all_timeout(left, a, alen, 3000) < 0 ||
        read_exact_timeout(right, got, alen, 3000) < 0 ||
        memcmp(got, a, alen) != 0)
        return -1;
    if (write_all_timeout(right, b, blen, 3000) < 0 ||
        read_exact_timeout(left, got, blen, 3000) < 0 ||
        memcmp(got, b, blen) != 0)
        return -1;
    return 0;
}

static int count_fds(pid_t pid)
{
    char path[128];
    DIR *dir;
    struct dirent *de;
    int n = 0;
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    dir = opendir(path);
    if (!dir)
        return -1;
    while ((de = readdir(dir)) != NULL)
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            n++;
    closedir(dir);
    return n;
}

static void stop_child(pid_t pid)
{
    long long end;
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    end = now_ms() + 1000;
    while (now_ms() < end) {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
}

static pid_t spawn_lmuxd(const char *role, int fd, const char *unix_arg1,
                         const char *unix_arg2, const char *unix_arg3,
                         const char *unix_arg4)
{
    pid_t pid = fork();
    if (pid == 0) {
        char fdarg[32];
        char *argv[18];
        int n = 0, cfd;
        for (cfd = 3; cfd < 256; cfd++)
            if (cfd != fd)
                close(cfd);
        snprintf(fdarg, sizeof(fdarg), "%d", fd);
        argv[n++] = (char *)"lmuxd";
        argv[n++] = (char *)"--role";
        argv[n++] = (char *)role;
        argv[n++] = (char *)"--carrier-fd";
        argv[n++] = fdarg;
#define ADD_UNIX_ARG(arg) do { if (arg) { argv[n++] = (char *)"--unix"; argv[n++] = (char *)(arg); } } while (0)
        ADD_UNIX_ARG(unix_arg1);
        ADD_UNIX_ARG(unix_arg2);
        ADD_UNIX_ARG(unix_arg3);
        ADD_UNIX_ARG(unix_arg4);
#undef ADD_UNIX_ARG
        if (getenv("LMUX2_IT_VERBOSE"))
            argv[n++] = (char *)"--verbose";
        argv[n] = NULL;
        execv("build/host/lmuxd", argv);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host_connect(const char *carrier, const char *control,
                                const char *unix_arg)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--unix", unix_arg, "--verbose", (char *)NULL);
        else
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--unix", unix_arg, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_target_listen(const char *carrier)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "target",
                  "--carrier-listen", carrier, "--verbose", (char *)NULL);
        else
            execl("build/host/lmuxd", "lmuxd", "--role", "target",
                  "--carrier-listen", carrier, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int read_status_contains(const char *control, const char *want, int timeout_ms)
{
    char req[] = "STATUS\n";
    char buf[4096];
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    int fd = wait_connect(control, timeout_ms);
    if (fd < 0)
        return -1;
    if (write_all_timeout(fd, (const uint8_t *)req, strlen(req), timeout_ms) < 0) {
        close(fd);
        return -1;
    }
    buf[0] = 0;
    while (now_ms() < end && len + 1 < sizeof(buf)) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL,
                               remaining_timeout(end));
        ssize_t r;
        if (re & POLLIN) {
            r = read(fd, buf + len, sizeof(buf) - len - 1);
            if (r > 0) {
                len += (size_t)r;
                buf[len] = 0;
                if (strstr(buf, want)) {
                    close(fd);
                    return 0;
                }
                continue;
            }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            break;
    }
    close(fd);
    return -1;
}

static int do_host_first(const char *host_sock, const char *target_sock,
                         const uint8_t *early, size_t early_len)
{
    uint8_t got[128];
    int h = connect_unix_test(host_sock);
    int t;
    if (h < 0 || fd_has_hup(h, 200)) {
        fprintf(stderr, "host-first: host connect/hup failed fd=%d\n", h);
        return -1;
    }
    if (early_len && write_all_timeout(h, early, early_len, 1000) < 0) {
        fprintf(stderr, "host-first: early write failed\n");
        return -1;
    }
    t = connect_unix_test(target_sock);
    if (t < 0) {
        fprintf(stderr, "host-first: target connect failed\n");
        return -1;
    }
    if (early_len &&
        (read_exact_timeout(t, got, early_len, 3000) < 0 ||
         memcmp(got, early, early_len) != 0)) {
        fprintf(stderr, "host-first: early read/mismatch failed\n");
        return -1;
    }
    if (exchange_pair(h, t, (const uint8_t *)"H", 1, (const uint8_t *)"T", 1) < 0) {
        fprintf(stderr, "host-first: exchange failed\n");
        return -1;
    }
    close(h);
    close(t);
    return 0;
}

static int do_target_first(const char *host_sock, const char *target_sock,
                           const uint8_t *early, size_t early_len)
{
    uint8_t got[128];
    int t = connect_unix_test(target_sock);
    int h;
    if (t < 0 || fd_has_hup(t, 200))
        return -1;
    if (early_len && write_all_timeout(t, early, early_len, 1000) < 0)
        return -1;
    h = connect_unix_test(host_sock);
    if (h < 0)
        return -1;
    if (early_len &&
        (read_exact_timeout(h, got, early_len, 3000) < 0 ||
         memcmp(got, early, early_len) != 0))
        return -1;
    if (exchange_pair(h, t, (const uint8_t *)"h", 1, (const uint8_t *)"t", 1) < 0)
        return -1;
    close(h);
    close(t);
    return 0;
}

static int encode_and_write_frame(int fd, const struct lmux2_frame *fr)
{
    uint8_t enc[LMUX2_MAX_ENCODED];
    size_t enc_len = 0;
    if (lmux2_encode_frame(fr, enc, sizeof(enc), &enc_len) < 0)
        return -1;
    return write_all_timeout(fd, enc, enc_len, 1000);
}

static int inject_stale_data(int host_carrier_dup, uint64_t stream_id)
{
    struct lmux2_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_DATA;
    fr.stream_id = stream_id;
    fr.value = 0;
    fr.payload_len = 3;
    memcpy(fr.payload, "old", 3);
    return encode_and_write_frame(host_carrier_dup, &fr);
}

static int inject_stale_max_stream_data(int host_carrier_dup, uint64_t stream_id)
{
    struct lmux2_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_MAX_STREAM_DATA;
    fr.stream_id = stream_id;
    fr.value = 9999;
    return encode_and_write_frame(host_carrier_dup, &fr);
}

static int inject_peer_restart_hello(int host_carrier_dup, uint64_t conn_id)
{
    struct lmux2_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_HELLO;
    if (lmux2_make_hello_payload(LMUX2_ROLE_TARGET, conn_id,
                                 LMUX2_CORE_MAX_STREAMS,
                                 LMUX2_STREAM_RX_CAP,
                                 LMUX2_CONN_RX_CAP,
                                 fr.payload, &fr.payload_len) < 0)
        return -1;
    return encode_and_write_frame(host_carrier_dup, &fr);
}

static int inject_service_delete(int host_carrier_dup, uint64_t service_id)
{
    struct lmux2_frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_CONTROL;
    fr.stream_id = 0;
    fr.payload[0] = 1; /* control version */
    fr.payload[1] = 5; /* SERVICE_DELETE */
    lmux_put_le16(fr.payload + 2, 0);
    lmux_put_le32(fr.payload + 4, 0x70000001u);
    lmux2_put_le64(fr.payload + 8, service_id);
    lmux2_put_le64(fr.payload + 16, 0);
    fr.payload_len = 24;
    return encode_and_write_frame(host_carrier_dup, &fr);
}

static int wait_socket_ready(const char *path)
{
    int fd = wait_connect(path, 8000);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static int wait_socket_absent(const char *path)
{
    long long end = now_ms() + 3000;
    while (now_ms() < end) {
        struct stat st;
        if (lstat(path, &st) < 0 && errno == ENOENT)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int wait_path_exists(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        struct stat st;
        if (lstat(path, &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int abandon_once(const char *path)
{
    int fd = connect_unix_test(path);
    if (fd < 0 || fd_has_hup(fd, 200)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);
    usleep(100000);
    return 0;
}

static int integration_test(void)
{
    char dir[] = "/tmp/lmux2-it-XXXXXX";
    char host_sock[160], target_sock[160], host_aux[160], target_aux[160];
    char host_conflict[160], target_conflict[160];
    char unix_arg[512], unix_dup[512], unix_conflict[512], unix_aux[512];
    int carrier[2] = { -1, -1 };
    int inject_fd = -1;
    int inject_to_host_fd = -1;
    pid_t host = -1, target = -1;
    int rc = 1, i, host_fd_base = -1, target_fd_base = -1;
    static const uint8_t early_h[] = { 'H', 0x00, 0x03, 0xff, 'x' };
    static const uint8_t early_t[] = { 'T', 0x00, 0x03, 0x14, 'y' };
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(host_sock, sizeof(host_sock), "%s/gdb-host.sock", dir);
    snprintf(target_sock, sizeof(target_sock), "%s/gdb-target.sock", dir);
    snprintf(host_aux, sizeof(host_aux), "%s/aux-host.sock", dir);
    snprintf(target_aux, sizeof(target_aux), "%s/aux-target.sock", dir);
    snprintf(host_conflict, sizeof(host_conflict), "%s/gdb-conflict-host.sock", dir);
    snprintf(target_conflict, sizeof(target_conflict), "%s/gdb-conflict-target.sock", dir);
    snprintf(unix_arg, sizeof(unix_arg), "gdb:%s:%s", host_sock, target_sock);
    snprintf(unix_dup, sizeof(unix_dup), "gdb:%s:%s", host_sock, target_sock);
    snprintf(unix_conflict, sizeof(unix_conflict), "gdb:%s:%s", host_conflict, target_conflict);
    snprintf(unix_aux, sizeof(unix_aux), "aux:%s:%s", host_aux, target_aux);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, carrier) < 0)
        goto out;
    inject_fd = dup(carrier[0]);
    if (inject_fd < 0)
        goto out;
    inject_to_host_fd = dup(carrier[1]);
    if (inject_to_host_fd < 0)
        goto out;
    host = spawn_lmuxd("host", carrier[0], unix_arg, unix_dup, unix_conflict, unix_aux);
    target = spawn_lmuxd("target", carrier[1], NULL, NULL, NULL, NULL);
    close(carrier[0]); close(carrier[1]);
    carrier[0] = carrier[1] = -1;
    if (host < 0 || target < 0)
        goto out;

    where = "service ready";
    if (wait_socket_ready(host_sock) < 0 || wait_socket_ready(target_sock) < 0 ||
        wait_socket_ready(host_aux) < 0 || wait_socket_ready(target_aux) < 0 ||
        wait_socket_absent(target_conflict) < 0)
        goto out;
    host_fd_base = count_fds(host);
    target_fd_base = count_fds(target);

    where = "host-first early data";
    if (do_host_first(host_sock, target_sock, early_h, sizeof(early_h)) < 0)
        goto out;
    where = "target-first early data";
    if (do_target_first(host_sock, target_sock, early_t, sizeof(early_t)) < 0)
        goto out;
    where = "unrelated aux service initial";
    if (do_host_first(host_aux, target_aux, early_h, sizeof(early_h)) < 0)
        goto out;

    where = "abandoned host pending";
    for (i = 0; i < 10; i++)
        if (abandon_once(host_sock) < 0)
            goto out;
    where = "next after abandoned host pending";
    if (do_host_first(host_sock, target_sock, early_h, sizeof(early_h)) < 0)
        goto out;

    where = "abandoned target pending";
    for (i = 0; i < 10; i++)
        if (abandon_once(target_sock) < 0)
            goto out;
    where = "next after abandoned target pending";
    if (do_target_first(host_sock, target_sock, early_t, sizeof(early_t)) < 0)
        goto out;

    where = "100 repeated sessions";
    for (i = 0; i < 100; i++) {
        if ((i & 1) == 0) {
            if (do_host_first(host_sock, target_sock, early_h, sizeof(early_h)) < 0) {
                fprintf(stderr, "repeated host-first iteration %d failed\n", i);
                goto out;
            }
        } else {
            if (do_target_first(host_sock, target_sock, early_t, sizeof(early_t)) < 0) {
                fprintf(stderr, "repeated target-first iteration %d failed\n", i);
                goto out;
            }
        }
    }

    where = "stale old stream traffic";
    if (inject_stale_data(inject_fd, 1) < 0 ||
        inject_stale_max_stream_data(inject_fd, 1) < 0 ||
        do_host_first(host_sock, target_sock, early_h, sizeof(early_h)) < 0)
        goto out;

    where = "unrelated aux service survives churn";
    if (do_target_first(host_aux, target_aux, early_t, sizeof(early_t)) < 0)
        goto out;

    where = "peer restart HELLO on established carrier";
    if (inject_peer_restart_hello(inject_to_host_fd, 0xabcdef1234567890ULL) < 0 ||
        do_host_first(host_sock, target_sock, early_h, sizeof(early_h)) < 0)
        goto out;

    where = "fd leak check";
    usleep(200000);
    if (host_fd_base >= 0 && count_fds(host) > host_fd_base + 2)
        goto out;
    if (target_fd_base >= 0 && count_fds(target) > target_fd_base + 2)
        goto out;

    where = "service delete aux";
    stop_child(host);
    host = -1;
    if (inject_service_delete(inject_fd, 3) < 0 || wait_socket_absent(target_aux) < 0)
        goto out;

    where = "gdb survives aux delete";
    {
        int t = wait_connect(target_sock, 3000);
        if (t < 0)
            goto out;
        close(t);
        usleep(100000);
    }

    where = "carrier loss preserves target service listener";
    close(inject_fd);
    inject_fd = -1;
    {
        int t = wait_connect(target_sock, 3000);
        if (t < 0)
            goto out;
        close(t);
    }
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "where: %s\n", where);
    close(inject_fd);
    close(inject_to_host_fd);
    if (carrier[0] >= 0) close(carrier[0]);
    if (carrier[1] >= 0) close(carrier[1]);
    stop_child(host);
    stop_child(target);
    unlink(host_sock); unlink(target_sock); unlink(host_aux); unlink(target_aux);
    unlink(host_conflict); unlink(target_conflict);
    rmdir(dir);
    return rc ? fail(where) : 0;
}

static int delayed_peer_test(void)
{
    char dir[] = "/tmp/lmux2-delay-it-XXXXXX";
    char carrier[160], host_sock[160], target_sock[160], control[160], unix_arg[512];
    pid_t host = -1, target = -1;
    int h = -1, t = -1, rc = 1;
    static const uint8_t early1[] = { 'p', 'r', 'e', 0x00, 0x03, '1' };
    static const uint8_t early2[] = { 'r', 'e', 0x00, 'c', 0x03, '2' };
    uint8_t got[sizeof(early2)];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp delayed");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(host_sock, sizeof(host_sock), "%s/gdb-host.sock", dir);
    snprintf(target_sock, sizeof(target_sock), "%s/gdb-target.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(unix_arg, sizeof(unix_arg), "gdb:%s:%s", host_sock, target_sock);

    host = spawn_host_connect(carrier, control, unix_arg);
    if (host < 0)
        goto out;

    where = "host listeners before peer";
    if (wait_path_exists(control, 3000) < 0 ||
        wait_path_exists(host_sock, 3000) < 0)
        goto out;
    if (read_status_contains(control, "wire:   LMUX/2", 3000) < 0 ||
        read_status_contains(control, "carrier: down", 3000) < 0 ||
        read_status_contains(control, "gdb type=unix-rendezvous", 3000) < 0 ||
        read_status_contains(control, "configured=yes sent=no acknowledged=no", 3000) < 0)
        goto out;

    where = "pending host client before target";
    h = connect_unix_test(host_sock);
    if (h < 0 || fd_has_hup(h, 200) ||
        write_all_timeout(h, early1, sizeof(early1), 1000) < 0)
        goto out;

    where = "delayed target startup";
    target = spawn_target_listen(carrier);
    if (target < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    t = wait_connect(target_sock, 10000);
    if (t < 0 ||
        read_exact_timeout(t, got, sizeof(early1), 5000) < 0 ||
        memcmp(got, early1, sizeof(early1)) != 0)
        goto out;
    if (exchange_pair(h, t, (const uint8_t *)"A", 1, (const uint8_t *)"B", 1) < 0)
        goto out;
    close(h); h = -1;
    close(t); t = -1;
    if (read_status_contains(control, "factories: unix-rendezvous=yes", 3000) < 0)
        goto out;
    if (read_status_contains(control, "stale=0", 3000) < 0)
        goto out;

    where = "pending while carrier down";
    stop_child(target);
    target = -1;
    usleep(300000);
    if (wait_path_exists(host_sock, 1000) < 0)
        goto out;
    h = connect_unix_test(host_sock);
    if (h < 0 || fd_has_hup(h, 200) ||
        write_all_timeout(h, early2, sizeof(early2), 1000) < 0)
        goto out;

    where = "reconnect binds pending client";
    target = spawn_target_listen(carrier);
    if (target < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    t = wait_connect(target_sock, 10000);
    if (t < 0 ||
        read_exact_timeout(t, got, sizeof(early2), 5000) < 0 ||
        memcmp(got, early2, sizeof(early2)) != 0)
        goto out;
    if (exchange_pair(h, t, (const uint8_t *)"C", 1, (const uint8_t *)"D", 1) < 0)
        goto out;

    rc = 0;
out:
    if (rc)
        fprintf(stderr, "where delayed: %s\n", where);
    if (h >= 0) close(h);
    if (t >= 0) close(t);
    stop_child(host);
    stop_child(target);
    unlink(carrier); unlink(host_sock); unlink(target_sock); unlink(control);
    rmdir(dir);
    return rc ? fail(where) : 0;
}

int main(void)
{
    if (integration_test())
        return 1;
    if (delayed_peer_test())
        return 1;
    puts("lmux2 integration selftest ok");
    return 0;
}
