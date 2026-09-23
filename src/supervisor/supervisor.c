#include "supervisor.h"
#include "carrier.h"
#include "link_endpoint.h"
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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum supervisor_state {
    SUP_WORKER_DOWN = 0,
    SUP_LINK_DOWN,
    SUP_DOWN_BARRIER,
    SUP_UP_BARRIER,
    SUP_UP_FLUSH,
    SUP_ACTIVE
};

struct supervisor {
    struct lmux_carrier carrier;
    const char *worker_path;
    int verbose;
    int stop;
    enum supervisor_state state;
    pid_t worker_pid;
    int worker_data_fd;
    int worker_ctrl_fd;
    int sigchld_pipe[2];
    uint64_t link_epoch;
    long long restart_deadline_ms;
    long long restart_delay_ms;
    long long worker_started_ms;
    int shutting_down;
    int outer_enabled;
    int up_flush_worker_eagain;
    struct lmux_link_endpoint outer;
    struct lmux_proxy_ring phys_to_worker;
    struct lmux_proxy_ring worker_to_phys;
};

static struct supervisor *global_sup;

static void sup_log(struct supervisor *s, int verbose, const char *fmt, ...);
static void drain_outer_transport_to_worker_ring(struct supervisor *s);

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t make_outer_conn_id(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
            v = 0;
        close(fd);
    }
    if (!v)
        v = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)time(NULL) ^ 0x4f555431u;
    return v ? v : 1;
}

static void begin_outer(struct supervisor *s)
{
    lmux_link_endpoint_begin(&s->outer, make_outer_conn_id());
    sup_log(s, 1, "outer HELLO sent local_conn=%llu",
            (unsigned long long)s->outer.local_conn_id);
}

static void sup_log(struct supervisor *s, int verbose, const char *fmt, ...)
{
    va_list ap;
    if (verbose && !s->verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_msg(struct supervisor *s, uint8_t type, uint64_t epoch)
{
    uint8_t buf[LMUX_SUP_MSG_LEN];
    struct lmux_supervisor_msg msg;
    ssize_t w;
    msg.type = type;
    msg.link_epoch = epoch;
    if (lmux_supervisor_msg_encode(buf, &msg) < 0)
        return -1;
    w = send(s->worker_ctrl_fd, buf, sizeof(buf), 0);
    if (w == (ssize_t)sizeof(buf))
        return 0;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return -1;
    return -1;
}

static int next_epoch(struct supervisor *s)
{
    if (s->link_epoch == UINT64_MAX)
        return -1;
    s->link_epoch++;
    if (s->link_epoch == 0)
        s->link_epoch++;
    return 0;
}

static void clear_proxy(struct supervisor *s)
{
    lmux_proxy_ring_clear(&s->phys_to_worker);
    lmux_proxy_ring_clear(&s->worker_to_phys);
}

static void close_worker_fds(struct supervisor *s)
{
    close_fd(&s->worker_data_fd);
    close_fd(&s->worker_ctrl_fd);
}

static int create_socketpair(int type, int sv[2])
{
    if (socketpair(AF_UNIX, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) == 0)
        return 0;
    if (errno != EINVAL)
        return -1;
    if (socketpair(AF_UNIX, type, 0, sv) < 0)
        return -1;
    if (set_nonblock(sv[0]) < 0 || set_nonblock(sv[1]) < 0)
        return -1;
    (void)fcntl(sv[0], F_SETFD, fcntl(sv[0], F_GETFD, 0) | FD_CLOEXEC);
    (void)fcntl(sv[1], F_SETFD, fcntl(sv[1], F_GETFD, 0) | FD_CLOEXEC);
    return 0;
}

static int spawn_worker(struct supervisor *s)
{
    int data_pair[2] = {-1, -1};
    int ctrl_pair[2] = {-1, -1};
    pid_t pid;
    if (create_socketpair(SOCK_STREAM, data_pair) < 0 ||
        create_socketpair(SOCK_SEQPACKET, ctrl_pair) < 0)
        goto fail;
    pid = lmux_supervisor_spawn_worker(s->worker_path, data_pair[1],
                                       ctrl_pair[1], s->verbose);
    if (pid < 0)
        goto fail;
    close(data_pair[1]);
    close(ctrl_pair[1]);
    close_worker_fds(s);
    s->worker_pid = pid;
    s->worker_data_fd = data_pair[0];
    s->worker_ctrl_fd = ctrl_pair[0];
    s->worker_started_ms = now_ms();
    s->state = SUP_LINK_DOWN;
    clear_proxy(s);
    sup_log(s, 1, "supervisor worker spawned pid=%ld", (long)pid);
    return 0;
fail:
    if (data_pair[0] >= 0) close(data_pair[0]);
    if (data_pair[1] >= 0) close(data_pair[1]);
    if (ctrl_pair[0] >= 0) close(ctrl_pair[0]);
    if (ctrl_pair[1] >= 0) close(ctrl_pair[1]);
    return -1;
}

static void schedule_restart(struct supervisor *s, long long now)
{
    if (s->shutting_down)
        return;
    close_worker_fds(s);
    s->worker_pid = -1;
    clear_proxy(s);
    s->state = SUP_WORKER_DOWN;
    if (s->restart_delay_ms <= 0)
        s->restart_delay_ms = 250;
    else if (s->restart_delay_ms < 5000)
        s->restart_delay_ms *= 2;
    if (s->restart_delay_ms > 5000)
        s->restart_delay_ms = 5000;
    s->restart_deadline_ms = now + s->restart_delay_ms;
    sup_log(s, 1, "supervisor worker restart scheduled in %lld ms",
            s->restart_delay_ms);
}

static void reap_workers(struct supervisor *s)
{
    int status;
    pid_t pid;
    long long now = now_ms();
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == s->worker_pid) {
            sup_log(s, 1, "supervisor worker exited pid=%ld status=0x%x",
                    (long)pid, status);
            schedule_restart(s, now);
        }
    }
}

