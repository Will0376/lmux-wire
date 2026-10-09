#include "lxf.h"
#include "lmux.h"

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
    fprintf(stderr, "lmux2 file integration failed: %s\n", msg);
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

static int read_exact(int fd, void *buf, size_t len, int timeout_ms)
{
    unsigned char *p = buf;
    long long end = now_ms() + timeout_ms;
    while (len && now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        ssize_t r;
        if (!(re & POLLIN)) {
            if (re & (POLLHUP | POLLERR | POLLNVAL))
                return -1;
            continue;
        }
        r = read(fd, p, len);
        if (r > 0) {
            p += r;
            len -= (size_t)r;
        } else if (r == 0) {
            return -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
    }
    return len == 0 ? 0 : -1;
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

static int parse_conn_ids(const char *status, unsigned long long *local,
                          unsigned long long *peer)
{
    const char *p = strstr(status, "connection:");
    if (!p)
        return -1;
    p = strstr(p, "local_conn=");
    if (!p)
        return -1;
    *local = strtoull(p + 11, NULL, 10);
    p = strstr(p, "peer_conn=");
    if (!p)
        return -1;
    *peer = strtoull(p + 10, NULL, 10);
    return 0;
}

static int status_stream_progress(const char *status, unsigned long long *tx,
                                  unsigned long long *peer_limit)
{
    const char *p = status;
    int found = 0;
    *tx = 0;
    *peer_limit = 0;
    while ((p = strstr(p, "  id=")) != NULL) {
        const char *txp = strstr(p, " tx=");
        const char *plp = strstr(p, " peer_limit=");
        const char *next = strstr(p + 1, "\n  id=");
        if (txp && plp && (!next || txp < next) && (!next || plp < next)) {
            unsigned long long cur_tx = strtoull(txp + 4, NULL, 10);
            if (!found || cur_tx > *tx) {
                *tx = cur_tx;
                *peer_limit = strtoull(plp + 12, NULL, 10);
                found = 1;
            }
        }
        p++;
    }
    return found ? 0 : -1;
}

static int send_record_frag(int fd, uint8_t type, const uint8_t *payload,
                            uint32_t len, size_t frag)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t out_len, off = 0;
    if (lxf_encode_record(type, 0, payload, len, rec, sizeof(rec), &out_len) < 0)
        return -1;
    if (frag == 0)
        frag = out_len;
    while (off < out_len) {
        size_t n = out_len - off;
        if (n > frag)
            n = frag;
        if (write_all(fd, rec + off, n) < 0)
            return -1;
        off += n;
    }
    return 0;
}

static int recv_record(int fd, uint8_t *type, uint8_t *payload, uint32_t *len)
{
    uint8_t hdr[LXF_HEADER_LEN], flags;
    if (read_exact(fd, hdr, sizeof(hdr), 10000) < 0 ||
        lxf_decode_header(hdr, sizeof(hdr), type, &flags, len) < 0)
        return -1;
    (void)flags;
    if (*len > LXF_MAX_PAYLOAD)
        return -1;
    return read_exact(fd, payload, *len, 10000);
}

static void fill_pattern(uint8_t *p, size_t len, unsigned seed)
{
    size_t i;
    for (i = 0; i < len; i++)
        p[i] = (uint8_t)((i * 33u + seed + (i >> 3)) & 0xffu);
}

static int read_file_alloc(const char *path, uint8_t **out, size_t *len)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    uint8_t *p;
    size_t off = 0;
    if (fd < 0 || fstat(fd, &st) < 0 || st.st_size < 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    p = malloc((size_t)st.st_size + 1);
    if (!p) {
        close(fd);
        return -1;
    }
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, p + off, (size_t)st.st_size - off);
        if (r > 0)
            off += (size_t)r;
        else if (r == 0)
            break;
        else if (errno != EINTR) {
            free(p);
            close(fd);
            return -1;
        }
    }
    close(fd);
    *out = p;
    *len = off;
    return 0;
}

