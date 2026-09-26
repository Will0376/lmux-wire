#include "zeromux.h"

#include "probe.h"
#include "sender.h"
#include "install.h"
#include "lxf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_uint(const char *s, unsigned *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s)
        return -1;
    v = strtoul(s, &end, 10);
    if (!end || *end || v > 0xffffffffUL)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static int parse_parity(const char *s, enum lmux_serial_parity *out)
{
    if (strcmp(s, "none") == 0)
        *out = LMUX_SERIAL_PARITY_NONE;
    else if (strcmp(s, "even") == 0)
        *out = LMUX_SERIAL_PARITY_EVEN;
    else if (strcmp(s, "odd") == 0)
        *out = LMUX_SERIAL_PARITY_ODD;
    else
        return -1;
    return 0;
}

static int parse_flow(const char *s, enum lmux_serial_flow *out)
{
    if (strcmp(s, "none") == 0)
        *out = LMUX_SERIAL_FLOW_NONE;
    else if (strcmp(s, "rtscts") == 0)
        *out = LMUX_SERIAL_FLOW_RTSCTS;
    else if (strcmp(s, "xonxoff") == 0)
        *out = LMUX_SERIAL_FLOW_XONXOFF;
    else
        return -1;
    return 0;
}

static const char *parity_name(enum lmux_serial_parity p)
{
    switch (p) {
    case LMUX_SERIAL_PARITY_NONE: return "N";
    case LMUX_SERIAL_PARITY_EVEN: return "E";
    case LMUX_SERIAL_PARITY_ODD: return "O";
    }
    return "?";
}

static const char *flow_name(enum lmux_serial_flow f)
{
    switch (f) {
    case LMUX_SERIAL_FLOW_NONE: return "none";
    case LMUX_SERIAL_FLOW_RTSCTS: return "rtscts";
    case LMUX_SERIAL_FLOW_XONXOFF: return "xonxoff";
    }
    return "unknown";
}

static void print_probe(const char *tty, const struct lmux_serial_config *serial,
                        const struct lmux_zmx_probe_result *r)
{
    size_t i;
    printf("zeromux probe\n");
    printf("bootstrap:\n");
    printf("  tty=%s\n", tty);
    printf("  serial=%u %u%s%u flow=%s\n", serial->baud,
           serial->data_bits, parity_name(serial->parity),
           serial->stop_bits, flow_name(serial->flow));
    printf("  shell=%s\n", r->shell_usable ? "usable" : "unusable");
    printf("  authentication=%s\n",
           r->auth_required ? "required" : "pre-authenticated");
    printf("\nsystem:\n");
    printf("  kernel=%s\n", r->kernel[0] ? r->kernel : "unknown");
    printf("  machine=%s\n", r->machine[0] ? r->machine : "unknown");
    printf("\ntools:\n");
    for (i = 0; i < LMUX_ZMX_TOOL_COUNT; i++)
        printf("  %s=%s\n", r->tools[i].name,
               lmux_zmx_tri_name(r->tools[i].state));
    printf("\nfilesystem:\n");
    printf("  cwd=%s\n", r->cwd[0] ? r->cwd : "unknown");
    printf("  /tmp: %s\n", lmux_zmx_tri_name(r->path_tmp));
    printf("  /root: %s\n", lmux_zmx_tri_name(r->path_root));
}

struct cli_opts {
    const char *tty;
    struct lmux_serial_config serial;
    unsigned timeout_ms;
};

static int parse_serial_opts(int *argc, char ***argv, struct cli_opts *opts)
{
    if (strcmp((*argv)[0], "--tty") == 0 && *argc >= 2) {
        opts->tty = (*argv)[1]; *argv += 2; *argc -= 2;
    } else if (strcmp((*argv)[0], "--baud") == 0 && *argc >= 2) {
        if (parse_uint((*argv)[1], &opts->serial.baud) < 0)
            return fprintf(stderr, "invalid --baud\n"), -1;
        *argv += 2; *argc -= 2;
    } else if (strcmp((*argv)[0], "--data-bits") == 0 && *argc >= 2) {
        if (parse_uint((*argv)[1], &opts->serial.data_bits) < 0 ||
            opts->serial.data_bits < 5 || opts->serial.data_bits > 8)
            return fprintf(stderr, "invalid --data-bits\n"), -1;
        *argv += 2; *argc -= 2;
    } else if (strcmp((*argv)[0], "--parity") == 0 && *argc >= 2) {
        if (parse_parity((*argv)[1], &opts->serial.parity) < 0)
            return fprintf(stderr, "invalid --parity\n"), -1;
        *argv += 2; *argc -= 2;
    } else if (strcmp((*argv)[0], "--stop-bits") == 0 && *argc >= 2) {
        if (parse_uint((*argv)[1], &opts->serial.stop_bits) < 0 ||
            (opts->serial.stop_bits != 1 && opts->serial.stop_bits != 2))
            return fprintf(stderr, "invalid --stop-bits\n"), -1;
        *argv += 2; *argc -= 2;
    } else if (strcmp((*argv)[0], "--flow") == 0 && *argc >= 2) {
        if (parse_flow((*argv)[1], &opts->serial.flow) < 0)
            return fprintf(stderr, "invalid --flow\n"), -1;
        *argv += 2; *argc -= 2;
    } else if (strcmp((*argv)[0], "--timeout-ms") == 0 && *argc >= 2) {
        if (parse_uint((*argv)[1], &opts->timeout_ms) < 0 ||
            opts->timeout_ms == 0)
            return fprintf(stderr, "invalid --timeout-ms\n"), -1;
        *argv += 2; *argc -= 2;
    } else {
        return 0;
    }
    return 1;
}