static void sigchld_handler(int sig)
{
    int saved = errno;
    uint8_t b = 1;
    (void)sig;
    if (global_sup && global_sup->sigchld_pipe[1] >= 0)
        (void)write(global_sup->sigchld_pipe[1], &b, 1);
    errno = saved;
}

static void stop_handler(int sig)
{
    (void)sig;
    if (global_sup) {
        global_sup->shutting_down = 1;
        global_sup->stop = 1;
    }
}

static void physical_lost(struct supervisor *s)
{
    long long now = now_ms();
    if (lmux_carrier_fd(&s->carrier) < 0)
        return;
    sup_log(s, 1, "supervisor physical carrier lost");
    lmux_carrier_mark_lost(&s->carrier, now);
    if (s->outer_enabled)
        lmux_link_endpoint_down(&s->outer);
    clear_proxy(s);
    if (s->worker_pid > 0 && s->worker_ctrl_fd >= 0 &&
        s->state != SUP_LINK_DOWN && s->state != SUP_DOWN_BARRIER &&
        s->state != SUP_WORKER_DOWN) {
        if (send_msg(s, LMUX_SUP_LINK_DOWN, s->link_epoch) == 0) {
            s->state = SUP_DOWN_BARRIER;
            sup_log(s, 1, "supervisor LINK_DOWN epoch=%llu",
                    (unsigned long long)s->link_epoch);
        } else {
            schedule_restart(s, now);
        }
    } else if (s->state != SUP_WORKER_DOWN) {
        s->state = SUP_LINK_DOWN;
    }
}

static void maybe_send_link_up(struct supervisor *s)
{
    if (s->state != SUP_LINK_DOWN || s->worker_pid <= 0 ||
        s->worker_ctrl_fd < 0 || lmux_carrier_fd(&s->carrier) < 0)
        return;
    if (s->outer_enabled && s->outer.state != LMUX_LINK_ESTABLISHED)
        return;
    if (next_epoch(s) < 0) {
        s->stop = 1;
        return;
    }
    if (send_msg(s, LMUX_SUP_LINK_UP, s->link_epoch) < 0) {
        schedule_restart(s, now_ms());
        return;
    }
    s->state = SUP_UP_BARRIER;
    sup_log(s, 1, "supervisor LINK_UP epoch=%llu",
            (unsigned long long)s->link_epoch);
}

