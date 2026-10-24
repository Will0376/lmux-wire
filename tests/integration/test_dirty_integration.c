#include "dirty.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TOTAL_BYTES 8192

struct proxy_dir {
    int in_fd;
    int out_fd;
    int to_target;
    uint8_t line[LMUX_DIRTY_RECORD_MAX];
    size_t line_len;
};

struct faults {
    int corrupted_fragment;
    int corrupted_reverse_fragment;
    int dropped_fragment;
    int duplicated_fragment;
    int dropped_ack;
    int injected_long_noise;
};

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pattern(uint8_t *buf, size_t offset, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        size_t n = offset + i;
        buf[i] = (uint8_t)((n * 131u + 0x5du) ^ (n >> 3));
    }
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int adapter_quiet(const struct lmux_dirty *d)
{
    unsigned i;
    if (d->output_pos != d->output_len || d->pending_fail) return 0;
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++)
        if (d->tx[i].used) return 0;
    for (i = 0; i < LMUX_DIRTY_CONTROL_MAX; i++)
        if (d->control[i].used) return 0;
    return 1;
}

static int child_peer(int fd, int host)
{
    struct lmux_dirty d;
    uint8_t io[64], packet[LMUX_DIRTY_PACKET_MAX];
    size_t sent = 0, received = 0;
    long long end = now_ms() + 15000;
    long long complete_since = 0;
    if (set_nonblock(fd) < 0) return 10;
    lmux_dirty_init(&d, host ? 0x1111 : 0x2222,
                    host ? 0x2222 : 0x1111, 0, 0, 115200,
                    (uint64_t)now_ms());
    while (now_ms() < end) {
        const uint8_t *out, *got;
        size_t out_len = 0, got_len;
        ssize_t n;
        uint64_t now = (uint64_t)now_ms();

        if (host && sent < TOTAL_BYTES) {
            size_t chunk = 97 + ((sent / 97) * 211) % 928;
            if (chunk > TOTAL_BYTES - sent) chunk = TOTAL_BYTES - sent;
            pattern(packet, sent, chunk);
            if (lmux_dirty_submit(&d, packet, chunk, now, NULL) == 0)
                sent += chunk;
            else if (errno != EAGAIN)
                return 11;
        }

        if (lmux_dirty_receive_peek(&d, &got, &got_len, NULL) == 1) {
            pattern(packet, received, got_len);
            if (memcmp(got, packet, got_len) != 0 ||
                received + got_len > TOTAL_BYTES)
                return 12;
            if (!host) {
                if (lmux_dirty_submit(&d, got, got_len, now, NULL) < 0) {
                    if (errno != EAGAIN) return 13;
                } else {
                    received += got_len;
                    if (lmux_dirty_receive_consume(&d) < 0) return 14;
                }
            } else {
                received += got_len;
                if (lmux_dirty_receive_consume(&d) < 0) return 15;
            }
        }

        if (lmux_dirty_output_peek(&d, now, &out, &out_len) < 0 &&
            !d.pending_fail)
            return 16;
        if (out_len) {
            if (out_len > 13) out_len = 13;
            n = write(fd, out, out_len);
            if (n > 0) {
                if (lmux_dirty_output_consume(&d, (size_t)n) < 0) return 17;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR) {
                lmux_dirty_peer_lost(&d);
                return d.fail_reason == LMUX_DIRTY_FAIL_PEER ? 18 : 19;
            }
        }

        n = read(fd, io, 11);
        if (n > 0) {
            if (lmux_dirty_feed(&d, io, (size_t)n, now) < 0) return 20;
        } else if (n == 0) {
            if (received == TOTAL_BYTES && (!host || sent == TOTAL_BYTES) &&
                adapter_quiet(&d)) {
                close(fd);
                return 0;
            }
            fprintf(stderr,
                    "dirty %s EOF sent=%zu recv=%zu tx=%u/%u ctl=%u/%u out=%u/%u failed=%u\n",
                    host ? "host" : "target", sent, received,
                    d.tx[0].used, d.tx[1].used,
                    d.control[0].used, d.control[1].used,
                    d.output_pos, d.output_len, d.failed);
            lmux_dirty_peer_lost(&d);
            return 21;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return 22;
        }
        if (lmux_dirty_tick(&d, now) < 0) return 23;

        if (received == TOTAL_BYTES && (!host || sent == TOTAL_BYTES) &&
            adapter_quiet(&d)) {
            if (host || (complete_since &&
                         now_ms() - complete_since >=
                         (long long)d.retry_interval_ms * 2)) {
                close(fd);
                return 0;
            }
            if (!complete_since) complete_since = now_ms();
        } else {
            complete_since = 0;
        }
        {
            struct pollfd pfd = { fd, POLLIN | POLLOUT, 0 };
            poll(&pfd, 1, 5);
        }
    }
    fprintf(stderr,
            "dirty %s timeout sent=%zu recv=%zu tx=%u/%u ctl=%u/%u/%u out=%u/%u fail=%u reason=%u\n",
            host ? "host" : "target", sent, received,
            d.tx[0].used, d.tx[1].used, d.control[0].used,
            d.control[1].used, d.control[2].used,
            d.output_pos, d.output_len, d.failed, d.fail_reason);
    return 24;
}

