#include "supervisor.h"
#include "boot_protocol.h"
#include "carrier.h"
#include "carrier_manager.h"
#include "dirty.h"
#include "dispatch_ipc.h"
#include "lmc1.h"
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
#define LMUX_LMC1_COMPLETED_CACHE 2

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
    struct lmux_carrier_manager carriers;
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
    struct lmux_lmc1_raw_io lmc_io[LMUX_CARRIER_MAX_ENDPOINTS];
    struct lmux_lmc1_dirty_io lmc_dirty_io[LMUX_CARRIER_MAX_ENDPOINTS];
    unsigned long long carrier_open_fail_reported[LMUX_CARRIER_MAX_ENDPOINTS];
    short carrier_last_revents[LMUX_CARRIER_MAX_ENDPOINTS];
    struct lmux_dirty dirty;
    int dirty_active;
    unsigned carrier_failure_hold_mask;
    uint8_t carrier_boot_id[LMUX_LMC1_BOOT_ID_LEN];
    struct {
        int valid;
        struct lmux_lmc1_msg request;
        enum lmux_lmc1_codec codec;
        long long deadline_ms;
    } carrier_pending;
    struct lmux_lmc1_msg carrier_completed[LMUX_LMC1_COMPLETED_CACHE];
    unsigned carrier_completed_next;
    struct lmux_lmc1_msg carrier_admin_completed[LMUX_LMC1_COMPLETED_CACHE];
    unsigned carrier_admin_completed_next;
    struct {
        int valid;
        unsigned endpoint;
        enum lmux_lmc1_codec codec;
        struct lmux_lmc1_msg request;
    } carrier_commit_drain;
    int carrier_committed_wait_outer;
    int carrier_wait_host_outer_hello;
};

static struct supervisor *global_sup;

size_t lmux_supervisor_memory_footprint(void)
{
    return sizeof(struct supervisor);
}

static void sup_log(struct supervisor *s, int verbose, const char *fmt, ...);
static void drain_outer_transport_to_worker_ring(struct supervisor *s);
static int write_full_fd(int fd, const uint8_t *buf, size_t len);
static int ensure_workers_dir(struct supervisor *s);
static void close_worker_fds(struct supervisor *s);
static int spawn_worker(struct supervisor *s);
static void maybe_begin_committed_outer(struct supervisor *s);
static void feed_lmc_physical(struct supervisor *s, unsigned endpoint,
                              int allow_outer);
static void flush_lmc_physical(struct supervisor *s, unsigned endpoint);
static void maybe_apply_deferred_carrier_commit(struct supervisor *s);

static struct lmux_carrier *active_carrier(struct supervisor *s)
{
    return lmux_carrier_manager_active(&s->carriers);
}

static enum lmux_carrier_mode active_carrier_mode(const struct supervisor *s)
{
    const struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_active_endpoint_const(&s->carriers);
    return ep ? ep->mode : LMUX_CARRIER_MODE_RAW;
}

static int lmc_any_output(const struct supervisor *s, unsigned endpoint)
{
    return endpoint < LMUX_CARRIER_MAX_ENDPOINTS &&
        (lmux_lmc1_raw_has_output(&s->lmc_io[endpoint]) ||
         lmux_lmc1_dirty_has_output(&s->lmc_dirty_io[endpoint]));
}

static int dirty_has_work(const struct lmux_dirty *d)
{
    unsigned i;
    if (!d)
        return 0;
    if (d->output_pos < d->output_len || d->pending_fail)
        return 1;
    for (i = 0; i < LMUX_DIRTY_WINDOW_MAX; i++)
        if (d->tx[i].used)
            return 1;
    for (i = 0; i < LMUX_DIRTY_CONTROL_MAX; i++)
        if (d->control[i].used)
            return 1;
    return 0;
}

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

static void random_bytes(uint8_t *out, size_t len)
{
    size_t off = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        while (off < len) {
            ssize_t r = read(fd, out + off, len - off);
            if (r > 0) {
                off += (size_t)r;
                continue;
            }
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        close(fd);
    }
    while (off < len) {
        uint64_t v = ((uint64_t)(uint32_t)getpid() << 32) ^
            (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)out ^ off;
        size_t n = len - off < sizeof(v) ? len - off : sizeof(v);
        memcpy(out + off, &v, n);
        off += n;
    }
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
    unsigned active = (unsigned)s->carriers.active_index;
    if (lmux_carrier_fd(active_carrier(s)) < 0)
        return;
    sup_log(s, 1, "supervisor physical carrier lost");
    if (s->dirty_active && active_carrier_mode(s) == LMUX_CARRIER_MODE_DIRTY) {
        lmux_dirty_peer_lost(&s->dirty);
        if (active < LMUX_CARRIER_MAX_ENDPOINTS)
            s->carrier_failure_hold_mask |= 1u << active;
    }
    lmux_carrier_mark_lost(active_carrier(s), now);
    if (s->outer_enabled)
        lmux_link_endpoint_down(&s->outer);
    s->carrier_wait_host_outer_hello = 0;
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
        s->worker_ctrl_fd < 0 || lmux_carrier_fd(active_carrier(s)) < 0)
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
    s->carrier_wait_host_outer_hello = 0;
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
    if (lmux_carrier_fd(active_carrier(s)) >= 0)
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
    if (events & LMUX_LINK_EV_PEER_HELLO) {
        if (s->carrier_wait_host_outer_hello) {
            s->carrier_wait_host_outer_hello = 0;
            sup_log(s, 1,
                    "carrier adoption confirmed by host OUTER HELLO generation=%llu",
                    (unsigned long long)s->carriers.generation);
        }
        sup_log(s, 1, "outer peer HELLO received peer_conn=%llu",
                (unsigned long long)s->outer.peer_conn_id);
    }
    if (events & LMUX_LINK_EV_HELLO_ACK_QUEUED)
        sup_log(s, 1, "outer HELLO_ACK queued local_conn=%llu ack_peer=%llu",
                (unsigned long long)s->outer.local_conn_id,
                (unsigned long long)s->outer.pending_ack_peer_conn_id);
    if (events & LMUX_LINK_EV_HELLO_ACK_COMMITTED)
        sup_log(s, 1, "outer HELLO_ACK committed local_conn=%llu peer_conn=%llu",
                (unsigned long long)s->outer.local_conn_id,
                (unsigned long long)s->outer.peer_conn_id);
    if (events & LMUX_LINK_EV_PEER_HELLO_ACK)
        sup_log(s, 1, "outer peer HELLO_ACK received peer_conn=%llu ack_local=%llu",
                (unsigned long long)s->outer.peer_conn_id,
                (unsigned long long)s->outer.local_conn_id);
    if (events & LMUX_LINK_EV_ESTABLISHED) {
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
    feed_lmc_physical(s, (unsigned)s->carriers.active_index,
                      !s->carrier_committed_wait_outer);
}

