#ifndef LMUX_HOST_H
#define LMUX_HOST_H

#include "carrier.h"
#include "carrier_manager.h"
#include "boot_protocol.h"
#include "host_control_structured.h"
#include "link_endpoint.h"
#include "dirty.h"
#include "lxf.h"
#include "lmux2_core.h"
#include "lmux2_service.h"
#include "services.h"

#include <poll.h>
#include <stdint.h>
#include <stddef.h>

struct v2_app;

void v2_app_log(struct v2_app *a, int verbose, const char *fmt, ...);
long long v2_now_ms(void);

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
    int internal;
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
    int internal;
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

#define V2_HOST_STRUCTURED_MAX_EXECS V2_EXEC_MAX_RUNNING
#define V2_HOST_STRUCTURED_CTL_QUEUE_MAX 8
#define V2_HOST_STRUCTURED_EXEC_POLL_MS 1000
#define V2_HOST_STRUCTURED_FILE_HISTORY 8
#define V2_HOST_STRUCTURED_FILE_ERROR_MAX 160
#define V2_HOST_STRUCTURED_FILE_NAMESPACE_MAX 64
#define V2_HOST_STRUCTURED_FILE_TOKEN_MAX 128

struct v2_host_structured_file_operation {
    int used;
    uint64_t op_id;
    uint64_t local_conn;
    uint64_t peer_conn;
    uint64_t incarnation;
    uint64_t bytes;
    uint64_t total;
    uint32_t error_code;
    uint8_t kind;
    uint8_t state;
    uint8_t total_known;
    uint8_t digest[32];
    char error[V2_HOST_STRUCTURED_FILE_ERROR_MAX];
    char local_path[LXF_PATH_MAX];
    char target_path[LXF_PATH_MAX];
    char request_namespace[V2_HOST_STRUCTURED_FILE_NAMESPACE_MAX];
    char request_token[V2_HOST_STRUCTURED_FILE_TOKEN_MAX];
};

struct v2_host_structured_exec {
    int used;
    uint64_t ref;
    uint64_t exec_id;
    uint64_t incarnation;
    uint64_t req_id;
    int active;
    int lost;
    int lost_emitted;
    int done;
    int auto_reattach;
    int status_pending;
    int final_pending;
    int final_queued;
    long long next_status_ms;
    uint8_t final_state;
    uint8_t final_fail;
    uint16_t final_errno;
    uint32_t final_exit_code;
    uint32_t final_term_signal;
    uint8_t final_payload[64];
    uint16_t final_payload_len;
    uint64_t stdin_stream;
    uint64_t stdout_stream;
    uint64_t stderr_stream;
    int stdout_stream_complete;
    int stderr_stream_complete;
    uint8_t stdin_buf[4096];
    size_t stdin_len;
    size_t stdin_pos;
    int stdin_backpressure;
    int stdin_ready_pending;
    int stdin_stream_opened;
    int stdin_close_requested;
    int stdin_close_pending;
    int stdin_close_committed;
};

enum v2_host_structured_control_kind {
    V2_HC2_CTL_NONE = 0,
    V2_HC2_CTL_INFO,
    V2_HC2_CTL_START,
    V2_HC2_CTL_ATTACH,
    V2_HC2_CTL_STATUS,
    V2_HC2_CTL_SIGNAL,
    V2_HC2_CTL_RELEASE
};

struct v2_host_structured_control_op {
    enum v2_host_structured_control_kind kind;
    uint64_t request_id;
    uint64_t ref;
};

struct v2_host_structured_pending_frame {
    uint16_t type;
    uint16_t flags;
    uint64_t request_id;
    uint32_t payload_len;
    uint8_t payload[LMUX_HC2_MAX_PAYLOAD];
};

