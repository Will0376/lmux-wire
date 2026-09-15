#define LMUXD_NO_MAIN
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "lmuxd.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <dirent.h>
#include <sys/select.h>

static int fail(const char *msg)
{
    fprintf(stderr, "lmuxd integration test failed: %s\n", msg);
    return 1;
}

static void init_daemon_app(struct app *a, int carrier_fd, const char *ch1,
                            const char *ch3, const char *control)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->carrier_fd = carrier_fd;
    (void)set_nonblock(a->carrier_fd);
    a->control_listen_fd = -1;
    a->control_fd = -1;
    a->control_path = control;
    a->verbose = 0;
    for (i = 0; i < LMUX_CHANNELS; i++) {
        a->ch[i].ep.fd = -1;
        a->ch[i].ep.listen_fd = -1;
        a->ch[i].file.fd = -1;
    }
    a->ch[0].ep.type = EP_SOCKET;
    strcpy(a->ch[0].ep.path, ch1);
    a->ch[2].ep.type = EP_SOCKET;
    strcpy(a->ch[2].ep.path, ch3);
    compute_local_features(a);
    make_session(a);
}

static void daemon_child(int carrier_fd, const char *ch1, const char *ch3,
                         const char *control)
{
    struct app a;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    init_daemon_app(&a, carrier_fd, ch1, ch3, control);
    global_app = &a;
    if (create_socket_listener(&a, 0) < 0 ||
        create_socket_listener(&a, 2) < 0 ||
        create_control_listener(&a) < 0) {
        cleanup(&a);
        _exit(2);
    }
    run_loop(&a);
    cleanup(&a);
    _exit(0);
}

static int wait_fd_event(int fd, short events, int timeout_ms);

static int connect_unix_test(const char *path)
{
    int fd;
    struct sockaddr_un sa;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int err = 0;
        socklen_t errlen = sizeof(err);
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        if (!(wait_fd_event(fd, POLLOUT | POLLERR | POLLHUP | POLLNVAL, 1000) & POLLOUT) ||
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
    int fd;
    while (now_ms() < end) {
        fd = connect_unix_test(path);
        if (fd >= 0)
            return fd;
        usleep(20000);
    }
    return -1;
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
    if (left > 30000)
        return 30000;
    return (int)left;
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
    uint8_t got[64];
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

static int read_status(const char *control, char *buf, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t off = 0;
    long long end = now_ms() + 1000;
    if (fd < 0)
        return -1;
    if (write_all_timeout(fd, (const uint8_t *)"STATUS\n", 7, 1000) < 0) {
        close(fd);
        return -1;
    }
    while (off + 1 < cap && now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL,
                               remaining_timeout(end));
        ssize_t r;
        if (!(re & POLLIN))
            break;
        r = read(fd, buf + off, cap - off - 1);
        if (r > 0) {
            off += (size_t)r;
            buf[off] = 0;
            if (strstr(buf, "ch4 "))
                break;
            continue;
        }
        break;
    }
    buf[off] = 0;
    close(fd);
    return off ? 0 : -1;
}

static unsigned long long status_value(const char *status, const char *key)
{
    const char *p = strstr(status, key);
    if (!p)
        return 0;
    p += strlen(key);
    return strtoull(p, NULL, 10);
}

static int status_has_channel_client(const char *status, int ch, const char *client)
{
    char prefix[32], needle[64];
    const char *p;
    snprintf(prefix, sizeof(prefix), "ch%d ", ch);
    p = strstr(status, prefix);
    if (!p)
        return 0;
    snprintf(needle, sizeof(needle), "client=%s", client);
    return strstr(p, needle) != NULL;
}

static int wait_channel_client(const char *control, int ch, const char *client,
                               unsigned long long resets, unsigned long long sessions,
                               char *status, size_t cap)
{
    long long end = now_ms() + 3000;
    while (now_ms() < end) {
        if (read_status(control, status, cap) == 0 &&
            status_value(status, "resets=") == resets &&
            status_value(status, "sessions=") == sessions &&
            status_has_channel_client(status, ch, client))
            return 0;
        usleep(20000);
    }
    fprintf(stderr, "wait_channel_client status:\n%s\n", status);
    return -1;
}

static int count_fds(pid_t pid)
{
    char path[64];
    DIR *d;
    struct dirent *de;
    int n = 0;
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    d = opendir(path);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            n++;
    }
    closedir(d);
    return n;
}

