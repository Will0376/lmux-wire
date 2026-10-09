#include "session.h"
#include "probe.h"
#include "sender.h"
#include "install.h"
#include "lxf.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum script_mode {
    MODE_BASIC_ECHO = 0,
    MODE_NO_ECHO,
    MODE_NOISE,
    MODE_AUTH,
    MODE_STALE,
    MODE_TRUNCATED,
    MODE_PROBE_MISSING,
    MODE_UPLOAD,
    MODE_UPLOAD_REMOTE_FAIL,
    MODE_UPLOAD_REMOTE_FAIL_MID,
    MODE_UPLOAD_BAD_SHA,
    MODE_UPLOAD_LOST,
    MODE_UPLOAD_STALL,
    MODE_INSTALL_FRESH,
    MODE_INSTALL_SAME,
    MODE_INSTALL_DIFFERENT,
    MODE_INSTALL_TRANSFER_FAIL,
    MODE_INSTALL_MOVE_FAIL,
    MODE_INSTALL_FINAL_BAD_SHA,
    MODE_INSTALL_LAUNCH_FAIL,
    MODE_INSTALL_DELAYED_DEATH,
    MODE_INSTALL_SAME_BUSY,
    MODE_INSTALL_MISSING_CAP
};

static int fail(const char *msg)
{
    fprintf(stderr, "test_zeromux: %s\n", msg);
    return 1;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int write_str(int fd, const char *s)
{
    return write_all(fd, s, strlen(s));
}

static int read_line(int fd, char *line, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r == 1) {
            line[n++] = ch;
            if (ch == '\n')
                break;
            continue;
        }
        if (r == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
    line[n] = 0;
    return (int)n;
}

static char *find_nonce(char *line)
{
    char *p = strstr(line, "ZMX_BEGIN_");
    char *e;
    if (!p)
        return NULL;
    p += strlen("ZMX_BEGIN_");
    e = strstr(p, "\\n");
    if (!e)
        e = strchr(p, '\n');
    if (e)
        *e = 0;
    return p;
}

static int copy_marker_nonce(const char *line, const char *prefix,
                             char *out, size_t cap)
{
    const char *p = strstr(line, prefix);
    size_t n = 0;
    if (!p || !out || cap == 0)
        return -1;
    p += strlen(prefix);
    while (*p && *p != '\\' && *p != '\'' && *p != '"' &&
           *p != '\r' && *p != '\n' && n + 1 < cap)
        out[n++] = *p++;
    out[n] = 0;
    return n ? 0 : -1;
}

static int copy_zmx_var_nonce(const char *line, char *out, size_t cap)
{
    const char *p = strstr(line, "zmx_n='");
    size_t n = 0;
    if (!p || !out || cap == 0)
        return -1;
    p += strlen("zmx_n='");
    while (*p && *p != '\'' && n + 1 < cap)
        out[n++] = *p++;
    out[n] = 0;
    return n ? 0 : -1;
}

static int uu_val(int ch)
{
    if (ch == '`' || ch == ' ')
        return 0;
    return (ch - 0x20) & 0x3f;
}

static int decode_uue_line(const char *line, uint8_t *out, size_t cap,
                           size_t *out_len)
{
    int n = uu_val((unsigned char)line[0]);
    size_t used = 0, i = 1;
    while (used < (size_t)n) {
        int a, b, c, d;
        if (!line[i] || !line[i + 1] || !line[i + 2] || !line[i + 3])
            return -1;
        a = uu_val((unsigned char)line[i++]);
        b = uu_val((unsigned char)line[i++]);
        c = uu_val((unsigned char)line[i++]);
        d = uu_val((unsigned char)line[i++]);
        if (used < cap) out[used++] = (uint8_t)((a << 2) | (b >> 4));
        if (used < (size_t)n && used < cap)
            out[used++] = (uint8_t)((b << 4) | (c >> 2));
        if (used < (size_t)n && used < cap)
            out[used++] = (uint8_t)((c << 6) | d);
        if (used > cap)
            return -1;
    }
    *out_len = used;
    return 0;
}

static void upload_target_loop(int fd, enum script_mode mode)
{
    char line[2048];
    uint8_t stored[8192], final_file[8192], digest[32];
    size_t stored_len = 0;
    size_t final_len = 0;
    int have_file = 0, removed = 0;
    int final_have = 0, dir_exists = 0, pid_alive = 0, kill_checks = 0;
    int install_mode = mode >= MODE_INSTALL_FRESH;
    int echo_disabled = 0, decoder_status = 0, in_data = 0;
    int saw_zero = 0, saw_uudecode = 0;
    char fg_nonce[128] = "";
    if (mode == MODE_INSTALL_SAME || mode == MODE_INSTALL_SAME_BUSY) {
        memcpy(final_file, "install-data", 12);
        final_len = 12;
        final_have = 1;
        dir_exists = 1;
    } else if (mode == MODE_INSTALL_DIFFERENT ||
               mode == MODE_INSTALL_MOVE_FAIL ||
               mode == MODE_INSTALL_LAUNCH_FAIL) {
        memcpy(final_file, "old-data", 8);
        final_len = 8;
        final_have = 1;
        dir_exists = 1;
    }
    while (read_line(fd, line, sizeof(line)) > 0) {
        char original[2048], out[256], hex[65];
        char *nonce;
        char nonce_copy[128];
        snprintf(original, sizeof(original), "%s", line);
        if (strstr(original, "<<"))
            _exit(41);
        if (in_data) {
            size_t got = 0;
            line[strcspn(line, "\r\n")] = 0;
            snprintf(out, sizeof(out), "~ZMX_DATA_END_%s", fg_nonce);
            if (strcmp(line, out) == 0) {
                have_file = !decoder_status;
                if (have_file)
                    removed = 0;
                echo_disabled = 0;
                snprintf(out, sizeof(out), "ZMX_END_%s_%d\n", fg_nonce,
                         decoder_status);
                write_str(fd, out);
                fg_nonce[0] = 0;
                in_data = 0;
                continue;
            }
            if (decoder_status)
                continue;
            if (strncmp(line, "begin ", 6) == 0) {
                if (mode == MODE_UPLOAD_REMOTE_FAIL ||
                    mode == MODE_INSTALL_TRANSFER_FAIL)
                    decoder_status = 1;
                continue;
            }
            if (strcmp(line, "`") == 0) {
                saw_zero = 1;
                continue;
            }
            if (strcmp(line, "end") == 0)
                continue;
            if (saw_zero)
                decoder_status = 1;
            if (decode_uue_line(line, stored + stored_len,
                                sizeof(stored) - stored_len, &got) == 0) {
                stored_len += got;
                if (mode == MODE_UPLOAD_REMOTE_FAIL_MID && stored_len > 0)
                    decoder_status = 1;
            } else {
                decoder_status = 1;
            }
            continue;
        }
        if (copy_zmx_var_nonce(original, fg_nonce, sizeof(fg_nonce)) == 0)
            continue;
        if (strstr(original, "stty -echo")) {
            echo_disabled = 1;
            continue;
        }
        if (strstr(original, "uudecode -o ")) {
            stored_len = 0;
            have_file = 0;
            saw_zero = 0;
            saw_uudecode = 1;
            decoder_status = strstr(original, " -o '/tmp'") ? 1 : 0;
            if (!echo_disabled)
                decoder_status = 1;
            if (mode == MODE_UPLOAD_LOST)
                _exit(0);
            continue;
        }
        if (strstr(original, ")") && saw_uudecode && fg_nonce[0]) {
            snprintf(out, sizeof(out), "ZMX_READY_%s\n", fg_nonce);
            write_str(fd, out);
            if (mode == MODE_UPLOAD_STALL)
                pause();
            in_data = 1;
            continue;
        }
        nonce = find_nonce(line);
        if (!nonce)
            continue;
        snprintf(nonce_copy, sizeof(nonce_copy), "%s", nonce);
        nonce = nonce_copy;
        snprintf(out, sizeof(out), "ZMX_BEGIN_%s\n", nonce);
        write_str(fd, out);
        if (strstr(original, "command -v")) {
            int ok = 0;
            if (strstr(original, "command -v uudecode ") ||
                strstr(original, "command -v sha256sum ") ||
                strstr(original, "command -v stty ") ||
                strstr(original, "command -v rm ") ||
                strstr(original, "command -v printf ") ||
                strstr(original, "command -v chmod ") ||
                strstr(original, "command -v mv ") ||
                strstr(original, "command -v sync ") ||
                strstr(original, "command -v kill ") ||
                (strstr(original, "command -v mkdir ") &&
                 mode != MODE_INSTALL_MISSING_CAP)) {
                write_str(fd, "/bin/tool\n");
                ok = 1;
            }
            snprintf(out, sizeof(out), "ZMX_END_%s_%d\n", nonce,
                     ok ? 0 : 1);
            write_str(fd, out);
        } else if (install_mode && strstr(original, "mkdir -p")) {
            dir_exists = 1;
            snprintf(out, sizeof(out), "ZMX_END_%s_%d\n", nonce,
                     dir_exists ? 0 : 1);
            write_str(fd, out);
        } else if (strstr(original, "sha256sum")) {
            int use_final = install_mode && strstr(original, "/lmuxd") &&
                !strstr(original, ".lmuxd.zmx.");
            int ok = use_final ? final_have : (have_file && !removed);
            if (ok) {
                struct sha256_ctx sha;
                sha256_init(&sha);
                if (use_final)
                    sha256_update(&sha, final_file, final_len);
                else
                    sha256_update(&sha, stored, stored_len);
                sha256_final(&sha, digest);
                sha256_hex(digest, hex);
                if (mode == MODE_UPLOAD_BAD_SHA ||
                    mode == MODE_INSTALL_FINAL_BAD_SHA)
                    hex[0] = hex[0] == '0' ? '1' : '0';
                snprintf(out, sizeof(out), "%s  target\n", hex);
                write_str(fd, out);
                snprintf(out, sizeof(out), "ZMX_END_%s_0\n", nonce);
            } else {
                snprintf(out, sizeof(out), "ZMX_END_%s_1\n", nonce);
            }
            write_str(fd, out);
        } else if (strstr(original, "rm -f")) {
            removed = 1;
            have_file = 0;
            snprintf(out, sizeof(out), "ZMX_END_%s_0\n", nonce);
            write_str(fd, out);
        } else if (install_mode && strstr(original, "chmod 0755") &&
                   strstr(original, "mv -f")) {
            if (mode == MODE_INSTALL_MOVE_FAIL || !have_file) {
                snprintf(out, sizeof(out), "ZMX_END_%s_1\n", nonce);
            } else {
                memcpy(final_file, stored, stored_len);
                final_len = stored_len;
                final_have = 1;
                have_file = 0;
                snprintf(out, sizeof(out), "ZMX_END_%s_0\n", nonce);
            }
            write_str(fd, out);
        } else if (install_mode && strstr(original, "chmod 0755")) {
            snprintf(out, sizeof(out), "ZMX_END_%s_%d\n", nonce,
                     final_have ? 0 : 1);
            write_str(fd, out);
        } else if (install_mode && strstr(original, "--supervisor") &&
                   strstr(original, "--outer") &&
                   strstr(original, "--carrier")) {
            if (!strstr(original, "</dev/null") ||
                !strstr(original, "2>&1") ||
                !strstr(original, "/dev/ttyGS0")) {
                snprintf(out, sizeof(out), "ZMX_END_%s_1\n", nonce);
            } else {
                write_str(fd, "1234\n");
                pid_alive = mode != MODE_INSTALL_LAUNCH_FAIL;
                snprintf(out, sizeof(out), "ZMX_END_%s_0\n", nonce);
            }
            write_str(fd, out);
        } else if (install_mode && strstr(original, "kill -0")) {
            kill_checks++;
            if ((mode == MODE_INSTALL_DELAYED_DEATH ||
                 mode == MODE_INSTALL_SAME_BUSY) && kill_checks > 1)
                pid_alive = 0;
            snprintf(out, sizeof(out), "ZMX_END_%s_%d\n", nonce,
                     pid_alive ? 0 : 1);
            write_str(fd, out);
        } else {
            snprintf(out, sizeof(out), "ZMX_END_%s_0\n", nonce);
            write_str(fd, out);
        }
    }
    _exit(0);
}

static void target_loop(int fd, enum script_mode mode)
{
    char line[2048];
    int tx = 0;
    if (mode == MODE_AUTH) {
        write_all(fd, "login: ", 7);
        sleep(2);
        _exit(0);
    }
    if (mode == MODE_NOISE)
        write_all(fd, "kernel printk before prompt\r\n# ", 31);
    if (mode == MODE_STALE)
        write_str(fd, "ZMX_BEGIN_stale\nold\nZMX_END_stale_0\n");
    while (read_line(fd, line, sizeof(line)) > 0) {
        char original[2048];
        char *nonce;
        char out[512];
        snprintf(original, sizeof(original), "%s", line);
        nonce = find_nonce(line);
        if (!nonce)
            continue;
        if (mode == MODE_BASIC_ECHO) {
            snprintf(out, sizeof(out), "echoed command for %s\n", nonce);
            write_all(fd, out, strlen(out));
        }
        tx++;
        if (mode == MODE_TRUNCATED) {
            snprintf(out, sizeof(out), "ZMX_BEGIN_%s\npartial\n", nonce);
            write_all(fd, out, strlen(out));
            continue;
        }
        if (mode == MODE_NOISE)
            write_all(fd, "random console text\r\n", 21);
        snprintf(out, sizeof(out), "ZMX_BEGIN_%s\r\n", nonce);
        if (mode == MODE_NOISE) {
            write_all(fd, out, 5);
            write_all(fd, out + 5, strlen(out) - 5);
        } else {
            write_all(fd, out, strlen(out));
        }
        if (mode == MODE_PROBE_MISSING) {
            if (strstr(original, "uname -s"))
                write_str(fd, "Linux\n");
            else if (strstr(original, "uname -m"))
                write_str(fd, "armv5tel\n");
            else if (strstr(original, "pwd"))
                write_str(fd, "/root\n");
            else if (strstr(original, "command -v rz")) {
                snprintf(out, sizeof(out), "ZMX_END_%s_1\r\n# ", nonce);
                write_all(fd, out, strlen(out));
                continue;
            } else if (strstr(original, "command -v printf")) {
                write_str(fd, "/bin/printf\n");
            }
        } else {
            snprintf(out, sizeof(out), "payload-%d\r\n", tx);
            write_all(fd, out, strlen(out));
        }
        snprintf(out, sizeof(out), "ZMX_END_%s_0\r\n# ", nonce);
        write_all(fd, out, strlen(out));
    }
    _exit(0);
}

static pid_t spawn_target(int *client_fd, enum script_mode mode)
{
    int sv[2];
    pid_t pid;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        close(sv[0]);
        if (mode == MODE_UPLOAD || mode == MODE_UPLOAD_REMOTE_FAIL ||
            mode == MODE_UPLOAD_REMOTE_FAIL_MID ||
            mode == MODE_UPLOAD_BAD_SHA || mode == MODE_UPLOAD_LOST ||
            mode == MODE_UPLOAD_STALL || mode >= MODE_INSTALL_FRESH)
            upload_target_loop(sv[1], mode);
        else
            target_loop(sv[1], mode);
    }
    close(sv[1]);
    *client_fd = sv[0];
    return pid;
}

