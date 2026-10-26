#ifndef LMUX_CARRIER_MANAGER_H
#define LMUX_CARRIER_MANAGER_H

#include "carrier.h"

#include <stddef.h>
#include <stdint.h>

#define LMUX_CARRIER_MAX_ENDPOINTS 2
#define LMUX_CARRIER_NAME_MAX 31

enum lmux_carrier_mode {
    LMUX_CARRIER_MODE_RAW = 1,
    LMUX_CARRIER_MODE_DIRTY = 2
};

enum lmux_carrier_endpoint_state {
    LMUX_CARRIER_ENDPOINT_DOWN = 0,
    LMUX_CARRIER_ENDPOINT_LISTENING,
    LMUX_CARRIER_ENDPOINT_OPEN,
    LMUX_CARRIER_ENDPOINT_FAILED,
    LMUX_CARRIER_ENDPOINT_RELEASED
};

struct lmux_carrier_endpoint {
    int configured;
    char name[LMUX_CARRIER_NAME_MAX + 1];
    struct lmux_carrier carrier;
    enum lmux_carrier_mode mode;
    int administratively_released;
    unsigned long long standby_rx_bytes;
    unsigned long long standby_discard_events;
};

struct lmux_carrier_manager {
    struct lmux_carrier_endpoint endpoint[LMUX_CARRIER_MAX_ENDPOINTS];
    int active_index;
    unsigned configured_count;
    uint64_t generation;
};

void lmux_carrier_manager_init(struct lmux_carrier_manager *m);
void lmux_carrier_manager_cleanup(struct lmux_carrier_manager *m);

int lmux_carrier_name_valid(const char *name);
int lmux_carrier_manager_add(struct lmux_carrier_manager *m,
                             const char *name, int make_active,
                             struct lmux_carrier_endpoint **out);
int lmux_carrier_manager_rename(struct lmux_carrier_manager *m,
                                struct lmux_carrier_endpoint *ep,
                                const char *name);
struct lmux_carrier_endpoint *
lmux_carrier_manager_find(struct lmux_carrier_manager *m, const char *name);
const struct lmux_carrier_endpoint *
lmux_carrier_manager_find_const(const struct lmux_carrier_manager *m,
                                const char *name);
struct lmux_carrier_endpoint *
lmux_carrier_manager_at(struct lmux_carrier_manager *m, unsigned index);
const struct lmux_carrier_endpoint *
lmux_carrier_manager_at_const(const struct lmux_carrier_manager *m,
                              unsigned index);
struct lmux_carrier_endpoint *
lmux_carrier_manager_active_endpoint(struct lmux_carrier_manager *m);
const struct lmux_carrier_endpoint *
lmux_carrier_manager_active_endpoint_const(const struct lmux_carrier_manager *m);
struct lmux_carrier *
lmux_carrier_manager_active(struct lmux_carrier_manager *m);
const struct lmux_carrier *
lmux_carrier_manager_active_const(const struct lmux_carrier_manager *m);
int lmux_carrier_manager_is_active(const struct lmux_carrier_manager *m,
                                   const struct lmux_carrier_endpoint *ep);
int lmux_carrier_manager_set_active(struct lmux_carrier_manager *m,
                                    const char *name,
                                    enum lmux_carrier_mode mode,
                                    uint64_t generation);
int lmux_carrier_manager_release(struct lmux_carrier_manager *m,
                                 const char *name);
int lmux_carrier_manager_resume(struct lmux_carrier_manager *m,
                                const char *name);

const char *lmux_carrier_mode_name(enum lmux_carrier_mode mode);
const char *lmux_carrier_type_name(const struct lmux_carrier_endpoint *ep);
enum lmux_carrier_endpoint_state
lmux_carrier_endpoint_state(const struct lmux_carrier_endpoint *ep);
const char *lmux_carrier_endpoint_state_name(
    enum lmux_carrier_endpoint_state state);

/* Parse the bounded NAME=VALUE syntax used by named carrier CLI options. */
int lmux_carrier_named_value_parse(const char *spec,
                                   char name[LMUX_CARRIER_NAME_MAX + 1],
                                   const char **value);

/* Start configured listener endpoints. Active endpoint errors are fatal. */
int lmux_carrier_manager_start(struct lmux_carrier_manager *m);

/*
 * Try all reopenable configured endpoints. Return a local table-index bitmask
 * for newly opened endpoints. Array indices are never carrier identities.
 */
unsigned lmux_carrier_manager_try_open(struct lmux_carrier_manager *m,
                                       long long now_ms);
/* As above, but leave local table slots selected by skip_mask closed. */
unsigned lmux_carrier_manager_try_open_except(struct lmux_carrier_manager *m,
                                              long long now_ms,
                                              unsigned skip_mask);

#endif
