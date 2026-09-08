/*
 * AUTO-GENERATED OS ABSTRACTION LAYER SKELETON
 *
 * Purpose:
 *   This header is a starting point for Linux NIC driver porting.
 *   Replace placeholder types and APIs with target-OS definitions.
 *
 * Notes:
 *   - Keep this layer small and explicit.
 *   - Prefer wrapping target-OS primitives rather than leaking them upward.
 *   - Use this as the single place where OS-specific adaptation happens.
 */

#ifndef NIC_OSAL_NETDEV_H
#define NIC_OSAL_NETDEV_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*nic_osal_netdev_open_t)(nic_osal_netdev_t *ndev);
typedef int (*nic_osal_netdev_stop_t)(nic_osal_netdev_t *ndev);
typedef int (*nic_osal_netdev_xmit_t)(nic_osal_netdev_t *ndev,
                                            nic_osal_pkt_buf_t *pkt);

typedef struct nic_osal_netdev_ops {
    nic_osal_netdev_open_t open;
    nic_osal_netdev_stop_t stop;
    nic_osal_netdev_xmit_t xmit;
} nic_osal_netdev_ops_t;

int  nic_os_netdev_register(nic_osal_device_t *dev,
                                  nic_osal_netdev_t *ndev,
                                  const nic_osal_netdev_ops_t *ops);
void nic_os_netdev_unregister(nic_osal_netdev_t *ndev);

void nic_os_netif_start_queue(nic_osal_netdev_t *ndev);
void nic_os_netif_stop_queue(nic_osal_netdev_t *ndev);
void nic_os_netif_wake_queue(nic_osal_netdev_t *ndev);

nic_osal_pkt_buf_t *nic_os_pkt_buf_alloc(size_t size);
void nic_os_pkt_buf_free(nic_osal_pkt_buf_t *pkt);
void *nic_os_pkt_buf_put(nic_osal_pkt_buf_t *pkt, size_t len);

int nic_os_netif_rx(nic_osal_netdev_t *ndev, nic_osal_pkt_buf_t *pkt);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_NETDEV_H */
