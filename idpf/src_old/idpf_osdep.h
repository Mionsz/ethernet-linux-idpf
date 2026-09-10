/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDPF_OSDEP_H_
#define _IDPF_OSDEP_H_

#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/param.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/bus.h>

#include "idpf_type_compat.h"
#include "idpf_list.h"
#include "idpf_alloc.h"

struct idpf_hw;

typedef struct mtx idpf_lock;
typedef bus_size_t resource_size_t;

#ifndef __iomem
#define __iomem
#endif

#ifndef fallthrough
#define fallthrough			((void)0)
#endif

#ifndef ETH_ALEN
#define ETH_ALEN			6
#endif

#ifndef IDPF_DEV_ID_VF_SIOV
#define IDPF_DEV_ID_VF_SIOV		0x0DD5
#endif
#ifndef IDPF_DEV_ID_PF_SIMICS
#define IDPF_DEV_ID_PF_SIMICS		0xF002
#endif
#ifndef IDPF_DEV_ID_VF_SIMICS
#define IDPF_DEV_ID_VF_SIMICS		0xF00C
#endif
#ifndef IDPF_SUBDEV_ID_SIMICS
#define IDPF_SUBDEV_ID_SIMICS		0x12D1
#endif
#ifndef IDPF_SUBDEV_ID_EMR
#define IDPF_SUBDEV_ID_EMR		0xF0D1
#endif
#ifndef IS_SIMICS_DEVICE
#define IS_SIMICS_DEVICE(subdev)	((subdev) == IDPF_SUBDEV_ID_SIMICS)
#endif
#ifndef IS_EMR_DEVICE
#define IS_EMR_DEVICE(subdev)		((subdev) == IDPF_SUBDEV_ID_EMR)
#endif
#ifndef IS_SILICON_DEVICE
#define IS_SILICON_DEVICE(subdev)	\
    (!IS_SIMICS_DEVICE(subdev) && !IS_EMR_DEVICE(subdev))
#endif

#define CPU_TO_LE16(value)	htole16(value)
#define CPU_TO_LE32(value)	htole32(value)
#define LE16_TO_CPU(value)	le16toh(value)
#define LE32_TO_CPU(value)	le32toh(value)
#define IDPF_HI_DWORD(value)	((u32)(((u64)(value) >> 32) & 0xffffffffULL))
#define IDPF_LO_DWORD(value)	((u32)((u64)(value) & 0xffffffffULL))

#define IDPF_SUCCESS			0
#define IDPF_ERR_PARAM			EINVAL
#define IDPF_ERR_CFG			EINVAL
#define IDPF_ERR_NO_MEMORY		ENOMEM
#define IDPF_ERR_CTLQ_ERROR		EBADMSG
#define IDPF_ERR_CTLQ_EMPTY		ENOBUFS
#define IDPF_ERR_CTLQ_FULL		ENOSPC
#define IDPF_ERR_CTLQ_NO_WORK		ENOMSG

void *idpf_calloc(struct idpf_hw *hw, int count, size_t size);
void idpf_free(struct idpf_hw *hw, void *ptr);
void *idpf_memset(void *addr, int value, size_t size,
    enum idpf_memset_type type);
void *idpf_memcpy(void *dst, const void *src, size_t size,
    enum idpf_memcpy_type type);
void idpf_init_lock(idpf_lock *lock);
void idpf_acquire_lock(idpf_lock *lock);
void idpf_release_lock(idpf_lock *lock);
void idpf_destroy_lock(idpf_lock *lock);

#define idpf_wmb()	atomic_thread_fence_rel()
#define idpf_rmb()	atomic_thread_fence_acq()

#endif /* _IDPF_OSDEP_H_ */