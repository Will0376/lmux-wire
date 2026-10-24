#include "carrier_manager.h"

#include <errno.h>
#include <string.h>

static int ascii_alnum(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

void lmux_carrier_manager_init(struct lmux_carrier_manager *m)
{
    unsigned i;
    memset(m, 0, sizeof(*m));
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++) {
        lmux_carrier_init(&m->endpoint[i].carrier);
        m->endpoint[i].mode = LMUX_CARRIER_MODE_RAW;
    }
    m->active_index = -1;
    m->generation = 1;
}

void lmux_carrier_manager_cleanup(struct lmux_carrier_manager *m)
{
    unsigned i;
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++)
        lmux_carrier_cleanup(&m->endpoint[i].carrier);
}

int lmux_carrier_name_valid(const char *name)
{
    size_t i, len;
    if (!name || !name[0]) {
        errno = EINVAL;
        return 0;
    }
    len = strlen(name);
    if (len > LMUX_CARRIER_NAME_MAX) {
        errno = ENAMETOOLONG;
        return 0;
    }
    if (!ascii_alnum((unsigned char)name[0])) {
        errno = EINVAL;
        return 0;
    }
    for (i = 1; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!ascii_alnum(ch) && ch != '-' && ch != '_' && ch != '.') {
            errno = EINVAL;
            return 0;
        }
    }
    return 1;
}

struct lmux_carrier_endpoint *
lmux_carrier_manager_at(struct lmux_carrier_manager *m, unsigned index)
{
    if (!m || index >= LMUX_CARRIER_MAX_ENDPOINTS ||
        !m->endpoint[index].configured) {
        errno = ENOENT;
        return NULL;
    }
    return &m->endpoint[index];
}

const struct lmux_carrier_endpoint *
lmux_carrier_manager_at_const(const struct lmux_carrier_manager *m,
                              unsigned index)
{
    if (!m || index >= LMUX_CARRIER_MAX_ENDPOINTS ||
        !m->endpoint[index].configured) {
        errno = ENOENT;
        return NULL;
    }
    return &m->endpoint[index];
}

struct lmux_carrier_endpoint *
lmux_carrier_manager_find(struct lmux_carrier_manager *m, const char *name)
{
    unsigned i;
    if (!m || !name) {
        errno = EINVAL;
        return NULL;
    }
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++)
        if (m->endpoint[i].configured &&
            strcmp(m->endpoint[i].name, name) == 0)
            return &m->endpoint[i];
    errno = ENOENT;
    return NULL;
}

const struct lmux_carrier_endpoint *
lmux_carrier_manager_find_const(const struct lmux_carrier_manager *m,
                                const char *name)
{
    unsigned i;
    if (!m || !name) {
        errno = EINVAL;
        return NULL;
    }
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++)
        if (m->endpoint[i].configured &&
            strcmp(m->endpoint[i].name, name) == 0)
            return &m->endpoint[i];
    errno = ENOENT;
    return NULL;
}

int lmux_carrier_manager_add(struct lmux_carrier_manager *m,
                             const char *name, int make_active,
                             struct lmux_carrier_endpoint **out)
{
    unsigned i;
    struct lmux_carrier_endpoint *ep = NULL;
    if (!m || !lmux_carrier_name_valid(name))
        return -1;
    if (lmux_carrier_manager_find(m, name)) {
        errno = EEXIST;
        return -1;
    }
    if (make_active && m->active_index >= 0) {
        errno = EBUSY;
        return -1;
    }
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++) {
        if (!m->endpoint[i].configured) {
            ep = &m->endpoint[i];
            break;
        }
    }
    if (!ep) {
        errno = ENOSPC;
        return -1;
    }
    ep->configured = 1;
    strcpy(ep->name, name);
    m->configured_count++;
    if (make_active)
        m->active_index = (int)i;
    if (out)
        *out = ep;
    return 0;
}

int lmux_carrier_manager_rename(struct lmux_carrier_manager *m,
                                struct lmux_carrier_endpoint *ep,
                                const char *name)
{
    struct lmux_carrier_endpoint *other;
    if (!m || !ep || !ep->configured || !lmux_carrier_name_valid(name))
        return -1;
    other = lmux_carrier_manager_find(m, name);
    if (other && other != ep) {
        errno = EEXIST;
        return -1;
    }
    strcpy(ep->name, name);
    return 0;
}

struct lmux_carrier_endpoint *
lmux_carrier_manager_active_endpoint(struct lmux_carrier_manager *m)
{
    if (!m || m->active_index < 0 ||
        m->active_index >= LMUX_CARRIER_MAX_ENDPOINTS) {
        errno = ENOENT;
        return NULL;
    }
    return lmux_carrier_manager_at(m, (unsigned)m->active_index);
}

const struct lmux_carrier_endpoint *
lmux_carrier_manager_active_endpoint_const(const struct lmux_carrier_manager *m)
{
    if (!m || m->active_index < 0 ||
        m->active_index >= LMUX_CARRIER_MAX_ENDPOINTS) {
        errno = ENOENT;
        return NULL;
    }
    return lmux_carrier_manager_at_const(m, (unsigned)m->active_index);
}

struct lmux_carrier *
lmux_carrier_manager_active(struct lmux_carrier_manager *m)
{
    struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_active_endpoint(m);
    return ep ? &ep->carrier : NULL;
}

const struct lmux_carrier *
lmux_carrier_manager_active_const(const struct lmux_carrier_manager *m)
{
    const struct lmux_carrier_endpoint *ep =
        lmux_carrier_manager_active_endpoint_const(m);
    return ep ? &ep->carrier : NULL;
}

