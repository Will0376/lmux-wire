#ifndef LMUX_ZEROMUX_INSTALL_H
#define LMUX_ZEROMUX_INSTALL_H

#include "probe.h"
#include "sender.h"

#include <stdint.h>

enum lmux_zmx_install_status {
    LMUX_ZMX_INSTALL_OK = 0,
    LMUX_ZMX_INSTALL_UNSUPPORTED,
    LMUX_ZMX_INSTALL_BAD_PATH,
    LMUX_ZMX_INSTALL_LOCAL_IO,
    LMUX_ZMX_INSTALL_REMOTE_FAILED,
    LMUX_ZMX_INSTALL_TRANSFER_FAILED,
    LMUX_ZMX_INSTALL_VERIFY_FAILED,
    LMUX_ZMX_INSTALL_LAUNCH_FAILED,
    LMUX_ZMX_INSTALL_SESSION_LOST,
    LMUX_ZMX_INSTALL_TIMEOUT
};

struct lmux_zmx_install_result {
    enum lmux_zmx_install_status status;
    uint64_t local_size;
    uint8_t sha256[32];
    int already_installed;
    int uploaded;
    uint64_t uploaded_bytes;
    const char *sender_backend;
    int installed;
    int launched;
    unsigned long supervisor_pid;
    char final_path[320];
    char temp_path[320];
    char log_path[320];
    char detail[160];
};

const char *lmux_zmx_install_status_name(enum lmux_zmx_install_status st);
void lmux_zmx_install_result_init(struct lmux_zmx_install_result *r);
int lmux_zmx_install(struct lmux_zmx_session *s,
                     const struct lmux_zmx_probe_result *probe,
                     const char *local_path,
                     const char *install_dir,
                     const char *lmux_carrier,
                     struct lmux_zmx_install_result *res);

#endif
