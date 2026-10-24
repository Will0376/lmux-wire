#include "lmux2_runtime.h"
#include "boot_protocol.h"
#include "carrier.h"
#include "carrier_manager.h"
#include "dirty.h"
#include "lmc1.h"
#include "host.h"
#include "lmux2_control.h"
#include "lmux2_core.h"
#include "lmux.h"
#include "services.h"
#include "supervisor.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum carrier_switch_phase {
    CARRIER_SWITCH_IDLE = 0,
    CARRIER_SWITCH_QUERYING,
    CARRIER_SWITCH_WAIT_QUERY,
    CARRIER_SWITCH_PREPARING,
    CARRIER_SWITCH_WAIT_READY,
    CARRIER_SWITCH_SENDING_COMMIT,
    CARRIER_SWITCH_RESOLVING,
    CARRIER_SWITCH_CONVERGING
};

struct v2_app {
    enum lmux2_role role;
    struct lmux_carrier_manager carriers;
    int verbose;
    int stop;
    int worker_mode;
    int worker_link_active;
    int worker_link_up_flush;
    int worker_link_up_ack_sent;
    int worker_link_up_delay_done;
    int supervisor_ctrl_fd;
    int outer_enabled;
    uint64_t worker_link_epoch;
    struct lmux_link_endpoint outer;
    int factories_reported;
    int peer_restart_converging;
    uint64_t next_service_id;
    unsigned long long connection_recreated_count;
    unsigned long long connection_established_count;
    unsigned long long peer_restart_count;
    unsigned long long stream_open_count;
    unsigned long long stream_close_count;
    unsigned long long stream_reset_count;
    struct lmux2_conn conn;
    uint8_t enc_in[LMUX2_MAX_ENCODED];
    size_t enc_in_len;
    int discard_until_delim;
    uint8_t out[LMUX2_MAX_ENCODED];
    size_t out_len;
    size_t out_pos;
    struct v2_service services[V2_MAX_SERVICES];
    struct v2_target_service_status target_status[V2_MAX_SERVICES];
    struct v2_host host;
    struct lmux_lmc1_raw_io lmc_io[LMUX_CARRIER_MAX_ENDPOINTS];
    struct lmux_lmc1_dirty_io lmc_dirty_io[LMUX_CARRIER_MAX_ENDPOINTS];
    struct lmux_dirty dirty;
    int dirty_active;
    struct {
        enum carrier_switch_phase phase;
        char name[LMUX_CARRIER_NAME_MAX + 1];
        enum lmux_carrier_mode mode;
        uint8_t nonce[LMUX_LMC1_NONCE_LEN];
        uint8_t cookie[LMUX_LMC1_COOKIE_LEN];
        uint8_t boot_id[LMUX_LMC1_BOOT_ID_LEN];
        uint64_t observed_generation;
        uint64_t committed_generation;
        long long deadline_ms;
        long long next_retry_ms;
        uint64_t pre_local_conn;
        uint64_t pre_peer_conn;
        unsigned long long pre_established_count;
        int old_outer_frozen;
        int target_committed;
        unsigned resolve_attempt;
    } carrier_switch;
    int carrier_committed_but_down;
    char carrier_last_error[96];
    struct {
        int active;
        unsigned endpoint;
        size_t pos;
        long long deadline_ms;
        uint8_t wire[32];
        size_t wire_len;
    } carrier_console;
    unsigned carrier_console_hold_mask;
    unsigned carrier_failure_hold_mask;
};

static struct v2_app *global_v2_app;

static void reset_connection_streams(struct v2_app *a);
static void retire_connection_streams(struct v2_app *a);
static void build_next_output(struct v2_app *a);
static void flush_carrier(struct v2_app *a);
static void worker_maybe_finish_link_up(struct v2_app *a);
static void feed_lmux_bytes(struct v2_app *a, const uint8_t *buf, size_t len);
static void feed_raw_endpoint(struct v2_app *a, unsigned endpoint,
                              int allow_outer);
static void flush_raw_control(struct v2_app *a, unsigned endpoint);

static enum lmux_carrier_mode active_carrier_mode(const struct v2_app *a)
{
    const struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_active_endpoint_const(&a->carriers);
    return ep ? ep->mode : LMUX_CARRIER_MODE_RAW;
}

static int lmc_any_output(const struct v2_app *a, unsigned endpoint)
{
    return endpoint < LMUX_CARRIER_MAX_ENDPOINTS &&
        (lmux_lmc1_raw_has_output(&a->lmc_io[endpoint]) ||
         lmux_lmc1_dirty_has_output(&a->lmc_dirty_io[endpoint]));
}

static int dirty_has_work(const struct lmux_dirty *d)
{
    unsigned i;
    if (!d)
        return 0;
    if (d->output_pos < d->output_len || d->pending_fail)
        return 1;
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++)
        if (d->tx[i].used)
            return 1;
    for (i = 0; i < LMUX_DIRTY_CONTROL_MAX; i++)
        if (d->control[i].used)
            return 1;
    return 0;
}

static struct lmux_carrier *active_carrier(struct v2_app *a)
{
    return lmux_carrier_manager_active(&a->carriers);
}

static const struct lmux_carrier *active_carrier_const(const struct v2_app *a)
{
    return lmux_carrier_manager_active_const(&a->carriers);
}

void v2_app_log(struct v2_app *a, int verbose, const char *fmt, ...)
{
    va_list ap;
    if (verbose && !a->verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

#define log_msg v2_app_log

long long v2_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define now_ms v2_now_ms

static void cleanup_terminal_bindings(struct v2_app *a);

static uint64_t make_conn_id(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
            v = 0;
        close(fd);
    }
    if (!v)
        v = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)time(NULL) ^ 0x4c4d5832u;
    if (!v)
        v = 1;
    return v;
}

static void runtime_random_bytes(uint8_t *out, size_t len)
{
    size_t off = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        while (off < len) {
            ssize_t r = read(fd, out + off, len - off);
            if (r > 0) {
                off += (size_t)r;
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        close(fd);
    }
    while (off < len) {
        uint64_t v = make_conn_id() ^ (uint64_t)off;
        size_t n = len - off < sizeof(v) ? len - off : sizeof(v);
        memcpy(out + off, &v, n);
        off += n;
    }
}

static int parse_role(const char *s, enum lmux2_role *role)
{
    if (strcmp(s, "host") == 0) {
        *role = LMUX2_ROLE_HOST;
        return 0;
    }
    if (strcmp(s, "target") == 0) {
        *role = LMUX2_ROLE_TARGET;
        return 0;
    }
    return -1;
}

static const char *role_name(enum lmux2_role role)
{
    return role == LMUX2_ROLE_HOST ? "host" : "target";
}

static void begin_outer(struct v2_app *a)
{
    lmux_link_endpoint_begin(&a->outer, make_conn_id());
    log_msg(a, 1, "outer HELLO sent local_conn=%llu",
            (unsigned long long)a->outer.local_conn_id);
}

static void try_open_carrier(struct v2_app *a)
{
    unsigned opened = lmux_carrier_manager_try_open_except(
        &a->carriers, now_ms(),
        a->role == LMUX2_ROLE_HOST ? a->carrier_console_hold_mask : 0);
    int id;
    for (id = 0; id < LMUX_CARRIER_MAX_ENDPOINTS; id++) {
        struct lmux_carrier_endpoint *ep;
        if (!(opened & (1u << id)))
            continue;
        ep = lmux_carrier_manager_at(&a->carriers, (unsigned)id);
        if (!ep)
            continue;
        log_msg(a, 1, "lmux2 %s carrier opened %s%s",
                ep->name,
                lmux_carrier_label(&ep->carrier),
                lmux_carrier_manager_is_active(&a->carriers, ep) ?
                " active" : " standby");
        if (!lmux_carrier_manager_is_active(&a->carriers, ep))
            continue;
        if (a->carrier_console.active)
            continue;
        if (a->carrier_failure_hold_mask & (1u << (unsigned)id))
            continue;
        if (a->outer_enabled) {
            if (!a->carrier_switch.old_outer_frozen)
                begin_outer(a);
        } else {
            reset_connection_streams(a);
        }
    }
}

int v2_runtime_carrier_console_start(struct v2_app *a, const char *name,
                                     char *error, size_t error_cap)
{
    static const uint8_t escape[] = "\r\n~LMUX CONSOLE\r\n";
    struct lmux_carrier_endpoint *ep;
    ptrdiff_t endpoint;
    if (!a || a->role != LMUX2_ROLE_HOST || !name) {
        snprintf(error, error_cap, "unavailable");
        return -1;
    }
    ep = lmux_carrier_manager_find(&a->carriers, name);
    if (!ep) {
        snprintf(error, error_cap, "unknown-carrier");
        return -1;
    }
    endpoint = ep - a->carriers.endpoint;
    if (!lmux_carrier_manager_is_active(&a->carriers, ep) ||
        ep->mode != LMUX_CARRIER_MODE_DIRTY) {
        snprintf(error, error_cap, "not-active-dirty");
        return -1;
    }
    if (a->carrier_switch.phase != CARRIER_SWITCH_IDLE ||
        a->carrier_console.active) {
        snprintf(error, error_cap, "busy");
        return -1;
    }
    if (a->carrier_console_hold_mask & (1u << (unsigned)endpoint)) {
        snprintf(error, error_cap, "already-released");
        return -1;
    }
    v2_host_control_outer_down(&a->host);
    lmux_link_endpoint_down(&a->outer);
    retire_connection_streams(a);
    a->out_len = a->out_pos = 0;
    a->enc_in_len = 0;
    a->discard_until_delim = 0;
    memset(&a->dirty, 0, sizeof(a->dirty));
    a->dirty_active = 0;
    lmux_lmc1_raw_init(&a->lmc_io[endpoint]);
    lmux_lmc1_dirty_init(&a->lmc_dirty_io[endpoint]);
    memset(&a->carrier_console, 0, sizeof(a->carrier_console));
    a->carrier_console.active = 1;
    a->carrier_console.endpoint = (unsigned)endpoint;
    a->carrier_console.deadline_ms = now_ms() + 5000;
    memcpy(a->carrier_console.wire, escape, sizeof(escape) - 1);
    a->carrier_console.wire_len = sizeof(escape) - 1;
    return 0;
}

static void carrier_console_finish(struct v2_app *a, int ok,
                                   const char *reason)
{
    char line[128];
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&a->carriers,
                                a->carrier_console.endpoint);
    if (ok) {
        snprintf(line, sizeof(line), "OK CARRIER CONSOLE name=%s\n",
                 ep ? ep->name : "-");
        a->carrier_console_hold_mask |=
            1u << a->carrier_console.endpoint;
        if (ep && lmux_carrier_fd(&ep->carrier) >= 0)
            lmux_carrier_mark_lost(&ep->carrier, now_ms());
    } else {
        snprintf(line, sizeof(line), "ERR CARRIER CONSOLE %s\n",
                 reason ? reason : "failed");
    }
    a->carrier_console.active = 0;
    v2_host_control_carrier_result(&a->host, line);
}

static void carrier_console_tick(struct v2_app *a, long long now)
{
    struct lmux_carrier_endpoint *ep;
    struct lmux_carrier_io io;
    if (!a->carrier_console.active)
        return;
    if (now > a->carrier_console.deadline_ms) {
        carrier_console_finish(a, 0, "timeout");
        return;
    }
    ep = lmux_carrier_manager_at(&a->carriers,
                                 a->carrier_console.endpoint);
    if (!ep) {
        carrier_console_finish(a, 0, "carrier-missing");
        return;
    }
    if (lmux_carrier_fd(&ep->carrier) < 0)
        return;
    io = lmux_carrier_write(&ep->carrier,
        a->carrier_console.wire + a->carrier_console.pos,
        a->carrier_console.wire_len - a->carrier_console.pos);
    if (io.status == LMUX_CARRIER_IO_PROGRESS) {
        a->carrier_console.pos += (size_t)io.n;
        if (a->carrier_console.pos == a->carrier_console.wire_len)
            carrier_console_finish(a, 1, NULL);
    } else if (io.status != LMUX_CARRIER_IO_WOULD_BLOCK) {
        carrier_console_finish(a, 0, "write-failed");
    }
}

static void carrier_lost(struct v2_app *a)
{
    struct lmux_carrier *carrier = active_carrier(a);
    unsigned active = (unsigned)a->carriers.active_index;
    if (!carrier || lmux_carrier_fd(carrier) < 0)
        return;
    log_msg(a, 1, "lmux2 carrier lost");
    if (a->dirty_active && active_carrier_mode(a) == LMUX_CARRIER_MODE_DIRTY) {
        lmux_dirty_peer_lost(&a->dirty);
        if (active < LMUX_CARRIER_MAX_ENDPOINTS)
            a->carrier_failure_hold_mask |= 1u << active;
    }
    if (a->outer_enabled) {
        if (a->role == LMUX2_ROLE_HOST)
            v2_host_control_outer_down(&a->host);
        lmux_link_endpoint_down(&a->outer);
        retire_connection_streams(a);
    } else {
        reset_connection_streams(a);
    }
    lmux_carrier_mark_lost(carrier, now_ms());
    a->out_len = a->out_pos = 0;
    a->enc_in_len = 0;
    a->discard_until_delim = 0;
    if (a->role == LMUX2_ROLE_HOST &&
        active_carrier_mode(a) == LMUX_CARRIER_MODE_DIRTY) {
        a->carrier_committed_but_down = 1;
        snprintf(a->carrier_last_error, sizeof(a->carrier_last_error),
                 "dirty-%s", lmux_dirty_fail_reason_name(a->dirty.fail_reason));
    }
}

int v2_runtime_boot_supported(struct v2_app *a)
{
    return a && a->outer_enabled &&
           a->outer.state == LMUX_LINK_ESTABLISHED &&
           (a->outer.negotiated_caps & LMUX_LINK_CAP_BOOT_INFO);
}

int v2_runtime_boot_upload_supported(struct v2_app *a)
{
    return v2_runtime_boot_supported(a) &&
           (a->outer.negotiated_caps & LMUX_LINK_CAP_BOOT_UPLOAD);
}

int v2_runtime_boot_managed_supported(struct v2_app *a)
{
    return v2_runtime_boot_supported(a) &&
           (a->outer.negotiated_caps & LMUX_LINK_CAP_BOOT_MANAGED_WORKER);
}

int v2_runtime_boot_supervisor_supported(struct v2_app *a)
{
    return v2_runtime_boot_supported(a) &&
           (a->outer.negotiated_caps & LMUX_LINK_CAP_BOOT_SUPERVISOR_CONTROL);
}

int v2_runtime_queue_boot_request_body(struct v2_app *a, uint16_t opcode,
                                       uint64_t request_id,
                                       const uint8_t *body, uint32_t body_len)
{
    uint8_t payload[LMUX_LINK_MAX_PAYLOAD];
    uint32_t len = 0;
    if (!v2_runtime_boot_supported(a))
        return -1;
    if (lmux_boot_make_request(payload, sizeof(payload), &len, opcode,
                               request_id, body, body_len) < 0)
        return -1;
    return lmux_link_boot_queue_frame(&a->outer, LMUX_LINK_T_BOOT_REQUEST,
                                      payload, len);
}

