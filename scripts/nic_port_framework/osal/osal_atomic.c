/*
 * osal_atomic.c
 *
 * FreeBSD-oriented OS abstraction for atomic counters, bit operations,
 * and memory barriers.
 */

#include "osal_atomic.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/bitset.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>

#include <sys/errno.h>
#include <sys/libkern.h>

struct nic_os_atomic32_priv {
    volatile int32_t value;
};

struct nic_os_bitset_priv {
    u_long *bits;                 /* serialized by lock; setbit(9) needs non-volatile */
    size_t nbits;
    size_t nwords;
    struct mtx lock;
};

static struct nic_os_atomic32_priv *
nic_os_atomic32_get_priv(nic_osal_atomic32_t *a)
{
    if (a == NULL || a->os_private == NULL)
        return NULL;

    return (struct nic_os_atomic32_priv *)a->os_private;
}

static struct nic_os_bitset_priv *
nic_os_bitset_get_priv(nic_osal_bitset_t *bitset)
{
    if (bitset == NULL || bitset->os_private == NULL)
        return NULL;

    return (struct nic_os_bitset_priv *)bitset->os_private;
}

static int
nic_os_bitset_validate_index(struct nic_os_bitset_priv *priv, size_t bit)
{
    if (priv == NULL)
        return EINVAL;
    if (bit >= priv->nbits)
        return EINVAL;
    return 0;
}

int
nic_os_atomic32_init(nic_osal_atomic32_t *a, int32_t initial_value)
{
    struct nic_os_atomic32_priv *priv;

    if (a == NULL)
        return EINVAL;
    if (a->os_private != NULL)
        return EBUSY;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    priv->value = initial_value;
    a->os_private = priv;
    return 0;
}

void
nic_os_atomic32_destroy(nic_osal_atomic32_t *a)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return;

    free(priv, M_DEVBUF);
    a->os_private = NULL;
}

int32_t
nic_os_atomic32_read(nic_osal_atomic32_t *a)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return 0;

    return atomic_load_acq_int((volatile u_int *)&priv->value);
}

void
nic_os_atomic32_set(nic_osal_atomic32_t *a, int32_t value)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return;

    atomic_store_rel_int((volatile u_int *)&priv->value, (u_int)value);
}

void
nic_os_atomic32_inc(nic_osal_atomic32_t *a)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return;

    atomic_add_int((volatile u_int *)&priv->value, 1);
}

void
nic_os_atomic32_dec(nic_osal_atomic32_t *a)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return;

    atomic_subtract_int((volatile u_int *)&priv->value, 1);
}

void
nic_os_atomic32_add(nic_osal_atomic32_t *a, int32_t value)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return;

    atomic_add_int((volatile u_int *)&priv->value, (u_int)value);
}

void
nic_os_atomic32_sub(nic_osal_atomic32_t *a, int32_t value)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return;

    atomic_subtract_int((volatile u_int *)&priv->value, (u_int)value);
}

int32_t
nic_os_atomic32_add_return(nic_osal_atomic32_t *a, int32_t value)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return 0;

    return (int32_t)atomic_fetchadd_int((volatile u_int *)&priv->value,
                                        (u_int)value) + value;
}

int32_t
nic_os_atomic32_sub_return(nic_osal_atomic32_t *a, int32_t value)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return 0;

    return (int32_t)atomic_fetchadd_int((volatile u_int *)&priv->value,
                                        (u_int)(-value)) - value + value;
}

int32_t
nic_os_atomic32_inc_return(nic_osal_atomic32_t *a)
{
    return nic_os_atomic32_add_return(a, 1);
}

int32_t
nic_os_atomic32_dec_return(nic_osal_atomic32_t *a)
{
    return nic_os_atomic32_add_return(a, -1);
}

int32_t
nic_os_atomic32_xchg(nic_osal_atomic32_t *a, int32_t new_value)
{
    struct nic_os_atomic32_priv *priv;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL)
        return 0;

    return (int32_t)atomic_swap_int((volatile u_int *)&priv->value,
                                    (u_int)new_value);
}

