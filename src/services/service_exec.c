#include "services.h"
#include "lmux.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXEC_RECORD_HEADER 12
#define EXEC_ROLE_SHIFT 56
#define EXEC_ID_MASK 0x00ffffffffffffffULL

struct child_error {
    uint32_t stage;
    uint32_t errnum;
};

extern char **environ;

static uint64_t pack_token(uint64_t exec_id, enum v2_exec_role role)
{
    return ((uint64_t)role << EXEC_ROLE_SHIFT) | (exec_id & EXEC_ID_MASK);
}

static enum v2_exec_role token_role(uint64_t token)
{
    return (enum v2_exec_role)(token >> EXEC_ROLE_SHIFT);
}

static uint64_t token_exec_id(uint64_t token)
{
    return token & EXEC_ID_MASK;
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int set_nonblock_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        return -1;
    return 0;
}

static void exec_obj_close_streams(struct lmux2_conn *conn, struct v2_exec_obj *e,
                                   int send_close)
{
    if (send_close) {
        if (e->control_stream_id)
            (void)lmux2_close_stream(conn, e->control_stream_id);
        if (e->stdin_stream_id)
            (void)lmux2_close_stream(conn, e->stdin_stream_id);
        if (e->stdout_stream_id)
            (void)lmux2_close_stream(conn, e->stdout_stream_id);
        if (e->stderr_stream_id)
            (void)lmux2_close_stream(conn, e->stderr_stream_id);
    }
    e->control_stream_id = 0;
    e->stdin_stream_id = 0;
    e->stdout_stream_id = 0;
    e->stderr_stream_id = 0;
    e->ctl_in_len = 0;
    e->ctl_out_len = e->ctl_out_pos = 0;
    e->stdin_len = e->stdin_pos = 0;
}

static uint64_t exec_random_nonzero64(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
            v = 0;
        close(fd);
    }
    if (!v)
        v = ((uint64_t)(uint32_t)getpid() << 32) ^
            (uint64_t)(uintptr_t)&v ^ (uint64_t)v2_now_ms() ^
            0x65786563696e6349ULL;
    if (!v)
        v = 1;
    return v;
}

static void exec_obj_kill_and_reap(struct v2_exec_obj *e)
{
    int status;
    if (!e->used || e->pid <= 0 || e->child_reaped)
        return;
    if (e->pgid > 0)
        (void)kill(-e->pgid, SIGTERM);
    else
        (void)kill(e->pid, SIGTERM);
    if (waitpid(e->pid, &status, WNOHANG) == 0) {
        if (e->pgid > 0)
            (void)kill(-e->pgid, SIGKILL);
        else
            (void)kill(e->pid, SIGKILL);
        while (waitpid(e->pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    e->child_reaped = 1;
}

static void exec_obj_reset(struct v2_exec_obj *e)
{
    close_fd(&e->stdin_fd);
    close_fd(&e->stdout_fd);
    close_fd(&e->stderr_fd);
    close_fd(&e->err_fd);
    memset(e, 0, sizeof(*e));
    e->stdin_fd = -1;
    e->stdout_fd = -1;
    e->stderr_fd = -1;
    e->err_fd = -1;
}

static void exec_close_stdin_side(struct v2_exec_obj *e)
{
    close_fd(&e->stdin_fd);
    e->stdin_closed = 1;
}

static void exec_close_stdout_side(struct v2_exec_obj *e)
{
    close_fd(&e->stdout_fd);
}

static void exec_state_init_fds(struct v2_exec_service_state *st)
{
    int i;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
        st->execs[i].stdin_fd = -1;
        st->execs[i].stdout_fd = -1;
        st->execs[i].stderr_fd = -1;
        st->execs[i].err_fd = -1;
    }
}

void v2_exec_service_reset(struct v2_service *s)
{
    int i;
    if (!s)
        return;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
        struct v2_exec_obj *e = &s->exec.execs[i];
        if (e->used) {
            exec_obj_kill_and_reap(e);
            exec_obj_reset(e);
        }
    }
    memset(&s->exec, 0, sizeof(s->exec));
    s->exec.incarnation = exec_random_nonzero64();
    s->exec.next_exec_id = 1;
    s->exec.ctl_out_len = s->exec.ctl_out_pos = 0;
    exec_state_init_fds(&s->exec);
}

int v2_exec_create_service(struct v2_app *a, struct v2_service *s,
                           const char *name)
{
    (void)a;
    if (v2_copy_bounded(s->name, sizeof(s->name), name) < 0)
        return -1;
    s->type = V2_SERVICE_EXEC;
    v2_exec_service_reset(s);
    s->exec.next_exec_id = 1;
    return 0;
}

static struct v2_exec_obj *find_exec(struct v2_service *s, uint64_t exec_id)
{
    int i;
    if (!exec_id)
        return NULL;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++)
        if (s->exec.execs[i].used && s->exec.execs[i].exec_id == exec_id)
            return &s->exec.execs[i];
    return NULL;
}

static int count_active_execs(struct v2_service *s)
{
    int i, n = 0;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
        struct v2_exec_obj *e = &s->exec.execs[i];
        if (!e->used)
            continue;
        if (e->status != V2_EXEC_STATUS_DONE &&
            e->status != V2_EXEC_STATUS_START_FAILED)
            n++;
    }
    return n;
}