static void flush_outer_physical(struct supervisor *s)
{
    if (active_carrier_mode(s) == LMUX_CARRIER_MODE_DIRTY) {
        while (lmux_carrier_fd(active_carrier(s)) >= 0) {
            const uint8_t *wire;
            size_t wire_len;
            struct lmux_carrier_io io;
            unsigned events = 0;
            int peek;
            if (s->carrier_commit_drain.valid &&
                !(s->dirty.output_pos > 0 &&
                  s->dirty.output_pos < s->dirty.output_len))
                return;
            if (!s->dirty_active || lmux_dirty_failed(&s->dirty)) {
                physical_lost(s);
                return;
            }
            if (s->dirty.output_pos == s->dirty.output_len &&
                lmc_any_output(s, (unsigned)s->carriers.active_index))
                return;
            if (s->dirty.output_pos == s->dirty.output_len) {
                if (s->carrier_wait_host_outer_hello)
                    return;
                if (!lmux_link_has_pending_output(&s->outer))
                    (void)lmux_link_build_output(&s->outer);
                if (lmux_link_has_pending_output(&s->outer)) {
                    size_t n = lmux_link_output_len(&s->outer);
                    if (n > LMUX_DIRTY_PACKET_MAX)
                        n = LMUX_DIRTY_PACKET_MAX;
                    if (lmux_dirty_submit(&s->dirty,
                            lmux_link_output_ptr(&s->outer), n,
                            (uint64_t)now_ms(), NULL) == 0) {
                        lmux_link_output_consume_events(&s->outer, n, &events);
                        handle_outer_events(s, events);
                    } else if (errno != EAGAIN) {
                        physical_lost(s);
                        return;
                    }
                }
            }
            peek = lmux_dirty_output_peek(&s->dirty, (uint64_t)now_ms(),
                                          &wire, &wire_len);
            if (peek < 0) {
                physical_lost(s);
                return;
            }
            if (peek == 0)
                return;
            io = lmux_carrier_write(active_carrier(s), wire, wire_len);
            if (io.status == LMUX_CARRIER_IO_PROGRESS) {
                if (lmux_dirty_output_consume(&s->dirty,
                                               (size_t)io.n) < 0) {
                    physical_lost(s);
                    return;
                }
                if (s->carrier_commit_drain.valid &&
                    s->dirty.output_pos == s->dirty.output_len)
                    return;
                continue;
            }
            if (io.status == LMUX_CARRIER_IO_WOULD_BLOCK)
                return;
            physical_lost(s);
            return;
        }
        return;
    }
    while (lmux_carrier_fd(active_carrier(s)) >= 0) {
        struct lmux_carrier_io io;
        unsigned active = (unsigned)s->carriers.active_index;
        if (s->carrier_wait_host_outer_hello)
            return;
        if (s->carrier_commit_drain.valid &&
            !(s->outer.out_pos > 0 && s->outer.out_pos < s->outer.out_len))
            return;
        if (!lmux_link_has_pending_output(&s->outer) &&
            active < LMUX_CARRIER_MAX_ENDPOINTS &&
            lmc_any_output(s, active))
            return;
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
        io = lmux_carrier_write(active_carrier(s), lmux_link_output_ptr(&s->outer),
                                lmux_link_output_len(&s->outer));
        if (io.status == LMUX_CARRIER_IO_PROGRESS) {
            unsigned events = 0;
            lmux_link_output_consume_events(&s->outer, (size_t)io.n, &events);
            handle_outer_events(s, events);
            if (s->carrier_commit_drain.valid &&
                !lmux_link_has_pending_output(&s->outer))
                return;
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
    unsigned opened = lmux_carrier_manager_try_open(&s->carriers, now_ms());
    int id;
    for (id = 0; id < LMUX_CARRIER_MAX_ENDPOINTS; id++) {
        struct lmux_carrier_endpoint *ep =
            lmux_carrier_manager_at(&s->carriers, (unsigned)id);
        if (!ep)
            continue;
        if (ep->carrier.open_fail_count !=
            s->carrier_open_fail_reported[id]) {
            unsigned long long count = ep->carrier.open_fail_count;
            const struct lmux_tty_diagnostic *diag =
                &ep->carrier.tty_diagnostic;
            s->carrier_open_fail_reported[id] = count;
            if (count <= 3 || (count & (count - 1)) == 0) {
                sup_log(s, 0,
                        "supervisor %s carrier open failed path=%s count=%llu errno=%d (%s) tty_policy=%s stage=%s syscall_errno=%d (%s)",
                        ep->name,
                        ep->carrier.path ? ep->carrier.path : "-",
                        count, ep->carrier.last_errno,
                        strerror(ep->carrier.last_errno),
                        lmux_tty_policy_name(ep->carrier.tty_policy),
                        lmux_tty_config_stage_name(diag->stage),
                        diag->syscall_errno,
                        diag->syscall_errno ?
                        strerror(diag->syscall_errno) : "none");
                if (diag->stage == LMUX_TTY_CONFIG_VALIDATE)
                    sup_log(s, 0,
                            "supervisor %s carrier tty validation mismatch=0x%x ispeed=%s ospeed=%s clocal=%s cread=%s csize=%s control=%s iflag=%s lflag=%s oflag=%s expected ispeed=%lu ospeed=%lu iflag=0x%lx oflag=0x%lx cflag=0x%lx lflag=0x%lx actual ispeed=%lu ospeed=%lu iflag=0x%lx oflag=0x%lx cflag=0x%lx lflag=0x%lx",
                            ep->name, diag->validation_mismatch,
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_ISPEED ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_OSPEED ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_CLOCAL ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_CREAD ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_CSIZE ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_CONTROL ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_IFLAG ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_LFLAG ? "bad" : "ok",
                            diag->validation_mismatch &
                            LMUX_TTY_MISMATCH_OFLAG ? "bad" : "ok",
                            diag->expected_ispeed,
                            diag->expected_ospeed,
                            diag->expected_iflag,
                            diag->expected_oflag,
                            diag->expected_cflag,
                            diag->expected_lflag,
                            diag->actual_ispeed,
                            diag->actual_ospeed,
                            diag->actual_iflag,
                            diag->actual_oflag,
                            diag->actual_cflag,
                            diag->actual_lflag);
            }
        }
        if (!(opened & (1u << id)))
            continue;
        sup_log(s, 1, "supervisor %s carrier opened %s%s",
                ep->name,
                lmux_carrier_label(&ep->carrier),
                lmux_carrier_manager_is_active(&s->carriers, ep) ?
                " active" : " standby");
        if (!lmux_carrier_manager_is_active(&s->carriers, ep))
            continue;
        if (s->outer_enabled && !s->carrier_committed_wait_outer &&
            !(s->carrier_failure_hold_mask & (1u << (unsigned)id)) &&
            !(ep->mode == LMUX_CARRIER_MODE_DIRTY && s->dirty_active &&
              lmux_dirty_failed(&s->dirty)))
            begin_outer(s);
        else if (!s->outer_enabled)
            maybe_send_link_up(s);
        maybe_begin_committed_outer(s);
    }
}

static void accept_physical(struct supervisor *s,
                            unsigned id)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&s->carriers, id);
    int rc = ep ? lmux_carrier_accept(&ep->carrier) : -1;
    if (rc > 0) {
        sup_log(s, 1, "supervisor %s carrier accepted%s",
                ep->name,
                lmux_carrier_manager_is_active(&s->carriers, ep) ?
                " active" : " standby");
        if (!lmux_carrier_manager_is_active(&s->carriers, ep))
            return;
        if (s->outer_enabled && !s->carrier_committed_wait_outer &&
            !(s->carrier_failure_hold_mask & (1u << id)) &&
            !(ep->mode == LMUX_CARRIER_MODE_DIRTY && s->dirty_active &&
              lmux_dirty_failed(&s->dirty)))
            begin_outer(s);
        else if (!s->outer_enabled)
            maybe_send_link_up(s);
        maybe_begin_committed_outer(s);
    }
}

