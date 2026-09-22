#include "lmux2_runtime.h"
#include "lmux2_core.h"
#include "lmux.h"
#include "lxf.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef CRTSCTS
#define CRTSCTS 020000000000
#endif

#define V2_MAX_SERVICES 8
#define V2_MAX_DESIRED 8
#define V2_MAX_BINDINGS 8
#define V2_NAME_MAX 32
#define V2_PATH_MAX 108
#define V2_CTRL_VERSION 1
#define V2_CTRL_MAX_TLVS 3

enum v2_service_type {
    V2_SERVICE_UNIX_RENDEZVOUS = 1,
    V2_SERVICE_PTY = 2,
    V2_SERVICE_FILE = 3
};

enum v2_ctrl_kind {
    V2_CTRL_FACTORY_REPORT = 1,
    V2_CTRL_SERVICE_ENSURE = 2,
    V2_CTRL_SERVICE_OK = 3,
    V2_CTRL_SERVICE_ERROR = 4,
    V2_CTRL_SERVICE_DELETE = 5,
    V2_CTRL_SERVICE_STATE = 6,
    V2_CTRL_SERVICE_EVENT = 7,
    V2_CTRL_SERVICE_CONTROL = 8
};

enum v2_ctrl_error {
    V2_E_NOTFOUND = 1,
    V2_E_CONFLICT = 2,
    V2_E_LIMIT = 3,
    V2_E_BAD_REQUEST = 4,
    V2_E_IO = 5
};

enum v2_event {
    V2_EVENT_PENDING_CLIENT = 1,
    V2_EVENT_PENDING_CLEARED = 2,
    V2_EVENT_DELETED = 3,
    V2_EVENT_WINSIZE = 4
};

enum v2_tlv_type {
    V2_TLV_NAME = 1,
    V2_TLV_SERVICE_TYPE = 2,
    V2_TLV_TARGET_PATH = 3,
    V2_TLV_ERROR_CODE = 4,
    V2_TLV_EVENT = 5,
    V2_TLV_ARGV = 6,
    V2_TLV_SUPERVISED = 7,
    V2_TLV_WINSIZE = 8
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
    pid_t child_pid;
    long long respawn_at_ms;
    int supervised;
    uint64_t active_stream_id;
    struct v2_file_session file;
    struct v2_binding bindings[V2_MAX_BINDINGS];
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

struct v2_app {
    enum lmux2_role role;
    const char *carrier_path;
    int carrier_fd;
    int carrier_fd_owned;
    int verbose;
    int stop;
    int factories_reported;
    int factory_unix;
    int factory_pty;
    int factory_file;
    int desired_sent;
    uint64_t next_service_id;
    uint32_t next_req_id;
    const char *control_path;
    int control_listen_fd;
    int control_fd;
    char control_in[256];
    size_t control_in_len;
    const char *carrier_listen_path;
    const char *carrier_connect_path;
    int carrier_listen_fd;
    int carrier_created_path;
    struct lmux2_conn conn;
    uint8_t enc_in[LMUX2_MAX_ENCODED];
    size_t enc_in_len;
    int discard_until_delim;
    uint8_t out[LMUX2_MAX_ENCODED];
    size_t out_len;
    size_t out_pos;
    struct v2_service services[V2_MAX_SERVICES];
    struct v2_desired desired[V2_MAX_DESIRED];
};

static struct v2_app *global_v2_app;

static void reset_connection_streams(struct v2_app *a);
static void pty_terminate_child(struct v2_service *s);
static void file_session_reset(struct v2_file_session *fs);

static void log_msg(struct v2_app *a, int verbose, const char *fmt, ...)
{
    va_list ap;
    if (verbose && !a->verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void cleanup_terminal_bindings(struct v2_app *a);

static int configure_tty(int fd)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
        return 0;
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_iflag &= ~(IXON | IXOFF | ICRNL | INLCR | IGNCR);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    return tcsetattr(fd, TCSANOW, &tio);
}

static uint64_t make_conn_id(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
            v = 0;
        close(fd);
    }
    if (!v)
        v = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)time(NULL) ^ 0x4c4d5832u;
    if (!v)
        v = 1;
    return v;
}

static int parse_role(const char *s, enum lmux2_role *role)
{
    if (strcmp(s, "host") == 0) {
        *role = LMUX2_ROLE_HOST;
        return 0;
    }
    if (strcmp(s, "target") == 0) {
        *role = LMUX2_ROLE_TARGET;
        return 0;
    }
    return -1;
}

static int copy_bounded(char *out, size_t cap, const char *in)
{
    size_t n = strlen(in);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(out, in, n + 1);
    return 0;
}

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

static int add_desired_unix(struct v2_app *a, const char *arg)
{
    int i;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        if (!a->desired[i].used) {
            a->desired[i].used = 1;
            a->desired[i].type = V2_SERVICE_UNIX_RENDEZVOUS;
            return split_unix_arg(arg, a->desired[i].name,
                                  a->desired[i].host_path,
                                  a->desired[i].target_path);
        }
    }
    return -1;
}

static int add_desired_file(struct v2_app *a, const char *arg)
{
    int i;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        if (!a->desired[i].used) {
            a->desired[i].used = 1;
            a->desired[i].type = V2_SERVICE_FILE;
            return split_file_arg(arg, a->desired[i].name,
                                  a->desired[i].host_path);
        }
    }
    return -1;
}

static int add_desired_pty(struct v2_app *a, const char *arg)
{
    int i;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        if (!a->desired[i].used) {
            a->desired[i].used = 1;
            a->desired[i].type = V2_SERVICE_PTY;
            a->desired[i].supervised = 1;
            return split_pty_arg(arg, a->desired[i].name,
                                 a->desired[i].host_path,
                                 a->desired[i].argv);
        }
    }
    return -1;
}

static void cleanup_service(struct v2_service *s)
{
    int i;
    file_session_reset(&s->file);
    if (s->type == V2_SERVICE_PTY)
        pty_terminate_child(s);
    close_fd(&s->pty_master_fd);
    close_fd(&s->pending_fd);
    close_fd(&s->listen_fd);
    if (s->created_path && s->listen_path[0])
        unlink(s->listen_path);
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        close_fd(&s->bindings[i].fd);
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;
    s->pending_fd = -1;
    s->pty_master_fd = -1;
    s->file.fd = -1;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        s->bindings[i].fd = -1;
}

static void init_service_slots(struct v2_app *a)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        a->services[i].listen_fd = -1;
        a->services[i].pending_fd = -1;
        a->services[i].pty_master_fd = -1;
        a->services[i].file.fd = -1;
        for (j = 0; j < V2_MAX_BINDINGS; j++)
            a->services[i].bindings[j].fd = -1;
    }
}

static struct v2_service *find_service_id(struct v2_app *a, uint64_t id)
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (a->services[i].used && a->services[i].id == id)
            return &a->services[i];
    return NULL;
}

static struct v2_service *find_service_name(struct v2_app *a, const char *name)
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (a->services[i].used && strcmp(a->services[i].name, name) == 0)
            return &a->services[i];
    return NULL;
}

static struct v2_service *alloc_service(struct v2_app *a)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        if (!a->services[i].used) {
            memset(&a->services[i], 0, sizeof(a->services[i]));
            a->services[i].listen_fd = -1;
            a->services[i].pending_fd = -1;
            a->services[i].pty_master_fd = -1;
            a->services[i].file.fd = -1;
            for (j = 0; j < V2_MAX_BINDINGS; j++)
                a->services[i].bindings[j].fd = -1;
            a->services[i].used = 1;
            a->services[i].id = a->next_service_id++;
            if (a->services[i].id == 0)
                a->services[i].id = a->next_service_id++;
            a->services[i].next_token = 1;
            return &a->services[i];
        }
    }
    return NULL;
}

static struct v2_binding *find_binding(struct v2_app *a, uint64_t stream_id,
                                       struct v2_service **svc_out)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
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

static struct v2_binding *alloc_binding(struct v2_service *s)
{
    int i;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        if (s->bindings[i].fd < 0)
            return &s->bindings[i];
    return NULL;
}

static void child_close_daemon_fds(void)
{
    int fd;
    for (fd = 3; fd < 256; fd++)
        close(fd);
}

static void pty_restore_baseline(struct v2_service *s)
{
    int fd;
    if (!s->pty_have_baseline_tio || !s->pty_slave_path[0])
        return;
    fd = open(s->pty_slave_path, O_RDWR | O_NOCTTY);
    if (fd >= 0) {
        (void)tcsetattr(fd, TCSANOW, &s->pty_baseline_tio);
        close(fd);
    }
}

static int pty_create_master(struct v2_service *s)
{
    int unlock = 0;
    int ptn = -1;
    int sfd;
    s->pty_master_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (s->pty_master_fd < 0)
        return -1;
    if (ioctl(s->pty_master_fd, TIOCSPTLCK, &unlock) < 0 ||
        ioctl(s->pty_master_fd, TIOCGPTN, &ptn) < 0)
        return -1;
    snprintf(s->pty_slave_path, sizeof(s->pty_slave_path), "/dev/pts/%d", ptn);
    sfd = open(s->pty_slave_path, O_RDWR | O_NOCTTY);
    if (sfd >= 0) {
        if (tcgetattr(sfd, &s->pty_baseline_tio) == 0)
            s->pty_have_baseline_tio = 1;
        close(sfd);
    }
    return 0;
}

