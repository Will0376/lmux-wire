#ifndef LMUX_SUPERVISOR_H
#define LMUX_SUPERVISOR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct lmux_link_endpoint;

#define LMUX_SUP_MSG_LEN 16
#define LMUX_SUP_PROXY_CAP 4096
#define LMUX_SUP_WORKER_DATA_FD 3
#define LMUX_SUP_WORKER_CTRL_FD 4

enum lmux_supervisor_msg_type {
    LMUX_SUP_LINK_DOWN = 1,
    LMUX_SUP_LINK_DOWN_ACK = 2,
    LMUX_SUP_LINK_UP = 3,
    LMUX_SUP_LINK_UP_ACK = 4,
    LMUX_SUP_SHUTDOWN = 5
};

struct lmux_supervisor_msg {
    uint8_t type;
    uint64_t link_epoch;
};

struct lmux_proxy_ring {
    uint8_t buf[LMUX_SUP_PROXY_CAP];
    size_t r;
    size_t w;
    size_t len;
};

int lmux_supervisor_main(int argc, char **argv);

int lmux_supervisor_msg_encode(uint8_t out[LMUX_SUP_MSG_LEN],
                               const struct lmux_supervisor_msg *msg);
int lmux_supervisor_msg_decode(const uint8_t in[LMUX_SUP_MSG_LEN],
                               struct lmux_supervisor_msg *msg);

void lmux_proxy_ring_clear(struct lmux_proxy_ring *r);
size_t lmux_proxy_ring_len(const struct lmux_proxy_ring *r);
size_t lmux_proxy_ring_space(const struct lmux_proxy_ring *r);
size_t lmux_proxy_ring_write(struct lmux_proxy_ring *r,
                             const uint8_t *buf, size_t len);
ssize_t lmux_proxy_read_into(int fd, struct lmux_proxy_ring *r);
ssize_t lmux_proxy_write_from(int fd, struct lmux_proxy_ring *r);
int lmux_supervisor_drain_fd(int fd);

pid_t lmux_supervisor_spawn_worker(const char *worker_path, int data_fd,
                                   int ctrl_fd, int verbose);
size_t lmux_supervisor_link_push_worker(struct lmux_link_endpoint *link,
                                        const uint8_t *buf, size_t len);
size_t lmux_supervisor_link_pull_worker(struct lmux_link_endpoint *link,
                                        uint8_t *buf, size_t len);

#endif
