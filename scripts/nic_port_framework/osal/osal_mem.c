#include "osal_mem.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/libkern.h>

MALLOC_DEFINE(M_NIC_OSAL, "nic_osal", "NIC OSAL allocations");

void *
nic_os_malloc(size_t size)
{
    if (size == 0)
        return NULL;

    return malloc(size, M_NIC_OSAL, M_NOWAIT);
}

void *
nic_os_calloc(size_t count, size_t size)
{
    size_t total;

    if (count == 0 || size == 0)
        return NULL;

    if (count > SIZE_MAX / size)
        return NULL;

    total = count * size;
    return malloc(total, M_NIC_OSAL, M_NOWAIT | M_ZERO);
}

void *
nic_os_zalloc(size_t size)
{
    if (size == 0)
        return NULL;

    return malloc(size, M_NIC_OSAL, M_NOWAIT | M_ZERO);
}

void
nic_os_free(void *ptr)
{
    if (ptr == NULL)
        return;

    free(ptr, M_NIC_OSAL);
}

void *
nic_os_memcpy(void *dst, const void *src, size_t size)
{
    return memcpy(dst, src, size);
}

void *
nic_os_memset(void *dst, int value, size_t size)
{
    return memset(dst, value, size);
}

int
nic_os_memcmp(const void *a, const void *b, size_t size)
{
    return memcmp(a, b, size);
}