struct v2_host {
    int factory_unix;
    int factory_pty;
    int factory_file;
    int factory_exec;
    int desired_sent;
    uint32_t next_req_id;
    const char *control_path;
    int control_listen_fd;
    int control_fd;
    int carrier_control_fd;
    int carrier_switch_waiting;
    char control_in[4096];
    size_t control_in_len;
    const char *structured_path;
    int structured_listen_fd;
    int structured_fd;
    int structured_created_path;
    int structured_ready;
    uint8_t structured_rx[LMUX_HC2_RX_CAP];
    size_t structured_rx_len;
    uint8_t structured_tx[LMUX_HC2_TX_CAP];
    size_t structured_tx_len;
    size_t structured_tx_pos;
    uint64_t structured_next_ref;
    uint64_t structured_known_exec_incarnation;
    int structured_exec_info_pending;
    struct v2_host_structured_control_op structured_control_op;
    struct v2_host_structured_pending_frame structured_control_queue[V2_HOST_STRUCTURED_CTL_QUEUE_MAX];
    int structured_control_queue_head;
    int structured_control_queue_len;
    int structured_exec_poll_next;
    struct v2_host_structured_exec structured_execs[V2_HOST_STRUCTURED_MAX_EXECS];
    uint64_t structured_next_file_op_id;
    struct v2_host_structured_file_operation
        structured_file_operations[V2_HOST_STRUCTURED_FILE_HISTORY];
    uint64_t structured_file_op_id;
    uint64_t structured_file_req_id;
    uint64_t structured_file_stream;
    int structured_file_active;
    int structured_file_kind;
    int structured_file_phase;
    int structured_file_fd;
    uint64_t structured_file_expected;
    uint64_t structured_file_count;
    uint64_t structured_file_last_progress;
    uint32_t structured_file_mode;
    char structured_file_local_path[LXF_PATH_MAX];
    char structured_file_target_path[LXF_PATH_MAX];
    char structured_file_tmp_path[LXF_PATH_MAX];
    struct sha256_ctx structured_file_sha;
    uint8_t structured_file_digest[32];
    uint8_t structured_file_in[LXF_MAX_RECORD];
    size_t structured_file_in_len;
    size_t structured_file_need_len;
    uint8_t structured_file_out[LXF_MAX_RECORD];
    size_t structured_file_out_len;
    size_t structured_file_out_pos;
    int boot_pending;
    int boot_control_gone;
    uint16_t boot_opcode;
    uint64_t boot_request_id;
    long long boot_deadline_ms;
    uint64_t boot_next_request_id;
    int boot_upload_active;
    int boot_upload_ready_sent;
    int boot_upload_abort_needed;
    uint64_t boot_upload_id;
    uint64_t boot_upload_declared_size;
    uint64_t boot_upload_sent;
    uint32_t boot_upload_max_chunk;
    uint32_t boot_upload_last_chunk;
    struct sha256_ctx boot_upload_sha;
    uint8_t boot_upload_digest[32];
    int deploy_active;
    int deploy_client_gone;
    int deploy_cancel_before_activate;
    int deploy_notified_link_down;
    int deploy_reported_reconciled;
    int deploy_restart_supervisor;
    int deploy_no_file_restart;
    int deploy_install_already;
    uint32_t deploy_phase;
    uint32_t deploy_confirm_timeout_ms;
    uint64_t deploy_activation_id;
    uint64_t deploy_generation;
    uint64_t deploy_pre_local_conn;
    uint64_t deploy_pre_peer_conn;
    uint64_t deploy_pre_established_count;
    long long deploy_deadline_ms;
    uint8_t deploy_sha[32];
    int exec_active;
    int exec_phase;
    int exec_status_pending;
    int exec_close_pending;
    int exec_close_stdin_after_drain;
    int exec_done_seen;
    int exec_structured;
    uint64_t exec_service_id;
    uint64_t exec_id;
    uint64_t exec_control_stream;
    uint64_t exec_retiring_control_stream;
    unsigned long long structured_control_transactions;
    unsigned long long structured_control_completions;
    unsigned long long structured_control_autopolls;
    unsigned long long structured_control_retirements;
    unsigned long long structured_control_retire_wait_ticks;
    uint64_t exec_stdin_stream;
    uint64_t exec_stdout_stream;
    uint64_t exec_stderr_stream;
    uint64_t exec_conflict_stream;
    uint8_t exec_ctl_in[1024];
    size_t exec_ctl_in_len;
    uint8_t exec_ctl_out[1024];
    size_t exec_ctl_out_len;
    size_t exec_ctl_out_pos;
    long long exec_next_status_ms;
    uint8_t exec_stdin[4096];
    size_t exec_stdin_len;
    size_t exec_stdin_pos;
    struct v2_host_service services[V2_MAX_SERVICES];
    struct v2_desired desired[V2_MAX_DESIRED];
};

struct v2_host_status {
    enum lmux2_role role;
    const struct lmux_carrier *carrier;
    const struct lmux_carrier_manager *carriers;
    const struct lmux_link_endpoint *outer;
    const struct lmux2_conn *conn;
    const struct v2_target_service_status *target_services;
    unsigned long long connection_recreated_count;
    unsigned long long connection_established_count;
    unsigned long long peer_restart_count;
    unsigned long long stream_open_count;
    unsigned long long stream_close_count;
    unsigned long long stream_reset_count;
    const char *carrier_switch_phase;
    const char *carrier_switch_name;
    const char *carrier_last_error;
    int carrier_committed_but_down;
    const char *carrier_console_state;
    const char *carrier_console_name;
    unsigned carrier_console_hold_mask;
    unsigned carrier_failure_hold_mask;
    const char *carrier_admin_phase;
    const char *carrier_admin_name;
    const char *carrier_admin_action;
    const char *carrier_admin_last_name;
    const char *carrier_admin_last_action;
    const char *carrier_admin_last_result;
    const char *carrier_admin_last_error;
    unsigned carrier_admin_target_known_mask;
    unsigned carrier_admin_target_released_mask;
    const struct lmux_dirty *dirty;
};

size_t v2_host_link_push_lmux(struct lmux_link_endpoint *link,
                              const uint8_t *buf, size_t len);