int lmux_zeromux_probe_main(int argc, char **argv)
{
    struct cli_opts opts;
    struct lmux_zmx_session session;
    struct lmux_zmx_probe_result result;
    int rc = 1;

    memset(&opts, 0, sizeof(opts));
    lmux_serial_config_default(&opts.serial);
    opts.timeout_ms = 5000;
    while (argc > 0) {
        int p = parse_serial_opts(&argc, &argv, &opts);
        if (p < 0)
            return 1;
        if (p == 0)
            return fprintf(stderr, "invalid zeromux probe option: %s\n", argv[0]), 1;
    }
    if (!opts.tty)
        return fprintf(stderr, "lmux zeromux probe: --tty is required\n"), 1;
    if (lmux_zmx_session_open_tty(&session, opts.tty, &opts.serial,
                                  opts.timeout_ms) < 0) {
        fprintf(stderr, "%s: %s\n", opts.tty, strerror(errno));
        return 1;
    }
    if (lmux_zmx_probe_run(&session, &result) == 0) {
        print_probe(opts.tty, &opts.serial, &result);
        rc = 0;
    } else {
        print_probe(opts.tty, &opts.serial, &result);
        fprintf(stderr, "zeromux probe failed: %s\n",
                lmux_zmx_error_name(result.status));
        rc = result.status == LMUX_ZMX_AUTH_REQUIRED ? 2 : 1;
    }
    lmux_zmx_session_close(&session);
    return rc;
}

static int lmux_zeromux_send_main(int argc, char **argv)
{
    struct cli_opts opts;
    const char *local, *target;
    struct lmux_zmx_session session;
    struct lmux_zmx_probe_result probe;
    struct lmux_zmx_sender_result res;
    char hex[65];
    int rc = 1;

    if (argc < 2)
        return fprintf(stderr, "Usage: lmux zeromux send LOCAL_FILE TARGET_TEMP --tty DEVICE [serial options]\n"), 1;
    local = argv[0];
    target = argv[1];
    argv += 2;
    argc -= 2;
    memset(&opts, 0, sizeof(opts));
    lmux_serial_config_default(&opts.serial);
    opts.timeout_ms = 10000;
    while (argc > 0) {
        int p = parse_serial_opts(&argc, &argv, &opts);
        if (p < 0)
            return 1;
        if (p == 0)
            return fprintf(stderr, "invalid zeromux send option: %s\n", argv[0]), 1;
    }
    if (!opts.tty)
        return fprintf(stderr, "lmux zeromux send: --tty is required\n"), 1;
    if (lmux_zmx_session_open_tty(&session, opts.tty, &opts.serial,
                                  opts.timeout_ms) < 0) {
        fprintf(stderr, "%s: %s\n", opts.tty, strerror(errno));
        return 1;
    }
    if (lmux_zmx_probe_run(&session, &probe) < 0) {
        fprintf(stderr, "zeromux send: shell/probe failed: %s\n",
                lmux_zmx_error_name(probe.status));
        goto out;
    }
    if (lmux_zmx_sender_upload_file(&session, &probe, local, target, &res) < 0) {
        fprintf(stderr, "zeromux send failed: %s %s\n",
                lmux_zmx_sender_status_name(res.status),
                res.detail[0] ? res.detail : "");
        goto out;
    }
    sha256_hex(res.sha256, hex);
    printf("sender=%s\n", res.backend ? res.backend : "unknown");
    printf("bytes=%llu\n", (unsigned long long)res.bytes);
    printf("sha256=%s\n", hex);
    printf("verified=%s\n", res.verified ? "yes" : "no");
    printf("target=%s\n", target);
    rc = 0;
out:
    lmux_zmx_session_close(&session);
    return rc;
}