int v2_runtime_queue_boot_request(struct v2_app *a, uint16_t opcode,
                                  uint64_t request_id)
{
    return v2_runtime_queue_boot_request_body(a, opcode, request_id, NULL, 0);
}

void v2_runtime_lmux_marker(struct v2_app *a, uint64_t *local_conn,
                            uint64_t *peer_conn,
                            uint64_t *established_count)
{
    if (local_conn)
        *local_conn = a->conn.local_conn_id;
    if (peer_conn)
        *peer_conn = a->conn.peer_conn_id;
    if (established_count)
        *established_count = a->connection_established_count;
}

int v2_runtime_lmux_established(struct v2_app *a)
{
    return a && a->conn.state == LMUX2_CONN_ESTABLISHED;
}

int v2_runtime_outer_enabled(struct v2_app *a)
{
    return a && a->outer_enabled;
}

int v2_runtime_open_stream_token(struct v2_app *a, uint64_t service_id,
                                 uint64_t token, uint64_t *stream_id)
{
    if (!a)
        return -1;
    return lmux2_open_stream_ex(&a->conn, service_id, token, stream_id);
}

int v2_runtime_stream_is_open(struct v2_app *a, uint64_t stream_id)
{
    const struct lmux2_stream *st;
    if (!a)
        return 0;
    st = lmux2_find_stream_const(&a->conn, stream_id);
    return st && (st->state == LMUX2_STREAM_OPEN ||
                  st->state == LMUX2_STREAM_REMOTE_CLOSED);
}

int v2_runtime_stream_state(struct v2_app *a, uint64_t stream_id)
{
    const struct lmux2_stream *st;
    if (!a)
        return LMUX2_STREAM_UNUSED;
    st = lmux2_find_stream_const(&a->conn, stream_id);
    return st ? (int)st->state : (int)LMUX2_STREAM_UNUSED;
}

size_t v2_runtime_stream_read(struct v2_app *a, uint64_t stream_id,
                              uint8_t *buf, size_t len)
{
    if (!a)
        return 0;
    return lmux2_stream_read(&a->conn, stream_id, buf, len);
}

size_t v2_runtime_stream_write(struct v2_app *a, uint64_t stream_id,
                               const uint8_t *buf, size_t len)
{
    if (!a)
        return 0;
    return lmux2_stream_write(&a->conn, stream_id, buf, len);
}

size_t v2_runtime_stream_rx_len(struct v2_app *a, uint64_t stream_id)
{
    if (!a)
        return 0;
    return lmux2_stream_rx_len(&a->conn, stream_id);
}

size_t v2_runtime_stream_tx_len(struct v2_app *a, uint64_t stream_id)
{
    if (!a)
        return 0;
    return lmux2_stream_tx_len(&a->conn, stream_id);
}

int v2_runtime_close_stream(struct v2_app *a, uint64_t stream_id)
{
    if (!a)
        return -1;
    return lmux2_close_stream(&a->conn, stream_id);
}

static void accept_carrier(struct v2_app *a, unsigned id)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&a->carriers, id);
    int rc = ep ? lmux_carrier_accept(&ep->carrier) : -1;
    if (rc <= 0)
        return;
    log_msg(a, 1, "lmux2 %s carrier accepted%s",
            ep->name,
            lmux_carrier_manager_is_active(&a->carriers, ep) ?
            " active" : " standby");
    if (!lmux_carrier_manager_is_active(&a->carriers, ep) ||
        (a->carrier_failure_hold_mask & (1u << id)))
        return;
    if (a->outer_enabled) {
        if (!a->carrier_switch.old_outer_frozen)
            begin_outer(a);
    } else {
        reset_connection_streams(a);
    }
}

static void drain_standby_carrier(struct v2_app *a,
                                  unsigned id)
{
    feed_raw_endpoint(a, id, 0);
}

static int queue_control(struct v2_app *a, uint8_t kind, uint32_t req_id,
                         uint64_t service_id, uint64_t token,
                         const uint8_t *tlv, uint16_t tlv_len)
{
    struct lmux2_frame fr;
    if (lmux2_control_make_frame(kind, req_id, service_id, token,
                                 tlv, tlv_len, &fr) < 0)
        return -1;
    return lmux2_queue_frame(&a->conn, &fr);
}

static int queue_factory_report(struct v2_app *a)
{
    uint8_t tlv[32];
    size_t off = 0;
    if (lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE,
                             V2_SERVICE_UNIX_RENDEZVOUS) < 0 ||
        lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE,
                             V2_SERVICE_PTY) < 0 ||
        lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE,
                             V2_SERVICE_FILE) < 0 ||
        lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE,
                             V2_SERVICE_EXEC) < 0)
        return -1;
    if (queue_control(a, V2_CTRL_FACTORY_REPORT, 0, 0, 0, tlv, (uint16_t)off) < 0)
        return -1;
    log_msg(a, 1, "lmux2 FACTORY_REPORT sent unix-rendezvous pty file exec");
    return 0;
}

static int queue_service_ok(struct v2_app *a, uint32_t req_id, const struct v2_service *s)
{
    uint8_t tlv[160];
    size_t off = 0;
    if (lmux2_control_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_NAME, s->name,
                              (uint16_t)(strlen(s->name) + 1)) < 0 ||
        lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE, s->type) < 0)
        return -1;
    if (s->target_path[0] &&
        lmux2_control_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_TARGET_PATH, s->target_path,
                              (uint16_t)(strlen(s->target_path) + 1)) < 0)
        return -1;
    if (s->argv[0] &&
        (lmux2_control_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_ARGV, s->argv,
                               (uint16_t)(strlen(s->argv) + 1)) < 0 ||
         lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SUPERVISED,
                              s->supervised ? 1 : 0) < 0))
        return -1;
    if (queue_control(a, V2_CTRL_SERVICE_OK, req_id, s->id, 0, tlv, (uint16_t)off) < 0)
        return -1;
    log_msg(a, 1, "lmux2 SERVICE_OK sent req=%u service=%s id=%llu type=%s",
            req_id, s->name, (unsigned long long)s->id, v2_service_type_name(s->type));
    return 0;
}

static int queue_service_error(struct v2_app *a, uint32_t req_id, uint64_t service_id,
                               uint8_t code)
{
    uint8_t tlv[8];
    size_t off = 0;
    (void)lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_ERROR_CODE, code);
    if (queue_control(a, V2_CTRL_SERVICE_ERROR, req_id, service_id, 0, tlv, (uint16_t)off) < 0)
        return -1;
    log_msg(a, 1, "lmux2 SERVICE_ERROR sent req=%u service=%llu code=%u",
            req_id, (unsigned long long)service_id, code);
    return 0;
}

int v2_runtime_queue_service_event(struct v2_app *a, const struct v2_service *s,
                                   uint8_t event, uint64_t token)
{
    uint8_t tlv[8];
    size_t off = 0;
    (void)lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_EVENT, event);
    return queue_control(a, V2_CTRL_SERVICE_EVENT, 0, s->id, token, tlv, (uint16_t)off);
}

int v2_runtime_send_winsize_control(struct v2_app *a, uint64_t service_id,
                                    unsigned rows, unsigned cols,
                                    unsigned xp, unsigned yp)
{
    uint8_t tlv[16], ws[8];
    size_t off = 0;
    uint32_t req_id;
    if (!service_id || rows == 0 || cols == 0 || rows > 1000 || cols > 1000)
        return -1;
    lmux_put_le16(ws, (uint16_t)rows);
    lmux_put_le16(ws + 2, (uint16_t)cols);
    lmux_put_le16(ws + 4, (uint16_t)xp);
    lmux_put_le16(ws + 6, (uint16_t)yp);
    if (lmux2_control_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_WINSIZE, ws, sizeof(ws)) < 0)
        return -1;
    req_id = ++a->host.next_req_id;
    if (req_id == 0)
        req_id = ++a->host.next_req_id;
    return queue_control(a, V2_CTRL_SERVICE_CONTROL, req_id,
                         service_id, 0, tlv, (uint16_t)off);
}

int v2_runtime_send_service_ensure(struct v2_app *a, struct v2_desired *d)
{
    uint8_t tlv[256];
    size_t off = 0;
    uint8_t type = (uint8_t)d->type;
    d->req_id = ++a->host.next_req_id;
    if (d->req_id == 0)
        d->req_id = ++a->host.next_req_id;
    if (lmux2_control_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_NAME, d->name,
                              (uint16_t)(strlen(d->name) + 1)) < 0 ||
        lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE, type) < 0)
        return -1;
    if (d->type == V2_SERVICE_UNIX_RENDEZVOUS) {
        if (lmux2_control_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_TARGET_PATH, d->target_path,
                                  (uint16_t)(strlen(d->target_path) + 1)) < 0)
            return -1;
    } else if (d->type == V2_SERVICE_PTY) {
        if (lmux2_control_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_ARGV, d->argv,
                                  (uint16_t)(strlen(d->argv) + 1)) < 0 ||
            lmux2_control_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SUPERVISED,
                                 d->supervised ? 1 : 0) < 0)
            return -1;
    } else if (d->type == V2_SERVICE_FILE) {
        /* No target-side configuration for the current file factory. */
    } else if (d->type == V2_SERVICE_EXEC) {
        /* Internal private exec service has no target-side path/config. */
    } else {
        return -1;
    }
    d->sent = 1;
    log_msg(a, 1, "lmux2 SERVICE_ENSURE sent req=%u name=%s type=%s",
            d->req_id, d->name, v2_service_type_name(d->type));
    return queue_control(a, V2_CTRL_SERVICE_ENSURE, d->req_id, 0, 0,
                         tlv, (uint16_t)off);
}

void v2_runtime_count_stream_open(struct v2_app *a)
{
    a->stream_open_count++;
}

void v2_runtime_count_stream_close(struct v2_app *a)
{
    a->stream_close_count++;
}

static int create_target_unix_service(struct v2_app *a, uint32_t req_id,
                                      const char *name, const char *path,
                                      enum v2_service_type type)
{
    struct v2_service *s = v2_service_find_name(a->services, name);
    if (type != V2_SERVICE_UNIX_RENDEZVOUS)
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    if (s) {
        if (s->type != type || strcmp(s->target_path, path) != 0)
            return queue_service_error(a, req_id, s->id, V2_E_CONFLICT);
        return queue_service_ok(a, req_id, s);
    }
    s = v2_service_alloc(a->services, &a->next_service_id);
    if (!s)
        return queue_service_error(a, req_id, 0, V2_E_LIMIT);
    s->type = type;
    if (v2_copy_bounded(s->name, sizeof(s->name), name) < 0 ||
        v2_copy_bounded(s->target_path, sizeof(s->target_path), path) < 0) {
        v2_service_cleanup(s);
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    }
    s->listen_fd = v2_create_listener(s->target_path, &s->created_path);
    if (s->listen_fd < 0) {
        v2_service_cleanup(s);
        return queue_service_error(a, req_id, 0, V2_E_IO);
    }
    v2_copy_bounded(s->listen_path, sizeof(s->listen_path), s->target_path);
    log_msg(a, 1, "lmux2 service %s id=%llu target listener %s", s->name,
            (unsigned long long)s->id, s->target_path);
    return queue_service_ok(a, req_id, s);
}

static int create_target_pty_service(struct v2_app *a, uint32_t req_id,
                                     const char *name, const char *argv,
                                     int supervised, enum v2_service_type type)
{
    struct v2_service *s = v2_service_find_name(a->services, name);
    if (type != V2_SERVICE_PTY || !supervised)
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    if (s) {
        if (s->type != type || strcmp(s->argv, argv) != 0 ||
            s->supervised != supervised)
            return queue_service_error(a, req_id, s->id, V2_E_CONFLICT);
        return queue_service_ok(a, req_id, s);
    }
    s = v2_service_alloc(a->services, &a->next_service_id);
    if (!s)
        return queue_service_error(a, req_id, 0, V2_E_LIMIT);
    s->type = type;
    s->supervised = supervised;
    if (v2_copy_bounded(s->name, sizeof(s->name), name) < 0 ||
        v2_copy_bounded(s->argv, sizeof(s->argv), argv) < 0) {
        v2_service_cleanup(s);
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    }
    if (v2_pty_create_master(s) < 0) {
        v2_service_cleanup(s);
        return queue_service_error(a, req_id, 0, V2_E_IO);
    }
    v2_pty_spawn_child(a, s);
    log_msg(a, 1, "lmux2 pty service %s id=%llu argv=%s slave=%s",
            s->name, (unsigned long long)s->id, s->argv, s->pty_slave_path);
    return queue_service_ok(a, req_id, s);
}

static int create_target_file_service(struct v2_app *a, uint32_t req_id,
                                      const char *name, enum v2_service_type type)
{
    struct v2_service *s = v2_service_find_name(a->services, name);
    if (type != V2_SERVICE_FILE)
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    if (s) {
        if (s->type != type)
            return queue_service_error(a, req_id, s->id, V2_E_CONFLICT);
        return queue_service_ok(a, req_id, s);
    }
    s = v2_service_alloc(a->services, &a->next_service_id);
    if (!s)
        return queue_service_error(a, req_id, 0, V2_E_LIMIT);
    s->type = type;
    s->file.fd = -1;
    if (v2_copy_bounded(s->name, sizeof(s->name), name) < 0) {
        v2_service_cleanup(s);
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    }
    log_msg(a, 1, "lmux2 file service %s id=%llu",
            s->name, (unsigned long long)s->id);
    return queue_service_ok(a, req_id, s);
}

static int create_target_exec_service(struct v2_app *a, uint32_t req_id,
                                      const char *name, enum v2_service_type type)
{
    struct v2_service *s = v2_service_find_name(a->services, name);
    if (type != V2_SERVICE_EXEC)
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    if (s) {
        if (s->type != type)
            return queue_service_error(a, req_id, s->id, V2_E_CONFLICT);
        return queue_service_ok(a, req_id, s);
    }
    s = v2_service_alloc(a->services, &a->next_service_id);
    if (!s)
        return queue_service_error(a, req_id, 0, V2_E_LIMIT);
    if (v2_exec_create_service(a, s, name) < 0) {
        v2_service_cleanup(s);
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    }
    log_msg(a, 1, "lmux2 exec service %s id=%llu",
            s->name, (unsigned long long)s->id);
    return queue_service_ok(a, req_id, s);
}

