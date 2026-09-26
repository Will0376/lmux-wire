#include "probe.h"

#include <stdio.h>
#include <string.h>

static const char *tool_names[LMUX_ZMX_TOOL_COUNT] = {
    "rz", "rx", "rb", "sz", "sx", "sb", "dd", "stty", "base64",
    "uudecode", "uuencode", "xxd", "printf", "cat", "chmod", "mv",
    "cp", "rm", "sync", "sha256sum", "mkdir", "kill"
};

const char *lmux_zmx_tri_name(enum lmux_zmx_tri v)
{
    switch (v) {
    case LMUX_ZMX_UNKNOWN: return "unknown";
    case LMUX_ZMX_NO: return "no";
    case LMUX_ZMX_YES: return "yes";
    }
    return "unknown";
}

void lmux_zmx_probe_result_init(struct lmux_zmx_probe_result *r)
{
    size_t i;
    memset(r, 0, sizeof(*r));
    r->status = LMUX_ZMX_OK;
    r->path_tmp = LMUX_ZMX_UNKNOWN;
    r->path_root = LMUX_ZMX_UNKNOWN;
    for (i = 0; i < LMUX_ZMX_TOOL_COUNT; i++) {
        r->tools[i].name = tool_names[i];
        r->tools[i].state = LMUX_ZMX_UNKNOWN;
    }
}

static void copy_first_line(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (!cap)
        return;
    while (src && src[n] && src[n] != '\n' && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static enum lmux_zmx_tri test_expr(struct lmux_zmx_session *s, const char *cmd)
{
    struct lmux_zmx_command_result res;
    if (lmux_zmx_session_command(s, cmd, &res) < 0)
        return LMUX_ZMX_UNKNOWN;
    return res.exit_status == 0 ? LMUX_ZMX_YES : LMUX_ZMX_NO;
}

static enum lmux_zmx_tri probe_tool(struct lmux_zmx_session *s,
                                    const char *name)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    return test_expr(s, cmd);
}

int lmux_zmx_probe_run(struct lmux_zmx_session *s,
                       struct lmux_zmx_probe_result *r)
{
    struct lmux_zmx_command_result res;
    size_t i;
    lmux_zmx_probe_result_init(r);
    if (lmux_zmx_session_establish(s) < 0) {
        r->status = s->last_error;
        r->auth_required = s->last_error == LMUX_ZMX_AUTH_REQUIRED;
        return -1;
    }
    r->shell_usable = 1;

    if (lmux_zmx_session_command(s, "uname -s 2>/dev/null", &res) == 0 &&
        res.exit_status == 0)
        copy_first_line(r->kernel, sizeof(r->kernel), res.output);
    if (lmux_zmx_session_command(s, "uname -m 2>/dev/null", &res) == 0 &&
        res.exit_status == 0)
        copy_first_line(r->machine, sizeof(r->machine), res.output);
    if (lmux_zmx_session_command(s, "pwd 2>/dev/null", &res) == 0 &&
        res.exit_status == 0)
        copy_first_line(r->cwd, sizeof(r->cwd), res.output);

    for (i = 0; i < LMUX_ZMX_TOOL_COUNT; i++)
        r->tools[i].state = probe_tool(s, r->tools[i].name);

    r->path_tmp = test_expr(s, "[ -d /tmp ]");
    r->path_root = test_expr(s, "[ -d /root ]");
    r->status = LMUX_ZMX_OK;
    return 0;
}
