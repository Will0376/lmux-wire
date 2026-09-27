#include "host.h"
#include "services.h"
#include "version.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *role_name(enum lmux2_role role)
{
    return role == LMUX2_ROLE_HOST ? "host" : "target";
}

static const char *conn_state_name(enum lmux2_conn_state st)
{
    switch (st) {
    case LMUX2_CONN_WAIT_HELLO: return "WAIT_HELLO";
    case LMUX2_CONN_ESTABLISHED: return "ESTABLISHED";
    case LMUX2_CONN_ERROR: return "ERROR";
    }
    return "?";
}

static const char *stream_state_name(enum lmux2_stream_state st)
{
    switch (st) {
    case LMUX2_STREAM_UNUSED: return "UNUSED";
    case LMUX2_STREAM_OPEN_SENT: return "OPEN_SENT";
    case LMUX2_STREAM_OPEN_RECEIVED: return "OPEN_RECEIVED";
    case LMUX2_STREAM_OPEN: return "OPEN";
    case LMUX2_STREAM_LOCAL_CLOSED: return "LOCAL_CLOSED";
    case LMUX2_STREAM_REMOTE_CLOSED: return "REMOTE_CLOSED";
    case LMUX2_STREAM_RESET: return "RESET";
    case LMUX2_STREAM_TERMINAL: return "TERMINAL";
    }
    return "?";
}

static const char *yesno(int v)
{
    return v ? "yes" : "no";
}

static void control_write_all(int fd, const char *buf, size_t len);
static void control_printf(struct v2_host *h, const char *fmt, ...);
static int exec_queue_record(struct v2_host *h, uint8_t type, uint8_t status,
                             uint64_t exec_id, const uint8_t *payload,
                             uint16_t len);
static void exec_finish_error(struct v2_host *h, const char *msg);

enum deploy_phase {
    DEPLOY_IDLE = 0,
    DEPLOY_UPLOADING,
    DEPLOY_ACTIVATING,
    DEPLOY_WAIT_FRESH_LMUX,
    DEPLOY_WAIT_RECONCILIATION,
    DEPLOY_VERIFY_TESTING,
    DEPLOY_CONFIRMING,
    DEPLOY_CONFIRM_AMBIGUOUS,
    DEPLOY_FINAL_VERIFY,
    DEPLOY_QUERY_STABLE,
    DEPLOY_INSTALL_SUPERVISOR,
    DEPLOY_RESTART_SUPERVISOR,
    DEPLOY_WAIT_SUPERVISOR_RECONNECT,
    DEPLOY_WAIT_RESTART_RECONCILIATION,
    DEPLOY_VERIFY_POST_RESTART,
    DEPLOY_CANCELING,
    DEPLOY_DONE
};

static const char *deploy_phase_name(uint32_t phase)
{
    switch (phase) {
    case DEPLOY_IDLE: return "IDLE";
    case DEPLOY_UPLOADING: return "UPLOADING";
    case DEPLOY_ACTIVATING: return "ACTIVATING";
    case DEPLOY_WAIT_FRESH_LMUX: return "WAIT_FRESH_LMUX";
    case DEPLOY_WAIT_RECONCILIATION: return "WAIT_RECONCILIATION";
    case DEPLOY_VERIFY_TESTING: return "VERIFY_TESTING";
    case DEPLOY_CONFIRMING: return "CONFIRMING";
    case DEPLOY_CONFIRM_AMBIGUOUS: return "CONFIRM_AMBIGUOUS";
    case DEPLOY_FINAL_VERIFY: return "FINAL_VERIFY";
    case DEPLOY_QUERY_STABLE: return "QUERY_STABLE";
    case DEPLOY_INSTALL_SUPERVISOR: return "INSTALL_SUPERVISOR";
    case DEPLOY_RESTART_SUPERVISOR: return "RESTART_SUPERVISOR";
    case DEPLOY_WAIT_SUPERVISOR_RECONNECT: return "WAIT_SUPERVISOR_RECONNECT";
    case DEPLOY_WAIT_RESTART_RECONCILIATION: return "WAIT_RESTART_RECONCILIATION";
    case DEPLOY_VERIFY_POST_RESTART: return "VERIFY_POST_RESTART";
    case DEPLOY_CANCELING: return "CANCELING";
    case DEPLOY_DONE: return "DONE";
    }
    return "?";
}

