#define LMUX_NO_MAIN
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "lmux.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>

static int fail(const char *msg)
{
    fprintf(stderr, "lmux client test failed: %s\n", msg);
    return 1;
}

static int capture_stderr(void (*fn)(const char *), const char *label,
                          const char *want)
{
    int p[2], saved, ok = -1;
    char buf[256];
    ssize_t n;
    if (pipe(p) < 0)
        return -1;
    saved = dup(STDERR_FILENO);
    if (saved < 0) {
        close(p[0]); close(p[1]);
        return -1;
    }
    if (dup2(p[1], STDERR_FILENO) < 0) {
        close(saved); close(p[0]); close(p[1]);
        return -1;
    }
    close(p[1]);
    fn(label);
    fflush(stderr);
    if (dup2(saved, STDERR_FILENO) < 0) {
        close(saved); close(p[0]);
        return -1;
    }
    close(saved);
    n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    if (n >= 0) {
        buf[n] = 0;
        ok = strstr(buf, want) != NULL ? 0 : -1;
    }
    return ok;
}

static int capture_term_filter(const uint8_t *in, size_t in_len,
                               const char *label, const char *want)
{
    int pfd[2], saved, prefix = 0, ok = -1;
    char buf[256];
    struct pipebuf p;
    ssize_t n;
    if (pipe(pfd) < 0)
        return -1;
    saved = dup(STDERR_FILENO);
    if (saved < 0) {
        close(pfd[0]); close(pfd[1]);
        return -1;
    }
    if (dup2(pfd[1], STDERR_FILENO) < 0) {
        close(saved); close(pfd[0]); close(pfd[1]);
        return -1;
    }
    close(pfd[1]);
    memset(&p, 0, sizeof(p));
    term_detach = 0;
    term_modal = 0;
    term_label = label;
    term_filter_input(&p, in, in_len, &prefix);
    fflush(stderr);
    if (dup2(saved, STDERR_FILENO) < 0) {
        close(saved); close(pfd[0]);
        return -1;
    }
    close(saved);
    n = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);
    if (n >= 0) {
        buf[n] = 0;
        ok = term_detach && p.len == 0 && strstr(buf, want) != NULL ? 0 : -1;
    }
    return ok;
}

static int capture_cmd_connect_failure(const char *path, const char *want)
{
    int p[2], saved, rc, ok = -1;
    char buf[512];
    ssize_t n;
    if (pipe(p) < 0)
        return -1;
    saved = dup(STDERR_FILENO);
    if (saved < 0) {
        close(p[0]); close(p[1]);
        return -1;
    }
    if (dup2(p[1], STDERR_FILENO) < 0) {
        close(saved); close(p[0]); close(p[1]);
        return -1;
    }
    close(p[1]);
    rc = cmd_connect(path, 1, NULL, 1, 0, path);
    fflush(stderr);
    if (dup2(saved, STDERR_FILENO) < 0) {
        close(saved); close(p[0]);
        return -1;
    }
    close(saved);
    n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    if (n >= 0) {
        buf[n] = 0;
        ok = rc == 1 && strstr(buf, want) != NULL &&
             strstr(buf, "[attached:") == NULL ? 0 : -1;
    }
    return ok;
}

static int test_term_disconnected_loop(void)
{
    int sv[2], in[2], out[2], err[2];
    int save_in, save_out, save_err, rc;
    char ebuf[256], obuf[16];
    ssize_t en, on;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0 ||
        pipe(in) < 0 || pipe(out) < 0 || pipe(err) < 0)
        return -1;
    save_in = dup(STDIN_FILENO);
    save_out = dup(STDOUT_FILENO);
    save_err = dup(STDERR_FILENO);
    if (save_in < 0 || save_out < 0 || save_err < 0)
        return -1;
    close(in[1]);
    close(sv[1]);
    if (dup2(in[0], STDIN_FILENO) < 0 ||
        dup2(out[1], STDOUT_FILENO) < 0 ||
        dup2(err[1], STDERR_FILENO) < 0)
        return -1;
    close(in[0]);
    close(out[1]);
    close(err[1]);
    rc = term_bridge_loop(sv[0], NULL, 1, 0, "console1");
    fflush(stderr);
    (void)dup2(save_in, STDIN_FILENO);
    (void)dup2(save_out, STDOUT_FILENO);
    (void)dup2(save_err, STDERR_FILENO);
    close(save_in); close(save_out); close(save_err);
    close(sv[0]);
    en = read(err[0], ebuf, sizeof(ebuf) - 1);
    on = read(out[0], obuf, sizeof(obuf));
    close(err[0]); close(out[0]);
    if (en < 0 || on < 0)
        return -1;
    ebuf[en] = 0;
    return rc == 0 && on == 0 && strstr(ebuf, "[disconnected: console1]") != NULL ? 0 : -1;
}

