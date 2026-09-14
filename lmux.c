#include "lxf.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios saved_termios;
static int saved_termios_valid;
static int term_detach;
static int term_modal;
static char host_cwd[LXF_PATH_MAX] = ".";

struct transfer_config {
    const char *sock_path;
    const char *remote_dir;
    int channel;
    int verbose;
    mode_t mode;
    int mode_set;
};

struct progress_state {
    const char *op;
    int verbose;
    int tty;
    int total_known;
    uint64_t total;
    uint64_t done;
    struct timespec start;
    struct timespec last;
    int drew;
};

enum target_base_source {
    TARGET_BASE_CONFIG = 1,
    TARGET_BASE_ENV,
    TARGET_BASE_CWD,
    TARGET_BASE_FALLBACK
};

static void restore_termios(void)
{
    if (saved_termios_valid)
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static int set_raw_termios(void)
{
    struct termios tio;
    if (!saved_termios_valid)
        return 0;
    tio = saved_termios;
    cfmakeraw(&tio);
    return tcsetattr(STDIN_FILENO, TCSANOW, &tio);
}

static const char *console_socket_for_arg(const char *arg)
{
    if (!arg || strcmp(arg, "1") == 0)
        return "/run/lmux/console1.sock";
    if (strcmp(arg, "2") == 0)
        return "/run/lmux/console2.sock";
    return arg;
}

static int is_pure_numeric(const char *s)
{
    if (!s || !*s)
        return 0;
    while (*s) {
        if (*s < '0' || *s > '9')
            return 0;
        s++;
    }
    return 1;
}

static void init_host_cwd(void)
{
    if (!getcwd(host_cwd, sizeof(host_cwd))) {
        perror("getcwd");
        strcpy(host_cwd, ".");
    }
}

static int is_abs_path(const char *p)
{
    return p && p[0] == '/';
}

static int copy_path(char *out, size_t cap, const char *in)
{
    size_t n;
    if (!in)
        return -1;
    n = strlen(in);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(out, in, n + 1);
    return 0;
}

static int join_path(char *out, size_t cap, const char *base, const char *leaf)
{
    size_t blen, llen;
    if (!base || !leaf || !*base || !*leaf)
        return -1;
    if (is_abs_path(leaf))
        return copy_path(out, cap, leaf);
    blen = strlen(base);
    llen = strlen(leaf);
    while (blen > 1 && base[blen - 1] == '/')
        blen--;
    if (blen + 1 + llen >= cap)
        return -1;
    memcpy(out, base, blen);
    out[blen] = '/';
    memcpy(out + blen + 1, leaf, llen + 1);
    return 0;
}

static int parse_channel_arg(const char *s, int *channel)
{
    if (!s || !channel || (strcmp(s, "1") != 0 && strcmp(s, "2") != 0))
        return -1;
    *channel = s[0] - '0';
    return 0;
}

static int default_channel_for_console_arg(const char *arg)
{
    if (!arg || strcmp(arg, "1") == 0)
        return 1;
    if (strcmp(arg, "2") == 0)
        return 2;
    return 0;
}

static int valid_target_base(const char *p)
{
    return p && p[0] == '/' && strlen(p) < LXF_PATH_MAX;
}

static void human_bytes(uint64_t bytes, char *out, size_t cap, int per_sec)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(out, cap, "%llu %s%s", (unsigned long long)bytes, units[u], per_sec ? "/s" : "");
    else if (v < 10.0)
        snprintf(out, cap, "%.2f %s%s", v, units[u], per_sec ? "/s" : "");
    else
        snprintf(out, cap, "%.1f %s%s", v, units[u], per_sec ? "/s" : "");
}

static void human_duration(double sec, char *out, size_t cap)
{
    unsigned long long s;
    if (sec < 0.0)
        sec = 0.0;
    s = (unsigned long long)(sec + 0.5);
    if (s < 60)
        snprintf(out, cap, "%llus", s);
    else if (s < 3600)
        snprintf(out, cap, "%llum %02llus", s / 60, s % 60);
    else
        snprintf(out, cap, "%lluh %02llum %02llus", s / 3600, (s / 60) % 60, s % 60);
}