static void handle_service_ensure(struct v2_app *a,
                                  const struct lmux2_control_view *cv)
{
    char name[V2_NAME_MAX], path[V2_PATH_MAX], argv[V2_PATH_MAX];
    uint8_t type, supervised = 0;
    if (a->role != LMUX2_ROLE_TARGET ||
        lmux2_control_tlv_string(cv, V2_TLV_NAME, name, sizeof(name)) < 0 ||
        lmux2_control_tlv_u8(cv, V2_TLV_SERVICE_TYPE, &type) < 0) {
        (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
        return;
    }
    log_msg(a, 1, "lmux2 SERVICE_ENSURE received req=%u name=%s type=%s",
            cv->req_id, name, v2_service_type_name((enum v2_service_type)type));
    if (type == V2_SERVICE_UNIX_RENDEZVOUS) {
        if (lmux2_control_tlv_string(cv, V2_TLV_TARGET_PATH, path, sizeof(path)) < 0) {
            (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
            return;
        }
        (void)create_target_unix_service(a, cv->req_id, name, path,
                                         (enum v2_service_type)type);
    } else if (type == V2_SERVICE_PTY) {
        if (lmux2_control_tlv_string(cv, V2_TLV_ARGV, argv, sizeof(argv)) < 0 ||
            lmux2_control_tlv_u8(cv, V2_TLV_SUPERVISED, &supervised) < 0) {
            (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
            return;
        }
        (void)create_target_pty_service(a, cv->req_id, name, argv, supervised != 0,
                                        (enum v2_service_type)type);
    } else if (type == V2_SERVICE_FILE) {
        (void)create_target_file_service(a, cv->req_id, name,
                                         (enum v2_service_type)type);
    } else if (type == V2_SERVICE_EXEC) {
        (void)create_target_exec_service(a, cv->req_id, name,
                                         (enum v2_service_type)type);
    } else {
        (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
    }
}

static void handle_service_delete(struct v2_app *a,
                                  const struct lmux2_control_view *cv)
{
    struct v2_service *s = v2_service_find_id(a->services, cv->service_id);
    if (!s)
        return;
    (void)v2_runtime_queue_service_event(a, s, V2_EVENT_DELETED, 0);
    v2_service_cleanup(s);
}

static void handle_service_control(struct v2_app *a,
                                   const struct lmux2_control_view *cv)
{
    struct v2_service *s = v2_service_find_id(a->services, cv->service_id);
    const uint8_t *p;
    uint16_t len;
    if (a->role != LMUX2_ROLE_TARGET || !s || s->type != V2_SERVICE_PTY)
        return;
    if (lmux2_control_tlv_bytes(cv, V2_TLV_WINSIZE, &p, &len) < 0 || len != 8)
        return;
    v2_pty_apply_winsize(s, lmux_get_le16(p), lmux_get_le16(p + 2),
                         lmux_get_le16(p + 4), lmux_get_le16(p + 6));
}

static void handle_control(struct v2_app *a, const struct lmux2_frame *fr)
{
    struct lmux2_control_view cv;
    if (lmux2_control_parse_frame(fr, &cv) < 0)
        return;
    switch (cv.kind) {
    case V2_CTRL_FACTORY_REPORT:
        if (a->role == LMUX2_ROLE_HOST)
            v2_host_on_factory_report(a, &a->host,
                                      lmux2_control_has_service_type(&cv, V2_SERVICE_UNIX_RENDEZVOUS),
                                      lmux2_control_has_service_type(&cv, V2_SERVICE_PTY),
                                      lmux2_control_has_service_type(&cv, V2_SERVICE_FILE),
                                      lmux2_control_has_service_type(&cv, V2_SERVICE_EXEC));
        break;
    case V2_CTRL_SERVICE_ENSURE:
        handle_service_ensure(a, &cv);
        break;
    case V2_CTRL_SERVICE_OK:
        if (a->role == LMUX2_ROLE_HOST) {
            uint8_t type = 0;
            (void)lmux2_control_tlv_u8(&cv, V2_TLV_SERVICE_TYPE, &type);
            v2_host_on_service_ok(a, &a->host, cv.req_id, cv.service_id,
                                  (enum v2_service_type)type,
                                  &a->next_service_id);
        }
        break;
    case V2_CTRL_SERVICE_ERROR:
        {
            uint8_t code = 0;
            (void)lmux2_control_tlv_u8(&cv, V2_TLV_ERROR_CODE, &code);
            if (a->role == LMUX2_ROLE_HOST)
                v2_host_on_service_error(a, cv.req_id, cv.service_id, code);
            else
                log_msg(a, 1, "lmux2 SERVICE_ERROR req=%u service=%llu code=%u",
                        cv.req_id, (unsigned long long)cv.service_id, code);
        }
        break;
    case V2_CTRL_SERVICE_DELETE:
        handle_service_delete(a, &cv);
        break;
    case V2_CTRL_SERVICE_EVENT:
        if (a->role == LMUX2_ROLE_HOST) {
            uint8_t event;
            if (lmux2_control_tlv_u8(&cv, V2_TLV_EVENT, &event) == 0)
                v2_host_on_service_event(a, &a->host, cv.service_id, cv.token, event);
        }
        break;
    case V2_CTRL_SERVICE_CONTROL:
        handle_service_control(a, &cv);
        break;
    default:
        break;
    }
    if (a->role == LMUX2_ROLE_HOST)
        v2_host_attach_pending_clients(a, &a->host, &a->conn);
}

static int stream_can_send_endpoint_data(const struct v2_app *a, uint64_t stream_id)
{
    const struct lmux2_stream *st = lmux2_find_stream_const(&a->conn, stream_id);
    if (!st)
        return 0;
    return st->state == LMUX2_STREAM_OPEN || st->state == LMUX2_STREAM_REMOTE_CLOSED;
}

static void close_binding(struct v2_app *a, struct v2_service *s, struct v2_binding *b,
                          int send_close)
{
    uint64_t stream_id = b->stream_id;
    if (b->fd >= 0)
        v2_close_fd(&b->fd);
    if (send_close && stream_id) {
        (void)lmux2_close_stream(&a->conn, stream_id);
        a->stream_close_count++;
        log_msg(a, 1, "lmux2 STREAM_CLOSE sent service=%s stream=%llu binding-detached",
                s->name, (unsigned long long)stream_id);
    }
    b->stream_id = 0;
}

static void clear_pending(struct v2_app *a, struct v2_service *s)
{
    if (a->role == LMUX2_ROLE_TARGET && s->type == V2_SERVICE_UNIX_RENDEZVOUS) {
        v2_unix_clear_pending(a, s);
        return;
    }
    v2_close_fd(&s->pending_fd);
    s->pending_token = 0;
}

static void accept_target_service_client(struct v2_app *a, struct v2_service *s)
{
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            return;
        }
        if (v2_set_nonblock(fd) < 0) {
            close(fd);
            continue;
        }
        v2_unix_accept_pending(a, s, fd);
    }
}

static void service_handle_open_received(struct v2_app *a, struct lmux2_stream *st)
{
    struct v2_service *s = v2_service_find_id(a->services, st->service_id);
    if (!s) {
        log_msg(a, 1, "lmux2 STREAM_OPEN received stream=%llu service=%llu refused=no-service",
                (unsigned long long)st->id, (unsigned long long)st->service_id);
        (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_REFUSED);
        return;
    }
    log_msg(a, 1, "lmux2 STREAM_OPEN received stream=%llu service=%s token=%llu",
            (unsigned long long)st->id, s->name, (unsigned long long)st->open_token);
    if (a->role == LMUX2_ROLE_TARGET && s->type == V2_SERVICE_EXEC) {
        if (v2_exec_handle_stream_open(a, &a->conn, s, st)) {
            a->stream_open_count++;
            log_msg(a, 1, "lmux2 exec stream open handled stream=%llu service=%s",
                    (unsigned long long)st->id, s->name);
            return;
        }
    }
    if (a->role == LMUX2_ROLE_TARGET && s->type == V2_SERVICE_PTY) {
        if (st->open_token || s->active_stream_id || s->pty_master_fd < 0) {
            (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return;
        }
        if (lmux2_accept_stream(&a->conn, st->id) < 0)
            return;
        a->stream_open_count++;
        if (v2_pty_attach_stream(s, st->id) < 0) {
            (void)lmux2_close_stream(&a->conn, st->id);
            a->stream_close_count++;
            log_msg(a, 1, "lmux2 STREAM_CLOSE sent stream=%llu service=%s pty-reset-failed",
                    (unsigned long long)st->id, s->name);
            return;
        }
        log_msg(a, 1, "lmux2 STREAM_OPEN_OK sent stream=%llu service=%s pty-bound child=%ld",
                (unsigned long long)st->id, s->name, (long)s->child_pid);
        log_msg(a, 1, "lmux2 target attached pty stream=%llu service=%s",
                (unsigned long long)st->id, s->name);
        return;
    }
    if (a->role == LMUX2_ROLE_TARGET && s->type == V2_SERVICE_FILE) {
        if (st->open_token || s->active_stream_id) {
            (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return;
        }
        v2_file_session_reset(&s->file);
        if (lmux2_accept_stream(&a->conn, st->id) < 0)
            return;
        s->active_stream_id = st->id;
        s->last_stream_id = st->id;
        a->stream_open_count++;
        log_msg(a, 1, "lmux2 STREAM_OPEN_OK sent stream=%llu service=%s file-bound",
                (unsigned long long)st->id, s->name);
        log_msg(a, 1, "lmux2 target attached file stream=%llu service=%s",
                (unsigned long long)st->id, s->name);
        return;
    }
    if (a->role == LMUX2_ROLE_TARGET && st->open_token) {
        if (s->pending_fd < 0 || s->pending_token != st->open_token) {
            (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return;
        }
        {
            uint64_t token = s->pending_token;
            v2_service_bind_fd_to_stream(s, st->id, s->pending_fd);
            s->pending_fd = -1;
            s->pending_token = 0;
            if (token)
                (void)v2_runtime_queue_service_event(a, s, V2_EVENT_PENDING_CLEARED, token);
        }
    }
    if (lmux2_accept_stream(&a->conn, st->id) < 0)
        return;
    s->last_stream_id = st->id;
    a->stream_open_count++;
    log_msg(a, 1, "lmux2 STREAM_OPEN_OK sent stream=%llu service=%s",
            (unsigned long long)st->id, s->name);
    if (a->role == LMUX2_ROLE_TARGET && !st->open_token)
        log_msg(a, 1, "lmux2 target accepted host-first stream=%llu service=%s",
                (unsigned long long)st->id, s->name);
}

static void process_open_received(struct v2_app *a)
{
    int i;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++)
        if (a->conn.streams[i].state == LMUX2_STREAM_OPEN_RECEIVED)
            service_handle_open_received(a, &a->conn.streams[i]);
}

static void deliver_stream_rx(struct v2_app *a, struct v2_service *s, struct v2_binding *b)
{
    uint8_t buf[256];
    size_t n;
    if (b->fd < 0)
        return;
    while ((n = lmux2_stream_read(&a->conn, b->stream_id, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(b->fd, buf + off, n - off);
            if (w > 0) {
                off += (size_t)w;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            close_binding(a, s, b, 1);
            return;
        }
    }
}

static void collect_endpoint_tx(struct v2_app *a, struct v2_service *s, struct v2_binding *b)
{
    uint8_t buf[256];
    size_t space;
    if (b->fd < 0)
        return;
    if (!stream_can_send_endpoint_data(a, b->stream_id))
        return;
    for (;;) {
        space = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(&a->conn, b->stream_id);
        if (space == 0)
            return;
        if (space > sizeof(buf))
            space = sizeof(buf);
        ssize_t r = read(b->fd, buf, space);
        if (r > 0) {
            size_t w = lmux2_stream_write(&a->conn, b->stream_id, buf, (size_t)r);
            if (w < (size_t)r)
                return;
            continue;
        }
        if (r == 0) {
            close_binding(a, s, b, 1);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        close_binding(a, s, b, 1);
        return;
    }
}

static void close_pty_attachment(struct v2_app *a, struct v2_service *s)
{
    if (s->active_stream_id) {
        uint64_t stream_id = s->active_stream_id;
        (void)lmux2_close_stream(&a->conn, s->active_stream_id);
        a->stream_close_count++;
        s->active_stream_id = 0;
        log_msg(a, 1, "lmux2 STREAM_CLOSE sent service=%s stream=%llu pty-detached child=%ld",
                s->name, (unsigned long long)stream_id, (long)s->child_pid);
    }
}

static void ack_unbound_remote_close(struct v2_app *a, uint64_t stream_id)
{
    struct lmux2_stream *st = lmux2_find_stream(&a->conn, stream_id);
    int has_binding = a->role == LMUX2_ROLE_HOST ?
        v2_host_stream_has_endpoint_binding(a->host.services, stream_id) :
        v2_stream_has_endpoint_binding(a->services, stream_id);
    if (!st || st->state != LMUX2_STREAM_REMOTE_CLOSED || has_binding)
        return;
    if (lmux2_close_stream(&a->conn, stream_id) == 0) {
        a->stream_close_count++;
        log_msg(a, 1, "lmux2 STREAM_CLOSE sent stream=%llu unbound-ack",
                (unsigned long long)stream_id);
    }
}

static void cleanup_terminal_bindings(struct v2_app *a)
{
    int i, j;
    if (a->role == LMUX2_ROLE_HOST) {
        v2_host_cleanup_terminal_bindings(a, &a->host, &a->conn);
        return;
    }
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
        if (!s->used)
            continue;
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            struct v2_binding *b = &s->bindings[j];
            struct lmux2_stream *st;
            if (b->fd < 0)
                continue;
            st = lmux2_find_stream(&a->conn, b->stream_id);
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED)
                close_binding(a, s, b, st && st->state == LMUX2_STREAM_REMOTE_CLOSED);
        }
        if (s->type == V2_SERVICE_PTY && s->active_stream_id) {
            const struct lmux2_stream *st = lmux2_find_stream_const(&a->conn, s->active_stream_id);
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED) {
                if (st && st->state == LMUX2_STREAM_REMOTE_CLOSED)
                    (void)lmux2_close_stream(&a->conn, s->active_stream_id);
                s->active_stream_id = 0;
            }
        }
        if (s->type == V2_SERVICE_FILE && s->active_stream_id) {
            const struct lmux2_stream *st = lmux2_find_stream_const(&a->conn, s->active_stream_id);
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED) {
                if (st && st->state == LMUX2_STREAM_REMOTE_CLOSED)
                    (void)lmux2_close_stream(&a->conn, s->active_stream_id);
                s->active_stream_id = 0;
                v2_file_session_reset(&s->file);
            }
        }
    }
}

static void invalidate_connection_runtime(struct v2_app *a)
{
    int i, j;
    if (a->role == LMUX2_ROLE_HOST) {
        v2_host_structured_on_outer_down(&a->host);
        v2_host_invalidate_remote(&a->host);
    } else {
        for (i = 0; i < V2_MAX_SERVICES; i++) {
            struct v2_service *s = &a->services[i];
            if (!s->used)
                continue;
            for (j = 0; j < V2_MAX_BINDINGS; j++)
                close_binding(a, s, &s->bindings[j], 0);
            s->active_stream_id = 0;
            if (s->type == V2_SERVICE_FILE)
                v2_file_session_reset(&s->file);
            if (s->type == V2_SERVICE_EXEC)
                v2_exec_cleanup_connection(a, s, &a->conn);
        }
    }
    a->factories_reported = 0;
    a->peer_restart_converging = 0;
    log_msg(a, 1, "lmux2 remote factory/service cache invalidated");
}

static void reset_connection_streams(struct v2_app *a)
{
    invalidate_connection_runtime(a);
    a->out_len = a->out_pos = 0;
    lmux2_conn_begin_physical(&a->conn, make_conn_id());
    a->connection_recreated_count++;
    log_msg(a, 1, "lmux2 connection reset/recreated local_conn=%llu",
            (unsigned long long)a->conn.local_conn_id);
    (void)lmux2_conn_start(&a->conn);
    log_msg(a, 1, "lmux2 HELLO sent local_conn=%llu",
            (unsigned long long)a->conn.local_conn_id);
}

static void retire_connection_streams(struct v2_app *a)
{
    invalidate_connection_runtime(a);
    a->out_len = a->out_pos = 0;
    a->enc_in_len = 0;
    a->discard_until_delim = 0;
    lmux2_conn_begin_physical(&a->conn, make_conn_id());
    log_msg(a, 1, "lmux2 session retired local_conn=%llu",
            (unsigned long long)a->conn.local_conn_id);
}

static int send_supervisor_msg(struct v2_app *a, uint8_t type, uint64_t epoch)
{
    uint8_t buf[LMUX_SUP_MSG_LEN];
    struct lmux_supervisor_msg msg;
    ssize_t w;
    msg.type = type;
    msg.link_epoch = epoch;
    if (lmux_supervisor_msg_encode(buf, &msg) < 0)
        return -1;
    w = send(a->supervisor_ctrl_fd, buf, sizeof(buf), 0);
    return w == (ssize_t)sizeof(buf) ? 0 : -1;
}

static void worker_link_down(struct v2_app *a, uint64_t epoch)
{
    a->worker_link_active = 0;
    a->worker_link_epoch = epoch;
    log_msg(a, 1, "lmux2 supervisor LINK_DOWN epoch=%llu",
            (unsigned long long)epoch);
    retire_connection_streams(a);
    if (lmux_carrier_fd(active_carrier(a)) >= 0)
        (void)lmux_supervisor_drain_fd(lmux_carrier_fd(active_carrier(a)));
    (void)send_supervisor_msg(a, LMUX_SUP_LINK_DOWN_ACK, epoch);
    log_msg(a, 1, "lmux2 supervisor LINK_DOWN_ACK epoch=%llu",
            (unsigned long long)epoch);
}

static void worker_link_up(struct v2_app *a, uint64_t epoch)
{
    a->worker_link_epoch = epoch;
    a->worker_link_active = 0;
    a->worker_link_up_flush = 1;
    a->worker_link_up_ack_sent = 0;
    a->worker_link_up_delay_done = 0;
    log_msg(a, 1, "lmux2 supervisor LINK_UP epoch=%llu",
            (unsigned long long)epoch);
    reset_connection_streams(a);
    build_next_output(a);
    flush_carrier(a);
    worker_maybe_finish_link_up(a);
}

static void worker_maybe_finish_link_up(struct v2_app *a)
{
    if (!a->worker_mode || !a->worker_link_up_flush || a->worker_link_up_ack_sent)
        return;
    build_next_output(a);
    flush_carrier(a);
    if (a->out_len == 0 && a->conn.outq_len == 0) {
        if (!a->worker_link_up_delay_done) {
            const char *delay = getenv("LMUX_SUP_TEST_UP_ACK_DELAY_MS");
            a->worker_link_up_delay_done = 1;
            if (delay && *delay) {
                long ms = strtol(delay, NULL, 10);
                if (ms > 0 && ms < 5000)
                    usleep((useconds_t)ms * 1000u);
            }
        }
        a->worker_link_up_ack_sent = 1;
        a->worker_link_up_flush = 0;
        a->worker_link_active = 1;
        (void)send_supervisor_msg(a, LMUX_SUP_LINK_UP_ACK, a->worker_link_epoch);
        log_msg(a, 1, "lmux2 supervisor LINK_UP_ACK epoch=%llu hello-committed",
                (unsigned long long)a->worker_link_epoch);
    }
}

static void handle_supervisor_control(struct v2_app *a)
{
    uint8_t buf[LMUX_SUP_MSG_LEN];
    ssize_t r;
    struct lmux_supervisor_msg msg;
    for (;;) {
        r = recv(a->supervisor_ctrl_fd, buf, sizeof(buf), 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (r != (ssize_t)sizeof(buf) ||
            lmux_supervisor_msg_decode(buf, &msg) < 0) {
            log_msg(a, 1, "lmux2 malformed supervisor sideband");
            a->stop = 1;
            return;
        }
        if (msg.type == LMUX_SUP_LINK_DOWN)
            worker_link_down(a, msg.link_epoch);
        else if (msg.type == LMUX_SUP_LINK_UP)
            worker_link_up(a, msg.link_epoch);
        else if (msg.type == LMUX_SUP_SHUTDOWN)
            a->stop = 1;
        else
            log_msg(a, 1, "lmux2 unexpected supervisor message type=%u", msg.type);
    }
}

static int step_file_services(struct v2_app *a)
{
    int i, progress = 0;
    if (a->role != LMUX2_ROLE_TARGET)
        return 0;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
        if (s->used && s->type == V2_SERVICE_FILE && s->active_stream_id)
            progress |= v2_file_service_step(a, &a->conn, a->services, s);
    }
    return progress;
}

static int step_exec_services(struct v2_app *a)
{
    int i, progress = 0;
    if (a->role != LMUX2_ROLE_TARGET)
        return 0;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
        if (s->used && s->type == V2_SERVICE_EXEC)
            progress |= v2_exec_service_step(a, &a->conn, s);
    }
    return progress;
}

static int frame_is_peer_restart(struct v2_app *a, const struct lmux2_frame *fr,
                                 uint64_t *old_peer, uint64_t *new_peer)
{
    struct lmux2_hello h;
    if (fr->type != LMUX2_T_HELLO || a->conn.state != LMUX2_CONN_ESTABLISHED)
        return 0;
    if (fr->stream_id != 0 || fr->value != 0 ||
        lmux2_parse_hello_payload(fr->payload, fr->payload_len, &h) < 0)
        return 0;
    if (h.role == a->role || h.conn_id == a->conn.peer_conn_id)
        return 0;
    if (old_peer)
        *old_peer = a->conn.peer_conn_id;
    if (new_peer)
        *new_peer = h.conn_id;
    return 1;
}

static void process_frame(struct v2_app *a, const struct lmux2_frame *fr)
{
    enum lmux2_conn_state before = a->conn.state;
    uint64_t old_peer = 0, new_peer = 0, old_local = a->conn.local_conn_id;
    int pre_established_drop = before != LMUX2_CONN_ESTABLISHED &&
        fr->type != LMUX2_T_HELLO;
    if (frame_is_peer_restart(a, fr, &old_peer, &new_peer)) {
        if (a->peer_restart_converging) {
            log_msg(a, 1, "lmux2 peer restart convergence old_peer=%llu new_peer=%llu",
                    (unsigned long long)old_peer, (unsigned long long)new_peer);
            a->conn.peer_conn_id = new_peer;
            a->peer_restart_converging = 0;
        } else {
            log_msg(a, 1, "lmux2 peer restart detected old_peer=%llu new_peer=%llu",
                    (unsigned long long)old_peer, (unsigned long long)new_peer);
            log_msg(a, 1, "lmux2 old session retired local=%llu peer=%llu",
                    (unsigned long long)old_local, (unsigned long long)old_peer);
            a->peer_restart_count++;
            reset_connection_streams(a);
            a->peer_restart_converging = 1;
            before = a->conn.state;
            pre_established_drop = 0;
        }
    }
    if (lmux2_handle_frame(&a->conn, fr) < 0)
        return;
    if (pre_established_drop) {
        log_msg(a, 1, "lmux2 pre-HELLO frame dropped type=%u stream=%llu",
                fr->type, (unsigned long long)fr->stream_id);
        cleanup_terminal_bindings(a);
        return;
    }
    cleanup_terminal_bindings(a);
    if (a->conn.state == LMUX2_CONN_ESTABLISHED &&
        before != LMUX2_CONN_ESTABLISHED) {
        a->connection_established_count++;
        log_msg(a, 1, "lmux2 connection established local_conn=%llu peer_conn=%llu",
                (unsigned long long)a->conn.local_conn_id,
                (unsigned long long)a->conn.peer_conn_id);
    }
    if (a->conn.state == LMUX2_CONN_ESTABLISHED &&
        before != LMUX2_CONN_ESTABLISHED && a->role == LMUX2_ROLE_TARGET &&
        !a->factories_reported) {
        (void)queue_factory_report(a);
        a->factories_reported = 1;
    }
    if (fr->type == LMUX2_T_CONTROL && a->conn.state == LMUX2_CONN_ESTABLISHED)
        handle_control(a, fr);
    else if (fr->type == LMUX2_T_STREAM_OPEN_OK)
        log_msg(a, 1, "lmux2 STREAM_OPEN_OK received stream=%llu",
                (unsigned long long)fr->stream_id);
    else if (fr->type == LMUX2_T_STREAM_OPEN_ERROR)
        log_msg(a, 1, "lmux2 STREAM_OPEN_ERROR received stream=%llu code=%llu",
                (unsigned long long)fr->stream_id, (unsigned long long)fr->value);
    else if (fr->type == LMUX2_T_STREAM_CLOSE) {
        log_msg(a, 1, "lmux2 STREAM_CLOSE received stream=%llu",
                (unsigned long long)fr->stream_id);
        ack_unbound_remote_close(a, fr->stream_id);
    } else if (fr->type == LMUX2_T_STREAM_RESET)
        log_msg(a, 1, "lmux2 STREAM_RESET received stream=%llu code=%llu",
                (unsigned long long)fr->stream_id, (unsigned long long)fr->value);
    if (fr->type != LMUX2_T_HELLO && a->conn.state == LMUX2_CONN_ESTABLISHED)
        a->peer_restart_converging = 0;
    process_open_received(a);
}

static void feed_lmux_bytes(struct v2_app *a, const uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] == 0) {
            if (!a->discard_until_delim && a->enc_in_len) {
                struct lmux2_frame fr;
                if (lmux2_decode_frame(a->enc_in, a->enc_in_len, &fr) == 0)
                    process_frame(a, &fr);
                else
                    a->conn.state = LMUX2_CONN_ERROR;
                if (a->conn.state == LMUX2_CONN_ERROR) {
                    carrier_lost(a);
                    return;
                }
            }
            a->enc_in_len = 0;
            a->discard_until_delim = 0;
        } else if (!a->discard_until_delim) {
            if (a->enc_in_len >= sizeof(a->enc_in)) {
                a->discard_until_delim = 1;
                a->enc_in_len = 0;
                a->conn.state = LMUX2_CONN_ERROR;
            } else {
                a->enc_in[a->enc_in_len++] = buf[i];
            }
        }
    }
}

