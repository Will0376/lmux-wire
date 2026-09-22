#include "host.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int host_service_can_accept_pending(const struct v2_host_service *s)
{
    if (s->pending_fd >= 0)
        return 0;
    if ((s->type == V2_SERVICE_PTY || s->type == V2_SERVICE_FILE) &&
        (s->active_stream_id || v2_host_service_has_active_binding(s)))
        return 0;
    return 1;
}

static void close_binding(struct v2_app *a, struct lmux2_conn *conn,
                          struct v2_host_service *s, struct v2_host_binding *b,
                          int send_close)
{
    uint64_t stream_id = b->stream_id;
    if (b->fd >= 0)
        v2_host_close_fd(&b->fd);
    if (send_close && stream_id) {
        (void)lmux2_close_stream(conn, stream_id);
        v2_runtime_count_stream_close(a);
        v2_app_log(a, 1, "lmux2 STREAM_CLOSE sent service=%s stream=%llu binding-detached",
                   s->name, (unsigned long long)stream_id);
    }
    b->stream_id = 0;
}

int v2_host_service_has_live_stream(const struct lmux2_conn *conn,
                                    const struct v2_host_service *s)
{
    int i;
    if (!s->id)
        return 0;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++) {
        const struct lmux2_stream *st = &conn->streams[i];
        if (st->service_id == s->id &&
            st->state != LMUX2_STREAM_UNUSED &&
            st->state != LMUX2_STREAM_TERMINAL &&
            st->state != LMUX2_STREAM_RESET)
            return 1;
    }
    return 0;
}

int v2_host_stream_can_send_endpoint_data(const struct lmux2_conn *conn,
                                          uint64_t stream_id)
{
    const struct lmux2_stream *st = lmux2_find_stream_const(conn, stream_id);
    if (!st)
        return 0;
    return st->state == LMUX2_STREAM_OPEN || st->state == LMUX2_STREAM_REMOTE_CLOSED;
}

int v2_host_endpoint_poll_events(const struct lmux2_conn *conn,
                                 const struct v2_host_binding *b)
{
    short ev = 0;
    if (b->fd < 0)
        return 0;
    if (v2_host_stream_can_send_endpoint_data(conn, b->stream_id) &&
        lmux2_stream_tx_len(conn, b->stream_id) < LMUX2_STREAM_TX_CAP)
        ev |= POLLIN;
    if (lmux2_stream_rx_len(conn, b->stream_id) > 0)
        ev |= POLLOUT;
    return ev;
}

void v2_host_attach_pending_clients(struct v2_app *a, struct v2_host *h,
                                    struct lmux2_conn *conn)
{
    int i;
    if (conn->state != LMUX2_CONN_ESTABLISHED)
        return;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_host_service *s = &h->services[i];
        uint64_t stream_id, token;
        if (!s->used || s->pending_fd < 0 || s->id == 0)
            continue;
        if (s->type == V2_SERVICE_UNIX_RENDEZVOUS && v2_host_service_has_live_stream(conn, s))
            continue;
        if ((s->type == V2_SERVICE_PTY || s->type == V2_SERVICE_FILE) &&
            (s->active_stream_id || v2_host_service_has_active_binding(s)))
            continue;
        token = 0;
        if (lmux2_open_stream_ex(conn, s->id, token, &stream_id) < 0)
            continue;
        v2_host_service_bind_fd_to_stream(s, stream_id, s->pending_fd);
        s->pending_fd = -1;
        v2_runtime_count_stream_open(a);
        v2_app_log(a, 1, "lmux2 STREAM_OPEN sent pending service=%s stream=%llu token=%llu",
                   s->name, (unsigned long long)stream_id, (unsigned long long)token);
    }
}

