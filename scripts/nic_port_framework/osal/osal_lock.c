#include "osal_lock.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/libkern.h>

#include <sys/errno.h>
#include <sys/libkern.h>

struct nic_os_lock_priv {
    struct mtx mtx;
};

struct nic_os_mutex_priv {
    struct mtx mtx;
};

int
nic_os_lock_init(nic_osal_lock_t *lock)
{
    struct nic_os_lock_priv *priv;

    if (lock == NULL)
        return EINVAL;

    if (lock->os_private != NULL)
        return EBUSY;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    mtx_init(&priv->mtx, "nic_osal_spin", NULL, MTX_SPIN);
    lock->os_private = priv;
    return 0;
}

void
nic_os_lock_destroy(nic_osal_lock_t *lock)
{
    struct nic_os_lock_priv *priv;

    if (lock == NULL || lock->os_private == NULL)
        return;

    priv = (struct nic_os_lock_priv *)lock->os_private;
    mtx_destroy(&priv->mtx);
    free(priv, M_DEVBUF);
    lock->os_private = NULL;
}

void
nic_os_lock(nic_osal_lock_t *lock)
{
    struct nic_os_lock_priv *priv;

    if (lock == NULL || lock->os_private == NULL)
        return;

    priv = (struct nic_os_lock_priv *)lock->os_private;
    mtx_lock_spin(&priv->mtx);
}

void
nic_os_unlock(nic_osal_lock_t *lock)
{
    struct nic_os_lock_priv *priv;

    if (lock == NULL || lock->os_private == NULL)
        return;

    priv = (struct nic_os_lock_priv *)lock->os_private;
    mtx_unlock_spin(&priv->mtx);
}

int
nic_os_lock_irqsave(nic_osal_lock_t *lock, unsigned long *flags)
{
    struct nic_os_lock_priv *priv;

    if (lock == NULL || lock->os_private == NULL || flags == NULL)
        return EINVAL;

    priv = (struct nic_os_lock_priv *)lock->os_private;

    /*
     * FreeBSD does not expose Linux-style interrupt flags save/restore
     * for this abstraction. Approximate irqsave semantics by entering
     * a critical section before acquiring the spin mutex.
     */
    critical_enter();
    mtx_lock_spin(&priv->mtx);
    *flags = 0;
    return 0;
}

void
nic_os_unlock_irqrestore(nic_osal_lock_t *lock, unsigned long *flags)
{
    struct nic_os_lock_priv *priv;

    (void)flags;

    if (lock == NULL || lock->os_private == NULL)
        return;

    priv = (struct nic_os_lock_priv *)lock->os_private;
    mtx_unlock_spin(&priv->mtx);
    critical_exit();
}

int
nic_os_mutex_init(nic_osal_mutex_t *mutex)
{
    struct nic_os_mutex_priv *priv;

    if (mutex == NULL)
        return EINVAL;

    if (mutex->os_private != NULL)
        return EBUSY;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    mtx_init(&priv->mtx, "nic_osal_mutex", NULL, MTX_DEF);
    mutex->os_private = priv;
    return 0;
}

void
nic_os_mutex_destroy(nic_osal_mutex_t *mutex)
{
    struct nic_os_mutex_priv *priv;

    if (mutex == NULL || mutex->os_private == NULL)
        return;

    priv = (struct nic_os_mutex_priv *)mutex->os_private;
    mtx_destroy(&priv->mtx);
    free(priv, M_DEVBUF);
    mutex->os_private = NULL;
}

void
nic_os_mutex_lock(nic_osal_mutex_t *mutex)
{
    struct nic_os_mutex_priv *priv;

    if (mutex == NULL || mutex->os_private == NULL)
        return;

    priv = (struct nic_os_mutex_priv *)mutex->os_private;
    mtx_lock(&priv->mtx);
}

void
nic_os_mutex_unlock(nic_osal_mutex_t *mutex)
{
    struct nic_os_mutex_priv *priv;

    if (mutex == NULL || mutex->os_private == NULL)
        return;

    priv = (struct nic_os_mutex_priv *)mutex->os_private;
    mtx_unlock(&priv->mtx);
}