static int uuencode_vectors(void)
{
    char out[256];
    size_t len;
    uint8_t abc[] = { 'a', 'b', 'c' };
    uint8_t one[] = { 0xff };
    uint8_t block[46];
    size_t i;
    if (lmux_zmx_uuencode_line(NULL, 0, out, sizeof(out), &len) < 0 ||
        strcmp(out, "`\n") != 0)
        return -1;
    if (lmux_zmx_uuencode_line(abc, sizeof(abc), out, sizeof(out), &len) < 0 ||
        strcmp(out, "#86)C\n") != 0)
        return -1;
    if (lmux_zmx_uuencode_line(one, sizeof(one), out, sizeof(out), &len) < 0 ||
        len != 6)
        return -1;
    for (i = 0; i < 45; i++)
        block[i] = (uint8_t)i;
    for (i = 0; i <= 45; i++) {
        if (lmux_zmx_uuencode_line(block, i, out, sizeof(out), &len) < 0 ||
            out[0] == '~')
            return -1;
    }
    if (lmux_zmx_uuencode_line(block, 45, out, sizeof(out), &len) < 0 ||
        len != 62)
        return -1;
    if (lmux_zmx_uuencode_line(block, 46, out, sizeof(out), &len) == 0)
        return -1;
    return 0;
}