static int put_data(const char *sock, const char *target, const uint8_t *data,
                    size_t len, int bad_hash, size_t frag)
{
    int fd = wait_connect(sock, 5000);
    uint8_t payload[LXF_MAX_PAYLOAD], got[LXF_MAX_PAYLOAD], digest[32], type;
    uint32_t rlen;
    struct sha256_ctx sha;
    size_t off = 0;
    if (fd < 0 || strlen(target) + 1 > sizeof(payload) - 16)
        return -1;
    lmux_put_le32(payload, 0644);
    lmux_put_le64(payload + 4, (uint64_t)len);
    lmux_put_le32(payload + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)payload + 16, target);
    if (send_record_frag(fd, LXF_T_PUT_REQUEST, payload,
                         (uint32_t)(16 + strlen(target) + 1), frag) < 0 ||
        recv_record(fd, &type, got, &rlen) < 0 || type != LXF_T_OK) {
        fprintf(stderr, "put %s initial response type=%u len=%u errno=%d\n",
                target, type, rlen, errno);
        close(fd);
        return -1;
    }
    sha256_init(&sha);
    while (off < len) {
        size_t n = len - off;
        if (n > LXF_MAX_PAYLOAD)
            n = LXF_MAX_PAYLOAD;
        sha256_update(&sha, data + off, n);
        if (send_record_frag(fd, LXF_T_DATA, data + off, (uint32_t)n, frag) < 0) {
            close(fd);
            return -1;
        }
        off += n;
    }
    sha256_final(&sha, digest);
    if (bad_hash)
        digest[0] ^= 0x55;
    lmux_put_le64(payload, (uint64_t)len);
    memcpy(payload + 8, digest, 32);
    if (send_record_frag(fd, LXF_T_DONE, payload, 40, frag) < 0 ||
        recv_record(fd, &type, got, &rlen) < 0) {
        fprintf(stderr, "put %s completion read failed errno=%d\n", target, errno);
        close(fd);
        return -1;
    }
    close(fd);
    if (bad_hash)
        return type == LXF_T_ERROR ? 0 : -1;
    return type == LXF_T_OK && rlen >= 40 && lmux_get_le64(got) == (uint64_t)len ? 0 : -1;
}

