#include "host.h"
#include "services.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

struct hc2_frame {
    uint16_t type;
    uint16_t flags;
    uint64_t request_id;
    const uint8_t *payload;
    uint32_t payload_len;
};

void lmux_hc2_put_u64(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)v);
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

uint64_t lmux_hc2_get_u64(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) |
        ((uint64_t)lmux_get_le32(p + 4) << 32);
}

static const char *hc2_service_type_name(enum v2_service_type t)
{
    return v2_service_type_name(t);
}

#define HC2_FILE_NONE 0
#define HC2_FILE_PUT_WAIT_OK 1
#define HC2_FILE_PUT_SEND 2
#define HC2_FILE_PUT_WAIT_DONE 3
#define HC2_FILE_GET_WAIT_OK 4
#define HC2_FILE_GET_RECV 5
#define HC2_FILE_DONE_NOTIFY 6
#define HC2_FILE_ERROR_NOTIFY 7

#define HC2_FILE_KIND_PUT 1
#define HC2_FILE_KIND_GET 2
#define HC2_FILE_PROGRESS_STEP 65536u
#define HC2_EXEC_MODE_INFO_ONCE 9

static int hc2_queue_frame(struct v2_host *h, uint16_t type, uint16_t flags,
                           uint64_t request_id, const uint8_t *payload,
                           uint32_t payload_len);

static struct v2_host_structured_exec *hc2_exec_by_ref(struct v2_host *h,
                                                       uint64_t ref)
{
    int i;
    if (!ref)
        return NULL;
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++)
        if (h->structured_execs[i].used &&
            h->structured_execs[i].ref == ref)
            return &h->structured_execs[i];
    return NULL;
}

static int hc2_exec_live_count(struct v2_host *h)
{
    int i, n = 0;
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++)
        if (h->structured_execs[i].used &&
            h->structured_execs[i].active &&
            !h->structured_execs[i].done &&
            !h->structured_execs[i].lost)
            n++;
    return n;
}

static struct v2_host_structured_exec *hc2_exec_alloc(struct v2_host *h)
{
    int i;
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++) {
        struct v2_host_structured_exec *e = &h->structured_execs[i];
        if (!e->used || e->done || e->lost || !e->active) {
            memset(e, 0, sizeof(*e));
            e->used = 1;
            e->ref = h->structured_next_ref++;
            if (!h->structured_next_ref)
                h->structured_next_ref = 1;
            return e;
        }
    }
    return NULL;
}

static void hc2_exec_close_streams(struct v2_app *a,
                                   struct v2_host_structured_exec *e)
{
    if (e->stdin_stream)
        (void)v2_runtime_close_stream(a, e->stdin_stream);
    if (e->stdout_stream)
        (void)v2_runtime_close_stream(a, e->stdout_stream);
    if (e->stderr_stream)
        (void)v2_runtime_close_stream(a, e->stderr_stream);
    e->stdin_stream = e->stdout_stream = e->stderr_stream = 0;
    e->stdin_close_pending = 0;
}

static void hc2_exec_free(struct v2_app *a, struct v2_host_structured_exec *e)
{
    if (!e || !e->used)
        return;
    if (a)
        hc2_exec_close_streams(a, e);
    memset(e, 0, sizeof(*e));
}

static void hc2_mark_entry_lost(struct v2_host *h,
                                struct v2_host_structured_exec *e,
                                uint32_t reason)
{
    uint8_t p[12];
    if (!e || !e->used || e->done || e->lost)
        return;
    e->lost = 1;
    e->active = 0;
    e->auto_reattach = 0;
    if (h->structured_fd < 0 || !h->structured_ready || e->lost_emitted)
        return;
    lmux_hc2_put_u64(p, e->ref);
    lmux_put_le32(p + 8, reason);
    if (hc2_queue_frame(h, LMUX_HC2_EXEC_LOST, 0, 0, p, sizeof(p)) == 0)
        e->lost_emitted = 1;
}

static int hc2_queue_exec_state_for_ref(struct v2_host *h, uint16_t frame_type,
                                        uint64_t request_id, uint64_t ref,
                                        uint64_t exec_id, uint8_t state,
                                        uint8_t fail, uint16_t errnum,
                                        uint32_t exit_code,
                                        uint32_t term_signal,
                                        const uint8_t *payload,
                                        uint16_t payload_len)
{
    uint8_t p[80];
    uint32_t flags = 0;
    if (payload_len >= 16) {
        if (payload[12])
            flags |= 1u;
        if (payload[13])
            flags |= 2u;
        if (payload[14])
            flags |= 4u;
        if (payload[15])
            flags |= 8u;
    }
    lmux_hc2_put_u64(p, ref);
    lmux_hc2_put_u64(p + 8, exec_id);
    p[16] = state;
    p[17] = fail;
    lmux_put_le16(p + 18, errnum);
    lmux_put_le32(p + 20, exit_code);
    lmux_put_le32(p + 24, term_signal);
    lmux_put_le32(p + 28, flags);
    if (payload_len >= 64) {
        memcpy(p + 32, payload + 16, 48);
        return v2_host_structured_queue_exec_frame(h, frame_type, request_id,
                                                   p, sizeof(p));
    }
    return v2_host_structured_queue_exec_frame(h, frame_type, request_id,
                                               p, 32);
}

static int hc2_queue_exec_stdin_ready(struct v2_host *h,
                                      struct v2_host_structured_exec *e)
{
    uint8_t p[8];
    lmux_hc2_put_u64(p, e->ref);
    return v2_host_structured_queue_exec_frame(h, LMUX_HC2_EXEC_STDIN_READY,
                                               0, p, sizeof(p));
}

static int hc2_stream_drained(struct v2_app *a, uint64_t stream_id)
{
    int st;
    if (!stream_id)
        return 1;
    st = v2_runtime_stream_state(a, stream_id);
    if (st == LMUX2_STREAM_UNUSED)
        return 1;
    if (st == LMUX2_STREAM_RESET)
        return v2_runtime_stream_rx_len(a, stream_id) == 0;
    if (st == LMUX2_STREAM_REMOTE_CLOSED || st == LMUX2_STREAM_TERMINAL)
        return v2_runtime_stream_rx_len(a, stream_id) == 0;
    return 0;
}

static int hc2_abs_path_ok(const char *p)
{
    return p && p[0] == '/' && strlen(p) < LXF_PATH_MAX;
}

static int hc2_make_tmp_for_final(const char *final, char *tmp, size_t cap)
{
    const char *slash = strrchr(final, '/');
    const char *base;
    size_t dir_len;
    if (!slash || final[0] != '/')
        return -1;
    base = slash + 1;
    if (!*base)
        return -1;
    dir_len = (size_t)(slash - final);
    if (dir_len == 0)
        dir_len = 1;
    if (snprintf(tmp, cap, "%.*s/.%s.lmux-structured-%ld",
                 (int)dir_len, final, base, (long)getpid()) >= (int)cap)
        return -1;
    return 0;
}

static void hc2_file_clear(struct v2_app *a, struct v2_host *h, int close_stream)
{
    int i;
    uint64_t stream = h->structured_file_stream;
    if (h->structured_file_fd >= 0)
        v2_host_close_fd(&h->structured_file_fd);
    if (h->structured_file_tmp_path[0])
        unlink(h->structured_file_tmp_path);
    if (close_stream && stream)
        (void)v2_runtime_close_stream(a, stream);
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_host_service *s = &h->services[i];
        if (s->used && s->type == V2_SERVICE_FILE &&
            s->active_stream_id == stream)
            s->active_stream_id = 0;
    }
    h->structured_file_active = 0;
    h->structured_file_kind = 0;
    h->structured_file_phase = HC2_FILE_NONE;
    h->structured_file_stream = 0;
    h->structured_file_op_id = 0;
    h->structured_file_req_id = 0;
    h->structured_file_expected = 0;
    h->structured_file_count = 0;
    h->structured_file_last_progress = 0;
    h->structured_file_mode = 0;
    h->structured_file_local_path[0] = 0;
    h->structured_file_target_path[0] = 0;
    h->structured_file_tmp_path[0] = 0;
    h->structured_file_in_len = 0;
    h->structured_file_need_len = 0;
    h->structured_file_out_len = h->structured_file_out_pos = 0;
}

static void hc2_close_client(struct v2_host *h)
{
    int i;
    v2_host_close_fd(&h->structured_fd);
    h->structured_ready = 0;
    h->structured_rx_len = 0;
    h->structured_tx_len = h->structured_tx_pos = 0;
    h->structured_exec_info_pending = 0;
    memset(&h->structured_control_op, 0, sizeof(h->structured_control_op));
    h->structured_control_queue_head = 0;
    h->structured_control_queue_len = 0;
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++)
        h->structured_execs[i].auto_reattach = 0;
    if (h->exec_structured) {
        h->exec_structured = 0;
        h->exec_active = 0;
        h->exec_phase = 0;
        h->exec_status_pending = 0;
        h->exec_close_pending = 0;
        h->exec_close_stdin_after_drain = 0;
        h->exec_done_seen = 0;
        h->exec_service_id = 0;
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
}