static void probe_set_tool(struct lmux_zmx_probe_result *r, const char *name,
                           enum lmux_zmx_tri v)
{
    size_t i;
    for (i = 0; i < LMUX_ZMX_TOOL_COUNT; i++) {
        if (strcmp(r->tools[i].name, name) == 0) {
            r->tools[i].state = v;
            return;
        }
    }
}

static int sender_capability_case(void)
{
    struct lmux_zmx_probe_result r;
    lmux_zmx_probe_result_init(&r);
    probe_set_tool(&r, "uudecode", LMUX_ZMX_YES);
    if (lmux_zmx_probe_tool_state(&r, "uudecode") != LMUX_ZMX_YES)
        return -1;
    probe_set_tool(&r, "uudecode", LMUX_ZMX_UNKNOWN);
    if (lmux_zmx_probe_tool_state(&r, "uudecode") != LMUX_ZMX_UNKNOWN)
        return -1;
    return 0;
}

static int sender_upload_case(void)
{
    char path[] = "/tmp/lmux-zmx-src-XXXXXX";
    const uint8_t data[] = { 0, 1, 2, 3, 4, 5, 0xff, 'L', 'M', 'U', 'X' };
    int lfd = -1, fd = -1, rc = 1, st;
    pid_t pid;
    struct lmux_zmx_session s;
    struct lmux_zmx_probe_result probe;
    struct lmux_zmx_sender_result res;

    lfd = mkstemp(path);
    if (lfd < 0)
        return -1;
    if (write_all(lfd, (const char *)data, sizeof(data)) < 0 ||
        close(lfd) < 0)
        goto out_no_pid;
    lfd = -1;
    pid = spawn_target(&fd, MODE_UPLOAD);
    if (pid < 0)
        goto out_no_pid;
    if (lmux_zmx_session_use_fd(&s, fd, 1, 2000) < 0)
        goto out;
    fd = -1;
    if (lmux_zmx_probe_run(&s, &probe) < 0)
        goto out;
    if (lmux_zmx_sender_upload_file(&s, &probe, path,
                                    "/tmp/zmx-test.bin",
                                    &res) < 0 ||
        res.status != LMUX_ZMX_SENDER_OK || !res.verified ||
        res.bytes != sizeof(data)) {
        fprintf(stderr, "sender_upload status=%s detail=%s bytes=%llu verified=%d session=%s\n",
                lmux_zmx_sender_status_name(res.status), res.detail,
                (unsigned long long)res.bytes, res.verified,
                lmux_zmx_error_name(s.last_error));
        goto out;
    }
    rc = 0;
out:
    lmux_zmx_session_close(&s);
    if (fd >= 0)
        close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
out_no_pid:
    if (lfd >= 0)
        close(lfd);
    unlink(path);
    return rc;
}

