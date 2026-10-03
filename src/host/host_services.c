#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int split_unix_arg(const char *arg, char *name, char *host_path, char *target_path)
{
    const char *a = strchr(arg, ':');
    const char *b = a ? strchr(a + 1, ':') : NULL;
    size_t nlen, hlen;
    if (!a || !b)
        return -1;
    nlen = (size_t)(a - arg);
    hlen = (size_t)(b - a - 1);
    if (nlen == 0 || nlen >= V2_NAME_MAX || hlen == 0 || hlen >= V2_PATH_MAX ||
        strlen(b + 1) == 0 || strlen(b + 1) >= V2_PATH_MAX)
        return -1;
    memcpy(name, arg, nlen);
    name[nlen] = 0;
    memcpy(host_path, a + 1, hlen);
    host_path[hlen] = 0;
    strcpy(target_path, b + 1);
    return 0;
}

static int split_pty_arg(const char *arg, char *name, char *host_path, char *argv)
{
    const char *a = strchr(arg, ':');
    const char *b = a ? strchr(a + 1, ':') : NULL;
    size_t nlen, hlen;
    if (!a || !b)
        return -1;
    nlen = (size_t)(a - arg);
    hlen = (size_t)(b - a - 1);
    if (nlen == 0 || nlen >= V2_NAME_MAX || hlen == 0 || hlen >= V2_PATH_MAX ||
        strlen(b + 1) == 0 || strlen(b + 1) >= V2_PATH_MAX)
        return -1;
    memcpy(name, arg, nlen);
    name[nlen] = 0;
    memcpy(host_path, a + 1, hlen);
    host_path[hlen] = 0;
    strcpy(argv, b + 1);
    return 0;
}

static int split_file_arg(const char *arg, char *name, char *host_path)
{
    const char *a = strchr(arg, ':');
    size_t nlen;
    if (!a)
        return -1;
    nlen = (size_t)(a - arg);
    if (nlen == 0 || nlen >= V2_NAME_MAX ||
        strlen(a + 1) == 0 || strlen(a + 1) >= V2_PATH_MAX)
        return -1;
    memcpy(name, arg, nlen);
    name[nlen] = 0;
    strcpy(host_path, a + 1);
    return 0;
}

int v2_host_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void v2_host_close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int v2_host_copy_bounded(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(out, in, n + 1);
    return 0;
}

int v2_host_create_listener(const char *path, int *created_path)
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
            fprintf(stderr, "lmux2 host create_listener unlink %s: %s\n",
                    path, strerror(errno));
            return -1;
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, "lmux2 host create_listener lstat %s: %s\n",
                path, strerror(errno));
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
        fprintf(stderr, "lmux2 host create_listener bind/listen %s: %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    *created_path = 1;
    return fd;
}

int v2_host_socket_peer_closed(int fd)
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

struct v2_host_service *v2_host_service_find_id(struct v2_host_service services[V2_MAX_SERVICES],
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

struct v2_host_service *v2_host_service_find_name(struct v2_host_service services[V2_MAX_SERVICES],
                                                  const char *name)
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (services[i].used && strcmp(services[i].name, name) == 0)
            return &services[i];
    return NULL;
}

struct v2_host_service *v2_host_service_alloc(struct v2_host_service services[V2_MAX_SERVICES],
                                              uint64_t *next_service_id)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        if (!services[i].used) {
            memset(&services[i], 0, sizeof(services[i]));
            services[i].listen_fd = -1;
            services[i].pending_fd = -1;
            for (j = 0; j < V2_MAX_BINDINGS; j++)
                services[i].bindings[j].fd = -1;
            services[i].used = 1;
            services[i].id = (*next_service_id)++;
            if (services[i].id == 0)
                services[i].id = (*next_service_id)++;
            return &services[i];
        }
    }
    return NULL;
}

void v2_host_service_cleanup(struct v2_host_service *s)
{
    int i;
    v2_host_close_fd(&s->pending_fd);
    v2_host_close_fd(&s->listen_fd);
    if (s->created_path && s->listen_path[0])
        unlink(s->listen_path);
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        v2_host_close_fd(&s->bindings[i].fd);
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->pending_fd = -1;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        s->bindings[i].fd = -1;
}