static void pty_spawn_child(struct v2_app *a, struct v2_service *s)
{
    pid_t pid;
    if (s->type != V2_SERVICE_PTY || !s->supervised ||
        s->child_pid > 0 || s->pty_master_fd < 0)
        return;
    pty_restore_baseline(s);
    pid = fork();
    if (pid < 0) {
        log_msg(a, 0, "service %s fork %s failed: %s",
                s->name, s->argv, strerror(errno));
        s->respawn_at_ms = now_ms() + 1000;
        return;
    }
    if (pid == 0) {
        pid_t ppid = getppid();
        int sfd;
        prctl(PR_SET_PDEATHSIG, SIGHUP);
        if (getppid() != ppid)
            _exit(127);
        if (setsid() < 0)
            _exit(127);
        sfd = open(s->pty_slave_path, O_RDWR);
        if (sfd < 0)
            _exit(127);
        (void)ioctl(sfd, TIOCSCTTY, 0);
        if (s->pty_have_baseline_tio)
            (void)tcsetattr(sfd, TCSANOW, &s->pty_baseline_tio);
        if (dup2(sfd, STDIN_FILENO) < 0 ||
            dup2(sfd, STDOUT_FILENO) < 0 ||
            dup2(sfd, STDERR_FILENO) < 0)
            _exit(127);
        if (sfd > STDERR_FILENO)
            close(sfd);
        setenv("TERM", "vt100", 1);
        child_close_daemon_fds();
        execl(s->argv, s->argv, (char *)NULL);
        _exit(127);
    }
    s->child_pid = pid;
    s->respawn_at_ms = 0;
    log_msg(a, 1, "lmux2 pty service %s spawned %s pid=%ld",
            s->name, s->argv, (long)pid);
}

static void pty_terminate_child(struct v2_service *s)
{
    long long deadline;
    if (s->child_pid <= 0)
        return;
    kill(s->child_pid, SIGHUP);
    deadline = now_ms() + 1000;
    while (now_ms() < deadline) {
        int st;
        pid_t r = waitpid(s->child_pid, &st, WNOHANG);
        if (r == s->child_pid) {
            s->child_pid = 0;
            return;
        }
        usleep(20000);
    }
    kill(s->child_pid, SIGKILL);
    (void)waitpid(s->child_pid, NULL, 0);
    s->child_pid = 0;
}

static int create_listener(const char *path, int *created_path)
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

static int lxf_abs_path_ok(const char *p)
{
    return p && p[0] == '/' && strlen(p) < LXF_PATH_MAX;
}

static int lxf_make_tmp_for_final(const char *final, char *tmp, size_t cap)
{
    const char *slash = strrchr(final, '/');
    const char *base;
    size_t dir_len;
    if (!slash || final[0] != '/')
        return -1;
    base = slash + 1;
    if (!*base)
        return -1;
    dir_len = (size_t)(slash - final);
    if (dir_len == 0)
        dir_len = 1;
    if (snprintf(tmp, cap, "%.*s/.%s.lmux-tmp-%ld-%ld",
                 (int)dir_len, final, base, (long)getpid(), (long)time(NULL)) >= (int)cap)
        return -1;
    return 0;
}

static void file_session_reset(struct v2_file_session *fs)
{
    if (fs->fd >= 0)
        close(fs->fd);
    if (fs->tmp_path[0])
        unlink(fs->tmp_path);
    memset(fs, 0, sizeof(*fs));
    fs->fd = -1;
}

static void file_session_abort_keep_output(struct v2_file_session *fs)
{
    uint8_t out[LXF_MAX_RECORD];
    size_t out_len = fs->out_len;
    size_t out_pos = fs->out_pos;
    if (out_len)
        memcpy(out, fs->outbuf, out_len);
    if (fs->fd >= 0)
        close(fs->fd);
    if (fs->tmp_path[0])
        unlink(fs->tmp_path);
    memset(fs, 0, sizeof(*fs));
    fs->fd = -1;
    if (out_len) {
        memcpy(fs->outbuf, out, out_len);
        fs->out_len = out_len;
        fs->out_pos = out_pos;
    }
}

static int file_queue_record(struct v2_file_session *fs, uint8_t type,
                             const uint8_t *payload, uint32_t len)
{
    if (fs->out_len != fs->out_pos)
        return -1;
    fs->out_len = fs->out_pos = 0;
    return lxf_encode_record(type, 0, payload, len, fs->outbuf,
                             sizeof(fs->outbuf), &fs->out_len);
}

static void file_send_error(struct v2_file_session *fs, uint32_t code, const char *msg)
{
    uint8_t p[160];
    size_t n = strlen(msg);
    if (n > sizeof(p) - 5)
        n = sizeof(p) - 5;
    lmux_put_le32(p, code);
    memcpy(p + 4, msg, n);
    p[4 + n] = 0;
    (void)file_queue_record(fs, LXF_T_ERROR, p, (uint32_t)(5 + n));
    file_session_abort_keep_output(fs);
}

static void file_send_ok_simple(struct v2_file_session *fs)
{
    (void)file_queue_record(fs, LXF_T_OK, NULL, 0);
}

static void file_handle_put_request(struct v2_app *a, struct v2_service *s,
                                    const uint8_t *p, uint32_t len)
{
    struct v2_file_session *fs = &s->file;
    struct stat st;
    const char *path;
    if (len < 17) {
        file_send_error(fs, LXF_E_PROTOCOL, "short PUT request");
        return;
    }
    path = (const char *)p + 16;
    if (memchr(path, 0, len - 16) == NULL || !lxf_abs_path_ok(path)) {
        file_send_error(fs, LXF_E_BADPATH, "TARGET path must be absolute");
        return;
    }
    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            file_send_error(fs, LXF_E_NOTREG, "destination is not regular");
            return;
        }
    } else if (errno != ENOENT) {
        file_send_error(fs, errno == EACCES ? LXF_E_DENIED : LXF_E_IO, strerror(errno));
        return;
    }
    if (lxf_make_tmp_for_final(path, fs->tmp_path, sizeof(fs->tmp_path)) < 0) {
        file_send_error(fs, LXF_E_BADPATH, "bad destination path");
        return;
    }
    fs->fd = open(fs->tmp_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fs->fd < 0) {
        file_send_error(fs, errno == ENOSPC ? LXF_E_NOSPACE : LXF_E_IO, strerror(errno));
        return;
    }
    strncpy(fs->path, path, sizeof(fs->path) - 1);
    fs->mode = lmux_get_le32(p) & 0777;
    fs->expected_size = lmux_get_le64(p + 4);
    fs->size_known = (lmux_get_le32(p + 12) & LXF_F_SIZE_KNOWN) != 0;
    fs->count = 0;
    fs->state = V2_FILE_PUT;
    sha256_init(&fs->sha);
    log_msg(a, 1, "lmux2 file service %s PUT path=%s size=%llu%s",
            s->name, fs->path, (unsigned long long)fs->expected_size,
            fs->size_known ? "" : " unknown");
    file_send_ok_simple(fs);
}

static void file_handle_get_request(struct v2_app *a, struct v2_service *s,
                                    const uint8_t *p, uint32_t len)
{
    struct v2_file_session *fs = &s->file;
    struct stat st;
    uint8_t ok[16];
    const char *path = (const char *)p;
    if (len < 2 || memchr(p, 0, len) == NULL || !lxf_abs_path_ok(path)) {
        file_send_error(fs, LXF_E_BADPATH, "TARGET path must be absolute");
        return;
    }
    if (lstat(path, &st) < 0) {
        file_send_error(fs, errno == ENOENT ? LXF_E_NOTFOUND : LXF_E_IO, strerror(errno));
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        file_send_error(fs, LXF_E_NOTREG, "source is not regular");
        return;
    }
    fs->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fs->fd < 0) {
        file_send_error(fs, errno == EACCES ? LXF_E_DENIED : LXF_E_IO, strerror(errno));
        return;
    }
    fs->state = V2_FILE_GET;
    fs->count = 0;
    fs->mode = st.st_mode & 0777;
    fs->expected_size = (uint64_t)st.st_size;
    sha256_init(&fs->sha);
    lmux_put_le32(ok, fs->mode);
    lmux_put_le64(ok + 4, fs->expected_size);
    lmux_put_le32(ok + 12, LXF_F_SIZE_KNOWN);
    (void)file_queue_record(fs, LXF_T_OK, ok, sizeof(ok));
    log_msg(a, 1, "lmux2 file service %s GET path=%s size=%llu",
            s->name, path, (unsigned long long)fs->expected_size);
}

