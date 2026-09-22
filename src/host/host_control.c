#include "host.h"
#include "version.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
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
                       s->listen_path[0] ? s->listen_path :
                       (s->host_path[0] ? s->host_path :
                        (s->target_path[0] ? s->target_path : "-")),
                       s->pending_fd >= 0 ? "yes" : "no",
                       (unsigned long long)s->pending_event_token,
                       (unsigned long long)s->active_stream_id,
                       (unsigned long long)s->last_stream_id);
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
        control_printf(h, "factories: unix-rendezvous pty file\n");
    } else {
        control_printf(h, "factories: unix-rendezvous=%s pty=%s file=%s desired_sent=%s\n",
                       yesno(h->factory_unix), yesno(h->factory_pty),
                       yesno(h->factory_file), yesno(h->desired_sent));
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
    char cmd[16], name[V2_NAME_MAX], service[V2_NAME_MAX];
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
    } else {
        (void)write(h->control_fd, "ERR unknown command\n", 20);
        v2_host_close_fd(&h->control_fd);
    }
}

void v2_host_control_service(struct v2_app *a, struct v2_host *h,
                             const struct v2_host_status *st)
{
    char tmp[128];
    ssize_t r;
    if (h->control_fd < 0)
        return;
    r = read(h->control_fd, tmp, sizeof(tmp));
    if (r <= 0) {
        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            v2_host_close_fd(&h->control_fd);
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
