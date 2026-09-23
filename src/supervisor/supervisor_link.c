#include "supervisor.h"
#include "link_endpoint.h"

size_t lmux_supervisor_link_push_worker(struct lmux_link_endpoint *link,
                                        const uint8_t *buf, size_t len)
{
    return lmux_link_transport_write(link, buf, len);
}

size_t lmux_supervisor_link_pull_worker(struct lmux_link_endpoint *link,
                                        uint8_t *buf, size_t len)
{
    return lmux_link_transport_read(link, buf, len);
}