static void file_handle_cwd_request(struct v2_app *a, struct v2_service *s,
                                    const uint8_t *p, uint32_t len)
{
    struct v2_file_session *fs = &s->file;
    struct v2_service *pty = NULL;
    char proc[64], cwd[LXF_PATH_MAX];
    ssize_t n;
    if (len < 1) {
        file_send_error(fs, LXF_E_PROTOCOL, "short CWD request");
        return;
    }
    if (p[0] == 1)
        pty = find_service_name(a, "console1");
    else if (p[0] == 2)
        pty = find_service_name(a, "console2");
    if (!pty || pty->type != V2_SERVICE_PTY) {
        file_send_error(fs, LXF_E_BADPATH, "channel is not a supervised shell");
        return;
    }
    if (pty->child_pid <= 0) {
        file_send_error(fs, LXF_E_IO, "shell is not running");
        return;
    }
    snprintf(proc, sizeof(proc), "/proc/%ld/cwd", (long)pty->child_pid);
    n = readlink(proc, cwd, sizeof(cwd) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(cwd) - 1) {
        file_send_error(fs, LXF_E_IO, "target cwd unavailable");
        return;
    }
    cwd[n] = 0;
    if (!lxf_abs_path_ok(cwd)) {
        file_send_error(fs, LXF_E_BADPATH, "target cwd is not absolute");
        return;
    }
    (void)file_queue_record(fs, LXF_T_CWD_REPLY, (const uint8_t *)cwd,
                            (uint32_t)(strlen(cwd) + 1));
}

static void file_handle_record(struct v2_app *a, struct v2_service *s,
                               uint8_t type, const uint8_t *p, uint32_t len)
{
    struct v2_file_session *fs = &s->file;
    if (fs->state == V2_FILE_IDLE) {
        if (type == LXF_T_PUT_REQUEST)
            file_handle_put_request(a, s, p, len);
        else if (type == LXF_T_GET_REQUEST)
            file_handle_get_request(a, s, p, len);
        else if (type == LXF_T_CWD_REQUEST)
            file_handle_cwd_request(a, s, p, len);
        else
            file_send_error(fs, LXF_E_PROTOCOL, "expected request");
    } else if (fs->state == V2_FILE_PUT) {
        if (type == LXF_T_DATA) {
            if (len && write(fs->fd, p, len) != (ssize_t)len) {
                file_send_error(fs, errno == ENOSPC ? LXF_E_NOSPACE : LXF_E_IO, strerror(errno));
                return;
            }
            sha256_update(&fs->sha, p, len);
            fs->count += len;
        } else if (type == LXF_T_DONE) {
            uint8_t digest[32], resp[40];
            if (len < 40) {
                file_send_error(fs, LXF_E_PROTOCOL, "short DONE");
                return;
            }
            sha256_final(&fs->sha, digest);
            if ((fs->size_known && fs->count != fs->expected_size) ||
                lmux_get_le64(p) != fs->count || memcmp(p + 8, digest, 32) != 0) {
                file_send_error(fs, LXF_E_HASH, "PUT hash/size mismatch");
                return;
            }
            (void)fchmod(fs->fd, fs->mode);
            (void)fsync(fs->fd);
            close(fs->fd);
            fs->fd = -1;
            if (rename(fs->tmp_path, fs->path) < 0) {
                file_send_error(fs, LXF_E_IO, strerror(errno));
                return;
            }
            fs->tmp_path[0] = 0;
            lmux_put_le64(resp, fs->count);
            memcpy(resp + 8, digest, 32);
            fs->state = V2_FILE_IDLE;
            (void)file_queue_record(fs, LXF_T_OK, resp, sizeof(resp));
        } else {
            file_send_error(fs, LXF_E_PROTOCOL, "bad PUT message");
        }
    } else {
        file_send_error(fs, LXF_E_PROTOCOL, "busy");
    }
}

static int connect_unix_carrier(const char *path)
{
    struct sockaddr_un sa;
    int fd;
    if (strlen(path) >= sizeof(sa.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

static void carrier_lost(struct v2_app *a)
{
    reset_connection_streams(a);
    if (a->carrier_listen_fd >= 0)
        close_fd(&a->carrier_fd);
}

static void accept_carrier(struct v2_app *a)
{
    int fd = accept(a->carrier_listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    (void)set_nonblock(fd);
    close_fd(&a->carrier_fd);
    a->carrier_fd = fd;
    a->carrier_fd_owned = 1;
    reset_connection_streams(a);
}

static int ctrl_put_tlv(uint8_t *p, size_t cap, size_t *off,
                        uint16_t type, const void *data, uint16_t len)
{
    if (*off + 4u + len > cap)
        return -1;
    lmux_put_le16(p + *off, type);
    lmux_put_le16(p + *off + 2, len);
    if (len)
        memcpy(p + *off + 4, data, len);
    *off += 4u + len;
    return 0;
}

static int ctrl_put_u8(uint8_t *p, size_t cap, size_t *off, uint16_t type, uint8_t v)
{
    return ctrl_put_tlv(p, cap, off, type, &v, 1);
}

static int queue_control(struct v2_app *a, uint8_t kind, uint32_t req_id,
                         uint64_t service_id, uint64_t token,
                         const uint8_t *tlv, uint16_t tlv_len)
{
    struct lmux2_frame fr;
    if (tlv_len > LMUX2_MAX_PAYLOAD - 24)
        return -1;
    memset(&fr, 0, sizeof(fr));
    fr.type = LMUX2_T_CONTROL;
    fr.stream_id = 0;
    fr.payload[0] = V2_CTRL_VERSION;
    fr.payload[1] = kind;
    lmux_put_le16(fr.payload + 2, 0);
    lmux_put_le32(fr.payload + 4, req_id);
    lmux2_put_le64(fr.payload + 8, service_id);
    lmux2_put_le64(fr.payload + 16, token);
    if (tlv_len)
        memcpy(fr.payload + 24, tlv, tlv_len);
    fr.payload_len = (uint16_t)(24 + tlv_len);
    return lmux2_queue_frame(&a->conn, &fr);
}

static int queue_factory_report(struct v2_app *a)
{
    uint8_t tlv[24];
    size_t off = 0;
    if (ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE,
                    V2_SERVICE_UNIX_RENDEZVOUS) < 0 ||
        ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE,
                    V2_SERVICE_PTY) < 0 ||
        ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE,
                    V2_SERVICE_FILE) < 0)
        return -1;
    return queue_control(a, V2_CTRL_FACTORY_REPORT, 0, 0, 0, tlv, (uint16_t)off);
}

static int queue_service_ok(struct v2_app *a, uint32_t req_id, const struct v2_service *s)
{
    uint8_t tlv[160];
    size_t off = 0;
    if (ctrl_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_NAME, s->name,
                     (uint16_t)(strlen(s->name) + 1)) < 0 ||
        ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE, s->type) < 0)
        return -1;
    if (s->target_path[0] &&
        ctrl_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_TARGET_PATH, s->target_path,
                     (uint16_t)(strlen(s->target_path) + 1)) < 0)
        return -1;
    if (s->argv[0] &&
        (ctrl_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_ARGV, s->argv,
                      (uint16_t)(strlen(s->argv) + 1)) < 0 ||
         ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SUPERVISED,
                     s->supervised ? 1 : 0) < 0))
        return -1;
    return queue_control(a, V2_CTRL_SERVICE_OK, req_id, s->id, 0, tlv, (uint16_t)off);
}

static int queue_service_error(struct v2_app *a, uint32_t req_id, uint64_t service_id,
                               uint8_t code)
{
    uint8_t tlv[8];
    size_t off = 0;
    (void)ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_ERROR_CODE, code);
    return queue_control(a, V2_CTRL_SERVICE_ERROR, req_id, service_id, 0, tlv, (uint16_t)off);
}

static int queue_service_event(struct v2_app *a, const struct v2_service *s,
                               uint8_t event, uint64_t token)
{
    uint8_t tlv[8];
    size_t off = 0;
    (void)ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_EVENT, event);
    return queue_control(a, V2_CTRL_SERVICE_EVENT, 0, s->id, token, tlv, (uint16_t)off);
}

static int queue_winsize_control(struct v2_app *a, struct v2_service *s,
                                 unsigned rows, unsigned cols,
                                 unsigned xp, unsigned yp)
{
    uint8_t tlv[16], ws[8];
    size_t off = 0;
    if (!s || rows == 0 || cols == 0 || rows > 1000 || cols > 1000)
        return -1;
    lmux_put_le16(ws, (uint16_t)rows);
    lmux_put_le16(ws + 2, (uint16_t)cols);
    lmux_put_le16(ws + 4, (uint16_t)xp);
    lmux_put_le16(ws + 6, (uint16_t)yp);
    if (ctrl_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_WINSIZE, ws, sizeof(ws)) < 0)
        return -1;
    return queue_control(a, V2_CTRL_SERVICE_CONTROL, ++a->next_req_id,
                         s->id, 0, tlv, (uint16_t)off);
}

struct ctrl_view {
    uint8_t kind;
    uint32_t req_id;
    uint64_t service_id;
    uint64_t token;
    const uint8_t *tlv;
    size_t tlv_len;
};