static int wait_both_established(const char *a_control, char *a_status, size_t a_cap,
                                 const char *b_control, char *b_status, size_t b_cap)
{
    long long end = now_ms() + 8000;
    int a_ok = 0, b_ok = 0;
    a_status[0] = 0;
    b_status[0] = 0;
    while (now_ms() < end) {
        if (!a_ok && read_status(a_control, a_status, a_cap) == 0 &&
            strstr(a_status, "link:   established"))
            a_ok = 1;
        if (!b_ok && read_status(b_control, b_status, b_cap) == 0 &&
            strstr(b_status, "link:   established"))
            b_ok = 1;
        if (a_ok && b_ok)
            return 0;
        usleep(20000);
    }
    fprintf(stderr, "A status:\n%s\nB status:\n%s\n", a_status, b_status);
    return -1;
}

static int wait_for_eof(int fd, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        uint8_t b;
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL,
                               remaining_timeout(end));
        ssize_t r;
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return 0;
        if (!(re & POLLIN))
            continue;
        r = read(fd, &b, 1);
        if (r == 0)
            return 0;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
        return -1;
    }
    return -1;
}

static int do_rendezvous_cycle(const char *host_ch3, const char *target_ch3,
                               int host_first, int host_ch1, int target_ch1)
{
    static const uint8_t ha[] = { 'h', 0x00, 0x03, 0xff };
    static const uint8_t tb[] = { 't', 0x14, 0x03, 0x00 };
    static const uint8_t ping[] = { 'c', '1' };
    static const uint8_t pong[] = { '1', 'c' };
    int h = -1, t = -1;
    int rc = -1;

    if (host_first) {
        h = connect_unix_test(host_ch3);
        if (h < 0)
            goto out;
        if (fd_has_hup(h, 200))
            goto out;
        t = connect_unix_test(target_ch3);
        if (t < 0)
            goto out;
    } else {
        t = connect_unix_test(target_ch3);
        if (t < 0)
            goto out;
        if (fd_has_hup(t, 200))
            goto out;
        h = connect_unix_test(host_ch3);
        if (h < 0)
            goto out;
    }
    if (exchange_pair(h, t, ha, sizeof(ha), tb, sizeof(tb)) < 0)
        goto out;
    if (exchange_pair(host_ch1, target_ch1, ping, sizeof(ping), pong, sizeof(pong)) < 0)
        goto out;
    rc = 0;
out:
    if (h >= 0)
        close(h);
    if (t >= 0)
        close(t);
    return rc;
}

