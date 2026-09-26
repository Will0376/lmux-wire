#include "sender.h"

#include "lxf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UU_LINE_BYTES 45

static char uu_char(unsigned v)
{
    v &= 0x3f;
    return v ? (char)(v + 0x20) : '`';
}

int lmux_zmx_uuencode_line(const uint8_t *data, size_t len,
                           char *out, size_t cap, size_t *out_len)
{
    size_t i = 0, used = 0;
    if (!out || !out_len || len > UU_LINE_BYTES || cap < 3)
        return -1;
    out[used++] = uu_char((unsigned)len);
    while (i < len) {
        unsigned a = data[i++];
        unsigned b = i < len ? data[i++] : 0;
        unsigned c = i < len ? data[i++] : 0;
        if (used + 4 >= cap)
            return -1;
        out[used++] = uu_char(a >> 2);
        out[used++] = uu_char(((a << 4) | (b >> 4)) & 0x3f);
        out[used++] = uu_char(((b << 2) | (c >> 6)) & 0x3f);
        out[used++] = uu_char(c & 0x3f);
    }
    out[used++] = '\n';
    out[used] = 0;
    *out_len = used;
    return 0;
}

struct uudecode_ctx {
    const char *local_path;
    const char *target_path;
    int fd;
    uint64_t bytes;
    struct sha256_ctx sha;
    uint8_t digest[32];
    enum lmux_zmx_sender_status status;
};

static int valid_uue_target_path(const char *p)
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

static int write_strm(struct lmux_zmx_stream *st, const char *s)
{
    return lmux_zmx_stream_write(st, s, strlen(s));
}

