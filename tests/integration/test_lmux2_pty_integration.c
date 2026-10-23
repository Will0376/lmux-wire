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

static int fail(const char *msg)
{
    fprintf(stderr, "lmux2 pty integration failed: %s\n", msg);
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

static int write_text_file(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    int rc;
    if (fd < 0)
        return -1;
    rc = write_all(fd, text, strlen(text));
    if (close(fd) < 0)
        rc = -1;
    return rc;
}

static int write_fifo_when_reader(const char *path, const char *text,
                                  int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        int fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            int rc = write_all(fd, text, strlen(text));
            close(fd);
            return rc;
        }
        if (errno != ENXIO && errno != EINTR)
            return -1;
        usleep(10000);
    }
    return -1;
}

static int file_equals(const char *path, const char *want)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf));
    close(fd);
    return n == (ssize_t)strlen(want) &&
           memcmp(buf, want, (size_t)n) == 0 ? 0 : -1;
}

static int substring_count(const char *buf, const char *needle)
{
    int count = 0;
    size_t n = strlen(needle);
    const char *p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += n;
    }
    return count;
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

static int read_until_rolling(int fd, const char *needle, char *out, size_t cap,
                              int timeout_ms)
{
    size_t len = 0;
    size_t overlap = strlen(needle);
    long long end = now_ms() + timeout_ms;
    if (cap < 2)
        return -1;
    if (overlap >= cap)
        overlap = cap - 1;
    out[0] = 0;
    while (now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r;
            if (len + 1 >= cap) {
                memmove(out, out + len - overlap, overlap);
                len = overlap;
                out[len] = 0;
            }
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

static int shell_cmd_expect(int fd, const char *cmd, const char *needle, char *out, size_t cap)
{
    if (write_all(fd, cmd, strlen(cmd)) < 0 || write_all(fd, "\n", 1) < 0)
        return -1;
    return read_until(fd, needle, out, cap, 5000);
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
    if (n > 160)
        n = 160;
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)buf[i];
        if (ch >= 32 && ch < 127 && ch != '\\')
            fputc(ch, stderr);
        else
            fprintf(stderr, "\\x%02x", ch);
    }
    fputc('\n', stderr);
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

static void dump_fds(pid_t pid)
{
    char path[128], linkpath[512], target[256];
    DIR *dir;
    struct dirent *de;
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    dir = opendir(path);
    if (!dir)
        return;
    fprintf(stderr, "fds for %ld:\n", (long)pid);
    while ((de = readdir(dir)) != NULL) {
        ssize_t n;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(linkpath, sizeof(linkpath), "%s/%s", path, de->d_name);
        n = readlink(linkpath, target, sizeof(target) - 1);
        if (n >= 0) {
            target[n] = 0;
            fprintf(stderr, "  %s -> %s\n", de->d_name, target);
        }
    }
    closedir(dir);
}

static int read_status_contains(const char *control, const char *want, int timeout_ms)
{
    char req[] = "STATUS\n";
    char last[4096];
    long long end = now_ms() + timeout_ms;
    last[0] = 0;
    while (now_ms() < end) {
        char buf[4096];
        size_t len = 0;
        int fd = wait_connect(control, 500);
        if (fd < 0) {
            usleep(50000);
            continue;
        }
        if (write_all(fd, req, strlen(req)) < 0) {
            close(fd);
            usleep(50000);
            continue;
        }
        buf[0] = 0;
        while (now_ms() < end && len + 1 < sizeof(buf)) {
            int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
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
                }
            }
            if (re & (POLLHUP | POLLERR | POLLNVAL))
                break;
        }
        close(fd);
        snprintf(last, sizeof(last), "%s", buf);
        usleep(50000);
    }
    fprintf(stderr, "status missing '%s':\n%s\n", want, last);
    return -1;
}

static int read_status_snapshot(const char *control, char *buf, size_t cap, int timeout_ms)
{
    char req[] = "STATUS\n";
    size_t len = 0;
    long long end = now_ms() + timeout_ms;
    int fd = wait_connect(control, timeout_ms);
    if (fd < 0)
        return -1;
    if (write_all(fd, req, strlen(req)) < 0) {
        close(fd);
        return -1;
    }
    if (cap)
        buf[0] = 0;
    while (now_ms() < end && len + 1 < cap) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        if (re & POLLIN) {
            ssize_t r = read(fd, buf + len, cap - len - 1);
            if (r > 0) {
                len += (size_t)r;
                buf[len] = 0;
            } else if (r == 0) {
                close(fd);
                return 0;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(fd);
                return -1;
            }
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL)) {
            close(fd);
            return len ? 0 : -1;
        }
    }
    close(fd);
    return len ? 0 : -1;
}