static void gc_completed(struct v2_service *s)
{
    int i, victim = -1, completed = 0;
    unsigned long long oldest = 0;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
        struct v2_exec_obj *e = &s->exec.execs[i];
        if (!e->used || (e->status != V2_EXEC_STATUS_DONE &&
                         e->status != V2_EXEC_STATUS_START_FAILED))
            continue;
        completed++;
        if (!e->control_stream_id && !e->stdin_stream_id &&
            !e->stdout_stream_id && !e->stderr_stream_id &&
            (victim < 0 || e->completed_seq < oldest)) {
            victim = i;
            oldest = e->completed_seq;
        }
    }
    if (completed <= V2_EXEC_MAX_COMPLETED || victim < 0)
        return;
    exec_obj_reset(&s->exec.execs[victim]);
}

static int evict_one_completed(struct v2_service *s)
{
    int i, victim = -1;
    unsigned long long oldest = 0;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
        struct v2_exec_obj *e = &s->exec.execs[i];
        if (!e->used || (e->status != V2_EXEC_STATUS_DONE &&
                         e->status != V2_EXEC_STATUS_START_FAILED))
            continue;
        if (e->control_stream_id || e->stdin_stream_id ||
            e->stdout_stream_id || e->stderr_stream_id)
            continue;
        if (victim < 0 || e->completed_seq < oldest) {
            victim = i;
            oldest = e->completed_seq;
        }
    }
    if (victim < 0)
        return -1;
    exec_obj_reset(&s->exec.execs[victim]);
    return 0;
}

static struct v2_exec_obj *alloc_exec(struct v2_service *s)
{
    int i, free_slot = -1;
    gc_completed(s);
    if (count_active_execs(s) >= V2_EXEC_MAX_RUNNING)
        return NULL;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
        if (!s->exec.execs[i].used) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0 && evict_one_completed(s) == 0) {
        for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
            if (!s->exec.execs[i].used) {
                free_slot = i;
                break;
            }
        }
    }
    if (free_slot >= 0) {
        struct v2_exec_obj *e = &s->exec.execs[free_slot];
        exec_obj_reset(e);
        e->used = 1;
        e->exec_id = s->exec.next_exec_id++;
        if (s->exec.next_exec_id == 0 || s->exec.next_exec_id > EXEC_ID_MASK)
            s->exec.next_exec_id = 1;
        if (e->exec_id == 0)
            e->exec_id = s->exec.next_exec_id++;
        e->status = V2_EXEC_STATUS_STARTING;
        return e;
    }
    return NULL;
}

static int queue_control_record(struct v2_service *s, uint8_t type,
                                uint8_t status, uint64_t exec_id,
                                const uint8_t *payload, uint16_t len)
{
    size_t off;
    if ((size_t)EXEC_RECORD_HEADER + len > sizeof(s->exec.ctl_out))
        return -1;
    if (s->exec.ctl_out_len != s->exec.ctl_out_pos)
        return -1;
    s->exec.ctl_out_len = s->exec.ctl_out_pos = 0;
    s->exec.ctl_out[0] = type;
    s->exec.ctl_out[1] = status;
    lmux_put_le16(s->exec.ctl_out + 2, len);
    lmux_put_le64(s->exec.ctl_out + 4, exec_id);
    off = EXEC_RECORD_HEADER;
    if (len)
        memcpy(s->exec.ctl_out + off, payload, len);
    s->exec.ctl_out_len = off + len;
    return 0;
}

static void encode_status_payload(struct v2_exec_obj *e, uint8_t *p,
                                  uint16_t *len)
{
    unsigned long long stdout_pending =
        e->stdout_len > e->stdout_pos ? e->stdout_len - e->stdout_pos : 0;
    unsigned long long stderr_pending =
        e->stderr_len > e->stderr_pos ? e->stderr_len - e->stderr_pos : 0;
    p[0] = (uint8_t)e->status;
    p[1] = (uint8_t)e->start_failure;
    lmux_put_le16(p + 2, (uint16_t)(e->start_errno < 0 ? 0 : e->start_errno));
    lmux_put_le32(p + 4, (uint32_t)e->exit_code);
    lmux_put_le32(p + 8, (uint32_t)e->term_signal);
    p[12] = e->stdin_closed ? 1 : 0;
    p[13] = e->stdout_eof ? 1 : 0;
    p[14] = e->stderr_eof ? 1 : 0;
    p[15] = e->child_reaped ? 1 : 0;
    lmux_put_le64(p + 16, e->stdout_child_read_total);
    lmux_put_le64(p + 24, e->stdout_lmux_written_total);
    lmux_put_le64(p + 32, stdout_pending);
    lmux_put_le64(p + 40, e->stderr_child_read_total);
    lmux_put_le64(p + 48, e->stderr_lmux_written_total);
    lmux_put_le64(p + 56, stderr_pending);
    *len = 64;
}

static int queue_exec_status(struct v2_service *s, struct v2_exec_obj *e,
                             uint8_t record_type)
{
    uint8_t payload[64];
    uint16_t len;
    encode_status_payload(e, payload, &len);
    return queue_control_record(s, record_type, V2_EXEC_OK, e->exec_id,
                                payload, len);
}

static int queue_pending_start_reply(struct v2_service *s,
                                     struct v2_exec_obj *e)
{
    if (!e->start_reply_pending)
        return 0;
    if (!s->exec.control_stream_id)
        return -1;
    if (queue_exec_status(s, e, V2_EXEC_REC_START_REPLY) < 0)
        return -1;
    e->start_reply_pending = 0;
    return 0;
}

