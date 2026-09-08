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

#ifndef NIC_OSAL_LOCK_H
#define NIC_OSAL_LOCK_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int  nic_os_lock_init(nic_osal_lock_t *lock);
void nic_os_lock_destroy(nic_osal_lock_t *lock);
void nic_os_lock(nic_osal_lock_t *lock);
void nic_os_unlock(nic_osal_lock_t *lock);

int  nic_os_lock_irqsave(nic_osal_lock_t *lock, unsigned long *flags);
void nic_os_unlock_irqrestore(nic_osal_lock_t *lock, unsigned long *flags);

int  nic_os_mutex_init(nic_osal_mutex_t *mutex);
void nic_os_mutex_destroy(nic_osal_mutex_t *mutex);
void nic_os_mutex_lock(nic_osal_mutex_t *mutex);
void nic_os_mutex_unlock(nic_osal_mutex_t *mutex);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_LOCK_H */