static double ts_elapsed(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static int progress_bar_width(int verbose)
{
    struct winsize ws;
    int width = verbose ? 18 : 24;
    if (isatty(STDERR_FILENO) && ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        if (verbose)
            width = ws.ws_col > 96 ? 24 : 16;
        else
            width = ws.ws_col > 60 ? 24 : 16;
    }
    if (width < 10)
        width = 10;
    if (width > 32)
        width = 32;
    return width;
}

static void make_bar(char *out, size_t cap, int width, int percent, uint64_t done)
{
    int i, fill;
    if ((size_t)width + 3 > cap)
        width = (int)cap - 3;
    if (width < 1)
        width = 1;
    out[0] = '[';
    if (percent >= 0) {
        fill = (percent * width + 50) / 100;
        for (i = 0; i < width; i++)
            out[1 + i] = i < fill ? '#' : '-';
    } else {
        fill = (int)((done / 4096) % (uint64_t)width);
        for (i = 0; i < width; i++)
            out[1 + i] = i == fill ? '>' : (i < fill ? '=' : ' ');
    }
    out[1 + width] = ']';
    out[2 + width] = 0;
}

static int progress_percent(uint64_t done, uint64_t total, int known)
{
    if (!known)
        return -1;
    if (total == 0)
        return 100;
    if (done >= total)
        return 100;
    return (int)((done * 100 + total / 2) / total);
}

static double progress_rate(uint64_t done, double elapsed)
{
    if (done == 0 || elapsed <= 0.05)
        return 0.0;
    return (double)done / elapsed;
}

static int progress_eta_seconds(uint64_t done, uint64_t total, double rate, double *eta)
{
    if (!eta || done >= total) {
        if (eta)
            *eta = 0.0;
        return eta ? 0 : -1;
    }
    if (rate <= 1.0)
        return -1;
    *eta = (double)(total - done) / rate;
    return 0;
}

static void progress_init(struct progress_state *p, const char *op, int total_known, uint64_t total, int verbose)
{
    memset(p, 0, sizeof(*p));
    p->op = op;
    p->verbose = verbose;
    p->tty = isatty(STDERR_FILENO);
    p->total_known = total_known;
    p->total = total;
    clock_gettime(CLOCK_MONOTONIC, &p->start);
    p->last = p->start;
}

static void progress_update(struct progress_state *p, uint64_t done, int force)
{
    struct timespec now;
    double since, elapsed, rate = 0.0, eta = -1.0;
    char bar[40], line[320], b_done[32], b_total[32], b_rate[32], ebuf[32], etabuf[32];
    int pct, bw;

    p->done = done;
    if (!p->tty)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    since = ts_elapsed(&p->last, &now);
    if (!force && since < 0.20)
        return;
    p->last = now;
    elapsed = ts_elapsed(&p->start, &now);
    rate = progress_rate(done, elapsed);
    pct = progress_percent(done, p->total, p->total_known);
    bw = progress_bar_width(p->verbose);
    make_bar(bar, sizeof(bar), bw, pct, done);
    if (!p->verbose) {
        snprintf(line, sizeof(line), "\r%s %s %s%%", p->op, bar, pct >= 0 ? (char[8]){0} : "--");
        if (pct >= 0)
            snprintf(line, sizeof(line), "\r%s %s %3d%%", p->op, bar, pct);
    } else {
        human_bytes(done, b_done, sizeof(b_done), 0);
        if (p->total_known)
            human_bytes(p->total, b_total, sizeof(b_total), 0);
        else
            strcpy(b_total, "");
        human_bytes((uint64_t)rate, b_rate, sizeof(b_rate), 1);
        human_duration(elapsed, ebuf, sizeof(ebuf));
        if (p->total_known && pct == 100) {
            strcpy(etabuf, "0s");
        } else if (p->total_known && elapsed > 0.25 &&
                   progress_eta_seconds(done, p->total, rate, &eta) == 0) {
            human_duration(eta, etabuf, sizeof(etabuf));
        } else {
            strcpy(etabuf, "--");
        }
        if (p->total_known)
            snprintf(line, sizeof(line), "\r%s %s %3d%%  %s/%s  %s  elapsed %s  ETA %s",
                     p->op, bar, pct, b_done, b_total, b_rate, ebuf, etabuf);
        else
            snprintf(line, sizeof(line), "\r%s %s --%%  %s  %s  elapsed %s  ETA --",
                     p->op, bar, b_done, b_rate, ebuf);
    }
    fputs(line, stderr);
    fflush(stderr);
    p->drew = 1;
}

static void progress_finish(struct progress_state *p, uint64_t done)
{
    progress_update(p, done, 1);
    if (p->drew) {
        fputc('\n', stderr);
        fflush(stderr);
        p->drew = 0;
    }
}

static void on_signal(int sig)
{
    restore_termios();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void usage(FILE *f)
{
    fprintf(f,
        "lmux %s\n"
        "Usage:\n"
        "  lmux connect SOCKET\n"
        "  lmux term [-v|--verbose] [--remote-dir DIR] [--channel 1|2] [1|2|SOCKET]\n"
        "  lmux exec SOCKET -- PROGRAM [ARGS...]\n"
        "  lmux put [OPTIONS] HOST_SRC [TARGET_DST]\n"
        "      upload file: HOST -> TARGET\n"
        "  lmux get [OPTIONS] TARGET_SRC [HOST_DST]\n"
        "      download file: TARGET -> HOST\n"
        "\n"
        "File options:\n"
        "  -S SOCKET, --socket SOCKET   file service socket (default " LXF_DEFAULT_SOCKET ")\n"
        "  --remote-dir DIR             TARGET base directory for relative/omitted paths\n"
        "  --channel 1|2                TARGET shell cwd context (default 1)\n"
        "  -v, --verbose                detailed transfer progress\n"
        "  --mode OCTAL                 PUT destination mode\n"
        "\n"
        "Examples:\n"
        "  lmux put ./lmuxd\n"
        "  lmux put ./lmuxd /root/lmux/lmuxd\n"
        "  lmux get log.log\n"
        "  lmux get /tmp/core.bin ./core.bin\n"
        "\n"
        "lmux term Ctrl-T commands:\n"
        "  Ctrl-T ?        help\n"
        "  Ctrl-T q/.      disconnect\n"
        "  Ctrl-T Ctrl-T   send literal Ctrl-T\n"
        "  Ctrl-T p        upload file   HOST -> TARGET\n"
        "  Ctrl-T g        download file TARGET -> HOST\n",
        LMUX_BUILD_VERSION);
}

static int connect_unix_mode(const char *path, int quiet)
{
    int fd;
    struct sockaddr_un sa;

    if (strlen(path) >= sizeof(sa.sun_path)) {
        if (!quiet)
            fprintf(stderr, "socket path too long: %s\n", path);
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (!quiet)
            perror("socket");
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (!quiet)
            perror(path);
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_unix(const char *path)
{
    return connect_unix_mode(path, 0);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct pipebuf {
    uint8_t buf[4096];
    size_t off;
    size_t len;
    int in_eof;
};

static void compact_pipebuf(struct pipebuf *p)
{
    if (p->off && p->len) {
        memmove(p->buf, p->buf + p->off, p->len);
        p->off = 0;
    } else if (p->len == 0) {
        p->off = 0;
    }
}

static int bridge_loop(int sock)
{
    struct pipebuf a, b;
    int stdin_open = 1, sock_open = 1, stdout_open = 1;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    if (set_nonblock(STDIN_FILENO) < 0 || set_nonblock(STDOUT_FILENO) < 0 || set_nonblock(sock) < 0) {
        perror("fcntl");
        return 1;
    }

    while ((stdin_open || a.len) && (sock_open || b.len) && stdout_open) {
        struct pollfd pfds[3];
        nfds_t n = 0;
        int stdin_idx = -1, sock_idx = -1, out_idx = -1;
        short sock_events = 0;

        if (stdin_open && a.len < sizeof(a.buf)) {
            stdin_idx = (int)n;
            pfds[n].fd = STDIN_FILENO;
            pfds[n].events = POLLIN;
            pfds[n++].revents = 0;
        }
        if (sock_open) {
            if (b.len < sizeof(b.buf))
                sock_events |= POLLIN;
            if (a.len)
                sock_events |= POLLOUT;
            if (sock_events) {
                sock_idx = (int)n;
                pfds[n].fd = sock;
                pfds[n].events = sock_events;
                pfds[n++].revents = 0;
            }
        }
        if (b.len && stdout_open) {
            out_idx = (int)n;
            pfds[n].fd = STDOUT_FILENO;
            pfds[n].events = POLLOUT;
            pfds[n++].revents = 0;
        }
        if (n == 0)
            break;
        if (poll(pfds, n, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }

        if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP))) {
            compact_pipebuf(&a);
            ssize_t r = read(STDIN_FILENO, a.buf + a.off + a.len, sizeof(a.buf) - a.off - a.len);
            if (r > 0)
                a.len += (size_t)r;
            else if (r == 0) {
                stdin_open = 0;
                if (a.len == 0)
                    shutdown(sock, SHUT_WR);
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                stdin_open = 0;
        }
        if (sock_idx >= 0 && (pfds[sock_idx].revents & POLLOUT) && a.len) {
            ssize_t w = write(sock, a.buf + a.off, a.len);
            if (w > 0) {
                a.off += (size_t)w;
                a.len -= (size_t)w;
                if (a.len == 0) {
                    a.off = 0;
                    if (!stdin_open)
                        shutdown(sock, SHUT_WR);
                }
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                sock_open = 0;
            }
        }
        if (sock_idx >= 0 && (pfds[sock_idx].revents & (POLLIN | POLLHUP)) && b.len < sizeof(b.buf)) {
            compact_pipebuf(&b);
            ssize_t r = read(sock, b.buf + b.off + b.len, sizeof(b.buf) - b.off - b.len);
            if (r > 0)
                b.len += (size_t)r;
            else if (r == 0)
                sock_open = 0;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                sock_open = 0;
        }
        if (out_idx >= 0 && (pfds[out_idx].revents & POLLOUT) && b.len) {
            ssize_t w = write(STDOUT_FILENO, b.buf + b.off, b.len);
            if (w > 0) {
                b.off += (size_t)w;
                b.len -= (size_t)w;
                if (b.len == 0)
                    b.off = 0;
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                stdout_open = 0;
            }
        }
        if (sock_idx >= 0 && (pfds[sock_idx].revents & (POLLERR | POLLNVAL)))
            sock_open = 0;
    }
    return 0;
}

static void term_help(void)
{
    static const char msg[] =
        "\r\nCtrl-T ?       help\r\n"
        "Ctrl-T q/.     disconnect\r\n"
        "Ctrl-T Ctrl-T  send literal Ctrl-T\r\n"
        "Ctrl-T p       upload file   HOST -> TARGET\r\n"
        "Ctrl-T g       download file TARGET -> HOST\r\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

static void append_byte(struct pipebuf *p, uint8_t b)
{
    compact_pipebuf(p);
    if (p->off + p->len < sizeof(p->buf))
        p->buf[p->off + p->len++] = b;
}

static void term_filter_input(struct pipebuf *out, const uint8_t *buf, size_t len, int *prefix)
{
    size_t i;
    for (i = 0; i < len && !term_detach && !term_modal; i++) {
        uint8_t b = buf[i];
        if (*prefix) {
            if (b == '?') {
                term_help();
            } else if (b == 'q' || b == '.') {
                term_detach = 1;
            } else if (b == 'p' || b == 'g') {
                term_modal = b;
            } else if (b == 0x14) {
                append_byte(out, 0x14);
            } else {
                append_byte(out, 0x14);
                append_byte(out, b);
            }
            *prefix = 0;
        } else if (b == 0x14) {
            *prefix = 1;
        } else {
            append_byte(out, b);
        }
    }
}

static int transfer_put(const struct transfer_config *cfg, const char *host_src, const char *target_dst);
static int transfer_get(const struct transfer_config *cfg, const char *target_src, const char *host_dst);
static int select_target_base(const char *sock_path, const char *configured_dir, int channel,
                              char *out, size_t cap, enum target_base_source *source);

static const char *path_basename(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int prompt_line(const char *prompt, const char *def, char *out, size_t cap)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    char line[LXF_PATH_MAX];
    if (flags >= 0)
        (void)fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
    fprintf(stderr, "%s", prompt);
    if (def)
        fprintf(stderr, " [%s]", def);
    fprintf(stderr, ": ");
    fflush(stderr);
    if (!fgets(line, sizeof(line), stdin)) {
        if (flags >= 0)
            (void)fcntl(STDIN_FILENO, F_SETFL, flags);
        return -1;
    }
    if (flags >= 0)
        (void)fcntl(STDIN_FILENO, F_SETFL, flags);
    line[strcspn(line, "\r\n")] = 0;
    if (line[0] == 0) {
        if (!def)
            return -1;
        if (strlen(def) >= cap)
            return -1;
        strcpy(out, def);
    } else {
        if (strlen(line) >= cap)
            return -1;
        strcpy(out, line);
    }
    return 0;
}

static int run_modal_transfer(char modal, const char *remote_dir, int channel, int verbose)
{
    struct transfer_config cfg;
    enum target_base_source src = TARGET_BASE_FALLBACK;
    char host_path[LXF_PATH_MAX];
    char target_path[LXF_PATH_MAX];
    char target_base[LXF_PATH_MAX];
    char def[LXF_PATH_MAX];
    int rc;

    restore_termios();
    memset(&cfg, 0, sizeof(cfg));
    cfg.sock_path = LXF_DEFAULT_SOCKET;
    cfg.channel = channel;
    cfg.verbose = verbose;
    fprintf(stderr, "\r\n");
    if (select_target_base(cfg.sock_path, remote_dir, channel, target_base, sizeof(target_base), &src) < 0) {
        strcpy(target_base, "/root");
        src = TARGET_BASE_FALLBACK;
    }
    if (modal == 'p') {
        fprintf(stderr, "=== UPLOAD: HOST -> TARGET ===\n");
        fprintf(stderr, "Host cwd:   %s\n", host_cwd);
        fprintf(stderr, "%s: %s%s\n",
                src == TARGET_BASE_CWD ? "Target cwd" : "Target base",
                target_base, src == TARGET_BASE_FALLBACK ? " (fallback)" : "");
        if (src == TARGET_BASE_FALLBACK)
            fprintf(stderr, "Target cwd unavailable; using %s\n", target_base);
        if (prompt_line("Host source", NULL, host_path, sizeof(host_path)) < 0)
            goto cancel;
        if (join_path(def, sizeof(def), target_base, path_basename(host_path)) < 0)
            goto cancel;
        if (prompt_line("Target destination", def, target_path, sizeof(target_path)) < 0)
            goto cancel;
        if (join_path(def, sizeof(def), target_base, target_path) < 0)
            goto cancel;
        rc = transfer_put(&cfg, host_path, def);
    } else {
        fprintf(stderr, "=== DOWNLOAD: TARGET -> HOST ===\n");
        fprintf(stderr, "Host cwd:   %s\n", host_cwd);
        fprintf(stderr, "%s: %s%s\n",
                src == TARGET_BASE_CWD ? "Target cwd" : "Target base",
                target_base, src == TARGET_BASE_FALLBACK ? " (fallback)" : "");
        if (src == TARGET_BASE_FALLBACK)
            fprintf(stderr, "Target cwd unavailable; using %s\n", target_base);
        if (prompt_line("Target source", NULL, target_path, sizeof(target_path)) < 0)
            goto cancel;
        if (join_path(def, sizeof(def), target_base, target_path) < 0)
            goto cancel;
        strcpy(target_path, def);
        if (join_path(def, sizeof(def), host_cwd, path_basename(target_path)) < 0)
            goto cancel;
        if (prompt_line("Host destination", def, host_path, sizeof(host_path)) < 0)
            goto cancel;
        if (join_path(def, sizeof(def), host_cwd, host_path) < 0)
            goto cancel;
        rc = transfer_get(&cfg, target_path, def);
    }
    fprintf(stderr, "%s\r\n", rc == 0 ? "transfer complete" : "transfer failed");
    (void)set_raw_termios();
    return rc;
cancel:
    fprintf(stderr, "transfer cancelled\r\n");
    (void)set_raw_termios();
    return 1;
}

static int term_bridge_loop(int sock, const char *remote_dir, int channel, int verbose)
{
    struct pipebuf a, b;
    int stdin_open = 1, sock_open = 1, stdout_open = 1;
    int prefix = 0;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    term_detach = 0;
    term_modal = 0;
    if (set_nonblock(STDIN_FILENO) < 0 || set_nonblock(STDOUT_FILENO) < 0 || set_nonblock(sock) < 0) {
        perror("fcntl");
        return 1;
    }

    while (!term_detach && (stdin_open || a.len) && (sock_open || b.len) && stdout_open) {
        struct pollfd pfds[3];
        nfds_t n = 0;
        int stdin_idx = -1, sock_idx = -1, out_idx = -1;
        short sock_events = 0;

        if (term_modal) {
            char modal = (char)term_modal;
            term_modal = 0;
            (void)run_modal_transfer(modal, remote_dir, channel, verbose);
            continue;
        }

        if (!term_modal && stdin_open && a.len + 2 < sizeof(a.buf)) {
            stdin_idx = (int)n;
            pfds[n].fd = STDIN_FILENO;
            pfds[n].events = POLLIN;
            pfds[n++].revents = 0;
        }
        if (sock_open) {
            if (b.len < sizeof(b.buf))
                sock_events |= POLLIN;
            if (a.len)
                sock_events |= POLLOUT;
            if (sock_events) {
                sock_idx = (int)n;
                pfds[n].fd = sock;
                pfds[n].events = sock_events;
                pfds[n++].revents = 0;
            }
        }
        if (b.len && stdout_open) {
            out_idx = (int)n;
            pfds[n].fd = STDOUT_FILENO;
            pfds[n].events = POLLOUT;
            pfds[n++].revents = 0;
        }
        if (poll(pfds, n, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }
        if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP))) {
            uint8_t tmp[256];
            ssize_t r = read(STDIN_FILENO, tmp, sizeof(tmp));
            if (r > 0)
                term_filter_input(&a, tmp, (size_t)r, &prefix);
            else if (r == 0)
                stdin_open = 0;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                stdin_open = 0;
        }
        if (term_modal)
            continue;
        if (sock_idx >= 0 && (pfds[sock_idx].revents & POLLOUT) && a.len) {
            ssize_t w = write(sock, a.buf + a.off, a.len);
            if (w > 0) {
                a.off += (size_t)w;
                a.len -= (size_t)w;
                if (!a.len)
                    a.off = 0;
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                sock_open = 0;
            }
        }
        if (sock_idx >= 0 && (pfds[sock_idx].revents & (POLLIN | POLLHUP)) && b.len < sizeof(b.buf)) {
            compact_pipebuf(&b);
            ssize_t r = read(sock, b.buf + b.off + b.len, sizeof(b.buf) - b.off - b.len);
            if (r > 0)
                b.len += (size_t)r;
            else if (r == 0)
                sock_open = 0;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                sock_open = 0;
        }
        if (out_idx >= 0 && (pfds[out_idx].revents & POLLOUT) && b.len) {
            ssize_t w = write(STDOUT_FILENO, b.buf + b.off, b.len);
            if (w > 0) {
                b.off += (size_t)w;
                b.len -= (size_t)w;
                if (!b.len)
                    b.off = 0;
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                stdout_open = 0;
            }
        }
        if (sock_idx >= 0 && (pfds[sock_idx].revents & (POLLERR | POLLNVAL)))
            sock_open = 0;
    }
    return 0;
}

static int cmd_connect(const char *path, int raw_term, const char *remote_dir, int channel, int verbose)
{
    int fd = connect_unix(path);

    if (fd < 0)
        return 1;
    if (raw_term && isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
            saved_termios_valid = 1;
            if (set_raw_termios() < 0)
                perror("tcsetattr raw");
            atexit(restore_termios);
            signal(SIGINT, on_signal);
            signal(SIGTERM, on_signal);
            signal(SIGHUP, on_signal);
        }
    }
    return raw_term ? term_bridge_loop(fd, remote_dir, channel, verbose) : bridge_loop(fd);
}

static int full_write(int fd, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t w = write(fd, buf, len);
        if (w > 0) {
            buf += w;
            len -= (size_t)w;
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int full_read(int fd, uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t r = read(fd, buf, len);
        if (r > 0) {
            buf += r;
            len -= (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int send_record(int fd, uint8_t type, uint8_t flags, const uint8_t *payload, uint32_t len)
{
    uint8_t rec[LXF_MAX_RECORD];
    size_t out_len;
    if (lxf_encode_record(type, flags, payload, len, rec, sizeof(rec), &out_len) < 0)
        return -1;
    return full_write(fd, rec, out_len);
}

static int recv_record(int fd, uint8_t *type, uint8_t *flags, uint8_t *payload, uint32_t *len)
{
    uint8_t hdr[LXF_HEADER_LEN];
    if (full_read(fd, hdr, sizeof(hdr)) < 0)
        return -1;
    if (lxf_decode_header(hdr, sizeof(hdr), type, flags, len) < 0)
        return -1;
    if (*len && full_read(fd, payload, *len) < 0)
        return -1;
    return 0;
}

static int parse_mode(const char *s, mode_t *mode)
{
    char *end;
    long v = strtol(s, &end, 8);
    if (*end || v < 0 || v > 07777)
        return -1;
    *mode = (mode_t)v;
    return 0;
}

static int make_temp_path(const char *final, char *tmp, size_t cap)
{
    const char *slash = strrchr(final, '/');
    size_t dir_len = slash ? (size_t)(slash - final) : 1;
    const char *base = slash ? slash + 1 : final;
    if (!slash || final[0] != '/' || base[0] == 0)
        return -1;
    if (snprintf(tmp, cap, "%.*s/.%s.lmux-tmp-%ld-%ld",
                 (int)dir_len, final, base, (long)getpid(), (long)time(NULL)) >= (int)cap)
        return -1;
    return 0;
}

static int read_error_payload(const uint8_t *payload, uint32_t len)
{
    uint32_t code = LXF_E_PROTOCOL;
    char msg[129];
    size_t n = 0;
    if (len >= 4) {
        code = lmux_get_le32(payload);
        n = len - 4;
        if (n > sizeof(msg) - 1)
            n = sizeof(msg) - 1;
        memcpy(msg, payload + 4, n);
    }
    msg[n] = 0;
    fprintf(stderr, "TARGET error: %s%s%s\n", lxf_strerror_code(code), n ? ": " : "", n ? msg : "");
    return 1;
}

static int query_target_cwd(const char *sock_path, int channel, char *out, size_t cap)
{
    int fd;
    uint8_t req[1], payload[LXF_MAX_PAYLOAD], type, flags;
    uint32_t len;

    if (channel < 1 || channel > 2)
        return -1;
    fd = connect_unix_mode(sock_path ? sock_path : LXF_DEFAULT_SOCKET, 1);
    if (fd < 0)
        return -1;
    req[0] = (uint8_t)channel;
    if (send_record(fd, LXF_T_CWD_REQUEST, 0, req, sizeof(req)) < 0 ||
        recv_record(fd, &type, &flags, payload, &len) < 0) {
        close(fd);
        return -1;
    }
    (void)flags;
    close(fd);
    if (type != LXF_T_CWD_REPLY || len < 2 || len >= cap || memchr(payload, 0, len) == NULL)
        return -1;
    memcpy(out, payload, len);
    out[len - 1] = 0;
    if (!valid_target_base(out))
        return -1;
    return 0;
}

static int select_target_base(const char *sock_path, const char *configured_dir, int channel,
                              char *out, size_t cap, enum target_base_source *source)
{
    const char *env;

    if (configured_dir && *configured_dir) {
        if (!valid_target_base(configured_dir) || copy_path(out, cap, configured_dir) < 0)
            return -1;
        if (source)
            *source = TARGET_BASE_CONFIG;
        return 0;
    }

    env = getenv("LMUX_REMOTE_DIR");
    if (env && *env && valid_target_base(env) && copy_path(out, cap, env) == 0) {
        if (source)
            *source = TARGET_BASE_ENV;
        return 0;
    }

    if (channel >= 1 && channel <= 2 && query_target_cwd(sock_path, channel, out, cap) == 0) {
        if (source)
            *source = TARGET_BASE_CWD;
        return 0;
    }

    if (copy_path(out, cap, "/root") < 0)
        return -1;
    if (source)
        *source = TARGET_BASE_FALLBACK;
    return 0;
}

static int resolve_target_path(const struct transfer_config *cfg, const char *in, const char *default_leaf,
                               char *out, size_t cap)
{
    char base[LXF_PATH_MAX];
    enum target_base_source source;

    if (in && is_abs_path(in))
        return copy_path(out, cap, in);
    if (select_target_base(cfg->sock_path, cfg->remote_dir, cfg->channel, base, sizeof(base), &source) < 0) {
        fprintf(stderr, "bad TARGET base directory\n");
        return -1;
    }
    if (!in)
        in = default_leaf;
    if (!in || !*in)
        return -1;
    if (join_path(out, cap, base, in) < 0) {
        fprintf(stderr, "TARGET path too long\n");
        return -1;
    }
    return 0;
}

static int resolve_host_path(const char *in, char *out, size_t cap)
{
    if (!in)
        return -1;
    if (strcmp(in, "-") == 0)
        return copy_path(out, cap, in);
    if (is_abs_path(in))
        return copy_path(out, cap, in);
    if (join_path(out, cap, host_cwd, in) < 0) {
        fprintf(stderr, "HOST path too long\n");
        return -1;
    }
    return 0;
}

static int transfer_put(const struct transfer_config *cfg, const char *host_src_arg, const char *target_dst_arg)
{
    int fd, in_fd = -1;
    struct stat st;
    uint64_t size = 0, sent = 0;
    uint32_t flags = 0;
    uint8_t payload[LXF_MAX_PAYLOAD], rec_payload[LXF_MAX_PAYLOAD], digest[32], rdigest[32];
    struct sha256_ctx sha;
    struct progress_state progress;
    uint8_t type, rflags;
    uint32_t len;
    char hex[65], host_src[LXF_PATH_MAX], target_dst[LXF_PATH_MAX];
    mode_t mode = cfg->mode_set ? cfg->mode : 0644;

    if (resolve_host_path(host_src_arg, host_src, sizeof(host_src)) < 0)
        return 1;
    if (strcmp(host_src, "-") == 0 && !target_dst_arg) {
        fprintf(stderr, "TARGET destination is required when HOST source is stdin\n");
        return 1;
    }
    if (resolve_target_path(cfg, target_dst_arg, path_basename(host_src), target_dst, sizeof(target_dst)) < 0)
        return 1;
    if (strcmp(host_src, "-") == 0) {
        in_fd = STDIN_FILENO;
    } else {
        in_fd = open(host_src, O_RDONLY);
        if (in_fd < 0) {
            perror(host_src);
            return 1;
        }
        if (fstat(in_fd, &st) < 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "%s is not a regular file\n", host_src);
            close(in_fd);
            return 1;
        }
        size = (uint64_t)st.st_size;
        flags |= LXF_F_SIZE_KNOWN;
        if (!cfg->mode_set)
            mode = st.st_mode & 0777;
    }

    fd = connect_unix(cfg->sock_path ? cfg->sock_path : LXF_DEFAULT_SOCKET);
    if (fd < 0) {
        if (in_fd > STDERR_FILENO) close(in_fd);
        return 1;
    }
    lmux_put_le32(payload, mode & 0777);
    lmux_put_le64(payload + 4, size);
    lmux_put_le32(payload + 12, flags);
    if (strlen(target_dst) + 1 > sizeof(payload) - 16) {
        fprintf(stderr, "TARGET path too long\n");
        return 1;
    }
    strcpy((char *)payload + 16, target_dst);
    if (send_record(fd, LXF_T_PUT_REQUEST, 0, payload, (uint32_t)(16 + strlen(target_dst) + 1)) < 0)
        return 1;
    if (recv_record(fd, &type, &rflags, rec_payload, &len) < 0)
        return 1;
    (void)rflags;
    if (type == LXF_T_ERROR)
        return read_error_payload(rec_payload, len);
    if (type != LXF_T_OK) {
        fprintf(stderr, "unexpected file response\n");
        return 1;
    }

    sha256_init(&sha);
    progress_init(&progress, "PUT", (flags & LXF_F_SIZE_KNOWN) != 0, size, cfg->verbose);
    progress_update(&progress, 0, 1);
    for (;;) {
        ssize_t r = read(in_fd, payload, sizeof(payload));
        if (r > 0) {
            sha256_update(&sha, payload, (size_t)r);
            sent += (uint64_t)r;
            if (send_record(fd, LXF_T_DATA, 0, payload, (uint32_t)r) < 0)
                return 1;
            progress_update(&progress, sent, 0);
        } else if (r == 0) {
            break;
        } else if (errno != EINTR) {
            perror(host_src);
            progress_finish(&progress, sent);
            return 1;
        }
    }
    progress_finish(&progress, sent);
    sha256_final(&sha, digest);
    lmux_put_le64(payload, sent);
    memcpy(payload + 8, digest, 32);
    if (send_record(fd, LXF_T_DONE, 0, payload, 40) < 0)
        return 1;
    if (recv_record(fd, &type, &rflags, rec_payload, &len) < 0)
        return 1;
    if (type == LXF_T_ERROR)
        return read_error_payload(rec_payload, len);
    if (type != LXF_T_OK || len < 40) {
        fprintf(stderr, "bad PUT completion response\n");
        return 1;
    }
    if (lmux_get_le64(rec_payload) != sent || memcmp(rec_payload + 8, digest, 32) != 0) {
        fprintf(stderr, "PUT verification mismatch\n");
        return 1;
    }
    memcpy(rdigest, rec_payload + 8, 32);
    sha256_hex(rdigest, hex);
    fprintf(stderr, "put: %llu bytes sha256=%s HOST:%s -> TARGET:%s\n",
            (unsigned long long)sent, hex, host_src, target_dst);
    if (in_fd > STDERR_FILENO)
        close(in_fd);
    close(fd);
    return 0;
}

static int transfer_get(const struct transfer_config *cfg, const char *target_src_arg, const char *host_dst_arg)
{
    int fd, out_fd = -1, to_stdout = 0;
    uint8_t payload[LXF_MAX_PAYLOAD], rec_payload[LXF_MAX_PAYLOAD], digest[32];
    uint8_t type, flags;
    uint32_t len;
    uint64_t expected = 0, got = 0;
    mode_t mode = 0644;
    struct sha256_ctx sha;
    struct progress_state progress;
    char tmp[LXF_PATH_MAX], hex[65], target_src[LXF_PATH_MAX], host_dst[LXF_PATH_MAX];

    if (resolve_target_path(cfg, target_src_arg, NULL, target_src, sizeof(target_src)) < 0)
        return 1;
    if (host_dst_arg) {
        if (resolve_host_path(host_dst_arg, host_dst, sizeof(host_dst)) < 0)
            return 1;
    } else if (join_path(host_dst, sizeof(host_dst), host_cwd, path_basename(target_src)) < 0) {
        fprintf(stderr, "HOST destination path too long\n");
        return 1;
    }
    fd = connect_unix(cfg->sock_path ? cfg->sock_path : LXF_DEFAULT_SOCKET);
    if (fd < 0)
        return 1;
    if (strlen(target_src) + 1 > sizeof(payload)) {
        fprintf(stderr, "TARGET path too long\n");
        return 1;
    }
    strcpy((char *)payload, target_src);
    if (send_record(fd, LXF_T_GET_REQUEST, 0, payload, (uint32_t)(strlen(target_src) + 1)) < 0)
        return 1;
    if (recv_record(fd, &type, &flags, rec_payload, &len) < 0)
        return 1;
    (void)flags;
    if (type == LXF_T_ERROR)
        return read_error_payload(rec_payload, len);
    if (type != LXF_T_OK || len < 16) {
        fprintf(stderr, "bad GET metadata response\n");
        return 1;
    }
    mode = lmux_get_le32(rec_payload) & 0777;
    expected = lmux_get_le64(rec_payload + 4);
    if (strcmp(host_dst, "-") == 0) {
        out_fd = STDOUT_FILENO;
        to_stdout = 1;
    } else {
        struct stat st;
        if (lstat(host_dst, &st) == 0 && !S_ISREG(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a regular file\n", host_dst);
            return 1;
        }
        if (make_temp_path(host_dst, tmp, sizeof(tmp)) < 0) {
            fprintf(stderr, "bad HOST path\n");
            return 1;
        }
        out_fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (out_fd < 0) {
            perror(tmp);
            return 1;
        }
    }
    sha256_init(&sha);
    progress_init(&progress, "GET", 1, expected, cfg->verbose);
    progress_update(&progress, 0, 1);
    for (;;) {
        if (recv_record(fd, &type, &flags, rec_payload, &len) < 0)
            goto fail;
        if (type == LXF_T_ERROR) {
            read_error_payload(rec_payload, len);
            goto fail;
        }
        if (type == LXF_T_DATA) {
            if (full_write(out_fd, rec_payload, len) < 0)
                goto fail;
            sha256_update(&sha, rec_payload, len);
            got += len;
            progress_update(&progress, got, 0);
        } else if (type == LXF_T_DONE) {
            if (len < 40)
                goto fail;
            progress_finish(&progress, got);
            sha256_final(&sha, digest);
            if (lmux_get_le64(rec_payload) != got || got != expected ||
                memcmp(rec_payload + 8, digest, 32) != 0) {
                fprintf(stderr, "GET verification mismatch\n");
                goto fail;
            }
            if (!to_stdout) {
                fchmod(out_fd, mode);
                fsync(out_fd);
                close(out_fd);
                out_fd = -1;
                if (rename(tmp, host_dst) < 0) {
                    perror(host_dst);
                    unlink(tmp);
                    return 1;
                }
            }
            sha256_hex(digest, hex);
            fprintf(stderr, "get: %llu bytes sha256=%s TARGET:%s -> HOST:%s\n",
                    (unsigned long long)got, hex, target_src, host_dst);
            close(fd);
            return 0;
        } else {
            goto fail;
        }
    }
fail:
    progress_finish(&progress, got);
    if (!to_stdout && out_fd >= 0) {
        close(out_fd);
        unlink(tmp);
    }
    close(fd);
    return 1;
}

static int cmd_put(int argc, char **argv)
{
    struct transfer_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sock_path = LXF_DEFAULT_SOCKET;
    cfg.channel = 1;
    cfg.mode = 0644;
    while (argc > 0 && argv[0][0] == '-') {
        if ((strcmp(argv[0], "-S") == 0 || strcmp(argv[0], "--socket") == 0) && argc >= 2) {
            cfg.sock_path = argv[1]; argv += 2; argc -= 2;
        } else if (strcmp(argv[0], "--remote-dir") == 0 && argc >= 2) {
            cfg.remote_dir = argv[1]; argv += 2; argc -= 2;
        } else if (strcmp(argv[0], "--channel") == 0 && argc >= 2) {
            if (parse_channel_arg(argv[1], &cfg.channel) < 0) {
                fprintf(stderr, "invalid TARGET shell channel: %s\n", argv[1]);
                return 1;
            }
            argv += 2; argc -= 2;
        } else if (strcmp(argv[0], "-v") == 0 || strcmp(argv[0], "--verbose") == 0) {
            cfg.verbose = 1; argv++; argc--;
        } else if (strcmp(argv[0], "--mode") == 0 && argc >= 2) {
            if (parse_mode(argv[1], &cfg.mode) < 0) {
                fprintf(stderr, "bad mode: %s\n", argv[1]);
                return 1;
            }
            cfg.mode_set = 1; argv += 2; argc -= 2;
        } else {
            break;
        }
    }
    if (argc < 1 || argc > 2) {
        usage(stderr);
        return 1;
    }
    return transfer_put(&cfg, argv[0], argc == 2 ? argv[1] : NULL);
}

static int cmd_get(int argc, char **argv)
{
    struct transfer_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sock_path = LXF_DEFAULT_SOCKET;
    cfg.channel = 1;
    while (argc > 0 && argv[0][0] == '-') {
        if ((strcmp(argv[0], "-S") == 0 || strcmp(argv[0], "--socket") == 0) && argc >= 2) {
            cfg.sock_path = argv[1]; argv += 2; argc -= 2;
        } else if (strcmp(argv[0], "--remote-dir") == 0 && argc >= 2) {
            cfg.remote_dir = argv[1]; argv += 2; argc -= 2;
        } else if (strcmp(argv[0], "--channel") == 0 && argc >= 2) {
            if (parse_channel_arg(argv[1], &cfg.channel) < 0) {
                fprintf(stderr, "invalid TARGET shell channel: %s\n", argv[1]);
                return 1;
            }
            argv += 2; argc -= 2;
        } else if (strcmp(argv[0], "-v") == 0 || strcmp(argv[0], "--verbose") == 0) {
            cfg.verbose = 1; argv++; argc--;
        } else {
            break;
        }
    }
    if (argc < 1 || argc > 2) {
        usage(stderr);
        return 1;
    }
    return transfer_get(&cfg, argv[0], argc == 2 ? argv[1] : NULL);
}

static int cmd_exec(int argc, char **argv)
{
    int fd;

    if (argc < 3 || strcmp(argv[1], "--") != 0) {
        usage(stderr);
        return 1;
    }
    fd = connect_unix(argv[1]);
    if (fd < 0)
        return 1;
    if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    if (fd > STDERR_FILENO)
        close(fd);
    execvp(argv[2], &argv[2]);
    perror(argv[2]);
    return 127;
}

#ifndef LMUX_NO_MAIN
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    init_host_cwd();

    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("lmux %s\n", LMUX_BUILD_VERSION);
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 1 : 0;
    }
    if (strcmp(argv[1], "connect") == 0) {
        if (argc != 3) {
            usage(stderr);
            return 1;
        }
        return cmd_connect(argv[2], 0, NULL, 0, 0);
    }
    if (strcmp(argv[1], "term") == 0) {
        const char *remote_dir = NULL;
        const char *arg = NULL;
        const char *sock;
        int i, channel = 0, channel_set = 0, verbose = 0;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--remote-dir") == 0) {
                if (++i >= argc) {
                    usage(stderr);
                    return 1;
                }
                remote_dir = argv[i];
            } else if (strcmp(argv[i], "--channel") == 0) {
                if (++i >= argc || parse_channel_arg(argv[i], &channel) < 0) {
                    usage(stderr);
                    return 1;
                }
                channel_set = 1;
            } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                verbose = 1;
            } else if (!arg) {
                arg = argv[i];
            } else {
                usage(stderr);
                return 1;
            }
        }
        if (is_pure_numeric(arg) && strcmp(arg, "1") != 0 && strcmp(arg, "2") != 0) {
            fprintf(stderr, "invalid console number: %s\n", arg);
            return 1;
        }
        if (!channel_set) {
            channel = default_channel_for_console_arg(arg);
        }
        sock = console_socket_for_arg(arg);
        return cmd_connect(sock, 1, remote_dir, channel, verbose);
    }
    if (strcmp(argv[1], "exec") == 0)
        return cmd_exec(argc - 2, argv + 2);
    if (strcmp(argv[1], "put") == 0)
        return cmd_put(argc - 2, argv + 2);
    if (strcmp(argv[1], "get") == 0)
        return cmd_get(argc - 2, argv + 2);

    usage(stderr);
    return 1;
}
#endif