int v2_host_service_has_active_binding(const struct v2_host_service *s)
{
    int i;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        if (s->bindings[i].fd >= 0)
            return 1;
    return 0;
}

int v2_host_stream_has_endpoint_binding(struct v2_host_service services[V2_MAX_SERVICES],
                                        uint64_t stream_id)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_host_service *s = &services[i];
        if (!s->used)
            continue;
        if (s->active_stream_id == stream_id)
            return 1;
        for (j = 0; j < V2_MAX_BINDINGS; j++)
            if (s->bindings[j].fd >= 0 && s->bindings[j].stream_id == stream_id)
                return 1;
    }
    return 0;
}

void v2_host_service_bind_fd_to_stream(struct v2_host_service *s,
                                       uint64_t stream_id, int fd)
{
    int i;
    for (i = 0; i < V2_MAX_BINDINGS; i++) {
        if (s->bindings[i].fd < 0) {
            s->bindings[i].stream_id = stream_id;
            s->bindings[i].fd = fd;
            s->last_stream_id = stream_id;
            return;
        }
    }
    close(fd);
}

static int desired_matches_service(const struct v2_desired *d, const struct v2_host_service *s)
{
    if (d->type != s->type)
        return 0;
    if (strcmp(d->host_path, s->host_path) != 0)
        return 0;
    if (d->type == V2_SERVICE_UNIX_RENDEZVOUS)
        return strcmp(d->target_path, s->target_path) == 0;
    if (d->type == V2_SERVICE_PTY)
        return strcmp(d->argv, s->argv) == 0 && d->supervised == s->supervised;
    return d->type == V2_SERVICE_FILE || d->type == V2_SERVICE_EXEC;
}

void v2_host_init(struct v2_host *h)
{
    memset(h, 0, sizeof(*h));
    h->control_listen_fd = -1;
    h->control_fd = -1;
    h->structured_listen_fd = -1;
    h->structured_fd = -1;
    h->structured_next_ref = 1;
    h->structured_next_file_op_id = 1;
    h->structured_file_fd = -1;
    h->next_req_id = 0;
    h->boot_next_request_id = 1;
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        h->services[i].listen_fd = -1;
        h->services[i].pending_fd = -1;
        for (j = 0; j < V2_MAX_BINDINGS; j++)
            h->services[i].bindings[j].fd = -1;
    }
}

static void ensure_private_exec_desired(struct v2_host *h)
{
    int i, free_idx = -1;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        if (h->desired[i].used) {
            if (strcmp(h->desired[i].name, "__exec") == 0)
                return;
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) {
        h->desired[free_idx].used = 1;
        h->desired[free_idx].type = V2_SERVICE_EXEC;
        h->desired[free_idx].internal = 1;
        strcpy(h->desired[free_idx].name, "__exec");
    }
}

void v2_host_cleanup(struct v2_host *h)
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (h->services[i].used)
            v2_host_service_cleanup(&h->services[i]);
    v2_host_close_fd(&h->control_fd);
    v2_host_close_fd(&h->control_listen_fd);
    if (h->control_path)
        unlink(h->control_path);
    v2_host_close_fd(&h->structured_fd);
    v2_host_close_fd(&h->structured_file_fd);
    if (h->structured_file_tmp_path[0])
        unlink(h->structured_file_tmp_path);
    v2_host_close_fd(&h->structured_listen_fd);
    if (h->structured_path && h->structured_created_path)
        unlink(h->structured_path);
}

int v2_host_add_unix(struct v2_host *h, const char *arg)
{
    int i;
    ensure_private_exec_desired(h);
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        if (!h->desired[i].used) {
            h->desired[i].used = 1;
            h->desired[i].type = V2_SERVICE_UNIX_RENDEZVOUS;
            return split_unix_arg(arg, h->desired[i].name,
                                  h->desired[i].host_path,
                                  h->desired[i].target_path);
        }
    }
    return -1;
}

