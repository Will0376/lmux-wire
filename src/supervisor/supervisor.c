#include "supervisor.h"
#include "boot_protocol.h"
#include "carrier.h"
#include "link_endpoint.h"
#include "lxf.h"
#include "lmux.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LMUX_WORKER_STATE_MAGIC0 'L'
#define LMUX_WORKER_STATE_MAGIC1 'M'
#define LMUX_WORKER_STATE_MAGIC2 'W'
#define LMUX_WORKER_STATE_MAGIC3 'S'
#define LMUX_WORKER_STATE_VERSION 1
#define LMUX_WORKER_STATE_LEN 128
#define LMUX_WORKER_STATE_CRC_OFF 124
#define LMUX_CONFIRM_TIMEOUT_MIN_MS 1000u
#define LMUX_CONFIRM_TIMEOUT_MAX_MS 300000u

enum managed_phase {
    MANAGED_UNMANAGED = 0,
    MANAGED_STABLE = 1,
    MANAGED_TESTING = 2,
    MANAGED_DEGRADED = 3
};

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
    char active_worker_path[LMUX_SUP_STORAGE_PATH_MAX];
    int verbose;
    int stop;
    enum supervisor_state state;
    pid_t worker_pid;
    int worker_data_fd;
    int worker_ctrl_fd;
    int sigchld_pipe[2];
    uint64_t link_epoch;
    uint64_t worker_incarnation;
    uint32_t worker_restart_count;
    long long restart_deadline_ms;
    long long restart_delay_ms;
    long long worker_started_ms;
    int shutting_down;
    int outer_enabled;
    int up_flush_worker_eagain;
    char storage_root[LMUX_SUP_STORAGE_PATH_MAX];
    char workers_dir[LMUX_SUP_STORAGE_PATH_MAX];
    char active_supervisor_path[LMUX_SUP_STORAGE_PATH_MAX];
    int supervisor_restart_pending;
    uint8_t supervisor_restart_sha[32];
    int upload_fd;
    uint64_t upload_id;
    uint64_t upload_declared_size;
    uint64_t upload_received;
    struct sha256_ctx upload_sha;
    enum managed_phase managed_phase;
    uint64_t managed_generation;
    uint64_t activation_id;
    uint8_t confirmed_sha[32];
    uint8_t candidate_sha[32];
    uint8_t running_sha[32];
    int running_sha_valid;
    uint32_t confirm_timeout_ms;
    uint32_t confirm_remaining_ms;
    long long confirm_deadline_ms;
    int confirm_timer_running;
    int confirm_started;
    uint16_t last_update_error;
    struct lmux_link_endpoint outer;
    struct lmux_proxy_ring phys_to_worker;
    struct lmux_proxy_ring worker_to_phys;
};

static struct supervisor *global_sup;

static void sup_log(struct supervisor *s, int verbose, const char *fmt, ...);
static void drain_outer_transport_to_worker_ring(struct supervisor *s);
static int write_full_fd(int fd, const uint8_t *buf, size_t len);
static int ensure_workers_dir(struct supervisor *s);
static void close_worker_fds(struct supervisor *s);
static int spawn_worker(struct supervisor *s);

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

static void upload_tmp_path(struct supervisor *s, char *out, size_t cap)
{
    if (lmux_supervisor_build_upload_tmp_path(s->storage_root, out, cap) < 0 && cap)
        out[0] = 0;
}

static void upload_cleanup(struct supervisor *s)
{
    char path[512];
    if (s->upload_fd >= 0) {
        close(s->upload_fd);
        s->upload_fd = -1;
    }
    if (s->upload_id) {
        upload_tmp_path(s, path, sizeof(path));
        if (path[0])
            unlink(path);
    }
    s->upload_id = 0;
    s->upload_declared_size = 0;
    s->upload_received = 0;
}

static int digest_is_zero(const uint8_t d[32])
{
    size_t i;
    for (i = 0; i < 32; i++)
        if (d[i])
            return 0;
    return 1;
}

static int digest_equal(const uint8_t a[32], const uint8_t b[32])
{
    return memcmp(a, b, 32) == 0;
}

static int path_join2(const char *a, const char *b, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", a, b);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static int state_path(struct supervisor *s, char *out, size_t cap)
{
    return path_join2(s->storage_root, "worker.state", out, cap);
}

static int state_tmp_path(struct supervisor *s, char *out, size_t cap)
{
    return path_join2(s->storage_root, "worker.state.tmp", out, cap);
}

static uint64_t get_le64_local(const uint8_t *p)
{
    return (uint64_t)lmux_get_le32(p) | ((uint64_t)lmux_get_le32(p + 4) << 32);
}

static void put_le64_local(uint8_t *p, uint64_t v)
{
    lmux_put_le32(p, (uint32_t)v);
    lmux_put_le32(p + 4, (uint32_t)(v >> 32));
}

static int state_record_valid(uint16_t phase, uint64_t generation,
                              uint64_t activation_id,
                              const uint8_t confirmed[32],
                              const uint8_t candidate[32])
{
    if (generation == 0 || digest_is_zero(confirmed))
        return 0;
    if (phase == MANAGED_STABLE)
        return activation_id == 0 && digest_is_zero(candidate);
    if (phase == MANAGED_TESTING)
        return activation_id != 0 && !digest_is_zero(candidate) &&
               !digest_equal(confirmed, candidate);
    return 0;
}

static int persist_worker_state(struct supervisor *s, uint16_t phase,
                                uint64_t generation, uint64_t activation_id,
                                const uint8_t confirmed[32],
                                const uint8_t candidate[32])
{
    uint8_t rec[LMUX_WORKER_STATE_LEN];
    char tmp[LMUX_SUP_STORAGE_PATH_MAX], path[LMUX_SUP_STORAGE_PATH_MAX];
    int fd = -1, dfd = -1, rc = -1;
    uint32_t crc;

    if (!state_record_valid(phase, generation, activation_id, confirmed, candidate))
        return -1;
    if (state_tmp_path(s, tmp, sizeof(tmp)) < 0 ||
        state_path(s, path, sizeof(path)) < 0)
        return -1;
    memset(rec, 0, sizeof(rec));
    rec[0] = LMUX_WORKER_STATE_MAGIC0;
    rec[1] = LMUX_WORKER_STATE_MAGIC1;
    rec[2] = LMUX_WORKER_STATE_MAGIC2;
    rec[3] = LMUX_WORKER_STATE_MAGIC3;
    lmux_put_le16(rec + 4, LMUX_WORKER_STATE_VERSION);
    lmux_put_le16(rec + 6, phase);
    put_le64_local(rec + 8, generation);
    put_le64_local(rec + 16, activation_id);
    memcpy(rec + 24, confirmed, 32);
    memcpy(rec + 56, candidate, 32);
    crc = lmux_crc32c(rec, LMUX_WORKER_STATE_CRC_OFF);
    lmux_put_le32(rec + LMUX_WORKER_STATE_CRC_OFF, crc);

    fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0)
        goto out;
    if (write_full_fd(fd, rec, sizeof(rec)) < 0 || fsync(fd) < 0)
        goto out;
    if (close(fd) < 0) {
        fd = -1;
        goto out;
    }
    fd = -1;
    if (rename(tmp, path) < 0)
        goto out;
    dfd = open(s->storage_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0 || fsync(dfd) < 0)
        goto out;
    rc = 0;
out:
    if (fd >= 0)
        close(fd);
    if (dfd >= 0)
        close(dfd);
    if (rc < 0)
        unlink(tmp);
    return rc;
}

