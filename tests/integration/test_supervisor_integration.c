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

static int shell_cmd_expect(int fd, const char *cmd, const char *needle,
                            char *out, size_t cap)
{
    return write_all(fd, cmd, strlen(cmd)) < 0 ||
           write_all(fd, "\n", 1) < 0 ||
           read_until(fd, needle, out, cap, 7000) < 0 ? -1 : 0;
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

static pid_t spawn_supervisor(const char *carrier)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--supervisor",
                  "--carrier-listen", carrier, "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--supervisor",
              "--carrier-listen", carrier, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_host(const char *carrier, const char *control,
                        const char *pty1, const char *file1)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--pty", pty1, "--file", file1, "--verbose", (char *)NULL);
        execl("build/host/lmuxd", "lmuxd", "--role", "host",
              "--carrier-connect", carrier, "--control", control,
              "--pty", pty1, "--file", file1, (char *)NULL);
        _exit(127);
    }
    return pid;
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
    char pty_arg[320], file_arg[320];
    char src[160], target[160], got[160];
    pid_t sup = -1, host = -1, worker = -1, worker2 = -1;
    int cfd = -1, rc = 1;
    long shell_pid = -1, shell_pid_after = -1, shell_pid_new = -1, shell_pid_sup_restart = -1;
    char out[8192];
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(console, sizeof(console), "%s/console1.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(pty_arg, sizeof(pty_arg), "console1:%s:/bin/sh", console);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    snprintf(src, sizeof(src), "%s/src.bin", dir);
    snprintf(target, sizeof(target), "%s/target.bin", dir);
    snprintf(got, sizeof(got), "%s/got.bin", dir);

    sup = spawn_supervisor(carrier);
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
    where = "host-first console";
    sleep(3);
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

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (host_first_startup_test() || integration_test())
        return 1;
    puts("supervisor integration selftest ok");
    return 0;
}
