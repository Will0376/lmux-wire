#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#include <time.h>
#include <unistd.h>

#include "boot_protocol.h"
#include "link_protocol.h"
#include "lxf.h"
#include "lmux.h"
#include "services.h"

static int fail(const char *msg)
{
    fprintf(stderr, "supervisor integration failed: %s\n", msg);
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

static int test_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w > 0) {
            p += w;
            len -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            (void)wait_fd_event(fd, POLLOUT, 1000);
        } else {
            return -1;
        }
    }
    return 0;
}

static int hex_encode_bytes(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char hexdig[] = "0123456789abcdef";
    size_t i;
    if (cap < len * 2 + 1)
        return -1;
    for (i = 0; i < len; i++) {
        out[i * 2] = hexdig[in[i] >> 4];
        out[i * 2 + 1] = hexdig[in[i] & 15];
    }
    out[len * 2] = 0;
    return 0;
}

static int hex_encode_cstr(const char *in, char *out, size_t cap)
{
    return hex_encode_bytes((const uint8_t *)in, strlen(in), out, cap);
}

static int read_until(int fd, const char *needle, char *out, size_t cap, int timeout_ms)
{
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    if (cap)
        out[0] = 0;
    while (now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r;
            if (len + 1 >= cap)
                return -1;
            r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                if (strstr(out, needle))
                    return 0;
            } else if (r == 0) {
                return -1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return -1;
            }
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int shell_cmd_expect(int fd, const char *cmd, const char *needle,
                            char *out, size_t cap)
{
    return write_all(fd, cmd, strlen(cmd)) < 0 ||
           write_all(fd, "\n", 1) < 0 ||
           read_until(fd, needle, out, cap, 7000) < 0 ? -1 : 0;
}

static int read_status(const char *control, char *out, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t len = 0;
    long long end = now_ms() + 2000;
    if (fd < 0 || cap == 0)
        return -1;
    out[0] = 0;
    if (write_all(fd, "STATUS\n", 7) < 0) {
        close(fd);
        return -1;
    }
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                continue;
            }
            if (r == 0) {
                close(fd);
                return len ? 0 : -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            close(fd);
            return len ? 0 : -1;
        }
    }
    close(fd);
    return len ? 0 : -1;
}

static int control_request_test(const char *control, const char *req,
                                char *out, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t len = 0;
    long long end = now_ms() + 5000;
    if (fd < 0 || cap == 0)
        return -1;
    out[0] = 0;
    if (write_all(fd, req, strlen(req)) < 0) {
        close(fd);
        return -1;
    }
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                continue;
            }
            if (r == 0) {
                close(fd);
                return len ? 0 : -1;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            close(fd);
            return len ? 0 : -1;
        }
    }
    close(fd);
    return len ? 0 : -1;
}

static int exec_request_test(const char *control, const char *req,
                             char *out, size_t cap)
{
    int fd = connect_unix_test(control);
    size_t len = 0;
    long long end = now_ms() + 20000;
    if (fd < 0 || cap == 0)
        return -1;
    out[0] = 0;
    if (write_all(fd, req, strlen(req)) < 0) {
        close(fd);
        return -1;
    }
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, out + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                out[len] = 0;
                if (strstr(out, "\nDONE\n") || strstr(out, "ERR EXEC"))
                    break;
                continue;
            }
            if (r == 0)
                break;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            break;
    }
    close(fd);
    return len ? 0 : -1;
}

static int parse_exec_id_line(const char *text, uint64_t *out)
{
    const char *p = strstr(text, "OK EXEC exec_id=");
    unsigned long long id;
    if (!p || sscanf(p, "OK EXEC exec_id=%llu", &id) != 1 || id == 0)
        return -1;
    *out = (uint64_t)id;
    return 0;
}

static int wait_marker(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        struct stat st;
        if (stat(path, &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int read_size_marker(const char *path, size_t *out)
{
    FILE *f = fopen(path, "r");
    unsigned long long v;
    if (!f)
        return -1;
    if (fscanf(f, "%llu", &v) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = (size_t)v;
    return 0;
}

static int verify_stdout_hex_pattern(const char *out, size_t expected)
{
    static const char pat[] = "0123456789abcdef";
    const char *p = out;
    size_t pos = 0;
    while ((p = strstr(p, "STDOUT_HEX ")) != NULL) {
        p += strlen("STDOUT_HEX ");
        while (p[0] && p[1] && p[0] != '\n' && p[1] != '\n') {
            unsigned value;
            if (sscanf(p, "%2x", &value) != 1)
                return -1;
            if (pos >= expected || value != (unsigned char)pat[pos & 15])
                return -1;
            pos++;
            p += 2;
        }
    }
    return pos == expected ? 0 : -1;
}

static size_t count_stdout_hex_bytes(const char *out)
{
    const char *p = out;
    size_t pos = 0;
    while ((p = strstr(p, "STDOUT_HEX ")) != NULL) {
        p += strlen("STDOUT_HEX ");
        while (p[0] && p[1] && p[0] != '\n' && p[1] != '\n') {
            pos++;
            p += 2;
        }
    }
    return pos;
}

static int hex_encode_pattern(size_t len, char *out, size_t cap)
{
    static const char hexdigits[] = "0123456789abcdef";
    size_t i;
    if (cap < len * 2 + 1)
        return -1;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)(i & 255);
        out[i * 2] = hexdigits[b >> 4];
        out[i * 2 + 1] = hexdigits[b & 15];
    }
    out[len * 2] = 0;
    return 0;
}

static int read_line_fd(int fd, char *out, size_t cap, int timeout_ms)
{
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    if (cap)
        out[0] = 0;
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            char ch;
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                out[len++] = ch;
                out[len] = 0;
                if (ch == '\n')
                    return 0;
                continue;
            }
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int boot_info_ok(const char *control, char *out, size_t cap)
{
    return control_request_test(control, "BOOT GET_INFO\n", out, cap) == 0 &&
           strstr(out, "OK GET_INFO") &&
           strstr(out, "outer_version=1") &&
           strstr(out, "boot_version=1") &&
           strstr(out, "capabilities=0x0000001f");
}

static int boot_worker_status_ok(const char *control, const char *state,
                                 char *out, size_t cap)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "worker_state=%s", state);
    return control_request_test(control, "BOOT WORKER_STATUS\n", out, cap) == 0 &&
           strstr(out, "OK WORKER_STATUS") &&
           strstr(out, needle) &&
           strstr(out, "worker_incarnation=");
}

static unsigned long long status_value(const char *status, const char *key)
{
    const char *p = strstr(status, key);
    if (!p)
        return 0;
    p += strlen(key);
    return strtoull(p, NULL, 10);
}

static int status_has_ready_layers(const char *status, int outer)
{
    if (!strstr(status, "connection: ESTABLISHED") ||
        !strstr(status, "factories: unix-rendezvous=yes pty=yes file=yes") ||
        !strstr(status, "desired_sent=yes"))
        return 0;
    if (!outer)
        return 1;
    return strstr(status, "outer: ESTABLISHED") &&
           strstr(status, "pre_established_drops=0") &&
           status_value(status, "transport_tx_bytes=") > 0 &&
           status_value(status, "transport_rx_bytes=") > 0;
}

static int wait_ready_layers(const char *control, int outer, int timeout_ms,
                             char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0 &&
            status_has_ready_layers(last, outer))
            return 0;
        usleep(50000);
    }
    return -1;
}

static int wait_ready_layers_after_outer_change(const char *control,
                                                unsigned long long old_local,
                                                unsigned long long old_peer,
                                                int timeout_ms,
                                                char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0 &&
            status_has_ready_layers(last, 1) &&
            status_value(last, "local_conn=") != old_local &&
            status_value(last, "peer_conn=") != old_peer)
            return 0;
        usleep(50000);
    }
    return -1;
}

static int wait_outer_only(const char *control, int timeout_ms,
                           char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0 &&
            strstr(last, "outer: ESTABLISHED") &&
            strstr(last, "outer_boot: caps=BOOT_INFO,BOOT_UPLOAD,BOOT_MANAGED,BOOT_SUPERVISOR"))
            return 0;
        usleep(50000);
    }
    return -1;
}

static int wait_pending_state(const char *control, int want_pending,
                              int timeout_ms, char *last, size_t cap)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        last[0] = 0;
    while (now_ms() < end) {
        if (read_status(control, last, cap) == 0) {
            int pending = strstr(last, "console1") &&
                          strstr(last, "pending_fd=yes");
            if (!!pending == !!want_pending)
                return 0;
        }
        usleep(20000);
    }
    return -1;
}

static int wait_fault_injected(int fd, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        char ch;
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            if (read(fd, &ch, 1) == 1 && ch == 'I')
                return 0;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static int discard_one_cobs_record(int fd, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    unsigned char ch;
    size_t n = 0;
    while (now_ms() < end) {
        int rem = (int)(end - now_ms());
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, rem);
        if (re & POLLIN) {
            ssize_t r = read(fd, &ch, 1);
            if (r == 1) {
                n++;
                if (ch == 0)
                    return n > 1 ? 0 : -1;
                continue;
            }
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            return -1;
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
}

static long parse_pid_marker(const char *buf, const char *prefix)
{
    const char *p = strstr(buf, prefix);
    if (!p)
        return -1;
    p += strlen(prefix);
    return strtol(p, NULL, 10);
}

static void dump_excerpt(const char *label, const char *buf)
{
    size_t i, n = strlen(buf);
    fprintf(stderr, "%s len=%zu data=", label, n);
    if (n > 200)
        n = 200;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch >= 32 && ch < 127 && ch != '\\')
            fputc(ch, stderr);
        else
            fprintf(stderr, "\\x%02x", ch);
    }
    fputc('\n', stderr);
}

static void stop_pid(pid_t pid)
{
    long long end;
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    end = now_ms() + 1500;
    while (now_ms() < end) {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return;
        usleep(20000);
    }
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
}

static void stop_pgrp(pid_t pid)
{
    if (pid <= 0)
        return;
    kill(-pid, SIGTERM);
    stop_pid(pid);
    kill(-pid, SIGKILL);
}

static pid_t spawn_supervisor_with_storage(const char *carrier,
                                           const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX_SUP_IT_OUTER")) {
            if (storage_root) {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-listen", carrier,
                          "--storage-root", storage_root, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-listen", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            } else {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-listen", carrier, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-listen", carrier, (char *)NULL);
            }
        } else {
            if (storage_root) {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--carrier-listen", carrier,
                          "--storage-root", storage_root, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-listen", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            } else {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--carrier-listen", carrier, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-listen", carrier, (char *)NULL);
            }
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_exec_storage(const char *exe, const char *carrier,
                                           const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (storage_root) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl(exe, "lmuxd", "--supervisor", "--outer",
                      "--carrier-listen", carrier, "--storage-root",
                      storage_root, "--verbose", (char *)NULL);
            execl(exe, "lmuxd", "--supervisor", "--outer",
                  "--carrier-listen", carrier, "--storage-root",
                  storage_root, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl(exe, "lmuxd", "--supervisor", "--outer",
                      "--carrier-listen", carrier, "--verbose", (char *)NULL);
            execl(exe, "lmuxd", "--supervisor", "--outer",
                  "--carrier-listen", carrier, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_respawner(const char *exe, const char *carrier,
                                        const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        for (;;) {
            pid_t child = fork();
            int st;
            if (child == 0) {
                if (storage_root)
                    execl(exe, "lmuxd", "--supervisor", "--outer",
                          "--carrier-listen", carrier, "--storage-root",
                          storage_root, (char *)NULL);
                execl(exe, "lmuxd", "--supervisor", "--outer",
                      "--carrier-listen", carrier, (char *)NULL);
                _exit(127);
            }
            if (child < 0)
                _exit(127);
            while (waitpid(child, &st, 0) < 0 && errno == EINTR)
                ;
            usleep(100000);
        }
    }
    return pid;
}

static pid_t spawn_supervisor(const char *carrier)
{
    return spawn_supervisor_with_storage(carrier, NULL);
}

static pid_t spawn_supervisor_bad_worker(const char *carrier)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor",
              "--outer", "--carrier-listen", carrier,
              "--worker-path", "/tmp/lmux-no-such-worker", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int supervisor_storage_start_fails(const char *carrier,
                                          const char *storage_root)
{
    pid_t pid = fork();
    int st;
    if (pid == 0) {
        execl("build/host/lmuxd", "lmuxd", "--supervisor",
              "--outer", "--carrier-listen", carrier,
              "--storage-root", storage_root, (char *)NULL);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) != 0 ? 0 : -1;
}

static int invalid_storage_root_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-storage-it-XXXXXX";
    char carrier[160], missing[180], file[180];
    int fd, rc = 1;
    if (!mkdtemp(dir))
        return fail("mkdtemp storage invalid");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(missing, sizeof(missing), "%s/missing", dir);
    snprintf(file, sizeof(file), "%s/file", dir);
    fd = open(file, O_CREAT | O_WRONLY, 0600);
    if (fd < 0)
        goto out;
    close(fd);
    if (supervisor_storage_start_fails(carrier, "relative") < 0 ||
        supervisor_storage_start_fails(carrier, missing) < 0 ||
        supervisor_storage_start_fails(carrier, file) < 0)
        goto out;
    rc = 0;
out:
    unlink(file);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static pid_t spawn_host(const char *carrier, const char *control,
                        const char *pty1, const char *file1)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX_SUP_IT_OUTER")) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-connect", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-connect", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--carrier-connect", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host_listen(const char *carrier, const char *control,
                               const char *pty1, const char *file1, int outer)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (outer) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-listen", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-listen", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--carrier-listen", carrier, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-listen", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_fd(int fd, int outer)
{
    pid_t pid = fork();
    char fdarg[32];
    snprintf(fdarg, sizeof(fdarg), "%d", fd);
    if (pid == 0) {
        if (outer) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-fd", fdarg, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--supervisor",
                  "--outer", "--carrier-fd", fdarg, (char *)NULL);
        } else {
            execl("build/host/lmuxd", "lmuxd", "--supervisor",
                  "--carrier-fd", fdarg, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_connect_storage(const char *carrier, int outer,
                                              const char *storage_root)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (outer) {
            if (storage_root) {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-connect", carrier,
                          "--storage-root", storage_root, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-connect", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            } else {
                if (getenv("LMUX2_IT_VERBOSE"))
                    execl("build/host/lmuxd", "lmuxd", "--supervisor",
                          "--outer", "--carrier-connect", carrier, "--verbose", (char *)NULL);
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--outer", "--carrier-connect", carrier, (char *)NULL);
            }
        } else {
            if (storage_root)
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-connect", carrier,
                      "--storage-root", storage_root, (char *)NULL);
            else
                execl("build/host/lmuxd", "lmuxd", "--supervisor",
                      "--carrier-connect", carrier, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

static pid_t spawn_supervisor_connect(const char *carrier, int outer)
{
    return spawn_supervisor_connect_storage(carrier, outer, NULL);
}

static pid_t spawn_host_fd(int fd, const char *control,
                           const char *pty1, const char *file1, int outer)
{
    pid_t pid = fork();
    char fdarg[32];
    snprintf(fdarg, sizeof(fdarg), "%d", fd);
    if (pid == 0) {
        if (outer) {
            if (getenv("LMUX2_IT_VERBOSE"))
                execl("build/host/lmuxd", "lmuxd", "--role", "host",
                      "--outer", "--carrier-fd", fdarg, "--control", control,
                      "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--outer", "--carrier-fd", fdarg, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        } else {
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-fd", fdarg, "--control", control,
                  "--pty", pty1, "--file", file1, (char *)NULL);
        }
        _exit(127);
    }
    return pid;
}

struct test_ring {
    unsigned char buf[8192];
    size_t r;
    size_t w;
    size_t len;
};

static void test_ring_clear(struct test_ring *r)
{
    r->r = r->w = r->len = 0;
}

static size_t test_ring_space(const struct test_ring *r)
{
    return sizeof(r->buf) - r->len;
}

static size_t test_ring_write(struct test_ring *r, const unsigned char *buf, size_t len)
{
    size_t done = 0;
    if (len > test_ring_space(r))
        len = test_ring_space(r);
    while (done < len) {
        size_t chunk = sizeof(r->buf) - r->w;
        if (chunk > len - done)
            chunk = len - done;
        memcpy(r->buf + r->w, buf + done, chunk);
        r->w = (r->w + chunk) % sizeof(r->buf);
        r->len += chunk;
        done += chunk;
    }
    return done;
}

static size_t test_ring_peek(const struct test_ring *r, const unsigned char **p)
{
    size_t chunk = sizeof(r->buf) - r->r;
    if (chunk > r->len)
        chunk = r->len;
    *p = r->buf + r->r;
    return chunk;
}

static void test_ring_consume(struct test_ring *r, size_t len)
{
    if (len > r->len)
        len = r->len;
    r->r = (r->r + len) % sizeof(r->buf);
    r->len -= len;
    if (r->len == 0)
        r->r = r->w = 0;
}

static pid_t spawn_persistent_proxy(const char *path, int stable_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        int lfd, cfd = -1;
        struct sockaddr_un sa;
        struct test_ring to_stable = {{0}, 0, 0, 0};
        struct test_ring to_client = {{0}, 0, 0, 0};
        signal(SIGPIPE, SIG_IGN);
        unlink(path);
        lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (lfd < 0)
            _exit(127);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(lfd, 4) < 0)
            _exit(127);
        (void)test_set_nonblock(stable_fd);
        for (;;) {
            struct pollfd pfds[3];
            nfds_t n = 0, i;
            pfds[n].fd = lfd;
            pfds[n].events = cfd < 0 ? POLLIN : 0;
            pfds[n++].revents = 0;
            pfds[n].fd = stable_fd;
            pfds[n].events = 0;
            if (cfd >= 0 && test_ring_space(&to_client) > 0)
                pfds[n].events |= POLLIN;
            if (to_stable.len > 0)
                pfds[n].events |= POLLOUT;
            pfds[n++].revents = 0;
            if (cfd >= 0) {
                pfds[n].fd = cfd;
                pfds[n].events = 0;
                if (test_ring_space(&to_stable) > 0)
                    pfds[n].events |= POLLIN;
                if (to_client.len > 0)
                    pfds[n].events |= POLLOUT;
                pfds[n++].revents = 0;
            }
            if (poll(pfds, n, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < n; i++) {
                if (!pfds[i].revents)
                    continue;
                if (pfds[i].fd == lfd && (pfds[i].revents & POLLIN)) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd >= 0) {
                        (void)test_set_nonblock(fd);
                        if (cfd >= 0)
                            close(cfd);
                        cfd = fd;
                        test_ring_clear(&to_client);
                    }
                } else if (pfds[i].fd == stable_fd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(stable_fd, tmp, sizeof(tmp));
                        if (r > 0)
                            (void)test_ring_write(&to_client, tmp, (size_t)r);
                        else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                            _exit(0);
                    }
                    if (pfds[i].revents & POLLOUT) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_stable, &p);
                        ssize_t w = write(stable_fd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_stable, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            _exit(0);
                    }
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                        _exit(0);
                } else if (cfd >= 0 && pfds[i].fd == cfd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(cfd, tmp, sizeof(tmp));
                        if (r > 0)
                            (void)test_ring_write(&to_stable, tmp, (size_t)r);
                        else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            close(cfd);
                            cfd = -1;
                            test_ring_clear(&to_client);
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & POLLOUT)) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_client, &p);
                        ssize_t w = write(cfd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_client, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            close(cfd);
                            cfd = -1;
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                        close(cfd);
                        cfd = -1;
                        test_ring_clear(&to_client);
                    }
                }
            }
        }
    }
    return pid;
}

