#ifndef LMUX_HOST_H
#define LMUX_HOST_H

#include "carrier.h"
#include "lmux2_core.h"
#include "lmux2_service.h"

#include <poll.h>
#include <stdint.h>
#include <stddef.h>

struct v2_app;

void v2_app_log(struct v2_app *a, int verbose, const char *fmt, ...);

struct v2_host_binding {
    uint64_t stream_id;
    int fd;
};

struct v2_desired {
    int used;
    int sent;
    uint32_t req_id;
    enum v2_service_type type;
    char name[V2_NAME_MAX];
    char host_path[V2_PATH_MAX];
    char target_path[V2_PATH_MAX];
    char argv[V2_PATH_MAX];
    int supervised;
    uint64_t service_id;
};

struct v2_host_service {
    int used;
    uint64_t id;
    enum v2_service_type type;
    char name[V2_NAME_MAX];
    char target_path[V2_PATH_MAX];
    char host_path[V2_PATH_MAX];
    char argv[V2_PATH_MAX];
    char listen_path[V2_PATH_MAX];
    int listen_fd;
    int created_path;
    int pending_fd;
    uint64_t pending_event_token;
    int supervised;
    uint64_t active_stream_id;
    uint64_t last_stream_id;
    struct v2_host_binding bindings[V2_MAX_BINDINGS];
};

struct v2_target_binding_status {
    int used;
    uint64_t stream_id;
};

struct v2_target_service_status {
    int used;
    uint64_t id;
    enum v2_service_type type;
    char name[V2_NAME_MAX];
    char path[V2_PATH_MAX];
    int listener_ready;
    int pending;
    uint64_t pending_token;
    uint64_t pending_event_token;
    uint64_t active_stream_id;
    uint64_t last_stream_id;
    int pty_master_ready;
    long child_pid;
    int file_active;
    struct v2_target_binding_status bindings[V2_MAX_BINDINGS];
};

struct v2_host {
    int factory_unix;
    int factory_pty;
    int factory_file;
    int desired_sent;
    uint32_t next_req_id;
    const char *control_path;
    int control_listen_fd;
    int control_fd;
    char control_in[256];
    size_t control_in_len;
    struct v2_host_service services[V2_MAX_SERVICES];
    struct v2_desired desired[V2_MAX_DESIRED];
};

struct v2_host_status {
    enum lmux2_role role;
    const struct lmux_carrier *carrier;
    const struct lmux2_conn *conn;
    const struct v2_target_service_status *target_services;
    unsigned long long connection_recreated_count;
    unsigned long long connection_established_count;
    unsigned long long peer_restart_count;
    unsigned long long stream_open_count;
    unsigned long long stream_close_count;
    unsigned long long stream_reset_count;
};

int v2_runtime_send_service_ensure(struct v2_app *a, struct v2_desired *d);
int v2_runtime_send_winsize_control(struct v2_app *a, uint64_t service_id,
                                    unsigned rows, unsigned cols,
                                    unsigned xp, unsigned yp);
void v2_runtime_count_stream_open(struct v2_app *a);
void v2_runtime_count_stream_close(struct v2_app *a);

int v2_host_set_nonblock(int fd);
void v2_host_close_fd(int *fd);
int v2_host_copy_bounded(char *out, size_t cap, const char *in);
int v2_host_create_listener(const char *path, int *created_path);
int v2_host_socket_peer_closed(int fd);
struct v2_host_service *v2_host_service_find_id(struct v2_host_service services[V2_MAX_SERVICES],
                                                uint64_t id);
struct v2_host_service *v2_host_service_find_name(struct v2_host_service services[V2_MAX_SERVICES],
                                                  const char *name);
struct v2_host_service *v2_host_service_alloc(struct v2_host_service services[V2_MAX_SERVICES],
                                              uint64_t *next_service_id);
void v2_host_service_cleanup(struct v2_host_service *s);
int v2_host_service_has_active_binding(const struct v2_host_service *s);
int v2_host_stream_has_endpoint_binding(struct v2_host_service services[V2_MAX_SERVICES],
                                        uint64_t stream_id);
void v2_host_service_bind_fd_to_stream(struct v2_host_service *s,
                                       uint64_t stream_id, int fd);

void v2_host_init(struct v2_host *h);
void v2_host_cleanup(struct v2_host *h);
int v2_host_add_unix(struct v2_host *h, const char *arg);
int v2_host_add_pty(struct v2_host *h, const char *arg);
int v2_host_add_file(struct v2_host *h, const char *arg);
int v2_host_create_desired_services(struct v2_app *a, struct v2_host *h,
                                    uint64_t *next_service_id);
void v2_host_invalidate_remote(struct v2_host *h);
void v2_host_on_factory_report(struct v2_app *a, struct v2_host *h,
                               int has_unix, int has_pty, int has_file);
void v2_host_on_service_ok(struct v2_app *a, struct v2_host *h,
                           uint32_t req_id, uint64_t service_id,
                           enum v2_service_type type,
                           uint64_t *next_service_id);
void v2_host_on_service_error(struct v2_app *a, uint32_t req_id,
                              uint64_t service_id, uint8_t code);
void v2_host_on_service_event(struct v2_app *a, struct v2_host *h,
                              uint64_t service_id, uint64_t token,
                              uint8_t event);

void v2_host_attach_pending_clients(struct v2_app *a, struct v2_host *h,
                                    struct lmux2_conn *conn);
void v2_host_cleanup_terminal_bindings(struct v2_app *a, struct v2_host *h,
                                       struct lmux2_conn *conn);
void v2_host_accept_service_client(struct v2_app *a, struct v2_host *h,
                                   struct lmux2_conn *conn,
                                   struct v2_host_service *s);
void v2_host_clear_pending(struct v2_host_service *s);
void v2_host_deliver_stream_rx(struct v2_app *a, struct lmux2_conn *conn,
                               struct v2_host_service *s,
                               struct v2_host_binding *b);
void v2_host_collect_endpoint_tx(struct v2_app *a, struct lmux2_conn *conn,
                                 struct v2_host_service *s,
                                 struct v2_host_binding *b);
void v2_host_close_binding(struct v2_app *a, struct lmux2_conn *conn,
                           struct v2_host_service *s,
                           struct v2_host_binding *b,
                           int send_close);
int v2_host_service_has_live_stream(const struct lmux2_conn *conn,
                                    const struct v2_host_service *s);
int v2_host_stream_can_send_endpoint_data(const struct lmux2_conn *conn,
                                          uint64_t stream_id);
int v2_host_endpoint_poll_events(const struct lmux2_conn *conn,
                                 const struct v2_host_binding *b);

int v2_host_control_start(struct v2_host *h);
void v2_host_control_accept(struct v2_host *h);
void v2_host_control_service(struct v2_app *a, struct v2_host *h,
                             const struct v2_host_status *st);
void v2_host_status_write(struct v2_app *a, struct v2_host *h,
                          const struct v2_host_status *st);

#endif
