/*
 * osal_log.c
 *
 * FreeBSD kernel logging backend for the OSAL.
 *
 * Kernel context has no stdio; diagnostics go through printf(9) and fatal
 * assertions through panic(9).
 */

#include "osal_log.h"

#include <sys/param.h>
#include <sys/systm.h>

static void
nic_os_vlog(const char *level, const char *fmt, va_list ap)
{
    printf("[%s] ", level);
    vprintf(fmt, ap);
}

void
nic_os_log_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    nic_os_vlog("ERR", fmt, ap);
    va_end(ap);
}

void
nic_os_log_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    nic_os_vlog("WRN", fmt, ap);
    va_end(ap);
}

void
nic_os_log_info(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    nic_os_vlog("INF", fmt, ap);
    va_end(ap);
}

void
nic_os_log_debug(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    nic_os_vlog("DBG", fmt, ap);
    va_end(ap);
}

void
nic_os_assert_fail(const char *expr, const char *file, int line)
{
    panic("ASSERT FAILED: %s (%s:%d)", expr, file, line);
}
