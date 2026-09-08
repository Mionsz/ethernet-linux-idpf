/*
 * osal_work.c
 *
 * FreeBSD-oriented OS abstraction for deferred work and kernel threads.
 */

#include "osal_work.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/taskqueue.h>
#include <sys/callout.h>
#include <sys/time.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <sys/errno.h>
#include <sys/libkern.h>

/* Base FreeBSD has no msecs_to_ticks(); that is a LinuxKPI helper. */
static int
nic_os_msecs_to_ticks(unsigned int ms)
{
    struct timeval tv;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;

    return (tvtohz(&tv));
}

struct nic_os_work_priv {
    struct task          task;
    struct callout       callout;
    struct mtx           lock;
    nic_osal_work_fn_t   fn;
    void                *arg;
    int                  destroying;
    int                  delayed_armed;
};

struct nic_os_thread_priv {
    struct proc         *proc;
    struct mtx           lock;
    struct cv            cv;
    void               (*entry)(void *arg);
    void                *arg;
    int                  stop_requested;
    int                  exited;
};

static void
nic_os_work_task_fn(void *context, int pending)
{
    struct nic_os_work_priv *priv;

    (void)pending;

    priv = (struct nic_os_work_priv *)context;
    if (priv == NULL || priv->fn == NULL)
        return;

    mtx_lock(&priv->lock);
    if (priv->destroying) {
        mtx_unlock(&priv->lock);
        return;
    }
    priv->delayed_armed = 0;
    mtx_unlock(&priv->lock);

    priv->fn(priv->arg);
}

static void
nic_os_work_callout_fn(void *context)
{
    struct nic_os_work_priv *priv;

    priv = (struct nic_os_work_priv *)context;
    if (priv == NULL)
        return;

    mtx_lock(&priv->lock);
    if (priv->destroying) {
        priv->delayed_armed = 0;
        mtx_unlock(&priv->lock);
        return;
    }
    priv->delayed_armed = 0;
    mtx_unlock(&priv->lock);

    taskqueue_enqueue(taskqueue_thread, &priv->task);
}

static void
nic_os_thread_trampoline(void *arg)
{
    struct nic_os_thread_priv *priv;

    priv = (struct nic_os_thread_priv *)arg;
    if (priv == NULL)
        kproc_exit(0);

    if (priv->entry != NULL)
        priv->entry(priv->arg);

    mtx_lock(&priv->lock);
    priv->exited = 1;
    cv_broadcast(&priv->cv);
    mtx_unlock(&priv->lock);

    kproc_exit(0);
}

int
nic_os_work_init(nic_osal_work_t *work,
                 nic_osal_work_fn_t fn,
                 void *arg)
{
    struct nic_os_work_priv *priv;

    if (work == NULL || fn == NULL)
        return EINVAL;
    if (work->os_private != NULL)
        return EBUSY;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    priv->fn = fn;
    priv->arg = arg;
    priv->destroying = 0;
    priv->delayed_armed = 0;

    mtx_init(&priv->lock, "nic_os_work", NULL, MTX_DEF);
    TASK_INIT(&priv->task, 0, nic_os_work_task_fn, priv);
    callout_init_mtx(&priv->callout, &priv->lock, 0);

    work->os_private = priv;
    return 0;
}

void
nic_os_work_destroy(nic_osal_work_t *work)
{
    struct nic_os_work_priv *priv;

    if (work == NULL || work->os_private == NULL)
        return;

    priv = (struct nic_os_work_priv *)work->os_private;

    mtx_lock(&priv->lock);
    priv->destroying = 1;
    priv->delayed_armed = 0;
    mtx_unlock(&priv->lock);

    callout_drain(&priv->callout);
    taskqueue_drain(taskqueue_thread, &priv->task);

    mtx_destroy(&priv->lock);
    free(priv, M_DEVBUF);
    work->os_private = NULL;
}