static void drain_standby_physical(struct supervisor *s,
                                   unsigned id)
{
    feed_lmc_physical(s, id, 0);
}

struct supervisor_lmc_context {
    struct supervisor *s;
    unsigned endpoint;
    enum lmux_lmc1_codec codec;
};

static int supervisor_carrier_busy(const struct supervisor *s)
{
    return s->upload_id != 0 || s->supervisor_restart_pending ||
        s->managed_phase == MANAGED_TESTING ||
        lmux_link_boot_tx_len(&s->outer) != 0 ||
        lmux_link_boot_rx_len(&s->outer) != 0;
}

static struct lmux_lmc1_msg *completed_for_nonce(
    struct supervisor *s, const uint8_t nonce[LMUX_LMC1_NONCE_LEN])
{
    unsigned i;
    for (i = 0; i < LMUX_LMC1_COMPLETED_CACHE; i++)
        if (s->carrier_completed[i].type == LMUX_LMC1_COMMITTED &&
            lmux_lmc1_nonce_equal(s->carrier_completed[i].nonce, nonce))
            return &s->carrier_completed[i];
    return NULL;
}

static int completed_matches(const struct lmux_lmc1_msg *done,
                             const struct lmux_lmc1_msg *request,
                             int require_cookie)
{
    if (!done || !request ||
        !lmux_lmc1_nonce_equal(done->nonce, request->nonce) ||
        strcmp(done->name, request->name) != 0 ||
        done->mode != request->mode || done->generation == 0 ||
        request->generation != done->generation - 1 ||
        memcmp(done->boot_id, request->boot_id,
               LMUX_LMC1_BOOT_ID_LEN) != 0)
        return 0;
    return !require_cookie ||
        memcmp(done->cookie, request->cookie,
               LMUX_LMC1_COOKIE_LEN) == 0;
}

static struct lmux_lmc1_msg *admin_completed_for_nonce(
    struct supervisor *s, const uint8_t nonce[LMUX_LMC1_NONCE_LEN])
{
    unsigned i;
    for (i = 0; i < LMUX_LMC1_COMPLETED_CACHE; i++)
        if ((s->carrier_admin_completed[i].type == LMUX_LMC1_RELEASED ||
             s->carrier_admin_completed[i].type == LMUX_LMC1_RESUMED) &&
            lmux_lmc1_nonce_equal(s->carrier_admin_completed[i].nonce,
                                  nonce))
            return &s->carrier_admin_completed[i];
    return NULL;
}

static int admin_completed_matches(const struct lmux_lmc1_msg *done,
                                   const struct lmux_lmc1_msg *request)
{
    uint8_t expected = request->type == LMUX_LMC1_RELEASE ?
        LMUX_LMC1_RELEASED : LMUX_LMC1_RESUMED;
    return done && request && done->type == expected &&
        lmux_lmc1_nonce_equal(done->nonce, request->nonce) &&
        strcmp(done->name, request->name) == 0 &&
        done->generation == request->generation &&
        memcmp(done->boot_id, request->boot_id,
               LMUX_LMC1_BOOT_ID_LEN) == 0;
}

static int queue_lmc_response(struct supervisor *s, unsigned endpoint,
                              enum lmux_lmc1_codec codec,
                              const struct lmux_lmc1_msg *msg)
{
    if (endpoint >= LMUX_CARRIER_MAX_ENDPOINTS)
        return -1;
    if (codec == LMUX_LMC1_CODEC_DIRTY_BOOTSTRAP) {
        if (lmux_lmc1_dirty_queue(&s->lmc_dirty_io[endpoint], msg) < 0)
            return -1;
    } else if (lmux_lmc1_raw_queue(&s->lmc_io[endpoint], msg) < 0) {
        return -1;
    }
    sup_log(s, 1, "LMC1 %s queued endpoint=%s generation=%llu status=%s",
            lmux_lmc1_type_name(msg->type),
            s->carriers.endpoint[endpoint].name,
            (unsigned long long)msg->generation,
            lmux_lmc1_status_name(msg->status));
    return 0;
}

static void queue_lmc_error(struct supervisor *s, unsigned endpoint,
                            enum lmux_lmc1_codec codec,
                            const struct lmux_lmc1_msg *request,
                            uint16_t status)
{
    struct lmux_lmc1_msg reply = *request;
    const struct lmux_carrier_endpoint *active =
        lmux_carrier_manager_active_endpoint_const(&s->carriers);
    reply.type = LMUX_LMC1_ERROR;
    reply.status = status;
    reply.generation = s->carriers.generation;
    reply.deadline_ms = 0;
    memcpy(reply.boot_id, s->carrier_boot_id, sizeof(reply.boot_id));
    if (active) {
        reply.mode = active->mode;
        strcpy(reply.name, active->name);
    }
    (void)queue_lmc_response(s, endpoint, codec, &reply);
}

static void retire_outer_for_carrier_commit(struct supervisor *s)
{
    lmux_link_endpoint_down(&s->outer);
    clear_proxy(s);
    managed_pause_confirm_timer(s);
    if (s->worker_pid > 0 && s->worker_ctrl_fd >= 0 &&
        s->state != SUP_LINK_DOWN && s->state != SUP_DOWN_BARRIER &&
        s->state != SUP_WORKER_DOWN) {
        if (send_msg(s, LMUX_SUP_LINK_DOWN, s->link_epoch) == 0) {
            s->state = SUP_DOWN_BARRIER;
            sup_log(s, 1, "supervisor LINK_DOWN carrier-commit epoch=%llu",
                    (unsigned long long)s->link_epoch);
        } else {
            schedule_restart(s, now_ms());
        }
    } else if (s->state != SUP_WORKER_DOWN) {
        s->state = SUP_LINK_DOWN;
    }
}

static int active_physical_record_partial(const struct supervisor *s)
{
    if (active_carrier_mode(s) == LMUX_CARRIER_MODE_DIRTY && s->dirty_active)
        return s->dirty.output_pos > 0 &&
            s->dirty.output_pos < s->dirty.output_len;
    return s->outer.out_pos > 0 && s->outer.out_pos < s->outer.out_len;
}