static int shell_quote_case(void)
{
    char out[256];
    if (lmux_zmx_shell_quote("/tmp/a b'$(not-run).bin", out, sizeof(out)) < 0)
        return -1;
    return strcmp(out, "'/tmp/a b'\\''$(not-run).bin'") == 0 ? 0 : -1;
}

static int sender_unsupported_case(void)
{
    struct lmux_zmx_probe_result probe;
    struct lmux_zmx_session s;
    struct lmux_zmx_sender_result res;
    lmux_zmx_probe_result_init(&probe);
    lmux_zmx_session_init(&s);
    probe_set_tool(&probe, "uudecode", LMUX_ZMX_NO);
    if (lmux_zmx_sender_upload_file(&s, &probe, "/no/file", "/tmp/x", &res) == 0)
        return -1;
    return res.status == LMUX_ZMX_SENDER_UNSUPPORTED ? 0 : -1;
}

static int sender_failure_case(enum script_mode mode,
                               enum lmux_zmx_sender_status expect)
{
    char path[] = "/tmp/lmux-zmx-src-XXXXXX";
    const uint8_t data[] = { 'f', 'a', 'i', 'l', 0, 0xff };
    int lfd = -1, fd = -1, rc = 1, st;
    pid_t pid;
    struct lmux_zmx_session s;
    struct lmux_zmx_probe_result probe;
    struct lmux_zmx_sender_result res;