static void hc2_close_client_runtime(struct v2_app *a, struct v2_host *h)
{
    int i;
    if (h->structured_file_active)
        hc2_file_clear(a, h, 1);
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++)
        if (h->structured_execs[i].used)
            hc2_exec_close_streams(a, &h->structured_execs[i]);
    if (h->exec_structured) {
        if (h->exec_control_stream)
            (void)v2_runtime_close_stream(a, h->exec_control_stream);
        if (h->exec_stdin_stream)
            (void)v2_runtime_close_stream(a, h->exec_stdin_stream);
        if (h->exec_stdout_stream)
            (void)v2_runtime_close_stream(a, h->exec_stdout_stream);
        if (h->exec_stderr_stream)
            (void)v2_runtime_close_stream(a, h->exec_stderr_stream);
    }
    hc2_close_client(h);
}

static int hc2_queue_frame(struct v2_host *h, uint16_t type, uint16_t flags,
                           uint64_t request_id, const uint8_t *payload,
                           uint32_t payload_len)
{
    uint8_t *p;
    size_t need = LMUX_HC2_HEADER_LEN + payload_len;
    if (payload_len > LMUX_HC2_MAX_PAYLOAD || need > sizeof(h->structured_tx))
        return -1;
    if (h->structured_tx_pos) {
        if (h->structured_tx_pos < h->structured_tx_len)
            memmove(h->structured_tx, h->structured_tx + h->structured_tx_pos,
                    h->structured_tx_len - h->structured_tx_pos);
        h->structured_tx_len -= h->structured_tx_pos;
        h->structured_tx_pos = 0;
    }
    if (sizeof(h->structured_tx) - h->structured_tx_len < need)
        return -1;
    p = h->structured_tx + h->structured_tx_len;
    lmux_put_le32(p, LMUX_HC2_MAGIC);
    lmux_put_le16(p + 4, LMUX_HC2_VERSION);
    lmux_put_le16(p + 6, type);
    lmux_put_le16(p + 8, flags);
    lmux_put_le16(p + 10, 0);
    lmux_hc2_put_u64(p + 12, request_id);
    lmux_put_le32(p + 20, payload_len);
    if (payload_len)
        memcpy(p + LMUX_HC2_HEADER_LEN, payload, payload_len);
    h->structured_tx_len += need;
    return 0;
}

int v2_host_structured_can_queue(struct v2_host *h, uint32_t payload_len)
{
    size_t used, need = LMUX_HC2_HEADER_LEN + payload_len;
    if (h->structured_fd < 0 || payload_len > LMUX_HC2_MAX_PAYLOAD ||
        need > sizeof(h->structured_tx))
        return 0;
    used = h->structured_tx_len - h->structured_tx_pos;
    return sizeof(h->structured_tx) - used >= need;
}

int v2_host_structured_queue_exec_frame(struct v2_host *h, uint16_t type,
                                        uint64_t request_id,
                                        const uint8_t *payload,
                                        uint32_t payload_len)
{
    return hc2_queue_frame(h, type, 0, request_id, payload, payload_len);
}

static void hc2_queue_error(struct v2_host *h, uint64_t request_id,
                            uint32_t code, const char *msg)
{
    uint8_t p[260];
    size_t n = msg ? strlen(msg) : 0;
    if (n > sizeof(p) - 9)
        n = sizeof(p) - 9;
    lmux_put_le32(p, code);
    lmux_put_le32(p + 4, (uint32_t)n);
    if (n)
        memcpy(p + 8, msg, n);
    (void)hc2_queue_frame(h, LMUX_HC2_ERROR, 0, request_id, p, (uint32_t)(8 + n));
}

static int hc2_control_frame_schedulable(uint16_t type)
{
    return type == LMUX_HC2_EXEC_START ||
        type == LMUX_HC2_EXEC_ATTACH ||
        type == LMUX_HC2_EXEC_STATUS ||
        type == LMUX_HC2_EXEC_SIGNAL ||
        type == LMUX_HC2_EXEC_RELEASE;
}

static int hc2_enqueue_control_frame(struct v2_host *h,
                                     const struct hc2_frame *fr)
{
    struct v2_host_structured_pending_frame *q;
    int idx;
    if (h->structured_control_queue_len >= V2_HOST_STRUCTURED_CTL_QUEUE_MAX ||
        fr->payload_len > LMUX_HC2_MAX_PAYLOAD)
        return -1;
    idx = (h->structured_control_queue_head +
           h->structured_control_queue_len) %
        V2_HOST_STRUCTURED_CTL_QUEUE_MAX;
    q = &h->structured_control_queue[idx];
    q->type = fr->type;
    q->flags = fr->flags;
    q->request_id = fr->request_id;
    q->payload_len = fr->payload_len;
    if (fr->payload_len)
        memcpy(q->payload, fr->payload, fr->payload_len);
    h->structured_control_queue_len++;
    return 0;
}

static int hc2_parse_frame(struct v2_host *h, struct hc2_frame *fr)
{
    const uint8_t *p = h->structured_rx;
    uint32_t len;
    if (h->structured_rx_len < LMUX_HC2_HEADER_LEN)
        return 0;
    if (lmux_get_le32(p) != LMUX_HC2_MAGIC ||
        lmux_get_le16(p + 4) != LMUX_HC2_VERSION)
        return -1;
    len = lmux_get_le32(p + 20);
    if (len > LMUX_HC2_MAX_PAYLOAD)
        return -1;
    if (h->structured_rx_len < LMUX_HC2_HEADER_LEN + len)
        return 0;
    fr->type = lmux_get_le16(p + 6);
    fr->flags = lmux_get_le16(p + 8);
    fr->request_id = lmux_hc2_get_u64(p + 12);
    fr->payload_len = len;
    fr->payload = p + LMUX_HC2_HEADER_LEN;
    return 1;
}

static void hc2_consume_frame(struct v2_host *h, const struct hc2_frame *fr)
{
    size_t n = LMUX_HC2_HEADER_LEN + fr->payload_len;
    if (n < h->structured_rx_len)
        memmove(h->structured_rx, h->structured_rx + n, h->structured_rx_len - n);
    h->structured_rx_len -= n;
}

static int hc2_get_string(const uint8_t *p, size_t len, size_t *off,
                          const uint8_t **out, uint16_t *out_len)
{
    uint16_t n;
    if (*off + 2 > len)
        return -1;
    n = lmux_get_le16(p + *off);
    *off += 2;
    if (*off + n > len)
        return -1;
    *out = p + *off;
    *out_len = n;
    *off += n;
    return 0;
}

static int hc2_queue_server_hello(struct v2_host *h, uint64_t request_id)
{
    uint8_t p[64];
    const char *ver = LMUX_BUILD_VERSION;
    size_t n = strlen(ver);
    if (n > 40)
        n = 40;
    lmux_put_le32(p, LMUX_HC2_CAP_STATUS | LMUX_HC2_CAP_EXEC | LMUX_HC2_CAP_FILE);
    lmux_put_le32(p + 4, LMUX_HC2_MAX_PAYLOAD);
    lmux_put_le32(p + 8, LMUX_HC2_TX_CAP);
    lmux_put_le16(p + 12, (uint16_t)n);
    memcpy(p + 14, ver, n);
    return hc2_queue_frame(h, LMUX_HC2_SERVER_HELLO, 0, request_id,
                           p, (uint32_t)(14 + n));
}

static int hc2_append_u8(uint8_t *p, size_t cap, size_t *off, uint8_t v)
{
    if (*off + 1 > cap)
        return -1;
    p[(*off)++] = v;
    return 0;
}

static int hc2_append_u32(uint8_t *p, size_t cap, size_t *off, uint32_t v)
{
    if (*off + 4 > cap)
        return -1;
    lmux_put_le32(p + *off, v);
    *off += 4;
    return 0;
}

static int hc2_append_u64(uint8_t *p, size_t cap, size_t *off, uint64_t v)
{
    if (*off + 8 > cap)
        return -1;
    lmux_hc2_put_u64(p + *off, v);
    *off += 8;
    return 0;
}

static int hc2_append_string(uint8_t *p, size_t cap, size_t *off, const char *s)
{
    size_t n = strlen(s);
    if (n > UINT16_MAX || *off + 2 + n > cap)
        return -1;
    lmux_put_le16(p + *off, (uint16_t)n);
    *off += 2;
    memcpy(p + *off, s, n);
    *off += n;
    return 0;
}

static int hc2_queue_file_started(struct v2_host *h)
{
    uint8_t p[32];
    lmux_hc2_put_u64(p, h->structured_file_op_id);
    p[8] = (uint8_t)h->structured_file_kind;
    memset(p + 9, 0, 7);
    lmux_put_le64(p + 16, h->structured_file_expected);
    return hc2_queue_frame(h, LMUX_HC2_FILE_STARTED, 0,
                           h->structured_file_req_id, p, 24);
}

static int hc2_queue_file_progress(struct v2_host *h)
{
    uint8_t p[16];
    lmux_hc2_put_u64(p, h->structured_file_op_id);
    lmux_put_le64(p + 8, h->structured_file_count);
    return hc2_queue_frame(h, LMUX_HC2_FILE_PROGRESS, 0, 0, p, sizeof(p));
}

static int hc2_queue_file_done(struct v2_host *h)
{
    uint8_t p[48];
    lmux_hc2_put_u64(p, h->structured_file_op_id);
    lmux_put_le64(p + 8, h->structured_file_count);
    memcpy(p + 16, h->structured_file_digest, 32);
    return hc2_queue_frame(h, LMUX_HC2_FILE_DONE, 0,
                           h->structured_file_req_id, p, sizeof(p));
}

