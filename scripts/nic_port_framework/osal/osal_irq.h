/*
 * osal_irq.h
 *
 * FreeBSD-oriented OS abstraction for interrupt registration.
 *
 * Design notes:
 * - Preferred model: the driver allocates IRQ resources during attach/reset
 *   and passes them into this OSAL via nic_os_irq_prealloc().
 * - Optional model: the OSAL allocates an IRQ resource from irq->vector using
 *   a conventional RID mapping suitable for many MSI-X configurations.
 * - Interrupt enable/disable here is a software gating mechanism for the
 *   registered callback. It does not replace hardware interrupt masking in
 *   device registers.
 *
 * Return conventions:
 * - Functions returning int use FreeBSD-style positive errno values:
 *     0        success
 *     1        success with state-change detail (only where documented)
 *     EINVAL   invalid argument/state
 *     EBUSY    object already initialized or still registered
 *     ENODEV   invalid device
 *     ENXIO    IRQ resource unavailable
 *     ENOMEM   allocation failure
 *     other errno values as returned by kernel APIs
 *
 * Typical lifecycle examples:
 *
 * 1) Preferred preallocated-resource flow:
 *
 *     nic_osal_irq_t irq = {0};
 *
 *     // Driver allocates struct resource *res and RID earlier.
 *     nic_os_irq_prealloc(&irq, res, rid, vector);
 *     nic_os_irq_register_ex(dev, &irq, handler, arg, "rxq0",
 *         NIC_OS_IRQ_F_NET | NIC_OS_IRQ_F_MPSAFE);
 *
 *     // Reset / temporary teardown
 *     nic_os_irq_disable(&irq);
 *     nic_os_irq_unregister(dev, &irq);
 *
 *     // Re-register later using same preallocated resource
 *     nic_os_irq_register(dev, &irq, handler, arg, "rxq0");
 *
 *     // Final detach
 *     nic_os_irq_unregister(dev, &irq);
 *     nic_os_irq_release(&irq);
 *     // Driver releases actual IRQ resource separately.
 *
 * 2) Internally allocated-resource flow:
 *
 *     nic_osal_irq_t irq = { .vector = 0 };
 *
 *     nic_os_irq_register(dev, &irq, handler, arg, "adminq");
 *     nic_os_irq_unregister(dev, &irq);
 *     nic_os_irq_release(&irq);
 *
 * Notes:
 * - For internally allocated IRQs, unregister() releases the IRQ resource.
 *   A later register() call will allocate it again if needed.
 * - For preallocated IRQs, unregister() never releases the underlying
 *   resource; release() frees only the OSAL wrapper state.
 */

#ifndef NIC_OSAL_IRQ_H
#define NIC_OSAL_IRQ_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nic_osal_irq_handler_t)(int vector, void *arg);

/*
 * Registration flags for nic_os_irq_register_ex().
 */
#define NIC_OS_IRQ_F_MPSAFE       (1u << 0) /* request INTR_MPSAFE */
#define NIC_OS_IRQ_F_EXCLUSIVE    (1u << 1) /* request INTR_EXCL */
#define NIC_OS_IRQ_F_NET          (1u << 2) /* request INTR_TYPE_NET */
#define NIC_OS_IRQ_F_MISC         (1u << 3) /* request INTR_TYPE_MISC */

/*
 * Associate a preallocated FreeBSD IRQ resource with an OSAL IRQ object.
 *
 * Parameters:
 * - irq:     OSAL IRQ object to initialize
 * - os_res:  preallocated OS IRQ resource (FreeBSD struct resource *)
 * - rid:     associated SYS_RES_IRQ RID
 * - vector:  logical vector/index to report to callback
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid
 * - EBUSY if irq is already initialized
 * - ENOMEM on allocation failure
 *
 * Notes:
 * - Ownership of the underlying IRQ resource remains with the driver.
 * - Use nic_os_irq_release() to release the OSAL wrapper object when done.
 */
int nic_os_irq_prealloc(nic_osal_irq_t *irq,
                        void *os_res,
                        int rid,
                        int vector);

/*
 * Register an interrupt handler with explicit registration flags.
 *
 * Parameters:
 * - dev:     OSAL device; on FreeBSD dev->os_private must be device_t
 * - irq:     initialized IRQ object or zeroed object with valid irq->vector
 * - handler: callback to invoke when interrupt fires
 * - arg:     callback argument
 * - name:    optional diagnostic name, may be NULL
 * - flags:   NIC_OS_IRQ_F_* flags
 *
 * Returns:
 * - 0 on success
 * - errno on failure
 *
 * Notes:
 * - If irq was initialized with nic_os_irq_prealloc(), the existing IRQ
 *   resource is used.
 * - Otherwise, the OSAL may allocate an IRQ resource based on irq->vector.
 * - For internally allocated IRQs, if unregister() was called earlier and
 *   released the resource, register_ex() will allocate it again.
 */
int nic_os_irq_register_ex(nic_osal_device_t *dev,
                           nic_osal_irq_t *irq,
                           nic_osal_irq_handler_t handler,
                           void *arg,
                           const char *name,
                           uint32_t flags);

/*
 * Convenience wrapper for standard NIC-driver registration.
 *
 * Default behavior:
 * - INTR_TYPE_NET
 * - INTR_MPSAFE
 */
int nic_os_irq_register(nic_osal_device_t *dev,
                        nic_osal_irq_t *irq,
                        nic_osal_irq_handler_t handler,
                        void *arg,
                        const char *name);

/*
 * Unregister an interrupt handler.
 *
 * Safe to call on NULL/uninitialized objects; such calls are ignored.
 *
 * Notes:
 * - For internally allocated IRQs, unregister() tears down the handler and
 *   releases the IRQ resource, but preserves the OSAL object for later
 *   re-registration or final nic_os_irq_release().
 * - For preallocated IRQs, unregister() tears down only the handler.
 */
void nic_os_irq_unregister(nic_osal_device_t *dev,
                           nic_osal_irq_t *irq);

/*
 * Release an OSAL IRQ object.
 *
 * Returns:
 * - 0 on success
 * - EBUSY if still registered
 * - EINVAL if invalid/uninitialized
 *
 * Notes:
 * - For preallocated IRQs, this releases only the OSAL wrapper state.
 * - For internally allocated IRQs, any remaining internally owned resource is
 *   also released here as a safety net.
 * - Caller must not use irq after successful release unless reinitialized.
 */
int nic_os_irq_release(nic_osal_irq_t *irq);

/*
 * Enable interrupt callback dispatch.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if irq is invalid/uninitialized
 *
 * Notes:
 * - This is software gating only.
 * - It does not program device interrupt mask registers.
 */
int nic_os_irq_enable(nic_osal_irq_t *irq);

/*
 * Disable interrupt callback dispatch.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if irq is invalid/uninitialized
 *
 * Notes:
 * - This is software gating only.
 * - It does not program device interrupt mask registers.
 */
int nic_os_irq_disable(nic_osal_irq_t *irq);

/*
 * Query whether an IRQ handler is currently registered.
 *
 * Returns:
 * - 1 if registered
 * - 0 otherwise
 */
int nic_os_irq_is_registered(nic_osal_irq_t *irq);

/*
 * Query whether interrupt callback dispatch is currently enabled.
 *
 * Returns:
 * - 1 if enabled
 * - 0 otherwise
 */
int nic_os_irq_is_enabled(nic_osal_irq_t *irq);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_IRQ_H */