static int write_split(int fd, const uint8_t *buf, size_t len)
{
    while (len) {
        size_t chunk = len > 7 ? 7 : len;
        ssize_t n = write(fd, buf, chunk);
        if (n > 0) { buf += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            if (poll(&pfd, 1, 1000) > 0) continue;
        }
        return -1;
    }
    return 0;
}

static int forward_record(struct proxy_dir *dir, struct faults *f)
{
    struct lmux_dirty_record record;
    static const uint8_t noise[] = "[ 0.123] console foreign output\nlogin: ";
    int decoded = lmux_dirty_record_decode(dir->line, dir->line_len, &record);
    if (decoded != 0) return -1;
    if (dir->to_target && record.type == LMUX_DIRTY_DATA &&
        record.fragment_index == 0 && !f->corrupted_fragment) {
        size_t half = dir->line_len / 2;
        static const uint8_t inside[] = "console-bytes-inside-fragment";
        f->corrupted_fragment = 1;
        if (write_split(dir->out_fd, dir->line, half) < 0 ||
            write_split(dir->out_fd, inside, sizeof(inside) - 1) < 0 ||
            write_split(dir->out_fd, dir->line + half,
                        dir->line_len - half) < 0)
            return -1;
        return 0;
    }
    if (!dir->to_target && record.type == LMUX_DIRTY_DATA &&
        record.fragment_index == 0 && !f->corrupted_reverse_fragment) {
        size_t half = dir->line_len / 2;
        static const uint8_t inside[] =
            "kernel-console-inside-target-record";
        f->corrupted_reverse_fragment = 1;
        if (write_split(dir->out_fd, dir->line, half) < 0 ||
            write_split(dir->out_fd, inside, sizeof(inside) - 1) < 0 ||
            write_split(dir->out_fd, dir->line + half,
                        dir->line_len - half) < 0)
            return -1;
        return 0;
    }
    if (dir->to_target && record.type == LMUX_DIRTY_DATA &&
        record.fragment_index == 1 && !f->dropped_fragment) {
        f->dropped_fragment = 1;
        return 0;
    }
    if (!dir->to_target && record.type == LMUX_DIRTY_ACK && !f->dropped_ack) {
        f->dropped_ack = 1;
        return 0;
    }
    if (!f->injected_long_noise && dir->to_target) {
        uint8_t garbage[2048];
        memset(garbage, 'x', sizeof(garbage));
        f->injected_long_noise = 1;
        if (write_split(dir->out_fd, garbage, sizeof(garbage)) < 0 ||
            write_split(dir->out_fd, (const uint8_t *)"\n", 1) < 0)
            return -1;
    }
    if (write_split(dir->out_fd, noise, sizeof(noise) - 1) < 0 ||
        write_split(dir->out_fd, dir->line, dir->line_len) < 0)
        return -1;
    if (dir->to_target && record.type == LMUX_DIRTY_DATA &&
        record.fragment_index == 2 && !f->duplicated_fragment) {
        f->duplicated_fragment = 1;
        if (write_split(dir->out_fd, dir->line, dir->line_len) < 0)
            return -1;
    }
    return 0;
}

static int proxy_read(struct proxy_dir *dir, struct faults *faults)
{
    uint8_t buf[23];
    ssize_t n = read(dir->in_fd, buf, sizeof(buf));
    size_t i;
    if (n <= 0) return n == 0 ? 1 :
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1);
    for (i = 0; i < (size_t)n; i++) {
        if (dir->line_len == sizeof(dir->line)) return -1;
        dir->line[dir->line_len++] = buf[i];
        if (buf[i] == '\n') {
            if (forward_record(dir, faults) < 0) return -1;
            dir->line_len = 0;
        }
    }
    return 0;
}

static int wait_child(pid_t pid, int *done)
{
    int status;
    pid_t r;
    if (*done) return 0;
    r = waitpid(pid, &status, WNOHANG);
    if (r == 0) return 0;
    if (r != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "dirty process child %ld failed status=%d\n",
                (long)pid, r == pid ? status : -1);
        return -1;
    }
    *done = 1;
    return 0;
}