static int hc2_queue_file_error_frame(struct v2_host *h, uint32_t code,
                                      const char *msg)
{
    uint8_t p[280];
    size_t n = msg ? strlen(msg) : 0;
    if (n > sizeof(p) - 14)
        n = sizeof(p) - 14;
    lmux_hc2_put_u64(p, h->structured_file_op_id);
    lmux_put_le32(p + 8, code);
    lmux_put_le16(p + 12, (uint16_t)n);
    if (n)
        memcpy(p + 14, msg, n);
    return hc2_queue_frame(h, LMUX_HC2_FILE_ERROR, 0,
                           h->structured_file_req_id, p, (uint32_t)(14 + n));
}

static int hc2_file_queue_lxf(struct v2_host *h, uint8_t type,
                              const uint8_t *payload, uint32_t len)
{
    if (h->structured_file_out_len != h->structured_file_out_pos)
        return -1;
    h->structured_file_out_len = h->structured_file_out_pos = 0;
    return lxf_encode_record(type, 0, payload, len, h->structured_file_out,
                             sizeof(h->structured_file_out),
                             &h->structured_file_out_len);
}

static int hc2_file_flush_lxf(struct v2_app *a, struct v2_host *h)
{
    while (h->structured_file_out_pos < h->structured_file_out_len &&
           v2_runtime_stream_is_open(a, h->structured_file_stream) &&
           v2_runtime_stream_tx_len(a, h->structured_file_stream) < LMUX2_STREAM_TX_CAP) {
        size_t n = h->structured_file_out_len - h->structured_file_out_pos;
        size_t space = LMUX2_STREAM_TX_CAP -
            v2_runtime_stream_tx_len(a, h->structured_file_stream);
        size_t w;
        if (n > space)
            n = space;
        w = v2_runtime_stream_write(a, h->structured_file_stream,
                                    h->structured_file_out +
                                    h->structured_file_out_pos, n);
        if (w == 0)
            break;
        h->structured_file_out_pos += w;
    }
    if (h->structured_file_out_pos == h->structured_file_out_len) {
        h->structured_file_out_pos = h->structured_file_out_len = 0;
        return 1;
    }
    return 0;
}

static int hc2_file_read_lxf(struct v2_app *a, struct v2_host *h,
                             uint8_t *type, const uint8_t **payload,
                             uint32_t *len)
{
    for (;;) {
        size_t want, n;
        if (h->structured_file_need_len == 0 &&
            h->structured_file_in_len >= LXF_HEADER_LEN) {
            uint8_t flags;
            if (lxf_decode_header(h->structured_file_in, h->structured_file_in_len,
                                  type, &flags, len) < 0) {
                (void)flags;
                return -1;
            }
            h->structured_file_need_len = LXF_HEADER_LEN + *len;
        }
        if (h->structured_file_need_len &&
            h->structured_file_in_len >= h->structured_file_need_len) {
            uint8_t flags;
            if (lxf_decode_header(h->structured_file_in, h->structured_file_in_len,
                                  type, &flags, len) < 0) {
                (void)flags;
                return -1;
            }
            *payload = h->structured_file_in + LXF_HEADER_LEN;
            return 1;
        }
        want = h->structured_file_need_len ? h->structured_file_need_len : LXF_HEADER_LEN;
        if (h->structured_file_in_len >= want)
            return 0;
        if (v2_runtime_stream_state(a, h->structured_file_stream) == LMUX2_STREAM_TERMINAL ||
            v2_runtime_stream_state(a, h->structured_file_stream) == LMUX2_STREAM_RESET)
            return -1;
        n = v2_runtime_stream_read(a, h->structured_file_stream,
                                   h->structured_file_in + h->structured_file_in_len,
                                   want - h->structured_file_in_len);
        if (n == 0)
            return 0;
        h->structured_file_in_len += n;
    }
}

static void hc2_file_consume_lxf(struct v2_host *h)
{
    h->structured_file_in_len = 0;
    h->structured_file_need_len = 0;
}

static int hc2_full_write_fd(int fd, const uint8_t *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int hc2_queue_status(struct v2_host *h, const struct v2_host_status *st,
                            uint64_t request_id)
{
    uint8_t p[LMUX_HC2_MAX_PAYLOAD];
    size_t off = 0;
    int i, service_count = 0;

    if (hc2_append_string(p, sizeof(p), &off, LMUX_BUILD_VERSION) < 0 ||
        hc2_append_u8(p, sizeof(p), &off,
                      lmux_carrier_fd(st->carrier) >= 0 ? 1 : 0) < 0 ||
        hc2_append_u64(p, sizeof(p), &off, st->carrier->rx_bytes) < 0 ||
        hc2_append_u64(p, sizeof(p), &off, st->carrier->tx_bytes) < 0)
        return -1;
    if (st->outer) {
        if (hc2_append_u8(p, sizeof(p), &off, (uint8_t)st->outer->state) < 0 ||
            hc2_append_u64(p, sizeof(p), &off, st->outer->local_conn_id) < 0 ||
            hc2_append_u64(p, sizeof(p), &off, st->outer->peer_conn_id) < 0 ||
            hc2_append_u64(p, sizeof(p), &off, st->outer->crc_errors) < 0 ||
            hc2_append_u64(p, sizeof(p), &off, st->outer->seq_errors) < 0 ||
            hc2_append_u64(p, sizeof(p), &off, st->outer->resync_drops) < 0)
            return -1;
    } else if (hc2_append_u8(p, sizeof(p), &off, 0) < 0 ||
               hc2_append_u64(p, sizeof(p), &off, 0) < 0 ||
               hc2_append_u64(p, sizeof(p), &off, 0) < 0 ||
               hc2_append_u64(p, sizeof(p), &off, 0) < 0 ||
               hc2_append_u64(p, sizeof(p), &off, 0) < 0 ||
               hc2_append_u64(p, sizeof(p), &off, 0) < 0) {
        return -1;
    }
    if (hc2_append_u8(p, sizeof(p), &off, (uint8_t)st->conn->state) < 0 ||
        hc2_append_u64(p, sizeof(p), &off, st->conn->local_conn_id) < 0 ||
        hc2_append_u64(p, sizeof(p), &off, st->conn->peer_conn_id) < 0 ||
        hc2_append_u64(p, sizeof(p), &off, st->connection_established_count) < 0 ||
        hc2_append_u8(p, sizeof(p), &off, h->factory_pty) < 0 ||
        hc2_append_u8(p, sizeof(p), &off, h->factory_unix) < 0 ||
        hc2_append_u8(p, sizeof(p), &off, h->factory_file) < 0 ||
        hc2_append_u8(p, sizeof(p), &off, h->factory_exec) < 0)
        return -1;
    for (i = 0; i < V2_MAX_DESIRED; i++)
        if (h->desired[i].used)
            service_count++;
    if (hc2_append_u32(p, sizeof(p), &off, (uint32_t)service_count) < 0)
        return -1;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        const struct v2_desired *d = &h->desired[i];
        if (!d->used)
            continue;
        if (hc2_append_string(p, sizeof(p), &off, d->name) < 0 ||
            hc2_append_string(p, sizeof(p), &off, hc2_service_type_name(d->type)) < 0 ||
            hc2_append_u8(p, sizeof(p), &off, d->service_id != 0) < 0 ||
            hc2_append_u64(p, sizeof(p), &off, d->service_id) < 0 ||
            hc2_append_u8(p, sizeof(p), &off, d->internal != 0) < 0)
            return -1;
    }
    return hc2_queue_frame(h, LMUX_HC2_STATUS_REPLY, 0, request_id,
                           p, (uint32_t)off);
}

static int hc2_queue_exec_record(struct v2_host *h, uint8_t type, uint8_t status,
                                 uint64_t exec_id, const uint8_t *payload,
                                 uint16_t len)
{
    if (12u + len > sizeof(h->exec_ctl_out) ||
        h->exec_ctl_out_len != h->exec_ctl_out_pos)
        return -1;
    h->exec_ctl_out[0] = type;
    h->exec_ctl_out[1] = status;
    lmux_put_le16(h->exec_ctl_out + 2, len);
    lmux_hc2_put_u64(h->exec_ctl_out + 4, exec_id);
    if (len)
        memcpy(h->exec_ctl_out + 12, payload, len);
    h->exec_ctl_out_len = 12u + len;
    h->exec_ctl_out_pos = 0;
    return 0;
}

static struct v2_host_service *hc2_exec_service(struct v2_host *h)
{
    struct v2_host_service *s = v2_host_service_find_name(h->services, "__exec");
    if (!s)
        s = v2_host_service_find_name(h->services, "exec");
    if (!s || s->type != V2_SERVICE_EXEC || !s->id)
        return NULL;
    return s;
}

static void hc2_begin_control_op(struct v2_host *h,
                                 enum v2_host_structured_control_kind kind,
                                 uint64_t request_id,
                                 struct v2_host_structured_exec *e,
                                 int phase)
{
    h->exec_active = 1;
    h->exec_structured = 1;
    h->exec_phase = phase;
    h->exec_close_stdin_after_drain = 0;
    h->exec_next_status_ms = 0;
    h->structured_control_op.kind = kind;
    h->structured_control_op.request_id = request_id;
    h->structured_control_op.ref = e ? e->ref : 0;
}

