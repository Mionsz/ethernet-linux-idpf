/*
 * osal_time.c
 *
 * FreeBSD-oriented OS abstraction for monotonic time, sleep/delay,
 * and timer services.
 */

#include "osal_time.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/callout.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/time.h>
#include <sys/libkern.h>

#include <machine/cpu.h>

#include <sys/errno.h>
#include <sys/libkern.h>

/* Base FreeBSD has no msecs_to_ticks(); that is a LinuxKPI helper. */
static int
nic_os_msecs_to_ticks(uint32_t ms)
{
    struct timeval tv;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    return (tvtohz(&tv));
}

struct nic_os_timer_priv {
    struct callout       callout;
    struct mtx           lock;
    void               (*callback)(void *arg);
    void                *arg;
    uint32_t             timeout_ms;
    int                  periodic;
    int                  active;
    int                  destroying;
};

static void
nic_os_timer_callout_fn(void *context)
{
    struct nic_os_timer_priv *priv;
    int periodic;
    uint32_t timeout_ms;
    void (*callback)(void *arg);
    void *arg;
    int ticks;

    priv = (struct nic_os_timer_priv *)context;
    if (priv == NULL)
        return;

    mtx_lock(&priv->lock);

    if (priv->destroying) {
        priv->active = 0;
        mtx_unlock(&priv->lock);
        return;
    }

    periodic = priv->periodic;
    timeout_ms = priv->timeout_ms;
    callback = priv->callback;
    arg = priv->arg;

    if (!periodic)
        priv->active = 0;

    mtx_unlock(&priv->lock);

    if (callback != NULL)
        callback(arg);

    if (!periodic)
        return;

    ticks = (timeout_ms == 0) ? 1 : max(1, (int)nic_os_msecs_to_ticks(timeout_ms));

    mtx_lock(&priv->lock);
    if (!priv->destroying && priv->periodic) {
        priv->active = 1;
        callout_reset(&priv->callout, ticks, nic_os_timer_callout_fn, priv);
    } else {
        priv->active = 0;
    }
    mtx_unlock(&priv->lock);
}

uint64_t
nic_os_time_ms(void)
{
    struct timeval tv;

    microuptime(&tv);
    return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
}

uint64_t
nic_os_time_us(void)
{
    struct timeval tv;

    microuptime(&tv);
    return ((uint64_t)tv.tv_sec * 1000000ULL) + (uint64_t)tv.tv_usec;
}

void
nic_os_sleep_ms(uint32_t ms)
{
    int ticks;

    if (ms == 0)
        return;

    ticks = max(1, (int)nic_os_msecs_to_ticks(ms));
    pause("nicoslp", ticks);
}

void
nic_os_delay_us(uint32_t us)
{
    if (us == 0)
        return;

    DELAY(us);
}

int
nic_os_timer_init(nic_osal_timer_t *timer,
                  void (*callback)(void *arg),
                  void *arg)
{
    struct nic_os_timer_priv *priv;

    if (timer == NULL || callback == NULL)
        return EINVAL;
    if (timer->os_private != NULL)
        return EBUSY;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    mtx_init(&priv->lock, "nic_os_timer", NULL, MTX_DEF);
    callout_init_mtx(&priv->callout, &priv->lock, 0);

    priv->callback = callback;
    priv->arg = arg;
    priv->timeout_ms = 0;
    priv->periodic = 0;
    priv->active = 0;
    priv->destroying = 0;

    timer->os_private = priv;
    return 0;
}

void
nic_os_timer_destroy(nic_osal_timer_t *timer)
{
    struct nic_os_timer_priv *priv;

    if (timer == NULL || timer->os_private == NULL)
        return;

    priv = (struct nic_os_timer_priv *)timer->os_private;

    mtx_lock(&priv->lock);
    priv->destroying = 1;
    priv->active = 0;
    mtx_unlock(&priv->lock);

    callout_drain(&priv->callout);

    mtx_destroy(&priv->lock);
    free(priv, M_DEVBUF);

    timer->os_private = NULL;
}

int
nic_os_timer_start(nic_osal_timer_t *timer, uint32_t timeout_ms, bool periodic)
{
    struct nic_os_timer_priv *priv;
    int ticks;

    if (timer == NULL || timer->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_timer_priv *)timer->os_private;
    ticks = (timeout_ms == 0) ? 1 : max(1, (int)nic_os_msecs_to_ticks(timeout_ms));

    mtx_lock(&priv->lock);
    if (priv->destroying) {
        mtx_unlock(&priv->lock);
        return EBUSY;
    }

    priv->timeout_ms = timeout_ms;
    priv->periodic = periodic ? 1 : 0;
    priv->active = 1;

    callout_reset(&priv->callout, ticks, nic_os_timer_callout_fn, priv);
    mtx_unlock(&priv->lock);

    return 0;
}

void
nic_os_timer_stop(nic_osal_timer_t *timer)
{
    struct nic_os_timer_priv *priv;

    if (timer == NULL || timer->os_private == NULL)
        return;

    priv = (struct nic_os_timer_priv *)timer->os_private;

    mtx_lock(&priv->lock);
    priv->periodic = 0;
    priv->active = 0;
    mtx_unlock(&priv->lock);

    callout_drain(&priv->callout);
}
