/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Userspace backing for the kernel primitives the control queue layer uses.
 *
 * Allocations are counted so tests can assert the driver frees everything it
 * takes, which is the check that matters most for the setup/teardown paths.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <machine/bus.h>

#include "idpf_controlq.h"

/* This file implements the wrappers, so it needs the real libc entry points. */
#undef malloc
#undef free

struct malloc_type M_DEVBUF[1] = { { "devbuf" } };
struct malloc_type M_TEMP[1] = { { "temp" } };

static long alloc_count;
static long sync_count;

void *
idpf_test_kmalloc(size_t size, int flags)
{
	void *p;

	p = malloc(size);
	if (p == NULL)
		return (NULL);

	if ((flags & M_ZERO) != 0)
		memset(p, 0, size);

	alloc_count++;

	return (p);
}

void
idpf_test_kfree(void *ptr)
{

	if (ptr == NULL)
		return;

	alloc_count--;
	free(ptr);
}

long
idpf_test_alloc_count(void)
{

	return (alloc_count);
}

void
idpf_test_dmamap_sync(bus_dma_tag_t tag __unused, bus_dmamap_t map __unused,
    int op __unused)
{

	sync_count++;
}

long
idpf_test_sync_count(void)
{

	return (sync_count);
}

/*
 * DMA memory is ordinary page-aligned memory.  The "physical" address is the
 * virtual one, which is enough for the driver's split into high and low
 * halves to be checked, and is guaranteed non-zero.
 */
void *
idpf_alloc_dma_mem(struct idpf_hw *hw __unused, struct idpf_dma_mem *mem,
    uint64_t size)
{
	bus_size_t sz = roundup2(size, 4096);
	void *va = NULL;

	if (posix_memalign(&va, 4096, sz) != 0)
		return (NULL);

	memset(va, 0, sz);

	mem->va = va;
	mem->pa = (bus_addr_t)(uintptr_t)va;
	mem->size = sz;
	mem->tag = NULL;
	mem->map = NULL;

	alloc_count++;

	return (mem->va);
}

void
idpf_free_dma_mem(struct idpf_hw *hw __unused, struct idpf_dma_mem *mem)
{

	if (mem->va == NULL)
		return;

	alloc_count--;
	free(mem->va);

	mem->va = NULL;
	mem->pa = 0;
	mem->size = 0;
}