static int test_process_fault_proxy(void)
{
    int host_pair[2], target_pair[2];
    pid_t host_pid, target_pid;
    struct proxy_dir dirs[2];
    struct faults faults;
    int host_done = 0, target_done = 0;
    long long end = now_ms() + 20000;
    signal(SIGPIPE, SIG_IGN);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, host_pair) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, target_pair) < 0)
        return -1;
    host_pid = fork();
    if (host_pid < 0) {
        close(host_pair[0]); close(host_pair[1]);
        close(target_pair[0]); close(target_pair[1]);
        return -1;
    }
    if (host_pid == 0) {
        int rc;
        close(host_pair[0]); close(target_pair[0]); close(target_pair[1]);
        rc = child_peer(host_pair[1], 1);
        if (rc) fprintf(stderr, "dirty host child rc=%d\n", rc);
        _exit(rc);
    }
    target_pid = fork();
    if (target_pid < 0) {
        close(host_pair[0]); close(host_pair[1]);
        close(target_pair[0]); close(target_pair[1]);
        kill(host_pid, SIGKILL);
        waitpid(host_pid, NULL, 0);
        return -1;
    }
    if (target_pid == 0) {
        int rc;
        close(target_pair[0]); close(host_pair[0]); close(host_pair[1]);
        rc = child_peer(target_pair[1], 0);
        if (rc) fprintf(stderr, "dirty target child rc=%d\n", rc);
        _exit(rc);
    }
    close(host_pair[1]); close(target_pair[1]);
    set_nonblock(host_pair[0]); set_nonblock(target_pair[0]);
    memset(&dirs, 0, sizeof(dirs)); memset(&faults, 0, sizeof(faults));
    dirs[0].in_fd = host_pair[0]; dirs[0].out_fd = target_pair[0];
    dirs[0].to_target = 1;
    dirs[1].in_fd = target_pair[0]; dirs[1].out_fd = host_pair[0];
    while (now_ms() < end && (!host_done || !target_done)) {
        struct pollfd pfds[2] = {
            { host_pair[0], POLLIN, 0 }, { target_pair[0], POLLIN, 0 }
        };
        poll(pfds, 2, 20);
        if (!host_done && proxy_read(&dirs[0], &faults) < 0) goto bad;
        if (proxy_read(&dirs[1], &faults) < 0) goto bad;
        if (wait_child(host_pid, &host_done) < 0 ||
            wait_child(target_pid, &target_done) < 0)
            goto bad;
    }
    close(host_pair[0]); close(target_pair[0]);
    if (!host_done || !target_done || !faults.corrupted_fragment ||
        !faults.corrupted_reverse_fragment ||
        !faults.dropped_fragment || !faults.duplicated_fragment ||
        !faults.dropped_ack || !faults.injected_long_noise)
        goto bad_after_close;
    return 0;
bad:
    close(host_pair[0]); close(target_pair[0]);
bad_after_close:
    fprintf(stderr,
            "dirty proxy incomplete host=%d target=%d corrupt=%d reverse-corrupt=%d drop-frag=%d dup=%d drop-ack=%d noise=%d\n",
            host_done, target_done, faults.corrupted_fragment,
            faults.corrupted_reverse_fragment, faults.dropped_fragment,
            faults.duplicated_fragment,
            faults.dropped_ack, faults.injected_long_noise);
    if (!host_done) kill(host_pid, SIGKILL);
    if (!target_done) kill(target_pid, SIGKILL);
    waitpid(host_pid, NULL, 0); waitpid(target_pid, NULL, 0);
    return -1;
}

static int test_process_peer_disappearance(void)
{
    int pair[2], status;
    pid_t pid;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) return -1;
    pid = fork();
    if (pid == 0) {
        struct lmux_dirty d;
        uint8_t byte;
        close(pair[0]);
        lmux_dirty_init(&d, 1, 2, 0, 0, 115200, 0);
        if (read(pair[1], &byte, 1) != 0) _exit(2);
        lmux_dirty_peer_lost(&d);
        _exit(lmux_dirty_failed(&d) &&
              d.fail_reason == LMUX_DIRTY_FAIL_PEER ? 0 : 3);
    }
    close(pair[1]); close(pair[0]);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

int main(void)
{
    if (test_process_fault_proxy() < 0) {
        fprintf(stderr, "dirty integration failed: fault proxy\n");
        return 1;
    }
    if (test_process_peer_disappearance() < 0) {
        fprintf(stderr, "dirty integration failed: peer disappearance\n");
        return 1;
    }
    printf("dirty process integration: ok\n");
    return 0;
}