static void handle_ctrl_msg(struct supervisor *s)
{
    uint8_t buf[LMUX_SUP_MSG_LEN];
    ssize_t r;
    struct lmux_supervisor_msg msg;
    for (;;) {
        r = recv(s->worker_ctrl_fd, buf, sizeof(buf), 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (r == 0) {
            sup_log(s, 1, "supervisor worker sideband closed state=%d pid=%ld",
                    s->state, (long)s->worker_pid);
            schedule_restart(s, now_ms());
            return;
        }
        if (r != (ssize_t)sizeof(buf)) {
            sup_log(s, 1, "supervisor malformed worker sideband len=%ld state=%d pid=%ld",
                    (long)r, s->state, (long)s->worker_pid);
            schedule_restart(s, now_ms());
            return;
        }
        if (lmux_supervisor_msg_decode(buf, &msg) < 0) {
            sup_log(s, 1, "supervisor malformed worker sideband payload len=%ld state=%d pid=%ld",
                    (long)r, s->state, (long)s->worker_pid);
            schedule_restart(s, now_ms());
            return;
        }
        if (msg.type == LMUX_SUP_LINK_DOWN_ACK) {
            if (s->state == SUP_DOWN_BARRIER && msg.link_epoch == s->link_epoch) {
                (void)lmux_supervisor_drain_fd(s->worker_data_fd);
                clear_proxy(s);
                s->state = SUP_LINK_DOWN;
                sup_log(s, 1, "supervisor LINK_DOWN_ACK epoch=%llu",
                        (unsigned long long)msg.link_epoch);
                maybe_send_link_up(s);
            } else {
                sup_log(s, 1, "supervisor stale LINK_DOWN_ACK epoch=%llu",
                        (unsigned long long)msg.link_epoch);
            }
        } else if (msg.type == LMUX_SUP_LINK_UP_ACK) {
            if (s->state == SUP_UP_BARRIER && msg.link_epoch == s->link_epoch) {
                s->state = SUP_UP_FLUSH;
                s->up_flush_worker_eagain = 0;
                sup_log(s, 1, "supervisor LINK_UP_ACK epoch=%llu",
                        (unsigned long long)msg.link_epoch);
                sup_log(s, 1, "supervisor UP_FLUSH epoch=%llu",
                        (unsigned long long)msg.link_epoch);
            } else {
                sup_log(s, 1, "supervisor stale LINK_UP_ACK epoch=%llu",
                        (unsigned long long)msg.link_epoch);
            }
        } else {
            sup_log(s, 1, "supervisor unexpected sideband type=%u", msg.type);
        }
    }
}

static void maybe_finish_up_flush(struct supervisor *s)
{
    int empty = lmux_proxy_ring_len(&s->worker_to_phys) == 0;
    if (s->outer_enabled)
        empty = empty &&
            lmux_link_transport_tx_len(&s->outer) == 0 &&
            !lmux_link_has_pending_output(&s->outer);
    if (s->state == SUP_UP_FLUSH && s->up_flush_worker_eagain && empty) {
        s->state = SUP_ACTIVE;
        sup_log(s, 1, "supervisor ACTIVE");
        if (s->outer_enabled)
            drain_outer_transport_to_worker_ring(s);
    }
}

static void outer_session_down(struct supervisor *s)
{
    sup_log(s, 1, "outer session down reason=%s",
            s->outer.last_error[0] ? s->outer.last_error : "-");
    clear_proxy(s);
    if (s->worker_pid > 0 && s->worker_ctrl_fd >= 0 &&
        s->state != SUP_LINK_DOWN && s->state != SUP_DOWN_BARRIER &&
        s->state != SUP_WORKER_DOWN) {
        if (send_msg(s, LMUX_SUP_LINK_DOWN, s->link_epoch) == 0) {
            s->state = SUP_DOWN_BARRIER;
            sup_log(s, 1, "supervisor LINK_DOWN epoch=%llu",
                    (unsigned long long)s->link_epoch);
        } else {
            schedule_restart(s, now_ms());
            return;
        }
    } else if (s->state != SUP_WORKER_DOWN) {
        s->state = SUP_LINK_DOWN;
    }
    if (lmux_carrier_fd(&s->carrier) >= 0)
        begin_outer(s);
}

static void outer_peer_restart(struct supervisor *s)
{
    uint64_t old_local = s->outer.local_conn_id;
    uint64_t old_peer = s->outer.peer_conn_id;
    uint64_t new_peer = s->outer.pending_peer_hello.conn_id;
    uint64_t new_local = make_outer_conn_id();
    unsigned events = 0;

    sup_log(s, 1, "outer peer restart detected old_peer=%llu new_peer=%llu",
            (unsigned long long)old_peer, (unsigned long long)new_peer);
    sup_log(s, 1, "outer old session retired local=%llu peer=%llu",
            (unsigned long long)old_local, (unsigned long long)old_peer);
    clear_proxy(s);
    if (s->worker_pid > 0 && s->worker_ctrl_fd >= 0 &&
        s->state != SUP_LINK_DOWN && s->state != SUP_DOWN_BARRIER &&
        s->state != SUP_WORKER_DOWN) {
        if (send_msg(s, LMUX_SUP_LINK_DOWN, s->link_epoch) == 0) {
            s->state = SUP_DOWN_BARRIER;
            sup_log(s, 1, "supervisor LINK_DOWN epoch=%llu",
                    (unsigned long long)s->link_epoch);
        } else {
            schedule_restart(s, now_ms());
            return;
        }
    } else if (s->state != SUP_WORKER_DOWN) {
        s->state = SUP_LINK_DOWN;
    }
    if (lmux_link_endpoint_restart_from_pending(&s->outer, new_local, &events) < 0) {
        outer_session_down(s);
        return;
    }
    sup_log(s, 1, "outer new incarnation local=%llu",
            (unsigned long long)s->outer.local_conn_id);
    sup_log(s, 1, "outer HELLO sent local=%llu seq=0",
            (unsigned long long)s->outer.local_conn_id);
    sup_log(s, 1, "outer peer HELLO accepted peer=%llu",
            (unsigned long long)s->outer.peer_conn_id);
    if (events & LMUX_LINK_EV_ESTABLISHED) {
        sup_log(s, 1, "outer established local_conn=%llu peer_conn=%llu",
                (unsigned long long)s->outer.local_conn_id,
                (unsigned long long)s->outer.peer_conn_id);
        maybe_send_link_up(s);
    }
}

static void handle_outer_events(struct supervisor *s, unsigned events)
{
    if (events & LMUX_LINK_EV_PEER_RESTART) {
        outer_peer_restart(s);
        return;
    }
    if (events & LMUX_LINK_EV_DOWN) {
        outer_session_down(s);
        return;
    }
    if (events & LMUX_LINK_EV_ESTABLISHED) {
        sup_log(s, 1, "outer peer HELLO accepted peer_conn=%llu",
                (unsigned long long)s->outer.peer_conn_id);
        sup_log(s, 1, "outer established local_conn=%llu peer_conn=%llu",
                (unsigned long long)s->outer.local_conn_id,
                (unsigned long long)s->outer.peer_conn_id);
        maybe_send_link_up(s);
    }
}

static void drain_outer_transport_to_worker_ring(struct supervisor *s)
{
    uint8_t buf[256];
    size_t n;
    if (s->state != SUP_ACTIVE)
        return;
    while (lmux_proxy_ring_space(&s->phys_to_worker) > 0 &&
           (n = lmux_supervisor_link_pull_worker(&s->outer, buf, sizeof(buf))) > 0) {
        if (n > lmux_proxy_ring_space(&s->phys_to_worker))
            n = lmux_proxy_ring_space(&s->phys_to_worker);
        (void)lmux_proxy_ring_write(&s->phys_to_worker, buf, n);
    }
}

static void feed_outer_physical(struct supervisor *s)
{
    uint8_t buf[256];
    for (;;) {
        struct lmux_carrier_io io = lmux_carrier_read(&s->carrier, buf, sizeof(buf));
        if (io.status == LMUX_CARRIER_IO_PROGRESS) {
            unsigned events = 0;
            (void)lmux_link_input_bytes(&s->outer, buf, (size_t)io.n, &events);
            handle_outer_events(s, events);
            drain_outer_transport_to_worker_ring(s);
            continue;
        }
        if (io.status == LMUX_CARRIER_IO_LOST || io.status == LMUX_CARRIER_IO_FATAL) {
            physical_lost(s);
            return;
        }
        return;
    }
}

static void flush_outer_physical(struct supervisor *s)
{
    while (lmux_carrier_fd(&s->carrier) >= 0) {
        struct lmux_carrier_io io;
        if (!lmux_link_has_pending_output(&s->outer)) {
            size_t plain = lmux_link_transport_tx_len(&s->outer);
            uint64_t seq = s->outer.next_tx_seq;
            int built = lmux_link_build_output(&s->outer);
            if (built > 0 && s->state == SUP_UP_FLUSH) {
                sup_log(s, 1,
                        "outer TRANSPORT encoded state=UP_FLUSH seq=%llu plain_before=%zu pending=%zu",
                        (unsigned long long)seq, plain,
                        lmux_link_output_len(&s->outer));
            }
        }
        if (!lmux_link_has_pending_output(&s->outer))
            return;
        if (s->state == SUP_UP_FLUSH) {
            sup_log(s, 1, "outer physical write attempt pending=%zu",
                    lmux_link_output_len(&s->outer));
        }
        io = lmux_carrier_write(&s->carrier, lmux_link_output_ptr(&s->outer),
                                lmux_link_output_len(&s->outer));
        if (io.status == LMUX_CARRIER_IO_PROGRESS) {
            lmux_link_output_consume(&s->outer, (size_t)io.n);
            if (s->state == SUP_UP_FLUSH) {
                sup_log(s, 1, "outer physical write result=%ld remaining=%zu",
                        (long)io.n, lmux_link_output_len(&s->outer));
                if (!lmux_link_has_pending_output(&s->outer) &&
                    lmux_link_transport_tx_len(&s->outer) == 0)
                    sup_log(s, 1, "outer TRANSPORT committed to physical");
            }
            continue;
        }
        if (io.status == LMUX_CARRIER_IO_WOULD_BLOCK) {
            if (s->state == SUP_UP_FLUSH)
                sup_log(s, 1, "outer physical write would block");
            return;
        }
        physical_lost(s);
        return;
    }
}

static void try_open_physical(struct supervisor *s)
{
    int rc;
    if (lmux_carrier_fd(&s->carrier) >= 0 ||
        lmux_carrier_listen_fd(&s->carrier) >= 0)
        return;
    rc = lmux_carrier_try_open(&s->carrier, now_ms());
    if (rc > 0) {
        sup_log(s, 1, "supervisor physical carrier opened %s",
                lmux_carrier_label(&s->carrier));
        if (s->outer_enabled)
            begin_outer(s);
        else
            maybe_send_link_up(s);
    }
}

static void accept_physical(struct supervisor *s)
{
    int rc = lmux_carrier_accept(&s->carrier);
    if (rc > 0) {
        sup_log(s, 1, "supervisor physical carrier accepted");
        if (s->outer_enabled)
            begin_outer(s);
        else
            maybe_send_link_up(s);
    }
}

static int add_poll(struct pollfd *pfds, int *kind, nfds_t *n,
                    int fd, short events, int k)
{
    if (*n >= 8 || fd < 0)
        return -1;
    pfds[*n].fd = fd;
    pfds[*n].events = events;
    pfds[*n].revents = 0;
    kind[*n] = k;
    (*n)++;
    return 0;
}

static int supervisor_loop(struct supervisor *s)
{
    while (!s->stop) {
        struct pollfd pfds[8];
        int kind[8];
        nfds_t n = 0, i;
        long long now = now_ms();
        int timeout = 1000;

        if (s->worker_pid > 0 && s->restart_delay_ms > 0 &&
            now - s->worker_started_ms > 30000)
            s->restart_delay_ms = 0;
        if (s->worker_pid <= 0 && now >= s->restart_deadline_ms) {
            if (spawn_worker(s) < 0)
                s->restart_deadline_ms = now + 1000;
        }
        try_open_physical(s);
        maybe_send_link_up(s);

        if (s->state == SUP_WORKER_DOWN && s->restart_deadline_ms > now) {
            long long d = s->restart_deadline_ms - now;
            timeout = d > 1000 ? 1000 : (int)d;
        }
        if (s->sigchld_pipe[0] >= 0)
            add_poll(pfds, kind, &n, s->sigchld_pipe[0], POLLIN, 1);
        if (lmux_carrier_listen_fd(&s->carrier) >= 0 &&
            lmux_carrier_fd(&s->carrier) < 0)
            add_poll(pfds, kind, &n, lmux_carrier_listen_fd(&s->carrier),
                     POLLIN, 2);
        if (lmux_carrier_fd(&s->carrier) >= 0) {
            short ev = 0;
            if (s->outer_enabled) {
                if ((s->outer.state != LMUX_LINK_ESTABLISHED ||
                     s->state == SUP_ACTIVE) &&
                    lmux_link_can_accept_physical(&s->outer) &&
                    lmux_link_transport_rx_space(&s->outer) > 0)
                    ev |= POLLIN;
                if (lmux_link_wants_physical_write(&s->outer))
                    ev |= POLLOUT;
                if ((ev & POLLOUT) && s->state == SUP_UP_FLUSH) {
                    sup_log(s, 1,
                            "outer physical POLLOUT armed pending=%zu tx_plain=%zu",
                            lmux_link_output_len(&s->outer),
                            lmux_link_transport_tx_len(&s->outer));
                }
            } else {
                if (s->state == SUP_ACTIVE &&
                    lmux_proxy_ring_space(&s->phys_to_worker) > 0)
                    ev |= POLLIN;
                else if (s->state == SUP_UP_BARRIER)
                    sup_log(s, 1, "supervisor physical POLLIN suppressed during UP_BARRIER");
                if ((s->state == SUP_ACTIVE || s->state == SUP_UP_FLUSH) &&
                    lmux_proxy_ring_len(&s->worker_to_phys) > 0)
                    ev |= POLLOUT;
            }
            add_poll(pfds, kind, &n, lmux_carrier_fd(&s->carrier), ev, 3);
        }
        if (s->worker_ctrl_fd >= 0)
            add_poll(pfds, kind, &n, s->worker_ctrl_fd, POLLIN, 4);
        if (s->worker_data_fd >= 0 &&
            (s->state == SUP_ACTIVE || s->state == SUP_UP_FLUSH)) {
            short ev = 0;
            if (s->outer_enabled) {
                if (lmux_link_transport_tx_space(&s->outer) > 0)
                    ev |= POLLIN;
            } else if (lmux_proxy_ring_space(&s->worker_to_phys) > 0) {
                ev |= POLLIN;
            }
            if (s->state == SUP_ACTIVE && lmux_proxy_ring_len(&s->phys_to_worker) > 0)
                ev |= POLLOUT;
            else if (s->state == SUP_UP_BARRIER)
                sup_log(s, 1, "supervisor worker data suppressed during UP_BARRIER");
            add_poll(pfds, kind, &n, s->worker_data_fd, ev, 5);
        }

        if (poll(pfds, n, timeout) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        for (i = 0; i < n; i++) {
            if (!pfds[i].revents)
                continue;
            if (kind[i] == 1) {
                uint8_t tmp[32];
                while (read(s->sigchld_pipe[0], tmp, sizeof(tmp)) > 0)
                    ;
                reap_workers(s);
            } else if (kind[i] == 2) {
                if (pfds[i].revents & POLLIN)
                    accept_physical(s);
            } else if (kind[i] == 3) {
                if (s->outer_enabled) {
                    if (pfds[i].revents & POLLIN)
                        feed_outer_physical(s);
                    if (pfds[i].revents & POLLOUT)
                        flush_outer_physical(s);
                } else {
                    if (pfds[i].revents & POLLIN) {
                        ssize_t r = lmux_proxy_read_into(lmux_carrier_fd(&s->carrier),
                                                         &s->phys_to_worker);
                        if (r == 0 || r == -1)
                            physical_lost(s);
                    }
                    if (pfds[i].revents & POLLOUT) {
                        ssize_t w = lmux_proxy_write_from(lmux_carrier_fd(&s->carrier),
                                                          &s->worker_to_phys);
                        if (w == -1)
                            physical_lost(s);
                    }
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    physical_lost(s);
            } else if (kind[i] == 4) {
                if (pfds[i].revents & POLLIN)
                    handle_ctrl_msg(s);
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    sup_log(s, 1, "supervisor worker sideband revents=0x%x state=%d pid=%ld",
                            pfds[i].revents, s->state, (long)s->worker_pid);
                    schedule_restart(s, now_ms());
                }
            } else if (kind[i] == 5) {
                if (pfds[i].revents & POLLIN) {
                    if (s->outer_enabled) {
                        for (;;) {
                            uint8_t tmp[512];
                            ssize_t r;
                            size_t room = lmux_link_transport_tx_space(&s->outer);
                            if (room == 0)
                                break;
                            if (room > sizeof(tmp))
                                room = sizeof(tmp);
                            r = read(s->worker_data_fd, tmp, room);
                            if (r > 0) {
                                size_t queued = lmux_supervisor_link_push_worker(&s->outer, tmp, (size_t)r);
                                if (s->state == SUP_UP_FLUSH) {
                                    sup_log(s, 1,
                                            "supervisor worker bytes read=%ld queued_outer=%zu state=UP_FLUSH",
                                            (long)r, queued);
                                }
                                flush_outer_physical(s);
                                continue;
                            }
                            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                                if (s->state == SUP_UP_FLUSH)
                                    s->up_flush_worker_eagain = 1;
                                flush_outer_physical(s);
                                break;
                            }
                            schedule_restart(s, now_ms());
                            break;
                        }
                    } else {
                        for (;;) {
                            ssize_t r;
                            if (lmux_proxy_ring_space(&s->worker_to_phys) == 0)
                                break;
                            r = lmux_proxy_read_into(s->worker_data_fd,
                                                     &s->worker_to_phys);
                            if (r > 0)
                                continue;
                            if (r == -2) {
                                if (s->state == SUP_UP_FLUSH)
                                    s->up_flush_worker_eagain = 1;
                                break;
                            }
                            if (r == 0 || r == -1) {
                                schedule_restart(s, now_ms());
                                break;
                            }
                        }
                    }
                }
                if (pfds[i].revents & POLLOUT) {
                    ssize_t w = lmux_proxy_write_from(s->worker_data_fd,
                                                      &s->phys_to_worker);
                    if (w == -1)
                        schedule_restart(s, now_ms());
                }
                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))
                    schedule_restart(s, now_ms());
            }
            maybe_finish_up_flush(s);
        }
    }
    return 0;
}