static int parse_control(const struct lmux2_frame *fr, struct ctrl_view *cv)
{
    if (fr->type != LMUX2_T_CONTROL || fr->stream_id != 0 ||
        fr->payload_len < 24 || fr->payload[0] != V2_CTRL_VERSION ||
        lmux_get_le16(fr->payload + 2) != 0)
        return -1;
    cv->kind = fr->payload[1];
    cv->req_id = lmux_get_le32(fr->payload + 4);
    cv->service_id = lmux2_get_le64(fr->payload + 8);
    cv->token = lmux2_get_le64(fr->payload + 16);
    cv->tlv = fr->payload + 24;
    cv->tlv_len = fr->payload_len - 24;
    return 0;
}

static int tlv_find(const struct ctrl_view *cv, uint16_t type,
                    const uint8_t **data, uint16_t *len)
{
    size_t off = 0;
    while (off + 4 <= cv->tlv_len) {
        uint16_t t = lmux_get_le16(cv->tlv + off);
        uint16_t l = lmux_get_le16(cv->tlv + off + 2);
        off += 4;
        if (off + l > cv->tlv_len)
            return -1;
        if (t == type) {
            *data = cv->tlv + off;
            *len = l;
            return 1;
        }
        off += l;
    }
    return off == cv->tlv_len ? 0 : -1;
}

static int tlv_string(const struct ctrl_view *cv, uint16_t type, char *out, size_t cap)
{
    const uint8_t *p;
    uint16_t len;
    int r = tlv_find(cv, type, &p, &len);
    if (r <= 0 || len == 0 || len >= cap || memchr(p, 0, len) == NULL)
        return -1;
    memcpy(out, p, len);
    out[len - 1] = 0;
    return 0;
}

static int tlv_u8(const struct ctrl_view *cv, uint16_t type, uint8_t *out)
{
    const uint8_t *p;
    uint16_t len;
    int r = tlv_find(cv, type, &p, &len);
    if (r <= 0 || len != 1)
        return -1;
    *out = p[0];
    return 0;
}

static int tlv_bytes(const struct ctrl_view *cv, uint16_t type,
                     const uint8_t **out, uint16_t *len)
{
    int r = tlv_find(cv, type, out, len);
    return r > 0 ? 0 : -1;
}

static int ctrl_has_service_type(const struct ctrl_view *cv, uint8_t want)
{
    size_t off = 0;
    while (off + 4 <= cv->tlv_len) {
        uint16_t t = lmux_get_le16(cv->tlv + off);
        uint16_t l = lmux_get_le16(cv->tlv + off + 2);
        off += 4;
        if (off + l > cv->tlv_len)
            return 0;
        if (t == V2_TLV_SERVICE_TYPE && l == 1 && cv->tlv[off] == want)
            return 1;
        off += l;
    }
    return 0;
}

static int send_service_ensure(struct v2_app *a, struct v2_desired *d)
{
    uint8_t tlv[256];
    size_t off = 0;
    uint8_t type = (uint8_t)d->type;
    d->req_id = ++a->next_req_id;
    if (d->req_id == 0)
        d->req_id = ++a->next_req_id;
    if (ctrl_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_NAME, d->name,
                     (uint16_t)(strlen(d->name) + 1)) < 0 ||
        ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SERVICE_TYPE, type) < 0)
        return -1;
    if (d->type == V2_SERVICE_UNIX_RENDEZVOUS) {
        if (ctrl_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_TARGET_PATH, d->target_path,
                         (uint16_t)(strlen(d->target_path) + 1)) < 0)
            return -1;
    } else if (d->type == V2_SERVICE_PTY) {
        if (ctrl_put_tlv(tlv, sizeof(tlv), &off, V2_TLV_ARGV, d->argv,
                         (uint16_t)(strlen(d->argv) + 1)) < 0 ||
            ctrl_put_u8(tlv, sizeof(tlv), &off, V2_TLV_SUPERVISED,
                        d->supervised ? 1 : 0) < 0)
            return -1;
    } else if (d->type == V2_SERVICE_FILE) {
        /* No target-side configuration for the current file factory. */
    } else {
        return -1;
    }
    d->sent = 1;
    return queue_control(a, V2_CTRL_SERVICE_ENSURE, d->req_id, 0, 0,
                         tlv, (uint16_t)off);
}

static int create_target_unix_service(struct v2_app *a, uint32_t req_id,
                                      const char *name, const char *path,
                                      enum v2_service_type type)
{
    struct v2_service *s = find_service_name(a, name);
    if (type != V2_SERVICE_UNIX_RENDEZVOUS)
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    if (s) {
        if (s->type != type || strcmp(s->target_path, path) != 0)
            return queue_service_error(a, req_id, s->id, V2_E_CONFLICT);
        return queue_service_ok(a, req_id, s);
    }
    s = alloc_service(a);
    if (!s)
        return queue_service_error(a, req_id, 0, V2_E_LIMIT);
    s->type = type;
    if (copy_bounded(s->name, sizeof(s->name), name) < 0 ||
        copy_bounded(s->target_path, sizeof(s->target_path), path) < 0) {
        cleanup_service(s);
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    }
    s->listen_fd = create_listener(s->target_path, &s->created_path);
    if (s->listen_fd < 0) {
        cleanup_service(s);
        return queue_service_error(a, req_id, 0, V2_E_IO);
    }
    copy_bounded(s->listen_path, sizeof(s->listen_path), s->target_path);
    log_msg(a, 1, "lmux2 service %s id=%llu target listener %s", s->name,
            (unsigned long long)s->id, s->target_path);
    return queue_service_ok(a, req_id, s);
}

static int create_target_pty_service(struct v2_app *a, uint32_t req_id,
                                     const char *name, const char *argv,
                                     int supervised, enum v2_service_type type)
{
    struct v2_service *s = find_service_name(a, name);
    if (type != V2_SERVICE_PTY || !supervised)
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    if (s) {
        if (s->type != type || strcmp(s->argv, argv) != 0 ||
            s->supervised != supervised)
            return queue_service_error(a, req_id, s->id, V2_E_CONFLICT);
        return queue_service_ok(a, req_id, s);
    }
    s = alloc_service(a);
    if (!s)
        return queue_service_error(a, req_id, 0, V2_E_LIMIT);
    s->type = type;
    s->supervised = supervised;
    if (copy_bounded(s->name, sizeof(s->name), name) < 0 ||
        copy_bounded(s->argv, sizeof(s->argv), argv) < 0) {
        cleanup_service(s);
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    }
    if (pty_create_master(s) < 0) {
        cleanup_service(s);
        return queue_service_error(a, req_id, 0, V2_E_IO);
    }
    pty_spawn_child(a, s);
    log_msg(a, 1, "lmux2 pty service %s id=%llu argv=%s slave=%s",
            s->name, (unsigned long long)s->id, s->argv, s->pty_slave_path);
    return queue_service_ok(a, req_id, s);
}

static int create_target_file_service(struct v2_app *a, uint32_t req_id,
                                      const char *name, enum v2_service_type type)
{
    struct v2_service *s = find_service_name(a, name);
    if (type != V2_SERVICE_FILE)
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    if (s) {
        if (s->type != type)
            return queue_service_error(a, req_id, s->id, V2_E_CONFLICT);
        return queue_service_ok(a, req_id, s);
    }
    s = alloc_service(a);
    if (!s)
        return queue_service_error(a, req_id, 0, V2_E_LIMIT);
    s->type = type;
    s->file.fd = -1;
    if (copy_bounded(s->name, sizeof(s->name), name) < 0) {
        cleanup_service(s);
        return queue_service_error(a, req_id, 0, V2_E_BAD_REQUEST);
    }
    log_msg(a, 1, "lmux2 file service %s id=%llu",
            s->name, (unsigned long long)s->id);
    return queue_service_ok(a, req_id, s);
}

