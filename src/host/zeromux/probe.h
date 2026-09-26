#ifndef LMUX_ZEROMUX_PROBE_H
#define LMUX_ZEROMUX_PROBE_H

#include "session.h"

#define LMUX_ZMX_TOOL_COUNT 22

enum lmux_zmx_tri {
    LMUX_ZMX_UNKNOWN = 0,
    LMUX_ZMX_NO,
    LMUX_ZMX_YES
};

struct lmux_zmx_tool_result {
    const char *name;
    enum lmux_zmx_tri state;
};

struct lmux_zmx_probe_result {
    enum lmux_zmx_error status;
    int shell_usable;
    int auth_required;
    char kernel[128];
    char machine[128];
    char cwd[256];
    enum lmux_zmx_tri path_tmp;
    enum lmux_zmx_tri path_root;
    struct lmux_zmx_tool_result tools[LMUX_ZMX_TOOL_COUNT];
};

void lmux_zmx_probe_result_init(struct lmux_zmx_probe_result *r);
int lmux_zmx_probe_run(struct lmux_zmx_session *s,
                       struct lmux_zmx_probe_result *r);
const char *lmux_zmx_tri_name(enum lmux_zmx_tri v);

#endif