int lmux_carrier_manager_is_active(const struct lmux_carrier_manager *m,
                                   const struct lmux_carrier_endpoint *ep)
{
    return m && ep && m->active_index >= 0 &&
        m->active_index < LMUX_CARRIER_MAX_ENDPOINTS &&
        ep == &m->endpoint[m->active_index];
}

int lmux_carrier_manager_set_active(struct lmux_carrier_manager *m,
                                    const char *name,
                                    enum lmux_carrier_mode mode,
                                    uint64_t generation)
{
    struct lmux_carrier_endpoint *ep;
    ptrdiff_t index;
    if (!m || !name || generation == 0 ||
        (mode != LMUX_CARRIER_MODE_RAW &&
         mode != LMUX_CARRIER_MODE_DIRTY)) {
        errno = EINVAL;
        return -1;
    }
    ep = lmux_carrier_manager_find(m, name);
    if (!ep)
        return -1;
    index = ep - m->endpoint;
    if (index < 0 || index >= LMUX_CARRIER_MAX_ENDPOINTS) {
        errno = EINVAL;
        return -1;
    }
    ep->mode = mode;
    m->active_index = (int)index;
    m->generation = generation;
    return 0;
}

const char *lmux_carrier_mode_name(enum lmux_carrier_mode mode)
{
    switch (mode) {
    case LMUX_CARRIER_MODE_RAW: return "raw";
    case LMUX_CARRIER_MODE_DIRTY: return "dirty";
    }
    return "unknown";
}

const char *lmux_carrier_type_name(const struct lmux_carrier_endpoint *ep)
{
    if (!ep || !ep->configured)
        return "none";
    switch (ep->carrier.kind) {
    case LMUX_CARRIER_TTY_PATH: return "tty";
    case LMUX_CARRIER_FD: return "fd";
    case LMUX_CARRIER_UNIX_LISTEN: return "unix-listen-test";
    case LMUX_CARRIER_UNIX_CONNECT: return "unix-connect-test";
    case LMUX_CARRIER_DISPATCH_CONNECT: return "uart-dispatch";
    case LMUX_CARRIER_NONE: return "unconfigured";
    }
    return "unknown";
}

enum lmux_carrier_endpoint_state
lmux_carrier_endpoint_state(const struct lmux_carrier_endpoint *ep)
{
    if (!ep || !ep->configured || ep->carrier.kind == LMUX_CARRIER_NONE)
        return LMUX_CARRIER_ENDPOINT_DOWN;
    if (lmux_carrier_fd(&ep->carrier) >= 0)
        return LMUX_CARRIER_ENDPOINT_OPEN;
    if (lmux_carrier_listen_fd(&ep->carrier) >= 0)
        return LMUX_CARRIER_ENDPOINT_LISTENING;
    if (ep->carrier.last_errno != 0)
        return LMUX_CARRIER_ENDPOINT_FAILED;
    return LMUX_CARRIER_ENDPOINT_DOWN;
}

const char *lmux_carrier_endpoint_state_name(
    enum lmux_carrier_endpoint_state state)
{
    switch (state) {
    case LMUX_CARRIER_ENDPOINT_DOWN: return "down";
    case LMUX_CARRIER_ENDPOINT_LISTENING: return "listening";
    case LMUX_CARRIER_ENDPOINT_OPEN: return "open";
    case LMUX_CARRIER_ENDPOINT_FAILED: return "failed";
    }
    return "unknown";
}

int lmux_carrier_named_value_parse(const char *spec,
                                   char name[LMUX_CARRIER_NAME_MAX + 1],
                                   const char **value)
{
    const char *eq;
    size_t len;
    if (!spec || !name || !value || !(eq = strchr(spec, '=')) ||
        eq == spec || !eq[1]) {
        errno = EINVAL;
        return -1;
    }
    len = (size_t)(eq - spec);
    if (len > LMUX_CARRIER_NAME_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, spec, len);
    name[len] = '\0';
    if (!lmux_carrier_name_valid(name))
        return -1;
    *value = eq + 1;
    return 0;
}

int lmux_carrier_manager_start(struct lmux_carrier_manager *m)
{
    unsigned i;
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++) {
        struct lmux_carrier_endpoint *ep =
            lmux_carrier_manager_at(m, i);
        struct lmux_carrier *c;
        if (!ep)
            continue;
        c = &ep->carrier;
        if (!lmux_carrier_is_configured(c))
            continue;
        if (lmux_carrier_start(c) < 0) {
            c->last_errno = errno;
            c->open_fail_count++;
            if (lmux_carrier_manager_is_active(m, ep))
                return -1;
        }
    }
    return 0;
}

unsigned lmux_carrier_manager_try_open(struct lmux_carrier_manager *m,
                                       long long now_ms)
{
    return lmux_carrier_manager_try_open_except(m, now_ms, 0);
}

unsigned lmux_carrier_manager_try_open_except(struct lmux_carrier_manager *m,
                                              long long now_ms,
                                              unsigned skip_mask)
{
    unsigned i, opened = 0;
    for (i = 0; i < LMUX_CARRIER_MAX_ENDPOINTS; i++) {
        struct lmux_carrier_endpoint *ep =
            lmux_carrier_manager_at(m, i);
        struct lmux_carrier *c;
        if (!ep || (skip_mask & (1u << i)))
            continue;
        c = &ep->carrier;
        if (!lmux_carrier_is_configured(c))
            continue;
        if (lmux_carrier_try_open(c, now_ms) > 0)
            opened |= 1u << i;
    }
    return opened;
}