int v2_host_add_pty(struct v2_host *h, const char *arg)
{
    int i;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        if (!h->desired[i].used) {
            h->desired[i].used = 1;
            h->desired[i].type = V2_SERVICE_PTY;
            h->desired[i].supervised = 1;
            return split_pty_arg(arg, h->desired[i].name,
                                 h->desired[i].host_path,
                                 h->desired[i].argv);
        }
    }
    return -1;
}

int v2_host_add_file(struct v2_host *h, const char *arg)
{
    int i;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        if (!h->desired[i].used) {
            h->desired[i].used = 1;
            h->desired[i].type = V2_SERVICE_FILE;
            return split_file_arg(arg, h->desired[i].name,
                                  h->desired[i].host_path);
        }
    }
    return -1;
}

int v2_host_create_desired_services(struct v2_app *a, struct v2_host *h,
                                    uint64_t *next_service_id)
{
    int i;
    ensure_private_exec_desired(h);
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        struct v2_desired *d = &h->desired[i];
        struct v2_host_service *s;
        if (!d->used)
            continue;
        s = v2_host_service_find_name(h->services, d->name);
        if (s) {
            if (!desired_matches_service(d, s))
                v2_app_log(a, 1, "lmux2 desired service %s conflicts locally; listener not created",
                           d->name);
            continue;
        }
        s = v2_host_service_alloc(h->services, next_service_id);
        if (!s)
            return -1;
        s->id = 0;
        s->type = d->type;
        if (v2_host_copy_bounded(s->name, sizeof(s->name), d->name) < 0 ||
            (d->host_path[0] &&
             v2_host_copy_bounded(s->host_path, sizeof(s->host_path), d->host_path) < 0)) {
            v2_host_service_cleanup(s);
            return -1;
        }
        if (d->target_path[0])
            v2_host_copy_bounded(s->target_path, sizeof(s->target_path), d->target_path);
        if (d->argv[0])
            v2_host_copy_bounded(s->argv, sizeof(s->argv), d->argv);
        s->supervised = d->supervised;
        s->internal = d->internal;
        if (d->type == V2_SERVICE_EXEC) {
            v2_app_log(a, 1, "lmux2 host internal service=%s type=%s",
                       s->name, v2_service_type_name(s->type));
            continue;
        }
        s->listen_fd = v2_host_create_listener(s->host_path, &s->created_path);
        if (s->listen_fd < 0) {
            fprintf(stderr, "lmux2 host listener %s: %s\n", s->host_path, strerror(errno));
            v2_host_service_cleanup(s);
            return -1;
        }
        v2_host_copy_bounded(s->listen_path, sizeof(s->listen_path), s->host_path);
        v2_app_log(a, 1, "lmux2 host local listener service=%s type=%s path=%s",
                   s->name, v2_service_type_name(s->type), s->host_path);
    }
    return 0;
}

int v2_host_ensure_private_exec(struct v2_app *a, struct v2_host *h)
{
    int i;
    ensure_private_exec_desired(h);
    if (v2_host_service_find_name(h->services, "exec"))
        return 0;
    if (v2_host_service_find_name(h->services, "__exec"))
        return 0;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        struct v2_desired *d = &h->desired[i];
        if (d->used && strcmp(d->name, "__exec") == 0) {
            if (!d->sent)
                return v2_runtime_send_service_ensure(a, d);
            return 0;
        }
    }
    return -1;
}

void v2_host_invalidate_remote(struct v2_host *h)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_host_service *s = &h->services[i];
        if (!s->used)
            continue;
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            v2_host_close_fd(&s->bindings[j].fd);
            s->bindings[j].stream_id = 0;
        }
        s->active_stream_id = 0;
        s->id = 0;
        s->pending_event_token = 0;
    }
    h->desired_sent = 0;
    h->factory_unix = 0;
    h->factory_pty = 0;
    h->factory_file = 0;
    h->factory_exec = 0;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        h->desired[i].sent = 0;
        h->desired[i].req_id = 0;
        h->desired[i].service_id = 0;
    }
}

