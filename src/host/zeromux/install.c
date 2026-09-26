#include "install.h"

#include "lxf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZMX_PATH_MAX 319
#define ZMX_STARTUP_GRACE_MS 1200
#define ZMX_STARTUP_CHECK_MS 100

static long long zmx_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

const char *lmux_zmx_install_status_name(enum lmux_zmx_install_status st)
{
    switch (st) {
    case LMUX_ZMX_INSTALL_OK: return "OK";
    case LMUX_ZMX_INSTALL_UNSUPPORTED: return "UNSUPPORTED";
    case LMUX_ZMX_INSTALL_BAD_PATH: return "BAD_PATH";
    case LMUX_ZMX_INSTALL_LOCAL_IO: return "LOCAL_IO";
    case LMUX_ZMX_INSTALL_REMOTE_FAILED: return "REMOTE_FAILED";
    case LMUX_ZMX_INSTALL_TRANSFER_FAILED: return "TRANSFER_FAILED";
    case LMUX_ZMX_INSTALL_VERIFY_FAILED: return "VERIFY_FAILED";
    case LMUX_ZMX_INSTALL_LAUNCH_FAILED: return "LAUNCH_FAILED";
    case LMUX_ZMX_INSTALL_SESSION_LOST: return "SESSION_LOST";
    case LMUX_ZMX_INSTALL_TIMEOUT: return "TIMEOUT";
    }
    return "?";
}

void lmux_zmx_install_result_init(struct lmux_zmx_install_result *r)
{
    memset(r, 0, sizeof(*r));
    r->status = LMUX_ZMX_INSTALL_OK;
}

static int safe_path(const char *p)
{
    const unsigned char *s = (const unsigned char *)p;
    if (!p || !*p || strlen(p) > 240)
        return 0;
    while (*s) {
        if (*s < 0x20 || *s == 0x7f)
            return 0;
        s++;
    }
    return 1;
}

static int path_join(char *out, size_t cap, const char *dir, const char *leaf)
{
    size_t dl;
    if (!out || !dir || !leaf || !*dir || !*leaf)
        return -1;
    dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/')
        dl--;
    if (snprintf(out, cap, "%.*s/%s", (int)dl, dir, leaf) >= (int)cap)
        return -1;
    return 0;
}

static void make_suffix(char out[33])
{
    uint32_t v[4] = { 0, 0, 0, 0 };
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)read(fd, v, sizeof(v));
        close(fd);
    }
    v[2] ^= (uint32_t)getpid();
    v[3] ^= (uint32_t)time(NULL);
    snprintf(out, 33, "%08x%08x%08x%08x", v[0], v[1], v[2], v[3]);
}

static enum lmux_zmx_install_status map_sender_status(enum lmux_zmx_sender_status st)
{
    switch (st) {
    case LMUX_ZMX_SENDER_OK: return LMUX_ZMX_INSTALL_OK;
    case LMUX_ZMX_SENDER_UNSUPPORTED: return LMUX_ZMX_INSTALL_UNSUPPORTED;
    case LMUX_ZMX_SENDER_SESSION_LOST: return LMUX_ZMX_INSTALL_SESSION_LOST;
    case LMUX_ZMX_SENDER_TIMEOUT: return LMUX_ZMX_INSTALL_TIMEOUT;
    case LMUX_ZMX_SENDER_REMOTE_FAILED: return LMUX_ZMX_INSTALL_REMOTE_FAILED;
    case LMUX_ZMX_SENDER_VERIFY_FAILED: return LMUX_ZMX_INSTALL_VERIFY_FAILED;
    case LMUX_ZMX_SENDER_LOCAL_IO: return LMUX_ZMX_INSTALL_LOCAL_IO;
    case LMUX_ZMX_SENDER_BAD_PATH: return LMUX_ZMX_INSTALL_BAD_PATH;
    case LMUX_ZMX_SENDER_TRANSFER_FAILED:
    default: return LMUX_ZMX_INSTALL_TRANSFER_FAILED;
    }
}