size_t v2_host_link_pull_lmux(struct lmux_link_endpoint *link,
                              uint8_t *buf, size_t len);

int v2_runtime_send_service_ensure(struct v2_app *a, struct v2_desired *d);
int v2_runtime_send_winsize_control(struct v2_app *a, uint64_t service_id,
                                    unsigned rows, unsigned cols,
                                    unsigned xp, unsigned yp);
int v2_runtime_boot_supported(struct v2_app *a);
int v2_runtime_boot_upload_supported(struct v2_app *a);
int v2_runtime_boot_managed_supported(struct v2_app *a);
int v2_runtime_boot_supervisor_supported(struct v2_app *a);
int v2_runtime_queue_boot_request(struct v2_app *a, uint16_t opcode,
                                  uint64_t request_id);
int v2_runtime_queue_boot_request_body(struct v2_app *a, uint16_t opcode,
                                       uint64_t request_id,
                                       const uint8_t *body, uint32_t body_len);
void v2_runtime_lmux_marker(struct v2_app *a, uint64_t *local_conn,
                            uint64_t *peer_conn,
                            uint64_t *established_count);
int v2_runtime_lmux_established(struct v2_app *a);
int v2_runtime_outer_enabled(struct v2_app *a);
int v2_runtime_carrier_switch_start(struct v2_app *a, const char *name,
                                    enum lmux_carrier_mode mode,
                                    unsigned timeout_ms,
                                    char *error, size_t error_cap);
int v2_runtime_carrier_admin_start(struct v2_app *a, const char *name,
                                   int release, unsigned timeout_ms,
                                   char *error, size_t error_cap);
int v2_runtime_open_stream_token(struct v2_app *a, uint64_t service_id,
                                 uint64_t token, uint64_t *stream_id);
int v2_runtime_stream_is_open(struct v2_app *a, uint64_t stream_id);
int v2_runtime_stream_state(struct v2_app *a, uint64_t stream_id);
size_t v2_runtime_stream_read(struct v2_app *a, uint64_t stream_id,
                              uint8_t *buf, size_t len);
size_t v2_runtime_stream_write(struct v2_app *a, uint64_t stream_id,
                               const uint8_t *buf, size_t len);
size_t v2_runtime_stream_rx_len(struct v2_app *a, uint64_t stream_id);
size_t v2_runtime_stream_tx_len(struct v2_app *a, uint64_t stream_id);
int v2_runtime_close_stream(struct v2_app *a, uint64_t stream_id);
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
int v2_host_ensure_private_exec(struct v2_app *a, struct v2_host *h);
void v2_host_invalidate_remote(struct v2_host *h);
void v2_host_on_factory_report(struct v2_app *a, struct v2_host *h,
                               int has_unix, int has_pty, int has_file,
                               int has_exec);
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
void v2_host_control_on_hup(struct v2_host *h);
void v2_host_control_carrier_service(struct v2_host *h, short revents);
void v2_host_control_carrier_result(struct v2_host *h, const char *line);
int v2_runtime_carrier_console_start(struct v2_app *a, const char *name,
                                     char *error, size_t error_cap);
int v2_host_desired_reconciled(const struct v2_host *h);
void v2_host_control_outer_down(struct v2_host *h);
void v2_host_control_check_timeout(struct v2_host *h);
void v2_host_control_tick(struct v2_app *a, struct v2_host *h,
                          const struct v2_host_status *st);
short v2_host_control_poll_events(const struct v2_host *h);
void v2_host_control_on_boot_response(struct v2_app *a, struct v2_host *h,
                                      uint16_t opcode,
                                      uint16_t status, uint64_t request_id,
                                      const uint8_t *body, uint32_t body_len);

int v2_host_structured_start(struct v2_host *h);
void v2_host_structured_accept(struct v2_host *h);
short v2_host_structured_poll_events(const struct v2_host *h);
void v2_host_structured_service(struct v2_app *a, struct v2_host *h,
                                const struct v2_host_status *st, short revents);
void v2_host_structured_tick(struct v2_app *a, struct v2_host *h,
                             const struct v2_host_status *st);
void v2_host_structured_on_exec_info(struct v2_host *h,
                                     uint64_t exec_incarnation);
void v2_host_structured_check_exec_incarnation(struct v2_host *h);
short v2_host_structured_file_poll_events(const struct v2_host *h);
void v2_host_structured_file_event(struct v2_app *a, struct v2_host *h,
                                   short revents);
int v2_host_structured_queue_exec_frame(struct v2_host *h, uint16_t type,
                                        uint64_t request_id,
                                        const uint8_t *payload,
                                        uint32_t payload_len);
int v2_host_structured_can_queue(struct v2_host *h, uint32_t payload_len);
void v2_host_structured_flush_tx(struct v2_app *a, struct v2_host *h);
void v2_host_structured_on_exec_clear(struct v2_host *h);
void v2_host_structured_on_outer_down(struct v2_host *h);

#endif