static int hc2_request_exec_info(struct v2_app *a, struct v2_host *h)
{
    if (h->structured_exec_info_pending || h->exec_active ||
        !v2_runtime_lmux_established(a) || !hc2_exec_service(h))
        return 0;
    h->exec_active = 1;
    h->exec_structured = 1;
    h->exec_phase = HC2_EXEC_MODE_INFO_ONCE;
    h->exec_next_status_ms = 0;
    h->structured_control_op.kind = V2_HC2_CTL_INFO;
    h->structured_control_op.request_id = 0;
    h->structured_control_op.ref = 0;
    h->structured_exec_info_pending = 1;
    if (hc2_queue_exec_record(h, V2_EXEC_REC_INFO, 0, 0, NULL, 0) < 0) {
        h->exec_active = 0;
        h->exec_structured = 0;
        h->exec_phase = 0;
        memset(&h->structured_control_op, 0, sizeof(h->structured_control_op));
        h->structured_exec_info_pending = 0;
        return -1;
    }
    return 0;
}

static void hc2_handle_exec_start(struct v2_app *a, struct v2_host *h,
                                  const struct hc2_frame *fr)
{
    struct v2_host_structured_exec *e;
    uint8_t payload[V2_EXEC_CTL_BUF_MAX - 12];
    const uint8_t *cwd, *arg;
    uint16_t cwd_len, arg_len, argc, i;
    size_t off = 0, out = 0, argc_pos;
    if (h->exec_active || h->boot_pending || h->boot_upload_active || h->deploy_active) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_BUSY, "busy");
        return;
    }
    if (hc2_exec_live_count(h) >= V2_HOST_STRUCTURED_MAX_EXECS) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT, "exec capacity");
        return;
    }
    if (hc2_get_string(fr->payload, fr->payload_len, &off, &cwd, &cwd_len) < 0 ||
        off + 2 > fr->payload_len) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "bad start");
        return;
    }
    argc = lmux_get_le16(fr->payload + off);
    off += 2;
    if (!argc || argc > LMUX_HC2_MAX_ARGC) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "bad argv");
        return;
    }
    lmux_put_le16(payload + out, cwd_len);
    out += 2;
    if (out + 2 + argc * 2 > sizeof(payload)) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "too large");
        return;
    }
    payload[out++] = (uint8_t)argc;
    payload[out++] = 0;
    argc_pos = out;
    out += argc * 2;
    if (out + cwd_len > sizeof(payload)) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "too large");
        return;
    }
    if (cwd_len) {
        memcpy(payload + out, cwd, cwd_len);
        out += cwd_len;
    }
    for (i = 0; i < argc; i++) {
        if (hc2_get_string(fr->payload, fr->payload_len, &off, &arg, &arg_len) < 0 ||
            arg_len == 0 || out + arg_len > sizeof(payload)) {
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "bad argv");
            return;
        }
        lmux_put_le16(payload + argc_pos + i * 2, arg_len);
        memcpy(payload + out, arg, arg_len);
        out += arg_len;
    }
    if (off != fr->payload_len) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "trailing bytes");
        return;
    }
    if (!v2_runtime_lmux_established(a) || !hc2_exec_service(h) ||
        !h->structured_known_exec_incarnation) {
        if (!h->structured_known_exec_incarnation)
            (void)hc2_request_exec_info(a, h);
        if (v2_host_ensure_private_exec(a, h) < 0)
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_SERVICE_UNAVAILABLE, "exec unavailable");
        else
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_CONNECTED, "not connected");
        return;
    }
    e = hc2_exec_alloc(h);
    if (!e) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT, "exec refs");
        return;
    }
    e->active = 1;
    e->incarnation = h->structured_known_exec_incarnation;
    e->req_id = fr->request_id;
    e->next_status_ms = v2_now_ms() + 100;
    hc2_begin_control_op(h, V2_HC2_CTL_START, fr->request_id, e, 1);
    if (hc2_queue_exec_record(h, V2_EXEC_REC_START, 0, 0,
                              payload, (uint16_t)out) < 0) {
        h->exec_active = h->exec_structured = 0;
        memset(&h->structured_control_op, 0, sizeof(h->structured_control_op));
        hc2_exec_free(NULL, e);
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT, "exec queue");
    }
}

static void hc2_handle_exec_attach(struct v2_app *a, struct v2_host *h,
                                   const struct hc2_frame *fr)
{
    struct v2_host_structured_exec *e;
    uint64_t ref;
    if (fr->payload_len < 8) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "bad attach");
        return;
    }
    ref = lmux_hc2_get_u64(fr->payload);
    e = hc2_exec_by_ref(h, ref);
    if (!e || !e->active || e->lost || !e->exec_id ||
        !h->structured_known_exec_incarnation ||
        e->incarnation != h->structured_known_exec_incarnation) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    if (h->exec_active) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_BUSY, "attachment busy");
        return;
    }
    if (!v2_runtime_lmux_established(a) || !hc2_exec_service(h) ||
        !h->structured_known_exec_incarnation) {
        if (!h->structured_known_exec_incarnation)
            (void)hc2_request_exec_info(a, h);
        if (v2_host_ensure_private_exec(a, h) < 0)
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_SERVICE_UNAVAILABLE, "exec unavailable");
        else
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_CONNECTED, "not connected");
        return;
    }
    if (h->exec_ctl_out_len != h->exec_ctl_out_pos ||
        hc2_queue_exec_record(h, V2_EXEC_REC_STATUS, 0, e->exec_id,
                              NULL, 0) < 0) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT,
                        "exec control queue");
        return;
    }
    hc2_begin_control_op(h, V2_HC2_CTL_ATTACH, fr->request_id, e, 4);
    h->exec_id = e->exec_id;
    h->exec_status_pending = 1;
}

static int hc2_ref_ok(struct v2_host *h, const struct hc2_frame *fr,
                      uint64_t *ref_out)
{
    struct v2_host_structured_exec *e;
    uint64_t ref;
    if (fr->payload_len < 8)
        return 0;
    ref = lmux_hc2_get_u64(fr->payload);
    e = hc2_exec_by_ref(h, ref);
    if (!e || !e->active || e->lost || !h->structured_known_exec_incarnation ||
        e->incarnation != h->structured_known_exec_incarnation)
        return 0;
    *ref_out = ref;
    return 1;
}

static void hc2_handle_exec_signal(struct v2_host *h, const struct hc2_frame *fr)
{
    struct v2_host_structured_exec *e;
    uint64_t ref;
    uint8_t sig, remote_sig;
    if (!hc2_ref_ok(h, fr, &ref) || fr->payload_len < 9) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    (void)ref;
    e = hc2_exec_by_ref(h, ref);
    sig = fr->payload[8];
    switch (sig) {
    case LMUX_HC2_SIG_INT: remote_sig = V2_EXEC_SIG_INT; break;
    case LMUX_HC2_SIG_TERM: remote_sig = V2_EXEC_SIG_TERM; break;
    case LMUX_HC2_SIG_KILL: remote_sig = V2_EXEC_SIG_KILL; break;
    case LMUX_HC2_SIG_HUP: remote_sig = V2_EXEC_SIG_HUP; break;
    default:
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "bad signal");
        return;
    }
    if (!e || !e->exec_id || h->exec_active) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_BUSY, "exec control busy");
        return;
    }
    if (h->exec_ctl_out_len != h->exec_ctl_out_pos ||
        hc2_queue_exec_record(h, V2_EXEC_REC_SIGNAL, 0,
                              e->exec_id, &remote_sig, 1) < 0) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT,
                        "exec control queue");
        return;
    }
    hc2_begin_control_op(h, V2_HC2_CTL_SIGNAL, fr->request_id, e, 5);
    h->exec_id = e->exec_id;
    h->exec_status_pending = 1;
    hc2_queue_error(h, fr->request_id, LMUX_HC2_OK, "ok");
}

static void hc2_handle_exec_stdin(struct v2_host *h, const struct hc2_frame *fr)
{
    struct v2_host_structured_exec *e;
    uint64_t ref;
    size_t data_len;
    if (!hc2_ref_ok(h, fr, &ref) || fr->payload_len < 8) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    e = hc2_exec_by_ref(h, ref);
    if (!e || !e->exec_id) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    data_len = fr->payload_len - 8;
    if (data_len > sizeof(e->stdin_buf) - e->stdin_len) {
        e->stdin_backpressure = 1;
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_BUSY, "stdin backpressure");
        return;
    }
    if (e->stdin_close_requested) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_IO, "stdin closed");
        return;
    }
    memcpy(e->stdin_buf + e->stdin_len, fr->payload + 8, data_len);
    e->stdin_len += data_len;
    hc2_queue_error(h, fr->request_id, LMUX_HC2_OK, "ok");
}

static void hc2_handle_exec_close_stdin(struct v2_host *h, const struct hc2_frame *fr)
{
    struct v2_host_structured_exec *e;
    uint64_t ref;
    if (!hc2_ref_ok(h, fr, &ref)) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    e = hc2_exec_by_ref(h, ref);
    if (!e || !e->exec_id) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    e->stdin_close_requested = 1;
    hc2_queue_error(h, fr->request_id, LMUX_HC2_OK, "ok");
}