enum outer_fault_kind {
    OUTER_FAULT_BAD_CRC = 1,
    OUTER_FAULT_SEQ_GAP = 2,
    OUTER_FAULT_BOOT_BAD_HASH = 3,
    OUTER_FAULT_BOOT_BAD_OFFSET = 4,
    OUTER_FAULT_BOOT_BAD_CRC = 5,
    OUTER_FAULT_BOOT_CONFIRM_REQ_BAD_CRC = 6,
    OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC = 7
};

enum outer_fault_dir {
    OUTER_FAULT_HOST_TO_TARGET = 1,
    OUTER_FAULT_TARGET_TO_HOST = 2
};

struct outer_fault_filter {
    int armed;
    int injected;
    enum outer_fault_kind kind;
    uint8_t rec[LMUX_LINK_MAX_ENCODED];
    size_t rec_len;
};

static int fault_transform_record(struct outer_fault_filter *f,
                                  const uint8_t *rec, size_t rec_len,
                                  uint8_t *out, size_t out_cap,
                                  size_t *out_len)
{
    uint8_t decoded[LMUX_LINK_MAX_DECODED];
    size_t decoded_len, enc_len;
    uint16_t header_len;
    uint32_t payload_len;
    uint8_t frame_type;
    uint8_t *payload;
    uint16_t opcode = 0;
    int matched = 0;

    *out_len = 0;
    if (!f->armed || f->injected ||
        lmux_cobs_decode(rec, rec_len, decoded, sizeof(decoded), &decoded_len) < 0 ||
        decoded_len < LMUX_LINK_HEADER_LEN + 4 ||
        decoded[0] != LMUX_LINK_MAGIC0 || decoded[1] != LMUX_LINK_MAGIC1 ||
        decoded[2] != LMUX_LINK_MAGIC2 || decoded[3] != LMUX_LINK_MAGIC3 ||
        decoded[6] < LMUX_LINK_T_HELLO || decoded[6] > LMUX_LINK_T_BOOT_EVENT) {
        if (rec_len + 1 > out_cap)
            return -1;
        memcpy(out, rec, rec_len);
        out[rec_len] = 0;
        *out_len = rec_len + 1;
        return 0;
    }
    frame_type = decoded[6];
    header_len = lmux_get_le16(decoded + 8);
    payload_len = lmux_get_le32(decoded + 12);
    if (header_len < LMUX_LINK_HEADER_LEN ||
        header_len > LMUX_LINK_MAX_HEADER_LEN ||
        payload_len > LMUX_LINK_MAX_PAYLOAD ||
        decoded_len != (size_t)header_len + payload_len + 4) {
        if (rec_len + 1 > out_cap)
            return -1;
        memcpy(out, rec, rec_len);
        out[rec_len] = 0;
        *out_len = rec_len + 1;
        return 0;
    }
    payload = decoded + header_len;
    if (frame_type == LMUX_LINK_T_BOOT_REQUEST &&
        payload_len >= LMUX_BOOT_REQ_HEADER_LEN)
        opcode = lmux_get_le16(payload);
    else if (frame_type == LMUX_LINK_T_BOOT_RESPONSE &&
             payload_len >= LMUX_BOOT_RESP_HEADER_LEN)
        opcode = lmux_get_le16(payload);

    if ((f->kind == OUTER_FAULT_BAD_CRC &&
         frame_type == LMUX_LINK_T_TRANSPORT) ||
        (f->kind == OUTER_FAULT_BOOT_BAD_CRC &&
         frame_type == LMUX_LINK_T_BOOT_REQUEST &&
         opcode == LMUX_BOOT_UPLOAD_DATA) ||
        (f->kind == OUTER_FAULT_BOOT_CONFIRM_REQ_BAD_CRC &&
         frame_type == LMUX_LINK_T_BOOT_REQUEST &&
         opcode == LMUX_BOOT_CONFIRM_CANDIDATE) ||
        (f->kind == OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC &&
         frame_type == LMUX_LINK_T_BOOT_RESPONSE &&
         opcode == LMUX_BOOT_CONFIRM_CANDIDATE)) {
        decoded[decoded_len - 1] ^= 0x5a;
        matched = 1;
    } else if (f->kind == OUTER_FAULT_SEQ_GAP &&
               frame_type == LMUX_LINK_T_TRANSPORT) {
        f->injected = 1;
        *out_len = 0;
        return 0;
    } else if (f->kind == OUTER_FAULT_BOOT_BAD_HASH &&
               frame_type == LMUX_LINK_T_BOOT_REQUEST &&
               opcode == LMUX_BOOT_UPLOAD_END &&
               payload_len >= LMUX_BOOT_REQ_HEADER_LEN + 40) {
        payload[LMUX_BOOT_REQ_HEADER_LEN + 8] ^= 0x5a;
        lmux_put_le32(decoded + decoded_len - 4,
                      lmux_crc32c(decoded, decoded_len - 4));
        matched = 1;
    } else if (f->kind == OUTER_FAULT_BOOT_BAD_OFFSET &&
               frame_type == LMUX_LINK_T_BOOT_REQUEST &&
               opcode == LMUX_BOOT_UPLOAD_DATA &&
               payload_len >= LMUX_BOOT_REQ_HEADER_LEN + 16) {
        payload[LMUX_BOOT_REQ_HEADER_LEN + 8] ^= 0x01;
        lmux_put_le32(decoded + decoded_len - 4,
                      lmux_crc32c(decoded, decoded_len - 4));
        matched = 1;
    }
    if (!matched) {
        if (rec_len + 1 > out_cap)
            return -1;
        memcpy(out, rec, rec_len);
        out[rec_len] = 0;
        *out_len = rec_len + 1;
        return 0;
    }
    if (lmux_cobs_encode(decoded, decoded_len, out, out_cap - 1, &enc_len) < 0)
        return -1;
    out[enc_len++] = 0;
    *out_len = enc_len;
    f->injected = 1;
    return 0;
}

static int fault_filter_feed(struct outer_fault_filter *f,
                             const uint8_t *in, size_t in_len,
                             struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (!f->armed || f->injected) {
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            continue;
        }
        if (ch == 0) {
            uint8_t out[LMUX_LINK_MAX_ENCODED];
            size_t out_len = 0;
            if (fault_transform_record(f, f->rec, f->rec_len, out, sizeof(out), &out_len) < 0)
                return -1;
            if (test_ring_write(dst, out, out_len) != out_len)
                return -1;
            f->rec_len = 0;
            if (f->injected && notify_fd >= 0)
                (void)write(notify_fd, "I", 1);
            continue;
        }
        if (f->rec_len >= sizeof(f->rec))
            return -1;
        f->rec[f->rec_len++] = ch;
    }
    return 0;
}

static pid_t spawn_fault_proxy(const char *path, int stable_fd,
                               enum outer_fault_kind kind,
                               enum outer_fault_dir dir,
                               int arm_fd, int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        int lfd, cfd = -1;
        struct sockaddr_un sa;
        struct test_ring to_stable = {{0}, 0, 0, 0};
        struct test_ring to_client = {{0}, 0, 0, 0};
        struct outer_fault_filter h2t, t2h;
        signal(SIGPIPE, SIG_IGN);
        memset(&h2t, 0, sizeof(h2t));
        memset(&t2h, 0, sizeof(t2h));
        h2t.kind = kind;
        t2h.kind = kind;
        unlink(path);
        lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (lfd < 0)
            _exit(127);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        strcpy(sa.sun_path, path);
        if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(lfd, 4) < 0)
            _exit(127);
        (void)test_set_nonblock(stable_fd);
        (void)test_set_nonblock(arm_fd);
        for (;;) {
            struct pollfd pfds[4];
            nfds_t n = 0, i;
            pfds[n].fd = arm_fd;
            pfds[n].events = POLLIN;
            pfds[n++].revents = 0;
            pfds[n].fd = lfd;
            pfds[n].events = cfd < 0 ? POLLIN : 0;
            pfds[n++].revents = 0;
            pfds[n].fd = stable_fd;
            pfds[n].events = 0;
            if (cfd >= 0 && test_ring_space(&to_client) > LMUX_LINK_MAX_ENCODED)
                pfds[n].events |= POLLIN;
            if (to_stable.len > 0)
                pfds[n].events |= POLLOUT;
            pfds[n++].revents = 0;
            if (cfd >= 0) {
                pfds[n].fd = cfd;
                pfds[n].events = 0;
                if (test_ring_space(&to_stable) > LMUX_LINK_MAX_ENCODED)
                    pfds[n].events |= POLLIN;
                if (to_client.len > 0)
                    pfds[n].events |= POLLOUT;
                pfds[n++].revents = 0;
            }
            if (poll(pfds, n, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < n; i++) {
                if (!pfds[i].revents)
                    continue;
                if (pfds[i].fd == arm_fd && (pfds[i].revents & POLLIN)) {
                    char tmp[16];
                    while (read(arm_fd, tmp, sizeof(tmp)) > 0) {
                        if (dir == OUTER_FAULT_HOST_TO_TARGET)
                            h2t.armed = 1;
                        else
                            t2h.armed = 1;
                    }
                } else if (pfds[i].fd == lfd && (pfds[i].revents & POLLIN)) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd >= 0) {
                        (void)test_set_nonblock(fd);
                        if (cfd >= 0)
                            close(cfd);
                        cfd = fd;
                        test_ring_clear(&to_client);
                    }
                } else if (pfds[i].fd == stable_fd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(stable_fd, tmp, sizeof(tmp));
                        if (r > 0) {
                            if (fault_filter_feed(&h2t, tmp, (size_t)r, &to_client,
                                                  h2t.injected ? -1 : notify_fd) < 0)
                                _exit(127);
                        } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            _exit(0);
                        }
                    }
                    if (pfds[i].revents & POLLOUT) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_stable, &p);
                        ssize_t w = write(stable_fd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_stable, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                            _exit(0);
                    }
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                        _exit(0);
                } else if (cfd >= 0 && pfds[i].fd == cfd) {
                    if (pfds[i].revents & POLLIN) {
                        unsigned char tmp[512];
                        ssize_t r = read(cfd, tmp, sizeof(tmp));
                        if (r > 0) {
                            if (fault_filter_feed(&t2h, tmp, (size_t)r, &to_stable,
                                                  t2h.injected ? -1 : notify_fd) < 0)
                                _exit(127);
                        } else if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                            close(cfd);
                            cfd = -1;
                            test_ring_clear(&to_client);
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & POLLOUT)) {
                        const unsigned char *p;
                        size_t chunk = test_ring_peek(&to_client, &p);
                        ssize_t w = write(cfd, p, chunk);
                        if (w > 0)
                            test_ring_consume(&to_client, (size_t)w);
                        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            close(cfd);
                            cfd = -1;
                        }
                    }
                    if (cfd >= 0 && (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                        close(cfd);
                        cfd = -1;
                        test_ring_clear(&to_client);
                    }
                }
            }
        }
    }
    return pid;
}

static int first_hello_truncate_feed(int *injected, uint8_t *rec,
                                     size_t *rec_len,
                                     const uint8_t *in, size_t in_len,
                                     struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (*injected) {
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            continue;
        }
        if (ch == 0) {
            size_t keep = *rec_len / 2;
            if (keep == 0 && *rec_len > 0)
                keep = 1;
            if (test_ring_write(dst, rec, keep) != keep)
                return -1;
            *rec_len = 0;
            *injected = 1;
            if (notify_fd >= 0)
                (void)write(notify_fd, "I", 1);
            continue;
        }
        if (*rec_len >= LMUX_LINK_MAX_ENCODED)
            return -1;
        rec[(*rec_len)++] = ch;
    }
    return 0;
}

static pid_t spawn_truncate_first_hello_proxy(int host_fd, int target_fd,
                                              int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        uint8_t rec[LMUX_LINK_MAX_ENCODED];
        size_t rec_len = 0;
        int injected = 0;
        signal(SIGPIPE, SIG_IGN);
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        for (;;) {
            struct pollfd pfds[2];
            nfds_t i;
            pfds[0].fd = host_fd;
            pfds[0].events = 0;
            if (test_ring_space(&to_target) > LMUX_LINK_MAX_ENCODED)
                pfds[0].events |= POLLIN;
            if (to_host.len > 0)
                pfds[0].events |= POLLOUT;
            pfds[0].revents = 0;
            pfds[1].fd = target_fd;
            pfds[1].events = 0;
            if (test_ring_space(&to_host) > LMUX_LINK_MAX_ENCODED)
                pfds[1].events |= POLLIN;
            if (to_target.len > 0)
                pfds[1].events |= POLLOUT;
            pfds[1].revents = 0;
            if (poll(pfds, 2, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < 2; i++) {
                int fd = pfds[i].fd;
                struct test_ring *out = i == 0 ? &to_target : &to_host;
                if (pfds[i].revents & POLLIN) {
                    uint8_t tmp[512];
                    ssize_t r = read(fd, tmp, sizeof(tmp));
                    if (r > 0) {
                        if (i == 1) {
                            if (first_hello_truncate_feed(&injected, rec, &rec_len,
                                                          tmp, (size_t)r, out,
                                                          notify_fd) < 0)
                                _exit(127);
                        } else if (test_ring_write(out, tmp, (size_t)r) != (size_t)r) {
                            _exit(127);
                        }
                    } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                          errno != EWOULDBLOCK && errno != EINTR)) {
                        _exit(0);
                    }
                }
                if (pfds[i].revents & POLLOUT) {
                    struct test_ring *src = i == 0 ? &to_host : &to_target;
                    const unsigned char *p;
                    size_t chunk = test_ring_peek(src, &p);
                    ssize_t w = write(fd, p, chunk);
                    if (w > 0)
                        test_ring_consume(src, (size_t)w);
                    else if (w < 0 && errno != EAGAIN &&
                             errno != EWOULDBLOCK && errno != EINTR)
                        _exit(0);
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    _exit(0);
            }
        }
    }
    return pid;
}