static int lmux_zeromux_install_main(int argc, char **argv)
{
    struct cli_opts opts;
    const char *local;
    const char *install_dir = NULL;
    const char *lmux_carrier = NULL;
    struct lmux_zmx_session session;
    struct lmux_zmx_probe_result probe;
    struct lmux_zmx_install_result res;
    char hex[65];
    int rc = 1;

    if (argc < 1)
        return fprintf(stderr, "Usage: lmux zeromux install FILE --tty DEVICE --install-dir TARGET_DIR --lmux-carrier TARGET_CARRIER [serial options]\n"), 1;
    local = argv[0];
    argv++;
    argc--;
    memset(&opts, 0, sizeof(opts));
    lmux_serial_config_default(&opts.serial);
    opts.timeout_ms = 10000;
    while (argc > 0) {
        int p = parse_serial_opts(&argc, &argv, &opts);
        if (p < 0)
            return 1;
        if (p > 0)
            continue;
        if (strcmp(argv[0], "--install-dir") == 0 && argc >= 2) {
            install_dir = argv[1];
            argv += 2;
            argc -= 2;
        } else if (strcmp(argv[0], "--lmux-carrier") == 0 && argc >= 2) {
            lmux_carrier = argv[1];
            argv += 2;
            argc -= 2;
        } else {
            return fprintf(stderr, "invalid zeromux install option: %s\n", argv[0]), 1;
        }
    }
    if (!opts.tty)
        return fprintf(stderr, "lmux zeromux install: --tty is required\n"), 1;
    if (!install_dir)
        return fprintf(stderr, "lmux zeromux install: --install-dir is required\n"), 1;
    if (!lmux_carrier)
        return fprintf(stderr, "lmux zeromux install: --lmux-carrier is required\n"), 1;

    if (lmux_zmx_session_open_tty(&session, opts.tty, &opts.serial,
                                  opts.timeout_ms) < 0) {
        fprintf(stderr, "%s: %s\n", opts.tty, strerror(errno));
        return 1;
    }
    if (lmux_zmx_probe_run(&session, &probe) < 0) {
        fprintf(stderr, "zeromux install: shell/probe failed: %s\n",
                lmux_zmx_error_name(probe.status));
        goto out;
    }
    if (lmux_zmx_install(&session, &probe, local, install_dir, lmux_carrier,
                         &res) < 0) {
        fprintf(stderr, "zeromux install failed: %s %s\n",
                lmux_zmx_install_status_name(res.status),
                res.detail[0] ? res.detail : "");
        if (res.installed)
            fprintf(stderr, "artifact installed; supervisor launch failed\n");
        goto out;
    }
    sha256_hex(res.sha256, hex);
    printf("zeromux install\n");
    printf("bootstrap=%s %u %u%s%u\n", opts.tty, opts.serial.baud,
           opts.serial.data_bits, parity_name(opts.serial.parity),
           opts.serial.stop_bits);
    printf("artifact sha256=%s\n", hex);
    printf("artifact=%s\n", res.already_installed ? "already-installed" : "uploaded");
    if (res.uploaded)
        printf("sender=%s\nuploaded bytes=%llu\n",
               res.sender_backend ? res.sender_backend : "unknown",
               (unsigned long long)res.uploaded_bytes);
    printf("installed=%s\n", res.final_path);
    printf("lmux_carrier=%s\n", lmux_carrier);
    printf("supervisor_pid=%lu\n", res.supervisor_pid);
    printf("status=started\n");
    rc = 0;
out:
    lmux_zmx_session_close(&session);
    return rc;
}

int lmux_zeromux_main(int argc, char **argv)
{
    if (argc >= 1 && strcmp(argv[0], "probe") == 0)
        return lmux_zeromux_probe_main(argc - 1, argv + 1);
    if (argc >= 1 && strcmp(argv[0], "send") == 0)
        return lmux_zeromux_send_main(argc - 1, argv + 1);
    if (argc >= 1 && strcmp(argv[0], "install") == 0)
        return lmux_zeromux_install_main(argc - 1, argv + 1);
    fprintf(stderr, "Usage: lmux zeromux probe --tty DEVICE [serial options]\n"
                    "       lmux zeromux send LOCAL_FILE TARGET_TEMP --tty DEVICE [serial options]\n"
                    "       lmux zeromux install FILE --tty DEVICE --install-dir TARGET_DIR --lmux-carrier TARGET_CARRIER [serial options]\n");
    return 1;
}