static void hc2_handle_exec_status_or_release(struct v2_host *h,
                                              const struct hc2_frame *fr,
                                              int release)
{
    struct v2_host_structured_exec *e;
    uint64_t ref;
    if (!hc2_ref_ok(h, fr, &ref)) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    e = hc2_exec_by_ref(h, ref);
    if (!e || !e->exec_id) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_NOT_FOUND, "bad exec ref");
        return;
    }
    if (!release && e->done && e->final_queued) {
        (void)hc2_queue_exec_state_for_ref(h, LMUX_HC2_EXEC_STATE,
                                           fr->request_id, e->ref,
                                           e->exec_id, e->final_state,
                                           e->final_fail, e->final_errno,
                                           e->final_exit_code,
                                           e->final_term_signal,
                                           e->final_payload,
                                           e->final_payload_len);
        return;
    }
    if (h->exec_active) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_BUSY, "exec control busy");
        return;
    }
    if (h->exec_ctl_out_len != h->exec_ctl_out_pos ||
        hc2_queue_exec_record(h, release ? V2_EXEC_REC_RELEASE : V2_EXEC_REC_STATUS,
                              0, e->exec_id, NULL, 0) < 0) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT,
                        "exec control queue");
        return;
    }
    hc2_begin_control_op(h, release ? V2_HC2_CTL_RELEASE : V2_HC2_CTL_STATUS,
                         fr->request_id, e, release ? 6 : 4);
    h->exec_id = e->exec_id;
    h->exec_status_pending = 1;
}

static struct v2_host_service *hc2_file_service(struct v2_host *h)
{
    int i;
    struct v2_host_service *s = v2_host_service_find_name(h->services, "files");
    if (s && s->type == V2_SERVICE_FILE && s->id)
        return s;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (h->services[i].used && h->services[i].type == V2_SERVICE_FILE &&
            h->services[i].id)
            return &h->services[i];
    return NULL;
}

static int hc2_file_open_stream(struct v2_app *a, struct v2_host *h,
                                struct v2_host_service *s)
{
    uint64_t stream_id = 0;
    if (s->active_stream_id || v2_host_service_has_active_binding(s) ||
        s->pending_fd >= 0)
        return -1;
    if (v2_runtime_open_stream_token(a, s->id, 0, &stream_id) < 0)
        return -1;
    s->active_stream_id = stream_id;
    s->last_stream_id = stream_id;
    h->structured_file_stream = stream_id;
    v2_runtime_count_stream_open(a);
    return 0;
}

static int hc2_file_prepare_put(struct v2_app *a, struct v2_host *h,
                                const char *local, const char *target,
                                uint64_t req_id)
{
    struct v2_host_service *s;
    struct stat st;
    uint8_t p[16 + LXF_PATH_MAX];
    size_t path_len;
    int fd;
    if (!hc2_abs_path_ok(local) || !hc2_abs_path_ok(target))
        return -1;
    fd = open(local, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    s = hc2_file_service(h);
    if (!v2_runtime_lmux_established(a) || !s) {
        close(fd);
        errno = ENOTCONN;
        return -1;
    }
    if (hc2_file_open_stream(a, h, s) < 0) {
        close(fd);
        errno = EBUSY;
        return -1;
    }
    path_len = strlen(target) + 1;
    lmux_put_le32(p, (uint32_t)(st.st_mode & 0777));
    lmux_put_le64(p + 4, (uint64_t)st.st_size);
    lmux_put_le32(p + 12, LXF_F_SIZE_KNOWN);
    memcpy(p + 16, target, path_len);
    if (hc2_file_queue_lxf(h, LXF_T_PUT_REQUEST, p, (uint32_t)(16 + path_len)) < 0) {
        close(fd);
        return -1;
    }
    h->structured_file_fd = fd;
    h->structured_file_active = 1;
    h->structured_file_kind = HC2_FILE_KIND_PUT;
    h->structured_file_phase = HC2_FILE_PUT_WAIT_OK;
    h->structured_file_req_id = req_id;
    h->structured_file_op_id = h->structured_next_file_op_id++;
    if (!h->structured_next_file_op_id)
        h->structured_next_file_op_id = 1;
    h->structured_file_expected = (uint64_t)st.st_size;
    h->structured_file_count = 0;
    h->structured_file_last_progress = 0;
    h->structured_file_mode = (uint32_t)(st.st_mode & 0777);
    strcpy(h->structured_file_local_path, local);
    strcpy(h->structured_file_target_path, target);
    sha256_init(&h->structured_file_sha);
    return 0;
}

static int hc2_file_prepare_get(struct v2_app *a, struct v2_host *h,
                                const char *target, const char *local,
                                uint64_t req_id)
{
    struct v2_host_service *s;
    struct stat st;
    size_t path_len;
    if (!hc2_abs_path_ok(target) || !hc2_abs_path_ok(local))
        return -1;
    if (lstat(local, &st) == 0 && !S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    if (hc2_make_tmp_for_final(local, h->structured_file_tmp_path,
                               sizeof(h->structured_file_tmp_path)) < 0) {
        errno = EINVAL;
        return -1;
    }
    s = hc2_file_service(h);
    if (!v2_runtime_lmux_established(a) || !s) {
        errno = ENOTCONN;
        return -1;
    }
    if (hc2_file_open_stream(a, h, s) < 0) {
        errno = EBUSY;
        return -1;
    }
    path_len = strlen(target) + 1;
    if (hc2_file_queue_lxf(h, LXF_T_GET_REQUEST, (const uint8_t *)target,
                           (uint32_t)path_len) < 0)
        return -1;
    h->structured_file_active = 1;
    h->structured_file_kind = HC2_FILE_KIND_GET;
    h->structured_file_phase = HC2_FILE_GET_WAIT_OK;
    h->structured_file_req_id = req_id;
    h->structured_file_op_id = h->structured_next_file_op_id++;
    if (!h->structured_next_file_op_id)
        h->structured_next_file_op_id = 1;
    h->structured_file_expected = 0;
    h->structured_file_count = 0;
    h->structured_file_last_progress = 0;
    strcpy(h->structured_file_local_path, local);
    strcpy(h->structured_file_target_path, target);
    sha256_init(&h->structured_file_sha);
    return 0;
}

static void hc2_handle_file_start(struct v2_app *a, struct v2_host *h,
                                  const struct hc2_frame *fr, int is_put)
{
    const uint8_t *a0, *a1;
    uint16_t a0_len, a1_len;
    char p0[LXF_PATH_MAX], p1[LXF_PATH_MAX];
    size_t off = 0;
    int rc;
    if (h->structured_file_active) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_BUSY, "file busy");
        return;
    }
    if (hc2_get_string(fr->payload, fr->payload_len, &off, &a0, &a0_len) < 0 ||
        hc2_get_string(fr->payload, fr->payload_len, &off, &a1, &a1_len) < 0 ||
        off != fr->payload_len || a0_len == 0 || a1_len == 0 ||
        a0_len >= sizeof(p0) || a1_len >= sizeof(p1)) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_INVALID_ARGUMENT, "bad file request");
        return;
    }
    memcpy(p0, a0, a0_len); p0[a0_len] = 0;
    memcpy(p1, a1, a1_len); p1[a1_len] = 0;
    rc = is_put ? hc2_file_prepare_put(a, h, p0, p1, fr->request_id) :
        hc2_file_prepare_get(a, h, p0, p1, fr->request_id);
    if (rc < 0) {
        uint32_t code = errno == ENOTCONN ? LMUX_HC2_E_NOT_CONNECTED :
            (errno == EBUSY ? LMUX_HC2_E_BUSY :
             (errno == ENOENT ? LMUX_HC2_E_NOT_FOUND :
              (errno == EACCES ? LMUX_HC2_E_IO : LMUX_HC2_E_INVALID_ARGUMENT)));
        hc2_file_clear(a, h, 1);
        hc2_queue_error(h, fr->request_id, code, strerror(errno));
        return;
    }
    if (hc2_queue_file_started(h) < 0) {
        hc2_file_clear(a, h, 1);
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT, "file started queue");
    }
}

static void hc2_file_maybe_progress(struct v2_host *h)
{
    if (h->structured_file_count - h->structured_file_last_progress <
        HC2_FILE_PROGRESS_STEP &&
        h->structured_file_count != h->structured_file_expected)
        return;
    if (hc2_queue_file_progress(h) == 0)
        h->structured_file_last_progress = h->structured_file_count;
}

static void hc2_file_remote_error(struct v2_app *a, struct v2_host *h,
                                  const uint8_t *p, uint32_t len)
{
    uint32_t code = LMUX_HC2_E_IO;
    const char *msg = "remote file error";
    if (len >= 5) {
        uint32_t lxf_code = lmux_get_le32(p);
        msg = (const char *)p + 4;
        if (memchr(msg, 0, len - 4) == NULL)
            msg = lxf_strerror_code(lxf_code);
        switch (lxf_code) {
        case LXF_E_NOTFOUND:
            code = LMUX_HC2_E_NOT_FOUND;
            break;
        case LXF_E_BADPATH:
        case LXF_E_NOTREG:
        case LXF_E_PROTOCOL:
            code = LMUX_HC2_E_INVALID_ARGUMENT;
            break;
        case LXF_E_NOSPACE:
            code = LMUX_HC2_E_RESOURCE_LIMIT;
            break;
        default:
            code = LMUX_HC2_E_IO;
            break;
        }
    }
    (void)hc2_queue_file_error_frame(h, code, msg);
    hc2_file_clear(a, h, 1);
}