static int hex_value(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

static int parse_sha256_hex(const char *s, uint8_t out[32])
{
    size_t i;
    if (!s || strlen(s) != 64)
        return -1;
    for (i = 0; i < 32; i++) {
        int hi = hex_value((unsigned char)s[i * 2]);
        int lo = hex_value((unsigned char)s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void digest_to_hex(const uint8_t digest[32], char out[65])
{
    static const char hexdigits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 32; i++) {
        out[i * 2] = hexdigits[digest[i] >> 4];
        out[i * 2 + 1] = hexdigits[digest[i] & 15];
    }
    out[64] = 0;
}

static int parse_hex_blob(const char *s, uint8_t *out, size_t cap, size_t *len)
{
    size_t n, i;
    if (!s)
        return -1;
    n = strlen(s);
    if (strcmp(s, "-") == 0) {
        *len = 0;
        return 0;
    }
    if ((n & 1) || n / 2 > cap)
        return -1;
    for (i = 0; i < n / 2; i++) {
        int hi = hex_value((unsigned char)s[i * 2]);
        int lo = hex_value((unsigned char)s[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *len = n / 2;
    return 0;
}

static void control_write_hex_line(struct v2_host *h, const char *prefix,
                                   const uint8_t *buf, size_t len)
{
    static const char hexdigits[] = "0123456789abcdef";
    char out[256];
    size_t i, off = 0;
    control_printf(h, "%s", prefix);
    while (off < len) {
        size_t n = len - off;
        if (n > (sizeof(out) / 2))
            n = sizeof(out) / 2;
        for (i = 0; i < n; i++) {
            out[i * 2] = hexdigits[buf[off + i] >> 4];
            out[i * 2 + 1] = hexdigits[buf[off + i] & 15];
        }
        control_write_all(h->control_fd, out, n * 2);
        off += n;
    }
    control_printf(h, "\n");
}

static void control_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            (void)poll(&pfd, 1, 1000);
            continue;
        }
        return;
    }
}

static void control_printf(struct v2_host *h, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    if (h->control_fd < 0)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    control_write_all(h->control_fd, buf, (size_t)n);
}

static uint64_t next_boot_request_id(struct v2_host *h)
{
    uint64_t req_id = h->boot_next_request_id++;
    if (h->boot_next_request_id == 0)
        h->boot_next_request_id = 1;
    if (req_id == 0)
        req_id = h->boot_next_request_id++;
    return req_id;
}

static void clear_boot_pending(struct v2_host *h);
static void deploy_clear(struct v2_host *h);

#define EXEC_MODE_START_WAIT 1
#define EXEC_MODE_START_DETACH 2
#define EXEC_MODE_ATTACH_WAIT 3
#define EXEC_MODE_STATUS_ONCE 4
#define EXEC_MODE_SIGNAL_ONCE 5
#define EXEC_MODE_RELEASE_ONCE 6
#define EXEC_MODE_ATTACH_CONFLICT 7
#define EXEC_MODE_ATTACH_STDOUT_ONCE 8

static void exec_clear(struct v2_host *h)
{
    h->exec_active = 0;
    h->exec_phase = 0;
    h->exec_status_pending = 0;
    h->exec_close_pending = 0;
    h->exec_close_stdin_after_drain = 0;
    h->exec_done_seen = 0;
    h->exec_service_id = 0;
    h->exec_id = 0;
    h->exec_control_stream = 0;
    h->exec_stdin_stream = 0;
    h->exec_stdout_stream = 0;
    h->exec_stderr_stream = 0;
    h->exec_conflict_stream = 0;
    h->exec_ctl_in_len = 0;
    h->exec_ctl_out_len = h->exec_ctl_out_pos = 0;
    h->exec_next_status_ms = 0;
    h->exec_stdin_len = h->exec_stdin_pos = 0;
}

static void upload_clear(struct v2_host *h)
{
    h->boot_upload_active = 0;
    h->boot_upload_ready_sent = 0;
    h->boot_upload_abort_needed = 0;
    h->boot_upload_id = 0;
    h->boot_upload_declared_size = 0;
    h->boot_upload_sent = 0;
    h->boot_upload_max_chunk = 0;
    h->boot_upload_last_chunk = 0;
    memset(h->boot_upload_digest, 0, sizeof(h->boot_upload_digest));
}

static void deploy_clear(struct v2_host *h)
{
    h->deploy_active = 0;
    h->deploy_client_gone = 0;
    h->deploy_cancel_before_activate = 0;
    h->deploy_notified_link_down = 0;
    h->deploy_reported_reconciled = 0;
    h->deploy_restart_supervisor = 0;
    h->deploy_no_file_restart = 0;
    h->deploy_install_already = 0;
    h->deploy_phase = DEPLOY_IDLE;
    h->deploy_confirm_timeout_ms = 0;
    h->deploy_activation_id = 0;
    h->deploy_generation = 0;
    h->deploy_pre_local_conn = 0;
    h->deploy_pre_peer_conn = 0;
    h->deploy_pre_established_count = 0;
    h->deploy_deadline_ms = 0;
    memset(h->deploy_sha, 0, sizeof(h->deploy_sha));
}

static int queue_boot_request_body(struct v2_app *a, struct v2_host *h,
                                   uint16_t opcode, const uint8_t *body,
                                   uint32_t body_len)
{
    uint64_t req_id;
    if (h->boot_pending)
        return -1;
    req_id = next_boot_request_id(h);
    if (v2_runtime_queue_boot_request_body(a, opcode, req_id, body, body_len) < 0)
        return -1;
    h->boot_pending = 1;
    h->boot_opcode = opcode;
    h->boot_request_id = req_id;
    h->boot_deadline_ms = v2_now_ms() + 10000;
    return 0;
}

static void finish_boot_error(struct v2_host *h, const char *msg);

static void finish_deploy_error(struct v2_host *h, const char *msg)
{
    if (h->control_fd >= 0 && !h->boot_control_gone)
        control_printf(h, "ERR DEPLOY %s\n", msg);
    v2_host_close_fd(&h->control_fd);
    clear_boot_pending(h);
    upload_clear(h);
    deploy_clear(h);
}

static void finish_deploy_success(struct v2_host *h, const char *msg)
{
    if (h->control_fd >= 0 && !h->boot_control_gone) {
        control_printf(h, "OK DEPLOY\n");
        if (msg && *msg)
            control_printf(h, "%s\n", msg);
    }
    v2_host_close_fd(&h->control_fd);
    clear_boot_pending(h);
    upload_clear(h);
    deploy_clear(h);
}

static int queue_activate_uploaded(struct v2_app *a, struct v2_host *h)
{
    uint8_t body[64];
    uint32_t len = 0;
    if (lmux_boot_encode_activate(body, sizeof(body), &len,
                                  h->deploy_sha,
                                  h->deploy_confirm_timeout_ms) < 0)
        return -1;
    return queue_boot_request_body(a, h, LMUX_BOOT_ACTIVATE_CANDIDATE,
                                   body, len);
}

static int queue_update_status(struct v2_app *a, struct v2_host *h)
{
    return queue_boot_request_body(a, h, LMUX_BOOT_UPDATE_STATUS, NULL, 0);
}

static int queue_confirm_deploy(struct v2_app *a, struct v2_host *h)
{
    uint8_t body[16];
    uint32_t len = 0;
    if (lmux_boot_encode_activation_id(body, sizeof(body), &len,
                                       h->deploy_activation_id) < 0)
        return -1;
    return queue_boot_request_body(a, h, LMUX_BOOT_CONFIRM_CANDIDATE,
                                   body, len);
}

static int queue_supervisor_install(struct v2_app *a, struct v2_host *h)
{
    uint8_t body[40];
    uint32_t len = 0;
    if (lmux_boot_encode_sha256(body, sizeof(body), &len, h->deploy_sha) < 0)
        return -1;
    return queue_boot_request_body(a, h, LMUX_BOOT_SUPERVISOR_INSTALL,
                                   body, len);
}

static int queue_supervisor_restart(struct v2_app *a, struct v2_host *h)
{
    uint8_t body[40];
    uint32_t len = 0;
    if (lmux_boot_encode_sha256(body, sizeof(body), &len, h->deploy_sha) < 0)
        return -1;
    v2_runtime_lmux_marker(a, &h->deploy_pre_local_conn,
                           &h->deploy_pre_peer_conn,
                           &h->deploy_pre_established_count);
    return queue_boot_request_body(a, h, LMUX_BOOT_SUPERVISOR_RESTART,
                                   body, len);
}

static void deploy_start_supervisor_install(struct v2_app *a,
                                            struct v2_host *h)
{
    char sha[65];
    if (!v2_runtime_boot_supervisor_supported(a)) {
        finish_deploy_error(h, "supervisor-unavailable");
        return;
    }
    digest_to_hex(h->deploy_sha, sha);
    if (h->control_fd >= 0 && !h->boot_control_gone)
        control_printf(h, "installing supervisor artifact sha256=%s\n", sha);
    h->deploy_phase = DEPLOY_INSTALL_SUPERVISOR;
    if (queue_supervisor_install(a, h) < 0)
        finish_deploy_error(h, "queue");
}

static int queue_rollback_deploy(struct v2_app *a, struct v2_host *h)
{
    uint8_t body[16];
    uint32_t len = 0;
    if (lmux_boot_encode_activation_id(body, sizeof(body), &len,
                                       h->deploy_activation_id) < 0)
        return -1;
    return queue_boot_request_body(a, h, LMUX_BOOT_ROLLBACK, body, len);
}

static int desired_reconciled(const struct v2_host *h)
{
    int i;
    if (!h->desired_sent)
        return 0;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        const struct v2_desired *d = &h->desired[i];
        if (d->used && (!d->sent || d->service_id == 0))
            return 0;
    }
    return 1;
}

static int status_fresh_lmux_ready(const struct v2_host *h,
                                   const struct v2_host_status *st)
{
    if (!st || st->conn->state != LMUX2_CONN_ESTABLISHED ||
        st->conn->local_conn_id == 0 || st->conn->peer_conn_id == 0)
        return 0;
    if (st->connection_established_count <= h->deploy_pre_established_count &&
        st->conn->local_conn_id == h->deploy_pre_local_conn &&
        st->conn->peer_conn_id == h->deploy_pre_peer_conn)
        return 0;
    return 1;
}

static int deploy_after_activation(const struct v2_host *h)
{
    return h->deploy_phase >= DEPLOY_WAIT_FRESH_LMUX;
}

static void upload_fail(struct v2_host *h, const char *msg)
{
    if (h->deploy_active) {
        finish_deploy_error(h, msg);
        return;
    }
    upload_clear(h);
    finish_boot_error(h, msg);
}

static int upload_queue_abort(struct v2_app *a, struct v2_host *h)
{
    uint8_t body[16];
    uint32_t len = 0;
    if (!h->boot_upload_id || !v2_runtime_boot_upload_supported(a))
        return -1;
    if (lmux_boot_encode_upload_abort(body, sizeof(body), &len,
                                      h->boot_upload_id) < 0)
        return -1;
    return queue_boot_request_body(a, h, LMUX_BOOT_UPLOAD_ABORT, body, len);
}

static int upload_queue_end(struct v2_app *a, struct v2_host *h)
{
    uint8_t body[64];
    uint32_t len = 0;
    if (!h->boot_upload_active || h->boot_upload_id == 0)
        return -1;
    sha256_final(&h->boot_upload_sha, h->boot_upload_digest);
    if (lmux_boot_encode_upload_end(body, sizeof(body), &len,
                                    h->boot_upload_id,
                                    h->boot_upload_digest) < 0)
        return -1;
    return queue_boot_request_body(a, h, LMUX_BOOT_UPLOAD_END, body, len);
}

static int upload_queue_data(struct v2_app *a, struct v2_host *h,
                             const uint8_t *buf, uint32_t len)
{
    uint8_t body[16 + LMUX_BOOT_UPLOAD_MAX_CHUNK];
    uint32_t body_len = 0;
    if (len > LMUX_BOOT_UPLOAD_MAX_CHUNK ||
        lmux_boot_encode_upload_data(body, sizeof(body), &body_len,
                                     h->boot_upload_id, h->boot_upload_sent,
                                     buf, len) < 0)
        return -1;
    sha256_update(&h->boot_upload_sha, buf, len);
    h->boot_upload_last_chunk = len;
    h->boot_upload_sent += len;
    return queue_boot_request_body(a, h, LMUX_BOOT_UPLOAD_DATA, body, body_len);
}

int v2_host_control_start(struct v2_host *h)
{
    if (!h->control_path)
        return 0;
    h->control_listen_fd = v2_host_create_listener(h->control_path, &(int){0});
    return h->control_listen_fd < 0 ? -1 : 0;
}

void v2_host_control_accept(struct v2_host *h)
{
    int fd = accept(h->control_listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    (void)v2_host_set_nonblock(fd);
    if (h->boot_pending || h->boot_upload_active || h->deploy_active ||
        h->exec_active) {
        (void)write(fd, "ERR busy\n", 9);
        close(fd);
        return;
    }
    v2_host_close_fd(&h->control_fd);
    h->control_fd = fd;
    h->control_in_len = 0;
}

static void write_host_services(struct v2_host *h)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        const struct v2_host_service *s = &h->services[i];
        if (!s->used)
            continue;
        control_printf(h, "  %s id=%llu type=%s listener=%s path=%s pending_fd=%s pending_event=%llu active_stream=%llu last_stream=%llu",
                       s->name, (unsigned long long)s->id,
                       v2_service_type_name(s->type),
                       s->listen_fd >= 0 ? "ready" : "none",
                       s->internal ? "-" : (s->listen_path[0] ? s->listen_path :
                       (s->host_path[0] ? s->host_path :
                        (s->target_path[0] ? s->target_path : "-"))),
                       s->pending_fd >= 0 ? "yes" : "no",
                       (unsigned long long)s->pending_event_token,
                       (unsigned long long)s->active_stream_id,
                       (unsigned long long)s->last_stream_id);
        if (s->internal)
            control_printf(h, " internal=yes");
        if (s->type == V2_SERVICE_PTY)
            control_printf(h, " target_child=not-reported");
        control_printf(h, "\n");
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            const struct v2_host_binding *b = &s->bindings[j];
            if (b->fd >= 0)
                control_printf(h, "    binding stream=%llu fd=%d\n",
                               (unsigned long long)b->stream_id, b->fd);
        }
    }
}

static void write_target_services(struct v2_host *h,
                                  const struct v2_target_service_status *services)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        const struct v2_target_service_status *s = &services[i];
        if (!s->used)
            continue;
        control_printf(h, "  %s id=%llu type=%s listener=%s path=%s pending=%s pending_token=%llu pending_event=%llu active_stream=%llu last_stream=%llu",
                       s->name, (unsigned long long)s->id,
                       v2_service_type_name(s->type),
                       s->listener_ready ? "ready" : "none",
                       s->path[0] ? s->path : "-",
                       s->pending ? "yes" : "no",
                       (unsigned long long)s->pending_token,
                       (unsigned long long)s->pending_event_token,
                       (unsigned long long)s->active_stream_id,
                       (unsigned long long)s->last_stream_id);
        if (s->type == V2_SERVICE_PTY)
            control_printf(h, " pty_master=%s child_pid=%ld",
                           s->pty_master_ready ? "ready" : "none",
                           s->child_pid);
        if (s->type == V2_SERVICE_FILE)
            control_printf(h, " file_active=%s", s->file_active ? "yes" : "no");
        control_printf(h, "\n");
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            const struct v2_target_binding_status *b = &s->bindings[j];
            if (b->used)
                control_printf(h, "    binding stream=%llu\n",
                               (unsigned long long)b->stream_id);
        }
    }
}

