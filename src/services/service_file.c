#include "services.h"
#include "lmux.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

void v2_file_session_reset(struct v2_file_session *fs)
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
    v2_app_log(a, 1, "lmux2 file service %s PUT path=%s size=%llu%s",
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
    v2_app_log(a, 1, "lmux2 file service %s GET path=%s size=%llu",
               s->name, path, (unsigned long long)fs->expected_size);
}

static void file_handle_cwd_request(struct v2_service services[V2_MAX_SERVICES],
                                    struct v2_service *s,
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
        pty = v2_service_find_name(services, "console1");
    else if (p[0] == 2)
        pty = v2_service_find_name(services, "console2");
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

static void file_handle_record(struct v2_app *a,
                               struct v2_service services[V2_MAX_SERVICES],
                               struct v2_service *s,
                               uint8_t type, const uint8_t *p, uint32_t len)
{
    struct v2_file_session *fs = &s->file;
    if (fs->state == V2_FILE_IDLE) {
        if (type == LXF_T_PUT_REQUEST)
            file_handle_put_request(a, s, p, len);
        else if (type == LXF_T_GET_REQUEST)
            file_handle_get_request(a, s, p, len);
        else if (type == LXF_T_CWD_REQUEST)
            file_handle_cwd_request(services, s, p, len);
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

static int file_stream_can_send(const struct lmux2_conn *conn, uint64_t stream_id)
{
    const struct lmux2_stream *st = lmux2_find_stream_const(conn, stream_id);
    if (!st)
        return 0;
    return st->state == LMUX2_STREAM_OPEN || st->state == LMUX2_STREAM_REMOTE_CLOSED;
}

static void file_pump_output(struct lmux2_conn *conn, struct v2_service *s)
{
    struct v2_file_session *fs = &s->file;
    while (fs->out_pos < fs->out_len &&
           file_stream_can_send(conn, s->active_stream_id) &&
           lmux2_stream_tx_len(conn, s->active_stream_id) < LMUX2_STREAM_TX_CAP) {
        size_t n = fs->out_len - fs->out_pos;
        size_t space = LMUX2_STREAM_TX_CAP - lmux2_stream_tx_len(conn, s->active_stream_id);
        size_t w;
        if (n > space)
            n = space;
        w = lmux2_stream_write(conn, s->active_stream_id, fs->outbuf + fs->out_pos, n);
        if (w == 0)
            break;
        fs->out_pos += w;
    }
    if (fs->out_pos == fs->out_len)
        fs->out_pos = fs->out_len = 0;
}

int v2_file_service_step(struct v2_app *a, struct lmux2_conn *conn,
                         struct v2_service services[V2_MAX_SERVICES],
                         struct v2_service *s)
{
    struct v2_file_session *fs = &s->file;
    int progress = 0;
    if (!s->active_stream_id)
        return 0;
    if (fs->out_pos < fs->out_len)
        progress = 1;
    file_pump_output(conn, s);
    if (fs->out_len != fs->out_pos)
        return progress;

    if (fs->state == V2_FILE_GET) {
        uint8_t p[LXF_MAX_PAYLOAD];
        ssize_t r = read(fs->fd, p, sizeof(p));
        if (r > 0) {
            sha256_update(&fs->sha, p, (size_t)r);
            fs->count += (uint64_t)r;
            if (file_queue_record(fs, LXF_T_DATA, p, (uint32_t)r) == 0)
                file_pump_output(conn, s);
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
                file_pump_output(conn, s);
            v2_app_log(a, 1, "lmux2 file service %s GET DONE bytes=%llu",
                       s->name, (unsigned long long)fs->count);
            return 1;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            file_send_error(fs, LXF_E_IO, strerror(errno));
            file_pump_output(conn, s);
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
            if (lmux2_stream_rx_len(conn, s->active_stream_id) == 0)
                break;
            n = want - fs->in_len;
            if (fs->in_len + n > sizeof(fs->inbuf)) {
                file_send_error(fs, LXF_E_PROTOCOL, "record too large");
                progress = 1;
                break;
            }
            n = lmux2_stream_read(conn, s->active_stream_id,
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
            file_handle_record(a, services, s, type, fs->inbuf + LXF_HEADER_LEN, len);
            fs->in_len = 0;
            fs->need_len = 0;
            file_pump_output(conn, s);
            progress = 1;
        } else {
            break;
        }
    }
    return progress;
}