static int drop_first_ack_feed(int *dropped, uint8_t *rec, size_t *rec_len,
                               const uint8_t *in, size_t in_len,
                               struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (ch == 0) {
            struct lmux_link_frame fr;
            if (!*dropped && *rec_len > 0 &&
                lmux_link_decode_frame(rec, *rec_len, &fr) == 0) {
                if (fr.type == LMUX_LINK_T_HELLO_ACK) {
                    *dropped = 1;
                    *rec_len = 0;
                    if (notify_fd >= 0)
                        (void)write(notify_fd, "I", 1);
                    continue;
                }
            }
            if (*rec_len > 0 && test_ring_write(dst, rec, *rec_len) != *rec_len)
                return -1;
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            *rec_len = 0;
            continue;
        }
        if (*rec_len >= LMUX_LINK_MAX_ENCODED)
            return -1;
        rec[(*rec_len)++] = ch;
    }
    return 0;
}

static pid_t spawn_drop_first_ack_proxy(int host_fd, int target_fd,
                                        int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        uint8_t h2t_rec[LMUX_LINK_MAX_ENCODED];
        uint8_t t2h_rec[LMUX_LINK_MAX_ENCODED];
        size_t h2t_rec_len = 0, t2h_rec_len = 0;
        int dropped = 0;
        signal(SIGPIPE, SIG_IGN);
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        for (;;) {
            struct pollfd pfds[2];
            nfds_t i;
            pfds[0].fd = host_fd;
            pfds[0].events = 0;
            if (test_ring_space(&to_target) > LMUX_LINK_MAX_ENCODED)
                pfds[0].events |= POLLIN;
            if (to_host.len > 0)
                pfds[0].events |= POLLOUT;
            pfds[0].revents = 0;
            pfds[1].fd = target_fd;
            pfds[1].events = 0;
            if (test_ring_space(&to_host) > LMUX_LINK_MAX_ENCODED)
                pfds[1].events |= POLLIN;
            if (to_target.len > 0)
                pfds[1].events |= POLLOUT;
            pfds[1].revents = 0;
            if (poll(pfds, 2, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < 2; i++) {
                int fd = pfds[i].fd;
                struct test_ring *out = i == 0 ? &to_target : &to_host;
                if (pfds[i].revents & POLLIN) {
                    uint8_t tmp[512];
                    ssize_t r = read(fd, tmp, sizeof(tmp));
                    if (r > 0) {
                        if (i == 0) {
                            if (drop_first_ack_feed(&dropped, h2t_rec,
                                                    &h2t_rec_len,
                                                    tmp, (size_t)r, out,
                                                    notify_fd) < 0)
                                _exit(127);
                        } else {
                            if (drop_first_ack_feed(&dropped, t2h_rec,
                                                    &t2h_rec_len,
                                                    tmp, (size_t)r, out,
                                                    notify_fd) < 0)
                                _exit(127);
                        }
                    } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                          errno != EWOULDBLOCK && errno != EINTR)) {
                        _exit(0);
                    }
                }
                if (pfds[i].revents & POLLOUT) {
                    struct test_ring *src = i == 0 ? &to_host : &to_target;
                    const unsigned char *p;
                    size_t chunk = test_ring_peek(src, &p);
                    ssize_t w = write(fd, p, chunk);
                    if (w > 0)
                        test_ring_consume(src, (size_t)w);
                    else if (w < 0 && errno != EAGAIN &&
                             errno != EWOULDBLOCK && errno != EINTR)
                        _exit(0);
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    _exit(0);
            }
        }
    }
    return pid;
}

static int duplicate_first_hello_feed(int *duplicated, uint8_t *rec,
                                      size_t *rec_len,
                                      const uint8_t *in, size_t in_len,
                                      struct test_ring *dst, int notify_fd)
{
    size_t i;
    for (i = 0; i < in_len; i++) {
        uint8_t ch = in[i];
        if (*duplicated) {
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            continue;
        }
        if (ch == 0) {
            struct lmux_link_frame fr;
            if (*rec_len > 0 &&
                lmux_link_decode_frame(rec, *rec_len, &fr) == 0 &&
                fr.type == LMUX_LINK_T_HELLO) {
                int n;
                for (n = 0; n < 8; n++) {
                    if (test_ring_write(dst, rec, *rec_len) != *rec_len ||
                        test_ring_write(dst, &ch, 1) != 1)
                        return -1;
                }
                *duplicated = 1;
                *rec_len = 0;
                if (notify_fd >= 0)
                    (void)write(notify_fd, "I", 1);
                continue;
            }
            if (*rec_len > 0 && test_ring_write(dst, rec, *rec_len) != *rec_len)
                return -1;
            if (test_ring_write(dst, &ch, 1) != 1)
                return -1;
            *rec_len = 0;
            continue;
        }
        if (*rec_len >= LMUX_LINK_MAX_ENCODED)
            return -1;
        rec[(*rec_len)++] = ch;
    }
    return 0;
}

static pid_t spawn_duplicate_first_target_hello_proxy(int host_fd, int target_fd,
                                                      int notify_fd)
{
    pid_t pid = fork();
    if (pid == 0) {
        struct test_ring to_host = {{0}, 0, 0, 0};
        struct test_ring to_target = {{0}, 0, 0, 0};
        uint8_t rec[LMUX_LINK_MAX_ENCODED];
        size_t rec_len = 0;
        int duplicated = 0;
        signal(SIGPIPE, SIG_IGN);
        (void)test_set_nonblock(host_fd);
        (void)test_set_nonblock(target_fd);
        for (;;) {
            struct pollfd pfds[2];
            nfds_t i;
            pfds[0].fd = host_fd;
            pfds[0].events = 0;
            if (test_ring_space(&to_target) > LMUX_LINK_MAX_ENCODED)
                pfds[0].events |= POLLIN;
            if (to_host.len > 0)
                pfds[0].events |= POLLOUT;
            pfds[0].revents = 0;
            pfds[1].fd = target_fd;
            pfds[1].events = 0;
            if (test_ring_space(&to_host) > 1024)
                pfds[1].events |= POLLIN;
            if (to_target.len > 0)
                pfds[1].events |= POLLOUT;
            pfds[1].revents = 0;
            if (poll(pfds, 2, 1000) < 0) {
                if (errno == EINTR)
                    continue;
                _exit(127);
            }
            for (i = 0; i < 2; i++) {
                int fd = pfds[i].fd;
                struct test_ring *out = i == 0 ? &to_target : &to_host;
                if (pfds[i].revents & POLLIN) {
                    uint8_t tmp[512];
                    ssize_t r = read(fd, tmp, sizeof(tmp));
                    if (r > 0) {
                        if (i == 1) {
                            if (duplicate_first_hello_feed(&duplicated, rec,
                                                           &rec_len, tmp,
                                                           (size_t)r, out,
                                                           notify_fd) < 0)
                                _exit(127);
                        } else if (test_ring_write(out, tmp, (size_t)r) != (size_t)r) {
                            _exit(127);
                        }
                    } else if (r == 0 || (r < 0 && errno != EAGAIN &&
                                          errno != EWOULDBLOCK && errno != EINTR)) {
                        _exit(0);
                    }
                }
                if (pfds[i].revents & POLLOUT) {
                    struct test_ring *src = i == 0 ? &to_host : &to_target;
                    const unsigned char *p;
                    size_t chunk = test_ring_peek(src, &p);
                    ssize_t w = write(fd, p, chunk);
                    if (w > 0)
                        test_ring_consume(src, (size_t)w);
                    else if (w < 0 && errno != EAGAIN &&
                             errno != EWOULDBLOCK && errno != EINTR)
                        _exit(0);
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    _exit(0);
            }
        }
    }
    return pid;
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

static int wait_fd_count(pid_t pid, int want, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    int n = -1;
    while (now_ms() < end) {
        n = count_fds(pid);
        if (n == want)
            return n;
        usleep(20000);
    }
    return n;
}

static pid_t find_child_pid(pid_t ppid)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        char path[128], buf[512];
        FILE *f;
        long pid, parent = -1;
        char *p;
        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        pid = strtol(de->d_name, NULL, 10);
        snprintf(path, sizeof(path), "/proc/%ld/status", pid);
        f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "PPid:", 5) == 0) {
                p = buf + 5;
                parent = strtol(p, NULL, 10);
                break;
            }
        }
        fclose(f);
        if (parent == (long)ppid) {
            closedir(d);
            return (pid_t)pid;
        }
    }
    closedir(d);
    return -1;
}

static pid_t wait_child_pid(pid_t ppid, pid_t not_pid, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        pid_t p = find_child_pid(ppid);
        if (p > 0 && p != not_pid)
            return p;
        usleep(20000);
    }
    return -1;
}

static int pid_exists(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static int wait_pid_gone(pid_t pid, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (!pid_exists(pid))
            return 0;
        usleep(20000);
    }
    return pid_exists(pid) ? -1 : 0;
}