void v2_host_status_write(struct v2_app *a, struct v2_host *h,
                          const struct v2_host_status *st)
{
    int i;
    (void)a;
    control_printf(h, "LMUX Wire\n");
    control_printf(h, "local:  %s\n", LMUX_BUILD_VERSION);
    control_printf(h, "wire:   LMUX/2\n");
    control_printf(h, "role:   %s\n", role_name(st->role));
    control_printf(h, "carrier: %s fd=%d opens=%llu lost=%llu open_fail=%llu rx_bytes=%llu tx_bytes=%llu\n",
                   lmux_carrier_fd(st->carrier) >= 0 ? "open" : "down",
                   lmux_carrier_fd(st->carrier),
                   st->carrier->open_count, st->carrier->lost_count,
                   st->carrier->open_fail_count, st->carrier->rx_bytes,
                   st->carrier->tx_bytes);
    if (st->outer) {
        control_printf(h, "outer: %s local_conn=%llu peer_conn=%llu tx_seq=%llu rx_expected_seq=%llu tx_frames=%llu rx_frames=%llu crc_errors=%llu seq_errors=%llu resync_drops=%llu protocol_errors=%llu tx_payload_max=%u transport_tx_bytes=%llu transport_rx_bytes=%llu last_error=%s\n",
                       lmux_link_state_name(st->outer->state),
                       (unsigned long long)st->outer->local_conn_id,
                       (unsigned long long)st->outer->peer_conn_id,
                       (unsigned long long)st->outer->next_tx_seq,
                       (unsigned long long)st->outer->expected_rx_seq,
                       st->outer->tx_frames, st->outer->rx_frames,
                       st->outer->crc_errors, st->outer->seq_errors,
                       st->outer->resync_drops, st->outer->protocol_errors,
                       st->outer->effective_transport_payload,
                       st->outer->transport_tx_bytes,
                       st->outer->transport_rx_bytes,
                       st->outer->last_error[0] ? st->outer->last_error : "-");
        control_printf(h, "outer_boot: caps=%s%s%s%s%s%s%s tx_frames=%llu rx_frames=%llu tx_bytes=%llu rx_bytes=%llu txq=%u rxq=%u pending_control=%s\n",
                       (st->outer->negotiated_caps & LMUX_LINK_CAP_BOOT_INFO) ? "BOOT_INFO" : "-",
                       (st->outer->negotiated_caps & LMUX_LINK_CAP_BOOT_UPLOAD) ? "," : "",
                       (st->outer->negotiated_caps & LMUX_LINK_CAP_BOOT_UPLOAD) ? "BOOT_UPLOAD" : "",
                       (st->outer->negotiated_caps & LMUX_LINK_CAP_BOOT_MANAGED_WORKER) ? "," : "",
                       (st->outer->negotiated_caps & LMUX_LINK_CAP_BOOT_MANAGED_WORKER) ? "BOOT_MANAGED" : "",
                       (st->outer->negotiated_caps & LMUX_LINK_CAP_BOOT_SUPERVISOR_CONTROL) ? "," : "",
                       (st->outer->negotiated_caps & LMUX_LINK_CAP_BOOT_SUPERVISOR_CONTROL) ? "BOOT_SUPERVISOR" : "",
                       st->outer->boot_tx_frames, st->outer->boot_rx_frames,
                       st->outer->boot_tx_bytes, st->outer->boot_rx_bytes,
                       lmux_link_boot_tx_len(st->outer),
                       lmux_link_boot_rx_len(st->outer),
                       yesno(h->boot_pending));
    } else {
        control_printf(h, "outer: disabled\n");
    }
    if (h->deploy_active) {
        char sha[65];
        digest_to_hex(h->deploy_sha, sha);
        control_printf(h, "deploy: phase=%s activation=%llu generation=%llu sha256=%s\n",
                       deploy_phase_name(h->deploy_phase),
                       (unsigned long long)h->deploy_activation_id,
                       (unsigned long long)h->deploy_generation, sha);
        control_printf(h, "deploy_client: %s\n",
                       h->control_fd >= 0 && !h->boot_control_gone ?
                       "attached" : "detached");
    }
    control_printf(h, "connection: %s local_conn=%llu peer_conn=%llu established=%llu recreations=%llu peer_restarts=%llu last_error=%s\n",
                   conn_state_name(st->conn->state),
                   (unsigned long long)st->conn->local_conn_id,
                   (unsigned long long)st->conn->peer_conn_id,
                   st->connection_established_count,
                   st->connection_recreated_count,
                   st->peer_restart_count,
                   st->conn->last_error[0] ? st->conn->last_error : "-");
    control_printf(h, "frames: tx=%llu rx=%llu protocol_errors=%llu stream_errors=%llu stale=%llu pre_established_drops=%llu\n",
                   st->conn->frames_tx, st->conn->frames_rx,
                   st->conn->protocol_errors, st->conn->stream_errors,
                   st->conn->stale_frames, st->conn->pre_established_drops);
    control_printf(h, "stream_counters: opens=%llu closes=%llu resets=%llu\n",
                   st->stream_open_count, st->stream_close_count,
                   st->stream_reset_count);
    control_printf(h, "flow: conn_tx=%llu peer_max_data=%llu conn_rx=%llu conn_consumed=%llu local_max_data=%llu last_max_data_sent=%llu dirty=%s outq=%zu\n",
                   (unsigned long long)st->conn->conn_send_offset,
                   (unsigned long long)st->conn->peer_max_data,
                   (unsigned long long)st->conn->conn_recv_offset,
                   (unsigned long long)st->conn->conn_rx_consumed,
                   (unsigned long long)st->conn->local_max_data,
                   (unsigned long long)st->conn->last_sent_max_data,
                   yesno(st->conn->max_data_dirty), st->conn->outq_len);
    if (st->role == LMUX2_ROLE_TARGET) {
        control_printf(h, "factories: unix-rendezvous pty file exec\n");
    } else {
        control_printf(h, "factories: unix-rendezvous=%s pty=%s file=%s exec=%s desired_sent=%s\n",
                       yesno(h->factory_unix), yesno(h->factory_pty),
                       yesno(h->factory_file), yesno(h->factory_exec),
                       yesno(h->desired_sent));
    }
    control_printf(h, "desired_services:\n");
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        const struct v2_desired *d = &h->desired[i];
        if (!d->used)
            continue;
        control_printf(h, "  %s type=%s host=%s target=%s configured=yes sent=%s acknowledged=%s req=%u service_id=%llu\n",
                       d->name, v2_service_type_name(d->type), d->host_path,
                       d->target_path[0] ? d->target_path : "-",
                       yesno(d->sent), yesno(d->service_id != 0), d->req_id,
                       (unsigned long long)d->service_id);
    }
    control_printf(h, "services:\n");
    if (st->role == LMUX2_ROLE_HOST)
        write_host_services(h);
    else
        write_target_services(h, st->target_services);
    control_printf(h, "streams:\n");
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++) {
        const struct lmux2_stream *ls = &st->conn->streams[i];
        if (ls->state == LMUX2_STREAM_UNUSED)
            continue;
        control_printf(h, "  id=%llu state=%s service=%llu tx=%llu rx_expect=%llu rx_len=%zu tx_len=%zu peer_limit=%llu local_limit=%llu consumed=%llu last_limit_sent=%llu limit_dirty=%s\n",
                       (unsigned long long)ls->id, stream_state_name(ls->state),
                       (unsigned long long)ls->service_id,
                       (unsigned long long)ls->send_offset,
                       (unsigned long long)ls->expected_recv_offset,
                       ls->rx.len, ls->tx.len,
                       (unsigned long long)ls->peer_max_stream_data,
                       (unsigned long long)ls->local_max_stream_data,
                       (unsigned long long)ls->recv_consumed,
                       (unsigned long long)ls->last_sent_max_stream_data,
                       yesno(ls->max_stream_data_dirty));
    }
}

