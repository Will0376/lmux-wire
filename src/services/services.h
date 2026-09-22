#ifndef LMUX_SERVICES_H
#define LMUX_SERVICES_H

#include "lmux2_core.h"
#include "lmux2_service.h"
#include "lxf.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

struct v2_binding {
    uint64_t stream_id;
    int fd;
};

enum v2_file_state {
    V2_FILE_IDLE = 0,
    V2_FILE_PUT,
    V2_FILE_GET
};

struct v2_file_session {
    enum v2_file_state state;
    uint8_t inbuf[LXF_MAX_RECORD];
    size_t in_len;
    size_t need_len;
    uint8_t outbuf[LXF_MAX_RECORD];
    size_t out_len;
    size_t out_pos;
    int fd;
    char path[LXF_PATH_MAX];
    char tmp_path[LXF_PATH_MAX];
    mode_t mode;
    uint64_t expected_size;
    uint64_t count;
    int size_known;
    struct sha256_ctx sha;
};

struct v2_service {
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
    uint64_t pending_token;
    uint64_t next_token;
    uint64_t pending_event_token;
    int pty_master_fd;
    char pty_slave_path[V2_PATH_MAX];
    struct termios pty_baseline_tio;
    int pty_have_baseline_tio;
    pid_t child_pid;
    long long respawn_at_ms;
    int supervised;
    uint64_t active_stream_id;
    uint64_t last_stream_id;
    struct v2_file_session file;
    struct v2_binding bindings[V2_MAX_BINDINGS];
};

struct v2_app;

void v2_app_log(struct v2_app *a, int verbose, const char *fmt, ...);
long long v2_now_ms(void);
int v2_runtime_queue_service_event(struct v2_app *a, const struct v2_service *s,
                                   uint8_t event, uint64_t token);

int v2_set_nonblock(int fd);
void v2_close_fd(int *fd);
int v2_copy_bounded(char *out, size_t cap, const char *in);
int v2_create_listener(const char *path, int *created_path);
int v2_socket_peer_closed(int fd);

void v2_service_slots_init(struct v2_service services[V2_MAX_SERVICES]);
void v2_service_cleanup(struct v2_service *s);
struct v2_service *v2_service_find_id(struct v2_service services[V2_MAX_SERVICES],
                                      uint64_t id);
struct v2_service *v2_service_find_name(struct v2_service services[V2_MAX_SERVICES],
                                        const char *name);
struct v2_service *v2_service_alloc(struct v2_service services[V2_MAX_SERVICES],
                                    uint64_t *next_service_id);
struct v2_binding *v2_service_find_binding(struct v2_service services[V2_MAX_SERVICES],
                                           uint64_t stream_id,
                                           struct v2_service **svc_out);
struct v2_binding *v2_service_alloc_binding(struct v2_service *s);
int v2_service_has_active_binding(const struct v2_service *s);
int v2_service_has_live_stream(const struct lmux2_conn *conn, const struct v2_service *s);
int v2_stream_has_endpoint_binding(struct v2_service services[V2_MAX_SERVICES],
                                   uint64_t stream_id);
void v2_service_bind_fd_to_stream(struct v2_service *s, uint64_t stream_id, int fd);

int v2_pty_create_master(struct v2_service *s);
void v2_pty_spawn_child(struct v2_app *a, struct v2_service *s);
void v2_pty_terminate_child(struct v2_service *s);
void v2_pty_restore_baseline(struct v2_service *s);
void v2_pty_supervise(struct v2_app *a, struct v2_service services[V2_MAX_SERVICES]);
void v2_pty_apply_winsize(struct v2_service *s, unsigned rows, unsigned cols,
                          unsigned xp, unsigned yp);
void v2_pty_deliver_stream_rx(struct lmux2_conn *conn, struct v2_service *s);
void v2_pty_collect_tx(struct lmux2_conn *conn, struct v2_service *s);

void v2_file_session_reset(struct v2_file_session *fs);
int v2_file_service_step(struct v2_app *a, struct lmux2_conn *conn,
                         struct v2_service services[V2_MAX_SERVICES],
                         struct v2_service *s);

void v2_unix_clear_pending(struct v2_app *a, struct v2_service *s);
void v2_unix_accept_pending(struct v2_app *a, struct v2_service *s, int fd);
void v2_unix_pair_unbound_target_streams(struct v2_app *a, struct lmux2_conn *conn,
                                         struct v2_service services[V2_MAX_SERVICES]);

#endif
