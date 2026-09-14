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

static int check_filter(const uint8_t *in, size_t in_len, const uint8_t *want, size_t want_len, int want_detach)
{
    struct pipebuf p;
    int prefix = 0;
    memset(&p, 0, sizeof(p));
    term_detach = 0;
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
    if (!is_pure_numeric("3") || is_pure_numeric("/tmp/3"))
        return fail("numeric term helper");
    if (test_path_helpers())
        return 1;
    if (test_progress_helpers())
        return 1;

    puts("lmux client selftest ok");
    return 0;
}