    lfd = mkstemp(path);
    if (lfd < 0)
        return -1;
    if (write_all(lfd, (const char *)data, sizeof(data)) < 0 ||
        close(lfd) < 0)
        goto out_no_pid;
    lfd = -1;
    pid = spawn_target(&fd, mode);
    if (pid < 0)
        goto out_no_pid;
    if (lmux_zmx_session_use_fd(&s, fd, 1, 800) < 0)
        goto out;
    fd = -1;
    if (lmux_zmx_probe_run(&s, &probe) < 0)
        goto out;
    if (lmux_zmx_sender_upload_file(&s, &probe, path,
                                    mode == MODE_UPLOAD_REMOTE_FAIL ?
                                    "/tmp" : "/tmp/fail.bin", &res) < 0 &&
        res.status == expect) {
        if (expect == LMUX_ZMX_SENDER_REMOTE_FAILED ||
            expect == LMUX_ZMX_SENDER_VERIFY_FAILED) {
            struct lmux_zmx_command_result cr;
            if (lmux_zmx_session_command(&s, ":", &cr) == 0 &&
                cr.exit_status == 0)
                rc = 0;
        } else {
            rc = 0;
        }
    } else
        fprintf(stderr, "sender_failure mode=%d expect=%s got=%s detail=%s last_error=%s phase=%s\n",
                mode, lmux_zmx_sender_status_name(expect),
                lmux_zmx_sender_status_name(res.status), res.detail,
                lmux_zmx_error_name(s.last_error), s.last_phase);
out:
    lmux_zmx_session_close(&s);
    if (fd >= 0)
        close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
out_no_pid:
    if (lfd >= 0)
        close(lfd);
    unlink(path);
    return rc;
}