static void hc2_file_fail(struct v2_app *a, struct v2_host *h,
                          uint32_t code, const char *msg)
{
    (void)hc2_queue_file_error_frame(h, code, msg);
    hc2_file_clear(a, h, 1);
}

static void hc2_file_complete(struct v2_app *a, struct v2_host *h)
{
    (void)hc2_queue_file_done(h);
    hc2_file_clear(a, h, 1);
}

static void hc2_file_step_put(struct v2_app *a, struct v2_host *h)
{
    uint8_t type;
    const uint8_t *payload;
    uint32_t len;
    int budget = 4;
    if (!hc2_file_flush_lxf(a, h))
        return;
    if (h->structured_file_phase == HC2_FILE_PUT_WAIT_OK ||
        h->structured_file_phase == HC2_FILE_PUT_WAIT_DONE) {
        int rr = hc2_file_read_lxf(a, h, &type, &payload, &len);
        if (rr < 0) {
            hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "bad file response");
            return;
        }
        if (rr == 0)
            return;
        if (type == LXF_T_ERROR) {
            hc2_file_remote_error(a, h, payload, len);
            return;
        }
        if (h->structured_file_phase == HC2_FILE_PUT_WAIT_OK) {
            if (type != LXF_T_OK) {
                hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "bad PUT start response");
                return;
            }
            hc2_file_consume_lxf(h);
            h->structured_file_phase = HC2_FILE_PUT_SEND;
        } else {
            if (type != LXF_T_OK || len < 40 ||
                lmux_get_le64(payload) != h->structured_file_count ||
                memcmp(payload + 8, h->structured_file_digest, 32) != 0) {
                hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "PUT verification mismatch");
                return;
            }
            hc2_file_complete(a, h);
            return;
        }
    }
    while (h->structured_file_phase == HC2_FILE_PUT_SEND && budget-- > 0) {
        uint8_t data[LXF_MAX_PAYLOAD], done[40];
        ssize_t r;
        if (h->structured_file_out_len != h->structured_file_out_pos)
            return;
        r = read(h->structured_file_fd, data, sizeof(data));
        if (r > 0) {
            sha256_update(&h->structured_file_sha, data, (size_t)r);
            h->structured_file_count += (uint64_t)r;
            if (hc2_file_queue_lxf(h, LXF_T_DATA, data, (uint32_t)r) < 0) {
                hc2_file_fail(a, h, LMUX_HC2_E_INTERNAL, "PUT queue failed");
                return;
            }
            hc2_file_maybe_progress(h);
            if (!hc2_file_flush_lxf(a, h))
                return;
        } else if (r == 0) {
            sha256_final(&h->structured_file_sha, h->structured_file_digest);
            lmux_put_le64(done, h->structured_file_count);
            memcpy(done + 8, h->structured_file_digest, 32);
            if (hc2_file_queue_lxf(h, LXF_T_DONE, done, sizeof(done)) < 0) {
                hc2_file_fail(a, h, LMUX_HC2_E_INTERNAL, "PUT done queue failed");
                return;
            }
            v2_host_close_fd(&h->structured_file_fd);
            h->structured_file_phase = HC2_FILE_PUT_WAIT_DONE;
            (void)hc2_file_flush_lxf(a, h);
            return;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            hc2_file_fail(a, h, LMUX_HC2_E_IO, strerror(errno));
            return;
        } else {
            return;
        }
    }
}

static void hc2_file_step_get(struct v2_app *a, struct v2_host *h)
{
    uint8_t type;
    const uint8_t *payload;
    uint32_t len;
    int budget = 8;
    if (!hc2_file_flush_lxf(a, h))
        return;
    while (budget-- > 0) {
        int rr = hc2_file_read_lxf(a, h, &type, &payload, &len);
        if (rr < 0) {
            hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "bad file response");
            return;
        }
        if (rr == 0)
            return;
        if (type == LXF_T_ERROR) {
            hc2_file_remote_error(a, h, payload, len);
            return;
        }
        if (h->structured_file_phase == HC2_FILE_GET_WAIT_OK) {
            if (type != LXF_T_OK || len < 16) {
                hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "bad GET metadata");
                return;
            }
            h->structured_file_mode = lmux_get_le32(payload) & 0777;
            h->structured_file_expected = lmux_get_le64(payload + 4);
            h->structured_file_fd = open(h->structured_file_tmp_path,
                                         O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (h->structured_file_fd < 0) {
                hc2_file_fail(a, h, LMUX_HC2_E_IO, strerror(errno));
                return;
            }
            h->structured_file_phase = HC2_FILE_GET_RECV;
            hc2_file_consume_lxf(h);
            continue;
        }
        if (type == LXF_T_DATA) {
            if (hc2_full_write_fd(h->structured_file_fd, payload, len) < 0) {
                hc2_file_fail(a, h, LMUX_HC2_E_IO, strerror(errno));
                return;
            }
            sha256_update(&h->structured_file_sha, payload, len);
            h->structured_file_count += len;
            hc2_file_maybe_progress(h);
            hc2_file_consume_lxf(h);
            continue;
        }
        if (type == LXF_T_DONE) {
            uint8_t digest[32];
            if (len < 40) {
                hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "short GET done");
                return;
            }
            sha256_final(&h->structured_file_sha, digest);
            if (lmux_get_le64(payload) != h->structured_file_count ||
                h->structured_file_count != h->structured_file_expected ||
                memcmp(payload + 8, digest, 32) != 0) {
                hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "GET verification mismatch");
                return;
            }
            memcpy(h->structured_file_digest, digest, 32);
            (void)fchmod(h->structured_file_fd, h->structured_file_mode);
            (void)fsync(h->structured_file_fd);
            v2_host_close_fd(&h->structured_file_fd);
            if (rename(h->structured_file_tmp_path, h->structured_file_local_path) < 0) {
                hc2_file_fail(a, h, LMUX_HC2_E_IO, strerror(errno));
                return;
            }
            h->structured_file_tmp_path[0] = 0;
            hc2_file_complete(a, h);
            return;
        }
        hc2_file_fail(a, h, LMUX_HC2_E_PROTOCOL, "unexpected GET record");
        return;
    }
}

static void hc2_file_step(struct v2_app *a, struct v2_host *h)
{
    int state;
    if (!h->structured_file_active)
        return;
    state = v2_runtime_stream_state(a, h->structured_file_stream);
    if (state == LMUX2_STREAM_TERMINAL || state == LMUX2_STREAM_RESET ||
        state == LMUX2_STREAM_UNUSED) {
        hc2_file_fail(a, h, LMUX_HC2_E_TARGET_LOST, "file stream lost");
        return;
    }
    if (h->structured_file_kind == HC2_FILE_KIND_PUT)
        hc2_file_step_put(a, h);
    else if (h->structured_file_kind == HC2_FILE_KIND_GET)
        hc2_file_step_get(a, h);
}

static void hc2_exec_read_stream(struct v2_app *a, struct v2_host *h,
                                 struct v2_host_structured_exec *e,
                                 uint64_t stream_id, uint16_t frame_type)
{
    uint8_t buf[520];
    size_t n;
    while (stream_id &&
           v2_host_structured_can_queue(h, (uint32_t)(8 + 512))) {
        n = v2_runtime_stream_read(a, stream_id, buf + 8, 512);
        if (!n)
            break;
        lmux_hc2_put_u64(buf, e->ref);
        if (v2_host_structured_queue_exec_frame(h, frame_type, 0, buf,
                                                (uint32_t)(8 + n)) < 0)
            break;
    }
}

static void hc2_exec_pump_stdin_entry(struct v2_app *a, struct v2_host *h,
                                      struct v2_host_structured_exec *e)
{
    if (!e->stdin_stream ||
        !v2_runtime_stream_is_open(a, e->stdin_stream))
        return;
    while (e->stdin_pos < e->stdin_len &&
           v2_runtime_stream_tx_len(a, e->stdin_stream) < LMUX2_STREAM_TX_CAP) {
        size_t n = e->stdin_len - e->stdin_pos;
        size_t w = v2_runtime_stream_write(a, e->stdin_stream,
                                           e->stdin_buf + e->stdin_pos, n);
        if (!w)
            break;
        e->stdin_pos += w;
    }
    if (e->stdin_pos > 0) {
        if (e->stdin_pos < e->stdin_len)
            memmove(e->stdin_buf, e->stdin_buf + e->stdin_pos,
                    e->stdin_len - e->stdin_pos);
        e->stdin_len -= e->stdin_pos;
        e->stdin_pos = 0;
        if (e->stdin_backpressure &&
            e->stdin_len < sizeof(e->stdin_buf))
            e->stdin_ready_pending = 1;
    }
    if (e->stdin_ready_pending &&
        hc2_queue_exec_stdin_ready(h, e) == 0) {
        e->stdin_ready_pending = 0;
        e->stdin_backpressure = 0;
    }
    if (e->stdin_close_requested && e->stdin_pos == e->stdin_len &&
        v2_runtime_stream_tx_len(a, e->stdin_stream) == 0 &&
        !e->stdin_close_pending) {
        (void)v2_runtime_close_stream(a, e->stdin_stream);
        e->stdin_close_pending = 1;
        e->stdin_close_committed = 1;
    }
}

