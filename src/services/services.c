#include "services.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int v2_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void v2_close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int v2_copy_bounded(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(out, in, n + 1);
    return 0;
}

int v2_create_listener(const char *path, int *created_path)
{
    struct sockaddr_un sa;
    struct stat st;
    int fd;
    if (strlen(path) >= sizeof(sa.sun_path))
        return -1;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode))
            return -1;
        if (unlink(path) < 0) {
            fprintf(stderr, "lmux2 create_listener unlink %s: %s\n", path, strerror(errno));
            return -1;
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, "lmux2 create_listener lstat %s: %s\n", path, strerror(errno));
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 8) < 0) {
        fprintf(stderr, "lmux2 create_listener bind/listen %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    *created_path = 1;
    return fd;
}

int v2_socket_peer_closed(int fd)
{
    uint8_t ch;
    ssize_t r;

    do {
        r = recv(fd, &ch, 1, MSG_PEEK);
    } while (r < 0 && errno == EINTR);
    if (r == 0)
        return 1;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (r < 0)
        return 1;
    return 0;
}

void v2_service_slots_init(struct v2_service services[V2_MAX_SERVICES])
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        services[i].listen_fd = -1;
        services[i].pending_fd = -1;
        services[i].pty_master_fd = -1;
        services[i].file.fd = -1;
        for (j = 0; j < V2_MAX_BINDINGS; j++)
            services[i].bindings[j].fd = -1;
    }
}

void v2_service_cleanup(struct v2_service *s)
{
    int i;
    v2_file_session_reset(&s->file);
    if (s->type == V2_SERVICE_PTY)
        v2_pty_terminate_child(s);
    v2_close_fd(&s->pty_master_fd);
    v2_close_fd(&s->pending_fd);
    v2_close_fd(&s->listen_fd);
    if (s->created_path && s->listen_path[0])
        unlink(s->listen_path);
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        v2_close_fd(&s->bindings[i].fd);
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->pending_fd = -1;
    s->pty_master_fd = -1;
    s->file.fd = -1;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        s->bindings[i].fd = -1;
}

struct v2_service *v2_service_find_id(struct v2_service services[V2_MAX_SERVICES],
                                      uint64_t id)
{
    int i;
    if (id == 0)
        return NULL;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (services[i].used && services[i].id == id)
            return &services[i];
    return NULL;
}

struct v2_service *v2_service_find_name(struct v2_service services[V2_MAX_SERVICES],
                                        const char *name)
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (services[i].used && strcmp(services[i].name, name) == 0)
            return &services[i];
    return NULL;
}

struct v2_service *v2_service_alloc(struct v2_service services[V2_MAX_SERVICES],
                                    uint64_t *next_service_id)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        if (!services[i].used) {
            memset(&services[i], 0, sizeof(services[i]));
            services[i].listen_fd = -1;
            services[i].pending_fd = -1;
            services[i].pty_master_fd = -1;
            services[i].file.fd = -1;
            for (j = 0; j < V2_MAX_BINDINGS; j++)
                services[i].bindings[j].fd = -1;
            services[i].used = 1;
            services[i].id = (*next_service_id)++;
            if (services[i].id == 0)
                services[i].id = (*next_service_id)++;
            services[i].next_token = 1;
            return &services[i];
        }
    }
    return NULL;
}

struct v2_binding *v2_service_find_binding(struct v2_service services[V2_MAX_SERVICES],
                                           uint64_t stream_id,
                                           struct v2_service **svc_out)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &services[i];
        if (!s->used)
            continue;
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            if (s->bindings[j].fd >= 0 && s->bindings[j].stream_id == stream_id) {
                if (svc_out)
                    *svc_out = s;
                return &s->bindings[j];
            }
        }
    }
    return NULL;
}

struct v2_binding *v2_service_alloc_binding(struct v2_service *s)
{
    int i;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        if (s->bindings[i].fd < 0)
            return &s->bindings[i];
    return NULL;
}

int v2_service_has_active_binding(const struct v2_service *s)
{
    int i;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        if (s->bindings[i].fd >= 0)
            return 1;
    return 0;
}

int v2_service_has_live_stream(const struct lmux2_conn *conn, const struct v2_service *s)
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

int v2_stream_has_endpoint_binding(struct v2_service services[V2_MAX_SERVICES],
                                   uint64_t stream_id)
{
    int i;
    if (v2_service_find_binding(services, stream_id, NULL))
        return 1;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &services[i];
        if (s->used && s->active_stream_id == stream_id)
            return 1;
    }
    return 0;
}

void v2_service_bind_fd_to_stream(struct v2_service *s, uint64_t stream_id, int fd)
{
    struct v2_binding *b = v2_service_alloc_binding(s);
    if (!b) {
        close(fd);
        return;
    }
    b->stream_id = stream_id;
    b->fd = fd;
    s->last_stream_id = stream_id;
}