static void feed_carrier(struct v2_app *a)
{
    uint8_t buf[256];
    for (;;) {
        struct lmux_carrier_io io = lmux_carrier_read(active_carrier(a), buf, sizeof(buf));
        if (io.status == LMUX_CARRIER_IO_PROGRESS) {
            feed_lmux_bytes(a, buf, (size_t)io.n);
            continue;
        }
        if (io.status == LMUX_CARRIER_IO_LOST || io.status == LMUX_CARRIER_IO_FATAL) {
            carrier_lost(a);
            return;
        }
        return;
    }
}

static void build_next_output(struct v2_app *a)
{
    struct lmux2_frame fr;
    if (a->out_len != a->out_pos)
        return;
    a->out_len = a->out_pos = 0;
    if (lmux2_next_frame(&a->conn, &fr) > 0)
        (void)lmux2_encode_frame(&fr, a->out, sizeof(a->out), &a->out_len);
}

static void flush_carrier(struct v2_app *a)
{
    if (lmux_carrier_fd(active_carrier(a)) < 0)
        return;
    while (a->out_pos < a->out_len) {
        struct lmux_carrier_io io = lmux_carrier_write(active_carrier(a),
                                                       a->out + a->out_pos,
                                                       a->out_len - a->out_pos);
        if (io.status == LMUX_CARRIER_IO_PROGRESS) {
            a->out_pos += (size_t)io.n;
            if (a->out_pos == a->out_len)
                a->out_pos = a->out_len = 0;
            continue;
        }
        if (io.status == LMUX_CARRIER_IO_WOULD_BLOCK)
            return;
        carrier_lost(a);
        return;
    }
}

static void outer_session_down(struct v2_app *a)
{
    log_msg(a, 1, "outer session down reason=%s",
            a->outer.last_error[0] ? a->outer.last_error : "-");
    if (a->role == LMUX2_ROLE_HOST)
        v2_host_control_outer_down(&a->host);
    retire_connection_streams(a);
    if (lmux_carrier_fd(active_carrier(a)) >= 0)
        begin_outer(a);
}

static void outer_peer_restart(struct v2_app *a)
{
    uint64_t old_local = a->outer.local_conn_id;
    uint64_t old_peer = a->outer.peer_conn_id;
    uint64_t new_peer = a->outer.pending_peer_hello.conn_id;
    uint64_t new_local = make_conn_id();
    unsigned events = 0;

    log_msg(a, 1, "outer peer restart detected old_peer=%llu new_peer=%llu",
            (unsigned long long)old_peer, (unsigned long long)new_peer);
    log_msg(a, 1, "outer old session retired local=%llu peer=%llu",
            (unsigned long long)old_local, (unsigned long long)old_peer);
    if (a->role == LMUX2_ROLE_HOST)
        v2_host_control_outer_down(&a->host);
    retire_connection_streams(a);
    if (lmux_link_endpoint_restart_from_pending(&a->outer, new_local, &events) < 0) {
        outer_session_down(a);
        return;
    }
    log_msg(a, 1, "outer new incarnation local=%llu",
            (unsigned long long)a->outer.local_conn_id);
    log_msg(a, 1, "outer HELLO sent local=%llu seq=0",
            (unsigned long long)a->outer.local_conn_id);
    log_msg(a, 1, "outer peer HELLO accepted peer=%llu",
            (unsigned long long)a->outer.peer_conn_id);
    if (events & LMUX_LINK_EV_ESTABLISHED) {
        log_msg(a, 1, "outer established local_conn=%llu peer_conn=%llu",
                (unsigned long long)a->outer.local_conn_id,
                (unsigned long long)a->outer.peer_conn_id);
        reset_connection_streams(a);
    }
}

static void handle_outer_events(struct v2_app *a, unsigned events)
{
    if (events & LMUX_LINK_EV_PEER_RESTART) {
        outer_peer_restart(a);
        return;
    }
    if (events & LMUX_LINK_EV_DOWN) {
        outer_session_down(a);
        return;
    }
    if (events & LMUX_LINK_EV_PEER_HELLO)
        log_msg(a, 1, "outer peer HELLO received peer_conn=%llu",
                (unsigned long long)a->outer.peer_conn_id);
    if (events & LMUX_LINK_EV_HELLO_ACK_QUEUED)
        log_msg(a, 1, "outer HELLO_ACK queued local_conn=%llu ack_peer=%llu",
                (unsigned long long)a->outer.local_conn_id,
                (unsigned long long)a->outer.pending_ack_peer_conn_id);
    if (events & LMUX_LINK_EV_HELLO_ACK_COMMITTED)
        log_msg(a, 1, "outer HELLO_ACK committed local_conn=%llu peer_conn=%llu",
                (unsigned long long)a->outer.local_conn_id,
                (unsigned long long)a->outer.peer_conn_id);
    if (events & LMUX_LINK_EV_PEER_HELLO_ACK)
        log_msg(a, 1, "outer peer HELLO_ACK received peer_conn=%llu ack_local=%llu",
                (unsigned long long)a->outer.peer_conn_id,
                (unsigned long long)a->outer.local_conn_id);
    if (events & LMUX_LINK_EV_ESTABLISHED) {
        log_msg(a, 1, "outer established local_conn=%llu peer_conn=%llu",
                (unsigned long long)a->outer.local_conn_id,
                (unsigned long long)a->outer.peer_conn_id);
        reset_connection_streams(a);
    }
}