static int queue_exec_error(struct v2_service *s, uint64_t exec_id,
                            uint8_t err)
{
    return queue_control_record(s, V2_EXEC_REC_ERROR, err, exec_id, NULL, 0);
}

static int parse_record(struct v2_exec_service_state *es,
                        struct v2_exec_record *r)
{
    if (es->ctl_in_len < EXEC_RECORD_HEADER)
        return 0;
    r->type = es->ctl_in[0];
    r->status = es->ctl_in[1];
    r->length = lmux_get_le16(es->ctl_in + 2);
    r->exec_id = lmux_get_le64(es->ctl_in + 4);
    if (r->length > V2_EXEC_CTL_BUF_MAX - EXEC_RECORD_HEADER)
        return -1;
    if (es->ctl_in_len < (size_t)EXEC_RECORD_HEADER + r->length)
        return 0;
    r->payload = es->ctl_in + EXEC_RECORD_HEADER;
    return 1;
}

static void consume_record(struct v2_exec_service_state *es, uint16_t len)
{
    size_t n = EXEC_RECORD_HEADER + len;
    if (n < es->ctl_in_len)
        memmove(es->ctl_in, es->ctl_in + n, es->ctl_in_len - n);
    es->ctl_in_len -= n;
}

static void child_write_error(int fd, enum v2_exec_start_failure stage,
                              int errnum)
{
    struct child_error ce;
    ce.stage = (uint32_t)stage;
    ce.errnum = (uint32_t)errnum;
    (void)write(fd, &ce, sizeof(ce));
}

static int make_pipe(int p[2])
{
#ifdef O_CLOEXEC
    if (pipe2(p, O_CLOEXEC) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL)
        return -1;
#endif
    if (pipe(p) < 0)
        return -1;
    (void)fcntl(p[0], F_SETFD, fcntl(p[0], F_GETFD, 0) | FD_CLOEXEC);
    (void)fcntl(p[1], F_SETFD, fcntl(p[1], F_GETFD, 0) | FD_CLOEXEC);
    return 0;
}

static int spawn_exec(struct v2_exec_obj *e, const char *cwd, char **argv)
{
    int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };
    int setup[2] = { -1, -1 };
    pid_t pid;
    if (make_pipe(in) < 0 || make_pipe(out) < 0 ||
        make_pipe(err) < 0 || make_pipe(setup) < 0) {
        e->start_failure = V2_EXEC_FAIL_PIPE;
        e->start_errno = errno;
        goto fail;
    }
    pid = fork();
    if (pid < 0) {
        e->start_failure = V2_EXEC_FAIL_FORK;
        e->start_errno = errno;
        goto fail;
    }
    if (pid == 0) {
        close(in[1]);
        close(out[0]);
        close(err[0]);
        close(setup[0]);
        if (setpgid(0, 0) < 0) {
            child_write_error(setup[1], V2_EXEC_FAIL_SETPGID, errno);
            _exit(127);
        }
        if (cwd && *cwd && chdir(cwd) < 0) {
            child_write_error(setup[1], V2_EXEC_FAIL_CHDIR, errno);
            _exit(127);
        }
        if (dup2(in[0], STDIN_FILENO) < 0 ||
            dup2(out[1], STDOUT_FILENO) < 0 ||
            dup2(err[1], STDERR_FILENO) < 0) {
            child_write_error(setup[1], V2_EXEC_FAIL_PIPE, errno);
            _exit(127);
        }
        (void)fcntl(STDIN_FILENO, F_SETFD, 0);
        (void)fcntl(STDOUT_FILENO, F_SETFD, 0);
        (void)fcntl(STDERR_FILENO, F_SETFD, 0);
        if (in[0] != STDIN_FILENO)
            close(in[0]);
        if (out[1] != STDOUT_FILENO)
            close(out[1]);
        if (err[1] != STDERR_FILENO)
            close(err[1]);
        execve(argv[0], argv, environ);
        child_write_error(setup[1], V2_EXEC_FAIL_EXEC, errno);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    e->stdin_fd = in[1];
    e->stdout_fd = out[0];
    in[1] = out[0] = -1;
    close(err[1]);
    close(setup[1]);
    e->pid = pid;
    e->pgid = pid;
    (void)setpgid(pid, pid);
    e->stderr_fd = err[0];
    e->err_fd = setup[0];
    err[0] = setup[0] = -1;
    (void)set_nonblock_cloexec(e->stdin_fd);
    (void)set_nonblock_cloexec(e->stdout_fd);
    (void)set_nonblock_cloexec(e->stderr_fd);
    (void)set_nonblock_cloexec(e->err_fd);
    return 0;
fail:
    if (in[0] >= 0) close(in[0]);
    if (in[1] >= 0) close(in[1]);
    if (out[0] >= 0) close(out[0]);
    if (out[1] >= 0) close(out[1]);
    if (err[0] >= 0) close(err[0]);
    if (err[1] >= 0) close(err[1]);
    if (setup[0] >= 0) close(setup[0]);
    if (setup[1] >= 0) close(setup[1]);
    e->status = V2_EXEC_STATUS_START_FAILED;
    e->child_reaped = 1;
    e->stdout_eof = 1;
    e->stderr_eof = 1;
    return -1;
}