static void apply_carrier_commit(struct supervisor *s, unsigned endpoint,
                                 enum lmux_lmc1_codec codec,
                                 const struct lmux_lmc1_msg *msg)
{
    struct lmux_carrier_endpoint *old_active =
        lmux_carrier_manager_active_endpoint(&s->carriers);
    struct lmux_carrier_endpoint *candidate =
        lmux_carrier_manager_find(&s->carriers, msg->name);
    struct lmux_lmc1_msg reply = *msg;
    long long now = now_ms();
    if (!candidate || endpoint >= LMUX_CARRIER_MAX_ENDPOINTS)
        return;
    reply.type = LMUX_LMC1_COMMITTED;
    reply.status = LMUX_LMC1_STATUS_OK;
    reply.generation = s->carriers.generation + 1;
    reply.deadline_ms = 0;
    memcpy(reply.boot_id, s->carrier_boot_id, sizeof(reply.boot_id));
    if (lmux_carrier_manager_set_active(&s->carriers, msg->name,
                                        msg->mode, reply.generation) < 0)
        return;
    s->carrier_failure_hold_mask &= ~(1u << endpoint);
    if (old_active && old_active != candidate &&
        old_active->carrier.kind == LMUX_CARRIER_DISPATCH_CONNECT) {
        lmux_carrier_dispatch_abort(&old_active->carrier);
        (void)lmux_carrier_dispatch_control(&old_active->carrier,
                                            LMUX_DISPATCH_IPC_IDLE);
    }
    if (msg->mode == LMUX_CARRIER_MODE_DIRTY) {
        uint64_t host_session, target_session;
        uint32_t baud = candidate->carrier.serial.baud ?
            candidate->carrier.serial.baud : 115200;
        lmux_lmc1_dirty_session_ids(s->carrier_boot_id, reply.generation,
                                    msg->name, &host_session,
                                    &target_session);
        lmux_dirty_init(&s->dirty, target_session, host_session, 0, 0,
                        baud, (uint64_t)now);
        s->dirty_active = 1;
    } else {
        memset(&s->dirty, 0, sizeof(s->dirty));
        s->dirty_active = 0;
    }
    retire_outer_for_carrier_commit(s);
    s->carrier_pending.valid = 0;
    s->carrier_completed[s->carrier_completed_next] = reply;
    s->carrier_completed_next =
        (s->carrier_completed_next + 1) % LMUX_LMC1_COMPLETED_CACHE;
    s->carrier_committed_wait_outer = 1;
    s->carrier_wait_host_outer_hello = 0;
    (void)queue_lmc_response(s, endpoint, codec, &reply);
    lmux_lmc1_raw_reset_rx(&s->lmc_io[endpoint]);
    lmux_lmc1_dirty_reset_rx(&s->lmc_dirty_io[endpoint]);
    sup_log(s, 0, "carrier committed active=%s mode=%s generation=%llu",
            msg->name, lmux_carrier_mode_name(msg->mode),
            (unsigned long long)reply.generation);
}

static void maybe_apply_deferred_carrier_commit(struct supervisor *s)
{
    struct lmux_lmc1_msg request;
    unsigned endpoint;
    enum lmux_lmc1_codec codec;
    if (!s->carrier_commit_drain.valid || active_physical_record_partial(s))
        return;
    request = s->carrier_commit_drain.request;
    endpoint = s->carrier_commit_drain.endpoint;
    codec = s->carrier_commit_drain.codec;
    s->carrier_commit_drain.valid = 0;
    apply_carrier_commit(s, endpoint, codec, &request);
}

static int supervisor_lmc_admin(struct supervisor *s,
                                const struct supervisor_lmc_context *ctx,
                                const struct lmux_lmc1_msg *msg)
{
    struct lmux_carrier_endpoint *rx_ep;
    struct lmux_carrier_endpoint *ep;
    struct lmux_lmc1_msg reply;
    struct lmux_lmc1_msg *done;
    int releasing = msg->type == LMUX_LMC1_RELEASE;

    if (!releasing && msg->type != LMUX_LMC1_RESUME)
        return 0;
    rx_ep = lmux_carrier_manager_at(&s->carriers, ctx->endpoint);
    if (!rx_ep || !lmux_carrier_manager_is_active(&s->carriers, rx_ep)) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_BAD_REQUEST);
        return 1;
    }
    if (msg->generation != s->carriers.generation ||
        memcmp(msg->boot_id, s->carrier_boot_id,
               LMUX_LMC1_BOOT_ID_LEN) != 0) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_STALE);
        return 1;
    }
    ep = lmux_carrier_manager_find(&s->carriers, msg->name);
    if (!ep) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_UNKNOWN_CARRIER);
        return 1;
    }
    if (s->carrier_pending.valid || s->carrier_commit_drain.valid ||
        s->carrier_committed_wait_outer) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_BUSY);
        return 1;
    }
    done = admin_completed_for_nonce(s, msg->nonce);
    if (done) {
        if (admin_completed_matches(done, msg))
            (void)queue_lmc_response(s, ctx->endpoint, ctx->codec, done);
        else
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_STALE);
        return 1;
    }
    if (releasing && lmux_carrier_manager_is_active(&s->carriers, ep)) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_BUSY);
        return 1;
    }
    if ((releasing ?
         lmux_carrier_manager_release(&s->carriers, msg->name) :
         lmux_carrier_manager_resume(&s->carriers, msg->name)) < 0) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        errno == EBUSY ? LMUX_LMC1_STATUS_BUSY :
                        LMUX_LMC1_STATUS_UNAVAILABLE);
        return 1;
    }
    {
        unsigned endpoint = (unsigned)(ep - s->carriers.endpoint);
        if (!lmux_carrier_manager_is_active(&s->carriers, ep)) {
            lmux_lmc1_raw_init(&s->lmc_io[endpoint]);
            lmux_lmc1_dirty_init(&s->lmc_dirty_io[endpoint]);
        }
    }
    reply = *msg;
    reply.type = releasing ? LMUX_LMC1_RELEASED : LMUX_LMC1_RESUMED;
    reply.status = LMUX_LMC1_STATUS_OK;
    reply.deadline_ms = 0;
    reply.mode = ep->mode;
    memcpy(reply.boot_id, s->carrier_boot_id, sizeof(reply.boot_id));
    s->carrier_admin_completed[s->carrier_admin_completed_next] = reply;
    s->carrier_admin_completed_next =
        (s->carrier_admin_completed_next + 1) % LMUX_LMC1_COMPLETED_CACHE;
    (void)queue_lmc_response(s, ctx->endpoint, ctx->codec, &reply);
    sup_log(s, 0, "carrier endpoint %s name=%s generation=%llu",
            releasing ? "released" : "resumed", ep->name,
            (unsigned long long)s->carriers.generation);
    return 1;
}

