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

#ifndef NIC_OSAL_LOG_H
#define NIC_OSAL_LOG_H

#include "osal_types.h"

#ifdef _KERNEL
#include <machine/stdarg.h>
#else
#include <stdarg.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void nic_os_log_error(const char *fmt, ...);
void nic_os_log_warn(const char *fmt, ...);
void nic_os_log_info(const char *fmt, ...);
void nic_os_log_debug(const char *fmt, ...);

void nic_os_assert_fail(const char *expr, const char *file, int line);

#define NIC_OS_ASSERT(expr) \
    do { \
        if (!(expr)) \
            nic_os_assert_fail(#expr, __FILE__, __LINE__); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_LOG_H */
