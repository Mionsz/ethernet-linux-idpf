/*
 * osal_work.h
 *
 * FreeBSD-oriented OS abstraction for deferred work and kernel threads.
 *
 * Design notes:
 * - Deferred work is implemented with taskqueue(9).
 * - Delayed work is implemented with callout(9) that enqueues a taskqueue task.
 * - Work destruction drains both delayed and queued/running work.
 * - Thread stop is cooperative: the worker function must periodically check
 *   nic_os_thread_should_stop().
 *
 * Return conventions:
 * - Functions returning int use FreeBSD-style positive errno values:
 *     0        success
 *     EINVAL   invalid argument/state
 *     EBUSY    object already initialized or being destroyed
 *     ENOMEM   allocation failure
 *     other errno values as returned by kernel APIs
 */

#ifndef NIC_OSAL_WORK_H
#define NIC_OSAL_WORK_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nic_osal_work_fn_t)(void *arg);

/*
 * Initialize a deferred work item.
 *
 * Parameters:
 * - work: caller-owned work object
 * - fn:   callback invoked when work runs
 * - arg:  callback argument
 *
 * Returns:
 * - 0 on success
 * - EINVAL if work/fn is invalid
 * - EBUSY if work is already initialized
 * - ENOMEM on allocation failure
 */
int nic_os_work_init(nic_osal_work_t *work,
                     nic_osal_work_fn_t fn,
                     void *arg);

/*
 * Destroy a work item.
 *
 * Semantics:
 * - Marks the work item as destroying.
 * - Drains any pending delayed callback.
 * - Drains queued/running task execution.
 * - Safe to call with NULL or uninitialized work; such calls are ignored.
 */
void nic_os_work_destroy(nic_osal_work_t *work);

/*
 * Schedule immediate execution of a work item.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if work is invalid/uninitialized
 * - EBUSY if work is being destroyed
 */
int nic_os_work_schedule(nic_osal_work_t *work);

/*
 * Schedule delayed execution of a work item.
 *
 * Parameters:
 * - delay_ms: delay in milliseconds; 0 is rounded up to the minimum delay
 *
 * Returns:
 * - 0 on success
 * - EINVAL if work is invalid/uninitialized
 * - EBUSY if work is being destroyed
 */
int nic_os_work_schedule_delayed(nic_osal_work_t *work,
                                 unsigned int delay_ms);

/*
 * Cancel delayed work if it has not fired yet.
 *
 * Returns:
 * - 1 if delayed work was pending and canceled
 * - 0 if no delayed work was pending
 * - EINVAL if work is invalid/uninitialized
 *
 * Notes:
 * - This only cancels the delayed callout stage.
 * - It does not guarantee cancellation of already-enqueued immediate work.
 * - Use nic_os_work_flush() if you need a quiescent postcondition.
 */
int nic_os_work_cancel(nic_osal_work_t *work);

/*
 * Flush pending delayed callback and queued/running work.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if work is invalid/uninitialized
 */
int nic_os_work_flush(nic_osal_work_t *work);

/*
 * Create a kernel worker thread.
 *
 * The created thread executes:
 *     entry(arg);
 *
 * Stop semantics are cooperative. The thread function should periodically
 * check nic_os_thread_should_stop(thread_handle) through a context object
 * that carries the thread handle.
 *
 * Parameters:
 * - thread_handle: receives opaque thread handle on success
 * - name: optional thread name; may be NULL
 * - entry: thread entry point
 * - arg: thread entry argument
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid
 * - ENOMEM on allocation failure
 * - other errno values from underlying kernel thread creation
 */
int nic_os_thread_create(void **thread_handle,
                         const char *name,
                         void (*entry)(void *arg),
                         void *arg);

/*
 * Request cooperative termination of a thread.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if thread_handle is invalid
 *
 * Notes:
 * - This does not forcibly kill the thread.
 * - The thread must cooperate by checking nic_os_thread_should_stop().
 */
int nic_os_thread_stop(void *thread_handle);

/*
 * Wait for thread termination and free thread handle resources.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if thread_handle is invalid
 *
 * Notes:
 * - After successful join, thread_handle is no longer valid.
 * - Caller is responsible for clearing its stored handle if needed.
 */
int nic_os_thread_join(void *thread_handle);

/*
 * Query whether cooperative stop has been requested for a thread.
 *
 * Returns:
 * - 1 if stop requested
 * - 0 otherwise
 *
 * Notes:
 * - Returns 0 for NULL/invalid handle.
 * - Intended to be called by the running thread function.
 */
int nic_os_thread_should_stop(void *thread_handle);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_WORK_H */