static void supervisor_lmc_control(void *opaque,
                                   const struct lmux_lmc1_msg *msg)
{
    struct supervisor_lmc_context *ctx = opaque;
    struct supervisor *s = ctx->s;
    struct lmux_carrier_endpoint *rx_ep =
        lmux_carrier_manager_at(&s->carriers, ctx->endpoint);
    struct lmux_carrier_endpoint *candidate;
    struct lmux_lmc1_msg reply;
    struct lmux_lmc1_msg *done;
    long long now = now_ms();

    if (!rx_ep)
        return;
    sup_log(s, 1, "LMC1 %s received endpoint=%s generation=%llu",
            lmux_lmc1_type_name(msg->type), rx_ep->name,
            (unsigned long long)msg->generation);
    if (msg->type == LMUX_LMC1_QUERY) {
        const struct lmux_carrier_endpoint *active =
            lmux_carrier_manager_active_endpoint_const(&s->carriers);
        memset(&reply, 0, sizeof(reply));
        reply.type = LMUX_LMC1_QUERY_REPLY;
        reply.mode = active ? active->mode : LMUX_CARRIER_MODE_RAW;
        reply.generation = s->carriers.generation;
        memcpy(reply.nonce, msg->nonce, sizeof(reply.nonce));
        memcpy(reply.boot_id, s->carrier_boot_id, sizeof(reply.boot_id));
        strcpy(reply.name, active ? active->name : rx_ep->name);
        (void)queue_lmc_response(s, ctx->endpoint, ctx->codec, &reply);
        return;
    }
    if (supervisor_lmc_admin(s, ctx, msg))
        return;
    if (msg->type == LMUX_LMC1_PREPARE) {
        done = completed_for_nonce(s, msg->nonce);
        if (done) {
            if (completed_matches(done, msg, 0) &&
                strcmp(rx_ep->name, done->name) == 0)
                (void)queue_lmc_response(s, ctx->endpoint, ctx->codec, done);
            else
                queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                                LMUX_LMC1_STATUS_STALE);
            return;
        }
        candidate = lmux_carrier_manager_find(&s->carriers, msg->name);
        if (!candidate) {
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_UNKNOWN_CARRIER);
            return;
        }
        if (candidate != rx_ep || lmux_carrier_fd(&candidate->carrier) < 0) {
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_UNAVAILABLE);
            return;
        }
        if (msg->mode != LMUX_CARRIER_MODE_RAW &&
            msg->mode != LMUX_CARRIER_MODE_DIRTY) {
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_UNSUPPORTED_MODE);
            return;
        }
        if ((msg->mode == LMUX_CARRIER_MODE_DIRTY &&
             ctx->codec != LMUX_LMC1_CODEC_DIRTY_BOOTSTRAP) ||
            (msg->mode == LMUX_CARRIER_MODE_RAW &&
             ctx->codec != LMUX_LMC1_CODEC_RAW)) {
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_BAD_REQUEST);
            return;
        }
        if (memcmp(msg->boot_id, s->carrier_boot_id,
                   LMUX_LMC1_BOOT_ID_LEN) != 0 ||
            msg->generation != s->carriers.generation) {
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_STALE);
            return;
        }
        if (supervisor_carrier_busy(s)) {
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_BUSY);
            return;
        }
        if (s->carrier_pending.valid &&
            now > s->carrier_pending.deadline_ms)
            s->carrier_pending.valid = 0;
        if (s->carrier_pending.valid) {
            if (!lmux_lmc1_nonce_equal(s->carrier_pending.request.nonce,
                                       msg->nonce) ||
                strcmp(s->carrier_pending.request.name, msg->name) != 0 ||
                s->carrier_pending.request.mode != msg->mode) {
                queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                                LMUX_LMC1_STATUS_BUSY);
                return;
            }
        } else {
            s->carrier_pending.valid = 1;
            s->carrier_pending.request = *msg;
            s->carrier_pending.codec = ctx->codec;
            random_bytes(s->carrier_pending.request.cookie,
                         LMUX_LMC1_COOKIE_LEN);
            s->carrier_pending.deadline_ms =
                now + LMUX_LMC1_DEFAULT_DEADLINE_MS;
        }
        reply = s->carrier_pending.request;
        reply.type = LMUX_LMC1_READY;
        reply.status = LMUX_LMC1_STATUS_OK;
        reply.deadline_ms = LMUX_LMC1_DEFAULT_DEADLINE_MS;
        memcpy(reply.boot_id, s->carrier_boot_id, sizeof(reply.boot_id));
        (void)queue_lmc_response(s, ctx->endpoint, ctx->codec, &reply);
        return;
    }
    if (msg->type != LMUX_LMC1_COMMIT)
        return;
    done = completed_for_nonce(s, msg->nonce);
    if (done) {
        if (completed_matches(done, msg, 1) &&
            strcmp(rx_ep->name, done->name) == 0)
            (void)queue_lmc_response(s, ctx->endpoint, ctx->codec, done);
        else
            queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                            LMUX_LMC1_STATUS_STALE);
        return;
    }
    if (!s->carrier_pending.valid ||
        !lmux_lmc1_nonce_equal(s->carrier_pending.request.nonce, msg->nonce) ||
        strcmp(s->carrier_pending.request.name, msg->name) != 0 ||
        s->carrier_pending.request.mode != msg->mode ||
        msg->generation != s->carriers.generation ||
        memcmp(msg->boot_id, s->carrier_boot_id,
               LMUX_LMC1_BOOT_ID_LEN) != 0) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_STALE);
        return;
    }
    if (now > s->carrier_pending.deadline_ms) {
        s->carrier_pending.valid = 0;
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_EXPIRED);
        return;
    }
    if (memcmp(msg->cookie, s->carrier_pending.request.cookie,
               LMUX_LMC1_COOKIE_LEN) != 0) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_BAD_COOKIE);
        return;
    }
    candidate = lmux_carrier_manager_find(&s->carriers, msg->name);
    if (!candidate || candidate != rx_ep ||
        lmux_carrier_fd(&candidate->carrier) < 0) {
        queue_lmc_error(s, ctx->endpoint, ctx->codec, msg,
                        LMUX_LMC1_STATUS_UNAVAILABLE);
        return;
    }

    if (candidate == lmux_carrier_manager_active_endpoint(&s->carriers) &&
        active_physical_record_partial(s)) {
        s->carrier_commit_drain.valid = 1;
        s->carrier_commit_drain.endpoint = ctx->endpoint;
        s->carrier_commit_drain.codec = ctx->codec;
        s->carrier_commit_drain.request = *msg;
        sup_log(s, 1, "LMC1 COMMIT waiting for old physical record boundary");
        return;
    }
    apply_carrier_commit(s, ctx->endpoint, ctx->codec, msg);
}

static void supervisor_lmc_outer(void *opaque, const uint8_t *encoded,
                                 size_t len)
{
    struct supervisor_lmc_context *ctx = opaque;
    struct supervisor *s = ctx->s;
    unsigned events = 0;
    uint8_t delim = 0;
    (void)lmux_link_input_bytes(&s->outer, encoded, len, &events);
    (void)lmux_link_input_bytes(&s->outer, &delim, 1, &events);
    handle_outer_events(s, events);
    drain_outer_boot_requests(s);
    drain_outer_transport_to_worker_ring(s);
}

static int supervisor_dirty_deliver(struct supervisor *s)
{
    const uint8_t *data;
    size_t len;
    uint32_t seq;
    int rc;
    while ((rc = lmux_dirty_receive_peek(&s->dirty, &data, &len, &seq)) > 0) {
        unsigned events = 0;
        (void)seq;
        if (lmux_link_input_bytes(&s->outer, data, len, &events) < 0)
            return -1;
        handle_outer_events(s, events);
        drain_outer_boot_requests(s);
        drain_outer_transport_to_worker_ring(s);
        if (lmux_dirty_receive_consume(&s->dirty) < 0)
            return -1;
    }
    return rc < 0 ? -1 : 0;
}