static enum lmux_zmx_install_status map_session_error(enum lmux_zmx_error e)
{
    if (e == LMUX_ZMX_TIMEOUT)
        return LMUX_ZMX_INSTALL_TIMEOUT;
    if (e == LMUX_ZMX_CARRIER_LOST)
        return LMUX_ZMX_INSTALL_SESSION_LOST;
    return LMUX_ZMX_INSTALL_REMOTE_FAILED;
}

static int require_tool(const struct lmux_zmx_probe_result *probe,
                        const char *name, struct lmux_zmx_install_result *res)
{
    if (lmux_zmx_probe_tool_state(probe, name) == LMUX_ZMX_YES)
        return 0;
    res->status = LMUX_ZMX_INSTALL_UNSUPPORTED;
    snprintf(res->detail, sizeof(res->detail), "missing capability: %s", name);
    return -1;
}

static int digest_local_file(const char *path, uint8_t digest[32],
                             uint64_t *size)
{
    int fd = open(path, O_RDONLY);
    struct sha256_ctx sha;
    uint8_t buf[4096];
    uint64_t total = 0;
    if (fd < 0)
        return -1;
    sha256_init(&sha);
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            sha256_update(&sha, buf, (size_t)n);
            total += (uint64_t)n;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    close(fd);
    sha256_final(&sha, digest);
    *size = total;
    return 0;
}