int
nic_os_work_schedule(nic_osal_work_t *work)
{
    struct nic_os_work_priv *priv;

    if (work == NULL || work->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_work_priv *)work->os_private;

    mtx_lock(&priv->lock);
    if (priv->destroying) {
        mtx_unlock(&priv->lock);
        return EBUSY;
    }
    mtx_unlock(&priv->lock);

    taskqueue_enqueue(taskqueue_thread, &priv->task);
    return 0;
}

int
nic_os_work_schedule_delayed(nic_osal_work_t *work,
                             unsigned int delay_ms)
{
    struct nic_os_work_priv *priv;
    int ticks;

    if (work == NULL || work->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_work_priv *)work->os_private;
    ticks = (delay_ms == 0) ? 1 : max(1, (int)nic_os_msecs_to_ticks(delay_ms));

    mtx_lock(&priv->lock);
    if (priv->destroying) {
        mtx_unlock(&priv->lock);
        return EBUSY;
    }
    priv->delayed_armed = 1;
    callout_reset(&priv->callout, ticks, nic_os_work_callout_fn, priv);
    mtx_unlock(&priv->lock);

    return 0;
}

int
nic_os_work_cancel(nic_osal_work_t *work)
{
    struct nic_os_work_priv *priv;
    int was_pending;

    if (work == NULL || work->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_work_priv *)work->os_private;

    mtx_lock(&priv->lock);
    was_pending = priv->delayed_armed ? 1 : 0;
    priv->delayed_armed = 0;
    mtx_unlock(&priv->lock);

    callout_stop(&priv->callout);
    return was_pending;
}

int
nic_os_work_flush(nic_osal_work_t *work)
{
    struct nic_os_work_priv *priv;

    if (work == NULL || work->os_private == NULL)
        return EINVAL;

    priv = (struct nic_os_work_priv *)work->os_private;

    callout_drain(&priv->callout);
    taskqueue_drain(taskqueue_thread, &priv->task);
    return 0;
}

int
nic_os_thread_create(void **thread_handle,
                     const char *name,
                     void (*entry)(void *arg),
                     void *arg)
{
    struct nic_os_thread_priv *priv;
    int error;

    if (thread_handle == NULL || entry == NULL)
        return EINVAL;

    *thread_handle = NULL;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    priv->entry = entry;
    priv->arg = arg;
    priv->stop_requested = 0;
    priv->exited = 0;

    mtx_init(&priv->lock, "nic_os_thread", NULL, MTX_DEF);
    cv_init(&priv->cv, "nic_os_thread_cv");

    error = kproc_create(nic_os_thread_trampoline,
                         priv,
                         &priv->proc,
                         0,
                         0,
                         "%s",
                         (name != NULL) ? name : "nic_os_thread");
    if (error != 0) {
        cv_destroy(&priv->cv);
        mtx_destroy(&priv->lock);
        free(priv, M_DEVBUF);
        return error;
    }

    *thread_handle = priv;
    return 0;
}

int
nic_os_thread_stop(void *thread_handle)
{
    struct nic_os_thread_priv *priv;

    if (thread_handle == NULL)
        return EINVAL;

    priv = (struct nic_os_thread_priv *)thread_handle;

    mtx_lock(&priv->lock);
    priv->stop_requested = 1;
    cv_broadcast(&priv->cv);
    mtx_unlock(&priv->lock);

    return 0;
}

int
nic_os_thread_join(void *thread_handle)
{
    struct nic_os_thread_priv *priv;

    if (thread_handle == NULL)
        return EINVAL;

    priv = (struct nic_os_thread_priv *)thread_handle;

    mtx_lock(&priv->lock);
    while (!priv->exited)
        cv_wait(&priv->cv, &priv->lock);
    mtx_unlock(&priv->lock);

    cv_destroy(&priv->cv);
    mtx_destroy(&priv->lock);
    free(priv, M_DEVBUF);

    return 0;
}

int
nic_os_thread_should_stop(void *thread_handle)
{
    struct nic_os_thread_priv *priv;
    int stop;

    if (thread_handle == NULL)
        return 0;

    priv = (struct nic_os_thread_priv *)thread_handle;

    mtx_lock(&priv->lock);
    stop = priv->stop_requested ? 1 : 0;
    mtx_unlock(&priv->lock);

    return stop;
}
