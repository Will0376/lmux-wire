#include "lxf.h"
#include "carrier_manager.h"
#include "host_control_structured.h"
#include "services.h"
#include "zeromux/zeromux.h"
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
static const char *term_label;
static char host_cwd[LXF_PATH_MAX] = ".";
static volatile sig_atomic_t term_winch_pending;

#define LMUX_DEFAULT_CONTROL_SOCKET "/run/lmux/control.sock"
#define LMUX_DEFAULT_STRUCTURED_SOCKET "/run/lmux/control-v2.sock"

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

static int full_write(int fd, const uint8_t *buf, size_t len);

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

static const char *console_label_for_arg(const char *arg)
{
    if (!arg || strcmp(arg, "1") == 0)
        return "console1";
    if (strcmp(arg, "2") == 0)
        return "console2";
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

static void on_winch(int sig)
{
    (void)sig;
    term_winch_pending = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "lmux %s (%s)\n"
        "Usage:\n"
        "  lmux connect SOCKET\n"
        "  lmux status [--control SOCKET]\n"
        "  lmux carrier status [--control SOCKET]\n"
        "  lmux carrier switch NAME [--mode raw|dirty] [--timeout-ms N] [--control SOCKET]\n"
        "  lmux carrier console NAME [--control SOCKET]\n"
        "  lmux deploy FILE [--restart-supervisor] [--timeout-ms N] [--control SOCKET]\n"
        "  lmux deploy --restart-supervisor [--control SOCKET]\n"
        "  lmux boot info [--control SOCKET]\n"
        "  lmux boot worker-status [--control SOCKET]\n"
        "  lmux boot upload FILE [--control SOCKET]\n"
        "  lmux boot activate SHA256 [--timeout-ms N] [--control SOCKET]\n"
        "  lmux boot update-status [--control SOCKET]\n"
        "  lmux boot confirm ACTIVATION_ID [--control SOCKET]\n"
        "  lmux boot rollback ACTIVATION_ID [--control SOCKET]\n"
        "  lmux boot supervisor-install SHA256 [--control SOCKET]\n"
        "  lmux boot supervisor-restart SHA256 [--control SOCKET]\n"
        "  lmux zeromux probe --tty DEVICE [serial options]\n"
        "  lmux zeromux send LOCAL_FILE TARGET_TEMP --tty DEVICE [serial options]\n"
        "  lmux zeromux install FILE --tty DEVICE --install-dir TARGET_DIR --lmux-carrier TARGET_CARRIER [serial options]\n"
        "  lmux term [-v|--verbose] [--remote-dir DIR] [--channel 1|2] [1|2|SOCKET]\n"
        "  lmux stream [--control SOCKET] -- /absolute/target/program [ARGS...]\n"
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
        "  lmux put ./lmuxd /tmp/lmuxd\n"
        "  lmux get log.log\n"
        "  lmux get /tmp/core.bin ./core.bin\n"
        "  tar -cf - DIR | lmux stream -- /bin/tar -xf - -C /tmp\n"
        "  lmux stream -- /bin/dd if=/dev/device bs=65536 > image.bin\n"
        "\n"
        "lmux term Ctrl-T commands:\n"
        "  Ctrl-T ?        help\n"
        "  Ctrl-T q/.      detach\n"
        "  Ctrl-T Ctrl-T   send literal Ctrl-T\n"
        "  Ctrl-T p        upload file   HOST -> TARGET\n"
        "  Ctrl-T g        download file TARGET -> HOST\n"
        "\n"
        "Zeromux serial options:\n"
        "  --baud N                    bootstrap UART baud (default 115200)\n"
        "  --data-bits 5|6|7|8         data bits (default 8)\n"
        "  --parity none|even|odd      parity (default none)\n"
        "  --stop-bits 1|2             stop bits (default 1)\n"
        "  --flow none|rtscts|xonxoff  flow control (default none)\n"
        "  --timeout-ms N              transaction timeout\n",
        LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
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

static int control_request(const char *path, const char *req, int print_reply, int quiet)
{
    int fd;
    fd = connect_unix_mode(path ? path : LMUX_DEFAULT_CONTROL_SOCKET, quiet);
    if (fd < 0)
        return -1;
    if (full_write(fd, (const uint8_t *)req, strlen(req)) < 0) {
        if (!quiet)
            perror("control write");
        close(fd);
        return -1;
    }
    if (print_reply) {
        uint8_t buf[512];
        ssize_t r;
        while ((r = read(fd, buf, sizeof(buf))) > 0)
            (void)full_write(STDOUT_FILENO, buf, (size_t)r);
    }
    close(fd);
    return 0;
}

static int read_control_line(int fd, char *out, size_t cap)
{
    size_t len = 0;
    while (len + 1 < cap) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            out[len++] = ch;
            out[len] = 0;
            if (ch == '\n')
                return 0;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return -1;
}

static int boot_upload_file(const char *control, const char *path)
{
    int fd = -1, cfd = -1, rc = 1;
    struct stat st;
    char req[128], line[256];
    uint8_t buf[4096];
    uint64_t done = 0;
    int saw_ok = 0, saw_err = 0;
    struct progress_state prog;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        fprintf(stderr, "lmux boot upload: not a regular file: %s\n", path);
        goto out;
    }
    cfd = connect_unix(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "BOOT UPLOAD %llu\n",
             (unsigned long long)st.st_size);
    if (full_write(cfd, (const uint8_t *)req, strlen(req)) < 0) {
        perror("control write");
        goto out;
    }
    if (read_control_line(cfd, line, sizeof(line)) < 0) {
        fprintf(stderr, "lmux boot upload: no READY\n");
        goto out;
    }
    if (strncmp(line, "READY", 5) != 0) {
        fputs(line, stderr);
        goto out;
    }
    progress_init(&prog, "upload", 1, (uint64_t)st.st_size, 1);
    while (done < (uint64_t)st.st_size) {
        size_t want = sizeof(buf);
        ssize_t r;
        if ((uint64_t)want > (uint64_t)st.st_size - done)
            want = (size_t)((uint64_t)st.st_size - done);
        r = read(fd, buf, want);
        if (r > 0) {
            if (full_write(cfd, buf, (size_t)r) < 0) {
                perror("control upload write");
                goto out;
            }
            done += (uint64_t)r;
            progress_update(&prog, done, 0);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        fprintf(stderr, "lmux boot upload: read failed\n");
        goto out;
    }
    progress_finish(&prog, done);
    while (read_control_line(cfd, line, sizeof(line)) == 0) {
        if (strncmp(line, "OK UPLOAD", 9) == 0)
            saw_ok = 1;
        if (strncmp(line, "ERR", 3) == 0)
            saw_err = 1;
        fputs(line, stdout);
    }
    rc = saw_ok && !saw_err ? 0 : 1;
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int deploy_file(const char *control, const char *path, unsigned timeout_ms,
                       int restart_supervisor)
{
    int fd = -1, cfd = -1, rc = 1;
    struct stat st;
    char req[160], line[512];
    uint8_t buf[4096];
    uint64_t done = 0;
    int saw_ok = 0, saw_err = 0;
    struct progress_state prog;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        fprintf(stderr, "lmux deploy: not a regular file: %s\n", path);
        goto out;
    }
    cfd = connect_unix(control);
    if (cfd < 0)
        goto out;
    snprintf(req, sizeof(req), "DEPLOY %llu %u %u\n",
             (unsigned long long)st.st_size, timeout_ms,
             restart_supervisor ? 1u : 0u);
    if (full_write(cfd, (const uint8_t *)req, strlen(req)) < 0) {
        perror("control write");
        goto out;
    }
    if (read_control_line(cfd, line, sizeof(line)) < 0) {
        fprintf(stderr, "lmux deploy: no READY\n");
        goto out;
    }
    if (strncmp(line, "READY", 5) != 0) {
        fputs(line, stderr);
        goto out;
    }
    progress_init(&prog, "deploy upload", 1, (uint64_t)st.st_size, 1);
    while (done < (uint64_t)st.st_size) {
        size_t want = sizeof(buf);
        ssize_t r;
        if ((uint64_t)want > (uint64_t)st.st_size - done)
            want = (size_t)((uint64_t)st.st_size - done);
        r = read(fd, buf, want);
        if (r > 0) {
            if (full_write(cfd, buf, (size_t)r) < 0) {
                perror("control deploy write");
                goto out;
            }
            done += (uint64_t)r;
            progress_update(&prog, done, 0);
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        fprintf(stderr, "lmux deploy: read failed\n");
        goto out;
    }
    progress_finish(&prog, done);
    while (read_control_line(cfd, line, sizeof(line)) == 0) {
        if (strncmp(line, "OK DEPLOY", 9) == 0)
            saw_ok = 1;
        if (strncmp(line, "ERR", 3) == 0)
            saw_err = 1;
        fputs(line, stdout);
    }
    rc = saw_ok && !saw_err ? 0 : 1;
out:
    if (cfd >= 0)
        close(cfd);
    if (fd >= 0)
        close(fd);
    return rc;
}

static int deploy_restart_supervisor(const char *control)
{
    return control_request(control, "DEPLOY_RESTART\n", 1, 0) < 0 ? 1 : 0;
}

static int send_winsize_control(int channel)
{
    struct winsize ws;
    char req[96];
    if (channel < 1 || channel > 2 || !isatty(STDIN_FILENO))
        return 0;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_row == 0 || ws.ws_col == 0)
        return -1;
    snprintf(req, sizeof(req), "WINSIZE %d %u %u %u %u\n", channel,
             (unsigned int)ws.ws_row, (unsigned int)ws.ws_col,
             (unsigned int)ws.ws_xpixel, (unsigned int)ws.ws_ypixel);
    return control_request(LMUX_DEFAULT_CONTROL_SOCKET, req, 0, 1);
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
        "Ctrl-T q/.     detach\r\n"
        "Ctrl-T Ctrl-T  send literal Ctrl-T\r\n"
        "Ctrl-T p       upload file   HOST -> TARGET\r\n"
        "Ctrl-T g       download file TARGET -> HOST\r\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

static void term_print_attached(const char *label)
{
    /* Output processing is disabled in raw mode, so emit CRLF explicitly. */
    fprintf(stderr, "[attached: %s]\r\n", label ? label : "console1");
    fflush(stderr);
}

static void term_print_waiting(const char *label)
{
    /* A local Unix socket accept is not proof of a remote LMUX stream. */
    fprintf(stderr, "[waiting: %s]\r\n", label ? label : "console1");
    fflush(stderr);
}

static void term_print_detached(const char *label)
{
    fprintf(stderr, "\r\n[detached: %s]\r\n", label ? label : "console1");
    fflush(stderr);
}

static void term_print_disconnected(const char *label)
{
    fprintf(stderr, "\r\n[disconnected: %s]\r\n", label ? label : "console1");
    fflush(stderr);
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
                term_print_detached(term_label);
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

static int term_bridge_loop(int sock, const char *remote_dir, int channel,
                            int verbose, const char *label)
{
    struct pipebuf a, b;
    int stdin_open = 1, sock_open = 1, stdout_open = 1;
    int prefix = 0;
    int remote_attached = 0;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    term_detach = 0;
    term_modal = 0;
    term_label = label;
    term_winch_pending = 1;
    term_print_waiting(label);
    if (set_nonblock(STDIN_FILENO) < 0 || set_nonblock(STDOUT_FILENO) < 0 || set_nonblock(sock) < 0) {
        perror("fcntl");
        return 1;
    }

    while (!term_detach && (stdin_open || a.len) && (sock_open || b.len) && stdout_open) {
        struct pollfd pfds[3];
        nfds_t n = 0;
        int stdin_idx = -1, sock_idx = -1, out_idx = -1;
        short sock_events = 0;

        if (term_winch_pending) {
            term_winch_pending = 0;
            (void)send_winsize_control(channel);
        }

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
            if (r > 0) {
                if (!remote_attached) {
                    term_print_attached(label);
                    remote_attached = 1;
                }
                b.len += (size_t)r;
            } else if (r == 0)
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
    if (!term_detach && !sock_open)
        term_print_disconnected(label);
    return 0;
}

static int cmd_connect(const char *path, int raw_term, const char *remote_dir,
                       int channel, int verbose, const char *label)
{
    int fd = raw_term ? connect_unix_mode(path, 1) : connect_unix(path);

    if (fd < 0) {
        if (raw_term)
            fprintf(stderr, "lmux term: %s: connect: %s\n",
                    label ? label : path, strerror(errno));
        return 1;
    }
    if (raw_term && isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
            saved_termios_valid = 1;
            if (set_raw_termios() < 0)
                perror("tcsetattr raw");
            atexit(restore_termios);
            signal(SIGINT, on_signal);
            signal(SIGTERM, on_signal);
            signal(SIGHUP, on_signal);
            signal(SIGWINCH, on_winch);
        }
    }
    if (raw_term) {
        return term_bridge_loop(fd, remote_dir, channel, verbose, label ? label : path);
    }
    return bridge_loop(fd);
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
    fd = connect_unix(argv[0]);
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

#define STREAM_QUEUE_CAP 65536u
#define STREAM_STDIN_CHUNK 1024u
#define STREAM_START_TIMEOUT_MS 30000
#define STREAM_CLEANUP_TIMEOUT_MS 5000

struct stream_queue {
    uint8_t data[STREAM_QUEUE_CAP];
    size_t pos;
    size_t len;
};

struct stream_frame {
    uint16_t type;
    uint16_t flags;
    uint64_t req;
    uint32_t len;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
};

struct stream_client {
    int fd;
    uint64_t ref;
    uint64_t next_req;
    uint8_t tx[LMUX_HC2_TX_CAP];
    size_t tx_pos;
    size_t tx_len;
    uint8_t rx[LMUX_HC2_RX_CAP];
    size_t rx_len;
    uint8_t stdin_pending[STREAM_STDIN_CHUNK];
    size_t stdin_pending_len;
    uint64_t stdin_req;
    uint64_t stdin_req_ready_gen;
    uint64_t close_req;
    uint64_t release_req;
    uint64_t ready_gen;
    int stdin_blocked;
    int stdin_open;
    int stdin_eof;
    int stdin_close_queued;
    int stdin_closed_remote;
    int done_seen;
    int lost_seen;
    int released;
    int cleanup_requested;
    int cleanup_signal_sent;
    int cleanup_kill_sent;
    long long cleanup_deadline;
    int exit_code;
    int term_signal;
    int failure_rc;
    int stdout_closed;
    int stderr_closed;
    struct stream_queue outq;
    struct stream_queue errq;
};

static volatile sig_atomic_t stream_sigint_pending;
static volatile sig_atomic_t stream_sigterm_pending;

static void stream_signal_handler(int sig)
{
    if (sig == SIGINT)
        stream_sigint_pending = 1;
    else if (sig == SIGTERM)
        stream_sigterm_pending = 1;
}

static int stream_set_nonblock(int fd, int *old_flags)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (old_flags)
        *old_flags = flags;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static void stream_restore_flags(int fd, int old_flags, int valid)
{
    if (valid)
        (void)fcntl(fd, F_SETFL, old_flags);
}

static size_t stream_q_len(const struct stream_queue *q)
{
    return q->len - q->pos;
}

static size_t stream_q_space(struct stream_queue *q)
{
    if (q->pos && q->pos == q->len) {
        q->pos = 0;
        q->len = 0;
    } else if (q->pos && q->len == sizeof(q->data)) {
        memmove(q->data, q->data + q->pos, q->len - q->pos);
        q->len -= q->pos;
        q->pos = 0;
    }
    return sizeof(q->data) - q->len;
}

static int stream_q_append(struct stream_queue *q, const uint8_t *p, size_t n)
{
    if (stream_q_space(q) < n)
        return -1;
    memcpy(q->data + q->len, p, n);
    q->len += n;
    return 0;
}

static int stream_append_frame(uint8_t *out, size_t cap, size_t *off,
                               uint16_t type, uint64_t req,
                               const uint8_t *payload, uint32_t len)
{
    uint8_t *p;
    if (len > LMUX_HC2_MAX_PAYLOAD || *off + LMUX_HC2_HEADER_LEN + len > cap)
        return -1;
    p = out + *off;
    lmux_put_le32(p, LMUX_HC2_MAGIC);
    lmux_put_le16(p + 4, LMUX_HC2_VERSION);
    lmux_put_le16(p + 6, type);
    lmux_put_le16(p + 8, 0);
    lmux_put_le16(p + 10, 0);
    lmux_put_le64(p + 12, req);
    lmux_put_le32(p + 20, len);
    if (len)
        memcpy(p + LMUX_HC2_HEADER_LEN, payload, len);
    *off += LMUX_HC2_HEADER_LEN + len;
    return 0;
}

static int stream_queue_frame(struct stream_client *c, uint16_t type,
                              uint64_t req, const uint8_t *payload,
                              uint32_t len)
{
    size_t used, need = LMUX_HC2_HEADER_LEN + len;
    if (len > LMUX_HC2_MAX_PAYLOAD || need > sizeof(c->tx))
        return -1;
    if (c->tx_pos) {
        if (c->tx_pos < c->tx_len)
            memmove(c->tx, c->tx + c->tx_pos, c->tx_len - c->tx_pos);
        c->tx_len -= c->tx_pos;
        c->tx_pos = 0;
    }
    used = c->tx_len - c->tx_pos;
    if (sizeof(c->tx) - used < need)
        return -1;
    return stream_append_frame(c->tx, sizeof(c->tx), &c->tx_len,
                               type, req, payload, len);
}

static int stream_flush_tx(struct stream_client *c)
{
    while (c->tx_pos < c->tx_len) {
        ssize_t w = write(c->fd, c->tx + c->tx_pos, c->tx_len - c->tx_pos);
        if (w > 0) {
            c->tx_pos += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return -1;
    }
    c->tx_pos = 0;
    c->tx_len = 0;
    return 0;
}

static int stream_read_frame_blocking(int fd, struct stream_frame *fr,
                                      int timeout_ms)
{
    uint8_t hdr[LMUX_HC2_HEADER_LEN];
    size_t got = 0;
    uint32_t len;
    long long end = (long long)time(NULL) * 1000 + timeout_ms;
    while (got < sizeof(hdr)) {
        struct pollfd pfd;
        int rem = timeout_ms;
        ssize_t r;
        if (timeout_ms >= 0) {
            long long now = (long long)time(NULL) * 1000;
            rem = now >= end ? 0 : (int)(end - now);
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, rem) <= 0)
            return -1;
        r = read(fd, hdr + got, sizeof(hdr) - got);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        return -1;
    }
    if (lmux_get_le32(hdr) != LMUX_HC2_MAGIC ||
        lmux_get_le16(hdr + 4) != LMUX_HC2_VERSION)
        return -1;
    len = lmux_get_le32(hdr + 20);
    if (len > LMUX_HC2_MAX_PAYLOAD)
        return -1;
    fr->type = lmux_get_le16(hdr + 6);
    fr->flags = lmux_get_le16(hdr + 8);
    fr->req = lmux_get_le64(hdr + 12);
    fr->len = len;
    got = 0;
    while (got < len) {
        struct pollfd pfd;
        int rem = timeout_ms;
        ssize_t r;
        if (timeout_ms >= 0) {
            long long now = (long long)time(NULL) * 1000;
            rem = now >= end ? 0 : (int)(end - now);
        }
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, rem) <= 0)
            return -1;
        r = read(fd, fr->payload + got, len - got);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int stream_parse_one_frame(struct stream_client *c, struct stream_frame *fr)
{
    uint32_t len;
    size_t n;
    if (c->rx_len < LMUX_HC2_HEADER_LEN)
        return 0;
    if (lmux_get_le32(c->rx) != LMUX_HC2_MAGIC ||
        lmux_get_le16(c->rx + 4) != LMUX_HC2_VERSION)
        return -1;
    len = lmux_get_le32(c->rx + 20);
    if (len > LMUX_HC2_MAX_PAYLOAD)
        return -1;
    n = LMUX_HC2_HEADER_LEN + len;
    if (c->rx_len < n)
        return 0;
    fr->type = lmux_get_le16(c->rx + 6);
    fr->flags = lmux_get_le16(c->rx + 8);
    fr->req = lmux_get_le64(c->rx + 12);
    fr->len = len;
    if (len)
        memcpy(fr->payload, c->rx + LMUX_HC2_HEADER_LEN, len);
    if (n < c->rx_len)
        memmove(c->rx, c->rx + n, c->rx_len - n);
    c->rx_len -= n;
    return 1;
}

static int stream_read_hc2(struct stream_client *c)
{
    ssize_t r;
    if (c->rx_len == sizeof(c->rx))
        return -1;
    do {
        r = read(c->fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len);
    } while (r < 0 && errno == EINTR);
    if (r > 0) {
        c->rx_len += (size_t)r;
        return 0;
    }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return -1;
}

static uint32_t stream_error_code(const struct stream_frame *fr)
{
    if (fr->type != LMUX_HC2_ERROR || fr->len < 4)
        return LMUX_HC2_E_PROTOCOL;
    return lmux_get_le32(fr->payload);
}

static void stream_print_error_frame(const char *prefix, const struct stream_frame *fr)
{
    uint32_t code = stream_error_code(fr);
    uint32_t n = fr->len >= 8 ? lmux_get_le32(fr->payload + 4) : 0;
    fprintf(stderr, "%s: hc2 error %u", prefix, code);
    if (n && n <= fr->len - 8)
        fprintf(stderr, ": %.*s", (int)n, (const char *)fr->payload + 8);
    fputc('\n', stderr);
}

static int stream_put_string(uint8_t *p, size_t cap, size_t *off,
                             const char *s)
{
    size_t n = strlen(s);
    if (n > UINT16_MAX || *off + 2 + n > cap)
        return -1;
    lmux_put_le16(p + *off, (uint16_t)n);
    *off += 2;
    memcpy(p + *off, s, n);
    *off += n;
    return 0;
}

static int stream_start_exec(struct stream_client *c, int argc, char **argv)
{
    uint8_t p[LMUX_HC2_MAX_PAYLOAD];
    size_t off = 0, flen = 0;
    uint16_t i;
    struct stream_frame fr;
    uint8_t frame[LMUX_HC2_HEADER_LEN + LMUX_HC2_MAX_PAYLOAD];

    if (argc < 1 || argc > LMUX_HC2_MAX_ARGC || argv[0][0] != '/') {
        fprintf(stderr, "lmux stream: PROGRAM must be an absolute path\n");
        return -1;
    }
    if (stream_append_frame(frame, sizeof(frame), &flen,
                            LMUX_HC2_CLIENT_HELLO, c->next_req++,
                            NULL, 0) < 0 ||
        full_write(c->fd, frame, flen) < 0 ||
        stream_read_frame_blocking(c->fd, &fr, STREAM_START_TIMEOUT_MS) < 0 ||
        fr.type != LMUX_HC2_SERVER_HELLO) {
        fprintf(stderr, "lmux stream: structured control handshake failed\n");
        return -1;
    }
    if (fr.len < 4 || !(lmux_get_le32(fr.payload) & LMUX_HC2_CAP_EXEC)) {
        fprintf(stderr, "lmux stream: structured exec unsupported\n");
        return -1;
    }
    if (stream_put_string(p, sizeof(p), &off, "") < 0 ||
        off + 2 > sizeof(p))
        return -1;
    lmux_put_le16(p + off, (uint16_t)argc);
    off += 2;
    for (i = 0; i < (uint16_t)argc; i++)
        if (stream_put_string(p, sizeof(p), &off, argv[i]) < 0) {
            fprintf(stderr, "lmux stream: argv too large\n");
            return -1;
        }
    flen = 0;
    if (stream_append_frame(frame, sizeof(frame), &flen,
                            LMUX_HC2_EXEC_START, c->next_req++,
                            p, (uint32_t)off) < 0 ||
        full_write(c->fd, frame, flen) < 0) {
        fprintf(stderr, "lmux stream: start request failed\n");
        return -1;
    }
    for (;;) {
        if (stream_read_frame_blocking(c->fd, &fr, STREAM_START_TIMEOUT_MS) < 0) {
            fprintf(stderr, "lmux stream: start timed out\n");
            return -1;
        }
        if (fr.type == LMUX_HC2_ERROR) {
            stream_print_error_frame("lmux stream: start failed", &fr);
            return -1;
        }
        if (fr.type != LMUX_HC2_EXEC_STARTED || fr.len < 32)
            continue;
        c->ref = lmux_get_le64(fr.payload);
        if (fr.payload[16] == V2_EXEC_STATUS_START_FAILED) {
            fprintf(stderr, "lmux stream: target exec failed");
            if (fr.payload[18] || fr.payload[19])
                fprintf(stderr, ": errno=%u", lmux_get_le16(fr.payload + 18));
            fputc('\n', stderr);
            return -1;
        }
        return 0;
    }
}

static int stream_queue_ref_frame(struct stream_client *c, uint16_t type,
                                  uint64_t req)
{
    uint8_t p[8];
    lmux_put_le64(p, c->ref);
    return stream_queue_frame(c, type, req, p, sizeof(p));
}

static int stream_queue_signal(struct stream_client *c, uint8_t sig)
{
    uint8_t p[9];
    lmux_put_le64(p, c->ref);
    p[8] = sig;
    return stream_queue_frame(c, LMUX_HC2_EXEC_SIGNAL, c->next_req++,
                              p, sizeof(p));
}

static int stream_try_queue_stdin(struct stream_client *c)
{
    uint8_t p[8 + STREAM_STDIN_CHUNK];
    uint64_t req;
    if (!c->stdin_pending_len || c->stdin_req || c->stdin_blocked)
        return 0;
    lmux_put_le64(p, c->ref);
    memcpy(p + 8, c->stdin_pending, c->stdin_pending_len);
    req = c->next_req++;
    if (stream_queue_frame(c, LMUX_HC2_EXEC_STDIN_DATA, req, p,
                           (uint32_t)(8 + c->stdin_pending_len)) < 0)
        return 0;
    c->stdin_req = req;
    c->stdin_req_ready_gen = c->ready_gen;
    return 0;
}

static int stream_try_queue_close_stdin(struct stream_client *c)
{
    uint64_t req;
    if (!c->stdin_eof || c->stdin_pending_len || c->stdin_req ||
        c->stdin_close_queued || c->stdin_closed_remote)
        return 0;
    req = c->next_req++;
    if (stream_queue_ref_frame(c, LMUX_HC2_EXEC_STDIN_CLOSE, req) < 0)
        return 0;
    c->close_req = req;
    c->stdin_close_queued = 1;
    return 0;
}

static int stream_queue_release(struct stream_client *c)
{
    uint64_t req;
    if (c->release_req || c->released || !c->ref)
        return 0;
    req = c->next_req++;
    if (stream_queue_ref_frame(c, LMUX_HC2_EXEC_RELEASE, req) < 0)
        return 0;
    c->release_req = req;
    return 0;
}

static void stream_handle_state(struct stream_client *c,
                                const struct stream_frame *fr)
{
    uint32_t flags;
    uint8_t state;
    if (fr->len < 32 || lmux_get_le64(fr->payload) != c->ref)
        return;
    state = fr->payload[16];
    c->exit_code = (int)lmux_get_le32(fr->payload + 20);
    c->term_signal = (int)lmux_get_le32(fr->payload + 24);
    flags = lmux_get_le32(fr->payload + 28);
    if (flags & 1u)
        c->stdin_closed_remote = 1;
    if (state == V2_EXEC_STATUS_DONE || state == V2_EXEC_STATUS_START_FAILED)
        c->done_seen = 1;
}

static int stream_handle_frame(struct stream_client *c,
                               const struct stream_frame *fr)
{
    uint32_t code;
    if (fr->type == LMUX_HC2_EXEC_STDOUT && fr->len >= 8 &&
        lmux_get_le64(fr->payload) == c->ref) {
        if (c->stdout_closed)
            return 0;
        return stream_q_append(&c->outq, fr->payload + 8, fr->len - 8);
    }
    if (fr->type == LMUX_HC2_EXEC_STDERR && fr->len >= 8 &&
        lmux_get_le64(fr->payload) == c->ref) {
        if (c->stderr_closed)
            return 0;
        return stream_q_append(&c->errq, fr->payload + 8, fr->len - 8);
    }
    if (fr->type == LMUX_HC2_EXEC_STDIN_READY && fr->len >= 8 &&
        lmux_get_le64(fr->payload) == c->ref) {
        c->ready_gen++;
        if (c->stdin_blocked && c->ready_gen != c->stdin_req_ready_gen)
            c->stdin_blocked = 0;
        return 0;
    }
    if (fr->type == LMUX_HC2_EXEC_STATE) {
        if (fr->req == c->release_req && c->release_req) {
            c->released = 1;
            c->release_req = 0;
        } else {
            stream_handle_state(c, fr);
        }
        return 0;
    }
    if (fr->type == LMUX_HC2_EXEC_LOST && fr->len >= 12 &&
        lmux_get_le64(fr->payload) == c->ref) {
        fprintf(stderr, "lmux stream: target execution lost\n");
        c->lost_seen = 1;
        c->failure_rc = 1;
        c->done_seen = 1;
        return 0;
    }
    if (fr->type != LMUX_HC2_ERROR)
        return 0;
    code = stream_error_code(fr);
    if (fr->req == c->stdin_req && c->stdin_req) {
        if (code == LMUX_HC2_OK) {
            c->stdin_req = 0;
            c->stdin_pending_len = 0;
            c->stdin_blocked = 0;
            return 0;
        }
        if (code == LMUX_HC2_E_BUSY) {
            c->stdin_req = 0;
            if (c->ready_gen != c->stdin_req_ready_gen)
                c->stdin_blocked = 0;
            else
                c->stdin_blocked = 1;
            return 0;
        }
        stream_print_error_frame("lmux stream: stdin failed", fr);
        c->failure_rc = 1;
        c->cleanup_requested = 1;
        return 0;
    }
    if (fr->req == c->close_req && c->close_req) {
        if (code != LMUX_HC2_OK && code != LMUX_HC2_E_NOT_FOUND)
            stream_print_error_frame("lmux stream: stdin close failed", fr);
        c->close_req = 0;
        return 0;
    }
    if (fr->req == c->release_req && c->release_req) {
        c->released = 1;
        c->release_req = 0;
        return 0;
    }
    if (code != LMUX_HC2_OK)
        stream_print_error_frame("lmux stream", fr);
    return 0;
}

static int stream_write_queue(int fd, struct stream_queue *q)
{
    while (stream_q_len(q)) {
        ssize_t w = write(fd, q->data + q->pos, q->len - q->pos);
        if (w > 0) {
            q->pos += (size_t)w;
            if (q->pos == q->len)
                q->pos = q->len = 0;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (w < 0 && errno == EPIPE)
            return 1;
        return -1;
    }
    return 0;
}

static int stream_loop(struct stream_client *c)
{
    int stdin_flags = 0, stdout_flags = 0, stderr_flags = 0;
    int stdin_flags_valid = 0, stdout_flags_valid = 0, stderr_flags_valid = 0;
    c->stdin_open = 1;
    c->next_req = c->next_req ? c->next_req : 10;
    if (stream_set_nonblock(c->fd, NULL) < 0)
        return 1;
    stdin_flags_valid = stream_set_nonblock(STDIN_FILENO, &stdin_flags) == 0;
    stdout_flags_valid = stream_set_nonblock(STDOUT_FILENO, &stdout_flags) == 0;
    stderr_flags_valid = stream_set_nonblock(STDERR_FILENO, &stderr_flags) == 0;

    for (;;) {
        struct pollfd pfds[4];
        int nfds = 0, hc2_idx = -1, in_idx = -1, out_idx = -1, err_idx = -1;
        int timeout = -1;
        int can_read_hc2 =
            (c->stdout_closed || stream_q_space(&c->outq) >= STREAM_STDIN_CHUNK) &&
            (c->stderr_closed || stream_q_space(&c->errq) >= STREAM_STDIN_CHUNK);
        int rr, i;

        if (stream_sigint_pending) {
            stream_sigint_pending = 0;
            c->cleanup_requested = 1;
            if (!c->cleanup_signal_sent) {
                (void)stream_queue_signal(c, LMUX_HC2_SIG_INT);
                c->cleanup_signal_sent = 1;
                c->cleanup_deadline = (long long)time(NULL) * 1000 +
                    STREAM_CLEANUP_TIMEOUT_MS;
            }
        }
        if (stream_sigterm_pending) {
            stream_sigterm_pending = 0;
            c->cleanup_requested = 1;
            if (!c->cleanup_signal_sent) {
                (void)stream_queue_signal(c, LMUX_HC2_SIG_TERM);
                c->cleanup_signal_sent = 1;
                c->cleanup_deadline = (long long)time(NULL) * 1000 +
                    STREAM_CLEANUP_TIMEOUT_MS;
            }
        }
        if (c->cleanup_requested && c->cleanup_signal_sent &&
            !c->done_seen && !c->cleanup_kill_sent &&
            (long long)time(NULL) * 1000 >= c->cleanup_deadline) {
            (void)stream_queue_signal(c, LMUX_HC2_SIG_KILL);
            c->cleanup_kill_sent = 1;
            c->cleanup_deadline = (long long)time(NULL) * 1000 +
                STREAM_CLEANUP_TIMEOUT_MS;
        }

        (void)stream_try_queue_stdin(c);
        (void)stream_try_queue_close_stdin(c);
        if (c->done_seen && !stream_q_len(&c->outq) && !stream_q_len(&c->errq))
            (void)stream_queue_release(c);
        if (c->released)
            break;
        if (c->cleanup_requested && c->cleanup_kill_sent && !c->done_seen &&
            (long long)time(NULL) * 1000 >= c->cleanup_deadline) {
            c->failure_rc = 1;
            break;
        }

        hc2_idx = nfds;
        pfds[nfds].fd = c->fd;
        pfds[nfds].events = (c->tx_pos < c->tx_len ? POLLOUT : 0) |
            (can_read_hc2 ? POLLIN : 0);
        pfds[nfds].revents = 0;
        nfds++;
        if (c->stdin_open && !c->stdin_eof && !c->cleanup_requested &&
            !c->stdin_pending_len && !c->stdin_req && !c->stdin_blocked &&
            !c->stdin_closed_remote) {
            in_idx = nfds;
            pfds[nfds].fd = STDIN_FILENO;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (!c->stdout_closed && stream_q_len(&c->outq)) {
            out_idx = nfds;
            pfds[nfds].fd = STDOUT_FILENO;
            pfds[nfds].events = POLLOUT;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (!c->stderr_closed && stream_q_len(&c->errq)) {
            err_idx = nfds;
            pfds[nfds].fd = STDERR_FILENO;
            pfds[nfds].events = POLLOUT;
            pfds[nfds].revents = 0;
            nfds++;
        }
        if (c->cleanup_requested && c->cleanup_signal_sent && !c->done_seen) {
            long long now = (long long)time(NULL) * 1000;
            timeout = c->cleanup_deadline > now ? (int)(c->cleanup_deadline - now) : 0;
        }
        rr = poll(pfds, nfds, timeout);
        if (rr < 0 && errno == EINTR)
            continue;
        if (rr < 0) {
            perror("lmux stream: poll");
            c->failure_rc = 1;
            break;
        }
        for (i = 0; i < nfds; i++) {
            if (!pfds[i].revents)
                continue;
            if (i == hc2_idx) {
                if ((pfds[i].revents & POLLOUT) && stream_flush_tx(c) < 0) {
                    fprintf(stderr, "lmux stream: structured control write failed\n");
                    c->failure_rc = 1;
                    c->done_seen = 1;
                }
                if ((pfds[i].revents & POLLIN) && stream_read_hc2(c) < 0) {
                    fprintf(stderr, "lmux stream: structured control closed\n");
                    c->failure_rc = 1;
                    c->done_seen = 1;
                }
                if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    c->failure_rc = 1;
                    c->done_seen = 1;
                }
            } else if (i == in_idx) {
                uint8_t buf[STREAM_STDIN_CHUNK];
                ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
                if (r > 0) {
                    memcpy(c->stdin_pending, buf, (size_t)r);
                    c->stdin_pending_len = (size_t)r;
                } else if (r == 0 || (r < 0 && errno != EINTR &&
                           errno != EAGAIN && errno != EWOULDBLOCK)) {
                    c->stdin_eof = 1;
                    c->stdin_open = 0;
                }
            } else if (i == out_idx) {
                int wr = stream_write_queue(STDOUT_FILENO, &c->outq);
                if (wr) {
                    c->cleanup_requested = 1;
                    c->failure_rc = 1;
                    c->stdout_closed = 1;
                    c->outq.pos = c->outq.len = 0;
                    if (!c->cleanup_signal_sent) {
                        (void)stream_queue_signal(c, LMUX_HC2_SIG_TERM);
                        c->cleanup_signal_sent = 1;
                        c->cleanup_deadline = (long long)time(NULL) * 1000 +
                            STREAM_CLEANUP_TIMEOUT_MS;
                    }
                }
            } else if (i == err_idx) {
                int wr = stream_write_queue(STDERR_FILENO, &c->errq);
                if (wr) {
                    c->cleanup_requested = 1;
                    c->failure_rc = 1;
                    c->stderr_closed = 1;
                    c->errq.pos = c->errq.len = 0;
                    if (!c->cleanup_signal_sent) {
                        (void)stream_queue_signal(c, LMUX_HC2_SIG_TERM);
                        c->cleanup_signal_sent = 1;
                        c->cleanup_deadline = (long long)time(NULL) * 1000 +
                            STREAM_CLEANUP_TIMEOUT_MS;
                    }
                }
            }
        }
        for (;;) {
            struct stream_frame fr;
            int pr = stream_parse_one_frame(c, &fr);
            if (pr < 0) {
                fprintf(stderr, "lmux stream: bad structured frame\n");
                c->failure_rc = 1;
                c->done_seen = 1;
                break;
            }
            if (!pr)
                break;
            if (stream_handle_frame(c, &fr) < 0) {
                fprintf(stderr, "lmux stream: local output queue full\n");
                c->failure_rc = 1;
                c->cleanup_requested = 1;
                break;
            }
        }
    }

    stream_restore_flags(STDIN_FILENO, stdin_flags, stdin_flags_valid);
    stream_restore_flags(STDOUT_FILENO, stdout_flags, stdout_flags_valid);
    stream_restore_flags(STDERR_FILENO, stderr_flags, stderr_flags_valid);
    if (c->failure_rc)
        return c->failure_rc;
    if (c->lost_seen)
        return 1;
    if (c->term_signal)
        return 128 + (c->term_signal & 127);
    return c->exit_code & 255;
}

static int cmd_stream(int argc, char **argv)
{
    const char *control = LMUX_DEFAULT_STRUCTURED_SOCKET;
    struct stream_client *c;
    struct sigaction old_int, old_term, sa;
    int rc;
    while (argc > 0) {
        if (strcmp(argv[0], "--control") == 0 && argc >= 2) {
            control = argv[1];
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[0], "--") == 0) {
            argv++;
            argc--;
            break;
        } else {
            usage(stderr);
            return 1;
        }
    }
    if (argc < 1) {
        usage(stderr);
        return 1;
    }
    c = calloc(1, sizeof(*c));
    if (!c) {
        perror("calloc");
        return 1;
    }
    c->fd = connect_unix(control);
    if (c->fd < 0) {
        free(c);
        return 1;
    }
    c->next_req = 1;
    if (stream_start_exec(c, argc, argv) < 0) {
        close(c->fd);
        free(c);
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stream_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);
    rc = stream_loop(c);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    close(c->fd);
    free(c);
    return rc;
}

static int cmd_status(int argc, char **argv)
{
    const char *control = LMUX_DEFAULT_CONTROL_SOCKET;
    while (argc > 0) {
        if (strcmp(argv[0], "--control") == 0 && argc >= 2) {
            control = argv[1];
            argv += 2;
            argc -= 2;
        } else {
            usage(stderr);
            return 1;
        }
    }
    return control_request(control, "STATUS\n", 1, 0) < 0 ? 1 : 0;
}

static int carrier_control_request(const char *control, const char *request)
{
    int fd = connect_unix(control);
    int saw_error = 0;
    uint8_t prefix[4];
    size_t prefix_len = 0;
    if (fd < 0)
        return 1;
    if (full_write(fd, (const uint8_t *)request, strlen(request)) < 0) {
        perror("carrier control write");
        close(fd);
        return 1;
    }
    for (;;) {
        uint8_t buf[512];
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r > 0) {
            size_t take = (size_t)r;
            if (take > sizeof(prefix) - prefix_len)
                take = sizeof(prefix) - prefix_len;
            if (take != 0) {
                memcpy(prefix + prefix_len, buf, take);
                prefix_len += take;
                if (prefix_len == sizeof(prefix) &&
                    memcmp(prefix, "ERR ", sizeof(prefix)) == 0)
                    saw_error = 1;
            }
            if (full_write(STDOUT_FILENO, buf, (size_t)r) < 0) {
                close(fd);
                return 1;
            }
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            saw_error = 1;
        break;
    }
    close(fd);
    return saw_error;
}

static int cmd_carrier(int argc, char **argv)
{
    const char *control = LMUX_DEFAULT_CONTROL_SOCKET;
    const char *mode = "raw";
    const char *name = NULL;
    const char *action;
    unsigned timeout_ms = 30000;
    char request[160];
    if (argc < 1)
        return usage(stderr), 1;
    action = argv[0];
    argv++;
    argc--;
    if (strcmp(action, "switch") == 0 || strcmp(action, "console") == 0) {
        if (argc < 1)
            return usage(stderr), 1;
        name = argv[0];
        argv++;
        argc--;
    } else if (strcmp(action, "status") != 0) {
        return usage(stderr), 1;
    }
    while (argc > 0) {
        if (strcmp(argv[0], "--control") == 0 && argc >= 2) {
            control = argv[1];
            argv += 2;
            argc -= 2;
        } else if (strcmp(action, "switch") == 0 &&
                   strcmp(argv[0], "--mode") == 0 && argc >= 2) {
            mode = argv[1];
            argv += 2;
            argc -= 2;
        } else if (strcmp(action, "switch") == 0 &&
                   strcmp(argv[0], "--timeout-ms") == 0 && argc >= 2) {
            char *end = NULL;
            unsigned long v = strtoul(argv[1], &end, 10);
            if (!end || *end || v < 1000 || v > 120000)
                return usage(stderr), 1;
            timeout_ms = (unsigned)v;
            argv += 2;
            argc -= 2;
        } else {
            return usage(stderr), 1;
        }
    }
    if (strcmp(action, "status") == 0)
        return carrier_control_request(control, "CARRIER STATUS\n");
    if (strcmp(action, "console") == 0) {
        if (strlen(name) > LMUX_CARRIER_NAME_MAX)
            return usage(stderr), 1;
        snprintf(request, sizeof(request), "CARRIER CONSOLE %s\n", name);
        return carrier_control_request(control, request);
    }
    if (strlen(name) > LMUX_CARRIER_NAME_MAX ||
        (strcmp(mode, "raw") != 0 && strcmp(mode, "dirty") != 0))
        return usage(stderr), 1;
    snprintf(request, sizeof(request), "CARRIER SWITCH %s %s %u\n",
             name, mode, timeout_ms);
    return carrier_control_request(control, request);
}

static int cmd_deploy(int argc, char **argv)
{
    const char *control = LMUX_DEFAULT_CONTROL_SOCKET;
    const char *file = NULL;
    unsigned timeout_ms = 30000;
    int restart_supervisor = 0;
    while (argc > 0) {
        if (strcmp(argv[0], "--control") == 0 && argc >= 2) {
            control = argv[1];
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[0], "--timeout-ms") == 0 && argc >= 2) {
            char *end = NULL;
            unsigned long v = strtoul(argv[1], &end, 10);
            if (!end || *end || v > 0xffffffffUL) {
                usage(stderr);
                return 1;
            }
            timeout_ms = (unsigned)v;
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[0], "--restart-supervisor") == 0) {
            restart_supervisor = 1;
            argv++;
            argc--;
        } else if (!file) {
            file = argv[0];
            argv++;
            argc--;
        } else {
            usage(stderr);
            return 1;
        }
    }
    if (!file && !restart_supervisor) {
        usage(stderr);
        return 1;
    }
    if (!file && timeout_ms != 30000) {
        fprintf(stderr, "lmux deploy: --timeout-ms requires FILE\n");
        return 1;
    }
    if (!file)
        return deploy_restart_supervisor(control);
    return deploy_file(control, file, timeout_ms, restart_supervisor);
}

static int cmd_boot(int argc, char **argv)
{
    const char *control = LMUX_DEFAULT_CONTROL_SOCKET;
    const char *file = NULL;
    const char *sha = NULL;
    const char *activation_id = NULL;
    unsigned timeout_ms = 30000;
    const char *cmd;
    const char *req = NULL;
    char reqbuf[160];
    if (argc < 1) {
        usage(stderr);
        return 1;
    }
    cmd = argv[0];
    argv++;
    argc--;
    if (strcmp(cmd, "upload") == 0) {
        if (argc < 1) {
            usage(stderr);
            return 1;
        }
        file = argv[0];
        argv++;
        argc--;
    } else if (strcmp(cmd, "activate") == 0) {
        if (argc < 1) {
            usage(stderr);
            return 1;
        }
        sha = argv[0];
        argv++;
        argc--;
    } else if (strcmp(cmd, "confirm") == 0 || strcmp(cmd, "rollback") == 0) {
        if (argc < 1) {
            usage(stderr);
            return 1;
        }
        activation_id = argv[0];
        argv++;
        argc--;
    } else if (strcmp(cmd, "supervisor-install") == 0 ||
               strcmp(cmd, "supervisor-restart") == 0) {
        if (argc < 1) {
            usage(stderr);
            return 1;
        }
        sha = argv[0];
        argv++;
        argc--;
    }
    while (argc > 0) {
        if (strcmp(argv[0], "--control") == 0 && argc >= 2) {
            control = argv[1];
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[0], "--timeout-ms") == 0 && argc >= 2 &&
                   strcmp(cmd, "activate") == 0) {
            char *end = NULL;
            unsigned long v = strtoul(argv[1], &end, 10);
            if (!end || *end || v > 0xffffffffUL) {
                usage(stderr);
                return 1;
            }
            timeout_ms = (unsigned)v;
            argv += 2;
            argc -= 2;
        } else {
            usage(stderr);
            return 1;
        }
    }
    if (strcmp(cmd, "info") == 0)
        req = "BOOT GET_INFO\n";
    else if (strcmp(cmd, "worker-status") == 0)
        req = "BOOT WORKER_STATUS\n";
    else if (strcmp(cmd, "upload") == 0)
        return boot_upload_file(control, file);
    else if (strcmp(cmd, "update-status") == 0)
        req = "BOOT UPDATE_STATUS\n";
    else if (strcmp(cmd, "activate") == 0) {
        if (!sha || strlen(sha) != 64)
            return usage(stderr), 1;
        snprintf(reqbuf, sizeof(reqbuf), "BOOT ACTIVATE %s %u\n", sha, timeout_ms);
        req = reqbuf;
    } else if (strcmp(cmd, "confirm") == 0) {
        snprintf(reqbuf, sizeof(reqbuf), "BOOT CONFIRM %s\n", activation_id);
        req = reqbuf;
    } else if (strcmp(cmd, "rollback") == 0) {
        snprintf(reqbuf, sizeof(reqbuf), "BOOT ROLLBACK %s\n", activation_id);
        req = reqbuf;
    } else if (strcmp(cmd, "supervisor-install") == 0) {
        if (!sha || strlen(sha) != 64)
            return usage(stderr), 1;
        snprintf(reqbuf, sizeof(reqbuf), "BOOT SUPERVISOR_INSTALL %s\n", sha);
        req = reqbuf;
    } else if (strcmp(cmd, "supervisor-restart") == 0) {
        if (!sha || strlen(sha) != 64)
            return usage(stderr), 1;
        snprintf(reqbuf, sizeof(reqbuf), "BOOT SUPERVISOR_RESTART %s\n", sha);
        req = reqbuf;
    }
    else {
        usage(stderr);
        return 1;
    }
    return control_request(control, req, 1, 0) < 0 ? 1 : 0;
}

#ifndef LMUX_NO_MAIN
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    init_host_cwd();

    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("lmux %s (%s)\n", LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
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
        return cmd_connect(argv[2], 0, NULL, 0, 0, argv[2]);
    }
    if (strcmp(argv[1], "status") == 0)
        return cmd_status(argc - 2, argv + 2);
    if (strcmp(argv[1], "carrier") == 0)
        return cmd_carrier(argc - 2, argv + 2);
    if (strcmp(argv[1], "deploy") == 0)
        return cmd_deploy(argc - 2, argv + 2);
    if (strcmp(argv[1], "boot") == 0)
        return cmd_boot(argc - 2, argv + 2);
    if (strcmp(argv[1], "zeromux") == 0)
        return lmux_zeromux_main(argc - 2, argv + 2);
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
        return cmd_connect(sock, 1, remote_dir, channel, verbose,
                           console_label_for_arg(arg));
    }
    if (strcmp(argv[1], "exec") == 0)
        return cmd_exec(argc - 2, argv + 2);
    if (strcmp(argv[1], "stream") == 0)
        return cmd_stream(argc - 2, argv + 2);
    if (strcmp(argv[1], "put") == 0)
        return cmd_put(argc - 2, argv + 2);
    if (strcmp(argv[1], "get") == 0)
        return cmd_get(argc - 2, argv + 2);

    usage(stderr);
    return 1;
}
#endif