static int hc2_exec_outputs_drained(struct v2_app *a,
                                    struct v2_host_structured_exec *e)
{
    return hc2_stream_drained(a, e->stdout_stream) &&
        hc2_stream_drained(a, e->stderr_stream);
}

static void hc2_exec_forget_dead_attachments(struct v2_app *a,
                                             struct v2_host_structured_exec *e)
{
    int st;
    if (e->stdin_stream) {
        st = v2_runtime_stream_state(a, e->stdin_stream);
        if (st == LMUX2_STREAM_RESET || st == LMUX2_STREAM_UNUSED) {
            e->stdin_stream = 0;
            if (!e->stdin_close_committed) {
                e->stdin_close_pending = 0;
                e->stdin_stream_opened = 0;
            }
        } else if (st == LMUX2_STREAM_TERMINAL) {
            e->stdin_stream = 0;
            if (e->stdin_pos < e->stdin_len) {
                e->stdin_close_pending = 0;
                e->stdin_stream_opened = 0;
            } else if (e->stdin_close_requested && !e->stdin_close_committed) {
                e->stdin_close_pending = 0;
                e->stdin_stream_opened = 0;
            } else if (e->stdin_close_requested) {
                e->stdin_close_pending = 1;
            } else {
                e->stdin_close_pending = 0;
            }
        }
    }
    if (e->stdout_stream) {
        st = v2_runtime_stream_state(a, e->stdout_stream);
        if (st == LMUX2_STREAM_RESET || st == LMUX2_STREAM_UNUSED)
            e->stdout_stream = 0;
    }
    if (e->stderr_stream) {
        st = v2_runtime_stream_state(a, e->stderr_stream);
        if (st == LMUX2_STREAM_RESET || st == LMUX2_STREAM_UNUSED)
            e->stderr_stream = 0;
    }
}

static void hc2_exec_queue_final_if_ready(struct v2_app *a, struct v2_host *h,
                                          struct v2_host_structured_exec *e)
{
    if (!e->final_pending || e->final_queued ||
        !hc2_exec_outputs_drained(a, e))
        return;
    if (hc2_queue_exec_state_for_ref(h, LMUX_HC2_EXEC_STATE, e->req_id,
                                     e->ref, e->exec_id, e->final_state,
                                     e->final_fail, e->final_errno,
                                     e->final_exit_code, e->final_term_signal,
                                     e->final_payload,
                                     e->final_payload_len) == 0) {
        e->final_queued = 1;
        e->done = 1;
        hc2_exec_close_streams(a, e);
    }
}

static void hc2_exec_start_status_tx(struct v2_host *h,
                                     struct v2_host_structured_exec *e)
{
    hc2_begin_control_op(h, V2_HC2_CTL_STATUS, e->req_id, e, 4);
    h->exec_id = e->exec_id;
    if (hc2_queue_exec_record(h, V2_EXEC_REC_STATUS, 0, e->exec_id,
                              NULL, 0) < 0) {
        h->exec_active = 0;
        h->exec_structured = 0;
        h->exec_phase = 0;
        h->exec_id = 0;
        memset(&h->structured_control_op, 0, sizeof(h->structured_control_op));
        return;
    }
    e->status_pending = 1;
    e->next_status_ms = v2_now_ms() + 100;
}

static void hc2_exec_table_step(struct v2_app *a, struct v2_host *h)
{
    struct v2_host_service *s = hc2_exec_service(h);
    int i;
    if (!v2_runtime_lmux_established(a) || !s || !h->structured_ready)
        return;
    if (!h->structured_known_exec_incarnation) {
        (void)hc2_request_exec_info(a, h);
        return;
    }
    if (h->exec_active)
        return;
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++) {
        struct v2_host_structured_exec *e = &h->structured_execs[i];
        if (!e->used || !e->active || e->lost || !e->exec_id)
            continue;
        if (e->incarnation != h->structured_known_exec_incarnation) {
            hc2_mark_entry_lost(h, e, LMUX_HC2_E_STALE_TARGET);
            continue;
        }
        hc2_exec_forget_dead_attachments(a, e);
        if (e->auto_reattach) {
            if (hc2_queue_exec_state_for_ref(h, LMUX_HC2_EXEC_STATE, 0,
                                             e->ref, e->exec_id,
                                             V2_EXEC_STATUS_RUNNING, 0, 0,
                                             0, 0, NULL, 0) < 0)
                continue;
            e->auto_reattach = 0;
        }
        if (!e->stdout_stream)
            (void)v2_runtime_open_stream_token(a, s->id,
                v2_exec_stream_token(e->exec_id, V2_EXEC_ROLE_STDOUT),
                &e->stdout_stream);
        if (!e->stderr_stream)
            (void)v2_runtime_open_stream_token(a, s->id,
                v2_exec_stream_token(e->exec_id, V2_EXEC_ROLE_STDERR),
                &e->stderr_stream);
        if ((e->stdin_pos < e->stdin_len ||
             (e->stdin_close_requested && !e->stdin_close_committed &&
              !e->stdin_stream_opened)) &&
            !e->stdin_stream && !e->stdin_close_pending &&
            !e->stdin_stream_opened) {
            if (v2_runtime_open_stream_token(a, s->id,
                    v2_exec_stream_token(e->exec_id, V2_EXEC_ROLE_STDIN),
                    &e->stdin_stream) == 0)
                e->stdin_stream_opened = 1;
        }
        hc2_exec_read_stream(a, h, e, e->stdout_stream, LMUX_HC2_EXEC_STDOUT);
        hc2_exec_read_stream(a, h, e, e->stderr_stream, LMUX_HC2_EXEC_STDERR);
        hc2_exec_pump_stdin_entry(a, h, e);
        hc2_exec_queue_final_if_ready(a, h, e);
        if (!e->done && !e->final_pending && !e->status_pending &&
            !h->exec_active && v2_now_ms() >= e->next_status_ms)
            hc2_exec_start_status_tx(h, e);
    }
}

static void hc2_handle_frame(struct v2_app *a, struct v2_host *h,
                             const struct v2_host_status *st,
                             const struct hc2_frame *fr, int allow_queue)
{
    if (fr->type != LMUX_HC2_CLIENT_HELLO && !h->structured_ready) {
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_PROTOCOL, "hello required");
        return;
    }
    if (fr->type != LMUX_HC2_CLIENT_HELLO && fr->request_id == 0) {
        hc2_queue_error(h, 0, LMUX_HC2_E_PROTOCOL, "request id required");
        return;
    }
    if (allow_queue && h->exec_active &&
        hc2_control_frame_schedulable(fr->type)) {
        if (hc2_enqueue_control_frame(h, fr) < 0)
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT,
                            "exec control queue full");
        return;
    }
    switch (fr->type) {
    case LMUX_HC2_CLIENT_HELLO:
        if (h->structured_ready) {
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_PROTOCOL,
                            "duplicate hello");
            break;
        }
        h->structured_ready = 1;
        if (hc2_queue_server_hello(h, fr->request_id) < 0)
            hc2_close_client(h);
        else if (v2_runtime_lmux_established(a) && hc2_exec_service(h) &&
                 !h->structured_known_exec_incarnation && !h->exec_active)
            (void)hc2_request_exec_info(a, h);
        break;
    case LMUX_HC2_STATUS_REQUEST:
        if (hc2_queue_status(h, st, fr->request_id) < 0)
            hc2_queue_error(h, fr->request_id, LMUX_HC2_E_RESOURCE_LIMIT, "status too large");
        break;
    case LMUX_HC2_EXEC_START:
        hc2_handle_exec_start(a, h, fr);
        break;
    case LMUX_HC2_EXEC_ATTACH:
        hc2_handle_exec_attach(a, h, fr);
        break;
    case LMUX_HC2_EXEC_STDIN_DATA:
        hc2_handle_exec_stdin(h, fr);
        break;
    case LMUX_HC2_EXEC_STDIN_CLOSE:
        hc2_handle_exec_close_stdin(h, fr);
        break;
    case LMUX_HC2_EXEC_SIGNAL:
        hc2_handle_exec_signal(h, fr);
        break;
    case LMUX_HC2_EXEC_STATUS:
        hc2_handle_exec_status_or_release(h, fr, 0);
        break;
    case LMUX_HC2_EXEC_RELEASE:
        hc2_handle_exec_status_or_release(h, fr, 1);
        break;
    case LMUX_HC2_FILE_PUT_START:
        hc2_handle_file_start(a, h, fr, 1);
        break;
    case LMUX_HC2_FILE_GET_START:
        hc2_handle_file_start(a, h, fr, 0);
        break;
    default:
        hc2_queue_error(h, fr->request_id, LMUX_HC2_E_PROTOCOL, "unknown frame");
        break;
    }
}