static int install_case(enum script_mode mode,
                        enum lmux_zmx_install_status expect,
                        int expect_ok)
{
    char path[] = "/tmp/lmux-zmx-install-src-XXXXXX";
    const char data[] = "install-data";
    int lfd = -1, fd = -1, rc = 1, st;
    pid_t pid;
    struct lmux_zmx_session s;
    struct lmux_zmx_probe_result probe;
    struct lmux_zmx_install_result res;

    lfd = mkstemp(path);
    if (lfd < 0)
        return -1;
    if (write_all(lfd, data, sizeof(data) - 1) < 0 ||
        close(lfd) < 0)
        goto out_no_pid;
    lfd = -1;
    pid = spawn_target(&fd, mode);
    if (pid < 0)
        goto out_no_pid;
    if (lmux_zmx_session_use_fd(&s, fd, 1, 1200) < 0)
        goto out;
    fd = -1;
    if (lmux_zmx_probe_run(&s, &probe) < 0)
        goto out;
    if (lmux_zmx_install(&s, &probe, path, "/root/zmx lmux",
                         "/dev/ttyGS0", &res) == 0) {
        struct lmux_zmx_command_result cr;
        if (!expect_ok || res.status != LMUX_ZMX_INSTALL_OK ||
            !res.installed || !res.launched || res.supervisor_pid != 1234) {
            fprintf(stderr, "install_case unexpected success mode=%d expect_ok=%d status=%s\n",
                    mode, expect_ok, lmux_zmx_install_status_name(res.status));
            goto out;
        }
        if (mode == MODE_INSTALL_SAME && (!res.already_installed || res.uploaded))
            goto out;
        if (mode == MODE_INSTALL_FRESH && (!res.uploaded || res.already_installed))
            goto out;
        if (lmux_zmx_session_command(&s, ":", &cr) < 0 || cr.exit_status != 0)
            goto out;
        rc = 0;
    } else if (!expect_ok && res.status == expect) {
        if ((mode == MODE_INSTALL_LAUNCH_FAIL ||
             mode == MODE_INSTALL_DELAYED_DEATH ||
             mode == MODE_INSTALL_SAME_BUSY) && !res.installed)
            goto out;
        if (mode == MODE_INSTALL_SAME_BUSY &&
            (!res.already_installed || res.uploaded))
            goto out;
        rc = 0;
    } else {
        fprintf(stderr, "install_case mode=%d status=%s detail=%s installed=%d launched=%d uploaded=%d already=%d pid=%lu\n",
                mode, lmux_zmx_install_status_name(res.status), res.detail,
                res.installed, res.launched, res.uploaded,
                res.already_installed, res.supervisor_pid);
    }
out:
    lmux_zmx_session_close(&s);
    if (fd >= 0)
        close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
out_no_pid:
    if (lfd >= 0)
        close(lfd);
    unlink(path);
    return rc;
}

static int install_bad_path_case(void)
{
    struct lmux_zmx_session s;
    struct lmux_zmx_probe_result probe;
    struct lmux_zmx_install_result res;
    lmux_zmx_session_init(&s);
    lmux_zmx_probe_result_init(&probe);
    probe_set_tool(&probe, "uudecode", LMUX_ZMX_YES);
    probe_set_tool(&probe, "stty", LMUX_ZMX_YES);
    probe_set_tool(&probe, "sha256sum", LMUX_ZMX_YES);
    probe_set_tool(&probe, "mkdir", LMUX_ZMX_YES);
    probe_set_tool(&probe, "mv", LMUX_ZMX_YES);
    probe_set_tool(&probe, "chmod", LMUX_ZMX_YES);
    probe_set_tool(&probe, "rm", LMUX_ZMX_YES);
    probe_set_tool(&probe, "sync", LMUX_ZMX_YES);
    probe_set_tool(&probe, "kill", LMUX_ZMX_YES);
    if (lmux_zmx_install(&s, &probe, "/no/file", "/bad\npath",
                         "/dev/ttyGS0", &res) == 0)
        return -1;
    return res.status == LMUX_ZMX_INSTALL_BAD_PATH ? 0 : -1;
}

struct slow_payload_ctx {
    int count;
    useconds_t delay_us;
};

static int slow_payload(struct lmux_zmx_stream *st, void *arg)
{
    struct slow_payload_ctx *ctx = arg;
    int i;
    for (i = 0; i < ctx->count; i++) {
        if (lmux_zmx_stream_write(st, "x\n", 2) < 0)
            return -1;
        usleep(ctx->delay_us);
    }
    return 0;
}

static void foreground_progress_target(int fd, int count)
{
    char line[256], nonce[128] = "";
    int payload_lines = 0;
    while (read_line(fd, line, sizeof(line)) > 0) {
        char out[256];
        if (!nonce[0] &&
            copy_marker_nonce(line, "ZMX_READY_", nonce, sizeof(nonce)) == 0) {
            snprintf(out, sizeof(out), "ZMX_READY_%s\n", nonce);
            write_str(fd, out);
            continue;
        }
        if (strstr(line, "cat >/dev/null"))
            continue;
        if (nonce[0] && strstr(line, "~ZMX_DATA_END_")) {
            snprintf(out, sizeof(out), "ZMX_END_%s_0\n", nonce);
            write_str(fd, out);
            break;
        }
        if (nonce[0] && payload_lines < count) {
            payload_lines++;
            continue;
        }
    }
    _exit(0);
}