static int abandon_pending_once(const char *path, const char *control,
                                unsigned long long resets, unsigned long long sessions,
                                const uint8_t *early, size_t early_len,
                                char *status, size_t status_cap)
{
    int fd = connect_unix_test(path);
    if (fd < 0)
        return -1;
    if (fd_has_hup(fd, 200)) {
        close(fd);
        return -1;
    }
    if (wait_channel_client(control, 3, "pending", resets, sessions, status, status_cap) < 0) {
        close(fd);
        return -1;
    }
    if (early_len && write_all_timeout(fd, early, early_len, 1000) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    if (wait_channel_client(control, 3, "none", resets, sessions, status, status_cap) < 0)
        return -1;
    return 0;
}

static void stop_child_pid(pid_t pid)
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

static int integration_rendezvous_test(void)
{
    char dir[128];
    char a1[160], a3[160], ac[160], b1[160], b3[160], bc[160];
    int carrier[2] = { -1, -1 };
    pid_t pa = -1, pb = -1;
    char st0[4096], st1[4096], st2[4096];
    unsigned long long resets0, sessions0, b_resets0, b_sessions0;
    int a_ch1 = -1, b_ch1 = -1, b_ch3 = -1, a_ch3 = -1;
    int i, rc = 1, fd_base_a = -1, fd_base_b = -1, fd_after = -1;
    const char *where = "setup";
    static const uint8_t c1a[] = { 'a', '1', 0x03 };
    static const uint8_t c1b[] = { 'b', '1', 0x14 };
    static const uint8_t d1[] = { 'a', 'b', 'c', 0x00 };
    static const uint8_t d2[] = { 'z', 'y', 'x', 0xff };
    static const uint8_t ghost[] = { 'g', 'h', 'o', 's', 't', 0x03 };

    snprintf(dir, sizeof(dir), "/tmp/lmux-it-%ld", (long)getpid());
    if (mkdir(dir, 0700) < 0)
        return fail("mkdir integration dir");
    snprintf(a1, sizeof(a1), "%s/a1.sock", dir);
    snprintf(a3, sizeof(a3), "%s/a3.sock", dir);
    snprintf(ac, sizeof(ac), "%s/ac.sock", dir);
    snprintf(b1, sizeof(b1), "%s/b1.sock", dir);
    snprintf(b3, sizeof(b3), "%s/b3.sock", dir);
    snprintf(bc, sizeof(bc), "%s/bc.sock", dir);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, carrier) < 0)
        goto out;
    where = "fork daemon A";
    pa = fork();
    if (pa == 0) {
        close(carrier[1]);
        daemon_child(carrier[0], a1, a3, ac);
    }
    where = "fork daemon B";
    pb = fork();
    if (pb == 0) {
        close(carrier[0]);
        daemon_child(carrier[1], b1, b3, bc);
    }
    close(carrier[0]);
    close(carrier[1]);
    carrier[0] = carrier[1] = -1;

    if (pa < 0 || pb < 0)
        goto out;
    where = "wait daemons established";
    if (wait_both_established(ac, st0, sizeof(st0), bc, st1, sizeof(st1)) < 0)
        goto out;
    resets0 = status_value(st0, "resets=");
    sessions0 = status_value(st0, "sessions=");
    b_resets0 = status_value(st1, "resets=");
    b_sessions0 = status_value(st1, "sessions=");

    where = "connect channel 1";
    a_ch1 = wait_connect(a1, 3000);
    b_ch1 = wait_connect(b1, 3000);
    if (a_ch1 < 0 || b_ch1 < 0)
        goto out;
    where = "channel 1 exchange";
    if (exchange_pair(a_ch1, b_ch1, c1a, sizeof(c1a), c1b, sizeof(c1b)) < 0)
        goto out;
    fd_base_a = count_fds(pa);
    fd_base_b = count_fds(pb);

    where = "target-first channel 3 pending";
    b_ch3 = connect_unix_test(b3);
    if (b_ch3 < 0 || fd_has_hup(b_ch3, 500))
        goto out;
    where = "host joins channel 3";
    a_ch3 = connect_unix_test(a3);
    if (a_ch3 < 0)
        goto out;
    where = "paired channel 3 exchange";
    if (exchange_pair(a_ch3, b_ch3, d1, sizeof(d1), d2, sizeof(d2)) < 0)
        goto out;
    close(a_ch3);
    a_ch3 = -1;
    where = "target EOF after host close";
    if (wait_for_eof(b_ch3, 3000) < 0)
        goto out;
    close(b_ch3);
    b_ch3 = -1;
    where = "channel 1 survives first close";
    if (exchange_pair(a_ch1, b_ch1, c1a, sizeof(c1a), c1b, sizeof(c1b)) < 0)
        goto out;

    where = "abandoned host pending client clears";
    if (abandon_pending_once(a3, ac, resets0, sessions0, ghost, sizeof(ghost),
                             st1, sizeof(st1)) < 0)
        goto out;
    where = "channel 1 survives abandoned host pending";
    if (exchange_pair(a_ch1, b_ch1, c1a, sizeof(c1a), c1b, sizeof(c1b)) < 0)
        goto out;
    if (fd_base_a >= 0) {
        fd_after = count_fds(pa);
        if (fd_after != fd_base_a)
            goto out;
    }

    where = "host-only channel 3 pending";
    a_ch3 = connect_unix_test(a3);
    if (a_ch3 < 0 || fd_has_hup(a_ch3, 2000))
        goto out;
    where = "status after host-only pending";
    if (read_status(ac, st1, sizeof(st1)) < 0 ||
        status_value(st1, "resets=") != resets0 ||
        status_value(st1, "sessions=") != sessions0 ||
        strstr(st1, "ch3 socket configured=yes link=idle client=pending peer_client=none") == NULL)
        goto out;
    where = "channel 1 survives host-only pending";
    if (exchange_pair(a_ch1, b_ch1, c1a, sizeof(c1a), c1b, sizeof(c1b)) < 0)
        goto out;
    where = "target joins host-pending channel 3";
    b_ch3 = connect_unix_test(b3);
    if (b_ch3 < 0)
        goto out;
    where = "host-pending channel 3 exchange";
    if (exchange_pair(a_ch3, b_ch3, d1, sizeof(d1), d2, sizeof(d2)) < 0)
        goto out;
    close(a_ch3);
    close(b_ch3);
    a_ch3 = b_ch3 = -1;

    where = "100 abandoned host pending cycles";
    for (i = 0; i < 100; i++) {
        if (abandon_pending_once(a3, ac, resets0, sessions0, ghost, sizeof(ghost),
                                 st2, sizeof(st2)) < 0)
            goto out;
        if (exchange_pair(a_ch1, b_ch1, c1a, sizeof(c1a), c1b, sizeof(c1b)) < 0)
            goto out;
    }
    if (fd_base_a >= 0 && count_fds(pa) != fd_base_a)
        goto out;
    where = "final pair after abandoned host cycles";
    a_ch3 = connect_unix_test(a3);
    b_ch3 = connect_unix_test(b3);
    if (a_ch3 < 0 || b_ch3 < 0 ||
        exchange_pair(a_ch3, b_ch3, d1, sizeof(d1), d2, sizeof(d2)) < 0)
        goto out;
    close(a_ch3);
    close(b_ch3);
    a_ch3 = b_ch3 = -1;

    where = "100 abandoned target pending cycles";
    for (i = 0; i < 100; i++) {
        if (abandon_pending_once(b3, bc, b_resets0, b_sessions0, ghost, sizeof(ghost),
                                 st2, sizeof(st2)) < 0)
            goto out;
        if (exchange_pair(a_ch1, b_ch1, c1a, sizeof(c1a), c1b, sizeof(c1b)) < 0)
            goto out;
    }
    if (fd_base_b >= 0 && count_fds(pb) != fd_base_b)
        goto out;
    where = "final pair after abandoned target cycles";
    b_ch3 = connect_unix_test(b3);
    a_ch3 = connect_unix_test(a3);
    if (a_ch3 < 0 || b_ch3 < 0 ||
        exchange_pair(a_ch3, b_ch3, d1, sizeof(d1), d2, sizeof(d2)) < 0)
        goto out;
    close(a_ch3);
    close(b_ch3);
    a_ch3 = b_ch3 = -1;
    if (wait_channel_client(ac, 3, "none", resets0, sessions0, st2, sizeof(st2)) < 0 ||
        wait_channel_client(bc, 3, "none", b_resets0, b_sessions0, st2, sizeof(st2)) < 0)
        goto out;

    for (i = 0; i < 100; i++) {
        where = "100-cycle rendezvous";
        if (do_rendezvous_cycle(a3, b3, i & 1, a_ch1, b_ch1) < 0)
            goto out;
        where = "100-cycle status";
        if (read_status(ac, st2, sizeof(st2)) < 0 ||
            status_value(st2, "resets=") != resets0 ||
            status_value(st2, "sessions=") != sessions0)
            goto out;
    }
    where = "final channel 1 exchange";
    if (exchange_pair(a_ch1, b_ch1, c1a, sizeof(c1a), c1b, sizeof(c1b)) < 0)
        goto out;
    rc = 0;
out:
    if (a_ch1 >= 0)
        close(a_ch1);
    if (b_ch1 >= 0)
        close(b_ch1);
    if (a_ch3 >= 0)
        close(a_ch3);
    if (b_ch3 >= 0)
        close(b_ch3);
    if (carrier[0] >= 0)
        close(carrier[0]);
    if (carrier[1] >= 0)
        close(carrier[1]);
    if (pa > 0) {
        stop_child_pid(pa);
    }
    if (pb > 0) {
        stop_child_pid(pb);
    }
    unlink(a1); unlink(a3); unlink(ac);
    unlink(b1); unlink(b3); unlink(bc);
    rmdir(dir);
    return rc ? fail(where) : 0;
}

int main(void)
{
    if (integration_rendezvous_test())
        return 1;
    puts("lmuxd integration selftest ok");
    return 0;
}