static int read_worker_state(struct supervisor *s)
{
    uint8_t rec[LMUX_WORKER_STATE_LEN], zero[32];
    char path[LMUX_SUP_STORAGE_PATH_MAX];
    int fd;
    ssize_t r;
    uint16_t version, phase;
    uint64_t generation, activation_id;
    uint32_t stored_crc, actual_crc;

    memset(zero, 0, sizeof(zero));
    if (state_path(s, path, sizeof(path)) < 0)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            s->managed_phase = MANAGED_UNMANAGED;
            return 0;
        }
        return -1;
    }
    r = read(fd, rec, sizeof(rec));
    if (r == (ssize_t)sizeof(rec)) {
        uint8_t extra;
        if (read(fd, &extra, 1) != 0)
            r = -1;
    }
    close(fd);
    if (r != (ssize_t)sizeof(rec))
        return -1;
    if (rec[0] != LMUX_WORKER_STATE_MAGIC0 ||
        rec[1] != LMUX_WORKER_STATE_MAGIC1 ||
        rec[2] != LMUX_WORKER_STATE_MAGIC2 ||
        rec[3] != LMUX_WORKER_STATE_MAGIC3)
        return -1;
    version = lmux_get_le16(rec + 4);
    phase = lmux_get_le16(rec + 6);
    if (version != LMUX_WORKER_STATE_VERSION)
        return -1;
    stored_crc = lmux_get_le32(rec + LMUX_WORKER_STATE_CRC_OFF);
    actual_crc = lmux_crc32c(rec, LMUX_WORKER_STATE_CRC_OFF);
    if (stored_crc != actual_crc)
        return -1;
    for (size_t i = 88; i < LMUX_WORKER_STATE_CRC_OFF; i++)
        if (rec[i])
            return -1;
    generation = get_le64_local(rec + 8);
    activation_id = get_le64_local(rec + 16);
    if (!state_record_valid(phase, generation, activation_id, rec + 24, rec + 56))
        return -1;
    s->managed_phase = (enum managed_phase)phase;
    s->managed_generation = generation;
    s->activation_id = activation_id;
    memcpy(s->confirmed_sha, rec + 24, 32);
    memcpy(s->candidate_sha, rec + 56, 32);
    if (phase == MANAGED_STABLE) {
        memcpy(s->running_sha, s->confirmed_sha, 32);
        s->running_sha_valid = 1;
    }
    return 0;
}

static int hash_fd_to_digest(int fd, uint8_t digest[32])
{
    uint8_t buf[4096];
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r > 0) {
            sha256_update(&ctx, buf, (size_t)r);
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        return -1;
    }
    sha256_final(&ctx, digest);
    return 0;
}

static int validate_artifact(struct supervisor *s, const uint8_t sha[32],
                             char *path_out, size_t path_cap)
{
    char hex[65], path[LMUX_SUP_STORAGE_PATH_MAX];
    uint8_t got[32];
    struct stat st;
    int fd;

    sha256_hex(sha, hex);
    if (lmux_supervisor_build_worker_artifact_path(s->storage_root, hex,
                                                   path, sizeof(path)) < 0)
        return -1;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0777) != 0755)
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (hash_fd_to_digest(fd, got) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    if (!digest_equal(got, sha))
        return -1;
    if (path_out) {
        if (strlen(path) >= path_cap)
            return -1;
        strcpy(path_out, path);
    }
    return 0;
}

static int path_dirname_copy(const char *path, char *out, size_t cap)
{
    const char *slash;
    size_t len;
    if (!path || path[0] != '/' || !out || cap == 0)
        return -1;
    slash = strrchr(path, '/');
    if (!slash)
        return -1;
    len = slash == path ? 1u : (size_t)(slash - path);
    if (len == 0 || len >= cap)
        return -1;
    memcpy(out, path, len);
    out[len] = 0;
    return 0;
}

static int build_supervisor_install_tmp_path(struct supervisor *s,
                                             char *out, size_t cap)
{
    char dir[LMUX_SUP_STORAGE_PATH_MAX];
    int n;
    if (path_dirname_copy(s->active_supervisor_path, dir, sizeof(dir)) < 0)
        return -1;
    n = snprintf(out, cap, "%s/.lmuxd.install.tmp.%ld", dir, (long)getpid());
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static int hash_path_to_digest(const char *path, uint8_t digest[32])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int rc;
    if (fd < 0)
        return -1;
    rc = hash_fd_to_digest(fd, digest);
    close(fd);
    return rc;
}

