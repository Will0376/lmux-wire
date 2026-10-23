#ifndef LMUX_SERVICES_H
#define LMUX_SERVICES_H

#include "lmux2_core.h"
#include "lmux2_service.h"
#include "lxf.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

#define V2_EXEC_MAX_RUNNING 4
#define V2_EXEC_MAX_COMPLETED 8
#define V2_EXEC_MAX_SLOTS (V2_EXEC_MAX_RUNNING + V2_EXEC_MAX_COMPLETED)
#define V2_EXEC_ARG_MAX 16
#define V2_EXEC_ARG_BYTES_MAX 4096
#define V2_EXEC_CTL_BUF_MAX 1024
#define V2_EXEC_STDIN_BUF_MAX 1024
#define V2_EXEC_OUTPUT_BUF_MAX 256
#define V2_EXEC_FD_STDIN 1
#define V2_EXEC_FD_STDOUT 2
#define V2_EXEC_FD_STDERR 3
#define V2_EXEC_FD_ERR 4
#define V2_PTY_OUTPUT_CAP 16384

enum v2_exec_role {
    V2_EXEC_ROLE_CONTROL = 1,
    V2_EXEC_ROLE_STDIN = 2,
    V2_EXEC_ROLE_STDOUT = 3,
    V2_EXEC_ROLE_STDERR = 4
};

enum v2_exec_signal {
    V2_EXEC_SIG_INT = 1,
    V2_EXEC_SIG_TERM = 2,
    V2_EXEC_SIG_KILL = 3,
    V2_EXEC_SIG_HUP = 4
};

enum v2_exec_record_type {
    V2_EXEC_REC_INFO = 0,
    V2_EXEC_REC_START = 1,
    V2_EXEC_REC_SIGNAL = 2,
    V2_EXEC_REC_CLOSE_STDIN = 3,
    V2_EXEC_REC_STATUS = 4,
    V2_EXEC_REC_RELEASE = 5,
    V2_EXEC_REC_INFO_REPLY = 100,
    V2_EXEC_REC_START_REPLY = 101,
    V2_EXEC_REC_STATUS_REPLY = 102,
    V2_EXEC_REC_ERROR = 103
};

enum v2_exec_status {
    V2_EXEC_STATUS_STARTING = 1,
    V2_EXEC_STATUS_RUNNING = 2,
    V2_EXEC_STATUS_EXITED_DRAINING = 3,
    V2_EXEC_STATUS_DONE = 4,
    V2_EXEC_STATUS_START_FAILED = 5,
    V2_EXEC_STATUS_TERMINATING = 6
};

enum v2_exec_error {
    V2_EXEC_OK = 0,
    V2_EXEC_E_BAD_REQUEST = 1,
    V2_EXEC_E_NOT_FOUND = 2,
    V2_EXEC_E_BUSY = 3,
    V2_EXEC_E_LIMIT = 4,
    V2_EXEC_E_IO = 5,
    V2_EXEC_E_NOT_RUNNING = 6
};

enum v2_exec_start_failure {
    V2_EXEC_FAIL_NONE = 0,
    V2_EXEC_FAIL_FORK = 1,
    V2_EXEC_FAIL_PIPE = 2,
    V2_EXEC_FAIL_CHDIR = 3,
    V2_EXEC_FAIL_SETPGID = 4,
    V2_EXEC_FAIL_EXEC = 5
};

struct v2_exec_record {
    uint8_t type;
    uint8_t status;
    uint16_t length;
    uint64_t exec_id;
    const uint8_t *payload;
};