static int run_lmux_file(const char *op, const char *sock,
                         const char *a, const char *b)
{
    pid_t pid = fork();
    int st;
    if (pid == 0) {
        execl("build/host/lmux", "lmux", op, "--socket", sock, a, b, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_boot_cli(const char *cmd, const char *control,
                             char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", cmd,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_boot_upload(const char *control, const char *file,
                                char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", "upload", file,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_deploy_ex(const char *control, const char *file,
                              unsigned timeout_ms, int restart_supervisor,
                              char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    char timeout[32];
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    snprintf(timeout, sizeof(timeout), "%u", timeout_ms);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        if (restart_supervisor)
            execl("build/host/lmux", "lmux", "deploy", file,
                  "--restart-supervisor", "--timeout-ms", timeout,
                  "--control", control, (char *)NULL);
        else
            execl("build/host/lmux", "lmux", "deploy", file,
                  "--timeout-ms", timeout, "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_deploy(const char *control, const char *file,
                           unsigned timeout_ms, char *out, size_t cap)
{
    return run_lmux_deploy_ex(control, file, timeout_ms, 0, out, cap);
}

static int run_lmux_deploy_restart_only(const char *control,
                                        char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "deploy", "--restart-supervisor",
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_deploy_wait_fault(const char *control, const char *file,
                                      unsigned timeout_ms, int fault_fd,
                                      char *out, size_t cap)
{
    int p[2] = {-1, -1}, st = 0, injected = 0, child_done = 0;
    pid_t pid;
    size_t len = 0;
    char timeout[32];
    long long deadline = now_ms() + 60000;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    snprintf(timeout, sizeof(timeout), "%u", timeout_ms);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "deploy", file,
              "--timeout-ms", timeout, "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    p[1] = -1;
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    (void)test_set_nonblock(p[0]);
    (void)test_set_nonblock(fault_fd);
    while (now_ms() < deadline) {
        struct pollfd pfds[2];
        int n = 0;
        pfds[n].fd = p[0];
        pfds[n].events = POLLIN | POLLHUP | POLLERR | POLLNVAL;
        pfds[n++].revents = 0;
        if (!injected) {
            pfds[n].fd = fault_fd;
            pfds[n].events = POLLIN | POLLHUP | POLLERR | POLLNVAL;
            pfds[n++].revents = 0;
        }
        (void)poll(pfds, n, 100);
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                ssize_t r;
                if (len + 1 >= cap)
                    break;
                r = read(p[0], out + len, cap - len - 1);
                if (r > 0) {
                    len += (size_t)r;
                    out[len] = 0;
                    continue;
                }
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    break;
                break;
            }
        }
        if (n > 1 && (pfds[1].revents & POLLIN)) {
            char ch;
            while (read(fault_fd, &ch, 1) == 1) {
                if (ch == 'I')
                    injected = 1;
            }
        }
        if (!child_done && waitpid(pid, &st, WNOHANG) == pid)
            child_done = 1;
        if (child_done) {
            for (;;) {
                ssize_t r;
                if (len + 1 >= cap)
                    break;
                r = read(p[0], out + len, cap - len - 1);
                if (r > 0) {
                    len += (size_t)r;
                    out[len] = 0;
                    continue;
                }
                break;
            }
            close(p[0]);
            return injected && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
        }
    }
    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    close(p[0]);
    return -1;
}

static int run_lmux_boot_activate(const char *control, const char *sha,
                                  unsigned timeout_ms, char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    char timeout[32];
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    snprintf(timeout, sizeof(timeout), "%u", timeout_ms);
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", "activate", sha,
              "--timeout-ms", timeout, "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_boot_id(const char *cmd, const char *control,
                            const char *id, char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", cmd, id,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int run_lmux_boot_sha(const char *cmd, const char *control,
                             const char *sha, char *out, size_t cap)
{
    int p[2], st;
    pid_t pid;
    size_t len = 0;
    if (pipe(p) < 0 || cap == 0)
        return -1;
    out[0] = 0;
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        if (dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(p[1]);
        execl("build/host/lmux", "lmux", "boot", cmd, sha,
              "--control", control, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        return -1;
    }
    while (len + 1 < cap) {
        ssize_t r = read(p[0], out + len, cap - len - 1);
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(p[0]);
        return -1;
    }
    close(p[0]);
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int sha_file_hex(const char *path, char hex[65])
{
    int fd = open(path, O_RDONLY);
    struct sha256_ctx sha;
    uint8_t buf[512], digest[32];
    if (fd < 0)
        return -1;
    sha256_init(&sha);
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r > 0) {
            sha256_update(&sha, buf, (size_t)r);
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    close(fd);
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    return 0;
}

static int copy_with_suffix(const char *src, const char *dst, const char *suffix)
{
    int in = -1, out = -1, rc = -1;
    uint8_t buf[4096];
    in = open(src, O_RDONLY);
    if (in < 0)
        goto done;
    out = open(dst, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (out < 0)
        goto done;
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r > 0) {
            if (write_all(out, buf, (size_t)r) < 0)
                goto done;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        goto done;
    }
    if (suffix && write_all(out, suffix, strlen(suffix)) < 0)
        goto done;
    if (fchmod(out, 0755) < 0)
        goto done;
    rc = 0;
done:
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return rc;
}

static void test_put_le64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)v);
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

static void fill_digest(uint8_t digest[32], unsigned seed)
{
    size_t i;
    for (i = 0; i < 32; i++)
        digest[i] = (uint8_t)(seed + i * 17u + 1u);
}

static int worker_artifact_path(const char *workers, const uint8_t digest[32],
                                char *out, size_t cap)
{
    char hex[65];
    sha256_hex(digest, hex);
    return snprintf(out, cap, "%s/%s.lmuxd", workers, hex) > 0 &&
           strlen(out) < cap ? 0 : -1;
}

static int write_worker_state_fixture(const char *root, uint16_t phase,
                                      uint64_t generation,
                                      uint64_t activation_id,
                                      const uint8_t confirmed[32],
                                      const uint8_t candidate[32],
                                      int corrupt_crc,
                                      int reserved_nonzero)
{
    uint8_t rec[128];
    char path[220];
    uint32_t crc;
    int fd;

    if (snprintf(path, sizeof(path), "%s/worker.state", root) <= 0 ||
        strlen(path) >= sizeof(path))
        return -1;
    memset(rec, 0, sizeof(rec));
    rec[0] = 'L';
    rec[1] = 'M';
    rec[2] = 'W';
    rec[3] = 'S';
    lmux_put_le16(rec + 4, 1);
    lmux_put_le16(rec + 6, phase);
    test_put_le64(rec + 8, generation);
    test_put_le64(rec + 16, activation_id);
    memcpy(rec + 24, confirmed, 32);
    memcpy(rec + 56, candidate, 32);
    if (reserved_nonzero)
        rec[88] = 1;
    crc = lmux_crc32c(rec, 124);
    if (corrupt_crc)
        crc ^= 0x01020304u;
    lmux_put_le32(rec + 124, crc);

    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    if (write_all(fd, rec, sizeof(rec)) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int write_small_executable(const char *path, const char *data)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0755);
    if (fd < 0)
        return -1;
    if (write_all(fd, data, strlen(data)) < 0 || fchmod(fd, 0755) < 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int partial_upload_disconnect(const char *control, const char *file,
                                     uint64_t size, size_t partial)
{
    int fd = -1, cfd = -1, rc = -1;
    char req[128], line[256];
    uint8_t buf[512];
    size_t sent = 0;
    fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    cfd = connect_unix_test(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "BOOT UPLOAD %llu\n",
             (unsigned long long)size);
    if (write_all(cfd, req, strlen(req)) < 0 ||
        read_line_fd(cfd, line, sizeof(line), 5000) < 0 ||
        strncmp(line, "READY", 5) != 0)
        goto out;
    while (sent < partial) {
        size_t want = partial - sent;
        ssize_t r;
        if (want > sizeof(buf))
            want = sizeof(buf);
        r = read(fd, buf, want);
        if (r <= 0)
            goto out;
        if (write_all(cfd, buf, (size_t)r) < 0)
            goto out;
        sent += (size_t)r;
    }
    rc = 0;
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int partial_deploy_disconnect(const char *control, const char *file,
                                     uint64_t size, size_t partial,
                                     unsigned timeout_ms)
{
    int fd = -1, cfd = -1, rc = -1;
    char req[128], line[256];
    uint8_t buf[512];
    size_t sent = 0;
    fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    cfd = connect_unix_test(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "DEPLOY %llu %u\n",
             (unsigned long long)size, timeout_ms);
    if (write_all(cfd, req, strlen(req)) < 0 ||
        read_line_fd(cfd, line, sizeof(line), 5000) < 0 ||
        strncmp(line, "READY", 5) != 0)
        goto out;
    while (sent < partial) {
        size_t want = partial - sent;
        ssize_t r;
        if (want > sizeof(buf))
            want = sizeof(buf);
        r = read(fd, buf, want);
        if (r <= 0)
            goto out;
        if (write_all(cfd, buf, (size_t)r) < 0)
            goto out;
        sent += (size_t)r;
    }
    rc = 0;
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int deploy_disconnect_after_activate(const char *control,
                                            const char *file,
                                            uint64_t size,
                                            unsigned timeout_ms)
{
    int fd = -1, cfd = -1, rc = -1;
    char req[128], line[256];
    uint8_t buf[512];
    uint64_t sent = 0;
    fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    cfd = connect_unix_test(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "DEPLOY %llu %u\n",
             (unsigned long long)size, timeout_ms);
    if (write_all(cfd, req, strlen(req)) < 0 ||
        read_line_fd(cfd, line, sizeof(line), 5000) < 0 ||
        strncmp(line, "READY", 5) != 0)
        goto out;
    while (sent < size) {
        size_t want = size - sent > sizeof(buf) ? sizeof(buf) : (size_t)(size - sent);
        ssize_t r = read(fd, buf, want);
        if (r <= 0)
            goto out;
        if (write_all(cfd, buf, (size_t)r) < 0)
            goto out;
        sent += (uint64_t)r;
    }
    for (;;) {
        if (read_line_fd(cfd, line, sizeof(line), 20000) < 0)
            goto out;
        if (strncmp(line, "activation started", 18) == 0) {
            rc = 0;
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0)
            goto out;
    }
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int wait_path_absent(const char *path, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (lstat(path, &(struct stat){0}) < 0 && errno == ENOENT)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int make_file(const char *path, size_t len)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    size_t i;
    if (fd < 0)
        return -1;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)(i * 31u + (i >> 7));
        if (write(fd, &b, 1) != 1) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

static int files_equal(const char *a, const char *b)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    unsigned char ba[512], bb[512];
    if (fa < 0 || fb < 0)
        return -1;
    for (;;) {
        ssize_t ra = read(fa, ba, sizeof(ba));
        ssize_t rb = read(fb, bb, sizeof(bb));
        if (ra != rb) {
            close(fa); close(fb);
            return -1;
        }
        if (ra <= 0) {
            close(fa); close(fb);
            return ra == 0 ? 0 : -1;
        }
        if (memcmp(ba, bb, (size_t)ra) != 0) {
            close(fa); close(fb);
            return -1;
        }
    }
}

static int integration_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-it-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], upload_src[160], upload_final[512], upload_tmp[512];
    char candidate_src[160], candidate_final[512], baseline_final[512];
    char other_src[160], other_final[512], bad_worker_src[160], bad_worker_final[512];
    char deploy_src[160], deploy_final[512];
    char active_dir[160], active_exe[220], active_hex[65];
    char pty_arg[320], file_arg[320];
    char src[160], target[160], got[160];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    int cfd = -1, rc = 1;
    long shell_pid = -1, shell_pid_after = -1, shell_pid_new = -1, shell_pid_sup_restart = -1;
    long shell_pid_candidate = -1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(storage_root, sizeof(storage_root), "%s/custom-storage", dir);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(upload_src, sizeof(upload_src), "%s/upload-worker.bin", dir);
    snprintf(candidate_src, sizeof(candidate_src), "%s/candidate-worker.lmuxd", dir);
    snprintf(other_src, sizeof(other_src), "%s/other-worker.lmuxd", dir);
    snprintf(bad_worker_src, sizeof(bad_worker_src), "%s/bad-worker.lmuxd", dir);
    snprintf(deploy_src, sizeof(deploy_src), "%s/deploy-worker.lmuxd", dir);
    snprintf(active_dir, sizeof(active_dir), "%s/active", dir);
    snprintf(active_exe, sizeof(active_exe), "%s/lmuxd", active_dir);
    snprintf(upload_tmp, sizeof(upload_tmp), "%s/.upload.tmp", workers);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    snprintf(src, sizeof(src), "%s/src.bin", dir);
    snprintf(target, sizeof(target), "%s/target.bin", dir);
    snprintf(got, sizeof(got), "%s/got.bin", dir);

    if (mkdir(storage_root, 0755) < 0 || mkdir(active_dir, 0755) < 0)
        goto out;
    sup = spawn_supervisor_with_storage(carrier,
                                        getenv("LMUX_SUP_IT_OUTER") ? storage_root : NULL);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 5000);
    if (worker <= 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    where = "host sockets";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_path_exists(console, 5000) < 0 ||
        wait_path_exists(file_sock, 5000) < 0)
        goto out;
    where = "startup layers";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") != NULL,
                          12000, out, sizeof(out)) < 0) {
        dump_excerpt("startup status", out);
        goto out;
    }
    if (getenv("LMUX_SUP_IT_OUTER")) {
        where = "boot get_info";
        if (!boot_info_ok(control, out, sizeof(out))) {
            dump_excerpt("boot get_info", out);
            goto out;
        }
        where = "boot worker_status active";
        if (!boot_worker_status_ok(control, "ACTIVE", out, sizeof(out))) {
            dump_excerpt("boot worker_status", out);
            goto out;
        }
        where = "boot cli";
        if (run_lmux_boot_cli("info", control, out, sizeof(out)) < 0 ||
            !strstr(out, "OK GET_INFO") ||
            run_lmux_boot_cli("worker-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "OK WORKER_STATUS")) {
            dump_excerpt("boot cli", out);
            goto out;
        }
    }
    where = "console through supervisor";
    cfd = wait_connect(console, 10000);
    if (cfd < 0)
        goto out;
    usleep(2000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID \"$$\"", "SPID:", out, sizeof(out)) < 0) {
        dump_excerpt("console first", out);
        goto out;
    }
    shell_pid = parse_pid_marker(out, "SPID:");
    if (shell_pid <= 0) {
        dump_excerpt("console first parse", out);
        goto out;
    }
    if (getenv("LMUX_SUP_IT_OUTER")) {
        int k;
        char hex[65], candidate_hex[65], baseline_hex[65], activation_id[32];
        char deploy_hex[65];
        char stale_activation_id[32], other_hex[65], bad_hex[65];
        unsigned long long outer_local_before, outer_peer_before;
        unsigned long long testing_generation;
        int fd_base = -1, fd_end = -1;
        struct stat st;
        where = "boot during pty";
        for (k = 0; k < 5; k++) {
            if (!boot_info_ok(control, out, sizeof(out)) ||
                !boot_worker_status_ok(control, "ACTIVE", out, sizeof(out))) {
                dump_excerpt("boot during pty", out);
                goto out;
            }
        }
        if (shell_cmd_expect(cfd, "printf '%s\\n' BOOT_PTY_ALIVE",
                             "BOOT_PTY_ALIVE", out, sizeof(out)) < 0) {
            dump_excerpt("pty after boot", out);
            goto out;
        }
        where = "boot upload success";
        if (make_file(upload_src, 384 * 1024) < 0 ||
            sha_file_hex(upload_src, hex) < 0) {
            goto out;
        }
        if (snprintf(upload_final, sizeof(upload_final), "%s/%s.lmuxd",
                     workers, hex) <= 0 ||
            strlen(upload_final) >= sizeof(upload_final))
            goto out;
        where = "boot upload abort cleanup";
        if (partial_upload_disconnect(control, upload_src, 384 * 1024, 4096) < 0 ||
            wait_path_absent(upload_tmp, 5000) < 0) {
            if (lstat(upload_tmp, &st) == 0)
                fprintf(stderr, "upload temp still present after abort\n");
            goto out;
        }
        where = "boot upload success";
        if (run_lmux_boot_upload(control, upload_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            !strstr(out, hex) ||
            lstat(upload_tmp, &st) == 0 ||
            lstat(upload_final, &st) < 0 ||
            (st.st_mode & 0111) == 0 ||
            files_equal(upload_src, upload_final) < 0) {
            dump_excerpt("boot upload", out);
            goto out;
        }
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' U PID \"$$\"",
                             "UPID:", out, sizeof(out)) < 0 ||
            parse_pid_marker(out, "UPID:") != shell_pid) {
            dump_excerpt("pty after upload", out);
            goto out;
        }
        where = "managed activation candidate upload";
        if (copy_with_suffix("build/host/lmuxd", candidate_src,
                             "LMUX_P4_TEST_CANDIDATE\n") < 0 ||
            sha_file_hex(candidate_src, candidate_hex) < 0)
            goto out;
        if (snprintf(candidate_final, sizeof(candidate_final), "%s/%s.lmuxd",
                     workers, candidate_hex) <= 0 ||
            strlen(candidate_final) >= sizeof(candidate_final))
            goto out;
        if (run_lmux_boot_upload(control, candidate_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            lstat(candidate_final, &st) < 0)
            goto out;
        where = "managed activation baseline identity";
        {
            char proc[64];
            snprintf(proc, sizeof(proc), "/proc/%ld/exe", (long)worker);
            if (sha_file_hex(proc, baseline_hex) < 0)
                goto out;
        }
        if (snprintf(baseline_final, sizeof(baseline_final), "%s/%s.lmuxd",
                     workers, baseline_hex) <= 0 ||
            strlen(baseline_final) >= sizeof(baseline_final))
            goto out;
        where = "managed activate";
        if (read_status(control, out, sizeof(out)) < 0)
            goto out;
        outer_local_before = status_value(out, "local_conn=");
        outer_peer_before = status_value(out, "peer_conn=");
        close(cfd);
        cfd = -1;
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") == 0) {
            dump_excerpt("activate", out);
            goto out;
        }
        snprintf(activation_id, sizeof(activation_id), "%llu",
                 status_value(out, "activation_id="));
        if (lstat(baseline_final, &st) < 0)
            goto out;
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            status_value(out, "local_conn=") != outer_local_before ||
            status_value(out, "peer_conn=") != outer_peer_before) {
            dump_excerpt("activate ready", out);
            goto out;
        }
        cfd = wait_connect(console, 10000);
        if (cfd < 0)
            goto out;
        usleep(1000000);
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' C PID \"$$\"",
                             "CPID:", out, sizeof(out)) < 0) {
            dump_excerpt("candidate console", out);
            goto out;
        }
        shell_pid_candidate = parse_pid_marker(out, "CPID:");
        if (shell_pid_candidate <= 0 || shell_pid_candidate == shell_pid) {
            dump_excerpt("candidate pid", out);
            goto out;
        }
        where = "managed explicit rollback";
        snprintf(stale_activation_id, sizeof(stale_activation_id), "%s", activation_id);
        if (run_lmux_boot_id("rollback", control, activation_id, out, sizeof(out)) < 0 ||
            !strstr(out, "OK ROLLBACK")) {
            dump_excerpt("rollback", out);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, baseline_hex)) {
            dump_excerpt("rollback status", out);
            goto out;
        }
        where = "managed activate stable no-op";
        if (run_lmux_boot_activate(control, baseline_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=STABLE") ||
            wait_child_pid(sup, worker, 1000) != -1) {
            dump_excerpt("activate no-op", out);
            goto out;
        }
        where = "managed candidate corruption before activate";
        if (copy_with_suffix("build/host/lmuxd", other_src,
                             "LMUX_P4_TEST_CORRUPT_CANDIDATE\n") < 0 ||
            sha_file_hex(other_src, other_hex) < 0)
            goto out;
        if (snprintf(other_final, sizeof(other_final), "%s/%s.lmuxd",
                     workers, other_hex) <= 0 ||
            strlen(other_final) >= sizeof(other_final))
            goto out;
        if (run_lmux_boot_upload(control, other_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            lstat(other_final, &st) < 0)
            goto out;
        {
            int fd = open(other_final, O_WRONLY | O_APPEND);
            if (fd < 0)
                goto out;
            if (write_all(fd, "corrupt\n", 8) < 0) {
                close(fd);
                goto out;
            }
            close(fd);
        }
        if (run_lmux_boot_activate(control, other_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT") ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, baseline_hex)) {
            dump_excerpt("corrupt candidate activate", out);
            goto out;
        }
        where = "managed candidate crash rollback";
        if (make_file(bad_worker_src, 4096) < 0 ||
            sha_file_hex(bad_worker_src, bad_hex) < 0)
            goto out;
        if (snprintf(bad_worker_final, sizeof(bad_worker_final), "%s/%s.lmuxd",
                     workers, bad_hex) <= 0 ||
            strlen(bad_worker_final) >= sizeof(bad_worker_final))
            goto out;
        if (run_lmux_boot_upload(control, bad_worker_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD") ||
            lstat(bad_worker_final, &st) < 0)
            goto out;
        if (run_lmux_boot_activate(control, bad_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING"))
            goto out;
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        {
            long long deadline = now_ms() + 12000;
            int stable = 0;
            while (now_ms() < deadline) {
                if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) == 0 &&
                    strstr(out, "phase=STABLE") &&
                    strstr(out, baseline_hex)) {
                    stable = 1;
                    break;
                }
                usleep(50000);
            }
            if (!stable) {
                dump_excerpt("candidate crash rollback status", out);
                goto out;
            }
        }
        worker2 = wait_child_pid(sup, -1, 1000);
        if (worker2 <= 0)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed timeout rollback";
        if (run_lmux_boot_activate(control, candidate_hex, 1000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING")) {
            dump_excerpt("activate timeout", out);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        {
            long long deadline = now_ms() + 8000;
            int stable = 0;
            while (now_ms() < deadline) {
                if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) == 0 &&
                    strstr(out, "phase=STABLE") &&
                    strstr(out, baseline_hex)) {
                    stable = 1;
                    break;
                }
                usleep(50000);
            }
            if (!stable) {
                dump_excerpt("timeout status", out);
                goto out;
            }
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed activate second";
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") == 0) {
            dump_excerpt("activate second", out);
            goto out;
        }
        snprintf(activation_id, sizeof(activation_id), "%llu",
                 status_value(out, "activation_id="));
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed activate testing idempotent";
        if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id))
            goto out;
        testing_generation = status_value(out, "generation=");
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") != strtoull(activation_id, NULL, 10) ||
            status_value(out, "generation=") != testing_generation ||
            wait_child_pid(sup, worker, 1000) != -1) {
            dump_excerpt("testing activate idempotent", out);
            goto out;
        }
        where = "managed activate busy";
        if (copy_with_suffix("build/host/lmuxd", other_src,
                             "LMUX_P4_TEST_BUSY_CANDIDATE\n") < 0 ||
            sha_file_hex(other_src, other_hex) < 0)
            goto out;
        if (run_lmux_boot_upload(control, other_src, out, sizeof(out)) < 0 ||
            !strstr(out, "OK UPLOAD"))
            goto out;
        if (run_lmux_boot_activate(control, other_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT BUSY") ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id) ||
            status_value(out, "generation=") != testing_generation) {
            dump_excerpt("testing activate busy", out);
            goto out;
        }
        where = "managed stale activation ids";
        if (!strcmp(stale_activation_id, activation_id) ||
            run_lmux_boot_id("confirm", control, stale_activation_id,
                             out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT ID_MISMATCH") ||
            run_lmux_boot_id("rollback", control, stale_activation_id,
                             out, sizeof(out)) < 0 ||
            !strstr(out, "ERR BOOT ID_MISMATCH") ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id) ||
            status_value(out, "generation=") != testing_generation ||
            wait_child_pid(sup, worker, 1000) != -1) {
            dump_excerpt("stale activation ids", out);
            goto out;
        }
        where = "managed supervisor restart while testing";
        close(cfd);
        cfd = -1;
        stop_pid(sup);
        sup = -1;
        worker = -1;
        sup = spawn_supervisor_with_storage(carrier, storage_root);
        if (sup < 0)
            goto out;
        worker = wait_child_pid(sup, -1, 10000);
        if (worker <= 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, baseline_hex)) {
            dump_excerpt("restart while testing status", out);
            goto out;
        }
        where = "managed activate third";
        if (run_lmux_boot_activate(control, candidate_hex, 30000,
                                   out, sizeof(out)) < 0 ||
            !strstr(out, "OK ACTIVATE") ||
            !strstr(out, "phase=TESTING") ||
            status_value(out, "activation_id=") == 0) {
            dump_excerpt("activate third", out);
            goto out;
        }
        snprintf(activation_id, sizeof(activation_id), "%llu",
                 status_value(out, "activation_id="));
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "managed physical loss pauses testing";
        stop_pid(host);
        host = -1;
        sleep(1);
        if (!pid_exists(worker))
            goto out;
        host = spawn_host(carrier, control, pty_arg, file_arg);
        if (host < 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=TESTING") ||
            !strstr(out, activation_id)) {
            dump_excerpt("testing after physical loss", out);
            goto out;
        }
        where = "managed confirm";
        if (run_lmux_boot_id("confirm", control, activation_id, out, sizeof(out)) < 0 ||
            !strstr(out, "OK CONFIRM") ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, candidate_hex)) {
            dump_excerpt("confirm/status", out);
            goto out;
        }
        where = "managed stable after supervisor restart";
        close(cfd);
        cfd = -1;
        stop_pid(sup);
        sup = -1;
        worker = -1;
        sup = spawn_supervisor_with_storage(carrier, storage_root);
        if (sup < 0)
            goto out;
        worker = wait_child_pid(sup, -1, 10000);
        if (worker <= 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, candidate_hex)) {
            dump_excerpt("restart managed status", out);
            goto out;
        }
        where = "switch supervisor to temp active executable";
        close(cfd);
        cfd = -1;
        stop_pid(sup);
        sup = -1;
        worker = -1;
        if (copy_with_suffix("build/host/lmuxd", active_exe,
                             "LMUX_ACTIVE_SUPERVISOR_A\n") < 0 ||
            sha_file_hex(active_exe, active_hex) < 0)
            goto out;
        sup = spawn_supervisor_exec_storage(active_exe, carrier, storage_root);
        if (sup < 0)
            goto out;
        worker = wait_child_pid(sup, -1, 10000);
        if (worker <= 0)
            goto out;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        where = "high-level deploy normal";
        if (copy_with_suffix("build/host/lmuxd", deploy_src,
                             "LMUX_P5_DEPLOY_CANDIDATE\n") < 0 ||
            sha_file_hex(deploy_src, deploy_hex) < 0)
            goto out;
        if (snprintf(deploy_final, sizeof(deploy_final), "%s/%s.lmuxd",
                     workers, deploy_hex) <= 0 ||
            strlen(deploy_final) >= sizeof(deploy_final))
            goto out;
        if (run_lmux_deploy(control, deploy_src, 30000, out, sizeof(out)) < 0 ||
            !strstr(out, "OK DEPLOY") ||
            !strstr(out, "deploy complete") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy normal", out);
            goto out;
        }
        if (sha_file_hex(active_exe, active_hex) < 0 ||
            strcmp(active_hex, deploy_hex) != 0) {
            fprintf(stderr, "active supervisor sha after deploy %s expected %s\n",
                    active_hex, deploy_hex);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy status", out);
            goto out;
        }
        where = "high-level deploy already active";
        if (run_lmux_deploy(control, deploy_src, 30000, out, sizeof(out)) < 0 ||
            !strstr(out, "OK DEPLOY") ||
            !strstr(out, "already active") ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy already active", out);
            goto out;
        }
        where = "high-level deploy candidate crash";
        if (run_lmux_deploy(control, bad_worker_src, 30000, out, sizeof(out)) == 0 ||
            !strstr(out, "ERR DEPLOY") ||
            !strstr(out, "candidate rolled back")) {
            dump_excerpt("deploy crash", out);
            goto out;
        }
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy crash rollback status", out);
            goto out;
        }
        where = "managed repeated rollback cycles";
        fd_base = count_fds(sup);
        if (fd_base < 0)
            goto out;
        for (k = 0; k < 3; k++) {
            if (run_lmux_boot_activate(control, baseline_hex, 30000,
                                       out, sizeof(out)) < 0 ||
                !strstr(out, "OK ACTIVATE") ||
                !strstr(out, "phase=TESTING") ||
                status_value(out, "activation_id=") == 0) {
                dump_excerpt("cycle activate", out);
                goto out;
            }
            snprintf(activation_id, sizeof(activation_id), "%llu",
                     status_value(out, "activation_id="));
            worker2 = wait_child_pid(sup, worker, 10000);
            if (worker2 <= 0 || worker2 == worker)
                goto out;
            worker = worker2;
            worker2 = -1;
            if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
                goto out;
            if (run_lmux_boot_id("rollback", control, activation_id,
                                 out, sizeof(out)) < 0 ||
                !strstr(out, "OK ROLLBACK")) {
                dump_excerpt("cycle rollback", out);
                goto out;
            }
            worker2 = wait_child_pid(sup, worker, 10000);
            if (worker2 <= 0 || worker2 == worker)
                goto out;
            worker = worker2;
            worker2 = -1;
            if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
                run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
                !strstr(out, "phase=STABLE") ||
                !strstr(out, deploy_hex)) {
                dump_excerpt("cycle status", out);
                goto out;
            }
        }
        fd_end = wait_fd_count(sup, fd_base, 3000);
        if (fd_end != fd_base) {
            fprintf(stderr, "managed fd baseline base=%d end=%d\n", fd_base, fd_end);
            goto out;
        }
        cfd = wait_connect(console, 10000);
        if (cfd < 0)
            goto out;
        usleep(1000000);
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' R PID \"$$\"",
                             "RPID:", out, sizeof(out)) < 0) {
            dump_excerpt("restart managed console", out);
            goto out;
        }
        shell_pid = parse_pid_marker(out, "RPID:");
        if (shell_pid <= 0)
            goto out;
        close(cfd);
        cfd = -1;
        where = "high-level deploy disconnect before activate";
        if (copy_with_suffix("build/host/lmuxd", other_src,
                             "LMUX_P51_DEPLOY_DISCONNECT_PRE_ACTIVATE\n") < 0 ||
            sha_file_hex(other_src, other_hex) < 0 ||
            stat(other_src, &st) < 0)
            goto out;
        if (partial_deploy_disconnect(control, other_src, (uint64_t)st.st_size,
                                      4096, 30000) < 0 ||
            wait_path_absent(upload_tmp, 5000) < 0 ||
            wait_child_pid(sup, worker, 1000) != -1 ||
            run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
            !strstr(out, "phase=STABLE") ||
            !strstr(out, deploy_hex)) {
            dump_excerpt("deploy disconnect before activate", out);
            goto out;
        }
        where = "high-level deploy disconnect after activate";
        fd_base = count_fds(host);
        if (fd_base < 0 ||
            deploy_disconnect_after_activate(control, other_src,
                                             (uint64_t)st.st_size, 30000) < 0)
            goto out;
        worker2 = wait_child_pid(sup, worker, 10000);
        if (worker2 <= 0 || worker2 == worker)
            goto out;
        worker = worker2;
        worker2 = -1;
        {
            long long deadline = now_ms() + 30000;
            int stable_done = 0;
            while (now_ms() < deadline) {
                if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) == 0 &&
                    strstr(out, "phase=STABLE") &&
                    strstr(out, other_hex)) {
                    stable_done = 1;
                    break;
                }
                usleep(50000);
            }
            if (!stable_done) {
                dump_excerpt("deploy disconnect after activate status", out);
                goto out;
            }
        }
        fd_end = wait_fd_count(host, fd_base, 3000);
        if (fd_end != fd_base) {
            fprintf(stderr, "deploy disconnect fd baseline base=%d end=%d\n",
                    fd_base, fd_end);
            goto out;
        }
        if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0)
            goto out;
        cfd = wait_connect(console, 10000);
        if (cfd < 0)
            goto out;
        usleep(1000000);
        if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' D PID \"$$\"",
                             "DPID:", out, sizeof(out)) < 0) {
            dump_excerpt("deploy disconnect shell", out);
            goto out;
        }
        shell_pid = parse_pid_marker(out, "DPID:");
        if (shell_pid <= 0)
            goto out;
        close(cfd);
        cfd = -1;
    }

    where = "file through supervisor";
    if (make_file(src, 256 * 1024) < 0 ||
        run_lmux_file("put", file_sock, src, target) < 0 ||
        run_lmux_file("get", file_sock, target, got) < 0 ||
        files_equal(src, got) < 0)
        goto out;

    where = "carrier loss preserves worker and shell";
    close(cfd);
    cfd = -1;
    stop_pid(host);
    host = -1;
    sleep(1);
    if (wait_child_pid(sup, -1, 1000) != worker)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    cfd = wait_connect(console, 10000);
    usleep(2000000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID2 \"$$\"", "SPID2:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_after = parse_pid_marker(out, "SPID2:");
    if (shell_pid_after != shell_pid)
        goto out;

    where = "worker crash restart";
    close(cfd);
    cfd = -1;
    kill(worker, SIGKILL);
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0)
        goto out;
    if (wait_pid_gone(worker, 3000) < 0 || wait_pid_gone((pid_t)shell_pid, 3000) < 0)
        goto out;
    cfd = wait_connect(console, 12000);
    usleep(2000000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID3 \"$$\"", "SPID3:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_new = parse_pid_marker(out, "SPID3:");
    if (shell_pid_new <= 0 || shell_pid_new == shell_pid)
        goto out;
    if (getenv("LMUX_SUP_IT_OUTER")) {
        where = "boot worker_status after restart";
        if (!boot_worker_status_ok(control, "ACTIVE", out, sizeof(out)) ||
            status_value(out, "restart_count=") < 1) {
            dump_excerpt("boot after worker restart", out);
            goto out;
        }
    }
    if (run_lmux_file("put", file_sock, src, target) < 0 ||
        run_lmux_file("get", file_sock, target, got) < 0 ||
        files_equal(src, got) < 0)
        goto out;

    where = "supervisor restart with host alive";
    close(cfd);
    cfd = -1;
    stop_pid(sup);
    sup = -1;
    worker = worker2;
    sup = spawn_supervisor(carrier);
    if (sup < 0)
        goto out;
    worker2 = wait_child_pid(sup, -1, 10000);
    if (worker2 <= 0)
        goto out;
    sleep(3);
    cfd = wait_connect(console, 12000);
    usleep(2000000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s:%s\\n' S PID4 \"$$\"", "SPID4:", out, sizeof(out)) < 0)
        goto out;
    shell_pid_sup_restart = parse_pid_marker(out, "SPID4:");
    if (shell_pid_sup_restart <= 0 || shell_pid_sup_restart == shell_pid_new)
        goto out;

    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    unlink(src);
    unlink(target);
    unlink(got);
    unlink(upload_final);
    unlink(candidate_final);
    unlink(other_final);
    unlink(bad_worker_final);
    unlink(deploy_final);
    unlink(candidate_src);
    unlink(other_src);
    unlink(bad_worker_src);
    unlink(deploy_src);
    unlink(upload_tmp);
    rmdir(workers);
    rmdir(storage_root);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int host_first_startup_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-hostfirst-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    pid_t sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[4096];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp host-first");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    if (wait_path_exists(console, 5000) < 0)
        goto out;
    setenv("LMUX_SUP_TEST_UP_ACK_DELAY_MS", "750", 1);
    sup = spawn_supervisor(carrier);
    unsetenv("LMUX_SUP_TEST_UP_ACK_DELAY_MS");
    if (sup < 0)
        goto out;
    where = "host-first startup layers";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") != NULL,
                          12000, out, sizeof(out)) < 0) {
        dump_excerpt("host-first status", out);
        goto out;
    }
    where = "host-first console";
    cfd = wait_connect(console, 12000);
    if (cfd < 0)
        goto out;
    usleep(1000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' H FIRST \"$$\"",
                         "HFIRST:", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "host_first where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int pending_abandon_test(int outer)
{
    char dir[] = "/tmp/lmux-supervisor-pending-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    pid_t host = -1;
    int dummy = -1, base_fds = -1, end_fds = -1, rc = 1, i;
    char out[4096];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp pending");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    host = spawn_host_listen(carrier, control, pty_arg, file_arg, outer);
    if (host < 0)
        goto out;
    if (wait_path_exists(control, 5000) < 0 ||
        wait_path_exists(console, 5000) < 0)
        goto out;
    dummy = wait_connect(carrier, 5000);
    if (dummy < 0)
        goto out;
    {
        long long end = now_ms() + 5000;
        do {
            if (read_status(control, out, sizeof(out)) == 0 &&
                strstr(out, "carrier: open"))
                break;
            usleep(20000);
        } while (now_ms() < end);
        if (!strstr(out, "carrier: open"))
            goto out;
    }
    if (outer) {
        long long end = now_ms() + 5000;
        do {
            if (read_status(control, out, sizeof(out)) == 0 &&
                strstr(out, "outer: WAIT_HELLO"))
                break;
            usleep(20000);
        } while (now_ms() < end);
        if (!strstr(out, "outer: WAIT_HELLO"))
            goto out;
    }
    base_fds = count_fds(host);
    if (base_fds < 0)
        goto out;
    where = outer ? "outer pending abandon" : "raw pending abandon";
    for (i = 0; i < 100; i++) {
        int cfd = wait_connect(console, 1000);
        if (cfd < 0)
            goto out;
        if (wait_pending_state(control, 1, 1000, out, sizeof(out)) < 0) {
            close(cfd);
            dump_excerpt("pending yes status", out);
            goto out;
        }
        close(cfd);
        if (wait_pending_state(control, 0, 1000, out, sizeof(out)) < 0) {
            dump_excerpt("pending no status", out);
            goto out;
        }
    }
    end_fds = wait_fd_count(host, base_fds, 1000);
    if (end_fds != base_fds) {
        fprintf(stderr, "pending fd baseline base=%d end=%d\n", base_fds, end_fds);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "pending where=%s\n", where);
    if (dummy >= 0)
        close(dummy);
    stop_pid(host);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int outer_boot_worker_backoff_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-boot-backoff-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char pty_arg[320], file_arg[320], out[4096];
    pid_t sup = -1, host = -1;
    int rc = 1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp boot backoff");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_bad_worker(carrier);
    if (sup < 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    where = "outer only ready";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_outer_only(control, 10000, out, sizeof(out)) < 0) {
        dump_excerpt("boot backoff status", out);
        goto out;
    }
    where = "boot info during backoff";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("boot backoff info", out);
        goto out;
    }
    where = "boot worker status backoff";
    if (!boot_worker_status_ok(control, "BACKOFF", out, sizeof(out))) {
        dump_excerpt("boot backoff worker", out);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "boot_backoff where=%s\n", where);
    stop_pid(host);
    stop_pid(sup);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int managed_expect_degraded(const char *storage_root)
{
    char dir[] = "/tmp/lmux-supervisor-managed-degraded-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char pty_arg[320], file_arg[320], out[4096];
    pid_t sup = -1, host = -1;
    int rc = 1;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp managed degraded case");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    sup = spawn_supervisor_with_storage(carrier, storage_root);
    if (sup < 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    where = "outer degraded ready";
    if (wait_outer_only(control, 10000, out, sizeof(out)) < 0) {
        dump_excerpt("managed degraded outer", out);
        goto out;
    }
    where = "degraded update status";
    if (run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=DEGRADED")) {
        dump_excerpt("managed degraded update", out);
        goto out;
    }
    where = "degraded no worker";
    if (wait_child_pid(sup, -1, 500) != -1)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "managed_degraded where=%s\n", where);
    stop_pid(host);
    stop_pid(sup);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int managed_corrupt_state_startup_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-managed-state-XXXXXX";
    char root[180], confirmed_path[220], candidate_path[220];
    uint8_t confirmed[32], candidate[32], zero[32];
    int rc = 1, i;

    if (!mkdtemp(dir))
        return fail("mkdtemp managed corrupt state");
    fill_digest(confirmed, 0x11);
    fill_digest(candidate, 0x77);
    memset(zero, 0, sizeof(zero));

    for (i = 0; i < 6; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        if (mkdir(root, 0755) < 0)
            goto out;
        if (i == 0) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 1, 0) < 0)
                goto out;
        } else if (i == 1) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 9, confirmed, zero, 0, 0) < 0)
                goto out;
        } else if (i == 2) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, candidate, 0, 0) < 0)
                goto out;
        } else if (i == 3) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_TESTING,
                                           1, 0, confirmed, candidate, 0, 0) < 0)
                goto out;
        } else if (i == 4) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_TESTING,
                                           1, 7, confirmed, confirmed, 0, 0) < 0)
                goto out;
        } else {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 0, 1) < 0)
                goto out;
        }
        if (managed_expect_degraded(root) < 0)
            goto out;
        snprintf(confirmed_path, sizeof(confirmed_path), "%s/worker.state", root);
        unlink(confirmed_path);
        rmdir(root);
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "managed_corrupt_state case=%d\n", i);
    for (i = 0; i < 6; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        snprintf(confirmed_path, sizeof(confirmed_path), "%s/worker.state", root);
        snprintf(candidate_path, sizeof(candidate_path), "%s/worker.state.tmp", root);
        unlink(confirmed_path);
        unlink(candidate_path);
        rmdir(root);
    }
    rmdir(dir);
    return rc;
}