static void feed_lmc_physical(struct supervisor *s, unsigned endpoint,
                              int allow_outer)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&s->carriers, endpoint);
    struct supervisor_lmc_context ctx = {
        s, endpoint, LMUX_LMC1_CODEC_RAW
    };
    uint8_t buf[256];
    unsigned budget = 8;
    if (!ep)
        return;
    while (budget-- > 0) {
        struct lmux_carrier_io r =
            lmux_carrier_read(&ep->carrier, buf, sizeof(buf));
        if (r.status == LMUX_CARRIER_IO_PROGRESS) {
            size_t i;
            int discarded = 0;
            (void)allow_outer;
            /* A COMMIT record can change endpoint authority in the middle of
             * this read.  Re-evaluate routing after every record delimiter so
             * an immediately following OUTER HELLO is not treated as standby
             * noise (and bytes preceding COMMIT cannot become OUTER data). */
            for (i = 0; i < (size_t)r.n; i++) {
                int current_allow =
                    lmux_carrier_manager_is_active(&s->carriers, ep) &&
                    !(s->carrier_failure_hold_mask & (1u << endpoint)) &&
                    !s->carrier_committed_wait_outer &&
                    !s->carrier_commit_drain.valid;
                int dirty_mode = current_allow &&
                    ep->mode == LMUX_CARRIER_MODE_DIRTY && s->dirty_active;
                int raw_mode = current_allow && !dirty_mode;
                int raw_boundary = s->lmc_io[endpoint].rx_len == 0 &&
                    !s->lmc_io[endpoint].discard_until_delim;
                int boot_was = lmux_lmc1_dirty_receiving(
                    &s->lmc_dirty_io[endpoint]);
                int boot_feed = !raw_mode || boot_was || raw_boundary;
                int data_was = dirty_mode && s->dirty.parser.in_record;
                if (!current_allow) {
                    ep->standby_rx_bytes++;
                    discarded = 1;
                }
                if (!boot_was && !data_was &&
                    lmux_lmc1_raw_feed(&s->lmc_io[endpoint], buf + i, 1,
                                       current_allow && !dirty_mode,
                                       supervisor_lmc_control,
                                       supervisor_lmc_outer, &ctx) < 0) {
                    if (current_allow)
                        physical_lost(s);
                    return;
                }
                /* Active RAW admits a bootstrap envelope only between whole
                 * COBS records.  Scanning inside encoded OUTER data can steal
                 * an accidental ASCII prefix and corrupt the byte stream. */
                if (raw_mode && !boot_was && !raw_boundary &&
                    s->lmc_dirty_io[endpoint].prefix_match) {
                    const char *prefix = LMUX_LMC1_DIRTY_PREFIX;
                    unsigned match =
                        s->lmc_dirty_io[endpoint].prefix_match;
                    if (buf[i] == (uint8_t)prefix[match])
                        boot_feed = 1;
                    else
                        lmux_lmc1_dirty_reset_rx(
                            &s->lmc_dirty_io[endpoint]);
                }
                ctx.codec = LMUX_LMC1_CODEC_DIRTY_BOOTSTRAP;
                if (boot_feed && lmux_lmc1_dirty_feed(
                        &s->lmc_dirty_io[endpoint], buf + i, 1,
                        supervisor_lmc_control, &ctx) < 0) {
                    if (current_allow)
                        physical_lost(s);
                    return;
                }
                ctx.codec = LMUX_LMC1_CODEC_RAW;
                if (!boot_was && lmux_lmc1_dirty_receiving(
                        &s->lmc_dirty_io[endpoint]))
                    lmux_lmc1_raw_reset_rx(&s->lmc_io[endpoint]);
                if (dirty_mode) {
                    if (lmux_dirty_feed(&s->dirty, buf + i, 1,
                                        (uint64_t)now_ms()) < 0 ||
                        supervisor_dirty_deliver(s) < 0) {
                        physical_lost(s);
                        return;
                    }
                    if (!data_was && s->dirty.parser.in_record)
                        lmux_lmc1_raw_reset_rx(&s->lmc_io[endpoint]);
                }
            }
            if (discarded)
                ep->standby_discard_events++;
            continue;
        }
        if (r.status == LMUX_CARRIER_IO_LOST ||
            r.status == LMUX_CARRIER_IO_FATAL) {
            if (lmux_carrier_manager_is_active(&s->carriers, ep))
                physical_lost(s);
            else
                lmux_carrier_mark_lost(&ep->carrier, now_ms());
        }
        return;
    }
}

static void maybe_begin_committed_outer(struct supervisor *s)
{
    unsigned active;
    if (!s->carrier_committed_wait_outer || s->state != SUP_LINK_DOWN)
        return;
    active = (unsigned)s->carriers.active_index;
    if (active >= LMUX_CARRIER_MAX_ENDPOINTS ||
        (s->carrier_failure_hold_mask & (1u << active)) ||
        lmc_any_output(s, active) ||
        lmux_carrier_wants_write(active_carrier(s)) ||
        lmux_carrier_fd(active_carrier(s)) < 0)
        return;
    s->carrier_committed_wait_outer = 0;
    begin_outer(s);
    /*
     * COMMITTED can be lost after the target has made the authority change.
     * In that case QUERY is only an observation and must not reset OUTER.
     * Hold the target's first replacement OUTER record until a valid host
     * HELLO proves that the host has adopted this carrier generation.  The
     * normal HELLO/HELLO_ACK exchange then remains the sole data-plane
     * synchronization; no extra LMC1 wire message is required.
     */
    s->carrier_wait_host_outer_hello = 1;
    sup_log(s, 1,
            "carrier waiting for host OUTER HELLO adoption generation=%llu",
            (unsigned long long)s->carriers.generation);
}

static void flush_lmc_physical(struct supervisor *s, unsigned endpoint)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_at(&s->carriers, endpoint);
    uint8_t completed = 0;
    int rc;
    int use_dirty;
    uint8_t tx_type;
    if (!ep || !lmc_any_output(s, endpoint))
        return;
    if (lmux_carrier_manager_is_active(&s->carriers, ep) &&
        ep->mode == LMUX_CARRIER_MODE_DIRTY && s->dirty_active &&
        s->dirty.output_pos < s->dirty.output_len)
        return;
    use_dirty = lmux_lmc1_dirty_has_output(&s->lmc_dirty_io[endpoint]) &&
        (!lmux_lmc1_raw_has_output(&s->lmc_io[endpoint]) ||
         s->lmc_dirty_io[endpoint].tx_pos != 0);
    tx_type = use_dirty ? s->lmc_dirty_io[endpoint].tx_type :
        s->lmc_io[endpoint].tx_type;
    /*
     * COMMITTED is the target's promise that the old OUTER has retired.
     * Keep it queued until the worker has acknowledged LINK_DOWN.  Otherwise
     * the host can start the replacement OUTER while this supervisor still
     * has to discard candidate data, making establishment depend on a HELLO
     * retry rather than the handover boundary itself.
     */
    if (tx_type == LMUX_LMC1_COMMITTED &&
        s->state != SUP_LINK_DOWN)
        return;
    rc = use_dirty ?
        lmux_lmc1_dirty_flush(&s->lmc_dirty_io[endpoint], &ep->carrier,
                              &completed) :
        lmux_lmc1_raw_flush(&s->lmc_io[endpoint], &ep->carrier,
                            &completed);
    if (rc < 0) {
        if (lmux_carrier_manager_is_active(&s->carriers, ep))
            physical_lost(s);
        else
            lmux_carrier_mark_lost(&ep->carrier, now_ms());
        return;
    }
    if (completed) {
        sup_log(s, 1, "LMC1 %s committed endpoint=%s",
                lmux_lmc1_type_name(completed), ep->name);
        if (completed == LMUX_LMC1_COMMITTED &&
            ep->carrier.kind == LMUX_CARRIER_DISPATCH_CONNECT &&
            lmux_carrier_dispatch_control(&ep->carrier,
                                          LMUX_DISPATCH_IPC_ACTIVE) < 0)
            physical_lost(s);
    }
    maybe_begin_committed_outer(s);
}

