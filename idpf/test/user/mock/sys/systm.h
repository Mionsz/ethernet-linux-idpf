/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Userspace stand-in for sys/systm.h.
 *
 * Only kernel-only headers are shadowed.  sys/param.h, sys/queue.h,
 * sys/endian.h and machine/atomic.h are used unmodified from the base system,
 * so endianness helpers and the TAILQ macros are the real implementations.
 */

#ifndef _IDPF_MOCK_SYSTM_H_
#define _IDPF_MOCK_SYSTM_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#ifndef nitems
#define nitems(x)	(sizeof((x)) / sizeof((x)[0]))
#endif

#define KASSERT(exp, msg) do {						\
	if (!(exp)) {							\
		printf("KASSERT failed at %s:%d: ", __FILE__, __LINE__);	\
		printf msg;						\
		printf("\n");						\
		abort();						\
	}								\
} while (0)

#define panic(...) do {							\
	printf("panic: " __VA_ARGS__);					\
	printf("\n");							\
	abort();							\
} while (0)

#endif /* _IDPF_MOCK_SYSTM_H_ */