static void drain_outer_transport_rx(struct v2_app *a)
{
    uint8_t buf[256];
    size_t n;
    while ((n = v2_host_link_pull_lmux(&a->outer, buf, sizeof(buf))) > 0)
        feed_lmux_bytes(a, buf, n);
}

static void drain_outer_boot_frames(struct v2_app *a)
{
    uint8_t type;
    uint8_t payload[LMUX_LINK_MAX_PAYLOAD];
    uint32_t len;
    while (lmux_link_boot_read_frame(&a->outer, &type, payload,
                                     sizeof(payload), &len) == 0) {
        if (type == LMUX_LINK_T_BOOT_RESPONSE) {
            struct lmux_boot_response resp;
            if (lmux_boot_parse_response(payload, len, &resp) == 0)
                v2_host_control_on_boot_response(a, &a->host, resp.opcode,
                                                 resp.status, resp.request_id,
                                                 resp.body, resp.body_len);
            continue;
        }
        log_msg(a, 1, "outer ignored BOOT frame type=%u", type);
    }
}

static const char *carrier_switch_phase_name(enum carrier_switch_phase phase)
{
    switch (phase) {
    case CARRIER_SWITCH_IDLE: return "idle";
    case CARRIER_SWITCH_QUERYING: return "querying";
    case CARRIER_SWITCH_WAIT_QUERY: return "wait-query";
    case CARRIER_SWITCH_PREPARING: return "preparing";
    case CARRIER_SWITCH_WAIT_READY: return "wait-ready";
    case CARRIER_SWITCH_SENDING_COMMIT: return "sending-commit";
    case CARRIER_SWITCH_RESOLVING: return "resolving";
    case CARRIER_SWITCH_CONVERGING: return "converging";
    }
    return "unknown";
}

static int carrier_endpoint_index(struct v2_app *a,
                                  struct lmux_carrier_endpoint *ep)
{
    ptrdiff_t index;
    if (!a || !ep)
        return -1;
    index = ep - a->carriers.endpoint;
    return index >= 0 && index < LMUX_CARRIER_MAX_ENDPOINTS ?
        (int)index : -1;
}

static int queue_switch_msg(struct v2_app *a, uint8_t type)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_find(&a->carriers, a->carrier_switch.name);
    struct lmux_lmc1_msg msg;
    int index = carrier_endpoint_index(a, ep);
    if (index < 0 || lmux_carrier_fd(&ep->carrier) < 0) {
        errno = ENODEV;
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.mode = a->carrier_switch.mode;
    msg.generation = a->carrier_switch.observed_generation;
    msg.deadline_ms = LMUX_LMC1_DEFAULT_DEADLINE_MS;
    memcpy(msg.nonce, a->carrier_switch.nonce, sizeof(msg.nonce));
    memcpy(msg.cookie, a->carrier_switch.cookie, sizeof(msg.cookie));
    memcpy(msg.boot_id, a->carrier_switch.boot_id, sizeof(msg.boot_id));
    strcpy(msg.name, a->carrier_switch.name);
    if (a->carrier_switch.mode == LMUX_CARRIER_MODE_DIRTY) {
        if (lmux_lmc1_dirty_queue(&a->lmc_dirty_io[index], &msg) < 0)
            return -1;
    } else if (lmux_lmc1_raw_queue(&a->lmc_io[index], &msg) < 0) {
        return -1;
    }
    log_msg(a, 1, "LMC1 %s queued endpoint=%s generation=%llu",
            lmux_lmc1_type_name(type), ep->name,
            (unsigned long long)msg.generation);
    return 0;
}

static void freeze_old_outer_for_commit(struct v2_app *a)
{
    if (a->carrier_switch.old_outer_frozen)
        return;
    a->carrier_switch.old_outer_frozen = 1;
    v2_host_control_outer_down(&a->host);
    lmux_link_endpoint_down(&a->outer);
    retire_connection_streams(a);
    a->out_len = a->out_pos = 0;
    a->enc_in_len = 0;
    a->discard_until_delim = 0;
    log_msg(a, 0, "carrier switch COMMIT sent; old OUTER frozen");
}

static void finish_carrier_switch(struct v2_app *a, int success,
                                  const char *reason)
{
    char detail[192];
    if (success) {
        a->carrier_committed_but_down = 0;
        a->carrier_last_error[0] = 0;
        snprintf(detail, sizeof(detail),
                 "OK CARRIER active=%s mode=%s generation=%llu\n",
                 a->carrier_switch.name,
                 lmux_carrier_mode_name(a->carrier_switch.mode),
                 (unsigned long long)a->carriers.generation);
    } else {
        snprintf(a->carrier_last_error, sizeof(a->carrier_last_error), "%s",
                 reason ? reason : "failed");
        if (a->carrier_switch.target_committed)
            a->carrier_committed_but_down = 1;
        snprintf(detail, sizeof(detail),
                 "ERR CARRIER %s%s\n",
                 a->carrier_switch.target_committed ?
                 "committed-but-down " : "",
                 a->carrier_last_error);
    }
    v2_host_control_carrier_result(&a->host, detail);
    a->carrier_switch.phase = CARRIER_SWITCH_IDLE;
}

static void activate_runtime_dirty(struct v2_app *a, uint64_t generation)
{
    uint64_t host_session, target_session;
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_active_endpoint(&a->carriers);
    uint32_t baud = ep && ep->carrier.serial.baud ?
        ep->carrier.serial.baud : 115200;
    lmux_lmc1_dirty_session_ids(a->carrier_switch.boot_id, generation,
                                a->carrier_switch.name,
                                &host_session, &target_session);
    lmux_dirty_init(&a->dirty, host_session, target_session, 0, 0,
                    baud, (uint64_t)now_ms());
    a->dirty_active = 1;
}

static void adopt_committed_carrier(struct v2_app *a, uint64_t generation)
{
    struct lmux_carrier_endpoint *ep;
    freeze_old_outer_for_commit(a);
    if (lmux_carrier_manager_set_active(&a->carriers,
                                        a->carrier_switch.name,
                                        a->carrier_switch.mode,
                                        generation) < 0) {
        finish_carrier_switch(a, 0, "local-carrier-missing");
        return;
    }
    ep = lmux_carrier_manager_active_endpoint(&a->carriers);
    a->carrier_failure_hold_mask &=
        ~(1u << (unsigned)a->carriers.active_index);
    a->carrier_switch.committed_generation = generation;
    a->carrier_switch.target_committed = 1;
    a->carrier_switch.phase = CARRIER_SWITCH_CONVERGING;
    a->carrier_switch.old_outer_frozen = 0;
    if (a->carrier_switch.mode == LMUX_CARRIER_MODE_DIRTY)
        activate_runtime_dirty(a, generation);
    else {
        memset(&a->dirty, 0, sizeof(a->dirty));
        a->dirty_active = 0;
    }
    lmux_lmc1_raw_reset_rx(&a->lmc_io[a->carriers.active_index]);
    lmux_lmc1_dirty_reset_rx(
        &a->lmc_dirty_io[a->carriers.active_index]);
    if (ep && lmux_carrier_fd(&ep->carrier) >= 0)
        begin_outer(a);
    log_msg(a, 0, "carrier authority active=%s mode=%s generation=%llu",
            a->carrier_switch.name,
            lmux_carrier_mode_name(a->carrier_switch.mode),
            (unsigned long long)generation);
}

struct runtime_lmc_context {
    struct v2_app *a;
    unsigned endpoint;
    enum lmux_lmc1_codec codec;
};

static void runtime_lmc_control(void *opaque,
                                const struct lmux_lmc1_msg *msg)
{
    struct runtime_lmc_context *ctx = opaque;
    struct v2_app *a = ctx->a;
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&a->carriers, ctx->endpoint);
    if (!ep || a->carrier_switch.phase == CARRIER_SWITCH_IDLE ||
        !lmux_lmc1_nonce_equal(msg->nonce, a->carrier_switch.nonce))
        return;
    log_msg(a, 1, "LMC1 %s received endpoint=%s generation=%llu status=%s",
            lmux_lmc1_type_name(msg->type), ep->name,
            (unsigned long long)msg->generation,
            lmux_lmc1_status_name(msg->status));
    if (msg->type == LMUX_LMC1_ERROR) {
        if (a->carrier_switch.phase == CARRIER_SWITCH_RESOLVING) {
            a->carrier_switch.next_retry_ms = now_ms();
            return;
        }
        finish_carrier_switch(a, 0, lmux_lmc1_status_name(msg->status));
        return;
    }
    if (msg->type == LMUX_LMC1_QUERY_REPLY) {
        if (a->carrier_switch.phase == CARRIER_SWITCH_WAIT_QUERY ||
            a->carrier_switch.phase == CARRIER_SWITCH_QUERYING) {
            memcpy(a->carrier_switch.boot_id, msg->boot_id,
                   sizeof(a->carrier_switch.boot_id));
            a->carrier_switch.observed_generation = msg->generation;
            if (strcmp(msg->name, a->carrier_switch.name) == 0 &&
                msg->mode == a->carrier_switch.mode) {
                adopt_committed_carrier(a, msg->generation);
            } else {
                a->carrier_switch.phase = CARRIER_SWITCH_PREPARING;
                a->carrier_switch.next_retry_ms = now_ms();
            }
            return;
        }
        if (a->carrier_switch.phase == CARRIER_SWITCH_RESOLVING) {
            if (memcmp(msg->boot_id, a->carrier_switch.boot_id,
                       LMUX_LMC1_BOOT_ID_LEN) != 0) {
                finish_carrier_switch(a, 0, "target-restarted");
            } else if (strcmp(msg->name, a->carrier_switch.name) == 0 &&
                       msg->mode == a->carrier_switch.mode &&
                       msg->generation >
                       a->carrier_switch.observed_generation) {
                adopt_committed_carrier(a, msg->generation);
            } else if (msg->generation ==
                       a->carrier_switch.observed_generation) {
                a->carrier_switch.next_retry_ms = now_ms();
            } else {
                finish_carrier_switch(a, 0, "authority-conflict");
            }
        }
        return;
    }
    if (msg->type == LMUX_LMC1_READY &&
        a->carrier_switch.phase == CARRIER_SWITCH_WAIT_READY) {
        if (msg->generation != a->carrier_switch.observed_generation ||
            memcmp(msg->boot_id, a->carrier_switch.boot_id,
                   LMUX_LMC1_BOOT_ID_LEN) != 0 ||
            strcmp(msg->name, a->carrier_switch.name) != 0 ||
            msg->mode != a->carrier_switch.mode) {
            finish_carrier_switch(a, 0, "invalid-ready");
            return;
        }
        memcpy(a->carrier_switch.cookie, msg->cookie,
               sizeof(a->carrier_switch.cookie));
        if (queue_switch_msg(a, LMUX_LMC1_COMMIT) < 0) {
            finish_carrier_switch(a, 0, "commit-queue");
            return;
        }
        a->carrier_switch.phase = CARRIER_SWITCH_SENDING_COMMIT;
        return;
    }
    if (msg->type == LMUX_LMC1_COMMITTED &&
        (a->carrier_switch.phase == CARRIER_SWITCH_RESOLVING ||
         a->carrier_switch.phase == CARRIER_SWITCH_SENDING_COMMIT)) {
        if (memcmp(msg->boot_id, a->carrier_switch.boot_id,
                   LMUX_LMC1_BOOT_ID_LEN) != 0 ||
            strcmp(msg->name, a->carrier_switch.name) != 0 ||
            msg->generation <= a->carrier_switch.observed_generation) {
            finish_carrier_switch(a, 0, "invalid-committed");
            return;
        }
        adopt_committed_carrier(a, msg->generation);
    }
}

static void runtime_lmc_outer(void *opaque, const uint8_t *encoded,
                              size_t len)
{
    struct runtime_lmc_context *ctx = opaque;
    struct v2_app *a = ctx->a;
    unsigned events = 0;
    uint8_t delim = 0;
    (void)lmux_link_input_bytes(&a->outer, encoded, len, &events);
    (void)lmux_link_input_bytes(&a->outer, &delim, 1, &events);
    handle_outer_events(a, events);
    drain_outer_boot_frames(a);
    drain_outer_transport_rx(a);
}

static int runtime_dirty_deliver(struct v2_app *a)
{
    const uint8_t *data;
    size_t len;
    uint32_t seq;
    int rc;
    while ((rc = lmux_dirty_receive_peek(&a->dirty, &data, &len, &seq)) > 0) {
        unsigned events = 0;
        (void)seq;
        if (lmux_link_input_bytes(&a->outer, data, len, &events) < 0)
            return -1;
        handle_outer_events(a, events);
        drain_outer_boot_frames(a);
        drain_outer_transport_rx(a);
        if (lmux_dirty_receive_consume(&a->dirty) < 0)
            return -1;
    }
    return rc < 0 ? -1 : 0;
}

static void feed_raw_endpoint(struct v2_app *a, unsigned endpoint,
                              int allow_outer)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&a->carriers, endpoint);
    struct runtime_lmc_context ctx = { a, endpoint, LMUX_LMC1_CODEC_RAW };
    uint8_t buf[256];
    unsigned budget = 8;
    if (!ep)
        return;
    while (budget-- > 0) {
        struct lmux_carrier_io r =
            lmux_carrier_read(&ep->carrier, buf, sizeof(buf));
        if (r.status == LMUX_CARRIER_IO_PROGRESS) {
            size_t i;
            int discarded = 0;
            (void)allow_outer;
            /* COMMITTED can make this endpoint authoritative in this same
             * read.  Route records according to current authority, not the
             * state captured before read(2). */
            for (i = 0; i < (size_t)r.n; i++) {
                int current_allow =
                    lmux_carrier_manager_is_active(&a->carriers, ep) &&
                    !(a->carrier_failure_hold_mask & (1u << endpoint)) &&
                    !a->carrier_switch.old_outer_frozen;
                int dirty_mode = current_allow &&
                    ep->mode == LMUX_CARRIER_MODE_DIRTY && a->dirty_active;
                int raw_mode = current_allow && !dirty_mode;
                int raw_boundary = a->lmc_io[endpoint].rx_len == 0 &&
                    !a->lmc_io[endpoint].discard_until_delim;
                int boot_was = lmux_lmc1_dirty_receiving(
                    &a->lmc_dirty_io[endpoint]);
                int boot_feed = !raw_mode || boot_was || raw_boundary;
                int data_was = dirty_mode && a->dirty.parser.in_record;
                if (!current_allow) {
                    ep->standby_rx_bytes++;
                    discarded = 1;
                }
                if (!boot_was && !data_was &&
                    lmux_lmc1_raw_feed(&a->lmc_io[endpoint], buf + i, 1,
                                       current_allow && !dirty_mode,
                                       runtime_lmc_control,
                                       runtime_lmc_outer, &ctx) < 0) {
                    if (current_allow)
                        carrier_lost(a);
                    return;
                }
                /* On active RAW, a bootstrap envelope is serialized between
                 * complete COBS records.  Never search for its ASCII prefix
                 * inside arbitrary encoded OUTER bytes. */
                if (raw_mode && !boot_was && !raw_boundary &&
                    a->lmc_dirty_io[endpoint].prefix_match) {
                    const char *prefix = LMUX_LMC1_DIRTY_PREFIX;
                    unsigned match =
                        a->lmc_dirty_io[endpoint].prefix_match;
                    if (buf[i] == (uint8_t)prefix[match])
                        boot_feed = 1;
                    else
                        lmux_lmc1_dirty_reset_rx(
                            &a->lmc_dirty_io[endpoint]);
                }
                ctx.codec = LMUX_LMC1_CODEC_DIRTY_BOOTSTRAP;
                if (boot_feed && lmux_lmc1_dirty_feed(
                        &a->lmc_dirty_io[endpoint], buf + i, 1,
                        runtime_lmc_control, &ctx) < 0) {
                    if (current_allow)
                        carrier_lost(a);
                    return;
                }
                ctx.codec = LMUX_LMC1_CODEC_RAW;
                if (!boot_was && lmux_lmc1_dirty_receiving(
                        &a->lmc_dirty_io[endpoint]))
                    lmux_lmc1_raw_reset_rx(&a->lmc_io[endpoint]);
                if (dirty_mode) {
                    if (lmux_dirty_feed(&a->dirty, buf + i, 1,
                                        (uint64_t)now_ms()) < 0 ||
                        runtime_dirty_deliver(a) < 0) {
                        carrier_lost(a);
                        return;
                    }
                    if (!data_was && a->dirty.parser.in_record)
                        lmux_lmc1_raw_reset_rx(&a->lmc_io[endpoint]);
                }
            }
            if (discarded)
                ep->standby_discard_events++;
            continue;
        }
        if (r.status == LMUX_CARRIER_IO_LOST ||
            r.status == LMUX_CARRIER_IO_FATAL) {
            if (lmux_carrier_manager_is_active(&a->carriers, ep))
                carrier_lost(a);
            else
                lmux_carrier_mark_lost(&ep->carrier, now_ms());
        }
        return;
    }
}