static void hc2_drain_control_queue(struct v2_app *a, struct v2_host *h,
                                    const struct v2_host_status *st)
{
    while (h->structured_control_queue_len > 0 && !h->exec_active &&
           h->structured_fd >= 0) {
        struct v2_host_structured_pending_frame *q =
            &h->structured_control_queue[h->structured_control_queue_head];
        struct hc2_frame fr;
        fr.type = q->type;
        fr.flags = q->flags;
        fr.request_id = q->request_id;
        fr.payload = q->payload;
        fr.payload_len = q->payload_len;
        h->structured_control_queue_head =
            (h->structured_control_queue_head + 1) %
            V2_HOST_STRUCTURED_CTL_QUEUE_MAX;
        h->structured_control_queue_len--;
        hc2_handle_frame(a, h, st, &fr, 0);
    }
}

int v2_host_structured_start(struct v2_host *h)
{
    if (!h->structured_path)
        return 0;
    h->structured_listen_fd =
        v2_host_create_listener(h->structured_path, &h->structured_created_path);
    if (h->structured_listen_fd < 0)
        return -1;
    if (chmod(h->structured_path, 0777) < 0) {
        close(h->structured_listen_fd);
        h->structured_listen_fd = -1;
        if (h->structured_created_path) {
            unlink(h->structured_path);
            h->structured_created_path = 0;
        }
        return -1;
    }
    return 0;
}

void v2_host_structured_accept(struct v2_host *h)
{
    int fd = accept(h->structured_listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    if (h->structured_fd >= 0) {
        close(fd);
        return;
    }
    if (v2_host_set_nonblock(fd) < 0) {
        close(fd);
        return;
    }
    h->structured_fd = fd;
    h->structured_ready = 0;
    h->structured_rx_len = 0;
    h->structured_tx_len = h->structured_tx_pos = 0;
    h->structured_control_queue_head = 0;
    h->structured_control_queue_len = 0;
}

short v2_host_structured_poll_events(const struct v2_host *h)
{
    short ev = 0;
    if (h->structured_fd < 0)
        return 0;
    if (h->structured_rx_len < sizeof(h->structured_rx))
        ev |= POLLIN;
    if (h->structured_tx_pos < h->structured_tx_len)
        ev |= POLLOUT;
    return ev;
}

void v2_host_structured_flush_tx(struct v2_app *a, struct v2_host *h)
{
    if (h->structured_fd < 0)
        return;
    while (h->structured_tx_pos < h->structured_tx_len) {
        ssize_t w = write(h->structured_fd,
                          h->structured_tx + h->structured_tx_pos,
                          h->structured_tx_len - h->structured_tx_pos);
        if (w > 0) {
            h->structured_tx_pos += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        hc2_close_client_runtime(a, h);
        return;
    }
    if (h->structured_tx_pos == h->structured_tx_len)
        h->structured_tx_pos = h->structured_tx_len = 0;
}

void v2_host_structured_service(struct v2_app *a, struct v2_host *h,
                                const struct v2_host_status *st, short revents)
{
    if (h->structured_fd < 0)
        return;
    if (revents & (POLLIN | POLLHUP)) {
        for (;;) {
            ssize_t r;
            int parsed;
            if (h->structured_rx_len == sizeof(h->structured_rx)) {
                hc2_queue_error(h, 0, LMUX_HC2_E_PROTOCOL, "rx overflow");
                break;
            }
            r = read(h->structured_fd, h->structured_rx + h->structured_rx_len,
                     sizeof(h->structured_rx) - h->structured_rx_len);
            if (r > 0) {
                h->structured_rx_len += (size_t)r;
                do {
                    struct hc2_frame fr;
                    parsed = hc2_parse_frame(h, &fr);
                    if (parsed < 0) {
                        hc2_close_client_runtime(a, h);
                        return;
                    }
                    if (parsed > 0) {
                        hc2_handle_frame(a, h, st, &fr, 1);
                        if (h->structured_fd < 0)
                            return;
                        hc2_consume_frame(h, &fr);
                    }
                } while (parsed > 0);
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (r == 0 || (revents & POLLIN)) {
                hc2_close_client_runtime(a, h);
                return;
            }
            break;
        }
    }
    if (h->structured_fd >= 0 && (revents & POLLOUT))
        v2_host_structured_flush_tx(a, h);
    if (h->structured_fd >= 0 && (revents & (POLLERR | POLLNVAL)))
        hc2_close_client_runtime(a, h);
}

static void hc2_fail_inflight_on_remote_down(struct v2_host *h)
{
    struct v2_host_structured_exec *e;
    uint64_t req = h->structured_control_op.request_id;
    switch (h->structured_control_op.kind) {
    case V2_HC2_CTL_START:
        e = hc2_exec_by_ref(h, h->structured_control_op.ref);
        if (!e || e->exec_id)
            break;
        hc2_exec_free(NULL, e);
        if (h->structured_fd >= 0 && h->structured_ready && req)
            hc2_queue_error(h, req, LMUX_HC2_E_TARGET_LOST, "remote reset");
        break;
    case V2_HC2_CTL_STATUS:
        e = hc2_exec_by_ref(h, h->structured_control_op.ref);
        if (e && req == e->req_id) {
            e->status_pending = 0;
            break;
        }
        if (h->structured_fd >= 0 && h->structured_ready && req)
            hc2_queue_error(h, req, LMUX_HC2_E_TARGET_LOST, "remote reset");
        break;
    case V2_HC2_CTL_ATTACH:
    case V2_HC2_CTL_RELEASE:
        if (h->structured_fd >= 0 && h->structured_ready && req)
            hc2_queue_error(h, req, LMUX_HC2_E_TARGET_LOST, "remote reset");
        break;
    case V2_HC2_CTL_SIGNAL:
    case V2_HC2_CTL_INFO:
    case V2_HC2_CTL_NONE:
    default:
        break;
    }
}

void v2_host_structured_tick(struct v2_app *a, struct v2_host *h,
                             const struct v2_host_status *st)
{
    (void)st;
    if (h->structured_fd >= 0 && h->structured_ready &&
        v2_runtime_lmux_established(a) && hc2_exec_service(h) &&
        !h->structured_known_exec_incarnation && !h->exec_active)
        (void)hc2_request_exec_info(a, h);
    if (h->structured_known_exec_incarnation)
        v2_host_structured_check_exec_incarnation(h);
    hc2_drain_control_queue(a, h, st);
    hc2_exec_table_step(a, h);
    hc2_file_step(a, h);
}

void v2_host_structured_on_exec_info(struct v2_host *h,
                                     uint64_t exec_incarnation)
{
    h->structured_known_exec_incarnation = exec_incarnation;
    h->structured_exec_info_pending = 0;
}

void v2_host_structured_check_exec_incarnation(struct v2_host *h)
{
    int i;
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++) {
        struct v2_host_structured_exec *e = &h->structured_execs[i];
        if (e->used && e->active && !e->done && !e->lost &&
            e->incarnation &&
            e->incarnation != h->structured_known_exec_incarnation)
            hc2_mark_entry_lost(h, e, LMUX_HC2_E_STALE_TARGET);
    }
}

short v2_host_structured_file_poll_events(const struct v2_host *h)
{
    (void)h;
    return 0;
}

void v2_host_structured_file_event(struct v2_app *a, struct v2_host *h,
                                   short revents)
{
    (void)revents;
    hc2_file_step(a, h);
}

void v2_host_structured_on_exec_clear(struct v2_host *h)
{
    struct v2_host_structured_exec *e = hc2_exec_by_ref(h, h->structured_control_op.ref);
    if (!h->exec_structured)
        return;
    if (h->exec_phase == HC2_EXEC_MODE_INFO_ONCE)
        h->structured_exec_info_pending = 0;
    h->exec_structured = 0;
    if (e && h->exec_id)
        e->exec_id = h->exec_id;
    if (e && h->exec_done_seen)
        e->done = 1;
    memset(&h->structured_control_op, 0, sizeof(h->structured_control_op));
}

static void hc2_retire_exec_lmux_attachment(struct v2_host *h)
{
    if (!h->exec_structured)
        return;
    hc2_fail_inflight_on_remote_down(h);
    h->exec_active = 0;
    h->exec_phase = 0;
    h->exec_status_pending = 0;
    h->exec_close_pending = 0;
    h->exec_close_stdin_after_drain = 0;
    h->exec_done_seen = 0;
    h->exec_structured = 0;
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
    memset(&h->structured_control_op, 0, sizeof(h->structured_control_op));
}

void v2_host_structured_on_outer_down(struct v2_host *h)
{
    int i;
    if (h->structured_file_active) {
        if (h->structured_fd >= 0)
            (void)hc2_queue_file_error_frame(h, LMUX_HC2_E_TARGET_LOST,
                                             "file transfer lost");
        hc2_file_clear(NULL, h, 0);
    }
    for (i = 0; i < V2_HOST_STRUCTURED_MAX_EXECS; i++) {
        struct v2_host_structured_exec *e = &h->structured_execs[i];
        if (!e->used || !e->active || e->done || e->lost)
            continue;
        if (h->structured_fd >= 0 && h->structured_ready)
            e->auto_reattach = 1;
        e->stdin_stream = 0;
        e->stdout_stream = 0;
        e->stderr_stream = 0;
        e->stdin_stream_opened = 0;
        e->stdin_close_committed = 0;
        e->stdin_close_pending = 0;
        e->status_pending = 0;
    }
    hc2_retire_exec_lmux_attachment(h);
    h->structured_known_exec_incarnation = 0;
    h->structured_exec_info_pending = 0;
}
