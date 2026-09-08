/*
 * osal_irq.c
 *
 * FreeBSD-oriented OS abstraction for interrupt registration.
 */

#include "osal_irq.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/mutex.h>
#include <machine/bus.h>

#include <sys/errno.h>
#include <sys/libkern.h>

struct nic_os_irq_priv {
    device_t                 dev;
    struct resource         *res;
    void                    *tag;
    struct mtx               lock;
    nic_osal_irq_handler_t   handler;
    void                    *handler_arg;
    char                    *name;
    int                      rid;
    int                      vector;
    int                      enabled;
    int                      registered;
    int                      res_preallocated;
    uint32_t                 reg_flags;
};

static device_t
nic_osal_get_device(nic_osal_device_t *dev)
{
    if (dev == NULL || dev->os_private == NULL)
        return (device_t)NULL;

    return (device_t)dev->os_private;
}

static int
nic_os_irq_bus_flags_from_reg_flags(uint32_t flags)
{
    int bus_flags;
    int type_count;

    bus_flags = 0;
    type_count = 0;

    if (flags & NIC_OS_IRQ_F_MPSAFE)
        bus_flags |= INTR_MPSAFE;
    if (flags & NIC_OS_IRQ_F_EXCLUSIVE)
        bus_flags |= INTR_EXCL;
    if (flags & NIC_OS_IRQ_F_NET) {
        bus_flags |= INTR_TYPE_NET;
        type_count++;
    }
    if (flags & NIC_OS_IRQ_F_MISC) {
        bus_flags |= INTR_TYPE_MISC;
        type_count++;
    }

    if (type_count == 0)
        bus_flags |= INTR_TYPE_NET;

    return bus_flags;
}

