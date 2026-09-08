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

#ifndef NIC_OSAL_DMA_H
#define NIC_OSAL_DMA_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* nic_osal_dma_addr_t, nic_osal_dma_buffer_t and nic_osal_dma_map_t
 * are defined in osal_types.h. */

typedef enum nic_osal_dma_dir {
    NIC_OSAL_DMA_TO_DEVICE,
    NIC_OSAL_DMA_FROM_DEVICE,
    NIC_OSAL_DMA_BIDIRECTIONAL,
} nic_osal_dma_dir_t;

int nic_os_dma_alloc(nic_osal_device_t *dev,
                     size_t size,
                     nic_osal_dma_buffer_t *buf);

void nic_os_dma_free(nic_osal_device_t *dev,
                     nic_osal_dma_buffer_t *buf);

int nic_os_dma_map(nic_osal_device_t *dev,
                   void *ptr,
                   size_t size,
                   nic_osal_dma_dir_t dir,
                   nic_osal_dma_map_t *map);

void nic_os_dma_unmap(nic_osal_device_t *dev,
                      nic_osal_dma_map_t *map,
                      nic_osal_dma_dir_t dir);

void nic_os_dma_sync_for_cpu(nic_osal_device_t *dev,
                             nic_osal_dma_map_t *map,
                             nic_osal_dma_dir_t dir);

void nic_os_dma_sync_for_device(nic_osal_device_t *dev,
                                nic_osal_dma_map_t *map,
                                nic_osal_dma_dir_t dir);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_DMA_H */