static void handle_service_ensure(struct v2_app *a, const struct ctrl_view *cv)
{
    char name[V2_NAME_MAX], path[V2_PATH_MAX], argv[V2_PATH_MAX];
    uint8_t type, supervised = 0;
    if (a->role != LMUX2_ROLE_TARGET ||
        tlv_string(cv, V2_TLV_NAME, name, sizeof(name)) < 0 ||
        tlv_u8(cv, V2_TLV_SERVICE_TYPE, &type) < 0) {
        (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
        return;
    }
    if (type == V2_SERVICE_UNIX_RENDEZVOUS) {
        if (tlv_string(cv, V2_TLV_TARGET_PATH, path, sizeof(path)) < 0) {
            (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
            return;
        }
        (void)create_target_unix_service(a, cv->req_id, name, path,
                                         (enum v2_service_type)type);
    } else if (type == V2_SERVICE_PTY) {
        if (tlv_string(cv, V2_TLV_ARGV, argv, sizeof(argv)) < 0 ||
            tlv_u8(cv, V2_TLV_SUPERVISED, &supervised) < 0) {
            (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
            return;
        }
        (void)create_target_pty_service(a, cv->req_id, name, argv, supervised != 0,
                                        (enum v2_service_type)type);
    } else if (type == V2_SERVICE_FILE) {
        (void)create_target_file_service(a, cv->req_id, name,
                                         (enum v2_service_type)type);
    } else {
        (void)queue_service_error(a, cv->req_id, 0, V2_E_BAD_REQUEST);
    }
}

static void handle_service_ok(struct v2_app *a, const struct ctrl_view *cv)
{
    int i;
    uint8_t type = 0;
    if (a->role != LMUX2_ROLE_HOST)
        return;
    (void)tlv_u8(cv, V2_TLV_SERVICE_TYPE, &type);
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        struct v2_desired *d = &a->desired[i];
        struct v2_service *s;
        if (!d->used || d->req_id != cv->req_id)
            continue;
        d->service_id = cv->service_id;
        s = find_service_id(a, cv->service_id);
        if (!s)
            s = alloc_service(a);
        if (!s)
            return;
        s->id = cv->service_id;
        s->type = type ? (enum v2_service_type)type : d->type;
        copy_bounded(s->name, sizeof(s->name), d->name);
        copy_bounded(s->host_path, sizeof(s->host_path), d->host_path);
        if (d->target_path[0])
            copy_bounded(s->target_path, sizeof(s->target_path), d->target_path);
        if (d->argv[0])
            copy_bounded(s->argv, sizeof(s->argv), d->argv);
        s->supervised = d->supervised;
        if (s->listen_fd < 0) {
            s->listen_fd = create_listener(s->host_path, &s->created_path);
            if (s->listen_fd >= 0)
                copy_bounded(s->listen_path, sizeof(s->listen_path), s->host_path);
        }
        log_msg(a, 1, "lmux2 service %s id=%llu type=%u host listener %s",
                s->name, (unsigned long long)s->id, (unsigned)s->type, s->host_path);
    }
}

static void handle_service_delete(struct v2_app *a, const struct ctrl_view *cv)
{
    struct v2_service *s = find_service_id(a, cv->service_id);
    if (!s)
        return;
    (void)queue_service_event(a, s, V2_EVENT_DELETED, 0);
    cleanup_service(s);
}

static void handle_service_event(struct v2_app *a, const struct ctrl_view *cv)
{
    struct v2_service *s = find_service_id(a, cv->service_id);
    uint8_t event;
    if (a->role != LMUX2_ROLE_HOST || !s || tlv_u8(cv, V2_TLV_EVENT, &event) < 0)
        return;
    if (event == V2_EVENT_PENDING_CLIENT)
        s->pending_event_token = cv->token;
    else if (event == V2_EVENT_PENDING_CLEARED && s->pending_event_token == cv->token)
        s->pending_event_token = 0;
}

static void handle_service_control(struct v2_app *a, const struct ctrl_view *cv)
{
    struct v2_service *s = find_service_id(a, cv->service_id);
    const uint8_t *p;
    uint16_t len;
    struct winsize ws;
    if (a->role != LMUX2_ROLE_TARGET || !s || s->type != V2_SERVICE_PTY)
        return;
    if (tlv_bytes(cv, V2_TLV_WINSIZE, &p, &len) < 0 || len != 8)
        return;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = lmux_get_le16(p);
    ws.ws_col = lmux_get_le16(p + 2);
    ws.ws_xpixel = lmux_get_le16(p + 4);
    ws.ws_ypixel = lmux_get_le16(p + 6);
    if (ws.ws_row == 0 || ws.ws_col == 0 || ws.ws_row > 1000 || ws.ws_col > 1000)
        return;
    if (s->pty_master_fd >= 0)
        (void)ioctl(s->pty_master_fd, TIOCSWINSZ, &ws);
}

static void handle_control(struct v2_app *a, const struct lmux2_frame *fr)
{
    struct ctrl_view cv;
    if (parse_control(fr, &cv) < 0)
        return;
    switch (cv.kind) {
    case V2_CTRL_FACTORY_REPORT:
        if (a->role == LMUX2_ROLE_HOST && !a->desired_sent) {
            int i;
            a->factory_unix = ctrl_has_service_type(&cv, V2_SERVICE_UNIX_RENDEZVOUS);
            a->factory_pty = ctrl_has_service_type(&cv, V2_SERVICE_PTY);
            a->factory_file = ctrl_has_service_type(&cv, V2_SERVICE_FILE);
            for (i = 0; i < V2_MAX_DESIRED; i++)
                if (a->desired[i].used &&
                    ((a->desired[i].type == V2_SERVICE_UNIX_RENDEZVOUS && a->factory_unix) ||
                     (a->desired[i].type == V2_SERVICE_PTY && a->factory_pty) ||
                     (a->desired[i].type == V2_SERVICE_FILE && a->factory_file)))
                    (void)send_service_ensure(a, &a->desired[i]);
            a->desired_sent = 1;
        }
        break;
    case V2_CTRL_SERVICE_ENSURE:
        handle_service_ensure(a, &cv);
        break;
    case V2_CTRL_SERVICE_OK:
        handle_service_ok(a, &cv);
        break;
    case V2_CTRL_SERVICE_ERROR:
        {
            uint8_t code = 0;
            (void)tlv_u8(&cv, V2_TLV_ERROR_CODE, &code);
            log_msg(a, 1, "lmux2 SERVICE_ERROR req=%u service=%llu code=%u",
                    cv.req_id, (unsigned long long)cv.service_id, code);
        }
        break;
    case V2_CTRL_SERVICE_DELETE:
        handle_service_delete(a, &cv);
        break;
    case V2_CTRL_SERVICE_EVENT:
        handle_service_event(a, &cv);
        break;
    case V2_CTRL_SERVICE_CONTROL:
        handle_service_control(a, &cv);
        break;
    default:
        break;
    }
}

static void bind_fd_to_stream(struct v2_service *s, uint64_t stream_id, int fd)
{
    struct v2_binding *b = alloc_binding(s);
    if (!b) {
        close(fd);
        return;
    }
    b->stream_id = stream_id;
    b->fd = fd;
}

static int stream_can_send_endpoint_data(const struct v2_app *a, uint64_t stream_id)
{
    const struct lmux2_stream *st = lmux2_find_stream_const(&a->conn, stream_id);
    if (!st)
        return 0;
    return st->state == LMUX2_STREAM_OPEN || st->state == LMUX2_STREAM_REMOTE_CLOSED;
}

static void close_binding(struct v2_app *a, struct v2_service *s, struct v2_binding *b,
                          int send_close)
{
    if (b->fd >= 0)
        close_fd(&b->fd);
    if (send_close)
        (void)lmux2_close_stream(&a->conn, b->stream_id);
    b->stream_id = 0;
    (void)s;
}

static void clear_pending(struct v2_app *a, struct v2_service *s)
{
    uint64_t token = s->pending_token;
    close_fd(&s->pending_fd);
    s->pending_token = 0;
    if (token)
        (void)queue_service_event(a, s, V2_EVENT_PENDING_CLEARED, token);
}

static void control_handle_line(struct v2_app *a, char *line)
{
    char cmd[16], name[V2_NAME_MAX], service[V2_NAME_MAX];
    unsigned rows, cols, xp = 0, yp = 0;
    if (sscanf(line, "%15s", cmd) != 1)
        return;
    if (strcmp(cmd, "WINSIZE") == 0) {
        struct v2_service *s;
        if (sscanf(line, "%*s %31s %u %u %u %u", name, &rows, &cols, &xp, &yp) < 3) {
            (void)write(a->control_fd, "ERR bad WINSIZE\n", 16);
            return;
        }
        if (strcmp(name, "1") == 0)
            strcpy(service, "console1");
        else if (strcmp(name, "2") == 0)
            strcpy(service, "console2");
        else
            strcpy(service, name);
        s = find_service_name(a, service);
        if (!s || s->type != V2_SERVICE_PTY ||
            queue_winsize_control(a, s, rows, cols, xp, yp) < 0)
            (void)write(a->control_fd, "ERR WINSIZE unavailable\n", 23);
        else
            (void)write(a->control_fd, "OK\n", 3);
    } else {
        (void)write(a->control_fd, "ERR unknown command\n", 20);
    }
}

static void accept_control_client(struct v2_app *a)
{
    int fd = accept(a->control_listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    (void)set_nonblock(fd);
    close_fd(&a->control_fd);
    a->control_fd = fd;
    a->control_in_len = 0;
}

static void service_control_client(struct v2_app *a)
{
    char tmp[128];
    ssize_t r;
    if (a->control_fd < 0)
        return;
    r = read(a->control_fd, tmp, sizeof(tmp));
    if (r <= 0) {
        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            close_fd(&a->control_fd);
        return;
    }
    for (ssize_t i = 0; i < r; i++) {
        char ch = tmp[i];
        if (ch == '\n') {
            a->control_in[a->control_in_len] = 0;
            control_handle_line(a, a->control_in);
            a->control_in_len = 0;
        } else if (a->control_in_len + 1 < sizeof(a->control_in)) {
            a->control_in[a->control_in_len++] = ch;
        } else {
            a->control_in_len = 0;
        }
    }
}

static void accept_service_client(struct v2_app *a, struct v2_service *s)
{
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);
        uint64_t stream_id;
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            return;
        }
        if (set_nonblock(fd) < 0) {
            close(fd);
            continue;
        }
        if (a->role == LMUX2_ROLE_HOST) {
            uint64_t token = s->pending_event_token;
            if (lmux2_open_stream_ex(&a->conn, s->id, token, &stream_id) < 0) {
                close(fd);
                continue;
            }
            s->pending_event_token = 0;
            bind_fd_to_stream(s, stream_id, fd);
            log_msg(a, 1, "lmux2 host local client service=%s stream=%llu token=%llu",
                    s->name, (unsigned long long)stream_id, (unsigned long long)token);
        } else {
            if (s->pending_fd >= 0) {
                close(fd);
                continue;
            }
            s->pending_fd = fd;
            s->pending_token = s->next_token++;
            if (s->next_token == 0)
                s->next_token = 1;
            (void)queue_service_event(a, s, V2_EVENT_PENDING_CLIENT, s->pending_token);
            log_msg(a, 1, "lmux2 target pending client service=%s token=%llu",
                    s->name, (unsigned long long)s->pending_token);
        }
    }
}