static int handle_start(struct v2_service *s, const struct v2_exec_record *r)
{
    uint16_t cwd_len, arg_lens[V2_EXEC_ARG_MAX];
    uint8_t argc;
    size_t off = 4, total = 0;
    char storage[V2_EXEC_ARG_BYTES_MAX + V2_EXEC_ARG_MAX + 1];
    char cwd[V2_PATH_MAX];
    char *argv[V2_EXEC_ARG_MAX + 1];
    struct v2_exec_obj *e;
    unsigned i;
    if (r->exec_id != 0 || r->length < 4)
        return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
    cwd_len = lmux_get_le16(r->payload);
    argc = r->payload[2];
    if (argc == 0 || argc > V2_EXEC_ARG_MAX)
        return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
    if (r->length < 4 + (size_t)argc * 2)
        return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
    for (i = 0; i < argc; i++) {
        arg_lens[i] = lmux_get_le16(r->payload + off);
        off += 2;
        if (arg_lens[i] == 0)
            return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
        total += arg_lens[i] + 1;
    }
    if (cwd_len >= sizeof(cwd) || total > sizeof(storage) ||
        off + cwd_len > r->length)
        return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
    if (cwd_len) {
        memcpy(cwd, r->payload + off, cwd_len);
        cwd[cwd_len] = 0;
        if (cwd[0] != '/')
            return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
    } else {
        cwd[0] = 0;
    }
    off += cwd_len;
    total = 0;
    for (i = 0; i < argc; i++) {
        if (off + arg_lens[i] > r->length)
            return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
        argv[i] = storage + total;
        memcpy(argv[i], r->payload + off, arg_lens[i]);
        argv[i][arg_lens[i]] = 0;
        off += arg_lens[i];
        total += arg_lens[i] + 1;
    }
    if (off != r->length || argv[0][0] != '/')
        return queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
    argv[argc] = NULL;
    e = alloc_exec(s);
    if (!e)
        return queue_exec_error(s, 0, V2_EXEC_E_LIMIT);
    if (spawn_exec(e, cwd[0] ? cwd : NULL, argv) < 0) {
        e->completed_seq = ++s->exec.next_completed_seq;
        return queue_exec_status(s, e, V2_EXEC_REC_START_REPLY);
    }
    return 0;
}

static int signal_from_wire(uint8_t sig, int *out)
{
    switch (sig) {
    case V2_EXEC_SIG_INT: *out = SIGINT; return 0;
    case V2_EXEC_SIG_TERM: *out = SIGTERM; return 0;
    case V2_EXEC_SIG_KILL: *out = SIGKILL; return 0;
    case V2_EXEC_SIG_HUP: *out = SIGHUP; return 0;
    }
    return -1;
}

static int handle_signal(struct v2_service *s, const struct v2_exec_record *r)
{
    struct v2_exec_obj *e = find_exec(s, r->exec_id);
    int sig;
    if (!e)
        return queue_exec_error(s, r->exec_id, V2_EXEC_E_NOT_FOUND);
    if (r->length != 1 || signal_from_wire(r->payload[0], &sig) < 0)
        return queue_exec_error(s, r->exec_id, V2_EXEC_E_BAD_REQUEST);
    if (e->child_reaped || e->status == V2_EXEC_STATUS_DONE ||
        e->status == V2_EXEC_STATUS_START_FAILED)
        return queue_exec_error(s, r->exec_id, V2_EXEC_E_NOT_RUNNING);
    if (kill(e->pgid > 0 ? -e->pgid : e->pid, sig) < 0 && errno != ESRCH)
        return queue_exec_error(s, r->exec_id, V2_EXEC_E_IO);
    e->status = V2_EXEC_STATUS_TERMINATING;
    return queue_exec_status(s, e, V2_EXEC_REC_STATUS_REPLY);
}

static int handle_control_record(struct v2_service *s,
                                 const struct v2_exec_record *r)
{
    struct v2_exec_obj *e;
    uint8_t info[8];
    switch (r->type) {
    case V2_EXEC_REC_INFO:
        lmux_put_le64(info, s->exec.incarnation);
        return queue_control_record(s, V2_EXEC_REC_INFO_REPLY, V2_EXEC_OK,
                                    0, info, sizeof(info));
    case V2_EXEC_REC_START:
        return handle_start(s, r);
    case V2_EXEC_REC_SIGNAL:
        return handle_signal(s, r);
    case V2_EXEC_REC_CLOSE_STDIN:
        e = find_exec(s, r->exec_id);
        if (!e)
            return queue_exec_error(s, r->exec_id, V2_EXEC_E_NOT_FOUND);
        e->close_stdin_requested = 1;
        return queue_exec_status(s, e, V2_EXEC_REC_STATUS_REPLY);
    case V2_EXEC_REC_STATUS:
        e = find_exec(s, r->exec_id);
        if (!e)
            return queue_exec_error(s, r->exec_id, V2_EXEC_E_NOT_FOUND);
        return queue_exec_status(s, e, V2_EXEC_REC_STATUS_REPLY);
    case V2_EXEC_REC_RELEASE:
        e = find_exec(s, r->exec_id);
        if (!e)
            return queue_exec_error(s, r->exec_id, V2_EXEC_E_NOT_FOUND);
        if (!e->child_reaped && e->pid > 0)
            exec_obj_kill_and_reap(e);
        exec_obj_reset(e);
        return queue_control_record(s, V2_EXEC_REC_STATUS_REPLY, V2_EXEC_OK,
                                    r->exec_id, NULL, 0);
    default:
        return queue_exec_error(s, r->exec_id, V2_EXEC_E_BAD_REQUEST);
    }
}

