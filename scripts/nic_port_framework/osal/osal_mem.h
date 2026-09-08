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

#ifndef NIC_OSAL_MEM_H
#define NIC_OSAL_MEM_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void *nic_os_malloc(size_t size);
void *nic_os_calloc(size_t count, size_t size);
void *nic_os_zalloc(size_t size);
void  nic_os_free(void *ptr);

void *nic_os_memcpy(void *dst, const void *src, size_t size);
void *nic_os_memset(void *dst, int value, size_t size);
int   nic_os_memcmp(const void *a, const void *b, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_MEM_H */