static int uudecode_payload(struct lmux_zmx_stream *st, void *arg)
{
    struct uudecode_ctx *ctx = arg;
    uint8_t in[UU_LINE_BYTES];
    char line[128];
    size_t out_len;
    ssize_t r;
    char header[320];

    (void)ctx->target_path;
    snprintf(header, sizeof(header), "begin 755 zmx_payload\n");
    if (write_strm(st, header) < 0)
        return -1;
    for (;;) {
        r = read(ctx->fd, in, sizeof(in));
        if (r > 0) {
            sha256_update(&ctx->sha, in, (size_t)r);
            ctx->bytes += (uint64_t)r;
            if (lmux_zmx_uuencode_line(in, (size_t)r, line,
                                       sizeof(line), &out_len) < 0 ||
                lmux_zmx_stream_write(st, line, out_len) < 0)
                return -1;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        ctx->status = LMUX_ZMX_SENDER_LOCAL_IO;
        break;
    }
    sha256_final(&ctx->sha, ctx->digest);
    return write_strm(st, "`\nend\n");
}

static enum lmux_zmx_sender_status map_session_error(enum lmux_zmx_error e)
{
    if (e == LMUX_ZMX_TIMEOUT)
        return LMUX_ZMX_SENDER_TIMEOUT;
    if (e == LMUX_ZMX_CARRIER_LOST)
        return LMUX_ZMX_SENDER_SESSION_LOST;
    return LMUX_ZMX_SENDER_TRANSFER_FAILED;
}

static void cleanup_target(struct lmux_zmx_session *s, const char *quoted,
                           struct lmux_zmx_sender_result *res)
{
    char cmd[640];
    struct lmux_zmx_command_result cr;
    res->cleanup_attempted = 1;
    snprintf(cmd, sizeof(cmd), "rm -f -- %s", quoted);
    if (lmux_zmx_session_command(s, cmd, &cr) == 0 && cr.exit_status == 0)
        res->cleanup_ok = 1;
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

int lmux_zmx_sender_uudecode_upload(struct lmux_zmx_session *s,
                                    const struct lmux_zmx_probe_result *probe,
                                    const char *local_path,
                                    const char *target_temp,
                                    struct lmux_zmx_sender_result *res)
{
    struct uudecode_ctx ctx;
    struct lmux_zmx_command_result cr;
    char quoted[512], verify_cmd[640];
    const char *start_cmd =
        "\n(\n"
        "zmx_n='%s'\n"
        "stty -echo\n"
        "printf 'ZMX_READY_%%s\\n' \"$zmx_n\"\n"
        "uudecode -o %s\n"
        "zmx_s=$?\n"
        "while IFS= read -r zmx_line; do\n"
        "    [ \"x$zmx_line\" = \"x~ZMX_DATA_END_$zmx_n\" ] && break\n"
        "done\n"
        "stty echo\n"
        "printf 'ZMX_END_%%s_%%d\\n' \"$zmx_n\" \"$zmx_s\"\n"
        ")\n";
    uint8_t remote_digest[32];
    int rc = -1;

    res->backend = "uudecode";
    if (!valid_uue_target_path(target_temp) ||
        lmux_zmx_shell_quote(target_temp, quoted, sizeof(quoted)) < 0) {
        res->status = LMUX_ZMX_SENDER_BAD_PATH;
        snprintf(res->detail, sizeof(res->detail), "bad target path");
        return -1;
    }
    if (lmux_zmx_probe_tool_state(probe, "sha256sum") != LMUX_ZMX_YES) {
        res->status = LMUX_ZMX_SENDER_UNSUPPORTED;
        snprintf(res->detail, sizeof(res->detail), "sha256sum unavailable");
        return -1;
    }
    if (lmux_zmx_probe_tool_state(probe, "stty") != LMUX_ZMX_YES) {
        res->status = LMUX_ZMX_SENDER_UNSUPPORTED;
        snprintf(res->detail, sizeof(res->detail), "stty unavailable");
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.local_path = local_path;
    ctx.target_path = target_temp;
    ctx.fd = open(local_path, O_RDONLY);
    if (ctx.fd < 0) {
        res->status = LMUX_ZMX_SENDER_LOCAL_IO;
        snprintf(res->detail, sizeof(res->detail), "open local failed");
        return -1;
    }
    sha256_init(&ctx.sha);
    if (lmux_zmx_session_stream_foreground(s, start_cmd, quoted,
                                           uudecode_payload,
                                           &ctx, &cr) < 0) {
        res->status = ctx.status ? ctx.status : map_session_error(s->last_error);
        if (res->status == LMUX_ZMX_SENDER_TIMEOUT)
            snprintf(res->detail, sizeof(res->detail), "timeout %s",
                     s->last_phase[0] ? s->last_phase : "transfer");
        else if (res->status == LMUX_ZMX_SENDER_SESSION_LOST)
            snprintf(res->detail, sizeof(res->detail), "session lost %s",
                     s->last_phase[0] ? s->last_phase : "transfer");
        if (res->status != LMUX_ZMX_SENDER_SESSION_LOST &&
            res->status != LMUX_ZMX_SENDER_TIMEOUT)
            cleanup_target(s, quoted, res);
        goto out;
    }
    if (ctx.status != LMUX_ZMX_SENDER_OK) {
        res->status = ctx.status;
        cleanup_target(s, quoted, res);
        goto out;
    }
    if (cr.exit_status != 0) {
        res->status = LMUX_ZMX_SENDER_REMOTE_FAILED;
        snprintf(res->detail, sizeof(res->detail), "decoder-status=%d",
                 cr.exit_status);
        cleanup_target(s, quoted, res);
        goto out;
    }
    snprintf(verify_cmd, sizeof(verify_cmd), "sha256sum %s 2>/dev/null", quoted);
    if (lmux_zmx_session_command(s, verify_cmd, &cr) < 0) {
        res->status = map_session_error(s->last_error);
        cleanup_target(s, quoted, res);
        goto out;
    }
    if (cr.exit_status != 0 || parse_sha256_output(cr.output, remote_digest) < 0) {
        res->status = LMUX_ZMX_SENDER_VERIFY_FAILED;
        snprintf(res->detail, sizeof(res->detail), "sha256sum failed");
        cleanup_target(s, quoted, res);
        goto out;
    }
    if (memcmp(remote_digest, ctx.digest, 32) != 0) {
        res->status = LMUX_ZMX_SENDER_VERIFY_FAILED;
        snprintf(res->detail, sizeof(res->detail), "sha256 mismatch");
        cleanup_target(s, quoted, res);
        goto out;
    }
    res->status = LMUX_ZMX_SENDER_OK;
    res->backend = "uudecode";
    res->bytes = ctx.bytes;
    memcpy(res->sha256, ctx.digest, 32);
    res->verified = 1;
    rc = 0;
out:
    if (ctx.fd >= 0)
        close(ctx.fd);
    return rc;
}