static int check_filter(const uint8_t *in, size_t in_len, const uint8_t *want, size_t want_len, int want_detach)
{
    struct pipebuf p;
    int prefix = 0;
    memset(&p, 0, sizeof(p));
    term_detach = 0;
    term_label = "console1";
    term_filter_input(&p, in, in_len, &prefix);
    if (term_detach != want_detach)
        return -1;
    if (p.len != want_len || memcmp(p.buf + p.off, want, want_len) != 0)
        return -1;
    return 0;
}

static int check_modal(const uint8_t *in, size_t in_len, int want_modal)
{
    struct pipebuf p;
    int prefix = 0;
    memset(&p, 0, sizeof(p));
    term_detach = 0;
    term_modal = 0;
    term_filter_input(&p, in, in_len, &prefix);
    return term_modal == want_modal && p.len == 0 && !term_detach ? 0 : -1;
}

static int test_path_helpers(void)
{
    struct transfer_config cfg;
    char out[LXF_PATH_MAX];

    strcpy(host_cwd, "/mnt/work");
    memset(&cfg, 0, sizeof(cfg));
    cfg.sock_path = LXF_DEFAULT_SOCKET;
    cfg.remote_dir = "/root/gsm";
    cfg.channel = 1;

    if (resolve_host_path("./foo.bin", out, sizeof(out)) < 0 || strcmp(out, "/mnt/work/./foo.bin") != 0)
        return fail("HOST relative source resolution");
    if (resolve_target_path(&cfg, NULL, "foo.bin", out, sizeof(out)) < 0 || strcmp(out, "/root/gsm/foo.bin") != 0)
        return fail("PUT omitted TARGET destination");
    if (resolve_target_path(&cfg, "log.log", NULL, out, sizeof(out)) < 0 || strcmp(out, "/root/gsm/log.log") != 0)
        return fail("GET relative TARGET source");
    if (resolve_target_path(&cfg, "/tmp/core.bin", NULL, out, sizeof(out)) < 0 || strcmp(out, "/tmp/core.bin") != 0)
        return fail("absolute TARGET source unchanged");
    if (join_path(out, sizeof(out), host_cwd, path_basename("/root/log.log")) < 0 ||
        strcmp(out, "/mnt/work/log.log") != 0)
        return fail("HOST destination default");
    if (default_channel_for_console_arg(NULL) != 1 ||
        default_channel_for_console_arg("1") != 1 ||
        default_channel_for_console_arg("2") != 2 ||
        default_channel_for_console_arg("/tmp/custom.sock") != 0)
        return fail("term console context");
    if (strcmp(console_label_for_arg(NULL), "console1") != 0 ||
        strcmp(console_label_for_arg("1"), "console1") != 0 ||
        strcmp(console_label_for_arg("2"), "console2") != 0 ||
        strcmp(console_label_for_arg("/tmp/custom.sock"), "/tmp/custom.sock") != 0)
        return fail("term console labels");
    return 0;
}

static int test_progress_helpers(void)
{
    char out[64], bar[40];
    struct progress_state p;
    double eta;

    if (progress_percent(0, 100, 1) != 0 ||
        progress_percent(50, 100, 1) != 50 ||
        progress_percent(100, 100, 1) != 100 ||
        progress_percent(0, 0, 1) != 100 ||
        progress_percent(1, 0, 0) != -1)
        return fail("progress percent");
    human_bytes(815, out, sizeof(out), 0);
    if (strcmp(out, "815 B") != 0)
        return fail("human bytes B");
    human_bytes(12700, out, sizeof(out), 0);
    if (strcmp(out, "12.4 KiB") != 0)
        return fail("human bytes KiB");
    human_bytes(3460300, out, sizeof(out), 0);
    if (strstr(out, "MiB") == NULL)
        return fail("human bytes MiB");
    human_bytes(3221225472ULL, out, sizeof(out), 0);
    if (strstr(out, "GiB") == NULL)
        return fail("human bytes GiB");
    human_duration(7, out, sizeof(out));
    if (strcmp(out, "7s") != 0)
        return fail("duration seconds");
    human_duration(128, out, sizeof(out));
    if (strcmp(out, "2m 08s") != 0)
        return fail("duration minutes");
    human_duration(3788, out, sizeof(out));
    if (strcmp(out, "1h 03m 08s") != 0)
        return fail("duration hours");
    make_bar(bar, sizeof(bar), 10, 50, 0);
    if (strcmp(bar, "[#####-----]") != 0)
        return fail("known-size bar");
    make_bar(bar, sizeof(bar), 10, -1, 8192);
    if (strchr(bar, '>') == NULL)
        return fail("unknown-size bar");
    if ((uint64_t)progress_rate(1024, 1.0) != 1024)
        return fail("rate calculation");
    if (progress_rate(1024, 0.0) != 0.0)
        return fail("no divide by zero");
    if (progress_eta_seconds(50, 100, 10.0, &eta) < 0 || (int)eta != 5)
        return fail("known-size ETA");
    if (progress_eta_seconds(50, 100, 0.0, &eta) == 0)
        return fail("unknown ETA unavailable");
    progress_init(&p, "PUT", 1, 100, 0);
    if (p.tty)
        return fail("non-TTY progress should not animate in selftest");
    return 0;
}