static void switch_tx_completed(struct v2_app *a, uint8_t type)
{
    if (type == LMUX_LMC1_QUERY &&
        a->carrier_switch.phase == CARRIER_SWITCH_QUERYING)
        a->carrier_switch.phase = CARRIER_SWITCH_WAIT_QUERY;
    else if (type == LMUX_LMC1_PREPARE &&
             a->carrier_switch.phase == CARRIER_SWITCH_PREPARING)
        a->carrier_switch.phase = CARRIER_SWITCH_WAIT_READY;
    else if (type == LMUX_LMC1_COMMIT &&
             a->carrier_switch.phase == CARRIER_SWITCH_SENDING_COMMIT) {
        freeze_old_outer_for_commit(a);
        a->carrier_switch.phase = CARRIER_SWITCH_RESOLVING;
        /* QUERY first resolves a lost COMMITTED; the next attempt repeats COMMIT. */
        a->carrier_switch.resolve_attempt = 1;
        a->carrier_switch.next_retry_ms = now_ms() + 250;
    }
}

static void flush_raw_control(struct v2_app *a, unsigned endpoint)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&a->carriers, endpoint);
    uint8_t completed = 0;
    int rc;
    int use_dirty;
    if (!ep || !lmc_any_output(a, endpoint))
        return;
    if (lmux_carrier_manager_is_active(&a->carriers, ep) &&
        ep->mode == LMUX_CARRIER_MODE_DIRTY && a->dirty_active &&
        a->dirty.output_pos < a->dirty.output_len)
        return;
    use_dirty = lmux_lmc1_dirty_has_output(&a->lmc_dirty_io[endpoint]) &&
        (!lmux_lmc1_raw_has_output(&a->lmc_io[endpoint]) ||
         a->lmc_dirty_io[endpoint].tx_pos != 0);
    rc = use_dirty ?
        lmux_lmc1_dirty_flush(&a->lmc_dirty_io[endpoint], &ep->carrier,
                              &completed) :
        lmux_lmc1_raw_flush(&a->lmc_io[endpoint], &ep->carrier,
                            &completed);
    if (rc < 0) {
        if (lmux_carrier_manager_is_active(&a->carriers, ep))
            carrier_lost(a);
        else
            lmux_carrier_mark_lost(&ep->carrier, now_ms());
        return;
    }
    if (completed) {
        log_msg(a, 1, "LMC1 %s committed endpoint=%s",
                lmux_lmc1_type_name(completed), ep->name);
        switch_tx_completed(a, completed);
    }
}

int v2_runtime_carrier_switch_start(struct v2_app *a, const char *name,
                                    enum lmux_carrier_mode mode,
                                    unsigned timeout_ms,
                                    char *error, size_t error_cap)
{
    struct lmux_carrier_endpoint *candidate, *active;
    if (!a || a->role != LMUX2_ROLE_HOST || !a->outer_enabled) {
        snprintf(error, error_cap, "unavailable");
        return -1;
    }
    if (mode != LMUX_CARRIER_MODE_RAW &&
        mode != LMUX_CARRIER_MODE_DIRTY) {
        snprintf(error, error_cap, "unsupported-mode");
        return -1;
    }
    candidate = lmux_carrier_manager_find(&a->carriers, name);
    if (!candidate) {
        snprintf(error, error_cap, "unknown-carrier");
        return -1;
    }
    if (a->carrier_switch.phase != CARRIER_SWITCH_IDLE) {
        snprintf(error, error_cap, "busy");
        return -1;
    }
    /* An explicit switch is the only operation which resumes a UART that was
     * deliberately released to an independent console consumer. */
    a->carrier_console_hold_mask &=
        ~(1u << (unsigned)(candidate - a->carriers.endpoint));
    active = lmux_carrier_manager_active_endpoint(&a->carriers);
    if (active == candidate && active->mode == mode &&
        a->outer.state == LMUX_LINK_ESTABLISHED &&
        a->conn.state == LMUX2_CONN_ESTABLISHED &&
        v2_host_desired_reconciled(&a->host))
        return 1;
    memset(&a->carrier_switch, 0, sizeof(a->carrier_switch));
    a->carrier_switch.phase = CARRIER_SWITCH_QUERYING;
    strcpy(a->carrier_switch.name, name);
    a->carrier_switch.mode = mode;
    runtime_random_bytes(a->carrier_switch.nonce,
                         sizeof(a->carrier_switch.nonce));
    a->carrier_switch.deadline_ms = now_ms() + timeout_ms;
    a->carrier_switch.next_retry_ms = now_ms();
    a->carrier_switch.pre_local_conn = a->conn.local_conn_id;
    a->carrier_switch.pre_peer_conn = a->conn.peer_conn_id;
    a->carrier_switch.pre_established_count =
        a->connection_established_count;
    a->carrier_committed_but_down = 0;
    a->carrier_last_error[0] = 0;
    return 0;
}

static void carrier_switch_tick(struct v2_app *a, long long now)
{
    struct lmux_carrier_endpoint *ep;
    char failure[96];
    if (a->carrier_switch.phase == CARRIER_SWITCH_IDLE)
        return;
    if (a->carrier_switch.phase == CARRIER_SWITCH_CONVERGING &&
        a->carrier_switch.target_committed && a->dirty_active &&
        active_carrier_mode(a) == LMUX_CARRIER_MODE_DIRTY &&
        lmux_dirty_failed(&a->dirty)) {
        snprintf(failure, sizeof(failure), "dirty-%s",
                 lmux_dirty_fail_reason_name(a->dirty.fail_reason));
        finish_carrier_switch(a, 0, failure);
        return;
    }
    if (now > a->carrier_switch.deadline_ms) {
        finish_carrier_switch(a, 0,
            a->carrier_switch.target_committed ? "convergence-timeout" :
            (a->carrier_switch.old_outer_frozen ? "ambiguous-commit" :
             "timeout"));
        return;
    }
    ep = lmux_carrier_manager_find(&a->carriers, a->carrier_switch.name);
    if (!ep) {
        finish_carrier_switch(a, 0, "local-carrier-missing");
        return;
    }
    if (a->carrier_switch.phase == CARRIER_SWITCH_CONVERGING &&
        a->carrier_switch.target_committed &&
        lmux_carrier_fd(&ep->carrier) < 0) {
        finish_carrier_switch(a, 0, "candidate-lost");
        return;
    }
    if (a->carrier_switch.phase == CARRIER_SWITCH_CONVERGING) {
        if (a->outer.state == LMUX_LINK_ESTABLISHED &&
            a->conn.state == LMUX2_CONN_ESTABLISHED &&
            a->connection_established_count >
                a->carrier_switch.pre_established_count &&
            v2_host_desired_reconciled(&a->host))
            finish_carrier_switch(a, 1, NULL);
        return;
    }
    if (now < a->carrier_switch.next_retry_ms ||
        lmux_carrier_fd(&ep->carrier) < 0)
        return;
    if (a->carrier_switch.phase == CARRIER_SWITCH_QUERYING ||
        a->carrier_switch.phase == CARRIER_SWITCH_WAIT_QUERY) {
        if (queue_switch_msg(a, LMUX_LMC1_QUERY) == 0) {
            a->carrier_switch.phase = CARRIER_SWITCH_QUERYING;
            a->carrier_switch.next_retry_ms = now + 500;
        }
    } else if (a->carrier_switch.phase == CARRIER_SWITCH_PREPARING ||
               a->carrier_switch.phase == CARRIER_SWITCH_WAIT_READY) {
        if (queue_switch_msg(a, LMUX_LMC1_PREPARE) == 0) {
            a->carrier_switch.phase = CARRIER_SWITCH_PREPARING;
            a->carrier_switch.next_retry_ms = now + 500;
        }
    } else if (a->carrier_switch.phase == CARRIER_SWITCH_RESOLVING) {
        uint8_t type = (a->carrier_switch.resolve_attempt++ & 1) ?
            LMUX_LMC1_QUERY : LMUX_LMC1_COMMIT;
        if (queue_switch_msg(a, type) == 0)
            a->carrier_switch.next_retry_ms = now + 500;
    }
}

static void feed_outer_carrier(struct v2_app *a)
{
    feed_raw_endpoint(a, (unsigned)a->carriers.active_index,
                      !a->carrier_switch.old_outer_frozen);
}

static void pump_lmux_output_to_outer(struct v2_app *a)
{
    while (a->out_pos < a->out_len &&
           lmux_link_transport_tx_space(&a->outer) > 0) {
        size_t n = v2_host_link_push_lmux(&a->outer, a->out + a->out_pos,
                                          a->out_len - a->out_pos);
        if (n == 0)
            break;
        a->out_pos += n;
        if (a->out_pos == a->out_len)
            a->out_pos = a->out_len = 0;
    }
}

static void flush_outer_carrier(struct v2_app *a)
{
    if (active_carrier_mode(a) == LMUX_CARRIER_MODE_DIRTY) {
        while (lmux_carrier_fd(active_carrier(a)) >= 0) {
            const uint8_t *wire;
            size_t wire_len;
            struct lmux_carrier_io io;
            unsigned events = 0;
            int peek;
            if (!a->dirty_active || lmux_dirty_failed(&a->dirty)) {
                carrier_lost(a);
                return;
            }
            if (a->dirty.output_pos == a->dirty.output_len &&
                lmc_any_output(a, (unsigned)a->carriers.active_index))
                return;
            if (a->dirty.output_pos == a->dirty.output_len) {
                if (!lmux_link_has_pending_output(&a->outer))
                    (void)lmux_link_build_output(&a->outer);
                if (lmux_link_has_pending_output(&a->outer)) {
                    size_t n = lmux_link_output_len(&a->outer);
                    if (n > LMUX_DIRTY_PACKET_MAX)
                        n = LMUX_DIRTY_PACKET_MAX;
                    if (lmux_dirty_submit(&a->dirty,
                            lmux_link_output_ptr(&a->outer), n,
                            (uint64_t)now_ms(), NULL) == 0) {
                        lmux_link_output_consume_events(&a->outer, n, &events);
                        handle_outer_events(a, events);
                    } else if (errno != EAGAIN) {
                        carrier_lost(a);
                        return;
                    }
                }
            }
            peek = lmux_dirty_output_peek(&a->dirty, (uint64_t)now_ms(),
                                          &wire, &wire_len);
            if (peek < 0) {
                carrier_lost(a);
                return;
            }
            if (peek == 0)
                return;
            io = lmux_carrier_write(active_carrier(a), wire, wire_len);
            if (io.status == LMUX_CARRIER_IO_PROGRESS) {
                if (lmux_dirty_output_consume(&a->dirty,
                                               (size_t)io.n) < 0) {
                    carrier_lost(a);
                    return;
                }
                continue;
            }
            if (io.status == LMUX_CARRIER_IO_WOULD_BLOCK)
                return;
            carrier_lost(a);
            return;
        }
        return;
    }
    while (lmux_carrier_fd(active_carrier(a)) >= 0) {
        struct lmux_carrier_io io;
        unsigned active = (unsigned)a->carriers.active_index;
        if (!lmux_link_has_pending_output(&a->outer) &&
            active < LMUX_CARRIER_MAX_ENDPOINTS &&
            lmc_any_output(a, active))
            return;
        if (!lmux_link_has_pending_output(&a->outer))
            (void)lmux_link_build_output(&a->outer);
        if (!lmux_link_has_pending_output(&a->outer))
            return;
        io = lmux_carrier_write(active_carrier(a),
                                lmux_link_output_ptr(&a->outer),
                                lmux_link_output_len(&a->outer));
        if (io.status == LMUX_CARRIER_IO_PROGRESS) {
            unsigned events = 0;
            lmux_link_output_consume_events(&a->outer, (size_t)io.n, &events);
            handle_outer_events(a, events);
            continue;
        }
        if (io.status == LMUX_CARRIER_IO_WOULD_BLOCK)
            return;
        carrier_lost(a);
        return;
    }
}

static int add_poll(struct pollfd *pfds, int *kind, int *svc, int *bind,
                    nfds_t *n, int fd, short events, int k, int s, int b)
{
    if (*n >= 64 || fd < 0)
        return -1;
    pfds[*n].fd = fd;
    pfds[*n].events = events;
    pfds[*n].revents = 0;
    kind[*n] = k;
    svc[*n] = s;
    bind[*n] = b;
    (*n)++;
    return 0;
}

static void fill_host_status(struct v2_app *a, struct v2_host_status *st)
{
    memset(st, 0, sizeof(*st));
    st->role = a->role;
    st->carrier = active_carrier_const(a);
    st->carriers = &a->carriers;
    st->outer = a->outer_enabled ? &a->outer : NULL;
    st->conn = &a->conn;
    st->target_services = a->target_status;
    st->connection_recreated_count = a->connection_recreated_count;
    st->connection_established_count = a->connection_established_count;
    st->peer_restart_count = a->peer_restart_count;
    st->stream_open_count = a->stream_open_count;
    st->stream_close_count = a->stream_close_count;
    st->stream_reset_count = a->stream_reset_count;
    st->carrier_switch_phase =
        carrier_switch_phase_name(a->carrier_switch.phase);
    st->carrier_switch_name = a->carrier_switch.phase == CARRIER_SWITCH_IDLE ?
        "-" : a->carrier_switch.name;
    st->carrier_last_error = a->carrier_last_error[0] ?
        a->carrier_last_error : "-";
    st->carrier_committed_but_down = a->carrier_committed_but_down;
    if (a->carrier_console.active) {
        const struct lmux_carrier_endpoint *ep =
            lmux_carrier_manager_at_const(&a->carriers,
                                           a->carrier_console.endpoint);
        st->carrier_console_state = "sending";
        st->carrier_console_name = ep ? ep->name : "-";
    } else if (a->carrier_console_hold_mask) {
        unsigned i;
        st->carrier_console_state = "released";
        st->carrier_console_name = "-";
        for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++) {
            const struct lmux_carrier_endpoint *ep;
            if (!(a->carrier_console_hold_mask & (1u << i)))
                continue;
            ep = lmux_carrier_manager_at_const(&a->carriers, i);
            st->carrier_console_name = ep ? ep->name : "-";
            break;
        }
    } else {
        st->carrier_console_state = "idle";
        st->carrier_console_name = "-";
    }
    st->carrier_console_hold_mask = a->carrier_console_hold_mask;
    st->carrier_failure_hold_mask = a->carrier_failure_hold_mask;
    st->dirty = a->dirty_active &&
        active_carrier_mode(a) == LMUX_CARRIER_MODE_DIRTY ? &a->dirty : NULL;
}

