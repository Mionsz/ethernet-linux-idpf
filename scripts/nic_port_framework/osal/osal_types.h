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

#ifndef NIC_OSAL_TYPES_H
#define NIC_OSAL_TYPES_H

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/stdint.h>
#include <sys/stddef.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t nic_osal_phys_addr_t;
typedef uintptr_t nic_osal_dma_addr_t;
typedef uintptr_t nic_osal_virt_addr_t;

typedef struct nic_osal_device {
    void *os_private;
} nic_osal_device_t;

typedef struct nic_osal_pci_device {
    void *os_private;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} nic_osal_pci_device_t;

typedef struct nic_osal_irq {
    void *os_private;
    int vector;
} nic_osal_irq_t;

typedef struct nic_osal_dma_buffer {
    void *vaddr;
    nic_osal_dma_addr_t dma_addr;
    size_t size;
    void *os_priv;
} nic_osal_dma_buffer_t;

typedef struct nic_osal_dma_map {
    nic_osal_dma_addr_t dma_addr;
    size_t size;
    void *os_priv;
} nic_osal_dma_map_t;

typedef struct nic_osal_lock {
    void *os_private;
} nic_osal_lock_t;

typedef struct nic_osal_mutex {
    void *os_private;
} nic_osal_mutex_t;

typedef struct nic_osal_netdev {
    void *os_private;
    char name[32];
    uint32_t mtu;
} nic_osal_netdev_t;

typedef struct nic_osal_pkt_buf {
    void *data;
    size_t len;
    size_t capacity;
    void *os_private;
} nic_osal_pkt_buf_t;

typedef struct nic_osal_work {
    void *os_private;
} nic_osal_work_t;

typedef struct nic_osal_timer {
    void *os_private;
} nic_osal_timer_t;

typedef struct nic_osal_atomic32 {
    void *os_private;
    volatile int32_t value;
} nic_osal_atomic32_t;

typedef struct nic_osal_bitset {
    void *os_private;
    size_t nbits;
} nic_osal_bitset_t;

typedef struct nic_osal_mmio {
    void *os_private;
    nic_osal_phys_addr_t base;
    size_t size;
} nic_osal_mmio_t;

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_TYPES_H */