static void foreground_marker_target(int fd)
{
    char line[256], nonce[128] = "";
    while (read_line(fd, line, sizeof(line)) > 0) {
        char out[512];
        if (!nonce[0] &&
            copy_marker_nonce(line, "ZMX_READY_", nonce, sizeof(nonce)) == 0) {
            snprintf(out, sizeof(out),
                     "printf 'ZMX_READY_%s\\n'\n"
                     "fooZMX_READY_%s\n"
                     "ZMX_READY_wrongnonce\n"
                     "ZMX_READY_%s trailing\n"
                     "ZMX_READY_%s\n",
                     nonce, nonce, nonce, nonce);
            write_str(fd, out);
            continue;
        }
        if (strstr(line, "cat >/dev/null"))
            continue;
        if (nonce[0] && strstr(line, "~ZMX_DATA_END_")) {
            snprintf(out, sizeof(out),
                     "fooZMX_END_%s_0\n"
                     "ZMX_END_wrongnonce_0\n"
                     "ZMX_END_%s_0 trailing\n"
                     "ZMX_END_%s_0\n",
                     nonce, nonce, nonce);
            write_str(fd, out);
            break;
        }
    }
    _exit(0);
}

static int foreground_progress_case(void)
{
    int sv[2], st, rc = 1;
    pid_t pid;
    struct lmux_zmx_session s;
    struct lmux_zmx_command_result res;
    struct slow_payload_ctx ctx;
    const char *start = "\nprintf 'ZMX_READY_%s\\n'\ncat >/dev/null\n";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        close(sv[0]);
        foreground_progress_target(sv[1], 12);
    }
    close(sv[1]);
    ctx.count = 12;
    ctx.delay_us = 40000;
    if (lmux_zmx_session_use_fd(&s, sv[0], 1, 150) < 0)
        goto out;
    sv[0] = -1;
    if (lmux_zmx_session_stream_foreground(&s, start, "",
                                           slow_payload, &ctx, &res) == 0 &&
        res.exit_status == 0)
        rc = 0;
out:
    lmux_zmx_session_close(&s);
    if (sv[0] >= 0)
        close(sv[0]);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    return rc;
}

static int foreground_marker_case(void)
{
    int sv[2], st, rc = 1;
    pid_t pid;
    struct lmux_zmx_session s;
    struct lmux_zmx_command_result res;
    struct slow_payload_ctx ctx;
    const char *start = "\nprintf 'ZMX_READY_%s\\n'\ncat >/dev/null\n";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        close(sv[0]);
        foreground_marker_target(sv[1]);
    }
    close(sv[1]);
    ctx.count = 1;
    ctx.delay_us = 0;
    if (lmux_zmx_session_use_fd(&s, sv[0], 1, 3000) < 0)
        goto out;
    sv[0] = -1;
    if (lmux_zmx_session_stream_foreground(&s, start, "", slow_payload,
                                           &ctx, &res) == 0 &&
        res.exit_status == 0)
        rc = 0;
out:
    lmux_zmx_session_close(&s);
    if (sv[0] >= 0)
        close(sv[0]);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    return rc;
}

static int run_command_case(enum script_mode mode, int expect_ok)
{
    int fd = -1, rc = 1, st;
    pid_t pid = spawn_target(&fd, mode);
    struct lmux_zmx_session s;
    struct lmux_zmx_command_result res;
    if (pid < 0)
        return fail("spawn target");
    if (lmux_zmx_session_use_fd(&s, fd, 1, 500) < 0)
        goto out;
    fd = -1;
    if (lmux_zmx_session_command(&s, "printf test", &res) == 0) {
        if (!expect_ok || !strstr(res.output, "payload-"))
            goto out;
    } else if (expect_ok) {
        goto out;
    }
    rc = 0;
out:
    lmux_zmx_session_close(&s);
    if (fd >= 0)
        close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    return rc;
}

static int auth_case(void)
{
    int fd = -1, rc = 1, st;
    pid_t pid = spawn_target(&fd, MODE_AUTH);
    struct lmux_zmx_session s;
    struct lmux_zmx_probe_result r;
    if (pid < 0)
        return fail("spawn auth target");
    if (lmux_zmx_session_use_fd(&s, fd, 1, 500) < 0)
        goto out;
    fd = -1;
    if (lmux_zmx_probe_run(&s, &r) < 0 &&
        r.status == LMUX_ZMX_AUTH_REQUIRED && r.auth_required)
        rc = 0;
out:
    lmux_zmx_session_close(&s);
    if (fd >= 0)
        close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    return rc;
}