static void fill_target_service_status(struct v2_app *a)
{
    int i, j;
    memset(a->target_status, 0, sizeof(a->target_status));
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        const struct v2_service *s = &a->services[i];
        struct v2_target_service_status *out = &a->target_status[i];
        if (!s->used)
            continue;
        out->used = 1;
        out->id = s->id;
        out->type = s->type;
        v2_copy_bounded(out->name, sizeof(out->name), s->name);
        if (s->listen_path[0])
            v2_copy_bounded(out->path, sizeof(out->path), s->listen_path);
        else if (s->target_path[0])
            v2_copy_bounded(out->path, sizeof(out->path), s->target_path);
        out->listener_ready = s->listen_fd >= 0;
        out->pending = s->pending_fd >= 0;
        out->pending_token = s->pending_token;
        out->pending_event_token = s->pending_event_token;
        out->active_stream_id = s->active_stream_id;
        out->last_stream_id = s->last_stream_id;
        out->pty_master_ready = s->pty_master_fd >= 0;
        out->child_pid = (long)s->child_pid;
        out->file_active = s->file.state != V2_FILE_IDLE;
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            if (s->bindings[j].fd >= 0) {
                out->bindings[j].used = 1;
                out->bindings[j].stream_id = s->bindings[j].stream_id;
            }
        }
    }
}

static void run_loop(struct v2_app *a)
{
    while (!a->stop) {
        struct pollfd pfds[64];
        int kind[64], svc[64], bind[64];
        nfds_t n = 0, i;
        int endpoint_id;
        long long now = now_ms();
        int timeout = 1000;
        if (!a->worker_mode)
            try_open_carrier(a);
        if (a->role == LMUX2_ROLE_HOST)
            carrier_console_tick(a, now);
        if (a->role == LMUX2_ROLE_HOST && a->outer_enabled)
            carrier_switch_tick(a, now);
        if (a->outer_enabled && a->dirty_active &&
            active_carrier_mode(a) == LMUX_CARRIER_MODE_DIRTY &&
            !(a->carrier_failure_hold_mask &
              (1u << (unsigned)a->carriers.active_index)) &&
            lmux_carrier_fd(active_carrier(a)) >= 0) {
            if (lmux_dirty_tick(&a->dirty, (uint64_t)now) < 0) {
                carrier_lost(a);
                continue;
            }
            if (timeout > 100)
                timeout = 100;
        }
        if (a->role == LMUX2_ROLE_HOST)
            v2_host_attach_pending_clients(a, &a->host, &a->conn);
        if (a->role == LMUX2_ROLE_HOST) {
            struct v2_host_status st;
            v2_host_control_check_timeout(&a->host);
            fill_target_service_status(a);
            fill_host_status(a, &st);
            v2_host_control_tick(a, &a->host, &st);
            v2_host_structured_tick(a, &a->host, &st);
        }
        (void)step_file_services(a);
        (void)step_exec_services(a);
        if (a->outer_enabled && a->outer.state == LMUX_LINK_WAIT_HELLO) {
            int tick = lmux_link_endpoint_tick(&a->outer, now);
            if (tick < 0)
                outer_session_down(a);
            else if (tick > 0 &&
                     (a->outer.hello_retry_count <= 3 ||
                      (a->outer.hello_retry_count &
                       (a->outer.hello_retry_count - 1)) == 0))
                log_msg(a, 1, "outer HELLO retry queued local_conn=%llu",
                        (unsigned long long)a->outer.local_conn_id);
            if (timeout > 250)
                timeout = 250;
        }
        if ((!a->worker_mode || a->worker_link_active || a->worker_link_up_flush) &&
            (!a->outer_enabled || a->outer.state == LMUX_LINK_ESTABLISHED))
            build_next_output(a);
        if (a->outer_enabled) {
            pump_lmux_output_to_outer(a);
            (void)lmux_link_build_output(&a->outer);
        }
        for (endpoint_id = 0; endpoint_id < LMUX_CARRIER_MAX_ENDPOINTS;
             endpoint_id++) {
            struct lmux_carrier_endpoint *ep =
                lmux_carrier_manager_at(&a->carriers,
                                        (unsigned)endpoint_id);
            if (!ep)
                continue;
            struct lmux_carrier *carrier = &ep->carrier;
            int active = lmux_carrier_manager_is_active(&a->carriers, ep);
            int data_active = active &&
                !(a->carrier_failure_hold_mask & (1u << endpoint_id));
            if (lmux_carrier_fd(carrier) >= 0 &&
                (!active || !a->worker_mode || a->worker_link_active ||
                 a->worker_link_up_flush)) {
                short ev = 0;
                if (a->carrier_console.active &&
                    (unsigned)endpoint_id == a->carrier_console.endpoint) {
                    add_poll(pfds, kind, svc, bind, &n,
                             lmux_carrier_fd(carrier), POLLOUT, 13,
                             endpoint_id, -1);
                    continue;
                }
                if (!data_active) {
                    ev = POLLIN;
                } else if (a->outer_enabled) {
                    if (a->carrier_switch.old_outer_frozen ||
                        (lmux_link_can_accept_physical(&a->outer) &&
                        lmux_link_transport_rx_space(&a->outer) > 0 &&
                         lmux_link_boot_rx_space(&a->outer) > 0))
                        ev |= POLLIN;
                    if (lmux_link_wants_physical_write(&a->outer) ||
                        (active && ep->mode == LMUX_CARRIER_MODE_DIRTY &&
                         a->dirty_active &&
                         dirty_has_work(&a->dirty)))
                        ev |= POLLOUT;
                } else {
                    ev = a->worker_link_active || !a->worker_mode ? POLLIN : 0;
                    if (a->out_pos < a->out_len)
                        ev |= POLLOUT;
                }
                if (lmc_any_output(a, (unsigned)endpoint_id))
                    ev |= POLLOUT;
                add_poll(pfds, kind, svc, bind, &n, lmux_carrier_fd(carrier),
                         ev, 1, endpoint_id, -1);
            }
            if (lmux_carrier_listen_fd(carrier) >= 0 &&
                lmux_carrier_fd(carrier) < 0)
                add_poll(pfds, kind, svc, bind, &n,
                         lmux_carrier_listen_fd(carrier), POLLIN, 8,
                         endpoint_id, -1);
        }
        if (a->host.control_listen_fd >= 0)
            add_poll(pfds, kind, svc, bind, &n, a->host.control_listen_fd, POLLIN, 6, -1, -1);
        if (a->host.control_fd >= 0) {
            short ev = v2_host_control_poll_events(&a->host);
            if (ev)
                add_poll(pfds, kind, svc, bind, &n, a->host.control_fd,
                         ev, 7, -1, -1);
        }
        if (a->host.carrier_control_fd >= 0)
            add_poll(pfds, kind, svc, bind, &n,
                     a->host.carrier_control_fd, POLLIN, 14, -1, -1);
        if (a->host.structured_listen_fd >= 0)
            add_poll(pfds, kind, svc, bind, &n, a->host.structured_listen_fd,
                     POLLIN, 11, -1, -1);
        if (a->host.structured_fd >= 0)
            add_poll(pfds, kind, svc, bind, &n, a->host.structured_fd,
                     v2_host_structured_poll_events(&a->host), 12, -1, -1);
        if (a->worker_mode && a->supervisor_ctrl_fd >= 0)
            add_poll(pfds, kind, svc, bind, &n, a->supervisor_ctrl_fd, POLLIN, 9, -1, -1);
        for (i = 0; i < V2_MAX_SERVICES; i++) {
            int j;
            struct v2_host_service *hs;
            struct v2_service *ts;
            if (a->role == LMUX2_ROLE_HOST) {
                hs = &a->host.services[i];
                if (!hs->used)
                    continue;
                if (hs->listen_fd >= 0 &&
                    !((hs->type == V2_SERVICE_PTY || hs->type == V2_SERVICE_FILE) &&
                      v2_host_service_has_active_binding(hs)))
                    add_poll(pfds, kind, svc, bind, &n, hs->listen_fd, POLLIN, 2, (int)i, -1);
                if (hs->pending_fd >= 0)
                    add_poll(pfds, kind, svc, bind, &n, hs->pending_fd, 0, 3, (int)i, -1);
                for (j = 0; j < V2_MAX_BINDINGS; j++) {
                    struct v2_host_binding *b = &hs->bindings[j];
                    short ev;
                    if (b->fd < 0)
                        continue;
                    ev = (short)v2_host_endpoint_poll_events(&a->conn, b);
                    add_poll(pfds, kind, svc, bind, &n, b->fd, ev, 4, (int)i, j);
                }
                continue;
            }
            ts = &a->services[i];
            if (!ts->used)
                continue;
            if (ts->listen_fd >= 0)
                add_poll(pfds, kind, svc, bind, &n, ts->listen_fd, POLLIN, 2, (int)i, -1);
            if (ts->pending_fd >= 0)
                add_poll(pfds, kind, svc, bind, &n, ts->pending_fd, 0, 3, (int)i, -1);
            for (j = 0; j < V2_MAX_BINDINGS; j++) {
                struct v2_binding *b = &ts->bindings[j];
                short ev = 0;
                if (b->fd < 0)
                    continue;
                if (stream_can_send_endpoint_data(a, b->stream_id) &&
                    lmux2_stream_tx_len(&a->conn, b->stream_id) < LMUX2_STREAM_TX_CAP)
                    ev |= POLLIN;
                if (lmux2_stream_rx_len(&a->conn, b->stream_id) > 0)
                    ev |= POLLOUT;
                add_poll(pfds, kind, svc, bind, &n, b->fd, ev, 4, (int)i, j);
            }
            if (ts->type == V2_SERVICE_EXEC) {
                for (j = 0; j < V2_EXEC_MAX_SLOTS; j++) {
                    struct v2_exec_obj *e = &ts->exec.execs[j];
                    short ev;
                    if (!e->used)
                        continue;
                    ev = v2_exec_fd_events(a, &a->conn, ts, j, V2_EXEC_FD_STDIN);
                    if (ev)
                        add_poll(pfds, kind, svc, bind, &n, e->stdin_fd, ev, 10, (int)i,
                                 j * 10 + V2_EXEC_FD_STDIN);
                    ev = v2_exec_fd_events(a, &a->conn, ts, j, V2_EXEC_FD_STDOUT);
                    if (ev)
                        add_poll(pfds, kind, svc, bind, &n, e->stdout_fd, ev, 10, (int)i,
                                 j * 10 + V2_EXEC_FD_STDOUT);
                    ev = v2_exec_fd_events(a, &a->conn, ts, j, V2_EXEC_FD_STDERR);
                    if (ev)
                        add_poll(pfds, kind, svc, bind, &n, e->stderr_fd, ev, 10, (int)i,
                                 j * 10 + V2_EXEC_FD_STDERR);
                    ev = v2_exec_fd_events(a, &a->conn, ts, j, V2_EXEC_FD_ERR);
                    if (ev)
                        add_poll(pfds, kind, svc, bind, &n, e->err_fd, ev, 10, (int)i,
                                 j * 10 + V2_EXEC_FD_ERR);
                }
            }
            if (ts->type == V2_SERVICE_PTY && ts->pty_master_fd >= 0) {
                short ev = 0;
                if (v2_pty_wants_master_read(ts))
                    ev |= POLLIN;
                if (ts->active_stream_id &&
                    lmux2_stream_rx_len(&a->conn, ts->active_stream_id) > 0)
                    ev |= POLLOUT;
                if (ev || ts->active_stream_id)
                    add_poll(pfds, kind, svc, bind, &n, ts->pty_master_fd,
                             ev, 5, (int)i, -1);
            }
        }
        if (poll(pfds, n, timeout) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (i = 0; i < n; i++) {
            if (!pfds[i].revents)
                continue;
            if (kind[i] == 1) {
                unsigned endpoint = (unsigned)svc[i];
                struct lmux_carrier_endpoint *ep =
                    lmux_carrier_manager_at(&a->carriers, endpoint);
                if (!ep)
                    continue;
                if (!lmux_carrier_manager_is_active(&a->carriers, ep) ||
                    (a->carrier_failure_hold_mask & (1u << endpoint))) {
                    if (pfds[i].revents & POLLIN)
                        drain_standby_carrier(a, endpoint);
                    if (pfds[i].revents & POLLOUT)
                        flush_raw_control(a, endpoint);
                    if (ep && lmux_carrier_fd(&ep->carrier) >= 0 &&
                        (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                        log_msg(a, 1, "lmux2 %s standby carrier revents=0x%x",
                                ep->name,
                                pfds[i].revents);
                        lmux_carrier_mark_lost(&ep->carrier, now_ms());
                    }
                } else if (a->outer_enabled) {
                    if (pfds[i].revents & POLLIN)
                        feed_outer_carrier(a);
                    if (pfds[i].revents & POLLOUT) {
                        if (ep->mode == LMUX_CARRIER_MODE_DIRTY &&
                            a->dirty_active) {
                            if (a->dirty.output_pos < a->dirty.output_len)
                                flush_outer_carrier(a);
                            else if (lmc_any_output(a, endpoint))
                                flush_raw_control(a, endpoint);
                            else if (!a->carrier_switch.old_outer_frozen)
                                flush_outer_carrier(a);
                        } else {
                            if (lmux_link_has_pending_output(&a->outer))
                                flush_outer_carrier(a);
                            if (!lmux_link_has_pending_output(&a->outer))
                                flush_raw_control(a, endpoint);
                            if (!lmc_any_output(a, endpoint) &&
                                !a->carrier_switch.old_outer_frozen)
                                flush_outer_carrier(a);
                        }
                    }
                } else {
                    if ((pfds[i].revents & POLLIN) && (!a->worker_mode || a->worker_link_active))
                        feed_carrier(a);
                    if (pfds[i].revents & POLLOUT)
                        flush_carrier(a);
                }
                if (lmux_carrier_manager_is_active(&a->carriers, ep) &&
                    (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)))
                    carrier_lost(a);
            } else if (kind[i] == 2) {
                if (a->role == LMUX2_ROLE_HOST)
                    v2_host_accept_service_client(a, &a->host, &a->conn,
                                                  &a->host.services[svc[i]]);
                else
                    accept_target_service_client(a, &a->services[svc[i]]);
            } else if (kind[i] == 3) {
                if (a->role == LMUX2_ROLE_HOST) {
                    struct v2_host_service *s = &a->host.services[svc[i]];
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                        v2_app_log(a, 1,
                                   "lmux2 host pending client abandoned service=%s fd=%d events=0 revents=0x%x",
                                   s->name, s->pending_fd, pfds[i].revents);
                        v2_host_clear_pending(s);
                    }
                } else {
                    struct v2_service *s = &a->services[svc[i]];
                    if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                        clear_pending(a, s);
                }
            } else if (kind[i] == 4) {
                if (a->role == LMUX2_ROLE_HOST) {
                    struct v2_host_service *s = &a->host.services[svc[i]];
                    struct v2_host_binding *b = &s->bindings[bind[i]];
                    if (pfds[i].revents & POLLOUT)
                        v2_host_deliver_stream_rx(a, &a->conn, s, b);
                    if (pfds[i].revents & POLLIN)
                        v2_host_collect_endpoint_tx(a, &a->conn, s, b);
                    if (b->fd >= 0 &&
                        ((pfds[i].revents & (POLLERR | POLLNVAL)) ||
                         ((pfds[i].revents & POLLHUP) && v2_host_socket_peer_closed(b->fd))))
                        v2_host_close_binding(a, &a->conn, s, b, 1);
                } else {
                    struct v2_service *s = &a->services[svc[i]];
                    struct v2_binding *b = &s->bindings[bind[i]];
                    if (pfds[i].revents & POLLOUT)
                        deliver_stream_rx(a, s, b);
                    if (pfds[i].revents & POLLIN)
                        collect_endpoint_tx(a, s, b);
                    if (b->fd >= 0 &&
                        ((pfds[i].revents & (POLLERR | POLLNVAL)) ||
                         ((pfds[i].revents & POLLHUP) && v2_socket_peer_closed(b->fd))))
                        close_binding(a, s, b, 1);
                }
            } else if (kind[i] == 5) {
                struct v2_service *s = &a->services[svc[i]];
                if (pfds[i].revents & POLLOUT)
                    v2_pty_deliver_stream_rx(&a->conn, s);
                if (pfds[i].revents & POLLIN)
                    v2_pty_collect_tx(&a->conn, s);
                if (pfds[i].revents & POLLHUP) {
                    /*
                     * A PTY master reports HUP in the fork-to-open interval
                     * before the supervised child opens its slave.  Reap
                     * first and treat HUP as terminal only when the child
                     * really exited; otherwise the accepted stream would be
                     * detached nondeterministically during service startup.
                     */
                    v2_pty_supervise(a, a->services);
                    if (s->child_pid <= 0)
                        close_pty_attachment(a, s);
                }
                if (pfds[i].revents & (POLLERR | POLLNVAL))
                    close_pty_attachment(a, s);
            } else if (kind[i] == 6) {
                if (pfds[i].revents & POLLIN)
                    v2_host_control_accept(&a->host);
            } else if (kind[i] == 7) {
                if (pfds[i].revents & (POLLIN | POLLHUP)) {
                    struct v2_host_status st;
                    fill_target_service_status(a);
                    fill_host_status(a, &st);
                    v2_host_control_service(a, &a->host, &st);
                }
                if (a->host.control_fd >= 0 &&
                    (pfds[i].revents & (POLLERR | POLLNVAL)))
                    v2_host_control_on_hup(&a->host);
            } else if (kind[i] == 8) {
                if (pfds[i].revents & POLLIN)
                    accept_carrier(a, (unsigned)svc[i]);
            } else if (kind[i] == 9) {
                if (pfds[i].revents & POLLIN)
                    handle_supervisor_control(a);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    a->stop = 1;
            } else if (kind[i] == 10) {
                struct v2_service *s = &a->services[svc[i]];
                int exec_idx = bind[i] / 10;
                int fd_kind = bind[i] % 10;
                v2_exec_on_fd_event(a, &a->conn, s, exec_idx, fd_kind,
                                    pfds[i].revents);
            } else if (kind[i] == 11) {
                if (pfds[i].revents & POLLIN)
                    v2_host_structured_accept(&a->host);
            } else if (kind[i] == 12) {
                struct v2_host_status st;
                fill_target_service_status(a);
                fill_host_status(a, &st);
                v2_host_structured_service(a, &a->host, &st, pfds[i].revents);
            } else if (kind[i] == 13) {
                if (pfds[i].revents & POLLOUT)
                    carrier_console_tick(a, now_ms());
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    carrier_console_finish(a, 0, "carrier-lost");
            } else if (kind[i] == 14) {
                if (pfds[i].revents &
                    (POLLIN | POLLHUP | POLLERR | POLLNVAL))
                    v2_host_control_carrier_service(&a->host,
                                                    pfds[i].revents);
            }
        }
        v2_unix_pair_unbound_target_streams(a, &a->conn, a->services);
        if (a->role == LMUX2_ROLE_HOST)
            v2_host_attach_pending_clients(a, &a->host, &a->conn);
        cleanup_terminal_bindings(a);
        (void)step_file_services(a);
        (void)step_exec_services(a);
        v2_pty_supervise(a, a->services);
        if (a->role == LMUX2_ROLE_TARGET) {
            for (i = 0; i < V2_MAX_SERVICES; i++)
                if (a->services[i].used &&
                    a->services[i].type == V2_SERVICE_PTY)
                    v2_pty_collect_tx(&a->conn, &a->services[i]);
        }
        if (!a->carrier_console.active &&
            (!a->worker_mode || a->worker_link_active || a->worker_link_up_flush) &&
            (!a->outer_enabled || a->outer.state == LMUX_LINK_ESTABLISHED)) {
            build_next_output(a);
            if (a->outer_enabled) {
                pump_lmux_output_to_outer(a);
                flush_outer_carrier(a);
            } else {
                flush_carrier(a);
            }
        }
        worker_maybe_finish_link_up(a);
    }
}