static int managed_artifact_failure_startup_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-managed-artifact-XXXXXX";
    char root[180], workers[220], path[320], state[220];
    uint8_t confirmed[32], candidate[32], zero[32];
    int rc = 1, i;

    if (!mkdtemp(dir))
        return fail("mkdtemp managed artifact state");
    fill_digest(confirmed, 0x21);
    fill_digest(candidate, 0x91);
    memset(zero, 0, sizeof(zero));

    for (i = 0; i < 3; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        snprintf(workers, sizeof(workers), "%s/workers", root);
        if (mkdir(root, 0755) < 0 || mkdir(workers, 0755) < 0)
            goto out;
        if (i == 0) {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 0, 0) < 0)
                goto out;
        } else if (i == 1) {
            if (worker_artifact_path(workers, confirmed, path, sizeof(path)) < 0 ||
                write_small_executable(path, "wrong artifact\n") < 0 ||
                write_worker_state_fixture(root, LMUX_BOOT_UPDATE_STABLE,
                                           1, 0, confirmed, zero, 0, 0) < 0)
                goto out;
        } else {
            if (write_worker_state_fixture(root, LMUX_BOOT_UPDATE_TESTING,
                                           2, 77, confirmed, candidate, 0, 0) < 0)
                goto out;
        }
        if (managed_expect_degraded(root) < 0)
            goto out;
        snprintf(state, sizeof(state), "%s/worker.state", root);
        unlink(state);
        if (worker_artifact_path(workers, confirmed, path, sizeof(path)) == 0)
            unlink(path);
        if (worker_artifact_path(workers, candidate, path, sizeof(path)) == 0)
            unlink(path);
        rmdir(workers);
        rmdir(root);
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "managed_artifact_failure case=%d\n", i);
    for (i = 0; i < 3; i++) {
        snprintf(root, sizeof(root), "%s/root-%d", dir, i);
        snprintf(workers, sizeof(workers), "%s/workers", root);
        snprintf(state, sizeof(state), "%s/worker.state", root);
        unlink(state);
        if (worker_artifact_path(workers, confirmed, path, sizeof(path)) == 0)
            unlink(path);
        if (worker_artifact_path(workers, candidate, path, sizeof(path)) == 0)
            unlink(path);
        rmdir(workers);
        rmdir(root);
    }
    rmdir(dir);
    return rc;
}