static int status_u64_after(const char *buf, const char *key, unsigned long long *out)
{
    const char *p = strstr(buf, key);
    char *endp;
    if (!p)
        return -1;
    p += strlen(key);
    errno = 0;
    *out = strtoull(p, &endp, 10);
    return errno == 0 && endp != p ? 0 : -1;
}

static int status_service_u64(const char *buf, const char *service,
                              const char *key, unsigned long long *out)
{
    const char *p = strstr(buf, "\nservices:\n");
    const char *end;
    size_t n = strlen(service);
    if (!p)
        return -1;
    end = strstr(p, "\nstreams:\n");
    while ((p = strstr(p, "\n  ")) != NULL) {
        if (end && p >= end)
            break;
        p += 3;
        if (strncmp(p, service, n) == 0 && p[n] == ' ') {
            const char *eol = strchr(p, '\n');
            const char *k = strstr(p, key);
            if (!k || (eol && k > eol))
                return -1;
            return status_u64_after(k, key, out);
        }
    }
    return -1;
}

static int status_service_last_stream(const char *buf, const char *service,
                                      unsigned long long *out)
{
    return status_service_u64(buf, service, "last_stream=", out);
}

static int wait_console_detached(const char *control,
                                 unsigned long long stream, int timeout_ms)
{
    char buf[16384], id[64], terminal[96], last[16384];
    long long end = now_ms() + timeout_ms;
    last[0] = 0;
    snprintf(id, sizeof(id), "  id=%llu state=", stream);
    snprintf(terminal, sizeof(terminal), "  id=%llu state=TERMINAL ", stream);
    while (now_ms() < end) {
        if (read_status_snapshot(control, buf, sizeof(buf), 1000) == 0) {
            const char *streams = strstr(buf, "\nstreams:\n");
            if (streams && (!strstr(streams, id) || strstr(streams, terminal)))
                return 0;
            snprintf(last, sizeof(last), "%s", buf);
        }
        usleep(10000);
    }
    fprintf(stderr, "stream %llu did not detach:\n%s\n", stream, last);
    return -1;
}

static int wait_console_stream_after(const char *control, const char *service,
                                     unsigned long long want_conn,
                                     unsigned long long prev_stream,
                                     int timeout_ms,
                                     unsigned long long *out_stream)
{
    char buf[16384], last[16384];
    unsigned long long last_conn = 0, last_stream = 0;
    int last_conn_ok = 0, last_stream_ok = 0;
    long long end = now_ms() + timeout_ms;
    last[0] = 0;
    while (now_ms() < end) {
        unsigned long long conn = 0, stream = 0;
        if (read_status_snapshot(control, buf, sizeof(buf), 1000) == 0 &&
            status_u64_after(buf, "local_conn=", &conn) == 0 &&
            status_service_last_stream(buf, service, &stream) == 0 &&
            conn == want_conn && stream > prev_stream) {
            if (out_stream)
                *out_stream = stream;
            return 0;
        }
        last_conn = conn;
        last_stream = stream;
        last_conn_ok = status_u64_after(buf, "local_conn=", &conn) == 0;
        last_stream_ok = status_service_last_stream(buf, service, &stream) == 0;
        if (last_conn_ok)
            last_conn = conn;
        if (last_stream_ok)
            last_stream = stream;
        snprintf(last, sizeof(last), "%s", buf);
        usleep(50000);
    }
    fprintf(stderr, "status missing %s local_conn=%llu stream_after=%llu parsed_conn=%s/%llu parsed_stream=%s/%llu:\n%s\n",
            service, want_conn, prev_stream,
            last_conn_ok ? "ok" : "bad", last_conn,
            last_stream_ok ? "ok" : "bad", last_stream, last);
    return -1;
}