static int parse_sha256_output(const char *out, uint8_t digest[32])
{
    size_t i;
    for (i = 0; i < 32; i++) {
        int hi, lo;
        char a = out[i * 2], b = out[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        else return -1;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        else return -1;
        digest[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int run_ok(struct lmux_zmx_session *s, const char *cmd,
                  struct lmux_zmx_install_result *res, const char *what)
{
    struct lmux_zmx_command_result cr;
    if (lmux_zmx_session_command(s, cmd, &cr) < 0) {
        res->status = map_session_error(s->last_error);
        snprintf(res->detail, sizeof(res->detail), "%s failed: %s",
                 what, lmux_zmx_error_name(s->last_error));
        return -1;
    }
    if (cr.exit_status != 0) {
        res->status = LMUX_ZMX_INSTALL_REMOTE_FAILED;
        snprintf(res->detail, sizeof(res->detail), "%s exit=%d",
                 what, cr.exit_status);
        return -1;
    }
    return 0;
}

static int remote_sha(struct lmux_zmx_session *s, const char *quoted,
                      uint8_t digest[32], int *present,
                      struct lmux_zmx_install_result *res)
{
    char cmd[760];
    struct lmux_zmx_command_result cr;
    *present = 0;
    snprintf(cmd, sizeof(cmd), "[ -f %s ] && sha256sum %s 2>/dev/null",
             quoted, quoted);
    if (lmux_zmx_session_command(s, cmd, &cr) < 0) {
        res->status = map_session_error(s->last_error);
        snprintf(res->detail, sizeof(res->detail), "remote sha failed");
        return -1;
    }
    if (cr.exit_status != 0)
        return 0;
    if (parse_sha256_output(cr.output, digest) < 0) {
        res->status = LMUX_ZMX_INSTALL_VERIFY_FAILED;
        snprintf(res->detail, sizeof(res->detail), "bad remote sha output");
        return -1;
    }
    *present = 1;
    return 0;
}

static int cleanup_temp(struct lmux_zmx_session *s, const char *qtemp)
{
    char cmd[480];
    struct lmux_zmx_command_result cr;
    snprintf(cmd, sizeof(cmd), "rm -f -- %s", qtemp);
    return lmux_zmx_session_command(s, cmd, &cr);
}

static int launch_supervisor(struct lmux_zmx_session *s,
                             const char *qfinal, const char *qcarrier,
                             const char *qlog,
                             struct lmux_zmx_install_result *res)
{
    char cmd[1600], *end;
    struct lmux_zmx_command_result cr;
    unsigned long pid;
    long long deadline;
    if (snprintf(cmd, sizeof(cmd),
                 "%s --supervisor --outer --carrier %s </dev/null >%s 2>&1 & "
                 "zmx_pid=$!; printf '%%s\\n' \"$zmx_pid\"",
                 qfinal, qcarrier, qlog) >= (int)sizeof(cmd)) {
        res->status = LMUX_ZMX_INSTALL_BAD_PATH;
        snprintf(res->detail, sizeof(res->detail), "launch command too long");
        return -1;
    }
    if (lmux_zmx_session_command(s, cmd, &cr) < 0) {
        res->status = map_session_error(s->last_error);
        snprintf(res->detail, sizeof(res->detail), "launch command failed");
        return -1;
    }
    if (cr.exit_status != 0) {
        res->status = LMUX_ZMX_INSTALL_LAUNCH_FAILED;
        snprintf(res->detail, sizeof(res->detail), "launch shell exit=%d",
                 cr.exit_status);
        return -1;
    }
    pid = strtoul(cr.output, &end, 10);
    if (!end || (*end && *end != '\n') || pid == 0) {
        res->status = LMUX_ZMX_INSTALL_LAUNCH_FAILED;
        snprintf(res->detail, sizeof(res->detail), "bad launch pid");
        return -1;
    }
    res->supervisor_pid = pid;

    deadline = zmx_now_ms() + ZMX_STARTUP_GRACE_MS;
    for (;;) {
        snprintf(cmd, sizeof(cmd), "kill -0 %lu 2>/dev/null", pid);
        if (lmux_zmx_session_command(s, cmd, &cr) < 0) {
            res->status = map_session_error(s->last_error);
            snprintf(res->detail, sizeof(res->detail),
                     "alive check failed pid=%lu", pid);
            return -1;
        }
        if (cr.exit_status != 0) {
            res->status = LMUX_ZMX_INSTALL_LAUNCH_FAILED;
            snprintf(res->detail, sizeof(res->detail),
                     "supervisor exited during startup pid=%lu", pid);
            return -1;
        }
        if (zmx_now_ms() >= deadline)
            break;
        usleep(ZMX_STARTUP_CHECK_MS * 1000);
    }
    res->launched = 1;
    return 0;
}

int lmux_zmx_install(struct lmux_zmx_session *s,
                     const struct lmux_zmx_probe_result *probe,
                     const char *local_path,
                     const char *install_dir,
                     const char *lmux_carrier,
                     struct lmux_zmx_install_result *res)
{
    char suffix[33], tmp_leaf[80], log_leaf[80];
    char qdir[384], qfinal[384], qtemp[384], qcarrier[384], qlog[384];
    char cmd[1200];
    uint8_t remote_digest[32];
    int present = 0;
    struct lmux_zmx_sender_result sender;

    lmux_zmx_install_result_init(res);
    if (!s || !probe || !local_path || !install_dir || !lmux_carrier) {
        res->status = LMUX_ZMX_INSTALL_BAD_PATH;
        snprintf(res->detail, sizeof(res->detail), "missing argument");
        return -1;
    }
    if (!safe_path(install_dir) || !safe_path(lmux_carrier)) {
        res->status = LMUX_ZMX_INSTALL_BAD_PATH;
        snprintf(res->detail, sizeof(res->detail), "bad target path");
        return -1;
    }
    if (require_tool(probe, "uudecode", res) < 0 ||
        require_tool(probe, "stty", res) < 0 ||
        require_tool(probe, "sha256sum", res) < 0 ||
        require_tool(probe, "mkdir", res) < 0 ||
        require_tool(probe, "mv", res) < 0 ||
        require_tool(probe, "chmod", res) < 0 ||
        require_tool(probe, "rm", res) < 0 ||
        require_tool(probe, "sync", res) < 0 ||
        require_tool(probe, "kill", res) < 0)
        return -1;

    if (digest_local_file(local_path, res->sha256, &res->local_size) < 0) {
        res->status = LMUX_ZMX_INSTALL_LOCAL_IO;
        snprintf(res->detail, sizeof(res->detail), "local file read failed");
        return -1;
    }

    make_suffix(suffix);
    snprintf(tmp_leaf, sizeof(tmp_leaf), ".lmuxd.zmx.%s.tmp", suffix);
    snprintf(log_leaf, sizeof(log_leaf), "/tmp/lmux-zeromux-%s.log", suffix);
    if (path_join(res->final_path, sizeof(res->final_path), install_dir, "lmuxd") < 0 ||
        path_join(res->temp_path, sizeof(res->temp_path), install_dir, tmp_leaf) < 0 ||
        snprintf(res->log_path, sizeof(res->log_path), "%s", log_leaf) >=
        (int)sizeof(res->log_path)) {
        res->status = LMUX_ZMX_INSTALL_BAD_PATH;
        snprintf(res->detail, sizeof(res->detail), "path too long");
        return -1;
    }
    if (lmux_zmx_shell_quote(install_dir, qdir, sizeof(qdir)) < 0 ||
        lmux_zmx_shell_quote(res->final_path, qfinal, sizeof(qfinal)) < 0 ||
        lmux_zmx_shell_quote(res->temp_path, qtemp, sizeof(qtemp)) < 0 ||
        lmux_zmx_shell_quote(lmux_carrier, qcarrier, sizeof(qcarrier)) < 0 ||
        lmux_zmx_shell_quote(res->log_path, qlog, sizeof(qlog)) < 0) {
        res->status = LMUX_ZMX_INSTALL_BAD_PATH;
        snprintf(res->detail, sizeof(res->detail), "path quote failed");
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "mkdir -p -- %s && [ -d %s ]", qdir, qdir);
    if (run_ok(s, cmd, res, "mkdir") < 0)
        return -1;

    if (remote_sha(s, qfinal, remote_digest, &present, res) < 0)
        return -1;
    if (present && memcmp(remote_digest, res->sha256, 32) == 0) {
        res->already_installed = 1;
        snprintf(cmd, sizeof(cmd), "chmod 0755 %s", qfinal);
        if (run_ok(s, cmd, res, "chmod final") < 0)
            return -1;
    } else {
        (void)cleanup_temp(s, qtemp);
        if (lmux_zmx_sender_upload_file(s, probe, local_path, res->temp_path,
                                        &sender) < 0) {
            res->status = map_sender_status(sender.status);
            snprintf(res->detail, sizeof(res->detail), "upload: %s",
                     sender.detail[0] ? sender.detail :
                     lmux_zmx_sender_status_name(sender.status));
            (void)cleanup_temp(s, qtemp);
            return -1;
        }
        res->uploaded = 1;
        res->uploaded_bytes = sender.bytes;
        res->sender_backend = sender.backend;
        snprintf(cmd, sizeof(cmd), "chmod 0755 %s && mv -f %s %s && sync",
                 qtemp, qtemp, qfinal);
        if (run_ok(s, cmd, res, "install final") < 0) {
            (void)cleanup_temp(s, qtemp);
            return -1;
        }
    }
    res->installed = 1;

    if (remote_sha(s, qfinal, remote_digest, &present, res) < 0)
        return -1;
    if (!present || memcmp(remote_digest, res->sha256, 32) != 0) {
        res->status = LMUX_ZMX_INSTALL_VERIFY_FAILED;
        snprintf(res->detail, sizeof(res->detail), "final sha mismatch");
        return -1;
    }

    if (launch_supervisor(s, qfinal, qcarrier, qlog, res) < 0)
        return -1;
    return 0;
}