int main(void)
{
    const uint8_t normal[] = { 'a', 0x03, 'b' };
    const uint8_t prefix_q[] = { 0x14, 'q' };
    const uint8_t prefix_dot[] = { 0x14, '.' };
    const uint8_t prefix_help[] = { 0x14, '?' };
    const uint8_t prefix_literal[] = { 0x14, 0x14 };
    const uint8_t prefix_unknown[] = { 0x14, 'x' };
    const uint8_t prefix_put[] = { 0x14, 'p' };
    const uint8_t prefix_get[] = { 0x14, 'g' };
    const uint8_t literal_want[] = { 0x14 };
    const uint8_t unknown_want[] = { 0x14, 'x' };

    if (check_filter(normal, sizeof(normal), normal, sizeof(normal), 0) < 0)
        return fail("normal/Ctrl-C forwarding");
    if (check_filter(prefix_q, sizeof(prefix_q), NULL, 0, 1) < 0)
        return fail("Ctrl-T q detach");
    if (check_filter(prefix_dot, sizeof(prefix_dot), NULL, 0, 1) < 0)
        return fail("Ctrl-T . detach");
    if (capture_term_filter(prefix_q, sizeof(prefix_q), "console1", "[detached: console1]") < 0)
        return fail("Ctrl-T q detach label");
    if (capture_term_filter(prefix_dot, sizeof(prefix_dot), "console2", "[detached: console2]") < 0)
        return fail("Ctrl-T . detach label");
    if (check_filter(prefix_help, sizeof(prefix_help), NULL, 0, 0) < 0)
        return fail("Ctrl-T ? help filtering");
    if (check_filter(prefix_literal, sizeof(prefix_literal), literal_want, sizeof(literal_want), 0) < 0)
        return fail("Ctrl-T Ctrl-T literal");
    if (check_filter(prefix_unknown, sizeof(prefix_unknown), unknown_want, sizeof(unknown_want), 0) < 0)
        return fail("unknown prefix preservation");
    if (check_modal(prefix_put, sizeof(prefix_put), 'p') < 0)
        return fail("Ctrl-T p modal");
    if (check_modal(prefix_get, sizeof(prefix_get), 'g') < 0)
        return fail("Ctrl-T g modal");

    if (strcmp(console_socket_for_arg(NULL), "/run/lmux/console1.sock") != 0)
        return fail("term default console1");
    if (strcmp(console_socket_for_arg("1"), "/run/lmux/console1.sock") != 0)
        return fail("term 1 console1");
    if (strcmp(console_socket_for_arg("2"), "/run/lmux/console2.sock") != 0)
        return fail("term 2 console2");
    if (strcmp(console_socket_for_arg("/tmp/x.sock"), "/tmp/x.sock") != 0)
        return fail("term explicit socket");
    if (strcmp(console_label_for_arg(NULL), "console1") != 0 ||
        strcmp(console_label_for_arg("1"), "console1") != 0 ||
        strcmp(console_label_for_arg("2"), "console2") != 0 ||
        strcmp(console_label_for_arg("/tmp/x.sock"), "/tmp/x.sock") != 0)
        return fail("term label mapping");
    if (capture_stderr(term_print_attached, "console1", "[attached: console1]\r\n") < 0)
        return fail("attached console label");
    if (capture_stderr(term_print_attached, "/tmp/x.sock", "[attached: /tmp/x.sock]\r\n") < 0)
        return fail("attached socket label");
    if (capture_stderr(term_print_disconnected, "console1", "[disconnected: console1]") < 0)
        return fail("disconnected console label");
    if (test_term_disconnected_loop() < 0)
        return fail("term EOF disconnected label");
    if (capture_cmd_connect_failure("/tmp/lmux-client-test-no-such.sock",
                                    "lmux term: /tmp/lmux-client-test-no-such.sock: connect:") < 0)
        return fail("term connect failure identity");
    if (!is_pure_numeric("3") || is_pure_numeric("/tmp/3"))
        return fail("numeric term helper");
    if (test_path_helpers())
        return 1;
    if (test_progress_helpers())
        return 1;

    puts("lmux client selftest ok");
    return 0;
}
