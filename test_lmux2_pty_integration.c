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
    char arg1[512], arg1_dup[512], arg1_conflict[512], arg2[512];
    pid_t target = -1, host = -1;
    int fd1 = -1, fd2 = -1, rc = 1;
    int host_fd_base = -1, target_fd_base = -1;
    long pid1 = -1, pid1_after = -1, pid2 = -1, pid1_respawn = -1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(c1, sizeof(c1), "%s/console1.sock", dir);
    snprintf(c2, sizeof(c2), "%s/console2.sock", dir);
    snprintf(c1_bad, sizeof(c1_bad), "%s/console1-bad.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(arg1, sizeof(arg1), "console1:%s:/bin/sh", c1);
    snprintf(arg1_dup, sizeof(arg1_dup), "console1:%s:/bin/sh", c1);
    snprintf(arg1_conflict, sizeof(arg1_conflict), "console1:%s:/bin/false", c1_bad);
    snprintf(arg2, sizeof(arg2), "console2:%s:/bin/sh", c2);

    target = spawn_target(carrier);
    if (target < 0)
        goto out;
    if (wait_path_exists(carrier, 8000) < 0)
        goto out;
    host = spawn_host(carrier, control, arg1, arg1_dup, arg1_conflict, arg2);
    if (host < 0)
        goto out;

    where = "console listeners";
    fd1 = wait_connect(c1, 10000);
    fd2 = wait_connect(c2, 10000);
    if (fd1 < 0 || fd2 < 0)
        goto out;
    if (access(c1_bad, F_OK) == 0)
        goto out;
    host_fd_base = count_fds(host);
    target_fd_base = count_fds(target);

    where = "initial shell pid/state";
    if (shell_cmd_expect(fd1, "LMUX_STATE=alive; printf 'P''ID1:%s\\n' \"$$\"", "PID1:", out, sizeof(out)) < 0) {
        dump_excerpt("initial console1", out);
        goto out;
    }
    pid1 = parse_pid_marker(out, "PID1:");
    if (pid1 <= 0)
        goto out;
    if (shell_cmd_expect(fd2, "printf 'P''ID2:%s\\n' \"$$\"", "PID2:", out, sizeof(out)) < 0)
        goto out;
    pid2 = parse_pid_marker(out, "PID2:");
    if (pid2 <= 0 || pid2 == pid1)
        goto out;

    where = "winsize isolation";
    if (send_winsize(control, "console1", 33, 101) < 0 ||
        send_winsize(control, "console2", 22, 77) < 0)
        goto out;
    if (shell_cmd_expect(fd1, "stty size", "33 101", out, sizeof(out)) < 0)
        goto out;
    if (shell_cmd_expect(fd2, "stty size", "22 77", out, sizeof(out)) < 0)
        goto out;

    where = "detach preserves shell";
    close(fd1);
    fd1 = wait_connect(c1, 5000);
    if (fd1 < 0)
        goto out;
    if (shell_cmd_expect(fd1, "printf 'P''ID1B:%s:%s\\n' \"$$\" \"$LMUX_STATE\"", "PID1B:", out, sizeof(out)) < 0)
        goto out;
    pid1_after = parse_pid_marker(out, "PID1B:");
    if (pid1_after != pid1 || !strstr(out, ":alive"))
        goto out;

    where = "console1 churn while console2 alive";
    for (int i = 0; i < 100; i++) {
        close(fd1);
        fd1 = wait_connect(c1, 5000);
        if (fd1 < 0 ||
            shell_cmd_expect(fd1, "printf 'C1''OK\\n'", "C1OK", out, sizeof(out)) < 0) {
            fprintf(stderr, "churn iteration failed: %d fd=%d\n", i, fd1);
            dump_excerpt("churn console1", out);
            goto out;
        }
    }
    if (shell_cmd_expect(fd2, "printf 'C2''OK:%s\\n' \"$$\"", "C2OK:", out, sizeof(out)) < 0 ||
        parse_pid_marker(out, "C2OK:") != pid2)
        goto out;
    usleep(300000);
    if (host_fd_base >= 0 && count_fds(host) != host_fd_base) {
        fprintf(stderr, "host fd count %d expected %d\n", count_fds(host), host_fd_base);
        goto out;
    }
    if (target_fd_base >= 0 && count_fds(target) != target_fd_base) {
        fprintf(stderr, "target fd count %d expected %d\n", count_fds(target), target_fd_base);
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
    rmdir(dir);
    return rc ? fail(where) : 0;
}

int main(void)
{
    if (integration_test())
        return 1;
    puts("lmux2 pty integration selftest ok");
    return 0;
}