int
nic_os_atomic32_cmpxchg(nic_osal_atomic32_t *a,
                        int32_t expected,
                        int32_t new_value,
                        int32_t *old_value)
{
    struct nic_os_atomic32_priv *priv;
    u_int prev;

    if (old_value == NULL)
        return EINVAL;

    priv = nic_os_atomic32_get_priv(a);
    if (priv == NULL) {
        *old_value = 0;
        return EINVAL;
    }

    prev = atomic_cmpset_int((volatile u_int *)&priv->value,
                             (u_int)expected,
                             (u_int)new_value)
           ? (u_int)expected
           : atomic_load_acq_int((volatile u_int *)&priv->value);

    *old_value = (int32_t)prev;
    return 0;
}

int
nic_os_bitset_init(nic_osal_bitset_t *bitset, size_t nbits)
{
    struct nic_os_bitset_priv *priv;
    size_t nwords;
    size_t bytes;

    if (bitset == NULL || nbits == 0)
        return EINVAL;
    if (bitset->os_private != NULL)
        return EBUSY;

    nwords = howmany(nbits, (sizeof(u_long) * NBBY));
    bytes = nwords * sizeof(u_long);

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    priv->bits = malloc(bytes, M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv->bits == NULL) {
        free(priv, M_DEVBUF);
        return ENOMEM;
    }

    priv->nbits = nbits;
    priv->nwords = nwords;
    mtx_init(&priv->lock, "nic_os_bitset", NULL, MTX_DEF);

    bitset->os_private = priv;
    bitset->nbits = nbits;
    return 0;
}

void
nic_os_bitset_destroy(nic_osal_bitset_t *bitset)
{
    struct nic_os_bitset_priv *priv;

    priv = nic_os_bitset_get_priv(bitset);
    if (priv == NULL)
        return;

    if (priv->bits != NULL)
        free(priv->bits, M_DEVBUF);

    mtx_destroy(&priv->lock);
    free(priv, M_DEVBUF);

    bitset->os_private = NULL;
    bitset->nbits = 0;
}

int
nic_os_bit_test(nic_osal_bitset_t *bitset, size_t bit)
{
    struct nic_os_bitset_priv *priv;
    int ret;

    priv = nic_os_bitset_get_priv(bitset);
    if (nic_os_bitset_validate_index(priv, bit) != 0)
        return 0;

    mtx_lock(&priv->lock);
    ret = isset(priv->bits, bit) ? 1 : 0;
    mtx_unlock(&priv->lock);

    return ret;
}

int
nic_os_bit_set(nic_osal_bitset_t *bitset, size_t bit)
{
    struct nic_os_bitset_priv *priv;
    int error;

    priv = nic_os_bitset_get_priv(bitset);
    error = nic_os_bitset_validate_index(priv, bit);
    if (error != 0)
        return error;

    mtx_lock(&priv->lock);
    setbit(priv->bits, bit);
    mtx_unlock(&priv->lock);

    return 0;
}

int
nic_os_bit_clear(nic_osal_bitset_t *bitset, size_t bit)
{
    struct nic_os_bitset_priv *priv;
    int error;

    priv = nic_os_bitset_get_priv(bitset);
    error = nic_os_bitset_validate_index(priv, bit);
    if (error != 0)
        return error;

    mtx_lock(&priv->lock);
    clrbit(priv->bits, bit);
    mtx_unlock(&priv->lock);

    return 0;
}

int
nic_os_bit_test_and_set(nic_osal_bitset_t *bitset, size_t bit)
{
    struct nic_os_bitset_priv *priv;
    int error;
    int old;

    priv = nic_os_bitset_get_priv(bitset);
    error = nic_os_bitset_validate_index(priv, bit);
    if (error != 0)
        return 0;

    mtx_lock(&priv->lock);
    old = isset(priv->bits, bit) ? 1 : 0;
    setbit(priv->bits, bit);
    mtx_unlock(&priv->lock);

    return old;
}

int
nic_os_bit_test_and_clear(nic_osal_bitset_t *bitset, size_t bit)
{
    struct nic_os_bitset_priv *priv;
    int error;
    int old;

    priv = nic_os_bitset_get_priv(bitset);
    error = nic_os_bitset_validate_index(priv, bit);
    if (error != 0)
        return 0;

    mtx_lock(&priv->lock);
    old = isset(priv->bits, bit) ? 1 : 0;
    clrbit(priv->bits, bit);
    mtx_unlock(&priv->lock);

    return old;
}

void
nic_os_mb(void)
{
    atomic_thread_fence_seq_cst();
}

void
nic_os_rmb(void)
{
    atomic_thread_fence_acq();
}

void
nic_os_wmb(void)
{
    atomic_thread_fence_rel();
}