static void cleanup_app(struct v2_app *a)
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (a->services[i].used)
            v2_service_cleanup(&a->services[i]);
    v2_host_cleanup(&a->host);
    lmux_carrier_manager_cleanup(&a->carriers);
}

static void on_signal(int sig)
{
    (void)sig;
    if (global_v2_app)
        global_v2_app->stop = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
            "lmuxd %s (%s)\n"
            "Usage: lmuxd --role host|target --carrier PATH [--control PATH] [--structured-control PATH] [--unix NAME:HOST_SOCKET:TARGET_SOCKET] [--pty NAME:HOST_SOCKET:ARGV] [--file NAME:HOST_SOCKET] [--verbose]\n"
            "       lmuxd --role host|target --carrier-fd FD [--control PATH] [--structured-control PATH] [--unix NAME:HOST_SOCKET:TARGET_SOCKET] [--pty NAME:HOST_SOCKET:ARGV] [--file NAME:HOST_SOCKET] [--verbose]\n"
            "       lmuxd --role host|target --carrier-listen PATH|--carrier-connect PATH [test carrier]\n"
            "       lmuxd --role host --outer --carrier PATH [host options]\n"
            "       lmuxd --role host --outer --carrier PATH [--carrier-name NAME] [--standby-carrier NAME=PATH [--standby-baud N] [--standby-rtscts]] [host options]\n"
            "       lmuxd --worker --carrier-fd FD --supervisor-control-fd FD [--verbose]\n"
            "       lmuxd --help\n"
            "       lmuxd --version\n",
            LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
}

int lmux2_runtime_main(int argc, char **argv)
{
    struct v2_app a;
    struct lmux_carrier_endpoint *primary_ep = NULL;
    struct lmux_carrier_endpoint *standby_ep = NULL;
    char primary_name[LMUX_CARRIER_NAME_MAX + 1] = "primary";
    unsigned standby_baud = 115200;
    int standby_baud_set = 0;
    int standby_rtscts = 0;
    int i, have_role = 0, explicit_role = 0;
    memset(&a, 0, sizeof(a));
    lmux_carrier_manager_init(&a.carriers);
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++) {
        lmux_lmc1_raw_init(&a.lmc_io[i]);
        lmux_lmc1_dirty_init(&a.lmc_dirty_io[i]);
    }
    v2_host_init(&a.host);
    lmux_link_endpoint_init(&a.outer, LMUX_LINK_ROLE_HOST);
    a.outer.local_caps |= LMUX_LINK_CAP_BOOT_INFO | LMUX_LINK_CAP_BOOT_UPLOAD |
                          LMUX_LINK_CAP_BOOT_MANAGED_WORKER |
                          LMUX_LINK_CAP_BOOT_SUPERVISOR_CONTROL;
    a.supervisor_ctrl_fd = -1;
    a.next_service_id = 1;
    v2_service_slots_init(a.services);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--role") == 0) {
            if (a.worker_mode || ++i >= argc || parse_role(argv[i], &a.role) < 0)
                return usage(stderr), 1;
            have_role = 1;
            explicit_role = 1;
        } else if (strcmp(argv[i], "--worker") == 0) {
            if (explicit_role)
                return usage(stderr), 1;
            a.worker_mode = 1;
            a.role = LMUX2_ROLE_TARGET;
            have_role = 1;
        } else if (strcmp(argv[i], "--outer") == 0) {
            a.outer_enabled = 1;
        } else if (strcmp(argv[i], "--carrier") == 0) {
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&a.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_tty_path(&primary_ep->carrier, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-name") == 0) {
            if (++i >= argc || !lmux_carrier_name_valid(argv[i]))
                return usage(stderr), 1;
            if (primary_ep) {
                if (lmux_carrier_manager_rename(&a.carriers, primary_ep,
                                                argv[i]) < 0)
                    return usage(stderr), 1;
            } else {
                strcpy(primary_name, argv[i]);
            }
        } else if (strcmp(argv[i], "--carrier-fd") == 0) {
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&a.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_fd(&primary_ep->carrier, atoi(argv[i])) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-listen") == 0) {
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&a.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_unix_listen(&primary_ep->carrier,
                                             argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-connect") == 0) {
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&a.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_unix_connect(&primary_ep->carrier,
                                              argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--standby-carrier") == 0 ||
                   strcmp(argv[i], "--standby-carrier-fd") == 0 ||
                   strcmp(argv[i], "--standby-carrier-listen") == 0 ||
                   strcmp(argv[i], "--standby-carrier-connect") == 0) {
            const char *option = argv[i];
            const char *value;
            char name[LMUX_CARRIER_NAME_MAX + 1];
            if (++i >= argc || standby_ep ||
                lmux_carrier_named_value_parse(argv[i], name, &value) < 0 ||
                lmux_carrier_manager_add(&a.carriers, name, 0,
                                         &standby_ep) < 0)
                return usage(stderr), 1;
            if (strcmp(option, "--standby-carrier") == 0) {
                if (lmux_carrier_set_tty_path(&standby_ep->carrier, value) < 0)
                    return usage(stderr), 1;
                lmux_carrier_set_tty_policy(&standby_ep->carrier,
                                            LMUX_TTY_POLICY_STRICT_UART);
                lmux_carrier_set_exclusive(&standby_ep->carrier, 1);
            } else if (strcmp(option, "--standby-carrier-fd") == 0) {
                if (lmux_carrier_set_fd(&standby_ep->carrier,
                                        atoi(value)) < 0)
                    return usage(stderr), 1;
            } else if (strcmp(option, "--standby-carrier-listen") == 0) {
                if (lmux_carrier_set_unix_listen(&standby_ep->carrier,
                                                 value) < 0)
                    return usage(stderr), 1;
            } else if (lmux_carrier_set_unix_connect(&standby_ep->carrier,
                                                     value) < 0) {
                return usage(stderr), 1;
            }
        } else if (strcmp(argv[i], "--standby-baud") == 0) {
            char *end = NULL;
            unsigned long baud;
            if (++i >= argc)
                return usage(stderr), 1;
            baud = strtoul(argv[i], &end, 10);
            if (!end || *end || baud == 0 || baud > 0xffffffffUL)
                return usage(stderr), 1;
            standby_baud = (unsigned)baud;
            standby_baud_set = 1;
        } else if (strcmp(argv[i], "--standby-rtscts") == 0) {
            standby_rtscts = 1;
        } else if (strcmp(argv[i], "--supervisor-control-fd") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.supervisor_ctrl_fd = atoi(argv[i]);
        } else if (strcmp(argv[i], "--control") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.host.control_path = argv[i];
        } else if (strcmp(argv[i], "--structured-control") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.host.structured_path = argv[i];
        } else if (strcmp(argv[i], "--unix") == 0) {
            if (++i >= argc || v2_host_add_unix(&a.host, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--pty") == 0) {
            if (++i >= argc || v2_host_add_pty(&a.host, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--file") == 0) {
            if (++i >= argc || v2_host_add_file(&a.host, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            a.verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("lmuxd %s (%s)\n", LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 1;
        }
    }
    if (standby_ep) {
        standby_ep->carrier.serial.baud = standby_baud;
        if (standby_rtscts)
            standby_ep->carrier.serial.flow = LMUX_SERIAL_FLOW_RTSCTS;
    } else if (standby_baud_set || standby_rtscts) {
        return usage(stderr), 1;
    }
    if (!have_role || !primary_ep ||
        !lmux_carrier_is_configured(&primary_ep->carrier))
        return usage(stderr), 1;
    if (a.outer_enabled && (a.role != LMUX2_ROLE_HOST || a.worker_mode))
        return usage(stderr), 1;
    if (a.worker_mode &&
        (a.role != LMUX2_ROLE_TARGET || a.supervisor_ctrl_fd < 0 ||
         primary_ep->carrier.kind != LMUX_CARRIER_FD || standby_ep))
        return usage(stderr), 1;
    if (!a.worker_mode && a.role != LMUX2_ROLE_HOST && standby_ep)
        return usage(stderr), 1;
    if (!a.worker_mode && a.supervisor_ctrl_fd >= 0)
        return usage(stderr), 1;
    if (lmux_carrier_manager_start(&a.carriers) < 0) {
        perror(lmux_carrier_label(active_carrier(&a)));
        return 1;
    }
    if (a.host.control_path) {
        if (v2_host_control_start(&a.host) < 0) {
            perror(a.host.control_path);
            cleanup_app(&a);
            return 1;
        }
    }
    if (a.host.structured_path) {
        if (v2_host_structured_start(&a.host) < 0) {
            perror(a.host.structured_path);
            cleanup_app(&a);
            return 1;
        }
    }
    if (a.role == LMUX2_ROLE_HOST &&
        v2_host_create_desired_services(&a, &a.host, &a.next_service_id) < 0) {
        cleanup_app(&a);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    global_v2_app = &a;
    lmux2_conn_init(&a.conn, a.role, make_conn_id());
    if (!a.worker_mode && !a.outer_enabled)
        (void)lmux2_conn_start(&a.conn);
    if (!a.worker_mode)
        try_open_carrier(&a);
    if (a.outer_enabled && lmux_carrier_fd(active_carrier(&a)) >= 0 &&
        a.outer.state == LMUX_LINK_DOWN)
        begin_outer(&a);
    fprintf(stderr, "lmuxd %s (%s) LMUX/2 role=%s%s%s\n", LMUX_BUILD_VERSION,
            LMUX_PROJECT_NAME, role_name(a.role), a.worker_mode ? " worker" : "",
            a.outer_enabled ? " outer=OUTER/1" : "");
    run_loop(&a);
    cleanup_app(&a);
    return 0;
}
