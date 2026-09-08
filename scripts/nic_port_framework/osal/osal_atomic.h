/*
 * osal_atomic.h
 *
 * FreeBSD-oriented OS abstraction for atomic counters, bit operations,
 * and memory barriers.
 *
 * Design notes:
 * - Atomic counters are 32-bit signed integer wrappers.
 * - Bitsets are dynamically sized and backed by an internal bitmap.
 * - Memory barriers provide driver-portable ordering primitives.
 *
 * Return conventions:
 * - Functions returning int use FreeBSD-style positive errno values:
 *     0        success
 *     1        success with state-change detail (only where documented)
 *     EINVAL   invalid argument/state
 *     EBUSY    already initialized
 *     ENOMEM   allocation failure
 *
 * Typical lifecycle:
 *
 *     nic_osal_atomic32_t a = {0};
 *     nic_osal_bitset_t flags = {0};
 *
 *     nic_os_atomic32_init(&a, 0);
 *     nic_os_atomic32_inc(&a);
 *     nic_os_atomic32_destroy(&a);
 *
 *     nic_os_bitset_init(&flags, 128);
 *     nic_os_bit_set(&flags, 5);
 *     nic_os_bit_test(&flags, 5);
 *     nic_os_bitset_destroy(&flags);
 */

#ifndef NIC_OSAL_ATOMIC_H
#define NIC_OSAL_ATOMIC_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 32-bit atomic counter operations.
 */
int  nic_os_atomic32_init(nic_osal_atomic32_t *a, int32_t initial_value);
void nic_os_atomic32_destroy(nic_osal_atomic32_t *a);

int32_t nic_os_atomic32_read(nic_osal_atomic32_t *a);
void    nic_os_atomic32_set(nic_osal_atomic32_t *a, int32_t value);

void    nic_os_atomic32_inc(nic_osal_atomic32_t *a);
void    nic_os_atomic32_dec(nic_osal_atomic32_t *a);
void    nic_os_atomic32_add(nic_osal_atomic32_t *a, int32_t value);
void    nic_os_atomic32_sub(nic_osal_atomic32_t *a, int32_t value);

int32_t nic_os_atomic32_add_return(nic_osal_atomic32_t *a, int32_t value);
int32_t nic_os_atomic32_sub_return(nic_osal_atomic32_t *a, int32_t value);
int32_t nic_os_atomic32_inc_return(nic_osal_atomic32_t *a);
int32_t nic_os_atomic32_dec_return(nic_osal_atomic32_t *a);

int32_t nic_os_atomic32_xchg(nic_osal_atomic32_t *a, int32_t new_value);
int     nic_os_atomic32_cmpxchg(nic_osal_atomic32_t *a,
                                int32_t expected,
                                int32_t new_value,
                                int32_t *old_value);

/*
 * Bitset operations.
 */
int  nic_os_bitset_init(nic_osal_bitset_t *bitset, size_t nbits);
void nic_os_bitset_destroy(nic_osal_bitset_t *bitset);

int  nic_os_bit_test(nic_osal_bitset_t *bitset, size_t bit);
int  nic_os_bit_set(nic_osal_bitset_t *bitset, size_t bit);
int  nic_os_bit_clear(nic_osal_bitset_t *bitset, size_t bit);

int  nic_os_bit_test_and_set(nic_osal_bitset_t *bitset, size_t bit);
int  nic_os_bit_test_and_clear(nic_osal_bitset_t *bitset, size_t bit);

/*
 * Memory barriers.
 */
void nic_os_mb(void);
void nic_os_rmb(void);
void nic_os_wmb(void);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_ATOMIC_H */