void v2_host_on_factory_report(struct v2_app *a, struct v2_host *h,
                               int has_unix, int has_pty, int has_file,
                               int has_exec)
{
    int i;
    if (h->desired_sent)
        return;
    h->factory_unix = has_unix;
    h->factory_pty = has_pty;
    h->factory_file = has_file;
    h->factory_exec = has_exec;
    v2_app_log(a, 1, "lmux2 FACTORY_REPORT received unix=%d pty=%d file=%d exec=%d",
               h->factory_unix, h->factory_pty, h->factory_file, h->factory_exec);
    v2_app_log(a, 1, "lmux2 reconciling desired services");
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        struct v2_desired *d = &h->desired[i];
        if (d->used &&
            ((d->type == V2_SERVICE_UNIX_RENDEZVOUS && h->factory_unix) ||
             (d->type == V2_SERVICE_PTY && h->factory_pty) ||
             (d->type == V2_SERVICE_FILE && h->factory_file) ||
             (d->type == V2_SERVICE_EXEC && h->factory_exec)))
            (void)v2_runtime_send_service_ensure(a, d);
    }
    h->desired_sent = 1;
}

void v2_host_on_service_ok(struct v2_app *a, struct v2_host *h,
                           uint32_t req_id, uint64_t service_id,
                           enum v2_service_type type,
                           uint64_t *next_service_id)
{
    int i;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        struct v2_desired *d = &h->desired[i];
        struct v2_host_service *s;
        if (!d->used || d->req_id != req_id)
            continue;
        d->service_id = service_id;
        s = v2_host_service_find_id(h->services, service_id);
        if (!s)
            s = v2_host_service_find_name(h->services, d->name);
        if (!s)
            s = v2_host_service_alloc(h->services, next_service_id);
        if (!s)
            return;
        s->id = service_id;
        s->type = type ? type : d->type;
        v2_host_copy_bounded(s->name, sizeof(s->name), d->name);
        if (d->host_path[0])
            v2_host_copy_bounded(s->host_path, sizeof(s->host_path), d->host_path);
        if (d->target_path[0])
            v2_host_copy_bounded(s->target_path, sizeof(s->target_path), d->target_path);
        if (d->argv[0])
            v2_host_copy_bounded(s->argv, sizeof(s->argv), d->argv);
        s->supervised = d->supervised;
        s->internal = d->internal;
        if (s->type != V2_SERVICE_EXEC && s->listen_fd < 0 && s->host_path[0]) {
            s->listen_fd = v2_host_create_listener(s->host_path, &s->created_path);
            if (s->listen_fd >= 0)
                v2_host_copy_bounded(s->listen_path, sizeof(s->listen_path), s->host_path);
        }
        v2_app_log(a, 1, "lmux2 SERVICE_OK service=%s id=%llu type=%s listener=%s",
                   s->name, (unsigned long long)s->id, v2_service_type_name(s->type),
                   s->listen_fd >= 0 ? "ready" : "missing");
    }
}

void v2_host_on_service_error(struct v2_app *a, uint32_t req_id,
                              uint64_t service_id, uint8_t code)
{
    v2_app_log(a, 1, "lmux2 SERVICE_ERROR req=%u service=%llu code=%u",
               req_id, (unsigned long long)service_id, code);
}

void v2_host_on_service_event(struct v2_app *a, struct v2_host *h,
                              uint64_t service_id, uint64_t token,
                              uint8_t event)
{
    struct v2_host_service *s = v2_host_service_find_id(h->services, service_id);
    if (!s)
        return;
    if (event == V2_EVENT_PENDING_CLIENT) {
        s->pending_event_token = token;
        v2_app_log(a, 1, "lmux2 SERVICE_EVENT pending service=%s token=%llu",
                   s->name, (unsigned long long)token);
    } else if (event == V2_EVENT_PENDING_CLEARED && s->pending_event_token == token) {
        s->pending_event_token = 0;
        v2_app_log(a, 1, "lmux2 SERVICE_EVENT pending-cleared service=%s token=%llu",
                   s->name, (unsigned long long)token);
    }
}