static int outer_host_restart_same_physical_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-host-restart-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    int stable[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[4096];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp host restart same physical");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0)
        goto out;
    proxy = spawn_persistent_proxy(carrier, stable[0]);
    close(stable[0]);
    stable[0] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    sup = spawn_supervisor_fd(stable[1], 1);
    close(stable[1]);
    stable[1] = -1;
    if (sup < 0)
        goto out;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    host = spawn_host(carrier, control, pty_arg, file_arg);
    unsetenv("LMUX_SUP_IT_OUTER");
    if (host < 0)
        goto out;
    where = "initial host ready";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("host restart initial status", out);
        goto out;
    }
    stop_pid(host);
    host = -1;
    where = "restarted host ready same physical";
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    host = spawn_host(carrier, control, pty_arg, file_arg);
    unsetenv("LMUX_SUP_IT_OUTER");
    if (host < 0 ||
        wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("host restart status", out);
        goto out;
    }
    cfd = wait_connect(console, 12000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' HOST RESTART", "HOSTRESTART", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "host_restart_same_physical where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int outer_supervisor_restart_same_host_physical_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-target-restart-XXXXXX";
    char carrier[160], control[160], console[160], pty_arg[320], file_sock[160], file_arg[320];
    int stable[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[4096];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp supervisor restart same physical");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0)
        goto out;
    proxy = spawn_persistent_proxy(carrier, stable[0]);
    close(stable[0]);
    stable[0] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd(stable[1], control, pty_arg, file_arg, 1);
    close(stable[1]);
    stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    sup = spawn_supervisor_connect(carrier, 1);
    if (sup < 0)
        goto out;
    where = "initial supervisor ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("supervisor restart initial status", out);
        goto out;
    }
    stop_pid(sup);
    sup = -1;
    where = "restarted supervisor ready same host physical";
    sup = spawn_supervisor_connect(carrier, 1);
    if (sup < 0 ||
        wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("supervisor restart status", out);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "supervisor_restart_same_physical where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    unlink(carrier);
    rmdir(dir);
    return rc;
}

static int outer_first_contact_lost_hello_test(int target_first)
{
    char dir[] = "/tmp/lmux-supervisor-first-contact-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int stable[2] = {-1, -1};
    pid_t sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp first contact");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0)
        goto out;

    if (target_first) {
        where = "spawn target";
        sup = spawn_supervisor_fd(stable[1], 1);
        close(stable[1]);
        stable[1] = -1;
        if (sup < 0)
            goto out;
        where = "discard target initial hello";
        if (discard_one_cobs_record(stable[0], 5000) < 0)
            goto out;
        where = "spawn host";
        host = spawn_host_fd(stable[0], control, pty_arg, file_arg, 1);
        close(stable[0]);
        stable[0] = -1;
        if (host < 0)
            goto out;
    } else {
        where = "spawn host";
        host = spawn_host_fd(stable[0], control, pty_arg, file_arg, 1);
        close(stable[0]);
        stable[0] = -1;
        if (host < 0)
            goto out;
        where = "discard host initial hello";
        if (discard_one_cobs_record(stable[1], 5000) < 0)
            goto out;
        where = "spawn target";
        sup = spawn_supervisor_fd(stable[1], 1);
        close(stable[1]);
        stable[1] = -1;
        if (sup < 0)
            goto out;
    }

    where = "ready after lost first hello";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0) {
        dump_excerpt("first contact status", out);
        goto out;
    }
    where = "boot after lost first hello";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("first contact boot", out);
        goto out;
    }
    where = "console after lost first hello";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' FIRST CONTACT",
                         "FIRSTCONTACT", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_first_contact_lost_hello target_first=%d where=%s\n",
                target_first, where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_first_contact_truncated_hello_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-first-trunc-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp first trunc");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(note) < 0)
        goto out;
    proxy = spawn_truncate_first_hello_proxy(host_pair[0], target_pair[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;

    where = "spawn target";
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0)
        goto out;
    where = "truncated target hello";
    if (wait_fault_injected(note[0], 5000) < 0)
        goto out;
    where = "spawn host";
    host = spawn_host_fd(host_pair[1], control, pty_arg, file_arg, 1);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0)
        goto out;

    where = "ready after truncated hello";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 25000, out, sizeof(out)) < 0) {
        dump_excerpt("first trunc status", out);
        goto out;
    }
    where = "boot after truncated hello";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("first trunc boot", out);
        goto out;
    }
    where = "console after truncated hello";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' FIRST TRUNC",
                         "FIRSTTRUNC", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_first_contact_truncated_hello where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_first_contact_lost_ack_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-first-ack-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp first ack");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(note) < 0)
        goto out;
    proxy = spawn_drop_first_ack_proxy(host_pair[0], target_pair[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;

    where = "spawn target";
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0)
        goto out;
    where = "spawn host";
    host = spawn_host_fd(host_pair[1], control, pty_arg, file_arg, 1);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0)
        goto out;
    where = "dropped hello ack";
    if (wait_fault_injected(note[0], 5000) < 0)
        goto out;

    where = "ready after lost hello ack";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 25000, out, sizeof(out)) < 0) {
        dump_excerpt("first ack status", out);
        goto out;
    }
    where = "boot after lost hello ack";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("first ack boot", out);
        goto out;
    }
    where = "console after lost hello ack";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' FIRST ACK",
                         "FIRSTACK", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_first_contact_lost_ack where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_duplicate_hello_burst_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-hello-burst-XXXXXX";
    char control[160], console[160], file_sock[160], pty_arg[320], file_arg[320];
    int host_pair[2] = {-1, -1};
    int target_pair[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1;
    int cfd = -1, rc = 1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp hello burst");
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0 ||
        pipe(note) < 0)
        goto out;
    proxy = spawn_duplicate_first_target_hello_proxy(host_pair[0],
                                                     target_pair[0], note[1]);
    close(host_pair[0]); host_pair[0] = -1;
    close(target_pair[0]); target_pair[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0)
        goto out;

    where = "spawn target";
    sup = spawn_supervisor_fd(target_pair[1], 1);
    close(target_pair[1]); target_pair[1] = -1;
    if (sup < 0)
        goto out;
    where = "spawn host";
    host = spawn_host_fd(host_pair[1], control, pty_arg, file_arg, 1);
    close(host_pair[1]); host_pair[1] = -1;
    if (host < 0)
        goto out;
    where = "duplicated target hello";
    if (wait_fault_injected(note[0], 5000) < 0)
        goto out;

    where = "ready after duplicate hello burst";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_ready_layers(control, 1, 25000, out, sizeof(out)) < 0) {
        dump_excerpt("hello burst status", out);
        goto out;
    }
    where = "boot after duplicate hello burst";
    if (!boot_info_ok(control, out, sizeof(out))) {
        dump_excerpt("hello burst boot", out);
        goto out;
    }
    where = "console after duplicate hello burst";
    cfd = wait_connect(console, 10000);
    if (cfd < 0 ||
        shell_cmd_expect(cfd, "printf '%s%s\\n' HELLO BURST",
                         "HELLOBURST", out, sizeof(out)) < 0)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_duplicate_hello_burst where=%s\n", where);
    if (cfd >= 0)
        close(cfd);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    if (host_pair[0] >= 0) close(host_pair[0]);
    if (host_pair[1] >= 0) close(host_pair[1]);
    if (target_pair[0] >= 0) close(target_pair[0]);
    if (target_pair[1] >= 0) close(target_pair[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    unlink(console);
    unlink(file_sock);
    unlink(control);
    rmdir(dir);
    return rc;
}

static int outer_fault_recovery_test(enum outer_fault_kind kind,
                                     enum outer_fault_dir dir)
{
    char dirbuf[] = "/tmp/lmux-supervisor-fault-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char pty_arg[320], file_arg[320], out[8192];
    int stable[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1, worker = -1;
    int cfd = -1, rc = 1;
    unsigned long long old_local, old_peer, new_local, new_peer;
    unsigned long long old_crc, old_seq, new_crc, new_seq;
    unsigned long long worker_incarnation = 0, worker_incarnation_after = 0;
    long shell_pid = -1, shell_pid_after = -1;
    const char *where = "setup";

    if (!mkdtemp(dirbuf))
        return fail("mkdtemp outer fault");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dirbuf);
    snprintf(control, sizeof(control), "%s/control.sock", dirbuf);
    snprintf(console, sizeof(console), "%s/console1.sock", dirbuf);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dirbuf);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy(carrier, stable[0], kind, dir, arm[0], note[1]);
    close(stable[0]); stable[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd(stable[1], control, pty_arg, file_arg, 1);
    close(stable[1]); stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    sup = spawn_supervisor_connect(carrier, 1);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0) {
        dump_excerpt("fault initial status", out);
        goto out;
    }
    old_local = status_value(out, "local_conn=");
    old_peer = status_value(out, "peer_conn=");
    old_crc = status_value(out, "crc_errors=");
    old_seq = status_value(out, "seq_errors=");
    if (old_local == 0 || old_peer == 0)
        goto out;
    where = "initial boot status";
    if (run_lmux_boot_cli("worker-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK WORKER_STATUS"))
        goto out;
    worker_incarnation = status_value(out, "worker_incarnation=");
    where = "initial shell";
    cfd = wait_connect(console, 10000);
    if (cfd < 0)
        goto out;
    usleep(2000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' F PID \"$$\"",
                         "FPID:", out, sizeof(out)) < 0)
        goto out;
    shell_pid = parse_pid_marker(out, "FPID:");
    if (shell_pid <= 0)
        goto out;

    where = "arm fault";
    if (write_all(arm[1], "A", 1) < 0)
        goto out;
    if (kind == OUTER_FAULT_SEQ_GAP && dir == OUTER_FAULT_HOST_TO_TARGET) {
        char big[3072];
        memset(big, '#', sizeof(big));
        big[0] = ':';
        big[sizeof(big) - 1] = '\n';
        (void)write_all(cfd, big, sizeof(big));
    } else if (dir == OUTER_FAULT_HOST_TO_TARGET) {
        const char *cmd = "printf 'FAULT_H2T\\n'\n";
        (void)write_all(cfd, cmd, strlen(cmd));
    } else {
        const char *cmd = "printf 'FAULT_T2H\\n'\n";
        (void)write_all(cfd, cmd, strlen(cmd));
    }
    if (wait_fault_injected(note[0], 5000) < 0) {
        dump_excerpt("fault inject wait", out);
        goto out;
    }
    close(cfd);
    cfd = -1;

    where = "fault recovery";
    if (wait_ready_layers_after_outer_change(control, old_local, old_peer,
                                             25000, out, sizeof(out)) < 0) {
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    new_local = status_value(out, "local_conn=");
    new_peer = status_value(out, "peer_conn=");
    new_crc = status_value(out, "crc_errors=");
    new_seq = status_value(out, "seq_errors=");
    if (new_local == old_local || new_peer == old_peer) {
        fprintf(stderr, "outer conn unchanged old=(%llu,%llu) new=(%llu,%llu)\n",
                old_local, old_peer, new_local, new_peer);
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    if (kind == OUTER_FAULT_BAD_CRC && dir == OUTER_FAULT_TARGET_TO_HOST &&
        new_crc <= old_crc) {
        fprintf(stderr, "crc counter did not advance old=%llu new=%llu\n",
                old_crc, new_crc);
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    if (kind == OUTER_FAULT_SEQ_GAP && dir == OUTER_FAULT_TARGET_TO_HOST &&
        new_seq <= old_seq) {
        fprintf(stderr, "seq counter did not advance old=%llu new=%llu\n",
                old_seq, new_seq);
        dump_excerpt("fault recovery status", out);
        goto out;
    }
    if (strstr(out, "protocol_errors=0") == NULL ||
        strstr(out, "stream_errors=0") == NULL ||
        strstr(out, "stale=0") == NULL ||
        strstr(out, "pre_established_drops=0") == NULL) {
        dump_excerpt("fault recovery lmux counters", out);
        goto out;
    }
    if (wait_child_pid(sup, -1, 1000) != worker) {
        fprintf(stderr, "worker pid changed expected=%ld\n", (long)worker);
        goto out;
    }

    where = "post fault boot";
    if (!boot_info_ok(control, out, sizeof(out)) ||
        run_lmux_boot_cli("worker-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK WORKER_STATUS") ||
        !strstr(out, "worker_state=ACTIVE"))
        goto out;
    worker_incarnation_after = status_value(out, "worker_incarnation=");
    if (worker_incarnation_after != worker_incarnation)
        goto out;

    where = "post fault shell";
    cfd = wait_connect(console, 12000);
    if (cfd < 0)
        goto out;
    usleep(2000000);
    if (shell_cmd_expect(cfd, "printf '%s%s:%s\\n' F PID2 \"$$\"",
                         "FPID2:", out, sizeof(out)) < 0) {
        dump_excerpt("post fault shell output", out);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("post fault shell status", out);
        goto out;
    }
    shell_pid_after = parse_pid_marker(out, "FPID2:");
    if (shell_pid_after != shell_pid) {
        fprintf(stderr, "post fault shell pid before=%ld after=%ld\n",
                shell_pid, shell_pid_after);
        if (read_status(control, out, sizeof(out)) == 0)
            dump_excerpt("post fault shell pid status", out);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_fault kind=%d dir=%d where=%s\n", kind, dir, where);
    if (cfd >= 0)
        close(cfd);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    unlink(carrier);
    rmdir(dirbuf);
    return rc;
}

static int outer_upload_fault_test(enum outer_fault_kind kind)
{
    char dirbuf[] = "/tmp/lmux-supervisor-upload-fault-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], upload_src[160], upload_tmp[512], upload_final[512];
    char pty_arg[320], file_arg[320], out[8192], hex[65];
    int stable[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1, worker = -1;
    int rc = 1, upload_rc;
    unsigned long long old_local = 0, old_peer = 0;
    struct stat st;
    const char *where = "setup";
    const char *expect = NULL;

    if (!mkdtemp(dirbuf))
        return fail("mkdtemp upload fault");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dirbuf);
    snprintf(control, sizeof(control), "%s/control.sock", dirbuf);
    snprintf(console, sizeof(console), "%s/console1.sock", dirbuf);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dirbuf);
    snprintf(storage_root, sizeof(storage_root), "%s/custom-storage", dirbuf);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(upload_src, sizeof(upload_src), "%s/upload-worker.bin", dirbuf);
    snprintf(upload_tmp, sizeof(upload_tmp), "%s/.upload.tmp", workers);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy(carrier, stable[0], kind,
                              OUTER_FAULT_HOST_TO_TARGET,
                              arm[0], note[1]);
    close(stable[0]); stable[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd(stable[1], control, pty_arg, file_arg, 1);
    close(stable[1]); stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    if (mkdir(storage_root, 0755) < 0)
        goto out;
    sup = spawn_supervisor_connect_storage(carrier, 1, storage_root);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0)
        goto out;
    old_local = status_value(out, "local_conn=");
    old_peer = status_value(out, "peer_conn=");
    if (make_file(upload_src, 64 * 1024) < 0 ||
        sha_file_hex(upload_src, hex) < 0)
        goto out;
    if (snprintf(upload_final, sizeof(upload_final), "%s/%s.lmuxd",
                 workers, hex) <= 0 ||
        strlen(upload_final) >= sizeof(upload_final))
        goto out;

    where = "arm upload fault";
    if (write_all(arm[1], "A", 1) < 0)
        goto out;
    upload_rc = run_lmux_boot_upload(control, upload_src, out, sizeof(out));
    if (wait_fault_injected(note[0], 5000) < 0) {
        dump_excerpt("upload fault inject wait", out);
        goto out;
    }

    if (kind == OUTER_FAULT_BOOT_BAD_HASH)
        expect = "ERR BOOT BAD_HASH";
    else if (kind == OUTER_FAULT_BOOT_BAD_OFFSET)
        expect = "ERR BOOT OFFSET_MISMATCH";

    if (expect) {
        where = "semantic upload error";
        if (upload_rc == 0 || !strstr(out, expect)) {
            dump_excerpt("upload semantic fault", out);
            goto out;
        }
        if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0)
            goto out;
    } else {
        where = "outer upload fault recovery";
        if (upload_rc == 0) {
            dump_excerpt("upload outer fault unexpectedly ok", out);
            goto out;
        }
        if (wait_ready_layers_after_outer_change(control, old_local, old_peer,
                                                 25000, out, sizeof(out)) < 0) {
            dump_excerpt("upload outer fault recovery", out);
            goto out;
        }
    }

    where = "upload cleanup";
    if (wait_path_absent(upload_tmp, 5000) < 0 ||
        lstat(upload_final, &st) == 0)
        goto out;

    where = "upload after fault";
    if (run_lmux_boot_upload(control, upload_src, out, sizeof(out)) < 0 ||
        !strstr(out, "OK UPLOAD") ||
        !strstr(out, hex) ||
        lstat(upload_tmp, &st) == 0 ||
        lstat(upload_final, &st) < 0 ||
        files_equal(upload_src, upload_final) < 0) {
        dump_excerpt("upload after fault", out);
        goto out;
    }
    if (wait_child_pid(sup, -1, 1000) != worker)
        goto out;
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_upload_fault kind=%d where=%s\n", kind, where);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    unlink(upload_final);
    unlink(upload_tmp);
    rmdir(workers);
    rmdir(storage_root);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    unlink(carrier);
    rmdir(dirbuf);
    return rc;
}

static int outer_deploy_confirm_fault_test(enum outer_fault_kind kind)
{
    char dirbuf[] = "/tmp/lmux-supervisor-deploy-fault-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], deploy_src[160], deploy_final[512];
    char pty_arg[320], file_arg[320], out[8192], hex[65];
    int stable[2] = {-1, -1};
    int arm[2] = {-1, -1};
    int note[2] = {-1, -1};
    pid_t proxy = -1, sup = -1, host = -1, worker = -1, worker2 = -1;
    int rc = 1;
    enum outer_fault_dir dir;
    struct stat st;
    const char *where = "setup";

    if (!mkdtemp(dirbuf))
        return fail("mkdtemp deploy fault");
    snprintf(carrier, sizeof(carrier), "%s/proxy.sock", dirbuf);
    snprintf(control, sizeof(control), "%s/control.sock", dirbuf);
    snprintf(console, sizeof(console), "%s/console1.sock", dirbuf);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dirbuf);
    snprintf(storage_root, sizeof(storage_root), "%s/custom-storage", dirbuf);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(deploy_src, sizeof(deploy_src), "%s/deploy-worker.lmuxd", dirbuf);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    dir = kind == OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC ?
        OUTER_FAULT_TARGET_TO_HOST : OUTER_FAULT_HOST_TO_TARGET;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, stable) < 0 ||
        pipe(arm) < 0 || pipe(note) < 0)
        goto out;
    proxy = spawn_fault_proxy(carrier, stable[0], kind, dir, arm[0], note[1]);
    close(stable[0]); stable[0] = -1;
    close(arm[0]); arm[0] = -1;
    close(note[1]); note[1] = -1;
    if (proxy < 0 || wait_path_exists(carrier, 5000) < 0)
        goto out;
    host = spawn_host_fd(stable[1], control, pty_arg, file_arg, 1);
    close(stable[1]); stable[1] = -1;
    if (host < 0 || wait_path_exists(control, 5000) < 0)
        goto out;
    if (mkdir(storage_root, 0755) < 0)
        goto out;
    sup = spawn_supervisor_connect_storage(carrier, 1, storage_root);
    if (sup < 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    where = "initial ready";
    if (wait_ready_layers(control, 1, 12000, out, sizeof(out)) < 0)
        goto out;
    where = "prepare deploy candidate";
    if (copy_with_suffix("build/host/lmuxd", deploy_src,
                         kind == OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC ?
                         "LMUX_P51_CONFIRM_RESPONSE_FAULT\n" :
                         "LMUX_P51_CONFIRM_REQUEST_FAULT\n") < 0 ||
        sha_file_hex(deploy_src, hex) < 0)
        goto out;
    if (snprintf(deploy_final, sizeof(deploy_final), "%s/%s.lmuxd",
                 workers, hex) <= 0 ||
        strlen(deploy_final) >= sizeof(deploy_final))
        goto out;
    where = "arm deploy confirm fault";
    if (write_all(arm[1], "A", 1) < 0)
        goto out;
    if (run_lmux_deploy_wait_fault(control, deploy_src, 30000, note[0],
                                   out, sizeof(out)) < 0 ||
        !strstr(out, "OK DEPLOY") ||
        !strstr(out, hex)) {
        dump_excerpt("deploy confirm fault output", out);
        goto out;
    }
    where = "deploy confirm fault final status";
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0 || worker2 == worker)
        goto out;
    worker = worker2;
    worker2 = -1;
    if (wait_ready_layers(control, 1, 15000, out, sizeof(out)) < 0 ||
        run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=STABLE") ||
        !strstr(out, hex) ||
        lstat(deploy_final, &st) < 0) {
        dump_excerpt("deploy confirm fault status", out);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0)
        fprintf(stderr, "outer_deploy_confirm_fault kind=%d where=%s\n",
                kind, where);
    if (stable[0] >= 0) close(stable[0]);
    if (stable[1] >= 0) close(stable[1]);
    if (arm[0] >= 0) close(arm[0]);
    if (arm[1] >= 0) close(arm[1]);
    if (note[0] >= 0) close(note[0]);
    if (note[1] >= 0) close(note[1]);
    stop_pid(host);
    stop_pid(sup);
    stop_pid(proxy);
    unlink(deploy_final);
    unlink(deploy_src);
    rmdir(workers);
    rmdir(storage_root);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    unlink(carrier);
    rmdir(dirbuf);
    return rc;
}