static void service_handle_open_received(struct v2_app *a, struct lmux2_stream *st)
{
    struct v2_service *s = find_service_id(a, st->service_id);
    if (!s) {
        (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_REFUSED);
        return;
    }
    if (a->role == LMUX2_ROLE_TARGET && s->type == V2_SERVICE_PTY) {
        if (st->open_token || s->active_stream_id || s->pty_master_fd < 0) {
            (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return;
        }
        if (lmux2_accept_stream(&a->conn, st->id) < 0)
            return;
        s->active_stream_id = st->id;
        log_msg(a, 1, "lmux2 target attached pty stream=%llu service=%s",
                (unsigned long long)st->id, s->name);
        return;
    }
    if (a->role == LMUX2_ROLE_TARGET && s->type == V2_SERVICE_FILE) {
        if (st->open_token || s->active_stream_id) {
            (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return;
        }
        file_session_reset(&s->file);
        if (lmux2_accept_stream(&a->conn, st->id) < 0)
            return;
        s->active_stream_id = st->id;
        log_msg(a, 1, "lmux2 target attached file stream=%llu service=%s",
                (unsigned long long)st->id, s->name);
        return;
    }
    if (a->role == LMUX2_ROLE_TARGET && st->open_token) {
        if (s->pending_fd < 0 || s->pending_token != st->open_token) {
            (void)lmux2_reject_stream(&a->conn, st->id, LMUX2_OPEN_E_BAD_STATE);
            return;
        }
        {
            uint64_t token = s->pending_token;
            bind_fd_to_stream(s, st->id, s->pending_fd);
            s->pending_fd = -1;
            s->pending_token = 0;
            if (token)
                (void)queue_service_event(a, s, V2_EVENT_PENDING_CLEARED, token);
        }
    }
    if (lmux2_accept_stream(&a->conn, st->id) < 0)
        return;
    if (a->role == LMUX2_ROLE_TARGET && !st->open_token)
        log_msg(a, 1, "lmux2 target accepted host-first stream=%llu service=%s",
                (unsigned long long)st->id, s->name);
}

static void process_open_received(struct v2_app *a)
{
    int i;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++)
        if (a->conn.streams[i].state == LMUX2_STREAM_OPEN_RECEIVED)
            service_handle_open_received(a, &a->conn.streams[i]);
}

static void deliver_stream_rx(struct v2_app *a, struct v2_service *s, struct v2_binding *b)
{
    uint8_t buf[256];
    size_t n;
    if (b->fd < 0)
        return;
    while ((n = lmux2_stream_read(&a->conn, b->stream_id, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(b->fd, buf + off, n - off);
            if (w > 0) {
                off += (size_t)w;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            close_binding(a, s, b, 1);
            return;
        }
    }
}

static void collect_endpoint_tx(struct v2_app *a, struct v2_service *s, struct v2_binding *b)
{
    uint8_t buf[256];
    size_t space;
    if (b->fd < 0)
        return;
    if (!stream_can_send_endpoint_data(a, b->stream_id))
        return;
    for (;;) {
        space = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(&a->conn, b->stream_id);
        if (space == 0)
            return;
        if (space > sizeof(buf))
            space = sizeof(buf);
        ssize_t r = read(b->fd, buf, space);
        if (r > 0) {
            size_t w = lmux2_stream_write(&a->conn, b->stream_id, buf, (size_t)r);
            if (w < (size_t)r)
                return;
            continue;
        }
        if (r == 0) {
            close_binding(a, s, b, 1);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        close_binding(a, s, b, 1);
        return;
    }
}

static void deliver_stream_rx_to_pty(struct v2_app *a, struct v2_service *s)
{
    uint8_t buf[256];
    size_t n;
    if (s->pty_master_fd < 0 || !s->active_stream_id)
        return;
    while ((n = lmux2_stream_read(&a->conn, s->active_stream_id, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(s->pty_master_fd, buf + off, n - off);
            if (w > 0) {
                off += (size_t)w;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            (void)lmux2_close_stream(&a->conn, s->active_stream_id);
            s->active_stream_id = 0;
            return;
        }
    }
}

static void collect_pty_tx(struct v2_app *a, struct v2_service *s)
{
    uint8_t buf[256];
    size_t space;
    if (s->pty_master_fd < 0 || !s->active_stream_id)
        return;
    if (!stream_can_send_endpoint_data(a, s->active_stream_id))
        return;
    for (;;) {
        space = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(&a->conn, s->active_stream_id);
        if (space == 0)
            return;
        if (space > sizeof(buf))
            space = sizeof(buf);
        ssize_t r = read(s->pty_master_fd, buf, space);
        if (r > 0) {
            size_t w = lmux2_stream_write(&a->conn, s->active_stream_id, buf, (size_t)r);
            if (w < (size_t)r)
                return;
            continue;
        }
        if (r == 0)
            return;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        (void)lmux2_close_stream(&a->conn, s->active_stream_id);
        s->active_stream_id = 0;
        return;
    }
}

static void close_pty_attachment(struct v2_app *a, struct v2_service *s)
{
    if (s->active_stream_id) {
        (void)lmux2_close_stream(&a->conn, s->active_stream_id);
        s->active_stream_id = 0;
    }
}

static void file_pump_output(struct v2_app *a, struct v2_service *s)
{
    struct v2_file_session *fs = &s->file;
    while (fs->out_pos < fs->out_len &&
           stream_can_send_endpoint_data(a, s->active_stream_id) &&
           lmux2_stream_tx_len(&a->conn, s->active_stream_id) < LMUX2_STREAM_TX_CAP) {
        size_t n = fs->out_len - fs->out_pos;
        size_t space = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(&a->conn, s->active_stream_id);
        size_t w;
        if (n > space)
            n = space;
        w = lmux2_stream_write(&a->conn, s->active_stream_id, fs->outbuf + fs->out_pos, n);
        if (w == 0)
            break;
        fs->out_pos += w;
    }
    if (fs->out_pos == fs->out_len)
        fs->out_pos = fs->out_len = 0;
}

static int file_service_step(struct v2_app *a, struct v2_service *s)
{
    struct v2_file_session *fs = &s->file;
    int progress = 0;
    if (!s->active_stream_id)
        return 0;
    if (fs->out_pos < fs->out_len)
        progress = 1;
    file_pump_output(a, s);
    if (fs->out_len != fs->out_pos)
        return progress;

    if (fs->state == V2_FILE_GET) {
        uint8_t p[LXF_MAX_PAYLOAD];
        ssize_t r = read(fs->fd, p, sizeof(p));
        if (r > 0) {
            sha256_update(&fs->sha, p, (size_t)r);
            fs->count += (uint64_t)r;
            if (file_queue_record(fs, LXF_T_DATA, p, (uint32_t)r) == 0)
                file_pump_output(a, s);
            return 1;
        } else if (r == 0) {
            uint8_t digest[32], done[40];
            sha256_final(&fs->sha, digest);
            close(fs->fd);
            fs->fd = -1;
            lmux_put_le64(done, fs->count);
            memcpy(done + 8, digest, 32);
            fs->state = V2_FILE_IDLE;
            if (file_queue_record(fs, LXF_T_DONE, done, sizeof(done)) == 0)
                file_pump_output(a, s);
            log_msg(a, 1, "lmux2 file service %s GET DONE bytes=%llu",
                    s->name, (unsigned long long)fs->count);
            return 1;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            file_send_error(fs, LXF_E_IO, strerror(errno));
            file_pump_output(a, s);
            return 1;
        }
    }

    while (fs->out_len == fs->out_pos) {
        size_t n, want;

        if (fs->need_len == 0 && fs->in_len >= LXF_HEADER_LEN) {
            uint8_t type, flags;
            uint32_t len;
            if (lxf_decode_header(fs->inbuf, fs->in_len, &type, &flags, &len) < 0) {
                (void)type; (void)flags;
                file_send_error(fs, LXF_E_PROTOCOL, "bad record header");
                fs->in_len = fs->need_len = 0;
                progress = 1;
                break;
            }
            fs->need_len = LXF_HEADER_LEN + len;
        }

        want = fs->need_len ? fs->need_len : LXF_HEADER_LEN;
        if (fs->in_len < want) {
            if (lmux2_stream_rx_len(&a->conn, s->active_stream_id) == 0)
                break;
            n = want - fs->in_len;
            if (fs->in_len + n > sizeof(fs->inbuf)) {
                file_send_error(fs, LXF_E_PROTOCOL, "record too large");
                progress = 1;
                break;
            }
            n = lmux2_stream_read(&a->conn, s->active_stream_id,
                                  fs->inbuf + fs->in_len, n);
            if (n == 0)
                break;
            fs->in_len += n;
            progress = 1;
            continue;
        }

        if (fs->need_len && fs->in_len >= fs->need_len) {
            uint8_t type, flags;
            uint32_t len;
            if (lxf_decode_header(fs->inbuf, fs->in_len, &type, &flags, &len) < 0) {
                (void)flags;
                file_send_error(fs, LXF_E_PROTOCOL, "bad record header");
                fs->in_len = fs->need_len = 0;
                progress = 1;
                break;
            }
            (void)flags;
            file_handle_record(a, s, type, fs->inbuf + LXF_HEADER_LEN, len);
            fs->in_len = 0;
            fs->need_len = 0;
            file_pump_output(a, s);
            progress = 1;
        } else {
            break;
        }
    }
    return progress;
}

static void pair_unbound_target_streams(struct v2_app *a)
{
    int i;
    if (a->role != LMUX2_ROLE_TARGET)
        return;
    for (i = 0; i < LMUX2_CORE_MAX_STREAMS; i++) {
        struct lmux2_stream *st = &a->conn.streams[i];
        struct v2_service *s;
        if (st->state != LMUX2_STREAM_OPEN || st->open_token)
            continue;
        if (find_binding(a, st->id, NULL))
            continue;
        s = find_service_id(a, st->service_id);
        if (s && s->type == V2_SERVICE_UNIX_RENDEZVOUS && s->pending_fd >= 0) {
            uint64_t token = s->pending_token;
            bind_fd_to_stream(s, st->id, s->pending_fd);
            s->pending_fd = -1;
            s->pending_token = 0;
            if (token)
                (void)queue_service_event(a, s, V2_EVENT_PENDING_CLEARED, token);
        }
    }
}

static int service_has_active_binding(const struct v2_service *s)
{
    int i;
    for (i = 0; i < V2_MAX_BINDINGS; i++)
        if (s->bindings[i].fd >= 0)
            return 1;
    return 0;
}

static void cleanup_terminal_bindings(struct v2_app *a)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
        if (!s->used)
            continue;
        for (j = 0; j < V2_MAX_BINDINGS; j++) {
            struct v2_binding *b = &s->bindings[j];
            const struct lmux2_stream *st;
            if (b->fd < 0)
                continue;
            st = lmux2_find_stream_const(&a->conn, b->stream_id);
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED)
                close_binding(a, s, b, st && st->state == LMUX2_STREAM_REMOTE_CLOSED);
        }
        if (s->type == V2_SERVICE_PTY && s->active_stream_id) {
            const struct lmux2_stream *st = lmux2_find_stream_const(&a->conn, s->active_stream_id);
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED) {
                if (st && st->state == LMUX2_STREAM_REMOTE_CLOSED)
                    (void)lmux2_close_stream(&a->conn, s->active_stream_id);
                s->active_stream_id = 0;
            }
        }
        if (s->type == V2_SERVICE_FILE && s->active_stream_id) {
            const struct lmux2_stream *st = lmux2_find_stream_const(&a->conn, s->active_stream_id);
            if (!st || st->state == LMUX2_STREAM_TERMINAL ||
                st->state == LMUX2_STREAM_RESET ||
                st->state == LMUX2_STREAM_REMOTE_CLOSED) {
                if (st && st->state == LMUX2_STREAM_REMOTE_CLOSED)
                    (void)lmux2_close_stream(&a->conn, s->active_stream_id);
                s->active_stream_id = 0;
                file_session_reset(&s->file);
            }
        }
    }
}

static void reset_connection_streams(struct v2_app *a)
{
    int i, j;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
        if (!s->used)
            continue;
        for (j = 0; j < V2_MAX_BINDINGS; j++)
            close_binding(a, s, &s->bindings[j], 0);
        s->active_stream_id = 0;
        if (s->type == V2_SERVICE_FILE)
            file_session_reset(&s->file);
    }
    lmux2_conn_begin_physical(&a->conn, make_conn_id());
    a->factories_reported = 0;
    a->desired_sent = 0;
    a->factory_unix = 0;
    a->factory_pty = 0;
    a->factory_file = 0;
    for (i = 0; i < V2_MAX_DESIRED; i++) {
        a->desired[i].sent = 0;
        a->desired[i].req_id = 0;
        a->desired[i].service_id = 0;
    }
    (void)lmux2_conn_start(&a->conn);
}

static void supervise_pty_services(struct v2_app *a)
{
    int i;
    long long t = now_ms();
    for (;;) {
        int st;
        pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid <= 0)
            break;
        for (i = 0; i < V2_MAX_SERVICES; i++) {
            struct v2_service *s = &a->services[i];
            if (s->used && s->type == V2_SERVICE_PTY && s->child_pid == pid) {
                s->child_pid = 0;
                s->respawn_at_ms = now_ms() + 1000;
                pty_restore_baseline(s);
                log_msg(a, 1, "lmux2 pty service %s child exited", s->name);
            }
        }
    }
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
        if (s->used && s->type == V2_SERVICE_PTY && s->supervised &&
            s->child_pid <= 0 && (s->respawn_at_ms == 0 || t >= s->respawn_at_ms))
            pty_spawn_child(a, s);
    }
}