static int reconnect_console_expect(const char *path, const char *control,
                                    const char *service,
                                    unsigned long long want_conn,
                                    unsigned long long *stream,
                                    const char *cmd, const char *needle,
                                    char *out, size_t cap, int *out_fd,
                                    int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    if (cap)
        out[0] = 0;
    while (now_ms() < end) {
        int fd;
        int rem = (int)(end - now_ms());
        unsigned long long next_stream = *stream;
        if (rem <= 0)
            break;
        fd = wait_connect(path, rem < 500 ? rem : 500);
        if (fd < 0)
            continue;
        rem = (int)(end - now_ms());
        if (rem <= 0) {
            close(fd);
            break;
        }
        if (wait_console_stream_after(control, service, want_conn,
                                      *stream, rem, &next_stream) == 0) {
            *stream = next_stream;
            if (cap)
                out[0] = 0;
            if (shell_cmd_expect(fd, cmd, needle, out, cap) == 0) {
                *out_fd = fd;
                return 0;
            }
        }
        close(fd);
    }
    return -1;
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

static pid_t spawn_target(const char *carrier)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "target",
                  "--carrier-listen", carrier, "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--role", "target",
              "--carrier-listen", carrier, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host(const char *carrier, const char *control,
                        const char *pty1, const char *pty1_dup,
                        const char *pty1_conflict, const char *pty2)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--pty", pty1, "--pty", pty1_dup,
                  "--pty", pty1_conflict, "--pty", pty2,
                  "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--role", "host",
              "--carrier-connect", carrier, "--control", control,
              "--pty", pty1, "--pty", pty1_dup,
              "--pty", pty1_conflict, "--pty", pty2, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int send_winsize(const char *control, const char *name, unsigned rows, unsigned cols)
{
    char line[128], got[64];
    int fd = wait_connect(control, 3000);
    if (fd < 0)
        return -1;
    snprintf(line, sizeof(line), "WINSIZE %s %u %u\n", name, rows, cols);
    if (write_all(fd, line, strlen(line)) < 0 ||
        read_until(fd, "OK", got, sizeof(got), 3000) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int integration_test(void)
{
    char dir[] = "/tmp/lmux2-pty-it-XXXXXX";
    char carrier[160], c1[160], c2[160], c1_bad[160], control[160];
    char shell1[160], shell2[160], partial_flag[160], detached_fifo[160], detached_done[160];
    char boundary_fifo[160], boundary_started[160], builtin_release[160];
    char overflow_started[160], overflow_done[160], raw_file[160];
    char shell_script[512];
    char arg1[512], arg1_dup[512], arg1_conflict[512], arg2[512];
    pid_t target = -1, host = -1;
    int fd1 = -1, fd2 = -1, rc = 1;
    int host_fd_base = -1, target_fd_base = -1;
    unsigned long long initial_conn = 0;
    unsigned long long console1_stream = 0;
    unsigned long long console2_stream = 0;
    long pid1 = -1, pid1_after = -1, pid2 = -1, pid1_respawn = -1;
    char out[65536], status[16384], cmd[1024];
    int have_busybox;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(c1, sizeof(c1), "%s/console1.sock", dir);
    snprintf(c2, sizeof(c2), "%s/console2.sock", dir);
    snprintf(c1_bad, sizeof(c1_bad), "%s/console1-bad.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(shell1, sizeof(shell1), "%s/test-shell", dir);
    snprintf(shell2, sizeof(shell2), "%s/test-shell-busybox", dir);
    snprintf(partial_flag, sizeof(partial_flag), "%s/partial-ran", dir);
    snprintf(detached_fifo, sizeof(detached_fifo), "%s/detached.fifo", dir);
    snprintf(detached_done, sizeof(detached_done), "%s/detached.done", dir);
    snprintf(boundary_fifo, sizeof(boundary_fifo), "%s/boundary.fifo", dir);
    snprintf(boundary_started, sizeof(boundary_started), "%s/boundary.started", dir);
    snprintf(builtin_release, sizeof(builtin_release), "%s/builtin.release", dir);
    snprintf(overflow_started, sizeof(overflow_started), "%s/overflow.started", dir);
    snprintf(overflow_done, sizeof(overflow_done), "%s/overflow.done", dir);
    snprintf(raw_file, sizeof(raw_file), "%s/raw.bytes", dir);
    have_busybox = access("/usr/bin/busybox", X_OK) == 0;
    snprintf(shell_script, sizeof(shell_script),
             "#!/bin/sh\nPS1='LMUX_TEST_PROMPT> '\nexport PS1\nexec %s sh -i\n",
             have_busybox ? "/usr/bin/busybox" : "/bin/sh");
    if (write_text_file(shell1, shell_script, 0700) < 0 ||
        write_text_file(shell2, shell_script, 0700) < 0 ||
        mkfifo(detached_fifo, 0600) < 0 ||
        mkfifo(boundary_fifo, 0600) < 0)
        goto out;
    snprintf(arg1, sizeof(arg1), "console1:%s:%s", c1, shell1);
    snprintf(arg1_dup, sizeof(arg1_dup), "console1:%s:%s", c1, shell1);
    snprintf(arg1_conflict, sizeof(arg1_conflict), "console1:%s:/bin/false", c1_bad);
    snprintf(arg2, sizeof(arg2), "console2:%s:%s", c2, shell2);

    host = spawn_host(carrier, control, arg1, arg1_dup, arg1_conflict, arg2);
    if (host < 0)
        goto out;

    where = "host console listeners before peer";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_path_exists(c1, 5000) < 0 ||
        wait_path_exists(c2, 5000) < 0 ||
        access(c1_bad, F_OK) == 0)
        goto out;

    target = spawn_target(carrier);
    if (target < 0)
        goto out;
    if (wait_path_exists(carrier, 8000) < 0)
        goto out;
    where = "service reconciliation";
    if (read_status_contains(control, "console1 id=1", 10000) < 0 ||
        read_status_contains(control, "console2 id=2", 10000) < 0)
        goto out;

    where = "console1 listener";
    fd1 = wait_connect(c1, 10000);
    if (fd1 < 0)
        goto out;
    if (access(c1_bad, F_OK) == 0)
        goto out;

    where = "first attach shows idle prompt";
    if (read_until_rolling(fd1, "LMUX_TEST_PROMPT> ",
                           out, sizeof(out), 5000) < 0) {
        dump_excerpt("first prompt", out);
        goto out;
    }

    where = "initial shell pid/state";
    if (shell_cmd_expect(fd1, "echo LMUX2_READY", "LMUX2_READY", out, sizeof(out)) < 0) {
        dump_excerpt("initial console1 ready", out);
        goto out;
    }
    if (shell_cmd_expect(fd1, "LMUX_STATE=alive; printf 'P''ID1:%s\\n' \"$$\"", "PID1:", out, sizeof(out)) < 0) {
        dump_excerpt("initial console1", out);
        goto out;
    }
    pid1 = parse_pid_marker(out, "PID1:");
    if (pid1 <= 0)
        goto out;
    if (read_status_snapshot(control, status, sizeof(status), 3000) < 0 ||
        status_u64_after(status, "local_conn=", &initial_conn) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn, 0, 5000,
                                  &console1_stream) < 0)
        goto out;

    where = "partial command is reset on attach";
    snprintf(cmd, sizeof(cmd), "printf UX_PARTIAL_EXECUTED > %s", partial_flag);
    if (write_all(fd1, cmd, strlen(cmd)) < 0 ||
        read_until(fd1, "UX_PARTIAL_EXECUTED", out, sizeof(out), 5000) < 0 ||
        access(partial_flag, F_OK) == 0) {
        dump_excerpt("partial before detach", out);
        goto out;
    }
    close(fd1);
    fd1 = -1;
    if (wait_console_detached(control, console1_stream, 5000) < 0 ||
        access(partial_flag, F_OK) == 0) {
        fprintf(stderr, "partial: detach or premature execution\n");
        goto out;
    }
    if ((fd1 = wait_connect(c1, 5000)) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        read_until(fd1, "LMUX_TEST_PROMPT> ", out, sizeof(out), 5000) < 0 ||
        strstr(out, "UX_PARTIAL_EXECUTED") != NULL ||
        access(partial_flag, F_OK) == 0 ||
        write_all(fd1, "\n", 1) < 0 ||
        read_until(fd1, "LMUX_TEST_PROMPT> ", out, sizeof(out), 5000) < 0 ||
        access(partial_flag, F_OK) == 0) {
        dump_excerpt("partial reset", out);
        goto out;
    }

    where = "new command after reset executes once";
    if (shell_cmd_expect(fd1, "printf 'AFTER_''RESET_ONCE\\n'",
                         "AFTER_RESET_ONCE\r\nLMUX_TEST_PROMPT> ",
                         out, sizeof(out)) < 0 ||
        substring_count(out, "AFTER_RESET_ONCE\r\n") != 1)
        goto out;

    where = "completed command is not replayed";
    close(fd1);
    fd1 = -1;
    if (wait_console_detached(control, console1_stream, 5000) < 0 ||
        (fd1 = wait_connect(c1, 5000)) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        write_all(fd1, "printf 'CURRENT_''LINE_ONLY\\n'\n",
                  strlen("printf 'CURRENT_''LINE_ONLY\\n'\n")) < 0 ||
        read_until(fd1, "\r\nCURRENT_LINE_ONLY\r\nLMUX_TEST_PROMPT> ",
                   out, sizeof(out), 5000) < 0 ||
        strstr(out, "LMUX2_READY") != NULL ||
        strstr(out, "LMUX_TEST_PROMPT> ") == NULL)
        goto out;

    where = "detached transcript is not replayed";
    snprintf(cmd, sizeof(cmd),
             "read UX_DETACHED_VALUE < %s; printf '%%s\\n' \"$UX_DETACHED_VALUE\"; : > %s\n",
             detached_fifo, detached_done);
    if (write_all(fd1, cmd, strlen(cmd)) < 0 ||
        read_until(fd1, "UX_DETACHED_VALUE", out, sizeof(out), 5000) < 0)
        goto out;
    close(fd1);
    fd1 = -1;
    if (wait_console_detached(control, console1_stream, 5000) < 0 ||
        write_fifo_when_reader(detached_fifo, "DETACHED_OUTPUT_TOKEN\n", 5000) < 0 ||
        wait_path_exists(detached_done, 5000) < 0 ||
        (fd1 = wait_connect(c1, 5000)) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        shell_cmd_expect(fd1, "printf 'DETACHED_CURRENT\\n'",
                         "DETACHED_CURRENT", out, sizeof(out)) < 0 ||
        strstr(out, "DETACHED_OUTPUT_TOKEN") != NULL)
        goto out;

    where = "active builtin survives attach and live output is exact";
    snprintf(cmd, sizeof(cmd),
             ": > %s; read UX_BOUNDARY_VALUE < %s; printf '%%s\\n' \"$UX_BOUNDARY_VALUE\"; "
             "printf '%%s_END\\n' \"$UX_BOUNDARY_VALUE\"\n",
             boundary_started, boundary_fifo);
    if (write_all(fd1, cmd, strlen(cmd)) < 0 ||
        wait_path_exists(boundary_started, 5000) < 0)
        goto out;
    close(fd1);
    fd1 = -1;
    if (wait_console_detached(control, console1_stream, 5000) < 0 ||
        (fd1 = wait_connect(c1, 5000)) < 0 ||
        write_fifo_when_reader(boundary_fifo, "BOUNDARY_OUTPUT_TOKEN\n", 5000) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        read_until(fd1, "BOUNDARY_OUTPUT_TOKEN_END", out, sizeof(out), 5000) < 0 ||
        substring_count(out, "BOUNDARY_OUTPUT_TOKEN\r\n") != 1) {
        dump_excerpt("builtin attach boundary", out);
        goto out;
    }

    where = "running shell builtin is not interrupted by attach";
    snprintf(cmd, sizeof(cmd),
             "printf 'BUILTIN_STARTED\\n'; "
             "while [ ! -e %s ]; do :; done; printf 'BUILTIN_SURVIVED\\n'\n",
             builtin_release);
    if (write_all(fd1, cmd, strlen(cmd)) < 0 ||
        read_until(fd1, "BUILTIN_STARTED\r\n", out, sizeof(out), 5000) < 0)
        goto out;
    close(fd1);
    fd1 = -1;
    if (wait_console_detached(control, console1_stream, 5000) < 0 ||
        (fd1 = wait_connect(c1, 5000)) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        write_text_file(builtin_release, "go\n", 0600) < 0 ||
        read_until(fd1, "BUILTIN_SURVIVED\r\nLMUX_TEST_PROMPT> ",
                   out, sizeof(out), 5000) < 0)
        goto out;

    where = "bounded detached output does not become scrollback";
    snprintf(cmd, sizeof(cmd),
             ": > %s; read UX_OVERFLOW_GO < %s; printf PTY_HISTORY_HEAD; "
             "dd if=/dev/zero bs=20000 count=1 2>/dev/null | tr '\\000' X; "
             "printf PTY_HISTORY_TAIL\\n; : > %s\n",
             overflow_started, boundary_fifo, overflow_done);
    if (write_all(fd1, cmd, strlen(cmd)) < 0 ||
        wait_path_exists(overflow_started, 5000) < 0)
        goto out;
    close(fd1);
    fd1 = -1;
    if (wait_console_detached(control, console1_stream, 5000) < 0) {
        fprintf(stderr, "overflow: stream did not detach\n");
        goto out;
    }
    if (write_fifo_when_reader(boundary_fifo, "go\n", 5000) < 0) {
        fprintf(stderr, "overflow: producer did not unblock\n");
        goto out;
    }
    if (wait_path_exists(overflow_done, 10000) < 0) {
        fprintf(stderr, "overflow: producer did not finish\n");
        fd1 = wait_connect(c1, 2000);
        if (fd1 >= 0) {
            if (read_until(fd1, "PTY_HISTORY_TAIL", out, sizeof(out), 5000) < 0)
                dump_excerpt("overflow stalled replay", out);
            else
                fprintf(stderr, "overflow: attaching released producer flag=%s len=%zu\n",
                        access(overflow_done, F_OK) == 0 ? "yes" : "no",
                        strlen(out));
        }
        goto out;
    }
    fd1 = wait_connect(c1, 5000);
    if (fd1 < 0) {
        fprintf(stderr, "overflow: reconnect failed\n");
        goto out;
    }
    if (wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0) {
        fprintf(stderr, "overflow: stream did not reattach\n");
        goto out;
    }
    if (shell_cmd_expect(fd1, "printf 'OVERFLOW_CURRENT\\n'",
                         "OVERFLOW_CURRENT", out, sizeof(out)) < 0 ||
        strstr(out, "PTY_HISTORY_HEAD") != NULL ||
        strstr(out, "PTY_HISTORY_TAIL") != NULL || strlen(out) > 4096) {
        fprintf(stderr, "overflow: replayed transcript head=%s tail=%s len=%zu\n",
                strstr(out, "PTY_HISTORY_HEAD") ? "yes" : "no",
                strstr(out, "PTY_HISTORY_TAIL") ? "yes" : "no", strlen(out));
        goto out;
    }

    where = "ansi raw mode receives no attach input";
    snprintf(cmd, sizeof(cmd),
             "stty raw -echo; printf '\\033[2J\\033[2;1HRAW_MODE_TOKEN\\033[?25h'; "
             "dd bs=1 count=3 of=%s 2>/dev/null; stty sane; "
             "printf '\\r\\nRAW_MODE_DONE\\r\\n'\n", raw_file);
    if (write_all(fd1, cmd, strlen(cmd)) < 0 ||
        read_until(fd1, "\033[2J\033[2;1HRAW_MODE_TOKEN\033[?25h",
                   out, sizeof(out), 5000) < 0)
        goto out;
    close(fd1);
    fd1 = -1;
    if (wait_console_detached(control, console1_stream, 5000) < 0 ||
        (fd1 = wait_connect(c1, 5000)) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        write_all(fd1, "abc", 3) < 0 ||
        read_until(fd1, "RAW_MODE_DONE", out, sizeof(out), 5000) < 0 ||
        strstr(out, "RAW_MODE_TOKEN") != NULL ||
        file_equals(raw_file, "abc") < 0)
        goto out;

    where = "detach preserves shell";
    close(fd1);
    fd1 = wait_connect(c1, 5000);
    if (fd1 < 0)
        goto out;
    if (wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0)
        goto out;
    if (shell_cmd_expect(fd1, "printf 'P''ID1B:%s:%s\\n' \"$$\" \"$LMUX_STATE\"", "PID1B:", out, sizeof(out)) < 0)
        goto out;
    pid1_after = parse_pid_marker(out, "PID1B:");
    if (pid1_after != pid1 || !strstr(out, ":alive"))
        goto out;

    where = "monotonic console stream ids";
    close(fd1);
    fd1 = wait_connect(c1, 5000);
    if (fd1 < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        shell_cmd_expect(fd1, "printf 'P''ID1E:%s:%s\\n' \"$$\" \"$LMUX_STATE\"", "PID1E:", out, sizeof(out)) < 0 ||
        parse_pid_marker(out, "PID1E:") != pid1 || !strstr(out, ":alive"))
        goto out;
    close(fd1);
    fd1 = wait_connect(c1, 5000);
    if (fd1 < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        shell_cmd_expect(fd1, "printf 'P''ID1F:%s:%s\\n' \"$$\" \"$LMUX_STATE\"", "PID1F:", out, sizeof(out)) < 0 ||
        parse_pid_marker(out, "PID1F:") != pid1 || !strstr(out, ":alive"))
        goto out;

    where = "console2 listener";
    fd2 = wait_connect(c2, 10000);
    if (fd2 < 0 ||
        wait_console_stream_after(control, "console2", initial_conn, 0, 5000,
                                  &console2_stream) < 0)
        goto out;
    if (shell_cmd_expect(fd2, "printf 'P''ID2:%s\\n' \"$$\"", "PID2:", out, sizeof(out)) < 0)
        goto out;
    pid2 = parse_pid_marker(out, "PID2:");
    if (pid2 <= 0 || pid2 == pid1)
        goto out;

    where = "continuous producer starts";
    if (write_all(fd1, "yes PTY_SPAM\n", strlen("yes PTY_SPAM\n")) < 0)
        goto out;
    if (read_until(fd1, "\r\nPTY_SPAM\r\n", out, sizeof(out), 5000) < 0) {
        dump_excerpt("continuous producer start", out);
        goto out;
    }
    close(fd1);
    fd1 = -1;
    where = "continuous producer detaches";
    if (wait_console_detached(control, console1_stream, 5000) < 0)
        goto out;
    where = "detached continuous output preserves poll fairness";
    if (shell_cmd_expect(fd2, "printf 'PTY_FAIRNESS_OK\\n'",
                         "PTY_FAIRNESS_OK", out, sizeof(out)) < 0) {
        dump_excerpt("console2 fairness", out);
        goto out;
    }
    where = "continuous producer reattaches";
    if ((fd1 = wait_connect(c1, 5000)) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn,
                                  console1_stream, 5000,
                                  &console1_stream) < 0 ||
        read_until(fd1, "PTY_SPAM\r\n", out, sizeof(out), 5000) < 0 ||
        write_all(fd1, "\003", 1) < 0)
        goto out;
    where = "continuous producer interrupt reaches prompt";
    if (read_until_rolling(fd1, "LMUX_TEST_PROMPT> ",
                           out, sizeof(out), 5000) < 0) {
        dump_excerpt("continuous producer interrupt", out);
        goto out;
    }

    host_fd_base = count_fds(host);
    target_fd_base = count_fds(target);

    where = "winsize isolation";
    if (send_winsize(control, "console1", 33, 101) < 0 ||
        send_winsize(control, "console2", 22, 77) < 0)
        goto out;
    if (shell_cmd_expect(fd1, "stty size", "33 101", out, sizeof(out)) < 0)
        goto out;
    if (shell_cmd_expect(fd2, "stty size", "22 77", out, sizeof(out)) < 0)
        goto out;

    where = "console1 churn while console2 alive";
    for (int i = 0; i < 100; i++) {
        char churn_cmd[96], churn_marker[48], churn_done[96], previous_marker[48];
        snprintf(churn_marker, sizeof(churn_marker), "C1OK_%03d", i);
        snprintf(churn_done, sizeof(churn_done),
                 "%s\r\nLMUX_TEST_PROMPT> ", churn_marker);
        if (i > 0)
            snprintf(previous_marker, sizeof(previous_marker),
                     "C1OK_%03d", i - 1);
        snprintf(churn_cmd, sizeof(churn_cmd),
                 "printf 'C1OK_''%03d\\n'", i);
        close(fd1);
        fd1 = -1;
        if (reconnect_console_expect(c1, control, "console1", initial_conn,
                                     &console1_stream,
                                     churn_cmd, churn_done,
                                     out, sizeof(out), &fd1, 5000) < 0) {
            fprintf(stderr, "churn iteration failed: %d fd=%d\n", i, fd1);
            dump_excerpt("churn console1", out);
            goto out;
        }
        if ((i > 0 && strstr(out, previous_marker) != NULL) ||
            strlen(out) > 1024) {
            fprintf(stderr, "churn replayed old transcript at iteration %d\n", i);
            dump_excerpt("churn transcript", out);
            goto out;
        }
    }
    if (shell_cmd_expect(fd1, "printf 'P''ID1CHURN:%s\\n' \"$$\"",
                         "PID1CHURN:", out, sizeof(out)) < 0 ||
        parse_pid_marker(out, "PID1CHURN:") != pid1)
        goto out;
    if (shell_cmd_expect(fd2, "printf 'C2''OK:%s\\n' \"$$\"", "C2OK:", out, sizeof(out)) < 0 ||
        parse_pid_marker(out, "C2OK:") != pid2)
        goto out;
    usleep(300000);
    if (host_fd_base >= 0 && count_fds(host) != host_fd_base) {
        fprintf(stderr, "host fd count %d expected %d\n", count_fds(host), host_fd_base);
        dump_fds(host);
        goto out;
    }
    if (target_fd_base >= 0 && count_fds(target) != target_fd_base) {
        fprintf(stderr, "target fd count %d expected %d\n", count_fds(target), target_fd_base);
        dump_fds(target);
        goto out;
    }

    where = "binary control byte input";
    if (write_all(fd1, "\003", 1) < 0)
        goto out;
    usleep(100000);
    if (write_all(fd1, "printf 'AFTER''CTRL\\n'\n",
                  strlen("printf 'AFTER''CTRL\\n'\n")) < 0 ||
        read_until(fd1, "AFTERCTRL", out, sizeof(out), 5000) < 0)
        goto out;

    where = "target-only daemon restart";
    stop_child(target);
    target = -1;
    (void)wait_fd_event(fd1, POLLHUP | POLLERR | POLLNVAL, 5000);
    if (fd1 >= 0) {
        close(fd1);
        fd1 = -1;
    }
    if (fd2 >= 0) {
        close(fd2);
        fd2 = -1;
    }
    if (read_status_contains(control, "carrier: down", 7000) < 0)
        goto out;
    fd1 = wait_connect(c1, 5000);
    if (fd1 < 0 ||
        write_all(fd1, "echo TARGET_RESTART_PENDING\n",
                  strlen("echo TARGET_RESTART_PENDING\n")) < 0)
        goto out;
    target = spawn_target(carrier);
    if (target < 0 || wait_path_exists(carrier, 8000) < 0)
        goto out;
    if (read_until(fd1, "TARGET_RESTART_PENDING", out, sizeof(out), 12000) < 0) {
        dump_excerpt("target restart pending console1", out);
        goto out;
    }
    if (read_status_snapshot(control, status, sizeof(status), 5000) < 0 ||
        status_u64_after(status, "local_conn=", &initial_conn) < 0 ||
        wait_console_stream_after(control, "console1", initial_conn, 0, 8000,
                                  &console1_stream) < 0)
        goto out;
    if (shell_cmd_expect(fd1, "LMUX_STATE=alive; printf 'P''ID1R:%s\\n' \"$$\"", "PID1R:", out, sizeof(out)) < 0)
        goto out;
    pid1_after = parse_pid_marker(out, "PID1R:");
    if (pid1_after <= 0 || pid1_after == pid1)
        goto out;
    pid1 = pid1_after;
    fd2 = wait_connect(c2, 10000);
    if (fd2 < 0 ||
        shell_cmd_expect(fd2, "printf 'P''ID2R:%s\\n' \"$$\"", "PID2R:", out, sizeof(out)) < 0)
        goto out;
    pid1_after = parse_pid_marker(out, "PID2R:");
    if (pid1_after <= 0 || pid1_after == pid2 || pid1_after == pid1)
        goto out;
    pid2 = pid1_after;
    host_fd_base = count_fds(host);
    target_fd_base = count_fds(target);

    where = "physical reconnect preserves shell";
    stop_child(host);
    host = -1;
    close(fd1); fd1 = -1;
    close(fd2); fd2 = -1;
    host = spawn_host(carrier, control, arg1, arg1_dup, arg1_conflict, arg2);
    if (host < 0)
        goto out;
    fd1 = wait_connect(c1, 10000);
    fd2 = wait_connect(c2, 10000);
    if (fd1 < 0 || fd2 < 0)
        goto out;
    if (shell_cmd_expect(fd1, "printf 'P''ID1C:%s:%s\\n' \"$$\" \"$LMUX_STATE\"", "PID1C:", out, sizeof(out)) < 0)
        goto out;
    pid1_after = parse_pid_marker(out, "PID1C:");
    if (pid1_after != pid1 || !strstr(out, ":alive"))
        goto out;
    if (shell_cmd_expect(fd2, "printf 'C2''POST:%s\\n' \"$$\"", "C2POST:", out, sizeof(out)) < 0 ||
        parse_pid_marker(out, "C2POST:") != pid2)
        goto out;
    host_fd_base = count_fds(host);
    target_fd_base = count_fds(target);

    where = "supervised respawn";
    if (write_all(fd1, "exit\n", 5) < 0)
        goto out;
    {
        int ev = wait_fd_event(fd1, POLLHUP | POLLERR | POLLNVAL, 3000);
        if (!(ev & (POLLHUP | POLLERR | POLLNVAL))) {
            fprintf(stderr, "respawn: no HUP, events=0x%x\n", ev);
            goto out;
        }
    }
    close(fd1);
    fd1 = -1;
    usleep(1500000);
    fd1 = wait_connect(c1, 5000);
    if (fd1 < 0 ||
        shell_cmd_expect(fd1, "printf 'P''ID1D:%s\\n' \"$$\"", "PID1D:", out, sizeof(out)) < 0) {
        fprintf(stderr, "respawn: reconnect/read failed fd=%d\n", fd1);
        dump_excerpt("respawn console1", out);
        goto out;
    }
    pid1_respawn = parse_pid_marker(out, "PID1D:");
    if (pid1_respawn <= 0 || pid1_respawn == pid1) {
        fprintf(stderr, "respawn: pid old=%ld new=%ld\n", pid1, pid1_respawn);
        goto out;
    }
    if (shell_cmd_expect(fd2, "printf 'C2''SURVIVE:%s\\n' \"$$\"", "C2SURVIVE:", out, sizeof(out)) < 0 ||
        parse_pid_marker(out, "C2SURVIVE:") != pid2)
        goto out;

    where = "fd baseline";
    usleep(300000);
    if (host_fd_base >= 0 && count_fds(host) != host_fd_base)
        goto out;
    if (target_fd_base >= 0 && count_fds(target) != target_fd_base)
        goto out;
    rc = 0;

out:
    if (rc)
        fprintf(stderr, "where: %s\n", where);
    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    stop_child(host);
    stop_child(target);
    unlink(carrier); unlink(c1); unlink(c2); unlink(c1_bad); unlink(control);
    unlink(shell1); unlink(shell2); unlink(partial_flag); unlink(detached_fifo); unlink(detached_done);
    unlink(boundary_fifo); unlink(boundary_started); unlink(overflow_started); unlink(overflow_done); unlink(raw_file);
    rmdir(dir);
    return rc ? fail(where) : 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (integration_test())
        return 1;
    puts("lmux2 pty integration selftest ok");
    return 0;
}
