/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/* Userspace stand-in for sys/mutex.h. Tests are single threaded, so the lock
 * only has to track ownership well enough for mtx_assert(). */

#ifndef _IDPF_MOCK_MUTEX_H_
#define _IDPF_MOCK_MUTEX_H_

#include <sys/systm.h>

#define MTX_DEF		0x00000000
#define MTX_SPIN	0x00000001
#define MTX_RECURSE	0x00000004
#define MTX_NEW		0x00000200

#define MA_OWNED	0x01
#define MA_NOTOWNED	0x02

struct mtx {
	const char	*name;
	int		 depth;
	int		 initialised;
};

static inline void
mtx_init(struct mtx *m, const char *name, const char *type __attribute__((unused)),
    int opts __attribute__((unused)))
{

	m->name = name;
	m->depth = 0;
	m->initialised = 1;
}

static inline void
mtx_destroy(struct mtx *m)
{

	KASSERT(m->depth == 0, ("mtx %s destroyed while held", m->name));
	m->initialised = 0;
}

static inline void
mtx_lock(struct mtx *m)
{

	KASSERT(m->initialised, ("mtx locked before mtx_init"));
	KASSERT(m->depth == 0, ("mtx %s recursed", m->name));
	m->depth++;
}

static inline void
mtx_unlock(struct mtx *m)
{

	KASSERT(m->depth == 1, ("mtx %s released while not held", m->name));
	m->depth--;
}

static inline void
mtx_assert_(const struct mtx *m, int what)
{

	if (what == MA_OWNED)
		KASSERT(m->depth == 1, ("mtx %s not held", m->name));
	else if (what == MA_NOTOWNED)
		KASSERT(m->depth == 0, ("mtx %s unexpectedly held", m->name));
}

#define mtx_assert(m, what)	mtx_assert_((m), (what))

#endif /* _IDPF_MOCK_MUTEX_H_ */