static int probe_degrades(void)
{
    int fd = -1, rc = 1, st;
    pid_t pid = spawn_target(&fd, MODE_PROBE_MISSING);
    struct lmux_zmx_session s;
    struct lmux_zmx_probe_result r;
    if (pid < 0)
        return fail("spawn probe target");
    if (lmux_zmx_session_use_fd(&s, fd, 1, 1000) < 0)
        goto out;
    fd = -1;
    if (lmux_zmx_probe_run(&s, &r) == 0 &&
        r.shell_usable &&
        strcmp(r.kernel, "Linux") == 0 &&
        r.tools[0].state == LMUX_ZMX_NO &&
        r.tools[12].state == LMUX_ZMX_YES)
        rc = 0;
    else
        fprintf(stderr, "probe_degrades status=%s shell=%d kernel=%s rz=%s printf=%s\n",
                lmux_zmx_error_name(r.status), r.shell_usable, r.kernel,
                lmux_zmx_tri_name(r.tools[0].state),
                lmux_zmx_tri_name(r.tools[12].state));
out:
    lmux_zmx_session_close(&s);
    if (fd >= 0)
        close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    return rc;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (run_command_case(MODE_BASIC_ECHO, 1))
        return fail("basic echo");
    if (run_command_case(MODE_NO_ECHO, 1))
        return fail("no echo");
    if (run_command_case(MODE_NOISE, 1))
        return fail("noise/fragment");
    if (run_command_case(MODE_STALE, 1))
        return fail("stale marker");
    if (run_command_case(MODE_TRUNCATED, 0))
        return fail("truncated timeout");
    if (auth_case())
        return fail("auth");
    if (probe_degrades())
        return fail("probe degrade");
    if (uuencode_vectors())
        return fail("uuencode vectors");
    if (shell_quote_case())
        return fail("shell quote");
    if (foreground_progress_case())
        return fail("foreground progress timeout");
    if (foreground_marker_case())
        return fail("foreground marker framing");
    if (sender_capability_case())
        return fail("sender capability");
    if (sender_upload_case())
        return fail("sender upload");
    if (sender_unsupported_case())
        return fail("sender unsupported");
    if (sender_failure_case(MODE_UPLOAD_REMOTE_FAIL,
                            LMUX_ZMX_SENDER_REMOTE_FAILED))
        return fail("sender remote failure");
    if (sender_failure_case(MODE_UPLOAD_REMOTE_FAIL_MID,
                            LMUX_ZMX_SENDER_REMOTE_FAILED))
        return fail("sender remote mid failure");
    if (sender_failure_case(MODE_UPLOAD_BAD_SHA,
                            LMUX_ZMX_SENDER_VERIFY_FAILED))
        return fail("sender sha failure");
    if (sender_failure_case(MODE_UPLOAD_LOST,
                            LMUX_ZMX_SENDER_SESSION_LOST))
        return fail("sender lost");
    if (sender_failure_case(MODE_UPLOAD_STALL,
                            LMUX_ZMX_SENDER_TIMEOUT))
        return fail("sender stall timeout");
    if (install_case(MODE_INSTALL_FRESH, LMUX_ZMX_INSTALL_OK, 1))
        return fail("install fresh");
    if (install_case(MODE_INSTALL_SAME, LMUX_ZMX_INSTALL_OK, 1))
        return fail("install same");
    if (install_case(MODE_INSTALL_DIFFERENT, LMUX_ZMX_INSTALL_OK, 1))
        return fail("install replace");
    if (install_case(MODE_INSTALL_TRANSFER_FAIL,
                     LMUX_ZMX_INSTALL_REMOTE_FAILED, 0))
        return fail("install transfer failure");
    if (install_case(MODE_INSTALL_MOVE_FAIL,
                     LMUX_ZMX_INSTALL_REMOTE_FAILED, 0))
        return fail("install move failure");
    if (install_case(MODE_INSTALL_FINAL_BAD_SHA,
                     LMUX_ZMX_INSTALL_VERIFY_FAILED, 0))
        return fail("install final sha failure");
    if (install_case(MODE_INSTALL_LAUNCH_FAIL,
                     LMUX_ZMX_INSTALL_LAUNCH_FAILED, 0))
        return fail("install launch failure");
    if (install_case(MODE_INSTALL_DELAYED_DEATH,
                     LMUX_ZMX_INSTALL_LAUNCH_FAILED, 0))
        return fail("install delayed death");
    if (install_case(MODE_INSTALL_SAME_BUSY,
                     LMUX_ZMX_INSTALL_LAUNCH_FAILED, 0))
        return fail("install already busy");
    if (install_case(MODE_INSTALL_MISSING_CAP,
                     LMUX_ZMX_INSTALL_UNSUPPORTED, 0))
        return fail("install missing capability");
    if (install_bad_path_case())
        return fail("install bad path");
    puts("zeromux selftest ok");
    return 0;
}