void v2_host_cleanup_terminal_bindings(struct v2_app *a, struct v2_host *h,
                                       struct lmux2_conn *conn)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_host_service *s = &h->services[i];
        if (!s->used)
            continue;
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            struct v2_host_binding *b = &s->bindings[j];
            struct lmux2_stream *st;
            if (b->fd < 0)
                continue;
            st = lmux2_find_stream(conn, b->stream_id);
            if (st && st->state == LMUX2_STREAM_TERMINAL &&
                st->open_token && st->remote_error == LMUX2_OPEN_E_BAD_STATE) {
                uint64_t stream_id;
                if (lmux2_open_stream_ex(conn, s->id, 0, &stream_id) == 0) {
                    v2_app_log(a, 1, "lmux2 host retrying stale-token client service=%s old_stream=%llu new_stream=%llu",
                               s->name, (unsigned long long)b->stream_id,
                               (unsigned long long)stream_id);
                    b->stream_id = stream_id;
                    s->last_stream_id = stream_id;
                    v2_runtime_count_stream_open(a);
                    continue;
                }
            }
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED)
                close_binding(a, conn, s, b, st && st->state == LMUX2_STREAM_REMOTE_CLOSED);
        }
        if ((s->type == V2_SERVICE_PTY || s->type == V2_SERVICE_FILE) &&
            s->active_stream_id) {
            const struct lmux2_stream *st = lmux2_find_stream_const(conn, s->active_stream_id);
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED) {
                if (st && st->state == LMUX2_STREAM_REMOTE_CLOSED) {
                    (void)lmux2_close_stream(conn, s->active_stream_id);
                    v2_runtime_count_stream_close(a);
                }
                s->active_stream_id = 0;
            }
        }
    }
}

void v2_host_accept_service_client(struct v2_app *a, struct v2_host *h,
                                   struct lmux2_conn *conn,
                                   struct v2_host_service *s)
{
    (void)h;
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);
        uint64_t stream_id, token = 0;
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            return;
        }
        if (v2_host_set_nonblock(fd) < 0) {
            close(fd);
            continue;
        }
        if (s->id == 0 || conn->state != LMUX2_CONN_ESTABLISHED ||
            (s->type == V2_SERVICE_UNIX_RENDEZVOUS && v2_host_service_has_live_stream(conn, s)) ||
            lmux2_open_stream_ex(conn, s->id, token, &stream_id) < 0) {
            if (!host_service_can_accept_pending(s)) {
                close(fd);
                continue;
            }
            s->pending_fd = fd;
            v2_app_log(a, 1, "lmux2 host local client pending service=%s reason=%s",
                       s->name, s->id == 0 ? "no-service" : "no-connection");
            continue;
        }
        v2_host_service_bind_fd_to_stream(s, stream_id, fd);
        v2_runtime_count_stream_open(a);
        v2_app_log(a, 1, "lmux2 STREAM_OPEN sent service=%s stream=%llu token=%llu",
                   s->name, (unsigned long long)stream_id, (unsigned long long)token);
    }
}

void v2_host_clear_pending(struct v2_host_service *s)
{
    v2_host_close_fd(&s->pending_fd);
}

void v2_host_deliver_stream_rx(struct v2_app *a, struct lmux2_conn *conn,
                               struct v2_host_service *s,
                               struct v2_host_binding *b)
{
    uint8_t buf[256];
    size_t n;
    if (b->fd < 0)
        return;
    while ((n = lmux2_stream_read(conn, b->stream_id, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(b->fd, buf + off, n - off);
            if (w > 0) {
                off += (size_t)w;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            close_binding(a, conn, s, b, 1);
            return;
        }
    }
}

void v2_host_collect_endpoint_tx(struct v2_app *a, struct lmux2_conn *conn,
                                 struct v2_host_service *s,
                                 struct v2_host_binding *b)
{
    uint8_t buf[256];
    size_t space;
    if (b->fd < 0)
        return;
    if (!v2_host_stream_can_send_endpoint_data(conn, b->stream_id))
        return;
    for (;;) {
        space = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(conn, b->stream_id);
        if (space == 0)
            return;
        if (space > sizeof(buf))
            space = sizeof(buf);
        ssize_t r = read(b->fd, buf, space);
        if (r > 0) {
            size_t w = lmux2_stream_write(conn, b->stream_id, buf, (size_t)r);
            if (w < (size_t)r)
                return;
            continue;
        }
        if (r == 0) {
            close_binding(a, conn, s, b, 1);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        close_binding(a, conn, s, b, 1);
        return;
    }
}

void v2_host_close_binding(struct v2_app *a, struct lmux2_conn *conn,
                           struct v2_host_service *s,
                           struct v2_host_binding *b,
                           int send_close)
{
    close_binding(a, conn, s, b, send_close);
}
