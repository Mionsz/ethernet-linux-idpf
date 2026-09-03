/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Userspace stand-in for sys/malloc.h.
 *
 * The kernel's three-argument malloc() and two-argument free() are redirected
 * to counted wrappers, which also gives the tests a leak check.  Only driver
 * sources include this; the test file uses idpf_test_alloc_count() rather than
 * calling libc malloc under the macro.
 */

#ifndef _IDPF_MOCK_MALLOC_H_
#define _IDPF_MOCK_MALLOC_H_

#include <sys/systm.h>

#define M_WAITOK	0x0002
#define M_NOWAIT	0x0001
#define M_ZERO		0x0100
#define M_NODUMP	0x0800

struct malloc_type {
	const char *ks_shortdesc;
};

#define MALLOC_DECLARE(type)	extern struct malloc_type type[1]
#define MALLOC_DEFINE(type, shortdesc, longdesc)			\
	struct malloc_type type[1] = { { shortdesc } }

extern struct malloc_type M_DEVBUF[1];
extern struct malloc_type M_TEMP[1];

void *idpf_test_kmalloc(size_t size, int flags);
void idpf_test_kfree(void *ptr);

/* Outstanding allocations, for leak assertions. */
long idpf_test_alloc_count(void);

#define malloc(size, type, flags)	idpf_test_kmalloc((size), (flags))
#define free(addr, type)		idpf_test_kfree((addr))

#endif /* _IDPF_MOCK_MALLOC_H_ */