static int copy_file_fsync(int src, int dst)
{
    uint8_t buf[4096];
    for (;;) {
        ssize_t r = read(src, buf, sizeof(buf));
        if (r > 0) {
            if (write_full_fd(dst, buf, (size_t)r) < 0)
                return -1;
            continue;
        }
        if (r == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static uint16_t install_supervisor_artifact(struct supervisor *s,
                                            const uint8_t sha[32],
                                            uint8_t *body,
                                            uint32_t *body_len)
{
    char artifact[LMUX_SUP_STORAGE_PATH_MAX];
    char tmp[LMUX_SUP_STORAGE_PATH_MAX];
    char dir[LMUX_SUP_STORAGE_PATH_MAX];
    uint8_t active_sha[32];
    uint32_t flags = LMUX_BOOT_SUPERVISOR_INSTALL_INSTALLED;
    int src = -1, dst = -1, dfd = -1;
    uint16_t status = LMUX_BOOT_STATUS_IO_ERROR;

    if (digest_is_zero(sha))
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (s->managed_phase != MANAGED_STABLE || !digest_equal(s->confirmed_sha, sha))
        return LMUX_BOOT_STATUS_WRONG_STATE;
    if (validate_artifact(s, sha, artifact, sizeof(artifact)) < 0)
        return LMUX_BOOT_STATUS_NOT_FOUND;
    if (hash_path_to_digest(s->active_supervisor_path, active_sha) == 0 &&
        digest_equal(active_sha, sha)) {
        flags |= LMUX_BOOT_SUPERVISOR_INSTALL_ALREADY;
        if (lmux_boot_encode_supervisor_install_response(body, LMUX_LINK_MAX_PAYLOAD,
                                                         body_len, sha, flags) < 0)
            return LMUX_BOOT_STATUS_INTERNAL_ERROR;
        return LMUX_BOOT_STATUS_OK;
    }
    if (build_supervisor_install_tmp_path(s, tmp, sizeof(tmp)) < 0 ||
        path_dirname_copy(s->active_supervisor_path, dir, sizeof(dir)) < 0)
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    src = open(artifact, O_RDONLY | O_CLOEXEC);
    if (src < 0)
        goto out;
    unlink(tmp);
    dst = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0755);
    if (dst < 0) {
        status = errno == ENOSPC ? LMUX_BOOT_STATUS_NO_SPACE : LMUX_BOOT_STATUS_IO_ERROR;
        goto out;
    }
    if (copy_file_fsync(src, dst) < 0 ||
        fchmod(dst, 0755) < 0 ||
        fsync(dst) < 0)
        goto out;
    if (close(dst) < 0) {
        dst = -1;
        goto out;
    }
    dst = -1;
    if (rename(tmp, s->active_supervisor_path) < 0)
        goto out;
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0 || fsync(dfd) < 0)
        goto out;
    if (lmux_boot_encode_supervisor_install_response(body, LMUX_LINK_MAX_PAYLOAD,
                                                     body_len, sha, flags) < 0) {
        status = LMUX_BOOT_STATUS_INTERNAL_ERROR;
        goto out;
    }
    status = LMUX_BOOT_STATUS_OK;
out:
    if (src >= 0)
        close(src);
    if (dst >= 0)
        close(dst);
    if (dfd >= 0)
        close(dfd);
    if (status != LMUX_BOOT_STATUS_OK && tmp[0])
        unlink(tmp);
    return status;
}

static int build_baseline_tmp_path(struct supervisor *s, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/.baseline.tmp", s->workers_dir);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static int materialize_running_baseline(struct supervisor *s, uint8_t digest[32])
{
    char proc[64], tmp[LMUX_SUP_STORAGE_PATH_MAX], final[LMUX_SUP_STORAGE_PATH_MAX];
    char hex[65];
    int src = -1, dst = -1, dfd = -1, rc = -1;
    uint8_t buf[4096];
    struct sha256_ctx ctx;
    struct stat st;

    tmp[0] = 0;
    if (s->worker_pid <= 0)
        return -1;
    if (ensure_workers_dir(s) < 0)
        return -1;
    snprintf(proc, sizeof(proc), "/proc/%ld/exe", (long)s->worker_pid);
    src = open(proc, O_RDONLY | O_CLOEXEC);
    if (src < 0)
        goto out;
    if (build_baseline_tmp_path(s, tmp, sizeof(tmp)) < 0)
        goto out;
    unlink(tmp);
    dst = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0755);
    if (dst < 0)
        goto out;
    sha256_init(&ctx);
    for (;;) {
        ssize_t r = read(src, buf, sizeof(buf));
        if (r > 0) {
            sha256_update(&ctx, buf, (size_t)r);
            if (write_full_fd(dst, buf, (size_t)r) < 0)
                goto out;
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;
        goto out;
    }
    sha256_final(&ctx, digest);
    sha256_hex(digest, hex);
    if (lmux_supervisor_build_worker_artifact_path(s->storage_root, hex,
                                                   final, sizeof(final)) < 0)
        goto out;
    if (stat(final, &st) == 0) {
        if (validate_artifact(s, digest, NULL, 0) < 0)
            goto out;
        close(dst);
        dst = -1;
        unlink(tmp);
        rc = 0;
        goto out;
    }
    if (fchmod(dst, 0755) < 0 || fsync(dst) < 0)
        goto out;
    if (close(dst) < 0) {
        dst = -1;
        goto out;
    }
    dst = -1;
    if (rename(tmp, final) < 0)
        goto out;
    dfd = open(s->workers_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0 || fsync(dfd) < 0)
        goto out;
    rc = 0;
out:
    if (src >= 0)
        close(src);
    if (dst >= 0)
        close(dst);
    if (dfd >= 0)
        close(dfd);
    if (rc < 0 && tmp[0])
        unlink(tmp);
    return rc;
}

static int ensure_workers_dir(struct supervisor *s)
{
    if (mkdir(s->workers_dir, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int set_active_worker_path(struct supervisor *s, const char *path)
{
    if (!path || strlen(path) >= sizeof(s->active_worker_path))
        return -1;
    strcpy(s->active_worker_path, path);
    return 0;
}

static uint64_t random_nonzero64(uint64_t salt)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
            v = 0;
        close(fd);
    }
    if (!v)
        v = ((uint64_t)(uint32_t)getpid() << 32) ^ (uint64_t)now_ms() ^ salt;
    return v ? v : 1;
}

static void managed_mark_degraded(struct supervisor *s, uint16_t reason)
{
    s->managed_phase = MANAGED_DEGRADED;
    s->last_update_error = reason;
    s->confirm_timer_running = 0;
    s->confirm_started = 0;
    s->active_worker_path[0] = 0;
    memset(s->running_sha, 0, sizeof(s->running_sha));
    s->running_sha_valid = 0;
    sup_log(s, 1, "managed worker DEGRADED reason=%s",
            lmux_boot_status_name(reason));
}

static int managed_select_artifact(struct supervisor *s,
                                   const uint8_t sha[32])
{
    return validate_artifact(s, sha, s->active_worker_path,
                             sizeof(s->active_worker_path));
}

static void managed_pause_confirm_timer(struct supervisor *s)
{
    long long now;
    if (s->managed_phase != MANAGED_TESTING || !s->confirm_timer_running)
        return;
    now = now_ms();
    if (s->confirm_deadline_ms > now)
        s->confirm_remaining_ms = (uint32_t)(s->confirm_deadline_ms - now);
    else
        s->confirm_remaining_ms = 0;
    s->confirm_timer_running = 0;
    sup_log(s, 1, "managed confirmation timer paused remaining_ms=%u",
            s->confirm_remaining_ms);
}

static void managed_resume_confirm_timer(struct supervisor *s)
{
    if (s->managed_phase != MANAGED_TESTING || !s->confirm_started ||
        s->confirm_timer_running || s->confirm_remaining_ms == 0)
        return;
    s->confirm_deadline_ms = now_ms() + s->confirm_remaining_ms;
    s->confirm_timer_running = 1;
    sup_log(s, 1, "managed confirmation timer resumed remaining_ms=%u",
            s->confirm_remaining_ms);
}

static void managed_start_confirm_timer(struct supervisor *s)
{
    if (s->managed_phase != MANAGED_TESTING || s->confirm_started ||
        !s->running_sha_valid || !digest_equal(s->running_sha, s->candidate_sha))
        return;
    s->confirm_started = 1;
    s->confirm_remaining_ms = s->confirm_timeout_ms;
    s->confirm_deadline_ms = now_ms() + s->confirm_remaining_ms;
    s->confirm_timer_running = 1;
    sup_log(s, 1, "managed confirmation timer started activation=%llu timeout_ms=%u",
            (unsigned long long)s->activation_id, s->confirm_timeout_ms);
}

static int managed_persist_stable(struct supervisor *s,
                                  const uint8_t sha[32], uint64_t generation)
{
    uint8_t zero[32];
    memset(zero, 0, sizeof(zero));
    if (persist_worker_state(s, MANAGED_STABLE, generation, 0, sha, zero) < 0)
        return -1;
    s->managed_phase = MANAGED_STABLE;
    s->managed_generation = generation;
    s->activation_id = 0;
    memcpy(s->confirmed_sha, sha, 32);
    memset(s->candidate_sha, 0, sizeof(s->candidate_sha));
    s->confirm_timer_running = 0;
    s->confirm_started = 0;
    s->confirm_timeout_ms = 0;
    s->confirm_remaining_ms = 0;
    s->last_update_error = LMUX_BOOT_STATUS_OK;
    return 0;
}

static void terminate_worker_now(struct supervisor *s)
{
    long long end;
    if (s->worker_pid <= 0)
        return;
    if (s->worker_ctrl_fd >= 0)
        (void)send_msg(s, LMUX_SUP_SHUTDOWN, s->link_epoch);
    end = now_ms() + 500;
    while (now_ms() < end) {
        pid_t r = waitpid(s->worker_pid, NULL, WNOHANG);
        if (r == s->worker_pid || (r < 0 && errno == ECHILD)) {
            s->worker_pid = -1;
            break;
        }
        usleep(10000);
    }
    if (s->worker_pid > 0) {
        if (kill(s->worker_pid, SIGTERM) < 0 && errno == ESRCH)
            s->worker_pid = -1;
    }
    if (s->worker_pid > 0) {
        end = now_ms() + 1000;
        while (now_ms() < end) {
            pid_t r = waitpid(s->worker_pid, NULL, WNOHANG);
            if (r == s->worker_pid || (r < 0 && errno == ECHILD)) {
                s->worker_pid = -1;
                break;
            }
            usleep(10000);
        }
    }
    if (s->worker_pid > 0) {
        if (kill(s->worker_pid, SIGKILL) < 0 && errno == ESRCH) {
            s->worker_pid = -1;
        } else {
            (void)waitpid(s->worker_pid, NULL, 0);
            s->worker_pid = -1;
        }
    }
    close_worker_fds(s);
    clear_proxy(s);
    s->state = SUP_WORKER_DOWN;
}

static int managed_spawn_selected(struct supervisor *s)
{
    if (!s->active_worker_path[0])
        return -1;
    s->restart_deadline_ms = 0;
    s->restart_delay_ms = 0;
    return spawn_worker(s);
}

static int managed_rollback_to_confirmed(struct supervisor *s, const char *why)
{
    char path[LMUX_SUP_STORAGE_PATH_MAX];
    uint8_t confirmed[32];
    uint64_t generation;

    if (s->managed_phase != MANAGED_TESTING)
        return -1;
    memcpy(confirmed, s->confirmed_sha, 32);
    generation = s->managed_generation;
    if (validate_artifact(s, confirmed, path, sizeof(path)) < 0 ||
        managed_persist_stable(s, confirmed, generation) < 0) {
        managed_mark_degraded(s, LMUX_BOOT_STATUS_IO_ERROR);
        return -1;
    }
    sup_log(s, 1, "managed rollback to confirmed reason=%s", why ? why : "-");
    terminate_worker_now(s);
    if (set_active_worker_path(s, path) < 0) {
        managed_mark_degraded(s, LMUX_BOOT_STATUS_INTERNAL_ERROR);
        return -1;
    }
    memcpy(s->running_sha, confirmed, 32);
    s->running_sha_valid = 1;
    if (managed_spawn_selected(s) < 0) {
        managed_mark_degraded(s, LMUX_BOOT_STATUS_IO_ERROR);
        return -1;
    }
    return 0;
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
    int replacement = s->worker_incarnation != 0;
    if (!s->active_worker_path[0])
        return -1;
    if (create_socketpair(SOCK_STREAM, data_pair) < 0 ||
        create_socketpair(SOCK_SEQPACKET, ctrl_pair) < 0)
        goto fail;
    pid = lmux_supervisor_spawn_worker(s->active_worker_path, data_pair[1],
                                       ctrl_pair[1], s->verbose);
    if (pid < 0)
        goto fail;
    close(data_pair[1]);
    close(ctrl_pair[1]);
    close_worker_fds(s);
    s->worker_pid = pid;
    if (s->worker_incarnation != UINT64_MAX)
        s->worker_incarnation++;
    if (replacement && s->worker_restart_count != UINT32_MAX)
        s->worker_restart_count++;
    s->worker_data_fd = data_pair[0];
    s->worker_ctrl_fd = ctrl_pair[0];
    s->worker_started_ms = now_ms();
    s->state = SUP_LINK_DOWN;
    clear_proxy(s);
    sup_log(s, 1, "supervisor worker spawned pid=%ld path=%s",
            (long)pid, s->active_worker_path);
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
    if (s->managed_phase == MANAGED_TESTING && s->running_sha_valid &&
        digest_equal(s->running_sha, s->candidate_sha)) {
        (void)now;
        (void)managed_rollback_to_confirmed(s, "candidate-exit");
        return;
    }
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
    managed_pause_confirm_timer(s);
    clear_proxy(s);
    upload_cleanup(s);
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
                managed_start_confirm_timer(s);
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
    upload_cleanup(s);
    managed_pause_confirm_timer(s);
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
    upload_cleanup(s);
    managed_pause_confirm_timer(s);
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
        managed_resume_confirm_timer(s);
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
        managed_resume_confirm_timer(s);
        maybe_send_link_up(s);
    }
}

static uint32_t boot_worker_state(const struct supervisor *s)
{
    if (s->worker_pid <= 0) {
        if (s->restart_deadline_ms > now_ms())
            return LMUX_BOOT_WORKER_BACKOFF;
        return LMUX_BOOT_WORKER_STOPPED;
    }
    switch (s->state) {
    case SUP_WORKER_DOWN: return LMUX_BOOT_WORKER_BACKOFF;
    case SUP_LINK_DOWN:
    case SUP_DOWN_BARRIER:
        return LMUX_BOOT_WORKER_LINK_DOWN;
    case SUP_UP_BARRIER:
    case SUP_UP_FLUSH:
        return LMUX_BOOT_WORKER_LINK_UP_BARRIER;
    case SUP_ACTIVE:
        return LMUX_BOOT_WORKER_ACTIVE;
    }
    return LMUX_BOOT_WORKER_UNKNOWN;
}

static int queue_boot_response(struct supervisor *s, uint16_t opcode,
                               uint16_t status, uint64_t request_id,
                               const uint8_t *body, uint32_t body_len)
{
    uint8_t payload[LMUX_LINK_MAX_PAYLOAD];
    uint32_t len = 0;
    if (lmux_boot_make_response(payload, sizeof(payload), &len, opcode,
                                status, request_id, body, body_len) < 0)
        return -1;
    return lmux_link_boot_queue_frame(&s->outer, LMUX_LINK_T_BOOT_RESPONSE,
                                      payload, len);
}

static int write_full_fd(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static uint64_t next_upload_id(struct supervisor *s)
{
    uint64_t id = ((uint64_t)(uint32_t)getpid() << 32) ^
                  (uint64_t)now_ms() ^ s->worker_incarnation ^
                  s->outer.local_conn_id;
    if (id == 0)
        id = 1;
    if (id == s->upload_id)
        id++;
    return id ? id : 1;
}

static uint16_t upload_begin(struct supervisor *s, const uint8_t *payload,
                             uint32_t len, uint8_t *body, uint32_t *body_len)
{
    struct lmux_boot_upload_begin req;
    char tmp[512];
    int fd;
    if (lmux_boot_decode_upload_begin(payload, len, &req) < 0)
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (s->upload_id)
        return LMUX_BOOT_STATUS_BUSY;
    if (ensure_workers_dir(s) < 0)
        return errno == ENOSPC ? LMUX_BOOT_STATUS_NO_SPACE : LMUX_BOOT_STATUS_IO_ERROR;
    upload_tmp_path(s, tmp, sizeof(tmp));
    if (!tmp[0])
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    unlink(tmp);
    fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0755);
    if (fd < 0)
        return errno == ENOSPC ? LMUX_BOOT_STATUS_NO_SPACE : LMUX_BOOT_STATUS_IO_ERROR;
    s->upload_fd = fd;
    s->upload_id = next_upload_id(s);
    s->upload_declared_size = req.declared_size;
    s->upload_received = 0;
    sha256_init(&s->upload_sha);
    if (lmux_boot_encode_upload_begin_response(body, LMUX_LINK_MAX_PAYLOAD,
                                               body_len, s->upload_id,
                                               LMUX_BOOT_UPLOAD_MAX_CHUNK) < 0) {
        upload_cleanup(s);
        return LMUX_BOOT_STATUS_INTERNAL_ERROR;
    }
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t upload_data(struct supervisor *s, const uint8_t *payload,
                            uint32_t len, uint8_t *body, uint32_t *body_len)
{
    struct lmux_boot_upload_data req;
    if (lmux_boot_decode_upload_data(payload, len, &req) < 0)
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (!s->upload_id || s->upload_fd < 0)
        return LMUX_BOOT_STATUS_WRONG_STATE;
    if (req.upload_id != s->upload_id) {
        upload_cleanup(s);
        return LMUX_BOOT_STATUS_ID_MISMATCH;
    }
    if (req.offset != s->upload_received) {
        upload_cleanup(s);
        return LMUX_BOOT_STATUS_OFFSET_MISMATCH;
    }
    if (req.data_len > LMUX_BOOT_UPLOAD_MAX_CHUNK ||
        req.data_len > s->upload_declared_size - s->upload_received) {
        upload_cleanup(s);
        return LMUX_BOOT_STATUS_SIZE_VIOLATION;
    }
    if (write_full_fd(s->upload_fd, req.data, req.data_len) < 0) {
        upload_cleanup(s);
        return errno == ENOSPC ? LMUX_BOOT_STATUS_NO_SPACE : LMUX_BOOT_STATUS_IO_ERROR;
    }
    sha256_update(&s->upload_sha, req.data, req.data_len);
    s->upload_received += req.data_len;
    if (lmux_boot_encode_upload_data_response(body, LMUX_LINK_MAX_PAYLOAD,
                                              body_len, s->upload_received) < 0)
        return LMUX_BOOT_STATUS_INTERNAL_ERROR;
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t upload_end(struct supervisor *s, const uint8_t *payload,
                           uint32_t len, uint8_t *body, uint32_t *body_len)
{
    struct lmux_boot_upload_end req;
    uint8_t digest[32];
    char hex[65], tmp[LMUX_SUP_STORAGE_PATH_MAX], final[LMUX_SUP_STORAGE_PATH_MAX];
    int dfd;
    if (lmux_boot_decode_upload_end(payload, len, &req) < 0)
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (!s->upload_id || s->upload_fd < 0)
        return LMUX_BOOT_STATUS_WRONG_STATE;
    if (req.upload_id != s->upload_id)
        return LMUX_BOOT_STATUS_ID_MISMATCH;
    if (s->upload_received != s->upload_declared_size)
        return LMUX_BOOT_STATUS_SIZE_VIOLATION;
    sha256_final(&s->upload_sha, digest);
    if (memcmp(digest, req.expected_sha256, sizeof(digest)) != 0) {
        upload_cleanup(s);
        return LMUX_BOOT_STATUS_BAD_HASH;
    }
    if (fchmod(s->upload_fd, 0755) < 0 ||
        fsync(s->upload_fd) < 0) {
        upload_cleanup(s);
        return LMUX_BOOT_STATUS_IO_ERROR;
    }
    close(s->upload_fd);
    s->upload_fd = -1;
    sha256_hex(digest, hex);
    upload_tmp_path(s, tmp, sizeof(tmp));
    if (lmux_supervisor_build_worker_artifact_path(s->storage_root, hex,
                                                   final, sizeof(final)) < 0 ||
        rename(tmp, final) < 0) {
        upload_cleanup(s);
        return errno == ENOSPC ? LMUX_BOOT_STATUS_NO_SPACE : LMUX_BOOT_STATUS_IO_ERROR;
    }
    dfd = open(s->workers_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        (void)fsync(dfd);
        close(dfd);
    }
    if (lmux_boot_encode_upload_end_response(body, LMUX_LINK_MAX_PAYLOAD,
                                             body_len, s->upload_received,
                                             digest) < 0) {
        upload_cleanup(s);
        return LMUX_BOOT_STATUS_INTERNAL_ERROR;
    }
    upload_cleanup(s);
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t upload_abort(struct supervisor *s, const uint8_t *payload,
                             uint32_t len)
{
    uint64_t id = 0;
    if (lmux_boot_decode_upload_abort(payload, len, &id) < 0)
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (!s->upload_id)
        return LMUX_BOOT_STATUS_OK;
    if (id != s->upload_id)
        return LMUX_BOOT_STATUS_ID_MISMATCH;
    upload_cleanup(s);
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t activate_candidate(struct supervisor *s, const uint8_t *payload,
                                   uint32_t len, uint8_t *body,
                                   uint32_t *body_len)
{
    struct lmux_boot_activate req;
    uint8_t baseline[32], zero[32];
    char candidate_path[LMUX_SUP_STORAGE_PATH_MAX];
    uint64_t new_generation, new_activation;

    memset(zero, 0, sizeof(zero));
    if (lmux_boot_decode_activate(payload, len, &req) < 0 ||
        req.confirm_timeout_ms < LMUX_CONFIRM_TIMEOUT_MIN_MS ||
        req.confirm_timeout_ms > LMUX_CONFIRM_TIMEOUT_MAX_MS ||
        digest_is_zero(req.candidate_sha256))
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (s->managed_phase == MANAGED_DEGRADED)
        return LMUX_BOOT_STATUS_WRONG_STATE;
    if (s->managed_phase == MANAGED_TESTING) {
        if (digest_equal(req.candidate_sha256, s->candidate_sha)) {
            if (lmux_boot_encode_activate_response(body, LMUX_LINK_MAX_PAYLOAD,
                                                   body_len, s->activation_id,
                                                   s->managed_generation,
                                                   s->confirm_timeout_ms,
                                                   LMUX_BOOT_UPDATE_TESTING) < 0)
                return LMUX_BOOT_STATUS_INTERNAL_ERROR;
            return LMUX_BOOT_STATUS_OK;
        }
        return LMUX_BOOT_STATUS_BUSY;
    }
    if (s->managed_phase == MANAGED_STABLE &&
        digest_equal(req.candidate_sha256, s->confirmed_sha)) {
        if (lmux_boot_encode_activate_response(body, LMUX_LINK_MAX_PAYLOAD,
                                               body_len, 0,
                                               s->managed_generation,
                                               0, LMUX_BOOT_UPDATE_STABLE) < 0)
            return LMUX_BOOT_STATUS_INTERNAL_ERROR;
        return LMUX_BOOT_STATUS_OK;
    }
    if (validate_artifact(s, req.candidate_sha256, candidate_path,
                          sizeof(candidate_path)) < 0)
        return LMUX_BOOT_STATUS_NOT_FOUND;

    if (s->managed_phase == MANAGED_UNMANAGED) {
        if (materialize_running_baseline(s, baseline) < 0)
            return LMUX_BOOT_STATUS_WRONG_STATE;
        if (managed_persist_stable(s, baseline, 1) < 0)
            return LMUX_BOOT_STATUS_IO_ERROR;
        sup_log(s, 1, "managed baseline materialized generation=1");
    }
    if (s->managed_phase != MANAGED_STABLE)
        return LMUX_BOOT_STATUS_WRONG_STATE;
    new_generation = s->managed_generation + 1;
    if (new_generation == 0)
        return LMUX_BOOT_STATUS_INTERNAL_ERROR;
    new_activation = random_nonzero64(s->managed_generation ^ s->outer.local_conn_id);
    if (persist_worker_state(s, MANAGED_TESTING, new_generation, new_activation,
                             s->confirmed_sha, req.candidate_sha256) < 0)
        return LMUX_BOOT_STATUS_IO_ERROR;

    s->managed_phase = MANAGED_TESTING;
    s->managed_generation = new_generation;
    s->activation_id = new_activation;
    memcpy(s->candidate_sha, req.candidate_sha256, 32);
    s->confirm_timeout_ms = req.confirm_timeout_ms;
    s->confirm_remaining_ms = req.confirm_timeout_ms;
    s->confirm_timer_running = 0;
    s->confirm_started = 0;
    s->last_update_error = LMUX_BOOT_STATUS_OK;
    set_active_worker_path(s, candidate_path);
    memcpy(s->running_sha, req.candidate_sha256, 32);
    s->running_sha_valid = 1;

    terminate_worker_now(s);
    if (managed_spawn_selected(s) < 0) {
        s->last_update_error = LMUX_BOOT_STATUS_IO_ERROR;
        (void)managed_rollback_to_confirmed(s, "spawn-failure");
        return LMUX_BOOT_STATUS_IO_ERROR;
    }
    sup_log(s, 1, "managed ACTIVATE testing activation=%llu generation=%llu",
            (unsigned long long)s->activation_id,
            (unsigned long long)s->managed_generation);
    if (lmux_boot_encode_activate_response(body, LMUX_LINK_MAX_PAYLOAD,
                                           body_len, s->activation_id,
                                           s->managed_generation,
                                           s->confirm_timeout_ms,
                                           LMUX_BOOT_UPDATE_TESTING) < 0)
        return LMUX_BOOT_STATUS_INTERNAL_ERROR;
    (void)zero;
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t confirm_candidate(struct supervisor *s, const uint8_t *payload,
                                  uint32_t len)
{
    uint64_t id = 0;
    if (lmux_boot_decode_activation_id(payload, len, &id) < 0)
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (s->managed_phase != MANAGED_TESTING)
        return LMUX_BOOT_STATUS_NOT_TESTING;
    if (id != s->activation_id)
        return LMUX_BOOT_STATUS_ID_MISMATCH;
    if (!s->running_sha_valid || !digest_equal(s->running_sha, s->candidate_sha))
        return LMUX_BOOT_STATUS_WRONG_STATE;
    if (managed_persist_stable(s, s->candidate_sha, s->managed_generation) < 0)
        return LMUX_BOOT_STATUS_IO_ERROR;
    memcpy(s->running_sha, s->confirmed_sha, 32);
    s->running_sha_valid = 1;
    sup_log(s, 1, "managed CONFIRM generation=%llu",
            (unsigned long long)s->managed_generation);
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t rollback_candidate(struct supervisor *s, const uint8_t *payload,
                                   uint32_t len)
{
    uint64_t id = 0;
    if (lmux_boot_decode_activation_id(payload, len, &id) < 0)
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (s->managed_phase != MANAGED_TESTING)
        return LMUX_BOOT_STATUS_NOT_TESTING;
    if (id != s->activation_id)
        return LMUX_BOOT_STATUS_ID_MISMATCH;
    if (managed_rollback_to_confirmed(s, "explicit") < 0)
        return LMUX_BOOT_STATUS_IO_ERROR;
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t update_status(struct supervisor *s, uint8_t *body,
                              uint32_t *body_len)
{
    struct lmux_boot_update_status st;
    memset(&st, 0, sizeof(st));
    st.phase = s->managed_phase == MANAGED_UNMANAGED ? LMUX_BOOT_UPDATE_UNMANAGED :
        (s->managed_phase == MANAGED_STABLE ? LMUX_BOOT_UPDATE_STABLE :
         (s->managed_phase == MANAGED_TESTING ? LMUX_BOOT_UPDATE_TESTING :
          LMUX_BOOT_UPDATE_DEGRADED));
    st.generation = s->managed_generation;
    st.activation_id = s->activation_id;
    if (s->confirm_timer_running) {
        long long rem = s->confirm_deadline_ms - now_ms();
        st.flags |= LMUX_BOOT_UPDATE_TIMER_RUNNING;
        st.confirm_remaining_ms = rem > 0 ? (uint32_t)rem : 0;
    } else if (s->managed_phase == MANAGED_TESTING && s->confirm_started) {
        st.flags |= LMUX_BOOT_UPDATE_TIMER_PAUSED;
        st.confirm_remaining_ms = s->confirm_remaining_ms;
    } else {
        st.confirm_remaining_ms = s->confirm_remaining_ms;
    }
    st.last_update_error = s->last_update_error;
    memcpy(st.confirmed_sha256, s->confirmed_sha, 32);
    memcpy(st.candidate_sha256, s->candidate_sha, 32);
    if (s->running_sha_valid)
        memcpy(st.running_sha256, s->running_sha, 32);
    if (lmux_boot_encode_update_status(body, LMUX_LINK_MAX_PAYLOAD,
                                       body_len, &st) < 0)
        return LMUX_BOOT_STATUS_INTERNAL_ERROR;
    return LMUX_BOOT_STATUS_OK;
}

static uint16_t request_supervisor_restart(struct supervisor *s,
                                           const uint8_t sha[32])
{
    uint8_t active_sha[32];
    if (digest_is_zero(sha))
        return LMUX_BOOT_STATUS_BAD_REQUEST;
    if (s->supervisor_restart_pending)
        return LMUX_BOOT_STATUS_BUSY;
    if (s->managed_phase != MANAGED_STABLE || !digest_equal(s->confirmed_sha, sha))
        return LMUX_BOOT_STATUS_WRONG_STATE;
    if (hash_path_to_digest(s->active_supervisor_path, active_sha) < 0 ||
        !digest_equal(active_sha, sha))
        return LMUX_BOOT_STATUS_BAD_HASH;
    s->supervisor_restart_pending = 1;
    memcpy(s->supervisor_restart_sha, sha, 32);
    sup_log(s, 1, "supervisor restart accepted");
    return LMUX_BOOT_STATUS_OK;
}

static void handle_boot_request(struct supervisor *s, const uint8_t *payload,
                                uint32_t len)
{
    struct lmux_boot_request req;
    uint8_t body[128];
    uint32_t body_len = 0;
    uint16_t status = LMUX_BOOT_STATUS_OK;

    if (lmux_boot_parse_request(payload, len, &req) < 0) {
        sup_log(s, 1, "outer malformed BOOT request");
        return;
    }
    if (req.flags != 0) {
        (void)queue_boot_response(s, req.opcode, LMUX_BOOT_STATUS_BAD_REQUEST,
                                  req.request_id, NULL, 0);
        return;
    }
    if (s->supervisor_restart_pending &&
        req.opcode != LMUX_BOOT_GET_INFO &&
        req.opcode != LMUX_BOOT_WORKER_STATUS &&
        req.opcode != LMUX_BOOT_UPDATE_STATUS) {
        (void)queue_boot_response(s, req.opcode, LMUX_BOOT_STATUS_BUSY,
                                  req.request_id, NULL, 0);
        return;
    }
    if (req.opcode == LMUX_BOOT_GET_INFO) {
        struct lmux_boot_info info;
        if (req.body_len != 0) {
            status = LMUX_BOOT_STATUS_BAD_REQUEST;
        } else {
            info.outer_version = LMUX_LINK_VERSION;
            info.boot_version = LMUX_BOOT_VERSION;
            info.capabilities = s->outer.local_caps & s->outer.peer_caps;
            info.max_outer_payload = s->outer.local_max_outer_payload;
            info.max_transport_payload = s->outer.local_max_transport_payload;
            if (lmux_boot_encode_get_info_response(body, sizeof(body),
                                                   &body_len, &info) < 0)
                status = LMUX_BOOT_STATUS_INTERNAL_ERROR;
        }
    } else if (req.opcode == LMUX_BOOT_WORKER_STATUS) {
        struct lmux_boot_worker_status ws;
        if (req.body_len != 0) {
            status = LMUX_BOOT_STATUS_BAD_REQUEST;
        } else {
            memset(&ws, 0, sizeof(ws));
            ws.worker_state = boot_worker_state(s);
            ws.worker_incarnation = s->worker_incarnation;
            ws.uptime_ms = s->worker_pid > 0 && s->worker_started_ms > 0 ?
                (uint64_t)(now_ms() - s->worker_started_ms) : 0;
            ws.restart_count = s->worker_restart_count;
            if (lmux_boot_encode_worker_status(body, sizeof(body),
                                               &body_len, &ws) < 0)
                status = LMUX_BOOT_STATUS_INTERNAL_ERROR;
        }
    } else if (req.opcode == LMUX_BOOT_UPLOAD_BEGIN) {
        status = upload_begin(s, req.body, req.body_len, body, &body_len);
    } else if (req.opcode == LMUX_BOOT_UPLOAD_DATA) {
        status = upload_data(s, req.body, req.body_len, body, &body_len);
    } else if (req.opcode == LMUX_BOOT_UPLOAD_END) {
        status = upload_end(s, req.body, req.body_len, body, &body_len);
    } else if (req.opcode == LMUX_BOOT_UPLOAD_ABORT) {
        status = upload_abort(s, req.body, req.body_len);
    } else if (req.opcode == LMUX_BOOT_ACTIVATE_CANDIDATE) {
        status = activate_candidate(s, req.body, req.body_len, body, &body_len);
    } else if (req.opcode == LMUX_BOOT_CONFIRM_CANDIDATE) {
        status = confirm_candidate(s, req.body, req.body_len);
    } else if (req.opcode == LMUX_BOOT_ROLLBACK) {
        status = rollback_candidate(s, req.body, req.body_len);
    } else if (req.opcode == LMUX_BOOT_UPDATE_STATUS) {
        if (req.body_len != 0)
            status = LMUX_BOOT_STATUS_BAD_REQUEST;
        else
            status = update_status(s, body, &body_len);
    } else if (req.opcode == LMUX_BOOT_SUPERVISOR_INSTALL) {
        uint8_t sha[32];
        if (lmux_boot_decode_sha256(req.body, req.body_len, sha) < 0)
            status = LMUX_BOOT_STATUS_BAD_REQUEST;
        else
            status = install_supervisor_artifact(s, sha, body, &body_len);
    } else if (req.opcode == LMUX_BOOT_SUPERVISOR_RESTART) {
        uint8_t sha[32];
        if (lmux_boot_decode_sha256(req.body, req.body_len, sha) < 0)
            status = LMUX_BOOT_STATUS_BAD_REQUEST;
        else
            status = request_supervisor_restart(s, sha);
    } else {
        status = LMUX_BOOT_STATUS_UNKNOWN_OPCODE;
    }
    if (queue_boot_response(s, req.opcode, status, req.request_id,
                            status == LMUX_BOOT_STATUS_OK ? body : NULL,
                            status == LMUX_BOOT_STATUS_OK ? body_len : 0) < 0)
        sup_log(s, 1, "outer BOOT response queue full opcode=%u", req.opcode);
    else
        sup_log(s, 1, "outer BOOT response queued opcode=%u status=%s",
                req.opcode, lmux_boot_status_name(status));
}

static void drain_outer_boot_requests(struct supervisor *s)
{
    uint8_t type;
    uint8_t payload[LMUX_LINK_MAX_PAYLOAD];
    uint32_t len;
    while (lmux_link_boot_read_frame(&s->outer, &type, payload,
                                     sizeof(payload), &len) == 0) {
        if (type == LMUX_LINK_T_BOOT_REQUEST)
            handle_boot_request(s, payload, len);
        else
            sup_log(s, 1, "outer ignored BOOT frame type=%u", type);
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
            drain_outer_boot_requests(s);
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

static void maybe_finish_supervisor_restart(struct supervisor *s)
{
    if (!s->supervisor_restart_pending)
        return;
    if (lmux_link_has_pending_output(&s->outer) ||
        lmux_link_boot_tx_len(&s->outer) != 0 ||
        lmux_link_transport_tx_len(&s->outer) != 0)
        return;
    sup_log(s, 1, "supervisor restart response committed; exiting");
    s->shutting_down = 1;
    s->stop = 1;
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

        if (s->managed_phase == MANAGED_TESTING &&
            s->confirm_timer_running && now >= s->confirm_deadline_ms) {
            s->confirm_timer_running = 0;
            s->confirm_remaining_ms = 0;
            (void)managed_rollback_to_confirmed(s, "confirm-timeout");
            now = now_ms();
        }
        if (s->worker_pid > 0 && s->restart_delay_ms > 0 &&
            now - s->worker_started_ms > 30000)
            s->restart_delay_ms = 0;
        if (s->worker_pid <= 0 && now >= s->restart_deadline_ms) {
            if (spawn_worker(s) < 0)
                s->restart_deadline_ms = now + 1000;
        }
        try_open_physical(s);
        maybe_send_link_up(s);
        if (s->outer_enabled)
            maybe_finish_supervisor_restart(s);

        if (s->state == SUP_WORKER_DOWN && s->restart_deadline_ms > now) {
            long long d = s->restart_deadline_ms - now;
            timeout = d > 1000 ? 1000 : (int)d;
        }
        if (s->managed_phase == MANAGED_TESTING && s->confirm_timer_running) {
            long long d = s->confirm_deadline_ms - now;
            if (d < 0)
                d = 0;
            if (d < timeout)
                timeout = (int)d;
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
                     (lmux_link_transport_rx_space(&s->outer) > 0 &&
                      lmux_link_boot_rx_space(&s->outer) > 0)) &&
                    lmux_link_can_accept_physical(&s->outer) &&
                    lmux_link_boot_rx_space(&s->outer) > 0)
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
                    maybe_finish_supervisor_restart(s);
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
    upload_cleanup(s);
    close_fd(&s->sigchld_pipe[0]);
    close_fd(&s->sigchld_pipe[1]);
    lmux_carrier_cleanup(&s->carrier);
}

static int managed_startup_select_worker(struct supervisor *s)
{
    char path[LMUX_SUP_STORAGE_PATH_MAX];
    if (read_worker_state(s) < 0) {
        managed_mark_degraded(s, LMUX_BOOT_STATUS_BAD_REQUEST);
        return 0;
    }
    if (s->managed_phase == MANAGED_UNMANAGED) {
        if (set_active_worker_path(s, s->worker_path) < 0) {
            managed_mark_degraded(s, LMUX_BOOT_STATUS_BAD_REQUEST);
            return -1;
        }
        sup_log(s, 1, "managed worker state=UNMANAGED legacy_path=%s",
                s->active_worker_path);
        return 1;
    }
    if (s->managed_phase == MANAGED_STABLE) {
        if (managed_select_artifact(s, s->confirmed_sha) < 0) {
            managed_mark_degraded(s, LMUX_BOOT_STATUS_NOT_FOUND);
            return 0;
        }
        memcpy(s->running_sha, s->confirmed_sha, 32);
        s->running_sha_valid = 1;
        sup_log(s, 1, "managed worker state=STABLE generation=%llu",
                (unsigned long long)s->managed_generation);
        return 1;
    }
    if (s->managed_phase == MANAGED_TESTING) {
        if (validate_artifact(s, s->confirmed_sha, path, sizeof(path)) < 0 ||
            managed_persist_stable(s, s->confirmed_sha, s->managed_generation) < 0 ||
            set_active_worker_path(s, path) < 0) {
            managed_mark_degraded(s, LMUX_BOOT_STATUS_NOT_FOUND);
            return 0;
        }
        memcpy(s->running_sha, s->confirmed_sha, 32);
        s->running_sha_valid = 1;
        sup_log(s, 1, "managed worker startup rolled TESTING back to STABLE generation=%llu",
                (unsigned long long)s->managed_generation);
        return 1;
    }
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
            "lmuxd %s (%s)\n"
            "Usage: lmuxd --supervisor [--outer] --carrier PATH [--storage-root DIR] [--worker-path PATH] [--verbose]\n"
            "       lmuxd --supervisor [--outer] --carrier-fd FD [--storage-root DIR] [--worker-path PATH] [--verbose]\n"
            "       lmuxd --supervisor [--outer] --carrier-listen PATH|--carrier-connect PATH [test carrier]\n",
            LMUX_BUILD_VERSION, LMUX_PROJECT_NAME);
}

int lmux_supervisor_main(int argc, char **argv)
{
    struct supervisor s;
    const char *worker_path = "/proc/self/exe";
    const char *storage_arg = NULL;
    const char *storage_source = "executable";
    char storage_err[160];
    int i;
    memset(&s, 0, sizeof(s));
    lmux_carrier_init(&s.carrier);
    lmux_link_endpoint_init(&s.outer, LMUX_LINK_ROLE_SUPERVISOR);
    s.outer.local_caps |= LMUX_LINK_CAP_BOOT_INFO | LMUX_LINK_CAP_BOOT_UPLOAD |
                          LMUX_LINK_CAP_BOOT_MANAGED_WORKER |
                          LMUX_LINK_CAP_BOOT_SUPERVISOR_CONTROL;
    s.worker_pid = -1;
    s.worker_data_fd = -1;
    s.worker_ctrl_fd = -1;
    s.upload_fd = -1;
    s.managed_phase = MANAGED_UNMANAGED;
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
        } else if (strcmp(argv[i], "--storage-root") == 0) {
            if (++i >= argc)
                return usage(stderr), 1;
            storage_arg = argv[i];
            storage_source = "explicit";
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
    storage_err[0] = 0;
    if (lmux_supervisor_resolve_exe_path(s.active_supervisor_path,
                                         sizeof(s.active_supervisor_path),
                                         storage_err, sizeof(storage_err)) < 0) {
        fprintf(stderr, "lmux supervisor: cannot resolve executable path: %s\n",
                storage_err[0] ? storage_err : "use absolute executable path");
        cleanup_supervisor(&s);
        return 1;
    }
    if (storage_arg) {
        if (lmux_supervisor_validate_storage_root(storage_arg, storage_err,
                                                  sizeof(storage_err)) < 0) {
            fprintf(stderr, "lmux supervisor: invalid --storage-root: %s\n",
                    storage_err[0] ? storage_err : storage_arg);
            cleanup_supervisor(&s);
            return 1;
        }
        if (strlen(storage_arg) >= sizeof(s.storage_root)) {
            fprintf(stderr, "lmux supervisor: storage root path too long\n");
            cleanup_supervisor(&s);
            return 1;
        }
        strcpy(s.storage_root, storage_arg);
    } else {
        if (lmux_supervisor_resolve_default_storage_root(s.storage_root,
                                                         sizeof(s.storage_root),
                                                         storage_err,
                                                         sizeof(storage_err)) < 0 ||
            lmux_supervisor_validate_storage_root(s.storage_root, storage_err,
                                                  sizeof(storage_err)) < 0) {
            fprintf(stderr, "lmux supervisor: cannot resolve storage root: %s\n",
                    storage_err[0] ? storage_err : "use --storage-root");
            cleanup_supervisor(&s);
            return 1;
        }
    }
    if (lmux_supervisor_build_workers_dir(s.storage_root, s.workers_dir,
                                          sizeof(s.workers_dir)) < 0) {
        fprintf(stderr, "lmux supervisor: workers path too long\n");
        cleanup_supervisor(&s);
        return 1;
    }
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
    sup_log(&s, 1, "supervisor storage root=%s source=%s",
            s.storage_root, storage_source);
    sup_log(&s, 1, "supervisor active executable=%s",
            s.active_supervisor_path);
    if (managed_startup_select_worker(&s) > 0)
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