static int step_file_services(struct v2_app *a)
{
    int i, progress = 0;
    if (a->role != LMUX2_ROLE_TARGET)
        return 0;
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &a->services[i];
        if (s->used && s->type == V2_SERVICE_FILE && s->active_stream_id)
            progress |= file_service_step(a, s);
    }
    return progress;
}

static void process_frame(struct v2_app *a, const struct lmux2_frame *fr)
{
    enum lmux2_conn_state before = a->conn.state;
    if (fr->type == LMUX2_T_CONTROL)
        handle_control(a, fr);
    (void)lmux2_handle_frame(&a->conn, fr);
    cleanup_terminal_bindings(a);
    if (a->conn.state == LMUX2_CONN_ESTABLISHED &&
        before != LMUX2_CONN_ESTABLISHED && a->role == LMUX2_ROLE_TARGET &&
        !a->factories_reported) {
        (void)queue_factory_report(a);
        a->factories_reported = 1;
    }
    process_open_received(a);
}

static void feed_carrier(struct v2_app *a)
{
    uint8_t buf[256];
    for (;;) {
        ssize_t r = read(a->carrier_fd, buf, sizeof(buf));
        ssize_t i;
        if (r > 0) {
            for (i = 0; i < r; i++) {
                if (buf[i] == 0) {
                    if (!a->discard_until_delim && a->enc_in_len) {
                        struct lmux2_frame fr;
                        if (lmux2_decode_frame(a->enc_in, a->enc_in_len, &fr) == 0)
                            process_frame(a, &fr);
                        else
                            a->conn.state = LMUX2_CONN_ERROR;
                    }
                    a->enc_in_len = 0;
                    a->discard_until_delim = 0;
                } else if (!a->discard_until_delim) {
                    if (a->enc_in_len >= sizeof(a->enc_in)) {
                        a->discard_until_delim = 1;
                        a->enc_in_len = 0;
                        a->conn.state = LMUX2_CONN_ERROR;
                    } else {
                        a->enc_in[a->enc_in_len++] = buf[i];
                    }
                }
            }
            continue;
        }
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            carrier_lost(a);
            return;
        }
        return;
    }
}

static void build_next_output(struct v2_app *a)
{
    struct lmux2_frame fr;
    if (a->out_len != a->out_pos)
        return;
    a->out_len = a->out_pos = 0;
    if (lmux2_next_frame(&a->conn, &fr) > 0)
        (void)lmux2_encode_frame(&fr, a->out, sizeof(a->out), &a->out_len);
}