static void control_handle_line(struct v2_app *a, struct v2_host *h,
                                const struct v2_host_status *st, char *line)
{
    char cmd[16], sub[32], name[V2_NAME_MAX], service[V2_NAME_MAX];
    unsigned rows, cols, xp = 0, yp = 0;
    struct v2_host_service *s;
    if (sscanf(line, "%15s", cmd) != 1)
        return;
    if (strcmp(cmd, "STATUS") == 0) {
        v2_host_status_write(a, h, st);
        v2_host_close_fd(&h->control_fd);
    } else if (strcmp(cmd, "WINSIZE") == 0) {
        if (sscanf(line, "%*s %31s %u %u %u %u", name, &rows, &cols, &xp, &yp) < 3) {
            (void)write(h->control_fd, "ERR bad WINSIZE\n", 16);
            return;
        }
        if (strcmp(name, "1") == 0)
            strcpy(service, "console1");
        else if (strcmp(name, "2") == 0)
            strcpy(service, "console2");
        else
            strcpy(service, name);
        s = v2_host_service_find_name(h->services, service);
        if (!s || s->type != V2_SERVICE_PTY ||
            v2_runtime_send_winsize_control(a, s->id, rows, cols, xp, yp) < 0)
            (void)write(h->control_fd, "ERR WINSIZE unavailable\n", 23);
        else
            (void)write(h->control_fd, "OK\n", 3);
        v2_host_close_fd(&h->control_fd);
    } else if (strcmp(cmd, "DEPLOY") == 0) {
        unsigned long long size;
        unsigned timeout_ms;
        unsigned restart_supervisor = 0;
        uint8_t body[16];
        uint32_t len = 0;
        int parsed = sscanf(line, "%*s %llu %u %u", &size, &timeout_ms,
                            &restart_supervisor);
        if (parsed < 2) {
            (void)write(h->control_fd, "ERR bad DEPLOY\n", 15);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (parsed >= 3 && restart_supervisor > 1) {
            (void)write(h->control_fd, "ERR bad DEPLOY\n", 15);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (h->boot_pending || h->boot_upload_active || h->deploy_active) {
            (void)write(h->control_fd, "ERR busy\n", 9);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (!v2_runtime_boot_upload_supported(a) ||
            !v2_runtime_boot_managed_supported(a) ||
            !v2_runtime_boot_supervisor_supported(a)) {
            (void)write(h->control_fd, "ERR BOOT managed-unavailable\n", 29);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (lmux_boot_encode_upload_begin(body, sizeof(body), &len,
                                          (uint64_t)size) < 0 ||
            queue_boot_request_body(a, h, LMUX_BOOT_UPLOAD_BEGIN,
                                    body, len) < 0) {
            (void)write(h->control_fd, "ERR BOOT queue\n", 15);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        h->boot_control_gone = 0;
        h->boot_upload_active = 1;
        h->boot_upload_declared_size = (uint64_t)size;
        h->boot_upload_sent = 0;
        sha256_init(&h->boot_upload_sha);
        h->deploy_active = 1;
        h->deploy_restart_supervisor = restart_supervisor != 0;
        h->deploy_no_file_restart = 0;
        h->deploy_phase = DEPLOY_UPLOADING;
        h->deploy_confirm_timeout_ms = timeout_ms;
        h->deploy_deadline_ms = v2_now_ms() + (long long)timeout_ms + 60000;
        h->deploy_pre_local_conn = 0;
        h->deploy_pre_peer_conn = 0;
        h->deploy_pre_established_count = 0;
        memset(h->deploy_sha, 0, sizeof(h->deploy_sha));
        return;
    } else if (strcmp(cmd, "DEPLOY_RESTART") == 0) {
        if (h->boot_pending || h->boot_upload_active || h->deploy_active) {
            (void)write(h->control_fd, "ERR busy\n", 9);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (!v2_runtime_boot_managed_supported(a) ||
            !v2_runtime_boot_supervisor_supported(a)) {
            (void)write(h->control_fd, "ERR BOOT supervisor-unavailable\n", 32);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        h->boot_control_gone = 0;
        h->deploy_active = 1;
        h->deploy_restart_supervisor = 1;
        h->deploy_no_file_restart = 1;
        h->deploy_phase = DEPLOY_QUERY_STABLE;
        h->deploy_deadline_ms = v2_now_ms() + 120000;
        if (queue_update_status(a, h) < 0) {
            finish_deploy_error(h, "queue");
            return;
        }
        return;
    } else if (strcmp(cmd, "EXEC") == 0) {
        char *save = NULL;
        char *tok = strtok_r(line, " \t\r\n", &save);
        char *subcmd = strtok_r(NULL, " \t\r\n", &save);
        char *cwd_hex;
        uint8_t payload[V2_EXEC_CTL_BUF_MAX - 12];
        uint8_t cwd[V2_PATH_MAX];
        uint8_t args[V2_EXEC_ARG_BYTES_MAX];
        uint16_t arg_lens[V2_EXEC_ARG_MAX];
        size_t cwd_len = 0, args_len = 0, off, n;
        uint8_t argc = 0;
        unsigned long long exec_id_ull;
        (void)tok;
        if (!subcmd) {
            (void)write(h->control_fd, "ERR bad EXEC\n", 13);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (h->boot_pending || h->boot_upload_active || h->deploy_active ||
            h->exec_active) {
            (void)write(h->control_fd, "ERR busy\n", 9);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (strcmp(subcmd, "ATTACH") == 0 ||
            strcmp(subcmd, "ATTACH_CONFLICT") == 0 ||
            strcmp(subcmd, "ATTACH_STDOUT_ONCE") == 0) {
            char *id_s = strtok_r(NULL, " \t\r\n", &save);
            char *opt;
            if (!id_s || sscanf(id_s, "%llu", &exec_id_ull) != 1 ||
                exec_id_ull == 0) {
                (void)write(h->control_fd, "ERR bad EXEC id\n", 16);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            exec_clear(h);
            h->exec_active = 1;
            if (strcmp(subcmd, "ATTACH_CONFLICT") == 0)
                h->exec_phase = EXEC_MODE_ATTACH_CONFLICT;
            else if (strcmp(subcmd, "ATTACH_STDOUT_ONCE") == 0)
                h->exec_phase = EXEC_MODE_ATTACH_STDOUT_ONCE;
            else
                h->exec_phase = EXEC_MODE_ATTACH_WAIT;
            h->exec_id = (uint64_t)exec_id_ull;
            while ((opt = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
                if (h->exec_phase == EXEC_MODE_ATTACH_CONFLICT ||
                    h->exec_phase == EXEC_MODE_ATTACH_STDOUT_ONCE) {
                    (void)write(h->control_fd, "ERR bad EXEC attach\n", 20);
                    v2_host_close_fd(&h->control_fd);
                    exec_clear(h);
                    return;
                }
                if (strcmp(opt, "--stdin") == 0) {
                    char *stdin_hex = strtok_r(NULL, " \t\r\n", &save);
                    if (!stdin_hex ||
                        parse_hex_blob(stdin_hex, h->exec_stdin,
                                       sizeof(h->exec_stdin),
                                       &h->exec_stdin_len) < 0) {
                        (void)write(h->control_fd, "ERR bad EXEC stdin\n", 19);
                        v2_host_close_fd(&h->control_fd);
                        exec_clear(h);
                        return;
                    }
                    h->exec_close_stdin_after_drain = 1;
                } else if (strcmp(opt, "--close-stdin") == 0) {
                    h->exec_close_stdin_after_drain = 1;
                } else {
                    (void)write(h->control_fd, "ERR bad EXEC attach\n", 20);
                    v2_host_close_fd(&h->control_fd);
                    exec_clear(h);
                    return;
                }
            }
            return;
        }
        if (strcmp(subcmd, "STATUS") == 0 ||
            strcmp(subcmd, "SIGNAL") == 0 ||
            strcmp(subcmd, "RELEASE") == 0) {
            char *id_s = strtok_r(NULL, " \t\r\n", &save);
            uint8_t sig_payload[1];
            uint8_t rec = V2_EXEC_REC_STATUS;
            uint16_t len = 0;
            if (!id_s || sscanf(id_s, "%llu", &exec_id_ull) != 1 ||
                exec_id_ull == 0) {
                (void)write(h->control_fd, "ERR bad EXEC id\n", 16);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            exec_clear(h);
            h->exec_active = 1;
            h->exec_id = (uint64_t)exec_id_ull;
            h->exec_phase = EXEC_MODE_STATUS_ONCE;
            if (strcmp(subcmd, "SIGNAL") == 0) {
                char *sig = strtok_r(NULL, " \t\r\n", &save);
                if (!sig) {
                    (void)write(h->control_fd, "ERR bad EXEC signal\n", 20);
                    v2_host_close_fd(&h->control_fd);
                    exec_clear(h);
                    return;
                }
                if (strcmp(sig, "INT") == 0)
                    sig_payload[0] = V2_EXEC_SIG_INT;
                else if (strcmp(sig, "TERM") == 0)
                    sig_payload[0] = V2_EXEC_SIG_TERM;
                else if (strcmp(sig, "KILL") == 0)
                    sig_payload[0] = V2_EXEC_SIG_KILL;
                else if (strcmp(sig, "HUP") == 0)
                    sig_payload[0] = V2_EXEC_SIG_HUP;
                else {
                    (void)write(h->control_fd, "ERR bad EXEC signal\n", 20);
                    v2_host_close_fd(&h->control_fd);
                    exec_clear(h);
                    return;
                }
                rec = V2_EXEC_REC_SIGNAL;
                len = 1;
                h->exec_phase = EXEC_MODE_SIGNAL_ONCE;
            } else if (strcmp(subcmd, "RELEASE") == 0) {
                rec = V2_EXEC_REC_RELEASE;
                h->exec_phase = EXEC_MODE_RELEASE_ONCE;
            }
            if (strtok_r(NULL, " \t\r\n", &save) != NULL ||
                exec_queue_record(h, rec, 0, h->exec_id,
                                  len ? sig_payload : NULL, len) < 0) {
                (void)write(h->control_fd, "ERR bad EXEC\n", 13);
                v2_host_close_fd(&h->control_fd);
                exec_clear(h);
            }
            return;
        }
        cwd_hex = strtok_r(NULL, " \t\r\n", &save);
        if ((strcmp(subcmd, "START") != 0 &&
             strcmp(subcmd, "START_DETACHED") != 0) || !cwd_hex) {
            (void)write(h->control_fd, "ERR bad EXEC\n", 13);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (parse_hex_blob(cwd_hex, cwd, sizeof(cwd), &cwd_len) < 0) {
            (void)write(h->control_fd, "ERR bad EXEC cwd\n", 17);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        exec_clear(h);
        memset(h->exec_stdin, 0, sizeof(h->exec_stdin));
        h->exec_stdin_len = h->exec_stdin_pos = 0;
        while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
            if (strcmp(tok, "--stdin") == 0) {
                char *stdin_hex = strtok_r(NULL, " \t\r\n", &save);
                if (!stdin_hex ||
                    parse_hex_blob(stdin_hex, h->exec_stdin,
                                   sizeof(h->exec_stdin),
                                   &h->exec_stdin_len) < 0 ||
                    strtok_r(NULL, " \t\r\n", &save) != NULL) {
                    (void)write(h->control_fd, "ERR bad EXEC stdin\n", 19);
                    v2_host_close_fd(&h->control_fd);
                    return;
                }
                break;
            }
            if (argc >= V2_EXEC_ARG_MAX ||
                parse_hex_blob(tok, args + args_len,
                               sizeof(args) - args_len, &n) < 0 ||
                n == 0 || n > UINT16_MAX) {
                (void)write(h->control_fd, "ERR bad EXEC argv\n", 18);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            arg_lens[argc++] = (uint16_t)n;
            args_len += n;
        }
        if (argc == 0 || cwd_len > UINT16_MAX) {
            (void)write(h->control_fd, "ERR bad EXEC argv\n", 18);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        off = 0;
        lmux_put_le16(payload + off, (uint16_t)cwd_len);
        off += 2;
        payload[off++] = argc;
        payload[off++] = 0;
        if (off + (size_t)argc * 2 + cwd_len + args_len > sizeof(payload)) {
            (void)write(h->control_fd, "ERR EXEC too-large\n", 19);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        for (n = 0; n < argc; n++) {
            lmux_put_le16(payload + off, arg_lens[n]);
            off += 2;
        }
        if (cwd_len) {
            memcpy(payload + off, cwd, cwd_len);
            off += cwd_len;
        }
        memcpy(payload + off, args, args_len);
        off += args_len;
        h->exec_active = 1;
        h->exec_phase = strcmp(subcmd, "START_DETACHED") == 0 ?
            EXEC_MODE_START_DETACH : EXEC_MODE_START_WAIT;
        h->exec_close_stdin_after_drain = h->exec_phase == EXEC_MODE_START_WAIT;
        if (exec_queue_record(h, V2_EXEC_REC_START, 0, 0,
                              payload, (uint16_t)off) < 0) {
            exec_finish_error(h, "queue");
            return;
        }
        return;
    } else if (strcmp(cmd, "BOOT") == 0) {
        uint16_t opcode;
        uint64_t req_id;
        if (sscanf(line, "%*s %31s", sub) != 1) {
            (void)write(h->control_fd, "ERR bad BOOT\n", 13);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (strcmp(sub, "GET_INFO") == 0)
            opcode = LMUX_BOOT_GET_INFO;
        else if (strcmp(sub, "WORKER_STATUS") == 0)
            opcode = LMUX_BOOT_WORKER_STATUS;
        else if (strcmp(sub, "UPDATE_STATUS") == 0)
            opcode = LMUX_BOOT_UPDATE_STATUS;
        else if (strcmp(sub, "UPLOAD") == 0) {
            unsigned long long size;
            uint8_t body[16];
            uint32_t len = 0;
            if (sscanf(line, "%*s %*s %llu", &size) != 1) {
                (void)write(h->control_fd, "ERR bad BOOT UPLOAD\n", 20);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (h->boot_pending || h->boot_upload_active || h->deploy_active) {
                (void)write(h->control_fd, "ERR busy\n", 9);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (!v2_runtime_boot_upload_supported(a)) {
                (void)write(h->control_fd, "ERR BOOT upload-unavailable\n", 28);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (lmux_boot_encode_upload_begin(body, sizeof(body), &len,
                                              (uint64_t)size) < 0 ||
                queue_boot_request_body(a, h, LMUX_BOOT_UPLOAD_BEGIN,
                                        body, len) < 0) {
                (void)write(h->control_fd, "ERR BOOT queue\n", 15);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            h->boot_control_gone = 0;
            h->boot_upload_active = 1;
            h->boot_upload_declared_size = (uint64_t)size;
            h->boot_upload_sent = 0;
            sha256_init(&h->boot_upload_sha);
            return;
        }
        else if (strcmp(sub, "ACTIVATE") == 0) {
            char hex[65];
            unsigned timeout_ms;
            uint8_t sha[32], body[64];
            uint32_t len = 0;
            if (sscanf(line, "%*s %*s %64s %u", hex, &timeout_ms) != 2 ||
                parse_sha256_hex(hex, sha) < 0 ||
                lmux_boot_encode_activate(body, sizeof(body), &len,
                                          sha, timeout_ms) < 0) {
                (void)write(h->control_fd, "ERR bad BOOT ACTIVATE\n", 22);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (h->boot_pending || h->boot_upload_active || h->deploy_active) {
                (void)write(h->control_fd, "ERR busy\n", 9);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (!v2_runtime_boot_managed_supported(a)) {
                (void)write(h->control_fd, "ERR BOOT managed-unavailable\n", 29);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (queue_boot_request_body(a, h, LMUX_BOOT_ACTIVATE_CANDIDATE,
                                        body, len) < 0) {
                (void)write(h->control_fd, "ERR BOOT queue\n", 15);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            h->boot_control_gone = 0;
            return;
        }
        else if (strcmp(sub, "CONFIRM") == 0 || strcmp(sub, "ROLLBACK") == 0) {
            unsigned long long id;
            uint8_t body[16];
            uint32_t len = 0;
            opcode = strcmp(sub, "CONFIRM") == 0 ?
                LMUX_BOOT_CONFIRM_CANDIDATE : LMUX_BOOT_ROLLBACK;
            if (sscanf(line, "%*s %*s %llu", &id) != 1 ||
                lmux_boot_encode_activation_id(body, sizeof(body), &len,
                                               (uint64_t)id) < 0) {
                (void)write(h->control_fd, "ERR bad BOOT activation-id\n", 27);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (h->boot_pending || h->boot_upload_active || h->deploy_active) {
                (void)write(h->control_fd, "ERR busy\n", 9);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (!v2_runtime_boot_managed_supported(a)) {
                (void)write(h->control_fd, "ERR BOOT managed-unavailable\n", 29);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (queue_boot_request_body(a, h, opcode, body, len) < 0) {
                (void)write(h->control_fd, "ERR BOOT queue\n", 15);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            h->boot_control_gone = 0;
            return;
        }
        else if (strcmp(sub, "SUPERVISOR_INSTALL") == 0 ||
                 strcmp(sub, "SUPERVISOR_RESTART") == 0) {
            char hex[65];
            uint8_t sha[32], body[40];
            uint32_t len = 0;
            opcode = strcmp(sub, "SUPERVISOR_INSTALL") == 0 ?
                LMUX_BOOT_SUPERVISOR_INSTALL : LMUX_BOOT_SUPERVISOR_RESTART;
            if (sscanf(line, "%*s %*s %64s", hex) != 1 ||
                parse_sha256_hex(hex, sha) < 0 ||
                lmux_boot_encode_sha256(body, sizeof(body), &len, sha) < 0) {
                (void)write(h->control_fd, "ERR bad BOOT supervisor-sha\n", 28);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (h->boot_pending || h->boot_upload_active || h->deploy_active) {
                (void)write(h->control_fd, "ERR busy\n", 9);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (!v2_runtime_boot_supervisor_supported(a)) {
                (void)write(h->control_fd, "ERR BOOT supervisor-unavailable\n", 32);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            if (queue_boot_request_body(a, h, opcode, body, len) < 0) {
                (void)write(h->control_fd, "ERR BOOT queue\n", 15);
                v2_host_close_fd(&h->control_fd);
                return;
            }
            h->boot_control_gone = 0;
            return;
        }
        else {
            (void)write(h->control_fd, "ERR unknown BOOT command\n", 25);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if (h->boot_pending || h->boot_upload_active || h->deploy_active) {
            (void)write(h->control_fd, "ERR busy\n", 9);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        if ((opcode == LMUX_BOOT_UPDATE_STATUS &&
             !v2_runtime_boot_managed_supported(a)) ||
            (opcode != LMUX_BOOT_UPDATE_STATUS &&
             !v2_runtime_boot_supported(a))) {
            (void)write(h->control_fd, "ERR BOOT unavailable\n", 21);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        req_id = next_boot_request_id(h);
        if (v2_runtime_queue_boot_request(a, opcode, req_id) < 0) {
            (void)write(h->control_fd, "ERR BOOT queue\n", 15);
            v2_host_close_fd(&h->control_fd);
            return;
        }
        h->boot_pending = 1;
        h->boot_control_gone = 0;
        h->boot_opcode = opcode;
        h->boot_request_id = req_id;
        h->boot_deadline_ms = v2_now_ms() + 5000;
    } else {
        (void)write(h->control_fd, "ERR unknown command\n", 20);
        v2_host_close_fd(&h->control_fd);
    }
}

void v2_host_control_on_hup(struct v2_host *h)
{
    if (h->boot_upload_active) {
        v2_host_close_fd(&h->control_fd);
        h->boot_control_gone = 1;
        h->control_in_len = 0;
        h->boot_upload_abort_needed = 1;
        return;
    }
    if (h->deploy_active) {
        v2_host_close_fd(&h->control_fd);
        h->boot_control_gone = 1;
        h->deploy_client_gone = 1;
        h->control_in_len = 0;
        if (h->deploy_phase == DEPLOY_ACTIVATING)
            h->deploy_cancel_before_activate = 1;
        else if (!deploy_after_activation(h))
            deploy_clear(h);
        return;
    }
    if (h->boot_pending && h->control_fd >= 0) {
        v2_host_close_fd(&h->control_fd);
        h->boot_control_gone = 1;
        h->control_in_len = 0;
        return;
    }
    if (h->exec_active) {
        v2_host_close_fd(&h->control_fd);
        h->control_in_len = 0;
        exec_clear(h);
        return;
    }
    v2_host_close_fd(&h->control_fd);
}

static void clear_boot_pending(struct v2_host *h)
{
    h->boot_pending = 0;
    h->boot_control_gone = 0;
    h->boot_opcode = 0;
    h->boot_request_id = 0;
    h->boot_deadline_ms = 0;
}

static void finish_boot_error(struct v2_host *h, const char *msg)
{
    if (h->control_fd >= 0 && !h->boot_control_gone)
        control_printf(h, "ERR BOOT %s\n", msg);
    v2_host_close_fd(&h->control_fd);
    clear_boot_pending(h);
    upload_clear(h);
}

void v2_host_control_outer_down(struct v2_host *h)
{
    if (h->deploy_active) {
        if (h->deploy_phase == DEPLOY_ACTIVATING &&
            h->deploy_cancel_before_activate) {
            h->deploy_phase = DEPLOY_CANCELING;
            h->deploy_deadline_ms = 0;
            clear_boot_pending(h);
            return;
        }
        if (deploy_after_activation(h)) {
            if (h->control_fd >= 0 && !h->boot_control_gone &&
                !h->deploy_notified_link_down) {
                if (h->deploy_phase == DEPLOY_RESTART_SUPERVISOR ||
                    h->deploy_phase == DEPLOY_WAIT_SUPERVISOR_RECONNECT ||
                    (h->boot_pending &&
                     h->boot_opcode == LMUX_BOOT_SUPERVISOR_RESTART))
                    control_printf(h, "supervisor link lost as expected; waiting for restart\n");
                else
                    control_printf(h, "link lost; candidate remains in TESTING, waiting for reconnect\n");
                h->deploy_notified_link_down = 1;
            }
            if (h->boot_pending &&
                h->boot_opcode == LMUX_BOOT_CONFIRM_CANDIDATE)
                h->deploy_phase = DEPLOY_CONFIRM_AMBIGUOUS;
            else if (h->boot_pending &&
                     h->boot_opcode == LMUX_BOOT_SUPERVISOR_RESTART)
                h->deploy_phase = DEPLOY_WAIT_SUPERVISOR_RECONNECT;
            h->deploy_deadline_ms = 0;
            clear_boot_pending(h);
            return;
        }
        finish_deploy_error(h, "link-down");
        return;
    }
    if (h->exec_active) {
        if (h->control_fd >= 0)
            control_printf(h, "ERR EXEC link-down\n");
        v2_host_close_fd(&h->control_fd);
        exec_clear(h);
    }
    if (h->boot_pending)
        finish_boot_error(h, "link-down");
    else if (h->boot_upload_active) {
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "ERR BOOT link-down\n");
        v2_host_close_fd(&h->control_fd);
        upload_clear(h);
        deploy_clear(h);
    }
}

void v2_host_control_check_timeout(struct v2_host *h)
{
    if (h->boot_pending && h->boot_deadline_ms > 0 &&
        v2_now_ms() >= h->boot_deadline_ms) {
        if (h->deploy_active)
            finish_deploy_error(h, "timeout");
        else
            finish_boot_error(h, "timeout");
    }
}

static int exec_queue_record(struct v2_host *h, uint8_t type, uint8_t status,
                             uint64_t exec_id, const uint8_t *payload,
                             uint16_t len)
{
    if (12u + len > sizeof(h->exec_ctl_out) ||
        h->exec_ctl_out_len != h->exec_ctl_out_pos)
        return -1;
    h->exec_ctl_out[0] = type;
    h->exec_ctl_out[1] = status;
    lmux_put_le16(h->exec_ctl_out + 2, len);
    lmux_put_le64(h->exec_ctl_out + 4, exec_id);
    if (len)
        memcpy(h->exec_ctl_out + 12, payload, len);
    h->exec_ctl_out_len = 12u + len;
    h->exec_ctl_out_pos = 0;
    return 0;
}

static int exec_parse_record(struct v2_host *h, struct v2_exec_record *r)
{
    if (h->exec_ctl_in_len < 12)
        return 0;
    r->type = h->exec_ctl_in[0];
    r->status = h->exec_ctl_in[1];
    r->length = lmux_get_le16(h->exec_ctl_in + 2);
    r->exec_id = lmux_get_le64(h->exec_ctl_in + 4);
    if (r->length > sizeof(h->exec_ctl_in) - 12)
        return -1;
    if (h->exec_ctl_in_len < 12u + r->length)
        return 0;
    r->payload = h->exec_ctl_in + 12;
    return 1;
}

static void exec_consume_record(struct v2_host *h, uint16_t len)
{
    size_t n = 12u + len;
    if (n < h->exec_ctl_in_len)
        memmove(h->exec_ctl_in, h->exec_ctl_in + n, h->exec_ctl_in_len - n);
    h->exec_ctl_in_len -= n;
}

static void exec_finish_error(struct v2_host *h, const char *msg)
{
    if (h->control_fd >= 0)
        control_printf(h, "ERR EXEC %s\n", msg);
    v2_host_close_fd(&h->control_fd);
    exec_clear(h);
}

static void exec_finish_done(struct v2_host *h)
{
    if (h->control_fd >= 0)
        control_printf(h, "DONE\n");
    v2_host_close_fd(&h->control_fd);
    exec_clear(h);
}

static void exec_close_lmux_streams(struct v2_app *a, struct v2_host *h)
{
    if (h->exec_control_stream)
        (void)v2_runtime_close_stream(a, h->exec_control_stream);
    if (h->exec_stdin_stream)
        (void)v2_runtime_close_stream(a, h->exec_stdin_stream);
    if (h->exec_stdout_stream)
        (void)v2_runtime_close_stream(a, h->exec_stdout_stream);
    if (h->exec_stderr_stream)
        (void)v2_runtime_close_stream(a, h->exec_stderr_stream);
}

static void exec_finish_done_remote(struct v2_app *a, struct v2_host *h)
{
    exec_close_lmux_streams(a, h);
    exec_finish_done(h);
}

static void exec_finish_simple(struct v2_app *a, struct v2_host *h)
{
    exec_close_lmux_streams(a, h);
    v2_host_close_fd(&h->control_fd);
    exec_clear(h);
}

static int exec_status_done(uint8_t st)
{
    return st == V2_EXEC_STATUS_DONE || st == V2_EXEC_STATUS_START_FAILED;
}

static int exec_mode_has_attachment(const struct v2_host *h)
{
    return h->exec_phase == EXEC_MODE_START_WAIT ||
        h->exec_phase == EXEC_MODE_ATTACH_WAIT ||
        h->exec_phase == EXEC_MODE_ATTACH_CONFLICT ||
        h->exec_phase == EXEC_MODE_ATTACH_STDOUT_ONCE;
}

static int exec_output_stream_drained(struct v2_app *a, uint64_t stream_id)
{
    int st;
    if (!stream_id)
        return 0;
    st = v2_runtime_stream_state(a, stream_id);
    return st == LMUX2_STREAM_REMOTE_CLOSED ||
        st == LMUX2_STREAM_TERMINAL ||
        st == LMUX2_STREAM_RESET ||
        st == LMUX2_STREAM_UNUSED;
}

static int exec_attachment_drained(struct v2_app *a, const struct v2_host *h)
{
    if (!exec_mode_has_attachment(h) ||
        h->exec_phase == EXEC_MODE_ATTACH_CONFLICT)
        return 1;
    if (h->exec_phase == EXEC_MODE_ATTACH_STDOUT_ONCE)
        return exec_output_stream_drained(a, h->exec_stdout_stream);
    return exec_output_stream_drained(a, h->exec_stdout_stream) &&
        exec_output_stream_drained(a, h->exec_stderr_stream);
}

static void exec_handle_record(struct v2_app *a, struct v2_host *h,
                               const struct v2_exec_record *r)
{
    if (r->type == V2_EXEC_REC_ERROR) {
        control_printf(h, "ERR EXEC remote status=%u exec_id=%llu\n",
                       r->status, (unsigned long long)r->exec_id);
        exec_close_lmux_streams(a, h);
        v2_host_close_fd(&h->control_fd);
        exec_clear(h);
        return;
    }
    if ((r->type == V2_EXEC_REC_START_REPLY ||
         r->type == V2_EXEC_REC_STATUS_REPLY) && r->length >= 16) {
        uint8_t state = r->payload[0];
        uint8_t fail = r->payload[1];
        uint16_t errnum = lmux_get_le16(r->payload + 2);
        uint32_t exit_code = lmux_get_le32(r->payload + 4);
        uint32_t term_signal = lmux_get_le32(r->payload + 8);
        h->exec_status_pending = 0;
        if (r->type == V2_EXEC_REC_START_REPLY) {
            h->exec_id = r->exec_id;
            control_printf(h, "OK EXEC exec_id=%llu stdin_len=%llu\n",
                           (unsigned long long)h->exec_id,
                           (unsigned long long)h->exec_stdin_len);
            if (state == V2_EXEC_STATUS_START_FAILED) {
                control_printf(h, "STATUS state=START_FAILED fail=%u errno=%u exit=%u signal=%u\n",
                               fail, errnum, exit_code, term_signal);
                exec_finish_done_remote(a, h);
                return;
            }
            if (h->exec_phase == EXEC_MODE_START_DETACH) {
                control_printf(h, "DETACHED\n");
                exec_finish_simple(a, h);
                return;
            }
        }
        control_printf(h, "STATUS state=%u fail=%u errno=%u exit=%u signal=%u stdin_closed=%u stdout_eof=%u stderr_eof=%u reaped=%u",
                       state, fail, errnum, exit_code, term_signal,
                       r->payload[12], r->payload[13], r->payload[14],
                       r->payload[15]);
        if (r->length >= 64) {
            control_printf(h, " stdout_child=%llu stdout_lmux=%llu stdout_pending=%llu stderr_child=%llu stderr_lmux=%llu stderr_pending=%llu",
                           (unsigned long long)lmux_get_le64(r->payload + 16),
                           (unsigned long long)lmux_get_le64(r->payload + 24),
                           (unsigned long long)lmux_get_le64(r->payload + 32),
                           (unsigned long long)lmux_get_le64(r->payload + 40),
                           (unsigned long long)lmux_get_le64(r->payload + 48),
                           (unsigned long long)lmux_get_le64(r->payload + 56));
        }
        control_printf(h, "\n");
        if (h->exec_phase == EXEC_MODE_STATUS_ONCE ||
            h->exec_phase == EXEC_MODE_SIGNAL_ONCE) {
            exec_finish_simple(a, h);
            return;
        }
        if (exec_status_done(state)) {
            if (exec_mode_has_attachment(h) &&
                h->exec_phase != EXEC_MODE_ATTACH_CONFLICT) {
                h->exec_done_seen = 1;
                if (!exec_attachment_drained(a, h))
                    return;
            }
            exec_finish_done_remote(a, h);
            return;
        }
    } else if (r->type == V2_EXEC_REC_STATUS_REPLY && r->length == 0 &&
               h->exec_phase == EXEC_MODE_RELEASE_ONCE) {
        h->exec_status_pending = 0;
        control_printf(h, "OK EXEC RELEASED exec_id=%llu\n",
                       (unsigned long long)r->exec_id);
        exec_finish_simple(a, h);
        return;
    }
}

static void exec_drain_control_rx(struct v2_app *a, struct v2_host *h)
{
    while (h->exec_control_stream &&
           h->exec_ctl_in_len < sizeof(h->exec_ctl_in)) {
        size_t n = v2_runtime_stream_read(a, h->exec_control_stream,
                                          h->exec_ctl_in + h->exec_ctl_in_len,
                                          sizeof(h->exec_ctl_in) - h->exec_ctl_in_len);
        if (!n)
            break;
        h->exec_ctl_in_len += n;
    }
    for (;;) {
        struct v2_exec_record r;
        int rc = exec_parse_record(h, &r);
        if (rc < 0) {
            exec_finish_error(h, "bad-record");
            return;
        }
        if (rc == 0)
            return;
        exec_handle_record(a, h, &r);
        if (!h->exec_active)
            return;
        exec_consume_record(h, r.length);
    }
}

static void exec_pump_control_tx(struct v2_app *a, struct v2_host *h)
{
    if (!h->exec_control_stream ||
        !v2_runtime_stream_is_open(a, h->exec_control_stream))
        return;
    while (h->exec_ctl_out_pos < h->exec_ctl_out_len &&
           v2_runtime_stream_tx_len(a, h->exec_control_stream) < LMUX2_STREAM_TX_CAP) {
        size_t n = h->exec_ctl_out_len - h->exec_ctl_out_pos;
        size_t w = v2_runtime_stream_write(a, h->exec_control_stream,
                                           h->exec_ctl_out + h->exec_ctl_out_pos, n);
        if (!w)
            break;
        h->exec_ctl_out_pos += w;
    }
    if (h->exec_ctl_out_pos == h->exec_ctl_out_len)
        h->exec_ctl_out_pos = h->exec_ctl_out_len = 0;
}

static size_t exec_read_output_stream(struct v2_app *a, struct v2_host *h,
                                      uint64_t stream_id, const char *prefix)
{
    uint8_t buf[256];
    size_t n, total = 0;
    while (stream_id && (n = v2_runtime_stream_read(a, stream_id,
                                                   buf, sizeof(buf))) > 0) {
        control_write_hex_line(h, prefix, buf, n);
        total += n;
    }
    return total;
}

static void exec_pump_stdin(struct v2_app *a, struct v2_host *h)
{
    if (!h->exec_stdin_stream ||
        !v2_runtime_stream_is_open(a, h->exec_stdin_stream))
        return;
    while (h->exec_stdin_pos < h->exec_stdin_len &&
           v2_runtime_stream_tx_len(a, h->exec_stdin_stream) < LMUX2_STREAM_TX_CAP) {
        size_t n = h->exec_stdin_len - h->exec_stdin_pos;
        size_t w = v2_runtime_stream_write(a, h->exec_stdin_stream,
                                           h->exec_stdin + h->exec_stdin_pos, n);
        if (!w)
            break;
        h->exec_stdin_pos += w;
    }
}

static struct v2_host_service *exec_host_service(struct v2_host *h)
{
    struct v2_host_service *s = v2_host_service_find_name(h->services, "__exec");
    if (!s)
        s = v2_host_service_find_name(h->services, "exec");
    if (!s || s->type != V2_SERVICE_EXEC || !s->id)
        return NULL;
    return s;
}

static void exec_tick(struct v2_app *a, struct v2_host *h,
                      const struct v2_host_status *st)
{
    struct v2_host_service *s;
    (void)st;
    if (!h->exec_active)
        return;
    if (!v2_runtime_lmux_established(a)) {
        exec_finish_error(h, "lmux-down");
        return;
    }
    s = exec_host_service(h);
    if (!s) {
        if (v2_host_ensure_private_exec(a, h) < 0)
            exec_finish_error(h, "service-unavailable");
        return;
    }
    h->exec_service_id = s->id;
    if (!h->exec_control_stream) {
        if (v2_runtime_open_stream_token(a, h->exec_service_id, 0,
                                         &h->exec_control_stream) < 0) {
            exec_finish_error(h, "open-control");
            return;
        }
        return;
    }
    exec_pump_control_tx(a, h);
    if (h->exec_id && exec_mode_has_attachment(h)) {
        if (exec_read_output_stream(a, h, h->exec_stdout_stream,
                                    "STDOUT_HEX ") > 0 &&
            h->exec_phase == EXEC_MODE_ATTACH_STDOUT_ONCE) {
            exec_finish_simple(a, h);
            return;
        }
        if (h->exec_phase != EXEC_MODE_ATTACH_STDOUT_ONCE)
            exec_read_output_stream(a, h, h->exec_stderr_stream, "STDERR_HEX ");
        exec_pump_stdin(a, h);
    }
    exec_drain_control_rx(a, h);
    if (!h->exec_active)
        return;
    if (!v2_runtime_stream_is_open(a, h->exec_control_stream))
        return;
    if (h->exec_id && exec_mode_has_attachment(h) && !h->exec_stdout_stream) {
        (void)v2_runtime_open_stream_token(a, h->exec_service_id,
                                           v2_exec_stream_token(h->exec_id, V2_EXEC_ROLE_STDOUT),
                                           &h->exec_stdout_stream);
    }
    if (h->exec_id && h->exec_phase != EXEC_MODE_ATTACH_CONFLICT &&
        h->exec_phase != EXEC_MODE_ATTACH_STDOUT_ONCE &&
        exec_mode_has_attachment(h) && !h->exec_stderr_stream) {
        (void)v2_runtime_open_stream_token(a, h->exec_service_id,
                                           v2_exec_stream_token(h->exec_id, V2_EXEC_ROLE_STDERR),
                                           &h->exec_stderr_stream);
    }
    if (h->exec_id && exec_mode_has_attachment(h) &&
        h->exec_close_stdin_after_drain &&
        !h->exec_stdin_stream && !h->exec_close_pending) {
        (void)v2_runtime_open_stream_token(a, h->exec_service_id,
                                           v2_exec_stream_token(h->exec_id, V2_EXEC_ROLE_STDIN),
                                           &h->exec_stdin_stream);
    }
    if (exec_mode_has_attachment(h)) {
        if (exec_read_output_stream(a, h, h->exec_stdout_stream,
                                    "STDOUT_HEX ") > 0 &&
            h->exec_phase == EXEC_MODE_ATTACH_STDOUT_ONCE) {
            exec_finish_simple(a, h);
            return;
        }
        if (h->exec_phase != EXEC_MODE_ATTACH_CONFLICT &&
            h->exec_phase != EXEC_MODE_ATTACH_STDOUT_ONCE)
            exec_read_output_stream(a, h, h->exec_stderr_stream, "STDERR_HEX ");
        exec_pump_stdin(a, h);
    }
    if (h->exec_done_seen && exec_attachment_drained(a, h)) {
        exec_finish_done_remote(a, h);
        return;
    }
    if (h->exec_phase == EXEC_MODE_ATTACH_CONFLICT && h->exec_stdout_stream &&
        v2_runtime_stream_is_open(a, h->exec_stdout_stream)) {
        int st;
        if (!h->exec_conflict_stream) {
            (void)v2_runtime_open_stream_token(a, h->exec_service_id,
                                               v2_exec_stream_token(h->exec_id,
                                                                    V2_EXEC_ROLE_STDOUT),
                                               &h->exec_conflict_stream);
            return;
        }
        st = v2_runtime_stream_state(a, h->exec_conflict_stream);
        if (st == LMUX2_STREAM_OPEN || st == LMUX2_STREAM_REMOTE_CLOSED) {
            exec_finish_error(h, "conflict-opened");
            return;
        }
        if (st == LMUX2_STREAM_TERMINAL || st == LMUX2_STREAM_RESET ||
            st == LMUX2_STREAM_UNUSED) {
            if (h->control_fd >= 0)
                control_printf(h, "OK EXEC ATTACH_BUSY exec_id=%llu\n",
                               (unsigned long long)h->exec_id);
            exec_finish_simple(a, h);
            return;
        }
    }
    if (h->exec_id && exec_mode_has_attachment(h) &&
        h->exec_close_stdin_after_drain && h->exec_stdin_stream &&
        h->exec_stdin_pos == h->exec_stdin_len &&
        v2_runtime_stream_tx_len(a, h->exec_stdin_stream) == 0 &&
        !h->exec_close_pending &&
        h->exec_ctl_out_len == h->exec_ctl_out_pos) {
        if (exec_queue_record(h, V2_EXEC_REC_CLOSE_STDIN, 0,
                              h->exec_id, NULL, 0) == 0) {
            h->exec_close_pending = 1;
            h->exec_status_pending = 1;
        }
    }
    if (h->exec_id && !h->exec_done_seen && !h->exec_status_pending &&
        v2_now_ms() >= h->exec_next_status_ms &&
        h->exec_ctl_out_len == h->exec_ctl_out_pos) {
        if (exec_queue_record(h, V2_EXEC_REC_STATUS, 0,
                              h->exec_id, NULL, 0) == 0) {
            h->exec_status_pending = 1;
            h->exec_next_status_ms = v2_now_ms() + 100;
        }
    }
}

short v2_host_control_poll_events(const struct v2_host *h)
{
    if (h->control_fd < 0)
        return 0;
    if (h->exec_active)
        return 0;
    if (h->boot_upload_active && h->boot_pending)
        return 0;
    return POLLIN;
}

void v2_host_control_tick(struct v2_app *a, struct v2_host *h,
                          const struct v2_host_status *st)
{
    if (h->boot_upload_active && h->boot_upload_abort_needed &&
        !h->boot_pending) {
        if (upload_queue_abort(a, h) < 0)
            upload_clear(h);
    } else if (h->boot_upload_active && h->boot_upload_ready_sent &&
               !h->boot_pending &&
               h->boot_upload_sent == h->boot_upload_declared_size) {
        if (upload_queue_end(a, h) < 0)
            upload_fail(h, "queue");
    }
    exec_tick(a, h, st);
    if (!h->deploy_active || h->boot_pending || h->boot_upload_active)
        return;
    if (h->deploy_deadline_ms > 0 && v2_now_ms() > h->deploy_deadline_ms) {
        finish_deploy_error(h, "timeout");
        return;
    }
    if (h->deploy_phase == DEPLOY_CANCELING) {
        if (!v2_runtime_boot_managed_supported(a))
            return;
        if (h->deploy_activation_id != 0) {
            if (queue_rollback_deploy(a, h) < 0)
                deploy_clear(h);
        } else {
            if (queue_update_status(a, h) < 0)
                deploy_clear(h);
        }
        return;
    }
    if (h->deploy_phase == DEPLOY_WAIT_FRESH_LMUX) {
        if (!status_fresh_lmux_ready(h, st))
            return;
        h->deploy_phase = DEPLOY_WAIT_RECONCILIATION;
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "fresh LMUX established\n");
    }
    if (h->deploy_phase == DEPLOY_WAIT_RECONCILIATION) {
        if (!desired_reconciled(h))
            return;
        h->deploy_phase = DEPLOY_VERIFY_TESTING;
        if (!h->deploy_reported_reconciled &&
            h->control_fd >= 0 && !h->boot_control_gone) {
            control_printf(h, "services reconciled\n");
            h->deploy_reported_reconciled = 1;
        }
        if (queue_update_status(a, h) < 0) {
            finish_deploy_error(h, "queue");
            return;
        }
        return;
    }
    if (h->deploy_phase == DEPLOY_WAIT_SUPERVISOR_RECONNECT) {
        if (!status_fresh_lmux_ready(h, st))
            return;
        h->deploy_phase = DEPLOY_WAIT_RESTART_RECONCILIATION;
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "fresh supervisor LMUX established\n");
    }
    if (h->deploy_phase == DEPLOY_WAIT_RESTART_RECONCILIATION) {
        if (!desired_reconciled(h))
            return;
        h->deploy_phase = DEPLOY_VERIFY_POST_RESTART;
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "post-restart services reconciled\n");
        if (queue_update_status(a, h) < 0) {
            finish_deploy_error(h, "queue");
            return;
        }
        return;
    }
    if (h->deploy_phase == DEPLOY_VERIFY_TESTING ||
        h->deploy_phase == DEPLOY_CONFIRM_AMBIGUOUS ||
        h->deploy_phase == DEPLOY_FINAL_VERIFY ||
        h->deploy_phase == DEPLOY_QUERY_STABLE ||
        h->deploy_phase == DEPLOY_VERIFY_POST_RESTART) {
        if (!v2_runtime_boot_managed_supported(a))
            return;
        if (queue_update_status(a, h) < 0) {
            finish_deploy_error(h, "queue");
            return;
        }
    }
}

void v2_host_control_on_boot_response(struct v2_app *a, struct v2_host *h,
                                      uint16_t opcode,
                                      uint16_t status, uint64_t request_id,
                                      const uint8_t *body, uint32_t body_len)
{
    if (!h->boot_pending || h->boot_request_id != request_id ||
        h->boot_opcode != opcode)
        return;
    if (status != LMUX_BOOT_STATUS_OK) {
        if (h->deploy_active) {
            if (h->deploy_phase == DEPLOY_CANCELING) {
                deploy_clear(h);
                return;
            }
            finish_deploy_error(h, lmux_boot_status_name(status));
            return;
        }
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "ERR BOOT %s\n", lmux_boot_status_name(status));
        v2_host_close_fd(&h->control_fd);
        clear_boot_pending(h);
        upload_clear(h);
        return;
    }
    if (h->boot_upload_active && opcode == LMUX_BOOT_UPLOAD_BEGIN) {
        struct lmux_boot_upload_begin_response resp;
        clear_boot_pending(h);
        if (lmux_boot_decode_upload_begin_response(body, body_len, &resp) < 0 ||
            resp.max_chunk == 0) {
            upload_fail(h, "malformed");
            return;
        }
        h->boot_upload_id = resp.upload_id;
        h->boot_upload_max_chunk = resp.max_chunk;
        if (h->boot_upload_max_chunk > LMUX_BOOT_UPLOAD_MAX_CHUNK)
            h->boot_upload_max_chunk = LMUX_BOOT_UPLOAD_MAX_CHUNK;
        if (h->boot_upload_abort_needed) {
            (void)upload_queue_abort(a, h);
            return;
        }
        if (h->control_fd >= 0 && !h->boot_control_gone) {
            control_printf(h, "READY max_chunk=%u\n", h->boot_upload_max_chunk);
            h->boot_upload_ready_sent = 1;
        }
        return;
    }
    if (h->boot_upload_active && opcode == LMUX_BOOT_UPLOAD_DATA) {
        struct lmux_boot_upload_data_response resp;
        clear_boot_pending(h);
        if (lmux_boot_decode_upload_data_response(body, body_len, &resp) < 0 ||
            resp.next_offset != h->boot_upload_sent) {
            upload_fail(h, "offset");
            return;
        }
        if (h->boot_upload_abort_needed) {
            (void)upload_queue_abort(a, h);
            return;
        }
        if (h->boot_upload_sent == h->boot_upload_declared_size) {
            if (upload_queue_end(a, h) < 0)
                upload_fail(h, "queue");
        }
        return;
    }
    if (h->boot_upload_active && opcode == LMUX_BOOT_UPLOAD_END) {
        struct lmux_boot_upload_end_response resp;
        char hex[65];
        clear_boot_pending(h);
        if (lmux_boot_decode_upload_end_response(body, body_len, &resp) < 0 ||
            resp.size != h->boot_upload_declared_size ||
            memcmp(resp.sha256, h->boot_upload_digest,
                   sizeof(h->boot_upload_digest)) != 0) {
            upload_fail(h, "verify");
            return;
        }
        if (h->deploy_active) {
            memcpy(h->deploy_sha, resp.sha256, sizeof(h->deploy_sha));
            sha256_hex(resp.sha256, hex);
            if (h->control_fd >= 0 && !h->boot_control_gone) {
                control_printf(h, "uploaded sha256=%s\n", hex);
                control_printf(h, "activation starting\n");
            }
            upload_clear(h);
            h->deploy_phase = DEPLOY_ACTIVATING;
            v2_runtime_lmux_marker(a, &h->deploy_pre_local_conn,
                                   &h->deploy_pre_peer_conn,
                                   &h->deploy_pre_established_count);
            if (queue_activate_uploaded(a, h) < 0) {
                finish_deploy_error(h, "queue");
                return;
            }
            return;
        }
        if (h->control_fd >= 0 && !h->boot_control_gone) {
            sha256_hex(resp.sha256, hex);
            control_printf(h, "OK UPLOAD\n");
            control_printf(h, "uploaded: %llu bytes\n",
                           (unsigned long long)resp.size);
            control_printf(h, "sha256=%s\n", hex);
            control_printf(h, "artifact verified and stored\n");
        }
        v2_host_close_fd(&h->control_fd);
        upload_clear(h);
        return;
    }
    if (h->boot_upload_active && opcode == LMUX_BOOT_UPLOAD_ABORT) {
        clear_boot_pending(h);
        v2_host_close_fd(&h->control_fd);
        upload_clear(h);
        deploy_clear(h);
        return;
    }
    if (h->deploy_active && opcode == LMUX_BOOT_ACTIVATE_CANDIDATE) {
        struct lmux_boot_activate_response ar;
        char sha[65];
        clear_boot_pending(h);
        if (lmux_boot_decode_activate_response(body, body_len, &ar) < 0) {
            finish_deploy_error(h, "malformed");
            return;
        }
        h->deploy_activation_id = ar.activation_id;
        h->deploy_generation = ar.generation;
        h->deploy_confirm_timeout_ms = ar.confirm_timeout_ms;
        h->deploy_deadline_ms = 0;
        digest_to_hex(h->deploy_sha, sha);
        if (ar.phase == LMUX_BOOT_UPDATE_STABLE) {
            if (h->control_fd >= 0 && !h->boot_control_gone)
                control_printf(h, "artifact already active sha256=%s\n", sha);
            deploy_start_supervisor_install(a, h);
            return;
        }
        if (ar.phase != LMUX_BOOT_UPDATE_TESTING || ar.activation_id == 0) {
            finish_deploy_error(h, "unexpected-activate-state");
            return;
        }
        if (h->deploy_cancel_before_activate) {
            h->deploy_phase = DEPLOY_CANCELING;
            if (queue_rollback_deploy(a, h) < 0)
                deploy_clear(h);
            return;
        }
        if (h->control_fd >= 0 && !h->boot_control_gone) {
            control_printf(h, "activation started id=%llu generation=%llu\n",
                           (unsigned long long)ar.activation_id,
                           (unsigned long long)ar.generation);
            control_printf(h, "waiting for fresh LMUX...\n");
        }
        h->deploy_phase = DEPLOY_WAIT_FRESH_LMUX;
        h->deploy_notified_link_down = 0;
        return;
    }
    if (h->deploy_active && opcode == LMUX_BOOT_UPDATE_STATUS) {
        struct lmux_boot_update_status us;
        char confirmed[65], candidate[65], running[65], uploaded[65];
        clear_boot_pending(h);
        if (lmux_boot_decode_update_status(body, body_len, &us) < 0) {
            finish_deploy_error(h, "malformed");
            return;
        }
        if (h->deploy_phase == DEPLOY_CANCELING) {
            if (us.phase == LMUX_BOOT_UPDATE_TESTING &&
                us.activation_id != 0 &&
                memcmp(us.candidate_sha256, h->deploy_sha, 32) == 0) {
                h->deploy_activation_id = us.activation_id;
                h->deploy_generation = us.generation;
                if (queue_rollback_deploy(a, h) < 0)
                    deploy_clear(h);
            } else {
                deploy_clear(h);
            }
            return;
        }
        digest_to_hex(us.confirmed_sha256, confirmed);
        digest_to_hex(us.candidate_sha256, candidate);
        digest_to_hex(us.running_sha256, running);
        digest_to_hex(h->deploy_sha, uploaded);
        if (h->deploy_phase == DEPLOY_QUERY_STABLE) {
            uint8_t zero[32];
            memset(zero, 0, sizeof(zero));
            if (us.phase != LMUX_BOOT_UPDATE_STABLE ||
                memcmp(us.confirmed_sha256, zero, 32) == 0 ||
                memcmp(us.running_sha256, us.confirmed_sha256, 32) != 0) {
                finish_deploy_error(h, "not-stable-confirmed-running");
                return;
            }
            memcpy(h->deploy_sha, us.confirmed_sha256, 32);
            digest_to_hex(h->deploy_sha, uploaded);
            if (h->control_fd >= 0 && !h->boot_control_gone)
                control_printf(h, "confirmed running sha256=%s\n", uploaded);
            deploy_start_supervisor_install(a, h);
            return;
        }
        if (h->deploy_phase == DEPLOY_VERIFY_POST_RESTART) {
            if (us.phase == LMUX_BOOT_UPDATE_STABLE &&
                memcmp(us.confirmed_sha256, h->deploy_sha, 32) == 0 &&
                memcmp(us.running_sha256, h->deploy_sha, 32) == 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "deploy complete sha256=%s", uploaded);
                finish_deploy_success(h, msg);
                return;
            }
            finish_deploy_error(h, "post-restart-status");
            return;
        }
        if (us.phase == LMUX_BOOT_UPDATE_STABLE &&
            memcmp(us.confirmed_sha256, h->deploy_sha, 32) == 0 &&
            memcmp(us.running_sha256, h->deploy_sha, 32) == 0) {
            deploy_start_supervisor_install(a, h);
            return;
        }
        if (us.phase != LMUX_BOOT_UPDATE_TESTING ||
            us.activation_id != h->deploy_activation_id ||
            us.generation != h->deploy_generation ||
            memcmp(us.candidate_sha256, h->deploy_sha, 32) != 0 ||
            memcmp(us.running_sha256, h->deploy_sha, 32) != 0) {
            if (h->control_fd >= 0 && !h->boot_control_gone) {
                control_printf(h, "ERR DEPLOY candidate rolled back or superseded\n");
                control_printf(h, "confirmed=%s\n", confirmed);
                control_printf(h, "candidate=%s\n", candidate);
                control_printf(h, "running=%s\n", running);
            }
            v2_host_close_fd(&h->control_fd);
            upload_clear(h);
            deploy_clear(h);
            return;
        }
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "confirming...\n");
        h->deploy_phase = DEPLOY_CONFIRMING;
        if (queue_confirm_deploy(a, h) < 0) {
            finish_deploy_error(h, "queue");
            return;
        }
        return;
    }
    if (h->deploy_active && opcode == LMUX_BOOT_CONFIRM_CANDIDATE) {
        clear_boot_pending(h);
        h->deploy_phase = DEPLOY_FINAL_VERIFY;
        if (queue_update_status(a, h) < 0) {
            finish_deploy_error(h, "queue");
            return;
        }
        return;
    }
    if (h->deploy_active && opcode == LMUX_BOOT_ROLLBACK) {
        clear_boot_pending(h);
        deploy_clear(h);
        return;
    }
    if (h->deploy_active && opcode == LMUX_BOOT_SUPERVISOR_INSTALL) {
        struct lmux_boot_supervisor_install_response ir;
        char sha[65];
        clear_boot_pending(h);
        if (lmux_boot_decode_supervisor_install_response(body, body_len, &ir) < 0 ||
            memcmp(ir.sha256, h->deploy_sha, 32) != 0) {
            finish_deploy_error(h, "malformed-install");
            return;
        }
        h->deploy_install_already =
            (ir.flags & LMUX_BOOT_SUPERVISOR_INSTALL_ALREADY) != 0;
        digest_to_hex(h->deploy_sha, sha);
        if (h->control_fd >= 0 && !h->boot_control_gone) {
            control_printf(h, "supervisor installed sha256=%s%s\n", sha,
                           h->deploy_install_already ? " already-installed" : "");
        }
        if (!h->deploy_restart_supervisor) {
            char msg[128];
            snprintf(msg, sizeof(msg), "deploy complete sha256=%s", sha);
            finish_deploy_success(h, msg);
            return;
        }
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "restarting supervisor...\n");
        h->deploy_phase = DEPLOY_RESTART_SUPERVISOR;
        if (queue_supervisor_restart(a, h) < 0) {
            finish_deploy_error(h, "queue");
            return;
        }
        return;
    }
    if (h->deploy_active && opcode == LMUX_BOOT_SUPERVISOR_RESTART) {
        clear_boot_pending(h);
        h->deploy_phase = DEPLOY_WAIT_SUPERVISOR_RECONNECT;
        h->deploy_deadline_ms = 0;
        if (h->control_fd >= 0 && !h->boot_control_gone)
            control_printf(h, "supervisor restart accepted; waiting for reconnect\n");
        return;
    }
    if (h->control_fd >= 0 && !h->boot_control_gone) {
        if (opcode == LMUX_BOOT_GET_INFO) {
            struct lmux_boot_info info;
            if (lmux_boot_decode_get_info_response(body, body_len, &info) < 0) {
                control_printf(h, "ERR BOOT malformed\n");
            } else {
                control_printf(h, "OK GET_INFO\n");
                control_printf(h, "outer_version=%u\n", info.outer_version);
                control_printf(h, "boot_version=%u\n", info.boot_version);
                control_printf(h, "capabilities=0x%08x\n", info.capabilities);
                control_printf(h, "max_outer_payload=%u\n", info.max_outer_payload);
                control_printf(h, "max_transport_payload=%u\n",
                               info.max_transport_payload);
            }
        } else if (opcode == LMUX_BOOT_WORKER_STATUS) {
            struct lmux_boot_worker_status ws;
            if (lmux_boot_decode_worker_status(body, body_len, &ws) < 0) {
                control_printf(h, "ERR BOOT malformed\n");
            } else {
                control_printf(h, "OK WORKER_STATUS\n");
                control_printf(h, "worker_state=%s\n",
                               lmux_boot_worker_state_name(ws.worker_state));
                control_printf(h, "flags=0x%08x\n", ws.flags);
                control_printf(h, "worker_incarnation=%llu\n",
                               (unsigned long long)ws.worker_incarnation);
                control_printf(h, "uptime_ms=%llu\n",
                               (unsigned long long)ws.uptime_ms);
                control_printf(h, "restart_count=%u\n", ws.restart_count);
            }
        } else if (opcode == LMUX_BOOT_ACTIVATE_CANDIDATE) {
            struct lmux_boot_activate_response ar;
            if (lmux_boot_decode_activate_response(body, body_len, &ar) < 0) {
                control_printf(h, "ERR BOOT malformed\n");
            } else {
                control_printf(h, "OK ACTIVATE\n");
                control_printf(h, "activation_id=%llu\n",
                               (unsigned long long)ar.activation_id);
                control_printf(h, "generation=%llu\n",
                               (unsigned long long)ar.generation);
                control_printf(h, "confirm_timeout_ms=%u\n",
                               ar.confirm_timeout_ms);
                control_printf(h, "phase=%s\n",
                               lmux_boot_update_phase_name(ar.phase));
            }
        } else if (opcode == LMUX_BOOT_UPDATE_STATUS) {
            struct lmux_boot_update_status us;
            char confirmed[65], candidate[65], running[65];
            if (lmux_boot_decode_update_status(body, body_len, &us) < 0) {
                control_printf(h, "ERR BOOT malformed\n");
            } else {
                digest_to_hex(us.confirmed_sha256, confirmed);
                digest_to_hex(us.candidate_sha256, candidate);
                digest_to_hex(us.running_sha256, running);
                control_printf(h, "OK UPDATE_STATUS\n");
                control_printf(h, "phase=%s\n",
                               lmux_boot_update_phase_name(us.phase));
                control_printf(h, "flags=0x%08x\n", us.flags);
                control_printf(h, "generation=%llu\n",
                               (unsigned long long)us.generation);
                control_printf(h, "activation_id=%llu\n",
                               (unsigned long long)us.activation_id);
                control_printf(h, "confirm_remaining_ms=%u\n",
                               us.confirm_remaining_ms);
                control_printf(h, "last_update_error=%s\n",
                               lmux_boot_status_name(us.last_update_error));
                control_printf(h, "confirmed_sha256=%s\n", confirmed);
                control_printf(h, "candidate_sha256=%s\n", candidate);
                control_printf(h, "running_sha256=%s\n", running);
            }
        } else if (opcode == LMUX_BOOT_CONFIRM_CANDIDATE) {
            control_printf(h, "OK CONFIRM\n");
        } else if (opcode == LMUX_BOOT_ROLLBACK) {
            control_printf(h, "OK ROLLBACK\n");
        } else if (opcode == LMUX_BOOT_SUPERVISOR_INSTALL) {
            struct lmux_boot_supervisor_install_response ir;
            char sha[65];
            if (lmux_boot_decode_supervisor_install_response(body, body_len, &ir) < 0) {
                control_printf(h, "ERR BOOT malformed\n");
            } else {
                digest_to_hex(ir.sha256, sha);
                control_printf(h, "OK SUPERVISOR_INSTALL\n");
                control_printf(h, "sha256=%s\n", sha);
                control_printf(h, "installed=%s\n",
                               (ir.flags & LMUX_BOOT_SUPERVISOR_INSTALL_INSTALLED) ? "yes" : "no");
                control_printf(h, "already_installed=%s\n",
                               (ir.flags & LMUX_BOOT_SUPERVISOR_INSTALL_ALREADY) ? "yes" : "no");
            }
        } else if (opcode == LMUX_BOOT_SUPERVISOR_RESTART) {
            control_printf(h, "OK SUPERVISOR_RESTART\n");
        }
    }
    v2_host_close_fd(&h->control_fd);
    clear_boot_pending(h);
}

void v2_host_control_service(struct v2_app *a, struct v2_host *h,
                             const struct v2_host_status *st)
{
    char tmp[128];
    ssize_t r;
    if (h->control_fd < 0)
        return;
    if (h->boot_upload_active && h->boot_upload_ready_sent &&
        !h->boot_pending) {
        uint8_t buf[LMUX_BOOT_UPLOAD_MAX_CHUNK];
        size_t want = h->boot_upload_max_chunk;
        if (want == 0 || want > sizeof(buf))
            want = sizeof(buf);
        if (h->boot_upload_declared_size - h->boot_upload_sent < want)
            want = (size_t)(h->boot_upload_declared_size - h->boot_upload_sent);
        if (want == 0) {
            if (upload_queue_end(a, h) < 0)
                upload_fail(h, "queue");
            return;
        }
        r = read(h->control_fd, buf, want);
        if (r > 0) {
            if (upload_queue_data(a, h, buf, (uint32_t)r) < 0)
                upload_fail(h, "queue");
            return;
        }
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            v2_host_control_on_hup(h);
        return;
    }
    r = read(h->control_fd, tmp, sizeof(tmp));
    if (r <= 0) {
        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            v2_host_control_on_hup(h);
        return;
    }
    for (ssize_t i = 0; i < r; i++) {
        char ch = tmp[i];
        if (ch == '\n') {
            h->control_in[h->control_in_len] = 0;
            control_handle_line(a, h, st, h->control_in);
            h->control_in_len = 0;
        } else if (h->control_in_len + 1 < sizeof(h->control_in)) {
            h->control_in[h->control_in_len++] = ch;
        } else {
            h->control_in_len = 0;
        }
    }
}