static int
nic_os_irq_alloc_internal_resource(struct nic_os_irq_priv *priv)
{
    int rid;

    if (priv == NULL || priv->dev == (device_t)NULL)
        return EINVAL;
    if (priv->res_preallocated)
        return EINVAL;
    if (priv->res != NULL)
        return 0;
    if (priv->rid < 0)
        return EINVAL;

    rid = priv->rid;
    priv->res = bus_alloc_resource_any(priv->dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
    if (priv->res == NULL) {
        device_printf(priv->dev,
            "osal_irq: failed to allocate IRQ resource for vector=%d rid=%d\n",
            priv->vector, priv->rid);
        return ENXIO;
    }

    priv->rid = rid;
    return 0;
}

static void
nic_os_irq_release_internal_resource(struct nic_os_irq_priv *priv)
{
    if (priv == NULL)
        return;
    if (priv->res_preallocated)
        return;
    if (priv->res == NULL)
        return;
    if (priv->dev == (device_t)NULL)
        return;

    bus_release_resource(priv->dev, SYS_RES_IRQ, priv->rid, priv->res);
    priv->res = NULL;
}

static void
nic_os_irq_dispatch(void *arg)
{
    struct nic_os_irq_priv *priv;
    nic_osal_irq_handler_t handler;
    void *handler_arg;
    int vector;
    int enabled;

    priv = (struct nic_os_irq_priv *)arg;
    if (priv == NULL)
        return;

    mtx_lock(&priv->lock);
    handler = priv->handler;
    handler_arg = priv->handler_arg;
    vector = priv->vector;
    enabled = priv->enabled;
    mtx_unlock(&priv->lock);

    if (!enabled || handler == NULL)
        return;

    handler(vector, handler_arg);
}

int
nic_os_irq_prealloc(nic_osal_irq_t *irq, void *os_res, int rid, int vector)
{
    struct nic_os_irq_priv *priv;

    if (irq == NULL || os_res == NULL || rid < 0 || vector < 0)
        return EINVAL;
    if (irq->os_private != NULL)
        return EBUSY;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    mtx_init(&priv->lock, "nic_os_irq", NULL, MTX_DEF);

    priv->dev = (device_t)NULL;
    priv->res = (struct resource *)os_res;
    priv->tag = NULL;
    priv->handler = NULL;
    priv->handler_arg = NULL;
    priv->name = NULL;
    priv->rid = rid;
    priv->vector = vector;
    priv->enabled = 1;
    priv->registered = 0;
    priv->res_preallocated = 1;
    priv->reg_flags = 0;

    irq->vector = vector;
    irq->os_private = priv;

    return 0;
}

int
nic_os_irq_register_ex(nic_osal_device_t *dev,
                       nic_osal_irq_t *irq,
                       nic_osal_irq_handler_t handler,
                       void *arg,
                       const char *name,
                       uint32_t flags)
{
    device_t bsd_dev;
    struct nic_os_irq_priv *priv;
    int error;
    int bus_flags;

    if (dev == NULL || irq == NULL || handler == NULL)
        return EINVAL;

    bsd_dev = nic_osal_get_device(dev);
    if (bsd_dev == (device_t)NULL)
        return ENODEV;

    bus_flags = nic_os_irq_bus_flags_from_reg_flags(flags);

    if (irq->os_private == NULL) {
        if (irq->vector < 0)
            return EINVAL;

        priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
        if (priv == NULL)
            return ENOMEM;

        mtx_init(&priv->lock, "nic_os_irq", NULL, MTX_DEF);

        priv->dev = bsd_dev;
        priv->res = NULL;
        priv->tag = NULL;
        priv->handler = NULL;
        priv->handler_arg = NULL;
        priv->name = NULL;
        priv->rid = 1 + irq->vector;
        priv->vector = irq->vector;
        priv->enabled = 1;
        priv->registered = 0;
        priv->res_preallocated = 0;
        priv->reg_flags = 0;

        irq->os_private = priv;
    }

    priv = (struct nic_os_irq_priv *)irq->os_private;

    mtx_lock(&priv->lock);

    if (priv->registered) {
        mtx_unlock(&priv->lock);
        return EBUSY;
    }

    priv->dev = bsd_dev;
    priv->vector = irq->vector;
    priv->reg_flags = flags;

    if (priv->name != NULL) {
        free(priv->name, M_DEVBUF);
        priv->name = NULL;
    }
    if (name != NULL) {
        size_t len = strlen(name) + 1;

        priv->name = malloc(len, M_DEVBUF, M_NOWAIT);
        if (priv->name != NULL)
            memcpy(priv->name, name, len);
    }

    mtx_unlock(&priv->lock);

    /*
     * Tightened re-registration path:
     * For internally allocated IRQs, ensure a resource exists now, because a
     * prior unregister() may have released it.
     */
    if (!priv->res_preallocated) {
        error = nic_os_irq_alloc_internal_resource(priv);
        if (error != 0) {
            mtx_lock(&priv->lock);
            if (priv->name != NULL) {
                free(priv->name, M_DEVBUF);
                priv->name = NULL;
            }
            mtx_unlock(&priv->lock);
            return error;
        }
    } else if (priv->res == NULL) {
        return ENXIO;
    }

    mtx_lock(&priv->lock);
    priv->handler = handler;
    priv->handler_arg = arg;
    priv->enabled = 1;
    mtx_unlock(&priv->lock);

    error = bus_setup_intr(bsd_dev,
                           priv->res,
                           bus_flags,
                           NULL,
                           nic_os_irq_dispatch,
                           priv,
                           &priv->tag);
    if (error != 0) {
        device_printf(bsd_dev,
            "osal_irq: bus_setup_intr failed: vector=%d rid=%d error=%d\n",
            priv->vector, priv->rid, error);

        mtx_lock(&priv->lock);
        priv->handler = NULL;
        priv->handler_arg = NULL;
        if (priv->name != NULL) {
            free(priv->name, M_DEVBUF);
            priv->name = NULL;
        }
        mtx_unlock(&priv->lock);

        if (!priv->res_preallocated)
            nic_os_irq_release_internal_resource(priv);

        return error;
    }

    mtx_lock(&priv->lock);
    priv->registered = 1;
    mtx_unlock(&priv->lock);

    if (priv->name != NULL) {
        device_printf(bsd_dev,
            "osal_irq: registered IRQ vector=%d rid=%d flags=0x%x name=%s prealloc=%d\n",
            priv->vector, priv->rid, priv->reg_flags, priv->name,
            priv->res_preallocated);
    } else {
        device_printf(bsd_dev,
            "osal_irq: registered IRQ vector=%d rid=%d flags=0x%x prealloc=%d\n",
            priv->vector, priv->rid, priv->reg_flags,
            priv->res_preallocated);
    }

    return 0;
}

int
nic_os_irq_register(nic_osal_device_t *dev,
                    nic_osal_irq_t *irq,
                    nic_osal_irq_handler_t handler,
                    void *arg,
                    const char *name)
{
    return nic_os_irq_register_ex(dev, irq, handler, arg, name,
                                  NIC_OS_IRQ_F_NET | NIC_OS_IRQ_F_MPSAFE);
}

void
nic_os_irq_unregister(nic_osal_device_t *dev, nic_osal_irq_t *irq)
{
    struct nic_os_irq_priv *priv;
    device_t bsd_dev;

    (void)dev;

    if (irq == NULL || irq->os_private == NULL)
        return;

    priv = (struct nic_os_irq_priv *)irq->os_private;
    bsd_dev = priv->dev;

    mtx_lock(&priv->lock);
    priv->enabled = 0;
    mtx_unlock(&priv->lock);

    if (priv->registered && priv->tag != NULL && bsd_dev != (device_t)NULL) {
        if (bus_teardown_intr(bsd_dev, priv->res, priv->tag) != 0) {
            device_printf(bsd_dev,
                "osal_irq: bus_teardown_intr returned non-zero: vector=%d rid=%d\n",
                priv->vector, priv->rid);
        }
    }

    mtx_lock(&priv->lock);
    priv->tag = NULL;
    priv->registered = 0;
    priv->handler = NULL;
    priv->handler_arg = NULL;
    mtx_unlock(&priv->lock);

    if (bsd_dev != (device_t)NULL) {
        if (priv->name != NULL) {
            device_printf(bsd_dev,
                "osal_irq: unregistered IRQ vector=%d rid=%d name=%s prealloc=%d\n",
                priv->vector, priv->rid, priv->name, priv->res_preallocated);
        } else {
            device_printf(bsd_dev,
                "osal_irq: unregistered IRQ vector=%d rid=%d prealloc=%d\n",
                priv->vector, priv->rid, priv->res_preallocated);
        }
    }

    /*
     * For internally allocated IRQs, unregister releases the resource.
     * A later register_ex() call will allocate it again if needed.
     */
    if (!priv->res_preallocated)
        nic_os_irq_release_internal_resource(priv);
}

int
nic_os_irq_release(nic_osal_irq_t *irq)
{
    struct nic_os_irq_priv *priv;

    if (irq == NULL || irq->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_irq_priv *)irq->os_private;

    mtx_lock(&priv->lock);
    if (priv->registered) {
        mtx_unlock(&priv->lock);
        return EBUSY;
    }
    mtx_unlock(&priv->lock);

    if (!priv->res_preallocated)
        nic_os_irq_release_internal_resource(priv);

    if (priv->name != NULL)
        free(priv->name, M_DEVBUF);

    mtx_destroy(&priv->lock);
    free(priv, M_DEVBUF);

    irq->os_private = NULL;
    irq->vector = 0;

    return 0;
}

int
nic_os_irq_enable(nic_osal_irq_t *irq)
{
    struct nic_os_irq_priv *priv;

    if (irq == NULL || irq->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_irq_priv *)irq->os_private;

    mtx_lock(&priv->lock);
    priv->enabled = 1;
    mtx_unlock(&priv->lock);

    return 0;
}

int
nic_os_irq_disable(nic_osal_irq_t *irq)
{
    struct nic_os_irq_priv *priv;

    if (irq == NULL || irq->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_irq_priv *)irq->os_private;

    mtx_lock(&priv->lock);
    priv->enabled = 0;
    mtx_unlock(&priv->lock);

    return 0;
}

int
nic_os_irq_is_registered(nic_osal_irq_t *irq)
{
    struct nic_os_irq_priv *priv;
    int registered;

    if (irq == NULL || irq->os_private == NULL)
        return 0;

    priv = (struct nic_os_irq_priv *)irq->os_private;

    mtx_lock(&priv->lock);
    registered = priv->registered ? 1 : 0;
    mtx_unlock(&priv->lock);

    return registered;
}

int
nic_os_irq_is_enabled(nic_osal_irq_t *irq)
{
    struct nic_os_irq_priv *priv;
    int enabled;

    if (irq == NULL || irq->os_private == NULL)
        return 0;

    priv = (struct nic_os_irq_priv *)irq->os_private;

    mtx_lock(&priv->lock);
    enabled = priv->enabled ? 1 : 0;
    mtx_unlock(&priv->lock);

    return enabled;
}
