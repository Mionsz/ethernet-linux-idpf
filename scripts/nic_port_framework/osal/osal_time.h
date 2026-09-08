/*
 * osal_time.h
 *
 * FreeBSD-oriented OS abstraction for monotonic time, sleep/delay,
 * and timer services.
 *
 * Design notes:
 * - nic_os_time_ms() / nic_os_time_us() return monotonic uptime-based time.
 * - nic_os_sleep_ms() is for sleepable context only.
 * - nic_os_delay_us() is a busy-wait delay for short delays.
 * - Timers are implemented with callout(9).
 * - Timer callbacks run in callout context and must not assume they may sleep.
 *
 * Return conventions:
 * - Functions returning int use FreeBSD-style positive errno values:
 *     0        success
 *     EINVAL   invalid argument/state
 *     EBUSY    already initialized or being destroyed
 *     ENOMEM   allocation failure
 *
 * Typical lifecycle:
 *
 *     nic_osal_timer_t timer = {0};
 *
 *     nic_os_timer_init(&timer, my_timer_cb, my_arg);
 *     nic_os_timer_start(&timer, 1000, false);  // one-shot after 1000 ms
 *     nic_os_timer_stop(&timer);
 *     nic_os_timer_destroy(&timer);
 *
 * Periodic example:
 *
 *     nic_os_timer_init(&timer, my_timer_cb, my_arg);
 *     nic_os_timer_start(&timer, 250, true);    // periodic every 250 ms
 *     nic_os_timer_stop(&timer);
 *     nic_os_timer_destroy(&timer);
 */

#ifndef NIC_OSAL_TIME_H
#define NIC_OSAL_TIME_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return monotonic uptime in milliseconds.
 */
uint64_t nic_os_time_ms(void);

/*
 * Return monotonic uptime in microseconds.
 */
uint64_t nic_os_time_us(void);

/*
 * Sleep/block the current thread for approximately ms milliseconds.
 *
 * Notes:
 * - Sleepable context only.
 * - A value of 0 returns immediately.
 */
void nic_os_sleep_ms(uint32_t ms);

/*
 * Busy-wait for approximately us microseconds.
 *
 * Notes:
 * - Intended for short hardware-oriented delays.
 * - Should not be used for long waits.
 */
void nic_os_delay_us(uint32_t us);

/*
 * Initialize a timer object.
 *
 * Parameters:
 * - timer: caller-owned timer object
 * - callback: timer callback
 * - arg: callback argument
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid
 * - EBUSY if timer is already initialized
 * - ENOMEM on allocation failure
 */
int nic_os_timer_init(nic_osal_timer_t *timer,
                      void (*callback)(void *arg),
                      void *arg);

/*
 * Destroy a timer object.
 *
 * Semantics:
 * - Stops and drains the timer if active.
 * - Safe to call on NULL/uninitialized timer; such calls are ignored.
 */
void nic_os_timer_destroy(nic_osal_timer_t *timer);

/*
 * Start or restart a timer.
 *
 * Parameters:
 * - timer: timer object
 * - timeout_ms: timeout in milliseconds; 0 is rounded up to minimum delay
 * - periodic: true for periodic timer, false for one-shot
 *
 * Returns:
 * - 0 on success
 * - EINVAL if timer is invalid/uninitialized
 * - EBUSY if timer is being destroyed
 */
int nic_os_timer_start(nic_osal_timer_t *timer,
                       uint32_t timeout_ms,
                       bool periodic);

/*
 * Stop a timer.
 *
 * Semantics:
 * - Stops the timer if active and drains any in-flight callback.
 * - Safe to call on NULL/uninitialized timer; such calls are ignored.
 */
void nic_os_timer_stop(nic_osal_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_TIME_H */