static int supervisor_install_restart_shortcut_test(void)
{
    char dir[] = "/tmp/lmux-supervisor-install-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160];
    char storage_root[160], workers[220], active_dir[160], active_exe[220];
    char worker_b[220], worker_c[220], final_b[512], active_hex[65], b_hex[65], c_hex[65];
    char pty_arg[320], file_arg[320], out[8192], activation_id[32];
    const char *where = "setup";
    pid_t respawner = -1, sup = -1, worker = -1, worker2 = -1, sup2 = -1;
    pid_t host = -1;
    unsigned long long generation_before, generation_after;
    int rc = 1;

    if (!mkdtemp(dir))
        return fail("mkdtemp supervisor install");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(storage_root, sizeof(storage_root), "%s/storage", dir);
    snprintf(workers, sizeof(workers), "%s/workers", storage_root);
    snprintf(active_dir, sizeof(active_dir), "%s/active", dir);
    snprintf(active_exe, sizeof(active_exe), "%s/lmuxd", active_dir);
    snprintf(worker_b, sizeof(worker_b), "%s/worker-b.lmuxd", dir);
    snprintf(worker_c, sizeof(worker_c), "%s/worker-c.lmuxd", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);

    if (mkdir(storage_root, 0755) < 0 || mkdir(active_dir, 0755) < 0 ||
        copy_with_suffix("build/host/lmuxd", active_exe,
                         "LMUX_SUPERVISOR_INSTALL_A\n") < 0 ||
        copy_with_suffix("build/host/lmuxd", worker_b,
                         "LMUX_SUPERVISOR_INSTALL_B\n") < 0 ||
        copy_with_suffix("build/host/lmuxd", worker_c,
                         "LMUX_SUPERVISOR_INSTALL_C\n") < 0 ||
        sha_file_hex(active_exe, active_hex) < 0 ||
        sha_file_hex(worker_b, b_hex) < 0 ||
        sha_file_hex(worker_c, c_hex) < 0)
        goto out;
    if (strcmp(active_hex, b_hex) == 0)
        goto out;
    if (snprintf(final_b, sizeof(final_b), "%s/%s.lmuxd", workers, b_hex) <= 0 ||
        strlen(final_b) >= sizeof(final_b))
        goto out;

    respawner = spawn_supervisor_respawner(active_exe, carrier, storage_root);
    if (respawner <= 0)
        goto out;
    sup = wait_child_pid(respawner, -1, 10000);
    if (sup <= 0)
        goto out;
    worker = wait_child_pid(sup, -1, 10000);
    if (worker <= 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host < 0)
        goto out;
    if (wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0)
        goto out;

    where = "prepare stable worker B without supervisor install";
    if (run_lmux_boot_upload(control, worker_b, out, sizeof(out)) < 0 ||
        lstat(final_b, &(struct stat){0}) < 0)
        goto out;
    if (run_lmux_boot_activate(control, b_hex, 30000, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=TESTING") ||
        status_value(out, "activation_id=") == 0) {
        dump_excerpt("install shortcut activate", out);
        goto out;
    }
    snprintf(activation_id, sizeof(activation_id), "%llu",
             status_value(out, "activation_id="));
    worker2 = wait_child_pid(sup, worker, 10000);
    if (worker2 <= 0 || worker2 == worker)
        goto out;
    worker = worker2;
    worker2 = -1;
    if (wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0)
        goto out;
    if (run_lmux_boot_id("confirm", control, activation_id, out, sizeof(out)) < 0 ||
        run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=STABLE") || !strstr(out, b_hex)) {
        dump_excerpt("install shortcut stable", out);
        goto out;
    }
    generation_before = status_value(out, "generation=");
    if (sha_file_hex(active_exe, active_hex) < 0 || strcmp(active_hex, b_hex) == 0)
        goto out;

    where = "restart rejected before install";
    if (run_lmux_boot_sha("supervisor-restart", control, b_hex,
                          out, sizeof(out)) < 0 ||
        !strstr(out, "ERR BOOT")) {
        dump_excerpt("restart rejected before install", out);
        goto out;
    }

    where = "unconfirmed artifact install rejected";
    if (run_lmux_boot_upload(control, worker_c, out, sizeof(out)) < 0 ||
        run_lmux_boot_sha("supervisor-install", control, c_hex,
                          out, sizeof(out)) < 0 ||
        !strstr(out, "ERR BOOT WRONG_STATE"))
        goto out;

    where = "deploy restart shortcut installs and restarts";
    if (run_lmux_deploy_restart_only(control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK DEPLOY") || !strstr(out, b_hex)) {
        dump_excerpt("restart shortcut", out);
        goto out;
    }
    sup2 = wait_child_pid(respawner, sup, 10000);
    if (sup2 <= 0 || sup2 == sup)
        goto out;
    sup = sup2;
    sup2 = -1;
    if (sha_file_hex(active_exe, active_hex) < 0 || strcmp(active_hex, b_hex) != 0)
        goto out;
    if (wait_ready_layers(control, 1, 20000, out, sizeof(out)) < 0 ||
        run_lmux_boot_cli("update-status", control, out, sizeof(out)) < 0 ||
        !strstr(out, "phase=STABLE") || !strstr(out, b_hex)) {
        dump_excerpt("restart shortcut final", out);
        goto out;
    }
    generation_after = status_value(out, "generation=");
    if (generation_after != generation_before)
        goto out;

    where = "already installed restart shortcut";
    if (run_lmux_deploy_restart_only(control, out, sizeof(out)) < 0 ||
        !strstr(out, "OK DEPLOY")) {
        dump_excerpt("restart shortcut already", out);
        goto out;
    }
    sup2 = wait_child_pid(respawner, sup, 10000);
    if (sup2 <= 0 || sup2 == sup)
        goto out;
    rc = 0;
out:
    if (rc)
        fprintf(stderr, "supervisor_install_restart_shortcut where=%s\n", where);
    stop_pid(host);
    stop_pgrp(respawner);
    unlink(worker_b);
    unlink(worker_c);
    unlink(active_exe);
    unlink(final_b);
    rmdir(active_dir);
    rmdir(workers);
    rmdir(storage_root);
    unlink(carrier);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(dir);
    return rc;
}

static int private_exec_basic_test(void)
{
    char dir[] = "/tmp/lmux-private-exec-it-XXXXXX";
    char carrier[160], control[160], console[160], file_sock[160], storage[160];
    char pty_arg[360], file_arg[240];
    char out[32768];
    char req[4096];
    char self_path[256], self_hex[sizeof(self_path) * 2 + 1];
    char helper_hex[64], pgsig_hex[32], bp_hex[32], stderr_bp_hex[64];
    char stdin_delay_hex[64], marker_hex[1100], total_hex[32];
    char stdin_hex[3200];
    char marker[512], marker2[1024];
    char *big = NULL;
    ssize_t self_len;
    size_t bp_count = 0;
    pid_t sup = -1, host = -1;
    int rc = 1;
    uint64_t exec_id = 0, old_id = 0;
    uint64_t ids[V2_EXEC_MAX_COMPLETED + 3];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp private exec");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(storage, sizeof(storage), "%s/storage", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    if (mkdir(storage, 0700) < 0)
        goto out;
    self_len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (self_len <= 0)
        goto out;
    self_path[self_len] = 0;
    if (hex_encode_cstr(self_path, self_hex, sizeof(self_hex)) < 0 ||
        hex_encode_cstr("--exec-helper", helper_hex, sizeof(helper_hex)) < 0 ||
        hex_encode_cstr("pgsig", pgsig_hex, sizeof(pgsig_hex)) < 0 ||
        hex_encode_cstr("stdout-bp", bp_hex, sizeof(bp_hex)) < 0 ||
        hex_encode_cstr("stderr-bp", stderr_bp_hex, sizeof(stderr_bp_hex)) < 0 ||
        hex_encode_cstr("stdin-delay", stdin_delay_hex, sizeof(stdin_delay_hex)) < 0)
        goto out;
    big = malloc(320000);
    if (!big)
        goto out;

    sup = spawn_supervisor_with_storage(carrier, storage);
    if (sup <= 0)
        goto out;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0)
        goto out;

    where = "ready";
    if (wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0) {
        dump_excerpt("private exec status", out);
        goto out;
    }

    where = "true";
    if (exec_request_test(control, "EXEC START - 2f62696e2f74727565\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "exit=0") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec true", out);
        goto out;
    }

    where = "false";
    if (exec_request_test(control, "EXEC START - 2f62696e2f66616c7365\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "exit=1") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec false", out);
        goto out;
    }

    where = "exec failure";
    if (exec_request_test(control, "EXEC START - 2f6e6f2f737563682d6c6d75782d65786563\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=START_FAILED") ||
        !strstr(out, "fail=5") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec failure", out);
        goto out;
    }

    where = "stdio";
    if (exec_request_test(control,
                          "EXEC START - 2f62696e2f7368 2d63 7072696e746620455845435f4f55543b207072696e746620455845435f455252203e26323b20636174 --stdin 410042ff0a\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STDOUT_HEX 455845435f4f5554") ||
        !strstr(out, "STDERR_HEX 455845435f455252") ||
        !strstr(out, "410042ff0a") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "exit=0") ||
        !strstr(out, "stdout_eof=1") ||
        !strstr(out, "stderr_eof=1") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec stdio", out);
        goto out;
    }

    where = "release completed";
    if (parse_exec_id_line(out, &old_id) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC RELEASE %llu\n",
             (unsigned long long)old_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC RELEASED")) {
        dump_excerpt("private exec release", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)old_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "ERR EXEC remote status=2")) {
        dump_excerpt("private exec released status", out);
        goto out;
    }

    where = "detached reconnect attach";
    if (exec_request_test(control,
                          "EXEC START_DETACHED - 2f62696e2f7368 2d63 736c65657020313b207072696e746620504f5354\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "DETACHED") ||
        parse_exec_id_line(out, &exec_id) < 0) {
        dump_excerpt("private exec detached start", out);
        goto out;
    }
    stop_pid(host);
    host = -1;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec reconnect attach", out);
        goto out;
    }

    where = "competing attach and reconnect frees";
    if (exec_request_test(control,
                          "EXEC START_DETACHED - 2f62696e2f7368 2d63 736c65657020333b20657869742030\n",
                          out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0) {
        dump_excerpt("private exec conflict start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC ATTACH_CONFLICT %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC ATTACH_BUSY")) {
        dump_excerpt("private exec attach conflict", out);
        goto out;
    }
    stop_pid(host);
    host = -1;
    host = spawn_host(carrier, control, pty_arg, file_arg);
    if (host <= 0 ||
        wait_ready_layers(control, getenv("LMUX_SUP_IT_OUTER") ? 1 : 0,
                          15000, out, sizeof(out)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec reconnect after conflict", out);
        goto out;
    }

    where = "signal process group";
    snprintf(marker, sizeof(marker), "%s/pg-int", dir);
    if (mkdir(marker, 0700) < 0 ||
        hex_encode_cstr(marker, marker_hex, sizeof(marker_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s %s\n",
             self_hex, helper_hex, pgsig_hex, marker_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/leader-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/child-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0) {
        dump_excerpt("private exec signal start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC SIGNAL %llu INT\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=6")) {
        dump_excerpt("private exec signal", out);
        goto out;
    }
    snprintf(marker2, sizeof(marker2), "%s/leader-sigint", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(marker2, sizeof(marker2), "%s/child-sigint", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec signal drain", out);
        goto out;
    }

    where = "signal term and stale";
    snprintf(marker, sizeof(marker), "%s/pg-term", dir);
    if (mkdir(marker, 0700) < 0 ||
        hex_encode_cstr(marker, marker_hex, sizeof(marker_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s %s\n",
             self_hex, helper_hex, pgsig_hex, marker_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/leader-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0 ||
        snprintf(marker2, sizeof(marker2), "%s/child-ready", marker) <= 0 ||
        wait_marker(marker2, 3000) < 0) {
        dump_excerpt("private exec term start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC SIGNAL %llu TERM\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=6")) {
        dump_excerpt("private exec term", out);
        goto out;
    }
    snprintf(marker2, sizeof(marker2), "%s/leader-sigterm", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(marker2, sizeof(marker2), "%s/child-sigterm", marker);
    if (wait_marker(marker2, 3000) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec term drain", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC RELEASE %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC RELEASED")) {
        dump_excerpt("private exec term release", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC SIGNAL %llu TERM\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "ERR EXEC remote status=2")) {
        dump_excerpt("private exec stale signal", out);
        goto out;
    }

    where = "stdout backpressure";
    snprintf(marker, sizeof(marker), "%s/stdout-blocked", dir);
    if (hex_encode_cstr(marker, marker_hex, sizeof(marker_hex)) < 0 ||
        hex_encode_cstr("16384", total_hex, sizeof(total_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s %s %s\n",
             self_hex, helper_hex, bp_hex, marker_hex, total_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0 ||
        wait_marker(marker, 5000) < 0 ||
        read_size_marker(marker, &bp_count) < 0 ||
        bp_count == 0 || bp_count > 100000) {
        dump_excerpt("private exec stdout bp start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        (!strstr(out, "STATUS state=2") &&
         !strstr(out, "STATUS state=3"))) {
        dump_excerpt("private exec stdout bp status", out);
        goto out;
    }
    if (!boot_info_ok(control, out, sizeof(out)))
        goto out;
    snprintf(req, sizeof(req), "EXEC ATTACH %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, big, 320000) < 0 ||
        verify_stdout_hex_pattern(big, bp_count) < 0) {
        char diag[2048];
        fprintf(stderr, "stdout bp expected=%llu host_received=%llu\n",
                (unsigned long long)bp_count,
                (unsigned long long)count_stdout_hex_bytes(big));
        snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
                 (unsigned long long)exec_id);
        if (exec_request_test(control, req, diag, sizeof(diag)) == 0)
            dump_excerpt("private exec stdout bp post-status", diag);
        dump_excerpt("private exec stdout bp attach", big);
        goto out;
    }

    where = "stderr independent backpressure";
    snprintf(req, sizeof(req), "EXEC START_DETACHED - %s %s %s\n",
             self_hex, helper_hex, stderr_bp_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        parse_exec_id_line(out, &exec_id) < 0) {
        dump_excerpt("private exec stderr bp start", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC ATTACH_STDOUT_ONCE %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "5354444f55545f4f4b")) {
        dump_excerpt("private exec stderr bp stdout", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC RELEASE %llu\n",
             (unsigned long long)exec_id);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC RELEASED")) {
        dump_excerpt("private exec stderr bp release", out);
        goto out;
    }

    where = "stdin backpressure";
    if (hex_encode_pattern(1500, stdin_hex, sizeof(stdin_hex)) < 0 ||
        hex_encode_cstr("1500", total_hex, sizeof(total_hex)) < 0)
        goto out;
    snprintf(req, sizeof(req), "EXEC START - %s %s %s %s --stdin %s\n",
             self_hex, helper_hex, stdin_delay_hex, total_hex, stdin_hex);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "535444494e5f4f4b") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec stdin backpressure", out);
        goto out;
    }

    where = "stdin epipe";
    if (exec_request_test(control,
                          "EXEC START - 2f62696e2f7368 2d63 6578656320303c262d3b20657869742030 --stdin 00010203040506070809\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "OK EXEC") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec epipe", out);
        goto out;
    }

    where = "exit drain repeated";
    for (int rep = 0; rep < 5; rep++) {
        if (exec_request_test(control,
                              "EXEC START - 2f62696e2f7368 2d63 7072696e7466204f55545f5441494c3b207072696e7466204552525f5441494c203e26323b20657869742030\n",
                              out, sizeof(out)) < 0 ||
            !strstr(out, "4f55545f5441494c") ||
            !strstr(out, "4552525f5441494c") ||
            !strstr(out, "STATUS state=4") ||
            !strstr(out, "\nDONE\n")) {
            dump_excerpt("private exec repeated drain", out);
            goto out;
        }
    }

    where = "completed gc";
    for (int gi = 0; gi < V2_EXEC_MAX_COMPLETED + 2; gi++) {
        if (exec_request_test(control, "EXEC START - 2f62696e2f74727565\n",
                              out, sizeof(out)) < 0 ||
            parse_exec_id_line(out, &ids[gi]) < 0 ||
            (gi > 0 && ids[gi] <= ids[gi - 1]) ||
            !strstr(out, "STATUS state=4")) {
            dump_excerpt("private exec gc start", out);
            goto out;
        }
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)ids[0]);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "ERR EXEC remote status=2")) {
        dump_excerpt("private exec gc evicted", out);
        goto out;
    }
    snprintf(req, sizeof(req), "EXEC STATUS %llu\n",
             (unsigned long long)ids[V2_EXEC_MAX_COMPLETED + 1]);
    if (exec_request_test(control, req, out, sizeof(out)) < 0 ||
        !strstr(out, "STATUS state=4")) {
        dump_excerpt("private exec gc retained", out);
        goto out;
    }

    where = "exit drain tail";
    if (exec_request_test(control,
                          "EXEC START - 2f62696e2f7368 2d63 693d303b207768696c65205b202469202d6c74203830205d3b20646f207072696e746620303132333435363738396162636465663b20693d242828692b3129293b20646f6e65\n",
                          out, sizeof(out)) < 0 ||
        !strstr(out, "30313233343536373839616263646566") ||
        !strstr(out, "STATUS state=4") ||
        !strstr(out, "\nDONE\n")) {
        dump_excerpt("private exec exit drain", out);
        goto out;
    }

    rc = 0;
out:
    if (rc)
        fprintf(stderr, "private_exec_basic where=%s\n", where);
    stop_pid(host);
    stop_pid(sup);
    free(big);
    unlink(carrier);
    unlink(control);
    unlink(console);
    unlink(file_sock);
    rmdir(storage);
    rmdir(dir);
    return rc;
}

static char helper_marker_dir[512];
static int helper_is_child;

static void helper_write_marker(const char *name)
{
    char path[640];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", helper_marker_dir, name);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd >= 0) {
        (void)write(fd, "1\n", 2);
        close(fd);
    }
}

static void helper_signal_handler(int sig)
{
    if (sig == SIGINT)
        helper_write_marker(helper_is_child ? "child-sigint" : "leader-sigint");
    else if (sig == SIGTERM)
        helper_write_marker(helper_is_child ? "child-sigterm" : "leader-sigterm");
    _exit(128 + sig);
}

static int exec_helper_pgsig(const char *dir)
{
    pid_t child;
    if (strlen(dir) >= sizeof(helper_marker_dir))
        return 2;
    strcpy(helper_marker_dir, dir);
    signal(SIGINT, helper_signal_handler);
    signal(SIGTERM, helper_signal_handler);
    child = fork();
    if (child < 0)
        return 2;
    if (child == 0) {
        helper_is_child = 1;
        signal(SIGINT, helper_signal_handler);
        signal(SIGTERM, helper_signal_handler);
        helper_write_marker("child-ready");
        for (;;)
            pause();
    }
    helper_write_marker("leader-ready");
    for (;;)
        pause();
}

static int exec_helper_stdout_bp(const char *marker, size_t total)
{
    static const char pat[] = "0123456789abcdef";
    size_t pos = 0;
    int marked = 0;
#ifdef F_SETPIPE_SZ
    (void)fcntl(STDOUT_FILENO, F_SETPIPE_SZ, 4096);
#endif
    if (test_set_nonblock(STDOUT_FILENO) < 0)
        return 2;
    while (pos < total) {
        char buf[512];
        size_t i, n = sizeof(buf);
        ssize_t w;
        if (n > total - pos)
            n = total - pos;
        for (i = 0; i < n; i++)
            buf[i] = pat[(pos + i) & 15];
        w = write(STDOUT_FILENO, buf, n);
        if (w > 0) {
            pos += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!marked) {
                int fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0600);
                if (fd >= 0) {
                    char tmp[64];
                    int len = snprintf(tmp, sizeof(tmp), "%llu\n",
                                       (unsigned long long)pos);
                    if (len > 0)
                        (void)write(fd, tmp, (size_t)len);
                    close(fd);
                }
                marked = 1;
            }
            return 0;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return 3;
    }
    return marked ? 0 : 4;
}

