#include "host.h"
#include "link_endpoint.h"

size_t v2_host_link_push_lmux(struct lmux_link_endpoint *link,
                              const uint8_t *buf, size_t len)
{
    return lmux_link_transport_write(link, buf, len);
}

size_t v2_host_link_pull_lmux(struct lmux_link_endpoint *link,
                              uint8_t *buf, size_t len)
{
    return lmux_link_transport_read(link, buf, len);
}