static void flush_carrier(struct v2_app *a)
{
    while (a->out_pos < a->out_len) {
        ssize_t w = write(a->carrier_fd, a->out + a->out_pos, a->out_len - a->out_pos);
        if (w > 0) {
            a->out_pos += (size_t)w;
            if (a->out_pos == a->out_len)
                a->out_pos = a->out_len = 0;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        carrier_lost(a);
        return;
    }
}

static int add_poll(struct pollfd *pfds, int *kind, int *svc, int *bind,
                    nfds_t *n, int fd, short events, int k, int s, int b)
{
    if (*n >= 64 || fd < 0)
        return -1;
    pfds[*n].fd = fd;
    pfds[*n].events = events;
    pfds[*n].revents = 0;
    kind[*n] = k;
    svc[*n] = s;
    bind[*n] = b;
    (*n)++;
    return 0;
}

static void run_loop(struct v2_app *a)
{
    while (!a->stop) {
        struct pollfd pfds[64];
        int kind[64], svc[64], bind[64];
        nfds_t n = 0, i;
        (void)step_file_services(a);
        build_next_output(a);
        if (a->carrier_fd >= 0) {
            short ev = POLLIN;
            if (a->out_pos < a->out_len)
                ev |= POLLOUT;
            add_poll(pfds, kind, svc, bind, &n, a->carrier_fd, ev, 1, -1, -1);
        }
        if (a->carrier_listen_fd >= 0 && a->carrier_fd < 0)
            add_poll(pfds, kind, svc, bind, &n, a->carrier_listen_fd, POLLIN, 8, -1, -1);
        if (a->control_listen_fd >= 0)
            add_poll(pfds, kind, svc, bind, &n, a->control_listen_fd, POLLIN, 6, -1, -1);
        if (a->control_fd >= 0)
            add_poll(pfds, kind, svc, bind, &n, a->control_fd, POLLIN, 7, -1, -1);
        for (i = 0; i < V2_MAX_SERVICES; i++) {
            int j;
            struct v2_service *s = &a->services[i];
            if (!s->used)
                continue;
            if (s->listen_fd >= 0 &&
                !(a->role == LMUX2_ROLE_HOST &&
                  (s->type == V2_SERVICE_PTY || s->type == V2_SERVICE_FILE) &&
                  service_has_active_binding(s)))
                add_poll(pfds, kind, svc, bind, &n, s->listen_fd, POLLIN, 2, (int)i, -1);
            if (s->pending_fd >= 0)
                add_poll(pfds, kind, svc, bind, &n, s->pending_fd, 0, 3, (int)i, -1);
            for (j = 0; j < V2_MAX_BINDINGS; j++) {
                struct v2_binding *b = &s->bindings[j];
                short ev = 0;
                if (b->fd < 0)
                    continue;
                if (stream_can_send_endpoint_data(a, b->stream_id) &&
                    lmux2_stream_tx_len(&a->conn, b->stream_id) < LMUX2_STREAM_TX_CAP)
                    ev |= POLLIN;
                if (lmux2_stream_rx_len(&a->conn, b->stream_id) > 0)
                    ev |= POLLOUT;
                add_poll(pfds, kind, svc, bind, &n, b->fd, ev, 4, (int)i, j);
            }
            if (a->role == LMUX2_ROLE_TARGET && s->type == V2_SERVICE_PTY &&
                s->pty_master_fd >= 0 && s->active_stream_id) {
                short ev = 0;
                if (stream_can_send_endpoint_data(a, s->active_stream_id) &&
                    lmux2_stream_tx_len(&a->conn, s->active_stream_id) < LMUX2_STREAM_TX_CAP)
                    ev |= POLLIN;
                if (lmux2_stream_rx_len(&a->conn, s->active_stream_id) > 0)
                    ev |= POLLOUT;
                add_poll(pfds, kind, svc, bind, &n, s->pty_master_fd, ev, 5, (int)i, -1);
            }
        }
        if (poll(pfds, n, 1000) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (i = 0; i < n; i++) {
            if (!pfds[i].revents)
                continue;
            if (kind[i] == 1) {
                if (pfds[i].revents & POLLIN)
                    feed_carrier(a);
                if (pfds[i].revents & POLLOUT)
                    flush_carrier(a);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    carrier_lost(a);
            } else if (kind[i] == 2) {
                accept_service_client(a, &a->services[svc[i]]);
            } else if (kind[i] == 3) {
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    clear_pending(a, &a->services[svc[i]]);
            } else if (kind[i] == 4) {
                struct v2_service *s = &a->services[svc[i]];
                struct v2_binding *b = &s->bindings[bind[i]];
                if (pfds[i].revents & POLLOUT)
                    deliver_stream_rx(a, s, b);
                if (pfds[i].revents & POLLIN)
                    collect_endpoint_tx(a, s, b);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    close_binding(a, s, b, 1);
            } else if (kind[i] == 5) {
                struct v2_service *s = &a->services[svc[i]];
                if (pfds[i].revents & POLLOUT)
                    deliver_stream_rx_to_pty(a, s);
                if (pfds[i].revents & POLLIN)
                    collect_pty_tx(a, s);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    close_pty_attachment(a, s);
            } else if (kind[i] == 6) {
                if (pfds[i].revents & POLLIN)
                    accept_control_client(a);
            } else if (kind[i] == 7) {
                if (pfds[i].revents & POLLIN)
                    service_control_client(a);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    close_fd(&a->control_fd);
            } else if (kind[i] == 8) {
                if (pfds[i].revents & POLLIN)
                    accept_carrier(a);
            }
        }
        pair_unbound_target_streams(a);
        cleanup_terminal_bindings(a);
        (void)step_file_services(a);
        supervise_pty_services(a);
        build_next_output(a);
        flush_carrier(a);
    }
}

static void cleanup_app(struct v2_app *a)
{
    int i;
    for (i = 0; i < V2_MAX_SERVICES; i++)
        if (a->services[i].used)
            cleanup_service(&a->services[i]);
    if (a->carrier_fd_owned)
        close_fd(&a->carrier_fd);
    close_fd(&a->carrier_listen_fd);
    if (a->carrier_created_path && a->carrier_listen_path)
        unlink(a->carrier_listen_path);
    close_fd(&a->control_fd);
    close_fd(&a->control_listen_fd);
    if (a->control_path)
        unlink(a->control_path);
}

static void on_signal(int sig)
{
    (void)sig;
    if (global_v2_app)
        global_v2_app->stop = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
            "lmuxd %s (%s)\n"
            "Usage: lmuxd --role host|target --carrier PATH [--control PATH] [--unix NAME:HOST_SOCKET:TARGET_SOCKET] [--pty NAME:HOST_SOCKET:ARGV] [--file NAME:HOST_SOCKET] [--verbose]\n"
            "       lmuxd --role host|target --carrier-fd FD [--control PATH] [--unix NAME:HOST_SOCKET:TARGET_SOCKET] [--pty NAME:HOST_SOCKET:ARGV] [--file NAME:HOST_SOCKET] [--verbose]\n"
            "       lmuxd --role host|target --carrier-listen PATH|--carrier-connect PATH [test carrier]\n"
            "       lmuxd --help\n"
            "       lmuxd --version\n",
            LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
}

int lmux2_runtime_main(int argc, char **argv)
{
    struct v2_app a;
    int i, have_role = 0;
    memset(&a, 0, sizeof(a));
    a.carrier_fd = -1;
    a.carrier_fd_owned = 1;
    a.carrier_listen_fd = -1;
    a.control_listen_fd = -1;
    a.control_fd = -1;
    a.next_service_id = 1;
    init_service_slots(&a);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--role") == 0) {
            if (++i >= argc || parse_role(argv[i], &a.role) < 0)
                return usage(stderr), 1;
            have_role = 1;
        } else if (strcmp(argv[i], "--carrier") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.carrier_path = argv[i];
        } else if (strcmp(argv[i], "--carrier-fd") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.carrier_fd = atoi(argv[i]);
            a.carrier_fd_owned = 0;
        } else if (strcmp(argv[i], "--carrier-listen") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.carrier_listen_path = argv[i];
        } else if (strcmp(argv[i], "--carrier-connect") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.carrier_connect_path = argv[i];
        } else if (strcmp(argv[i], "--control") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            a.control_path = argv[i];
        } else if (strcmp(argv[i], "--unix") == 0) {
            if (++i >= argc || add_desired_unix(&a, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--pty") == 0) {
            if (++i >= argc || add_desired_pty(&a, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--file") == 0) {
            if (++i >= argc || add_desired_file(&a, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            a.verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("lmuxd %s (%s)\n", LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 1;
        }
    }
    if (!have_role || (a.carrier_fd < 0 && !a.carrier_path &&
                       !a.carrier_listen_path && !a.carrier_connect_path))
        return usage(stderr), 1;
    if (a.carrier_listen_path) {
        a.carrier_listen_fd = create_listener(a.carrier_listen_path, &a.carrier_created_path);
        if (a.carrier_listen_fd < 0) {
            perror(a.carrier_listen_path);
            return 1;
        }
        a.carrier_fd = -1;
    } else if (a.carrier_connect_path) {
        a.carrier_fd = connect_unix_carrier(a.carrier_connect_path);
        if (a.carrier_fd < 0) {
            perror(a.carrier_connect_path);
            return 1;
        }
        a.carrier_fd_owned = 1;
    } else if (a.carrier_fd < 0) {
        a.carrier_fd = open(a.carrier_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (a.carrier_fd < 0) {
            perror(a.carrier_path);
            return 1;
        }
        (void)configure_tty(a.carrier_fd);
    } else {
        (void)set_nonblock(a.carrier_fd);
    }
    if (a.control_path) {
        a.control_listen_fd = create_listener(a.control_path, &(int){0});
        if (a.control_listen_fd < 0) {
            perror(a.control_path);
            cleanup_app(&a);
            return 1;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    global_v2_app = &a;
    lmux2_conn_init(&a.conn, a.role, make_conn_id());
    (void)lmux2_conn_start(&a.conn);
    fprintf(stderr, "lmuxd %s (%s) LMUX/2 role=%s\n", LMUX_BUILD_VERSION,
            LMUX_PROJECT_NAME, a.role == LMUX2_ROLE_HOST ? "host" : "target");
    run_loop(&a);
    cleanup_app(&a);
    return 0;
}