static int exec_helper_stderr_bp(void)
{
    pid_t err_child, out_child;
    err_child = fork();
    if (err_child < 0)
        return 2;
    if (err_child == 0) {
        char buf[512];
        memset(buf, 'E', sizeof(buf));
        for (;;) {
            ssize_t w = write(STDERR_FILENO, buf, sizeof(buf));
            if (w < 0 && errno != EINTR)
                _exit(0);
        }
    }
    out_child = fork();
    if (out_child < 0)
        return 2;
    if (out_child == 0) {
        usleep(200000);
        (void)write(STDOUT_FILENO, "STDOUT_OK", 9);
        _exit(0);
    }
    for (;;)
        pause();
}

static int exec_helper_stdin_delay(size_t total)
{
    size_t got = 0;
    usleep(300000);
    while (got < total) {
        uint8_t buf[256];
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r > 0) {
            ssize_t i;
            for (i = 0; i < r; i++) {
                if (buf[i] != (uint8_t)((got + (size_t)i) & 255))
                    return 3;
            }
            got += (size_t)r;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        return 4;
    }
    if (got != total)
        return 5;
    if (read(STDIN_FILENO, &(uint8_t){0}, 1) != 0)
        return 6;
    (void)write(STDOUT_FILENO, "STDIN_OK", 8);
    return 0;
}

static int exec_helper_main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[2], "pgsig") == 0)
        return exec_helper_pgsig(argv[3]);
    if (argc >= 5 && strcmp(argv[2], "stdout-bp") == 0)
        return exec_helper_stdout_bp(argv[3], (size_t)strtoull(argv[4], NULL, 10));
    if (argc >= 3 && strcmp(argv[2], "stderr-bp") == 0)
        return exec_helper_stderr_bp();
    if (argc >= 4 && strcmp(argv[2], "stdin-delay") == 0)
        return exec_helper_stdin_delay((size_t)strtoull(argv[3], NULL, 10));
    return 2;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--exec-helper") == 0)
        return exec_helper_main(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    if (invalid_storage_root_test())
        return 1;
    if (pending_abandon_test(0) || pending_abandon_test(1))
        return 1;
    if (outer_host_restart_same_physical_test() ||
        outer_supervisor_restart_same_host_physical_test() ||
        outer_first_contact_lost_hello_test(1) ||
        outer_first_contact_lost_hello_test(0) ||
        outer_first_contact_truncated_hello_test() ||
        outer_first_contact_lost_ack_test() ||
        outer_duplicate_hello_burst_test())
        return 1;
    if (host_first_startup_test() || integration_test())
        return 1;
    setenv("LMUX_SUP_IT_OUTER", "1", 1);
    if (outer_boot_worker_backoff_test() ||
        managed_corrupt_state_startup_test() ||
        managed_artifact_failure_startup_test() ||
        outer_fault_recovery_test(OUTER_FAULT_BAD_CRC, OUTER_FAULT_TARGET_TO_HOST) ||
        outer_fault_recovery_test(OUTER_FAULT_SEQ_GAP, OUTER_FAULT_HOST_TO_TARGET) ||
        outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_HASH) ||
        outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_OFFSET) ||
        outer_upload_fault_test(OUTER_FAULT_BOOT_BAD_CRC) ||
        outer_deploy_confirm_fault_test(OUTER_FAULT_BOOT_CONFIRM_RESP_BAD_CRC) ||
        outer_deploy_confirm_fault_test(OUTER_FAULT_BOOT_CONFIRM_REQ_BAD_CRC) ||
        private_exec_basic_test() ||
        supervisor_install_restart_shortcut_test() ||
        host_first_startup_test() || integration_test())
        return 1;
    unsetenv("LMUX_SUP_IT_OUTER");
    puts("supervisor integration selftest ok");
    return 0;
}