static int get_data(const char *sock, const char *target, uint8_t **out, size_t *out_len,
                    size_t frag)
{
    int fd = wait_connect(sock, 5000);
    uint8_t payload[LXF_MAX_PAYLOAD], got[LXF_MAX_PAYLOAD], type, digest[32], rdigest[32];
    uint32_t len;
    uint64_t expected, count = 0, done_count;
    uint8_t *buf = NULL;
    size_t cap = 0;
    struct sha256_ctx sha;
    if (fd < 0 || strlen(target) + 1 > sizeof(payload))
        return -1;
    strcpy((char *)payload, target);
    if (send_record_frag(fd, LXF_T_GET_REQUEST, payload,
                         (uint32_t)(strlen(target) + 1), frag) < 0 ||
        recv_record(fd, &type, got, &len) < 0 || type != LXF_T_OK || len < 16) {
        close(fd);
        return -1;
    }
    expected = lmux_get_le64(got + 4);
    cap = (size_t)expected + 1;
    buf = malloc(cap ? cap : 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    sha256_init(&sha);
    for (;;) {
        if (recv_record(fd, &type, got, &len) < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (type == LXF_T_DATA) {
            if (count + len > expected) {
                free(buf);
                close(fd);
                return -1;
            }
            memcpy(buf + count, got, len);
            sha256_update(&sha, got, len);
            count += len;
        } else if (type == LXF_T_DONE) {
            if (len < 40) {
                free(buf);
                close(fd);
                return -1;
            }
            done_count = lmux_get_le64(got);
            sha256_final(&sha, digest);
            memcpy(rdigest, got + 8, 32);
            close(fd);
            if (done_count != count || count != expected || memcmp(digest, rdigest, 32) != 0) {
                free(buf);
                return -1;
            }
            *out = buf;
            *out_len = (size_t)count;
            return 0;
        } else {
            free(buf);
            close(fd);
            return -1;
        }
    }
}

static int shell_cmd_expect(int fd, const char *cmd, const char *needle)
{
    char buf[4096];
    size_t len = 0;
    long long end = now_ms() + 5000;
    if (write_all(fd, cmd, strlen(cmd)) < 0 || write_all(fd, "\n", 1) < 0)
        return -1;
    while (now_ms() < end) {
        int re = wait_fd_event(fd, POLLIN | POLLHUP | POLLERR | POLLNVAL, 100);
        ssize_t r;
        if (re & POLLIN) {
            if (len + 1 >= sizeof(buf))
                return -1;
            r = read(fd, buf + len, sizeof(buf) - len - 1);
            if (r > 0) {
                len += (size_t)r;
                buf[len] = 0;
                if (strstr(buf, needle))
                    return 0;
            } else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                return -1;
            }
        }
        if (re & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
    }
    return -1;
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

static int count_temp_files(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL)
        if (strstr(de->d_name, ".lmux-tmp-"))
            n++;
    closedir(d);
    return n;
}

static int wait_temp_count(const char *dir, int want, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    int n = -1;
    while (now_ms() < end) {
        n = count_temp_files(dir);
        if (n == want)
            return 0;
        usleep(20000);
    }
    fprintf(stderr, "temp count in %s got=%d want=%d\n", dir, n, want);
    return -1;
}

static int write_file_data(const char *path, const uint8_t *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (write_all(fd, data, len) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int file_equals_data(const char *path, const uint8_t *data, size_t len)
{
    uint8_t *got = NULL;
    size_t got_len = 0;
    int ok;
    if (read_file_alloc(path, &got, &got_len) < 0)
        return -1;
    ok = got_len == len && memcmp(got, data, len) == 0;
    free(got);
    return ok ? 0 : -1;
}

static int wait_file_equals_data(const char *path, const uint8_t *data,
                                 size_t len, int timeout_ms)
{
    long long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (file_equals_data(path, data, len) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int expect_absent(const char *path)
{
    return access(path, F_OK) < 0 && errno == ENOENT ? 0 : -1;
}

static int put_partial_disconnect(const char *sock, const char *target,
                                  const uint8_t *data, size_t total_len,
                                  size_t send_len)
{
    int fd = wait_connect(sock, 5000);
    uint8_t payload[LXF_MAX_PAYLOAD], reply[LXF_MAX_PAYLOAD], type;
    uint32_t rlen;
    if (fd < 0 || strlen(target) + 1 > sizeof(payload) - 16)
        return -1;
    if (send_len > LXF_MAX_PAYLOAD)
        send_len = LXF_MAX_PAYLOAD;
    lmux_put_le32(payload, 0644);
    lmux_put_le64(payload + 4, (uint64_t)total_len);
    lmux_put_le32(payload + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)payload + 16, target);
    if (send_record_frag(fd, LXF_T_PUT_REQUEST, payload,
                         (uint32_t)(16 + strlen(target) + 1), 9) < 0 ||
        recv_record(fd, &type, reply, &rlen) < 0 || type != LXF_T_OK ||
        send_record_frag(fd, LXF_T_DATA, data, (uint32_t)send_len, 13) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int put_commit_disconnect_before_reply(const char *sock, const char *target,
                                              const uint8_t *data, size_t len)
{
    int fd = wait_connect(sock, 5000);
    uint8_t payload[LXF_MAX_PAYLOAD], reply[LXF_MAX_PAYLOAD], digest[32], type;
    uint32_t rlen;
    struct sha256_ctx sha;
    size_t off = 0;
    if (fd < 0 || strlen(target) + 1 > sizeof(payload) - 16)
        return -1;
    lmux_put_le32(payload, 0644);
    lmux_put_le64(payload + 4, (uint64_t)len);
    lmux_put_le32(payload + 12, LXF_F_SIZE_KNOWN);
    strcpy((char *)payload + 16, target);
    if (send_record_frag(fd, LXF_T_PUT_REQUEST, payload,
                         (uint32_t)(16 + strlen(target) + 1), 17) < 0 ||
        recv_record(fd, &type, reply, &rlen) < 0 || type != LXF_T_OK) {
        close(fd);
        return -1;
    }
    sha256_init(&sha);
    while (off < len) {
        size_t n = len - off;
        if (n > LXF_MAX_PAYLOAD)
            n = LXF_MAX_PAYLOAD;
        sha256_update(&sha, data + off, n);
        if (send_record_frag(fd, LXF_T_DATA, data + off, (uint32_t)n, 31) < 0) {
            close(fd);
            return -1;
        }
        off += n;
    }
    sha256_final(&sha, digest);
    lmux_put_le64(payload, (uint64_t)len);
    memcpy(payload + 8, digest, 32);
    if (send_record_frag(fd, LXF_T_DONE, payload, 40, 19) < 0) {
        close(fd);
        return -1;
    }
    if (wait_file_equals_data(target, data, len, 5000) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int get_partial_disconnect(const char *sock, const char *target)
{
    int fd = wait_connect(sock, 5000);
    uint8_t payload[LXF_MAX_PAYLOAD], got[LXF_MAX_PAYLOAD], type;
    uint32_t len;
    if (fd < 0 || strlen(target) + 1 > sizeof(payload))
        return -1;
    strcpy((char *)payload, target);
    if (send_record_frag(fd, LXF_T_GET_REQUEST, payload,
                         (uint32_t)(strlen(target) + 1), 11) < 0 ||
        recv_record(fd, &type, got, &len) < 0 || type != LXF_T_OK ||
        recv_record(fd, &type, got, &len) < 0 || type != LXF_T_DATA) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
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

static int run_lmux_cli(char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO)
                close(dn);
        }
        execv("build/host/lmux", argv);
        _exit(127);
    }
    if (pid > 0) {
        int st;
        if (waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0)
            return 0;
    }
    return -1;
}

static int monitored_put(const char *control, const char *sock, const char *target,
                         const uint8_t *data, size_t len)
{
    char status[8192];
    unsigned long long local0 = 0, peer0 = 0, local = 0, peer = 0;
    int crossed_210225 = 0, crossed_1m = 0, peer_limit_grew = 0;
    int saw_open = 0, saw_lost = 0;
    pid_t pid;
    long long end;

    if (read_status_snapshot(control, status, sizeof(status), 5000) < 0 ||
        parse_conn_ids(status, &local0, &peer0) < 0 ||
        local0 == 0 || peer0 == 0) {
        fprintf(stderr, "monitored_put: initial status failed\n%s\n", status);
        return -1;
    }

    pid = fork();
    if (pid == 0)
        _exit(put_data(sock, target, data, len, 0, 257) == 0 ? 0 : 1);
    if (pid <= 0)
        return -1;

    end = now_ms() + 30000;
    while (now_ms() < end) {
        int st;
        if (read_status_snapshot(control, status, sizeof(status), 1000) == 0) {
            unsigned long long tx = 0, peer_limit = 0;
            if (strstr(status, "lost=0") == NULL)
                saw_lost = 1;
            if (parse_conn_ids(status, &local, &peer) == 0 &&
                (local != local0 || peer != peer0))
                saw_lost = 1;
            if (status_stream_progress(status, &tx, &peer_limit) == 0) {
                saw_open = 1;
                if (tx > 210225)
                    crossed_210225 = 1;
                if (tx > 1024 * 1024)
                    crossed_1m = 1;
                if (peer_limit > 210225)
                    peer_limit_grew = 1;
            }
        }
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
                return fprintf(stderr, "monitored_put: child failed\n"), -1;
            break;
        }
        usleep(10000);
    }
    if (now_ms() >= end) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return fprintf(stderr, "monitored_put: child timed out\n"), -1;
    }
    if (read_status_snapshot(control, status, sizeof(status), 5000) < 0 ||
        parse_conn_ids(status, &local, &peer) < 0 ||
        local != local0 || peer != peer0 ||
        strstr(status, "lost=0") == NULL) {
        fprintf(stderr, "monitored_put: final status failed local0=%llu peer0=%llu\n%s\n",
                local0, peer0, status);
        return -1;
    }
    if (!(saw_open && crossed_210225 && crossed_1m && peer_limit_grew && !saw_lost)) {
        fprintf(stderr, "monitored_put: criteria saw_open=%d crossed210225=%d crossed1m=%d peer_limit_grew=%d saw_lost=%d\nlast status:\n%s\n",
                saw_open, crossed_210225, crossed_1m, peer_limit_grew, saw_lost, status);
        return -1;
    }
    return 0;
}

static pid_t spawn_target(const char *carrier)
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

static pid_t spawn_host(const char *carrier, const char *control,
                        const char *pty1, const char *pty2,
                        const char *gdb, const char *file1, const char *file_dup,
                        const char *file_conflict)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (getenv("LMUX2_IT_VERBOSE"))
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control, "--verbose",
                  "--pty", pty1, "--pty", pty2,
                  "--unix", gdb,
                  "--file", file1, "--file", file_dup,
                  "--pty", file_conflict, (char *)NULL);
        else
            execl("build/host/lmuxd", "lmuxd", "--role", "host",
                  "--carrier-connect", carrier, "--control", control,
                  "--pty", pty1, "--pty", pty2,
                  "--unix", gdb,
                  "--file", file1, "--file", file_dup,
                  "--pty", file_conflict, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int integration_test(void)
{
    char dir[] = "/tmp/lmux2-file-it-XXXXXX";
    char carrier[160], control[160], c1[160], c2[160], gdb[512], gdb_host[160], gdb_target[160];
    char file_sock[160], file_bad_sock[160];
    char pty1[512], pty2[512], file_arg[320], file_dup[320], file_conflict[320];
    char target_zero[160], target_norm[160], target_large[160], target_10_put[160];
    char target_bad[160], target_abort[160], target_abort_old[160], target_absent_abort[160];
    char target_commit[160], target_get_abort[160], target_torture[160];
    char target_get0[160], target_get_large[160], target_10_get[160], target_reconnect[160], target_cli[160];
    char cli_src[160], cli_dst[160];
    pid_t target = -1, host = -1;
    int fd1 = -1, gfd = -1, rc = 1;
    int host_fd_base = -1, target_fd_base = -1;
    uint8_t *data = NULL, *got = NULL;
    size_t got_len;
    const char *where = "setup";

    if (!mkdtemp(dir))
        return fail("mkdtemp");
    snprintf(carrier, sizeof(carrier), "%s/carrier.sock", dir);
    snprintf(control, sizeof(control), "%s/control.sock", dir);
    snprintf(c1, sizeof(c1), "%s/console1.sock", dir);
    snprintf(c2, sizeof(c2), "%s/console2.sock", dir);
    snprintf(gdb_host, sizeof(gdb_host), "%s/gdb.sock", dir);
    snprintf(gdb_target, sizeof(gdb_target), "%s/target-gdb.sock", dir);
    snprintf(file_sock, sizeof(file_sock), "%s/file.sock", dir);
    snprintf(file_bad_sock, sizeof(file_bad_sock), "%s/file-bad.sock", dir);
    snprintf(pty1, sizeof(pty1), "console1:%s:/bin/sh", c1);
    snprintf(pty2, sizeof(pty2), "console2:%s:/bin/sh", c2);
    snprintf(gdb, sizeof(gdb), "gdb:%s:%s", gdb_host, gdb_target);
    snprintf(file_arg, sizeof(file_arg), "files:%s", file_sock);
    snprintf(file_dup, sizeof(file_dup), "files:%s", file_sock);
    snprintf(file_conflict, sizeof(file_conflict), "files:%s:/bin/false", file_bad_sock);
    snprintf(target_zero, sizeof(target_zero), "%s/zero.bin", dir);
    snprintf(target_norm, sizeof(target_norm), "%s/normal.bin", dir);
    snprintf(target_large, sizeof(target_large), "%s/large.bin", dir);
    snprintf(target_10_put, sizeof(target_10_put), "%s/put-10m.bin", dir);
    snprintf(target_bad, sizeof(target_bad), "%s/bad.bin", dir);
    snprintf(target_abort, sizeof(target_abort), "%s/abort.bin", dir);
    snprintf(target_abort_old, sizeof(target_abort_old), "%s/abort-old.bin", dir);
    snprintf(target_absent_abort, sizeof(target_absent_abort), "%s/abort-absent.bin", dir);
    snprintf(target_commit, sizeof(target_commit), "%s/commit-lost-reply.bin", dir);
    snprintf(target_get_abort, sizeof(target_get_abort), "%s/get-abort.bin", dir);
    snprintf(target_torture, sizeof(target_torture), "%s/torture.bin", dir);
    snprintf(target_get0, sizeof(target_get0), "%s/get0.bin", dir);
    snprintf(target_get_large, sizeof(target_get_large), "%s/get-large.bin", dir);
    snprintf(target_10_get, sizeof(target_10_get), "%s/get-10m.bin", dir);
    snprintf(target_reconnect, sizeof(target_reconnect), "%s/reconnect.bin", dir);
    snprintf(target_cli, sizeof(target_cli), "%s/cli.bin", dir);
    snprintf(cli_src, sizeof(cli_src), "%s/cli-src.bin", dir);
    snprintf(cli_dst, sizeof(cli_dst), "%s/cli-dst.bin", dir);

    host = spawn_host(carrier, control, pty1, pty2, gdb, file_arg, file_dup, file_conflict);
    if (host < 0)
        goto out;

    where = "host listeners before peer";
    if (wait_path_exists(control, 5000) < 0 ||
        wait_path_exists(c1, 5000) < 0 ||
        wait_path_exists(gdb_host, 5000) < 0 ||
        wait_path_exists(file_sock, 5000) < 0 ||
        access(file_bad_sock, F_OK) == 0)
        goto out;

    target = spawn_target(carrier);
    if (target < 0 || wait_path_exists(carrier, 8000) < 0)
        goto out;

    where = "listeners";
    fd1 = wait_connect(c1, 10000);
    gfd = wait_connect(gdb_host, 10000);
    if (fd1 < 0 || gfd < 0 || wait_path_exists(file_sock, 10000) < 0)
        goto out;
    close(gfd); gfd = -1;
    if (access(file_bad_sock, F_OK) == 0)
        goto out;
    host_fd_base = count_fds(host);
    target_fd_base = count_fds(target);

    where = "CWD request";
    {
        int fd = wait_connect(file_sock, 5000);
        uint8_t req = 1, reply[LXF_MAX_PAYLOAD], type;
        uint32_t rlen;
        if (fd < 0 ||
            send_record_frag(fd, LXF_T_CWD_REQUEST, &req, 1, 1) < 0 ||
            recv_record(fd, &type, reply, &rlen) < 0 ||
            type != LXF_T_CWD_REPLY || rlen < 2 || memchr(reply, 0, rlen) == NULL ||
            reply[0] != '/') {
            if (fd >= 0) close(fd);
            goto out;
        }
        close(fd);
    }

    where = "zero PUT";
    if (put_data(file_sock, target_zero, (const uint8_t *)"", 0, 0, 7) < 0)
        goto out;
    if (read_file_alloc(target_zero, &got, &got_len) < 0 || got_len != 0)
        goto out;
    free(got); got = NULL;

    where = "normal PUT";
    data = malloc(10 * 1024 * 1024);
    if (!data)
        goto out;
    fill_pattern(data, 70000, 17);
    if (put_data(file_sock, target_norm, data, 3217, 0, 5) < 0 ||
        read_file_alloc(target_norm, &got, &got_len) < 0 ||
        got_len != 3217 || memcmp(got, data, got_len) != 0)
        goto out;
    free(got); got = NULL;

    where = "large PUT fragmented";
    fill_pattern(data, 70000, 91);
    if (put_data(file_sock, target_large, data, 70000, 0, 113) < 0 ||
        read_file_alloc(target_large, &got, &got_len) < 0 ||
        got_len != 70000 || memcmp(got, data, got_len) != 0)
        goto out;
    free(got); got = NULL;

    where = "10MiB PUT flow-control stress";
    fill_pattern(data, 10 * 1024 * 1024, 201);
    if (monitored_put(control, file_sock, target_10_put, data, 10 * 1024 * 1024) < 0 ||
        read_file_alloc(target_10_put, &got, &got_len) < 0 ||
        got_len != 10 * 1024 * 1024 || memcmp(got, data, got_len) != 0)
        goto out;
    free(got); got = NULL;

    where = "lmux CLI put/get";
    {
        int fd = open(cli_src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char *put_argv[] = { "lmux", "put", "-S", file_sock, cli_src, target_cli, NULL };
        char *get_argv[] = { "lmux", "get", "-S", file_sock, target_cli, cli_dst, NULL };
        if (fd < 0 || write_all(fd, "cli-data", 8) < 0) {
            if (fd >= 0) close(fd);
            goto out;
        }
        close(fd);
        if (run_lmux_cli(put_argv) < 0 || run_lmux_cli(get_argv) < 0 ||
            read_file_alloc(cli_dst, &got, &got_len) < 0 ||
            got_len != 8 || memcmp(got, "cli-data", 8) != 0)
            goto out;
        free(got); got = NULL;
    }

    where = "bad hash preserves destination";
    if (put_data(file_sock, target_bad, (const uint8_t *)"original", 8, 0, 11) < 0 ||
        put_data(file_sock, target_bad, (const uint8_t *)"replacement", 11, 1, 3) < 0 ||
        read_file_alloc(target_bad, &got, &got_len) < 0 ||
        got_len != 8 || memcmp(got, "original", 8) != 0)
        goto out;
    free(got); got = NULL;

    where = "mid PUT abort absent cleanup";
    fill_pattern(data, 256 * 1024, 41);
    if (put_partial_disconnect(file_sock, target_absent_abort, data, 256 * 1024, 4096) < 0 ||
        wait_temp_count(dir, 0, 5000) < 0 ||
        expect_absent(target_absent_abort) < 0)
        goto out;
    if (put_data(file_sock, target_absent_abort, (const uint8_t *)"next", 4, 0, 2) < 0 ||
        file_equals_data(target_absent_abort, (const uint8_t *)"next", 4) < 0)
        goto out;

    where = "mid PUT abort preserves old destination";
    fill_pattern(data, 64 * 1024, 57);
    if (write_file_data(target_abort_old, data, 12345) < 0)
        goto out;
    fill_pattern(data + 128 * 1024, 256 * 1024, 73);
    if (put_partial_disconnect(file_sock, target_abort_old, data + 128 * 1024,
                               256 * 1024, 8192) < 0 ||
        wait_temp_count(dir, 0, 5000) < 0 ||
        file_equals_data(target_abort_old, data, 12345) < 0)
        goto out;
    if (put_data(file_sock, target_abort_old, data + 128 * 1024,
                 256 * 1024, 0, 23) < 0 ||
        get_data(file_sock, target_abort_old, &got, &got_len, 7) < 0 ||
        got_len != 256 * 1024 || memcmp(got, data + 128 * 1024, got_len) != 0)
        goto out;
    free(got); got = NULL;

    where = "post commit lost success reply";
    fill_pattern(data, 32 * 1024, 88);
    if (write_file_data(target_commit, data, 4096) < 0)
        goto out;
    fill_pattern(data + 64 * 1024, 160 * 1024, 89);
    if (put_commit_disconnect_before_reply(file_sock, target_commit,
                                           data + 64 * 1024, 160 * 1024) < 0 ||
        file_equals_data(target_commit, data + 64 * 1024, 160 * 1024) < 0 ||
        put_data(file_sock, target_commit, data + 64 * 1024, 160 * 1024, 0, 29) < 0 ||
        file_equals_data(target_commit, data + 64 * 1024, 160 * 1024) < 0 ||
        wait_temp_count(dir, 0, 5000) < 0)
        goto out;

    where = "GET zero/large";
    close(open(target_get0, O_WRONLY | O_CREAT | O_TRUNC, 0644));
    if (get_data(file_sock, target_get0, &got, &got_len, 4) < 0 || got_len != 0)
        goto out;
    free(got); got = NULL;
    fill_pattern(data, 70000, 133);
    {
        int fd = open(target_get_large, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write_all(fd, data, 70000) < 0) {
            if (fd >= 0) close(fd);
            goto out;
        }
        close(fd);
    }
    if (get_data(file_sock, target_get_large, &got, &got_len, 17) < 0 ||
        got_len != 70000 || memcmp(got, data, got_len) != 0)
        goto out;
    free(got); got = NULL;

    where = "mid GET client abort cleanup";
    fill_pattern(data, 96 * 1024, 141);
    if (write_file_data(target_get_abort, data, 96 * 1024) < 0 ||
        get_partial_disconnect(file_sock, target_get_abort) < 0 ||
        wait_temp_count(dir, 0, 5000) < 0 ||
        file_equals_data(target_get_abort, data, 96 * 1024) < 0 ||
        get_data(file_sock, target_get_abort, &got, &got_len, 19) < 0 ||
        got_len != 96 * 1024 || memcmp(got, data, got_len) != 0)
        goto out;
    free(got); got = NULL;

    where = "10MiB GET flow-control stress";
    fill_pattern(data, 10 * 1024 * 1024, 219);
    {
        int fd = open(target_10_get, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write_all(fd, data, 10 * 1024 * 1024) < 0) {
            if (fd >= 0) close(fd);
            goto out;
        }
        close(fd);
    }
    if (get_data(file_sock, target_10_get, &got, &got_len, 31) < 0 ||
        got_len != 10 * 1024 * 1024 || memcmp(got, data, got_len) != 0)
        goto out;
    free(got); got = NULL;

    where = "bulk file does not starve PTY";
    {
        pid_t child = fork();
        if (child == 0) {
            uint8_t *cg = NULL;
            size_t clen = 0;
            int ok = get_data(file_sock, target_get_large, &cg, &clen, 29) == 0 && clen == 70000;
            free(cg);
            _exit(ok ? 0 : 1);
        }
        if (child <= 0)
            goto out;
        for (int i = 0; i < 10; i++)
            if (shell_cmd_expect(fd1, "printf 'F''ILEPTY\\n'", "FILEPTY") < 0)
                goto out;
        {
            int st;
            if (waitpid(child, &st, 0) != child || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
                goto out;
        }
    }

    where = "physical reconnect aborts file transfer";
    {
        int fd = wait_connect(file_sock, 5000);
        uint8_t payload[LXF_MAX_PAYLOAD], reply[LXF_MAX_PAYLOAD], type;
        uint32_t rlen;
        fill_pattern(data, 128 * 1024, 153);
        if (write_file_data(target_reconnect, data, 32768) < 0)
            goto out;
        fill_pattern(data + 256 * 1024, 256 * 1024, 154);
        if (fd < 0)
            goto out;
        lmux_put_le32(payload, 0644);
        lmux_put_le64(payload + 4, 256 * 1024);
        lmux_put_le32(payload + 12, LXF_F_SIZE_KNOWN);
        strcpy((char *)payload + 16, target_reconnect);
        if (send_record_frag(fd, LXF_T_PUT_REQUEST, payload,
                             (uint32_t)(16 + strlen(target_reconnect) + 1), 7) < 0 ||
            recv_record(fd, &type, reply, &rlen) < 0 || type != LXF_T_OK ||
            send_record_frag(fd, LXF_T_DATA, data + 256 * 1024, 4096, 19) < 0) {
            close(fd);
            goto out;
        }
        stop_child(host);
        host = -1;
        close(fd);
        close(fd1); fd1 = -1;
    }
    if (wait_temp_count(dir, 0, 5000) < 0 ||
        file_equals_data(target_reconnect, data, 32768) < 0)
        goto out;
    host = spawn_host(carrier, control, pty1, pty2, gdb, file_arg, file_dup, file_conflict);
    if (host < 0)
        goto out;
    fd1 = wait_connect(c1, 10000);
    if (fd1 < 0 || wait_path_exists(file_sock, 10000) < 0)
        goto out;
    if (shell_cmd_expect(fd1, "printf 'F''ILECARRIER\\n'", "FILECARRIER") < 0 ||
        put_data(file_sock, target_reconnect, (const uint8_t *)"after", 5, 0, 3) < 0 ||
        file_equals_data(target_reconnect, (const uint8_t *)"after", 5) < 0)
        goto out;
    host_fd_base = count_fds(host);
    target_fd_base = count_fds(target);

    where = "repeated local PUT abort torture";
    for (int i = 0; i < 10; i++) {
        fill_pattern(data, 32 * 1024, (unsigned)(170 + i));
        if (write_file_data(target_torture, data, 8192 + (size_t)i) < 0)
            goto out;
        fill_pattern(data + 128 * 1024, 192 * 1024, (unsigned)(190 + i));
        if (put_partial_disconnect(file_sock, target_torture, data + 128 * 1024,
                                   192 * 1024, 2048 + (size_t)i) < 0 ||
            wait_temp_count(dir, 0, 5000) < 0 ||
            file_equals_data(target_torture, data, 8192 + (size_t)i) < 0)
            goto out;
        if (put_data(file_sock, target_torture, (const uint8_t *)"probe", 5, 0, 2) < 0 ||
            file_equals_data(target_torture, (const uint8_t *)"probe", 5) < 0)
            goto out;
    }

    where = "fd/temp baseline";
    usleep(300000);
    if (count_temp_files(dir) != 0 ||
        (host_fd_base >= 0 && count_fds(host) != host_fd_base) ||
        (target_fd_base >= 0 && count_fds(target) != target_fd_base))
        goto out;

    rc = 0;

out:
    if (rc)
        fprintf(stderr, "where: %s\n", where);
    if (fd1 >= 0) close(fd1);
    if (gfd >= 0) close(gfd);
    free(data);
    free(got);
    stop_child(host);
    stop_child(target);
    unlink(carrier); unlink(control); unlink(c1); unlink(c2); unlink(gdb_host); unlink(gdb_target);
    unlink(file_sock); unlink(file_bad_sock);
    unlink(target_zero); unlink(target_norm); unlink(target_large); unlink(target_10_put);
    unlink(target_bad); unlink(target_abort); unlink(target_get0); unlink(target_get_large);
    unlink(target_10_get); unlink(target_reconnect);
    unlink(target_abort_old); unlink(target_absent_abort); unlink(target_commit);
    unlink(target_get_abort); unlink(target_torture);
    unlink(target_cli); unlink(cli_src); unlink(cli_dst);
    rmdir(dir);
    return rc ? fail(where) : 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (integration_test())
        return 1;
    puts("lmux2 file integration selftest ok");
    return 0;
}
