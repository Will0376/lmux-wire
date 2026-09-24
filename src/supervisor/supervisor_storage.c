#include "supervisor.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void storage_err(char *err, size_t cap, const char *msg)
{
    if (err && cap) {
        snprintf(err, cap, "%s", msg);
    }
}

static int join_path(char *out, size_t cap, const char *dir, const char *name)
{
    int n;
    if (!out || !dir || !name || !*dir || !*name || cap == 0)
        return -1;
    if (strcmp(dir, "/") == 0)
        n = snprintf(out, cap, "/%s", name);
    else
        n = snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int lmux_supervisor_storage_root_from_exe_path(const char *exe_path,
                                               char *out, size_t cap)
{
    const char *slash;
    size_t len;
    if (!exe_path || exe_path[0] != '/' || !out || cap == 0)
        return -1;
    slash = strrchr(exe_path, '/');
    if (!slash)
        return -1;
    len = (slash == exe_path) ? 1u : (size_t)(slash - exe_path);
    if (len == 0 || len >= cap)
        return -1;
    memcpy(out, exe_path, len);
    out[len] = 0;
    return 0;
}

int lmux_supervisor_resolve_default_storage_root(char *out, size_t cap,
                                                 char *err, size_t err_cap)
{
    char exe[LMUX_SUP_STORAGE_PATH_MAX];
    if (lmux_supervisor_resolve_exe_path(exe, sizeof(exe), err, err_cap) < 0)
        return -1;
    if (lmux_supervisor_storage_root_from_exe_path(exe, out, cap) < 0) {
        storage_err(err, err_cap,
                    "cannot derive storage root from /proc/self/exe; use --storage-root");
        return -1;
    }
    return 0;
}

int lmux_supervisor_resolve_exe_path(char *out, size_t cap,
                                     char *err, size_t err_cap)
{
    ssize_t n;
    if (!out || cap == 0) {
        storage_err(err, err_cap, "executable path buffer is empty");
        return -1;
    }
    n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) {
        storage_err(err, err_cap, "cannot read /proc/self/exe");
        return -1;
    }
    if ((size_t)n >= cap - 1) {
        storage_err(err, err_cap, "/proc/self/exe path too long");
        return -1;
    }
    out[n] = 0;
    if (out[0] != '/') {
        storage_err(err, err_cap, "/proc/self/exe is not absolute");
        return -1;
    }
    return 0;
}

int lmux_supervisor_validate_storage_root(const char *root,
                                          char *err, size_t err_cap)
{
    struct stat st;
    if (!root || !*root) {
        storage_err(err, err_cap, "storage root is empty");
        return -1;
    }
    if (root[0] != '/') {
        storage_err(err, err_cap, "storage root must be an absolute path");
        return -1;
    }
    if (strlen(root) >= LMUX_SUP_STORAGE_PATH_MAX) {
        storage_err(err, err_cap, "storage root path too long");
        return -1;
    }
    if (stat(root, &st) < 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "storage root stat failed: %s",
                     strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        storage_err(err, err_cap, "storage root is not a directory");
        return -1;
    }
    return 0;
}

int lmux_supervisor_build_workers_dir(const char *root, char *out, size_t cap)
{
    return join_path(out, cap, root, "workers");
}

int lmux_supervisor_build_upload_tmp_path(const char *root,
                                          char *out, size_t cap)
{
    char workers[LMUX_SUP_STORAGE_PATH_MAX];
    if (lmux_supervisor_build_workers_dir(root, workers, sizeof(workers)) < 0)
        return -1;
    return join_path(out, cap, workers, ".upload.tmp");
}

int lmux_supervisor_build_worker_artifact_path(const char *root,
                                               const char *sha256_hex,
                                               char *out, size_t cap)
{
    char workers[LMUX_SUP_STORAGE_PATH_MAX], name[80];
    int n;
    if (!sha256_hex || strlen(sha256_hex) != 64)
        return -1;
    if (lmux_supervisor_build_workers_dir(root, workers, sizeof(workers)) < 0)
        return -1;
    n = snprintf(name, sizeof(name), "%s.lmuxd", sha256_hex);
    if (n <= 0 || (size_t)n >= sizeof(name))
        return -1;
    return join_path(out, cap, workers, name);
}