static int add_poll(struct pollfd *pfds, int *kind, nfds_t *n,
                    int fd, short events, int k)
{
    if (*n >= 12 || fd < 0)
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
        struct pollfd pfds[12];
        int kind[12];
        nfds_t n = 0, i;
        int endpoint_id;
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
        maybe_apply_deferred_carrier_commit(s);
        if (s->outer_enabled && s->dirty_active &&
            active_carrier_mode(s) == LMUX_CARRIER_MODE_DIRTY &&
            !(s->carrier_failure_hold_mask &
              (1u << (unsigned)s->carriers.active_index)) &&
            lmux_carrier_fd(active_carrier(s)) >= 0) {
            if (lmux_dirty_tick(&s->dirty, (uint64_t)now) < 0) {
                physical_lost(s);
                continue;
            }
            if (timeout > 100)
                timeout = 100;
        }
        if (s->carrier_pending.valid &&
            now > s->carrier_pending.deadline_ms) {
            sup_log(s, 1, "LMC1 pending transaction expired");
            s->carrier_pending.valid = 0;
        }
        maybe_begin_committed_outer(s);
        maybe_send_link_up(s);
        if (s->outer_enabled)
            maybe_finish_supervisor_restart(s);
        if (s->outer_enabled && s->outer.state == LMUX_LINK_WAIT_HELLO) {
            int tick = lmux_link_endpoint_tick(&s->outer, now);
            if (tick < 0)
                outer_session_down(s);
            else if (tick > 0 &&
                     (s->outer.hello_retry_count <= 3 ||
                      (s->outer.hello_retry_count &
                       (s->outer.hello_retry_count - 1)) == 0)) {
                const struct lmux_carrier_endpoint *ep =
                    lmux_carrier_manager_active_endpoint_const(&s->carriers);
                unsigned endpoint = (unsigned)s->carriers.active_index;
                const struct lmux_carrier *carrier = ep ? &ep->carrier : NULL;
                const struct lmux_lmc1_raw_io *raw =
                    endpoint < LMUX_CARRIER_MAX_ENDPOINTS ?
                    &s->lmc_io[endpoint] : NULL;
                sup_log(s, 1,
                        "outer HELLO retry queued local_conn=%llu carrier=%s fd=%d opens=%llu lost=%llu open_fail=%llu errno=%d rx_bytes=%llu tx_bytes=%llu revents=0x%x raw_records=%llu raw_control=%llu raw_invalid=%llu raw_partial=%u outer_rx=%llu outer_tx=%llu pending_tx=%zu peer_hello=%d ack_committed=%d crc=%llu seq=%llu resync=%llu protocol=%llu",
                        (unsigned long long)s->outer.local_conn_id,
                        ep ? ep->name : "-",
                        carrier ? lmux_carrier_fd(carrier) : -1,
                        carrier ? carrier->open_count : 0,
                        carrier ? carrier->lost_count : 0,
                        carrier ? carrier->open_fail_count : 0,
                        carrier ? carrier->last_errno : 0,
                        carrier ? carrier->rx_bytes : 0,
                        carrier ? carrier->tx_bytes : 0,
                        endpoint < LMUX_CARRIER_MAX_ENDPOINTS ?
                            (unsigned)(unsigned short)
                                s->carrier_last_revents[endpoint] : 0,
                        raw ? raw->foreign_records : 0,
                        raw ? raw->valid_control_records : 0,
                        raw ? raw->invalid_control_records : 0,
                        raw ? (unsigned)raw->rx_len : 0,
                        s->outer.rx_frames, s->outer.tx_frames,
                        lmux_link_output_len(&s->outer),
                        s->outer.peer_hello_accepted,
                        s->outer.local_ack_committed,
                        s->outer.crc_errors, s->outer.seq_errors,
                        s->outer.resync_drops, s->outer.protocol_errors);
            }
            if (timeout > 250)
                timeout = 250;
        }

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
        for (endpoint_id = 0; endpoint_id < LMUX_CARRIER_MAX_ENDPOINTS;
             endpoint_id++) {
            struct lmux_carrier_endpoint *ep =
                lmux_carrier_manager_at(&s->carriers,
                                        (unsigned)endpoint_id);
            if (!ep)
                continue;
            struct lmux_carrier *carrier = &ep->carrier;
            int active = lmux_carrier_manager_is_active(&s->carriers, ep);
            int data_active = active &&
                !(s->carrier_failure_hold_mask & (1u << endpoint_id));
            if (lmux_carrier_listen_fd(carrier) >= 0 &&
                lmux_carrier_fd(carrier) < 0)
                add_poll(pfds, kind, &n, lmux_carrier_listen_fd(carrier),
                         POLLIN, 10 + endpoint_id);
            if (lmux_carrier_fd(carrier) >= 0) {
                short ev = 0;
                if (!data_active) {
                    ev = POLLIN;
                } else if (s->outer_enabled) {
                    if (s->carrier_committed_wait_outer ||
                        s->carrier_commit_drain.valid ||
                        ((s->outer.state != LMUX_LINK_ESTABLISHED ||
                          (lmux_link_transport_rx_space(&s->outer) > 0 &&
                           lmux_link_boot_rx_space(&s->outer) > 0)) &&
                         lmux_link_can_accept_physical(&s->outer) &&
                         lmux_link_boot_rx_space(&s->outer) > 0))
                        ev |= POLLIN;
                    if ((!s->carrier_wait_host_outer_hello &&
                         lmux_link_wants_physical_write(&s->outer)) ||
                        (active && ep->mode == LMUX_CARRIER_MODE_DIRTY &&
                         s->dirty_active && dirty_has_work(&s->dirty)))
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
                if (lmc_any_output(s, (unsigned)endpoint_id))
                    ev |= POLLOUT;
                if (lmux_carrier_wants_write(carrier))
                    ev |= POLLOUT;
                add_poll(pfds, kind, &n, lmux_carrier_fd(carrier), ev,
                         20 + endpoint_id);
            }
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
            } else if (kind[i] >= 10 && kind[i] < 10 + LMUX_CARRIER_MAX_ENDPOINTS) {
                unsigned endpoint = (unsigned)(kind[i] - 10);
                if (pfds[i].revents & POLLIN)
                    accept_physical(s, endpoint);
            } else if (kind[i] >= 20 && kind[i] < 20 + LMUX_CARRIER_MAX_ENDPOINTS) {
                unsigned endpoint = (unsigned)(kind[i] - 20);
                struct lmux_carrier_endpoint *ep =
                    lmux_carrier_manager_at(&s->carriers, endpoint);
                if (!ep)
                    continue;
                s->carrier_last_revents[endpoint] = pfds[i].revents;
                if (!lmux_carrier_manager_is_active(&s->carriers, ep) ||
                    (s->carrier_failure_hold_mask & (1u << endpoint))) {
                    if (pfds[i].revents & POLLIN)
                        drain_standby_physical(s, endpoint);
                    if (pfds[i].revents & POLLOUT)
                        flush_lmc_physical(s, endpoint);
                    if ((pfds[i].revents & POLLOUT) &&
                        !lmc_any_output(s, endpoint) &&
                        lmux_carrier_flush_pending(&ep->carrier) < 0)
                        lmux_carrier_mark_lost(&ep->carrier, now_ms());
                    if (ep && lmux_carrier_fd(&ep->carrier) >= 0 &&
                        (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL))) {
                        sup_log(s, 1,
                                "supervisor %s standby carrier revents=0x%x",
                                ep->name,
                                pfds[i].revents);
                        lmux_carrier_mark_lost(&ep->carrier, now_ms());
                    }
                } else if (s->outer_enabled) {
                    if (pfds[i].revents & POLLIN)
                        feed_outer_physical(s);
                    if (pfds[i].revents & POLLOUT) {
                        if (ep->mode == LMUX_CARRIER_MODE_DIRTY &&
                            s->dirty_active) {
                            if (s->dirty.output_pos < s->dirty.output_len)
                                flush_outer_physical(s);
                            else if (lmc_any_output(s, endpoint))
                                flush_lmc_physical(s, endpoint);
                            else
                                flush_outer_physical(s);
                        } else {
                            if (!s->carrier_wait_host_outer_hello &&
                                lmux_link_has_pending_output(&s->outer))
                                flush_outer_physical(s);
                            if ((s->carrier_wait_host_outer_hello ||
                                 !lmux_link_has_pending_output(&s->outer)))
                                flush_lmc_physical(s, endpoint);
                            if (!s->carrier_wait_host_outer_hello &&
                                !lmc_any_output(s, endpoint))
                                flush_outer_physical(s);
                        }
                        if (!lmc_any_output(s, endpoint) &&
                            lmux_carrier_flush_pending(&ep->carrier) < 0)
                            physical_lost(s);
                    }
                    maybe_finish_supervisor_restart(s);
                } else {
                    if (pfds[i].revents & POLLIN) {
                        ssize_t r = lmux_proxy_read_into(lmux_carrier_fd(active_carrier(s)),
                                                         &s->phys_to_worker);
                        if (r == 0 || r == -1)
                            physical_lost(s);
                    }
                    if (pfds[i].revents & POLLOUT) {
                        ssize_t w = lmux_proxy_write_from(lmux_carrier_fd(active_carrier(s)),
                                                          &s->worker_to_phys);
                        if (w == -1)
                            physical_lost(s);
                    }
                }
                if (lmux_carrier_manager_is_active(&s->carriers, ep) &&
                    (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)))
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
    lmux_carrier_manager_cleanup(&s->carriers);
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
            "       lmuxd --supervisor [--outer] --carrier PATH [--carrier-name NAME] [--standby-carrier NAME=PATH|--standby-dispatch NAME=SOCKET] [options]\n"
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
    struct lmux_carrier_endpoint *primary_ep = NULL;
    struct lmux_carrier_endpoint *standby_ep = NULL;
    char primary_name[LMUX_CARRIER_NAME_MAX + 1] = "primary";
    unsigned standby_baud = 115200;
    int standby_baud_set = 0;
    int standby_rtscts = 0;
    char storage_err[160];
    int i;
    memset(&s, 0, sizeof(s));
    lmux_carrier_manager_init(&s.carriers);
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++) {
        lmux_lmc1_raw_init(&s.lmc_io[i]);
        lmux_lmc1_dirty_init(&s.lmc_dirty_io[i]);
    }
    random_bytes(s.carrier_boot_id, sizeof(s.carrier_boot_id));
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
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&s.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_tty_path(&primary_ep->carrier, argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-name") == 0) {
            if (++i >= argc || !lmux_carrier_name_valid(argv[i]))
                return usage(stderr), 1;
            if (primary_ep) {
                if (lmux_carrier_manager_rename(&s.carriers, primary_ep,
                                                argv[i]) < 0)
                    return usage(stderr), 1;
            } else {
                strcpy(primary_name, argv[i]);
            }
        } else if (strcmp(argv[i], "--carrier-fd") == 0) {
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&s.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_fd(&primary_ep->carrier, atoi(argv[i])) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-listen") == 0) {
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&s.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_unix_listen(&primary_ep->carrier,
                                             argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--carrier-connect") == 0) {
            if (++i >= argc || primary_ep ||
                lmux_carrier_manager_add(&s.carriers, primary_name, 1,
                                         &primary_ep) < 0 ||
                lmux_carrier_set_unix_connect(&primary_ep->carrier,
                                              argv[i]) < 0)
                return usage(stderr), 1;
        } else if (strcmp(argv[i], "--standby-carrier") == 0 ||
                   strcmp(argv[i], "--standby-carrier-fd") == 0 ||
                   strcmp(argv[i], "--standby-carrier-listen") == 0 ||
                   strcmp(argv[i], "--standby-carrier-connect") == 0 ||
                   strcmp(argv[i], "--standby-dispatch") == 0) {
            const char *option = argv[i];
            const char *value;
            char name[LMUX_CARRIER_NAME_MAX + 1];
            if (++i >= argc || standby_ep ||
                lmux_carrier_named_value_parse(argv[i], name, &value) < 0 ||
                lmux_carrier_manager_add(&s.carriers, name, 0,
                                         &standby_ep) < 0)
                return usage(stderr), 1;
            if (strcmp(option, "--standby-carrier") == 0) {
                if (lmux_carrier_set_tty_path(&standby_ep->carrier, value) < 0)
                    return usage(stderr), 1;
                lmux_carrier_set_tty_policy(&standby_ep->carrier,
                                            LMUX_TTY_POLICY_STRICT_UART);
                lmux_carrier_set_exclusive(&standby_ep->carrier, 1);
            } else if (strcmp(option, "--standby-carrier-fd") == 0) {
                if (lmux_carrier_set_fd(&standby_ep->carrier,
                                        atoi(value)) < 0)
                    return usage(stderr), 1;
            } else if (strcmp(option, "--standby-carrier-listen") == 0) {
                if (lmux_carrier_set_unix_listen(&standby_ep->carrier,
                                                 value) < 0)
                    return usage(stderr), 1;
            } else if (strcmp(option, "--standby-dispatch") == 0) {
                if (lmux_carrier_set_dispatch_connect(&standby_ep->carrier,
                                                      value) < 0)
                    return usage(stderr), 1;
                standby_ep->mode = LMUX_CARRIER_MODE_DIRTY;
            } else if (lmux_carrier_set_unix_connect(&standby_ep->carrier,
                                                     value) < 0) {
                return usage(stderr), 1;
            }
        } else if (strcmp(argv[i], "--standby-baud") == 0) {
            char *end = NULL;
            unsigned long baud;
            if (++i >= argc)
                return usage(stderr), 1;
            baud = strtoul(argv[i], &end, 10);
            if (!end || *end || baud == 0 || baud > 0xffffffffUL)
                return usage(stderr), 1;
            standby_baud = (unsigned)baud;
            standby_baud_set = 1;
        } else if (strcmp(argv[i], "--standby-rtscts") == 0) {
            standby_rtscts = 1;
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
    if (standby_ep) {
        standby_ep->carrier.serial.baud = standby_baud;
        if (standby_rtscts)
            standby_ep->carrier.serial.flow = LMUX_SERIAL_FLOW_RTSCTS;
    } else if (standby_baud_set || standby_rtscts) {
        return usage(stderr), 1;
    }
    if (!primary_ep || !lmux_carrier_is_configured(&primary_ep->carrier))
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
    if (lmux_carrier_manager_start(&s.carriers) < 0) {
        perror(lmux_carrier_label(active_carrier(&s)));
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
    try_open_physical(&s);
    if (s.outer_enabled && lmux_carrier_fd(active_carrier(&s)) >= 0 &&
        s.outer.state == LMUX_LINK_DOWN)
        begin_outer(&s);
    (void)supervisor_loop(&s);
    cleanup_supervisor(&s);
    return 0;
}
