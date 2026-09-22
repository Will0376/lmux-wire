#include "services.h"

#include <unistd.h>

void v2_unix_clear_pending(struct v2_app *a, struct v2_service *s)
{
    uint64_t token = s->pending_token;
    v2_close_fd(&s->pending_fd);
    s->pending_token = 0;
    if (token)
        (void)v2_runtime_queue_service_event(a, s, V2_EVENT_PENDING_CLEARED, token);
}

void v2_unix_accept_pending(struct v2_app *a, struct v2_service *s, int fd)
{
    if (s->pending_fd >= 0) {
        close(fd);
        return;
    }
    s->pending_fd = fd;
    s->pending_token = s->next_token++;
    if (s->next_token == 0)
        s->next_token = 1;
    (void)v2_runtime_queue_service_event(a, s, V2_EVENT_PENDING_CLIENT, s->pending_token);
    v2_app_log(a, 1, "lmux2 target pending client service=%s token=%llu",
               s->name, (unsigned long long)s->pending_token);
}

void v2_unix_pair_unbound_target_streams(struct v2_app *a, struct lmux2_conn *conn,
                                         struct v2_service services[V2_MAX_SERVICES])
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        int j, best = -1;
        uint64_t best_id = 0, token;
        struct v2_service *s = &services[i];
        if (!s->used || s->type != V2_SERVICE_UNIX_RENDEZVOUS || s->pending_fd < 0)
            continue;
        for (j = 0; j < LMUX2_CORE_MAX_STREAMS; j++) {
            struct lmux2_stream *st = &conn->streams[j];
            if (st->state != LMUX2_STREAM_OPEN || st->open_token ||
                st->service_id != s->id ||
                v2_service_find_binding(services, st->id, NULL))
                continue;
            if (st->id > best_id) {
                best_id = st->id;
                best = j;
            }
        }
        if (best >= 0) {
            token = s->pending_token;
            v2_service_bind_fd_to_stream(s, conn->streams[best].id, s->pending_fd);
            s->pending_fd = -1;
            s->pending_token = 0;
            if (token)
                (void)v2_runtime_queue_service_event(a, s, V2_EVENT_PENDING_CLEARED, token);
        }
    }
}