static void process_control_stream(struct v2_service *s, struct lmux2_conn *conn)
{
    while (s->exec.control_stream_id &&
           s->exec.ctl_in_len < sizeof(s->exec.ctl_in)) {
        size_t n = lmux2_stream_read(conn, s->exec.control_stream_id,
                                     s->exec.ctl_in + s->exec.ctl_in_len,
                                     sizeof(s->exec.ctl_in) - s->exec.ctl_in_len);
        if (n == 0)
            break;
        s->exec.ctl_in_len += n;
    }
    for (;;) {
        struct v2_exec_record r;
        int rc = parse_record(&s->exec, &r);
        if (rc < 0) {
            (void)queue_exec_error(s, 0, V2_EXEC_E_BAD_REQUEST);
            s->exec.ctl_in_len = 0;
            return;
        }
        if (rc == 0)
            return;
        if (s->exec.ctl_out_len == s->exec.ctl_out_pos) {
            (void)handle_control_record(s, &r);
            consume_record(&s->exec, r.length);
        } else {
            return;
        }
    }
}

static void pump_control_output(struct v2_service *s, struct lmux2_conn *conn)
{
    if (!s->exec.control_stream_id || s->exec.ctl_out_pos >= s->exec.ctl_out_len)
        return;
    while (s->exec.ctl_out_pos < s->exec.ctl_out_len &&
           lmux2_stream_tx_len(conn, s->exec.control_stream_id) < LMUX2_STREAM_TX_CAP) {
        size_t n = s->exec.ctl_out_len - s->exec.ctl_out_pos;
        size_t w = lmux2_stream_write(conn, s->exec.control_stream_id,
                                      s->exec.ctl_out + s->exec.ctl_out_pos, n);
        if (w == 0)
            break;
        s->exec.ctl_out_pos += w;
    }
    if (s->exec.ctl_out_pos == s->exec.ctl_out_len)
        s->exec.ctl_out_pos = s->exec.ctl_out_len = 0;
}

static void clear_pending_start_replies(struct v2_service *s)
{
    int i;
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++)
        s->exec.execs[i].start_reply_pending = 0;
}

static void cleanup_service_control_stream(struct v2_service *s,
                                           struct lmux2_conn *conn)
{
    const struct lmux2_stream *st;
    if (!s->exec.control_stream_id)
        return;
    st = lmux2_find_stream_const(conn, s->exec.control_stream_id);
    if (!st || st->state == LMUX2_STREAM_TERMINAL ||
        st->state == LMUX2_STREAM_RESET) {
        s->exec.control_stream_id = 0;
        s->exec.ctl_in_len = 0;
        s->exec.ctl_out_len = s->exec.ctl_out_pos = 0;
        clear_pending_start_replies(s);
        return;
    }
    if (st->state == LMUX2_STREAM_REMOTE_CLOSED) {
        (void)lmux2_close_stream(conn, st->id);
        s->exec.control_stream_id = 0;
        s->exec.ctl_in_len = 0;
        s->exec.ctl_out_len = s->exec.ctl_out_pos = 0;
        clear_pending_start_replies(s);
    }
}

static void retire_idle_service_control_stream(struct v2_service *s,
                                               struct lmux2_conn *conn)
{
    const struct lmux2_stream *st;
    if (!s->exec.control_stream_id ||
        s->exec.ctl_in_len != 0 ||
        s->exec.ctl_out_len != s->exec.ctl_out_pos)
        return;
    st = lmux2_find_stream_const(conn, s->exec.control_stream_id);
    if (!st)
        return;
    if (st->rx.len != 0)
        return;
    if (st->state == LMUX2_STREAM_OPEN ||
        st->state == LMUX2_STREAM_LOCAL_CLOSED ||
        st->state == LMUX2_STREAM_REMOTE_CLOSED) {
        (void)lmux2_close_stream(conn, st->id);
        s->exec.control_stream_id = 0;
        s->exec.ctl_in_len = 0;
        s->exec.ctl_out_len = s->exec.ctl_out_pos = 0;
        clear_pending_start_replies(s);
    }
}