struct v2_exec_obj {
    int used;
    uint64_t exec_id;
    enum v2_exec_status status;
    pid_t pid;
    pid_t pgid;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    int err_fd;
    int stdin_closed;
    int close_stdin_requested;
    int stdout_eof;
    int stderr_eof;
    int child_reaped;
    int exit_code;
    int term_signal;
    int start_errno;
    int start_reply_pending;
    enum v2_exec_start_failure start_failure;
    uint64_t control_stream_id;
    uint64_t stdin_stream_id;
    uint64_t stdout_stream_id;
    uint64_t stderr_stream_id;
    uint8_t ctl_in[V2_EXEC_CTL_BUF_MAX];
    size_t ctl_in_len;
    uint8_t ctl_out[V2_EXEC_CTL_BUF_MAX];
    size_t ctl_out_len;
    size_t ctl_out_pos;
    uint8_t stdin_buf[V2_EXEC_STDIN_BUF_MAX];
    size_t stdin_len;
    size_t stdin_pos;
    uint8_t stdout_buf[V2_EXEC_OUTPUT_BUF_MAX];
    size_t stdout_len;
    size_t stdout_pos;
    uint8_t stderr_buf[V2_EXEC_OUTPUT_BUF_MAX];
    size_t stderr_len;
    size_t stderr_pos;
    unsigned long long stdout_child_read_total;
    unsigned long long stdout_lmux_written_total;
    unsigned long long stderr_child_read_total;
    unsigned long long stderr_lmux_written_total;
    unsigned long long completed_seq;
};

struct v2_exec_service_state {
    uint64_t incarnation;
    uint64_t next_exec_id;
    unsigned long long next_completed_seq;
    uint64_t control_stream_id;
    uint8_t ctl_in[V2_EXEC_CTL_BUF_MAX];
    size_t ctl_in_len;
    uint8_t ctl_out[V2_EXEC_CTL_BUF_MAX];
    size_t ctl_out_len;
    size_t ctl_out_pos;
    struct v2_exec_obj execs[V2_EXEC_MAX_SLOTS];
};

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
    uint8_t pty_output[V2_PTY_OUTPUT_CAP];
    uint64_t pty_output_start;
    uint64_t pty_output_end;
    uint64_t pty_send_pos;
    int pty_reset_pending;
    long long pty_reset_deadline_ms;
    pid_t child_pid;
    long long respawn_at_ms;
    int supervised;
    uint64_t active_stream_id;
    uint64_t last_stream_id;
    struct v2_file_session file;
    struct v2_exec_service_state exec;
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
int v2_pty_attach_stream(struct v2_service *s, uint64_t stream_id);
int v2_pty_wants_master_read(const struct v2_service *s);
void v2_pty_deliver_stream_rx(struct lmux2_conn *conn, struct v2_service *s);
void v2_pty_collect_tx(struct lmux2_conn *conn, struct v2_service *s);

void v2_file_session_reset(struct v2_file_session *fs);
int v2_file_service_step(struct v2_app *a, struct lmux2_conn *conn,
                         struct v2_service services[V2_MAX_SERVICES],
                         struct v2_service *s);

void v2_exec_service_reset(struct v2_service *s);
int v2_exec_create_service(struct v2_app *a, struct v2_service *s,
                           const char *name);
int v2_exec_handle_stream_open(struct v2_app *a, struct lmux2_conn *conn,
                               struct v2_service *s,
                               struct lmux2_stream *st);
void v2_exec_cleanup_connection(struct v2_app *a, struct v2_service *s,
                                struct lmux2_conn *conn);
int v2_exec_service_step(struct v2_app *a, struct lmux2_conn *conn,
                         struct v2_service *s);
short v2_exec_fd_events(struct v2_app *a, struct lmux2_conn *conn,
                        struct v2_service *s, int exec_idx, int fd_kind);
void v2_exec_on_fd_event(struct v2_app *a, struct lmux2_conn *conn,
                         struct v2_service *s, int exec_idx, int fd_kind,
                         short revents);
uint64_t v2_exec_stream_token(uint64_t exec_id, enum v2_exec_role role);

void v2_unix_clear_pending(struct v2_app *a, struct v2_service *s);
void v2_unix_accept_pending(struct v2_app *a, struct v2_service *s, int fd);
void v2_unix_pair_unbound_target_streams(struct v2_app *a, struct lmux2_conn *conn,
                                         struct v2_service services[V2_MAX_SERVICES]);

#endif
