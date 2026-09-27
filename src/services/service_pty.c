#include "services.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void child_close_daemon_fds(void)
{
    int fd;
    for (fd = 3; fd < 256; fd++)
        close(fd);
}

void v2_pty_restore_baseline(struct v2_service *s)
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

int v2_pty_create_master(struct v2_service *s)
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

void v2_pty_spawn_child(struct v2_app *a, struct v2_service *s)
{
    pid_t pid;
    if (s->type != V2_SERVICE_PTY || !s->supervised ||
        s->child_pid > 0 || s->pty_master_fd < 0)
        return;
    v2_pty_restore_baseline(s);
    pid = fork();
    if (pid < 0) {
        v2_app_log(a, 0, "service %s fork %s failed: %s",
                   s->name, s->argv, strerror(errno));
        s->respawn_at_ms = v2_now_ms() + 1000;
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
    v2_app_log(a, 1, "lmux2 pty service %s spawned %s pid=%ld",
               s->name, s->argv, (long)pid);
}

void v2_pty_terminate_child(struct v2_service *s)
{
    long long deadline;
    if (s->child_pid <= 0)
        return;
    kill(s->child_pid, SIGHUP);
    deadline = v2_now_ms() + 1000;
    while (v2_now_ms() < deadline) {
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

void v2_pty_supervise(struct v2_app *a, struct v2_service services[V2_MAX_SERVICES])
{
    int i;
    long long t = v2_now_ms();
    for (i = 0; i < V2_MAX_SERVICES; i++) {
        struct v2_service *s = &services[i];
        int st;
        pid_t pid;
        if (s->used && s->type == V2_SERVICE_PTY && s->child_pid > 0) {
            pid = waitpid(s->child_pid, &st, WNOHANG);
            if (pid == s->child_pid) {
                s->child_pid = 0;
                s->respawn_at_ms = v2_now_ms() + 1000;
                v2_pty_restore_baseline(s);
                v2_app_log(a, 1, "lmux2 pty service %s child exited", s->name);
            }
        }
        if (s->used && s->type == V2_SERVICE_PTY && s->supervised &&
            s->child_pid <= 0 && (s->respawn_at_ms == 0 || t >= s->respawn_at_ms))
            v2_pty_spawn_child(a, s);
    }
}

void v2_pty_apply_winsize(struct v2_service *s, unsigned rows, unsigned cols,
                          unsigned xp, unsigned yp)
{
    struct winsize ws;
    if (!s || s->type != V2_SERVICE_PTY ||
        rows == 0 || cols == 0 || rows > 1000 || cols > 1000)
        return;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = (unsigned short)xp;
    ws.ws_ypixel = (unsigned short)yp;
    if (s->pty_master_fd >= 0)
        (void)ioctl(s->pty_master_fd, TIOCSWINSZ, &ws);
}

static int pty_stream_can_send(const struct lmux2_conn *conn, uint64_t stream_id)
{
    const struct lmux2_stream *st = lmux2_find_stream_const(conn, stream_id);
    if (!st)
        return 0;
    return st->state == LMUX2_STREAM_OPEN || st->state == LMUX2_STREAM_REMOTE_CLOSED;
}

void v2_pty_deliver_stream_rx(struct lmux2_conn *conn, struct v2_service *s)
{
    uint8_t buf[256];
    size_t n;
    if (s->pty_master_fd < 0 || !s->active_stream_id)
        return;
    while ((n = lmux2_stream_read(conn, s->active_stream_id, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(s->pty_master_fd, buf + off, n - off);
            if (w > 0) {
                off += (size_t)w;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            (void)lmux2_close_stream(conn, s->active_stream_id);
            s->active_stream_id = 0;
            return;
        }
    }
}

void v2_pty_collect_tx(struct lmux2_conn *conn, struct v2_service *s)
{
    uint8_t buf[256];
    size_t space;
    if (s->pty_master_fd < 0 || !s->active_stream_id)
        return;
    if (!pty_stream_can_send(conn, s->active_stream_id))
        return;
    for (;;) {
        space = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(conn, s->active_stream_id);
        if (space == 0)
            return;
        if (space > sizeof(buf))
            space = sizeof(buf);
        ssize_t r = read(s->pty_master_fd, buf, space);
        if (r > 0) {
            size_t w = lmux2_stream_write(conn, s->active_stream_id, buf, (size_t)r);
            if (w < (size_t)r)
                return;
            continue;
        }
        if (r == 0)
            return;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        (void)lmux2_close_stream(conn, s->active_stream_id);
        s->active_stream_id = 0;
        return;
    }
}