static void cleanup_supervisor(struct supervisor *s)
{
    long long end;
    s->shutting_down = 1;
    if (s->worker_ctrl_fd >= 0)
        (void)send_msg(s, LMUX_SUP_SHUTDOWN, s->link_epoch);
    if (s->worker_pid > 0) {
        end = now_ms() + 1000;
        while (now_ms() < end) {
            if (waitpid(s->worker_pid, NULL, WNOHANG) == s->worker_pid) {
                s->worker_pid = -1;
                break;
            }
            usleep(20000);
        }
        if (s->worker_pid > 0) {
            kill(s->worker_pid, SIGTERM);
            end = now_ms() + 1000;
            while (now_ms() < end) {
                if (waitpid(s->worker_pid, NULL, WNOHANG) == s->worker_pid) {
                    s->worker_pid = -1;
                    break;
                }
                usleep(20000);
            }
        }
        if (s->worker_pid > 0) {
            kill(s->worker_pid, SIGKILL);
            (void)waitpid(s->worker_pid, NULL, 0);
        }
    }
    close_worker_fds(s);
    close_fd(&s->sigchld_pipe[0]);
    close_fd(&s->sigchld_pipe[1]);
    lmux_carrier_cleanup(&s->carrier);
}

static void usage(FILE *f)
{
    fprintf(f,
            "lmuxd %s (%s)\n"
            "Usage: lmuxd --supervisor [--outer] --carrier PATH [--worker-path PATH] [--verbose]\n"
            "       lmuxd --supervisor [--outer] --carrier-fd FD [--worker-path PATH] [--verbose]\n"
            "       lmuxd --supervisor [--outer] --carrier-listen PATH|--carrier-connect PATH [test carrier]\n",
            LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
}

int lmux_supervisor_main(int argc, char **argv)
{
    struct supervisor s;
    const char *worker_path = "/proc/self/exe";
    int i;
    memset(&s, 0, sizeof(s));
    lmux_carrier_init(&s.carrier);
    lmux_link_endpoint_init(&s.outer, LMUX_LINK_ROLE_SUPERVISOR);
    s.worker_pid = -1;
    s.worker_data_fd = -1;
    s.worker_ctrl_fd = -1;
    s.sigchld_pipe[0] = -1;
    s.sigchld_pipe[1] = -1;
    s.restart_deadline_ms = 0;
    s.state = SUP_WORKER_DOWN;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--supervisor") == 0) {
            continue;
        } else if (strcmp(argv[i], "--outer") == 0) {
            s.outer_enabled = 1;
        } else if (strcmp(argv[i], "--carrier") == 0) {
            if (++i >= argc || lmux_carrier_set_tty_path(&s.carrier, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-fd") == 0) {
            if (++i >= argc || lmux_carrier_set_fd(&s.carrier, atoi(argv[i])) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-listen") == 0) {
            if (++i >= argc || lmux_carrier_set_unix_listen(&s.carrier, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-connect") == 0) {
            if (++i >= argc || lmux_carrier_set_unix_connect(&s.carrier, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--worker-path") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            worker_path = argv[i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            s.verbose = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("lmuxd %s (%s)\n", LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            return usage(stderr), 1;
        }
    }
    if (!lmux_carrier_is_configured(&s.carrier))
        return usage(stderr), 1;
    s.worker_path = worker_path;
    if (pipe(s.sigchld_pipe) < 0)
        return 1;
    (void)set_nonblock(s.sigchld_pipe[0]);
    (void)set_nonblock(s.sigchld_pipe[1]);
    if (lmux_carrier_start(&s.carrier) < 0) {
        perror(lmux_carrier_label(&s.carrier));
        cleanup_supervisor(&s);
        return 1;
    }
    global_sup = &s;
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGCHLD, &sa, NULL);
    }
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "lmuxd %s (%s) supervisor%s\n", LMUX_BUILD_VERSION,
            LMUX_PROJECT_NAME, s.outer_enabled ? " outer=OUTER/1" : "");
    (void)spawn_worker(&s);
    if (lmux_carrier_fd(&s.carrier) < 0 && lmux_carrier_listen_fd(&s.carrier) < 0)
        try_open_physical(&s);
    if (s.outer_enabled && lmux_carrier_fd(&s.carrier) >= 0 &&
        s.outer.state == LMUX_LINK_DOWN)
        begin_outer(&s);
    (void)supervisor_loop(&s);
    cleanup_supervisor(&s);
    return 0;
}