int v2_exec_handle_stream_open(struct v2_app *a, struct lmux2_conn *conn,
                               struct v2_service *s,
                               struct lmux2_stream *st)
{
    enum v2_exec_role role = token_role(st->open_token);
    uint64_t exec_id = token_exec_id(st->open_token);
    struct v2_exec_obj *e;
    (void)a;
    if (s->type != V2_SERVICE_EXEC)
        return 0;
    if (st->open_token == 0 || role == V2_EXEC_ROLE_CONTROL) {
        cleanup_service_control_stream(s, conn);
        retire_idle_service_control_stream(s, conn);
        if (s->exec.control_stream_id) {
            (void)lmux2_reject_stream(conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return 1;
        }
        if (lmux2_accept_stream(conn, st->id) < 0)
            return 1;
        s->exec.control_stream_id = st->id;
        s->last_stream_id = st->id;
        return 1;
    }
    e = find_exec(s, exec_id);
    if (!e) {
        (void)lmux2_reject_stream(conn, st->id, LMUX2_OPEN_E_REFUSED);
        return 1;
    }
    switch (role) {
    case V2_EXEC_ROLE_STDIN:
        if (e->stdin_stream_id) {
            (void)lmux2_reject_stream(conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return 1;
        }
        e->stdin_stream_id = st->id;
        break;
    case V2_EXEC_ROLE_STDOUT:
        if (e->stdout_stream_id) {
            (void)lmux2_reject_stream(conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return 1;
        }
        e->stdout_stream_id = st->id;
        break;
    case V2_EXEC_ROLE_STDERR:
        if (e->stderr_stream_id) {
            (void)lmux2_reject_stream(conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return 1;
        }
        e->stderr_stream_id = st->id;
        break;
    default:
        (void)lmux2_reject_stream(conn, st->id, LMUX2_OPEN_E_BAD_STATE);
        return 1;
    }
    if (lmux2_accept_stream(conn, st->id) < 0)
        return 1;
    s->last_stream_id = st->id;
    return 1;
}

static void detach_dead_streams(struct v2_exec_obj *e, struct lmux2_conn *conn)
{
    const struct lmux2_stream *st;
    if (e->control_stream_id) {
        st = lmux2_find_stream_const(conn, e->control_stream_id);
        if (!st || st->state == LMUX2_STREAM_TERMINAL ||
            st->state == LMUX2_STREAM_RESET ||
            st->state == LMUX2_STREAM_REMOTE_CLOSED)
            e->control_stream_id = 0;
    }
    if (e->stdin_stream_id) {
        st = lmux2_find_stream_const(conn, e->stdin_stream_id);
        if (!st || st->state == LMUX2_STREAM_RESET ||
            ((st->state == LMUX2_STREAM_TERMINAL ||
              st->state == LMUX2_STREAM_REMOTE_CLOSED) &&
             e->stdin_fd < 0 &&
             e->stdin_pos == e->stdin_len &&
             lmux2_stream_rx_len(conn, e->stdin_stream_id) == 0))
            e->stdin_stream_id = 0;
    }
    if (e->stdout_stream_id) {
        st = lmux2_find_stream_const(conn, e->stdout_stream_id);
        if (!st || st->state == LMUX2_STREAM_TERMINAL ||
            st->state == LMUX2_STREAM_RESET ||
            st->state == LMUX2_STREAM_REMOTE_CLOSED)
            e->stdout_stream_id = 0;
    }
    if (e->stderr_stream_id) {
        st = lmux2_find_stream_const(conn, e->stderr_stream_id);
        if (!st || st->state == LMUX2_STREAM_TERMINAL ||
            st->state == LMUX2_STREAM_RESET ||
            st->state == LMUX2_STREAM_REMOTE_CLOSED)
            e->stderr_stream_id = 0;
    }
}

void v2_exec_cleanup_connection(struct v2_app *a, struct v2_service *s,
                                struct lmux2_conn *conn)
{
    int i;
    (void)a;
    s->exec.control_stream_id = 0;
    s->exec.ctl_in_len = 0;
    s->exec.ctl_out_len = s->exec.ctl_out_pos = 0;
    clear_pending_start_replies(s);
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++)
        exec_obj_close_streams(conn, &s->exec.execs[i], 0);
}

static int stream_can_send(const struct lmux2_conn *conn, uint64_t stream_id)
{
    const struct lmux2_stream *st = lmux2_find_stream_const(conn, stream_id);
    return st && (st->state == LMUX2_STREAM_OPEN ||
                  st->state == LMUX2_STREAM_REMOTE_CLOSED);
}

static void pump_child_output(struct v2_exec_obj *e, struct lmux2_conn *conn,
                              int fd, uint64_t stream_id, int *eof)
{
    uint8_t *pending;
    size_t *pending_len, *pending_pos;
    uint8_t buf[256];
    int can_send = stream_id && stream_can_send(conn, stream_id);
    int is_stdout = eof == &e->stdout_eof;
    int actual_fd = is_stdout ? e->stdout_fd : e->stderr_fd;
    (void)fd;
    if (is_stdout) {
        pending = e->stdout_buf;
        pending_len = &e->stdout_len;
        pending_pos = &e->stdout_pos;
    } else {
        pending = e->stderr_buf;
        pending_len = &e->stderr_len;
        pending_pos = &e->stderr_pos;
    }
    while (can_send && *pending_pos < *pending_len &&
           lmux2_stream_tx_len(conn, stream_id) < LMUX2_STREAM_TX_CAP) {
        size_t w = lmux2_stream_write(conn, stream_id,
                                      pending + *pending_pos,
                                      *pending_len - *pending_pos);
        if (!w)
            return;
        if (is_stdout)
            e->stdout_lmux_written_total += (unsigned long long)w;
        else
            e->stderr_lmux_written_total += (unsigned long long)w;
        *pending_pos += w;
    }
    if (*pending_pos < *pending_len)
        return;
    *pending_len = *pending_pos = 0;
    if (actual_fd < 0 || *eof) {
        if (*eof && stream_id)
            (void)lmux2_close_stream(conn, stream_id);
        return;
    }
    while ((can_send && lmux2_stream_tx_len(conn, stream_id) < LMUX2_STREAM_TX_CAP) ||
           (!can_send && *pending_len < V2_EXEC_OUTPUT_BUF_MAX)) {
        size_t space = can_send ?
            LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(conn, stream_id) :
            V2_EXEC_OUTPUT_BUF_MAX - *pending_len;
        ssize_t r;
        size_t w;
        if (space > sizeof(buf))
            space = sizeof(buf);
        r = read(fd, buf, space);
        if (r > 0) {
            if (is_stdout)
                e->stdout_child_read_total += (unsigned long long)r;
            else
                e->stderr_child_read_total += (unsigned long long)r;
            if (can_send) {
                w = lmux2_stream_write(conn, stream_id, buf, (size_t)r);
                if (is_stdout)
                    e->stdout_lmux_written_total += (unsigned long long)w;
                else
                    e->stderr_lmux_written_total += (unsigned long long)w;
                if (w < (size_t)r) {
                    size_t left = (size_t)r - w;
                    if (left > V2_EXEC_OUTPUT_BUF_MAX)
                        left = V2_EXEC_OUTPUT_BUF_MAX;
                    memcpy(pending, buf + w, left);
                    *pending_len = left;
                    *pending_pos = 0;
                    return;
                }
            } else {
                memcpy(pending + *pending_len, buf, (size_t)r);
                *pending_len += (size_t)r;
                return;
            }
            continue;
        }
        if (r == 0) {
            *eof = 1;
            if (is_stdout)
                exec_close_stdout_side(e);
            else
                close_fd(&e->stderr_fd);
            if (stream_id)
                (void)lmux2_close_stream(conn, stream_id);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        *eof = 1;
        if (is_stdout)
            exec_close_stdout_side(e);
        else
            close_fd(&e->stderr_fd);
        if (stream_id)
            (void)lmux2_close_stream(conn, stream_id);
        return;
    }
}

static void pump_stdin_rx(struct v2_exec_obj *e, struct lmux2_conn *conn)
{
    const struct lmux2_stream *st;
    if (e->stdin_fd < 0)
        return;
    if (e->stdin_len == e->stdin_pos) {
        e->stdin_len = e->stdin_pos = 0;
        if (e->stdin_stream_id) {
            e->stdin_len = lmux2_stream_read(conn, e->stdin_stream_id,
                                             e->stdin_buf,
                                             sizeof(e->stdin_buf));
        }
    }
    st = e->stdin_stream_id ?
        lmux2_find_stream_const(conn, e->stdin_stream_id) : NULL;
    if (st && (st->state == LMUX2_STREAM_REMOTE_CLOSED ||
               st->state == LMUX2_STREAM_TERMINAL) &&
        lmux2_stream_rx_len(conn, e->stdin_stream_id) == 0 &&
        e->stdin_len == e->stdin_pos) {
        uint64_t stream_id = e->stdin_stream_id;
        exec_close_stdin_side(e);
        e->stdin_stream_id = 0;
        (void)lmux2_close_stream(conn, stream_id);
    }
}

static void write_child_stdin(struct v2_exec_obj *e)
{
    while (e->stdin_fd >= 0 && e->stdin_pos < e->stdin_len) {
        ssize_t w = write(e->stdin_fd, e->stdin_buf + e->stdin_pos,
                          e->stdin_len - e->stdin_pos);
        if (w > 0) {
            e->stdin_pos += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        exec_close_stdin_side(e);
        return;
    }
    if (e->stdin_len == e->stdin_pos)
        e->stdin_len = e->stdin_pos = 0;
    if (e->close_stdin_requested && e->stdin_len == 0) {
        exec_close_stdin_side(e);
    }
}

static void check_start_pipe(struct v2_service *s, struct v2_exec_obj *e)
{
    struct child_error ce;
    ssize_t r;
    if (e->err_fd < 0 || e->status != V2_EXEC_STATUS_STARTING)
        return;
    r = read(e->err_fd, &ce, sizeof(ce));
    if (r == 0) {
        close_fd(&e->err_fd);
        e->status = V2_EXEC_STATUS_RUNNING;
        e->start_reply_pending = 1;
        (void)queue_pending_start_reply(s, e);
    } else if (r == (ssize_t)sizeof(ce)) {
        close_fd(&e->err_fd);
        e->start_failure = (enum v2_exec_start_failure)ce.stage;
        e->start_errno = (int)ce.errnum;
        e->status = V2_EXEC_STATUS_START_FAILED;
        e->start_reply_pending = 1;
        (void)queue_pending_start_reply(s, e);
    } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
    }
}

static void reap_child(struct v2_service *s, struct v2_exec_obj *e)
{
    int status;
    pid_t r;
    if (!e->used || e->child_reaped || e->pid <= 0)
        return;
    r = waitpid(e->pid, &status, WNOHANG);
    if (r <= 0)
        return;
    e->child_reaped = 1;
    if (WIFEXITED(status)) {
        e->exit_code = WEXITSTATUS(status);
        if (e->status != V2_EXEC_STATUS_START_FAILED)
            e->status = V2_EXEC_STATUS_EXITED_DRAINING;
    } else if (WIFSIGNALED(status)) {
        e->term_signal = WTERMSIG(status);
        if (e->status != V2_EXEC_STATUS_START_FAILED)
            e->status = V2_EXEC_STATUS_EXITED_DRAINING;
    }
    exec_close_stdin_side(e);
    if ((e->stdout_eof || e->stdout_fd < 0) &&
        (e->stderr_eof || e->stderr_fd < 0) &&
        e->status != V2_EXEC_STATUS_START_FAILED) {
        e->status = V2_EXEC_STATUS_DONE;
        e->completed_seq = ++s->exec.next_completed_seq;
    }
}

static void maybe_done(struct v2_service *s, struct v2_exec_obj *e)
{
    if (!e->used || !e->child_reaped || e->status == V2_EXEC_STATUS_DONE ||
        e->status == V2_EXEC_STATUS_START_FAILED)
        return;
    if ((e->stdout_eof || e->stdout_fd < 0) &&
        (e->stderr_eof || e->stderr_fd < 0) &&
        e->stdout_len == e->stdout_pos &&
        e->stderr_len == e->stderr_pos) {
        e->status = V2_EXEC_STATUS_DONE;
        e->completed_seq = ++s->exec.next_completed_seq;
    }
}

int v2_exec_service_step(struct v2_app *a, struct lmux2_conn *conn,
                         struct v2_service *s)
{
    int i, progress = 0;
    (void)a;
    if (!s || s->type != V2_SERVICE_EXEC)
        return 0;
    cleanup_service_control_stream(s, conn);
    if (s->exec.control_stream_id) {
        process_control_stream(s, conn);
        pump_control_output(s, conn);
        for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
            struct v2_exec_obj *e = &s->exec.execs[i];
            if (e->used && e->start_reply_pending &&
                queue_pending_start_reply(s, e) == 0)
                break;
        }
    }
    for (i = 0; i < V2_EXEC_MAX_SLOTS; i++) {
        struct v2_exec_obj *e = &s->exec.execs[i];
        if (!e->used)
            continue;
        detach_dead_streams(e, conn);
        check_start_pipe(s, e);
        if (e->start_reply_pending)
            (void)queue_pending_start_reply(s, e);
        pump_stdin_rx(e, conn);
        write_child_stdin(e);
        pump_child_output(e, conn, e->stdout_fd, e->stdout_stream_id, &e->stdout_eof);
        pump_child_output(e, conn, e->stderr_fd, e->stderr_stream_id, &e->stderr_eof);
        reap_child(s, e);
        maybe_done(s, e);
        progress = 1;
    }
    gc_completed(s);
    return progress;
}

short v2_exec_fd_events(struct v2_app *a, struct lmux2_conn *conn,
                        struct v2_service *s, int exec_idx, int fd_kind)
{
    struct v2_exec_obj *e;
    (void)a;
    if (!s || s->type != V2_SERVICE_EXEC ||
        exec_idx < 0 || exec_idx >= V2_EXEC_MAX_SLOTS)
        return 0;
    e = &s->exec.execs[exec_idx];
    if (!e->used)
        return 0;
    switch (fd_kind) {
    case V2_EXEC_FD_STDIN:
        return e->stdin_fd >= 0 && e->stdin_len > e->stdin_pos ? POLLOUT : 0;
    case V2_EXEC_FD_STDOUT:
        return e->stdout_fd >= 0 && !e->stdout_eof &&
            e->stdout_len < V2_EXEC_OUTPUT_BUF_MAX &&
            (!e->stdout_stream_id ||
             (stream_can_send(conn, e->stdout_stream_id) &&
              lmux2_stream_tx_len(conn, e->stdout_stream_id) < LMUX2_STREAM_TX_CAP)) ?
            POLLIN : 0;
    case V2_EXEC_FD_STDERR:
        return e->stderr_fd >= 0 && !e->stderr_eof &&
            e->stderr_len < V2_EXEC_OUTPUT_BUF_MAX &&
            (!e->stderr_stream_id ||
             (stream_can_send(conn, e->stderr_stream_id) &&
              lmux2_stream_tx_len(conn, e->stderr_stream_id) < LMUX2_STREAM_TX_CAP)) ?
            POLLIN : 0;
    case V2_EXEC_FD_ERR:
        return e->err_fd >= 0 ? POLLIN : 0;
    }
    return 0;
}

void v2_exec_on_fd_event(struct v2_app *a, struct lmux2_conn *conn,
                         struct v2_service *s, int exec_idx, int fd_kind,
                         short revents)
{
    struct v2_exec_obj *e;
    (void)a;
    if (!s || s->type != V2_SERVICE_EXEC ||
        exec_idx < 0 || exec_idx >= V2_EXEC_MAX_SLOTS)
        return;
    e = &s->exec.execs[exec_idx];
    if (!e->used)
        return;
    if (fd_kind == V2_EXEC_FD_STDIN && (revents & POLLOUT))
        write_child_stdin(e);
    else if (fd_kind == V2_EXEC_FD_STDOUT && (revents & (POLLIN | POLLHUP)))
        pump_child_output(e, conn, e->stdout_fd, e->stdout_stream_id, &e->stdout_eof);
    else if (fd_kind == V2_EXEC_FD_STDERR && (revents & (POLLIN | POLLHUP)))
        pump_child_output(e, conn, e->stderr_fd, e->stderr_stream_id, &e->stderr_eof);
    else if (fd_kind == V2_EXEC_FD_ERR && (revents & (POLLIN | POLLHUP)))
        check_start_pipe(s, e);
    if (revents & (POLLERR | POLLNVAL)) {
        if (fd_kind == V2_EXEC_FD_STDOUT)
        {
            e->stdout_eof = 1;
            exec_close_stdout_side(e);
        }
        else if (fd_kind == V2_EXEC_FD_STDERR)
        {
            e->stderr_eof = 1;
            close_fd(&e->stderr_fd);
        }
        else if (fd_kind == V2_EXEC_FD_STDIN) {
            exec_close_stdin_side(e);
        } else if (fd_kind == V2_EXEC_FD_ERR)
            check_start_pipe(s, e);
    } else if ((revents & POLLHUP) && fd_kind == V2_EXEC_FD_STDIN) {
        exec_close_stdin_side(e);
    }
    reap_child(s, e);
    maybe_done(s, e);
}

uint64_t v2_exec_stream_token(uint64_t exec_id, enum v2_exec_role role)
{
    return pack_token(exec_id, role);
}
