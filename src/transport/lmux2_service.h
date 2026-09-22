#ifndef LMUX2_SERVICE_H
#define LMUX2_SERVICE_H

#include <stdint.h>

#define V2_MAX_SERVICES 8
#define V2_MAX_DESIRED 8
#define V2_MAX_BINDINGS 8
#define V2_NAME_MAX 32
#define V2_PATH_MAX 108

typedef uint64_t v2_service_id_t;
typedef uint64_t v2_pending_token_t;

enum v2_service_type {
    V2_SERVICE_UNIX_RENDEZVOUS = 1,
    V2_SERVICE_PTY = 2,
    V2_SERVICE_FILE = 3
};

enum v2_event {
    V2_EVENT_PENDING_CLIENT = 1,
    V2_EVENT_PENDING_CLEARED = 2,
    V2_EVENT_DELETED = 3,
    V2_EVENT_WINSIZE = 4
};

static inline const char *v2_service_type_name(enum v2_service_type type)
{
    switch (type) {
    case V2_SERVICE_UNIX_RENDEZVOUS: return "unix-rendezvous";
    case V2_SERVICE_PTY: return "pty";
    case V2_SERVICE_FILE: return "file";
    }
    return "?";
}

#endif
