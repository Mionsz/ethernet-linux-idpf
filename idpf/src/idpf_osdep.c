/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

#include "idpf_osdep.h"

void *
idpf_calloc(struct idpf_hw *hw, int count, size_t size)
{

	(void)hw;
	if (count <= 0 || size > (size_t)-1 / (size_t)count)
		return (NULL);

	return (malloc((size_t)count * size, M_DEVBUF, M_NOWAIT | M_ZERO));
}

void
idpf_free(struct idpf_hw *hw, void *ptr)
{

	(void)hw;
	free(ptr, M_DEVBUF);
}

void *
idpf_memset(void *addr, int value, size_t size, enum idpf_memset_type type)
{

	(void)type;
	return (memset(addr, value, size));
}

void *
idpf_memcpy(void *dst, const void *src, size_t size,
    enum idpf_memcpy_type type)
{

	(void)type;
	return (memcpy(dst, src, size));
}

void
idpf_init_lock(idpf_lock *lock)
{

	mtx_init(lock, "idpf ctlq", NULL, MTX_DEF);
}

void
idpf_acquire_lock(idpf_lock *lock)
{

	mtx_lock(lock);
}

void
idpf_release_lock(idpf_lock *lock)
{

	mtx_unlock(lock);
}

void
idpf_destroy_lock(idpf_lock *lock)
{

	mtx_destroy(lock);
}