/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDPF_MEM_H_
#define _IDPF_MEM_H_

/*
 * DMA memory descriptor and mailbox register accessors used by the control
 * queue layer.
 *
 * FreeBSD port notes
 * ------------------
 * dma_addr_t becomes bus_addr_t, and the descriptor carries the bus_dma tag
 * and map that idpf_alloc_dma_mem() creates, because FreeBSD needs both to
 * release the mapping again.
 *
 * The mailbox accessors are the same MMIO seam as idpf_reg_wr32() in
 * idpf_txrx.h, restated here because this is the lowest-level header in the
 * driver and cannot include the datapath one.  [FBSD15:A31]
 */

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/systm.h>

#include "idpf_types.h"

#include <machine/atomic.h>
#include <machine/bus.h>

struct idpf_hw;

struct idpf_dma_mem {
	void		*va;
	bus_addr_t	 pa;
	bus_size_t	 size;
	bus_dma_tag_t	 tag;
	bus_dmamap_t	 map;
};

void *idpf_alloc_dma_mem(struct idpf_hw *hw, struct idpf_dma_mem *mem,
    u64 size);
void idpf_free_dma_mem(struct idpf_hw *hw, struct idpf_dma_mem *mem);

static inline void
idpf_mmio_wr32(void *addr, u32 value)
{

	atomic_thread_fence_rel();
	*(volatile u32 *)addr = htole32(value);
}

static inline u32
idpf_mmio_rd32(void *addr)
{
	u32 value;

	value = le32toh(*(volatile u32 *)addr);
	atomic_thread_fence_acq();

	return (value);
}

static inline void
idpf_mmio_wr64(void *addr, u64 value)
{

	atomic_thread_fence_rel();
	*(volatile u64 *)addr = htole64(value);
}

static inline u64
idpf_mmio_rd64(void *addr)
{
	u64 value;

	value = le64toh(*(volatile u64 *)addr);
	atomic_thread_fence_acq();

	return (value);
}

#define idpf_mbx_wr32(a, reg, value) \
	idpf_mmio_wr32((u8 *)(a)->mbx.vaddr + (reg), (value))
#define idpf_mbx_rd32(a, reg) \
	idpf_mmio_rd32((u8 *)(a)->mbx.vaddr + (reg))
#define idpf_mbx_wr64(a, reg, value) \
	idpf_mmio_wr64((u8 *)(a)->mbx.vaddr + (reg), (value))
#define idpf_mbx_rd64(a, reg) \
	idpf_mmio_rd64((u8 *)(a)->mbx.vaddr + (reg))

#define wr32(a, reg, value)	idpf_mbx_wr32(a, reg, value)
#define rd32(a, reg)		idpf_mbx_rd32(a, reg)
#define wr64(a, reg, value)	idpf_mbx_wr64(a, reg, value)
#define rd64(a, reg)		idpf_mbx_rd64(a, reg)

#endif /* _IDPF_MEM_H_ */
