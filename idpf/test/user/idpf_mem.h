/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDPF_USER_TEST_MEM_H_
#define _IDPF_USER_TEST_MEM_H_

#include <stdatomic.h>
#include <sys/endian.h>
#include <sys/queue.h>

#include <machine/bus.h>

#include "idpf_type_compat.h"
#include "idpf_alloc.h"

struct idpf_hw;

typedef int idpf_lock;
typedef bus_size_t resource_size_t;

struct idpf_dma_mem {
	void *va;
	bus_addr_t pa;
	bus_size_t size;
	bus_dma_tag_t tag;
	bus_dmamap_t map;
};

#ifndef __iomem
#define __iomem
#endif

#ifndef fallthrough
#define fallthrough ((void)0)
#endif

#define CPU_TO_LE16(value) htole16(value)
#define CPU_TO_LE32(value) htole32(value)
#define LE16_TO_CPU(value) le16toh(value)
#define LE32_TO_CPU(value) le32toh(value)
#define IDPF_HI_DWORD(value) ((u32)(((u64)(value) >> 32) & 0xffffffffULL))
#define IDPF_LO_DWORD(value) ((u32)((u64)(value) & 0xffffffffULL))

#define IDPF_SUCCESS 0
#define IDPF_ERR_PARAM EINVAL
#define IDPF_ERR_CFG EINVAL
#define IDPF_ERR_NO_MEMORY ENOMEM
#define IDPF_ERR_CTLQ_ERROR EBADMSG
#define IDPF_ERR_CTLQ_EMPTY ENOBUFS
#define IDPF_ERR_CTLQ_FULL ENOSPC
#define IDPF_ERR_CTLQ_NO_WORK ENOMSG

#define IDPF_SUBDEV_ID_SIMICS 0x12D1
#define IDPF_SUBDEV_ID_EMR 0xF0D1
#define IS_SIMICS_DEVICE(subdev) ((subdev) == IDPF_SUBDEV_ID_SIMICS)
#define IS_EMR_DEVICE(subdev) ((subdev) == IDPF_SUBDEV_ID_EMR)
#define IS_SILICON_DEVICE(subdev) \
	(!IS_SIMICS_DEVICE(subdev) && !IS_EMR_DEVICE(subdev))

#define LIST_ENTRY_TYPE(type) LIST_ENTRY(type)
#define LIST_HEAD_TYPE(name, type) LIST_HEAD(name, type)
#define LIST_FOR_EACH_ENTRY(pos, head, type, member) \
	LIST_FOREACH((pos), (head), member)
#define LIST_FOR_EACH_ENTRY_SAFE(pos, tmp, head, type, member) \
	LIST_FOREACH_SAFE((pos), (head), member, (tmp))

#define atomic_thread_fence_rel() atomic_thread_fence(memory_order_release)
#define atomic_thread_fence_acq() atomic_thread_fence(memory_order_acquire)
#define idpf_wmb() atomic_thread_fence(memory_order_release)
#define idpf_rmb() atomic_thread_fence(memory_order_acquire)

void idpf_test_wr32(struct idpf_hw *hw, u32 reg, u32 value);
u32 idpf_test_rd32(struct idpf_hw *hw, u32 reg);

#define wr32(hw, reg, value) idpf_test_wr32((hw), (reg), (value))
#define rd32(hw, reg) idpf_test_rd32((hw), (reg))

void idpf_init_lock(idpf_lock *lock);
void idpf_acquire_lock(idpf_lock *lock);
void idpf_release_lock(idpf_lock *lock);
void idpf_destroy_lock(idpf_lock *lock);
void *idpf_calloc(struct idpf_hw *hw, int count, size_t size);
void idpf_free(struct idpf_hw *hw, void *ptr);
void *idpf_memset(void *addr, int value, size_t size,
    enum idpf_memset_type type);
void *idpf_memcpy(void *dst, const void *src, size_t size,
    enum idpf_memcpy_type type);

#endif /* _IDPF_USER_TEST_MEM_H_ */