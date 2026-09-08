/*
 * osal_netdev.c
 *
 * FreeBSD backing for the OSAL network-device and packet-buffer surface.
 *
 * Packet buffers are mbufs. The pkt_buf handle carries the mbuf on os_private
 * and mirrors its data pointer and length so callers can use the generic
 * accessors without knowing about mbuf internals.
 *
 * Interface registration is deliberately not implemented here: under iflib the
 * driver owns ifnet creation via iflib_device_register(), and duplicating that
 * in the OSAL would create a second, conflicting attach path. These entry
 * points return ENOSYS so the absence is explicit at the call site.
 */

#include "osal_netdev.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>

static MALLOC_DEFINE(M_NIC_OSAL_PKT, "nic_osal_pkt", "NIC OSAL packet buffer");

int
nic_os_netdev_register(nic_osal_device_t *dev,
                       nic_osal_netdev_t *ndev,
                       const nic_osal_netdev_ops_t *ops)
{
    (void)dev;
    (void)ndev;
    (void)ops;

    /* iflib owns ifnet attach; see file header. */
    return ENOSYS;
}

void
nic_os_netdev_unregister(nic_osal_netdev_t *ndev)
{
    (void)ndev;
}

void
nic_os_netif_start_queue(nic_osal_netdev_t *ndev)
{
    (void)ndev;
}

void
nic_os_netif_stop_queue(nic_osal_netdev_t *ndev)
{
    (void)ndev;
}

void
nic_os_netif_wake_queue(nic_osal_netdev_t *ndev)
{
    (void)ndev;
}

nic_osal_pkt_buf_t *
nic_os_pkt_buf_alloc(size_t size)
{
    nic_osal_pkt_buf_t *pkt;
    struct mbuf *m;

    if (size == 0 || size > MJUM16BYTES)
        return (NULL);

    m = m_getjcl(M_NOWAIT, MT_DATA, M_PKTHDR, (int)MAX(size, MCLBYTES));
    if (m == NULL)
        return (NULL);

    pkt = malloc(sizeof(*pkt), M_NIC_OSAL_PKT, M_NOWAIT | M_ZERO);
    if (pkt == NULL) {
        m_freem(m);
        return (NULL);
    }

    pkt->os_private = m;
    pkt->data = mtod(m, void *);
    pkt->capacity = size;
    pkt->len = 0;

    return (pkt);
}

void
nic_os_pkt_buf_free(nic_osal_pkt_buf_t *pkt)
{
    if (pkt == NULL)
        return;

    if (pkt->os_private != NULL)
        m_freem((struct mbuf *)pkt->os_private);

    free(pkt, M_NIC_OSAL_PKT);
}

void *
nic_os_pkt_buf_put(nic_osal_pkt_buf_t *pkt, size_t len)
{
    struct mbuf *m;
    void *tail;

    if (pkt == NULL || pkt->os_private == NULL)
        return (NULL);
    if (pkt->len + len > pkt->capacity)
        return (NULL);

    m = (struct mbuf *)pkt->os_private;
    tail = (char *)pkt->data + pkt->len;
    pkt->len += len;

    m->m_len = (int)pkt->len;
    m->m_pkthdr.len = (int)pkt->len;

    return (tail);
}

int
nic_os_netif_rx(nic_osal_netdev_t *ndev, nic_osal_pkt_buf_t *pkt)
{
    (void)ndev;
    (void)pkt;

    /* iflib delivers RX via its own rxd_pkt_get path; see file header. */
    return ENOSYS;
}
