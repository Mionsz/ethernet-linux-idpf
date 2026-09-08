/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Minimal in-kernel assertion framework for the idpf test module.
 *
 * Runs in kernel context on purpose: the driver's locks, condition variables
 * and taskqueues are the real ones, so ordering and sleep-vs-spin errors show
 * up here rather than being mocked away.
 */

#ifndef _IDPF_TEST_H_
#define _IDPF_TEST_H_

#include <sys/param.h>
#include <sys/systm.h>

extern int idpf_test_checks;
extern int idpf_test_failures;
extern const char *idpf_test_current;

#define IDPF_EXPECT(cond, fmt, ...) do {				\
	idpf_test_checks++;						\
	if (!(cond)) {							\
		idpf_test_failures++;					\
		printf("idpf_test:   FAIL %s:%d: " fmt "\n",		\
		    idpf_test_current, __LINE__, ##__VA_ARGS__);	\
	}								\
} while (0)

#define IDPF_EXPECT_EQ(got, want) do {					\
	intmax_t _g = (intmax_t)(got), _w = (intmax_t)(want);		\
	IDPF_EXPECT(_g == _w, "%s: got %jd want %jd", #got, _g, _w);	\
} while (0)

#define IDPF_EXPECT_PTR_EQ(got, want)					\
	IDPF_EXPECT((const void *)(got) == (const void *)(want),	\
	    "%s: got %p want %p", #got, (const void *)(got),		\
	    (const void *)(want))

#define IDPF_EXPECT_NULL(p)						\
	IDPF_EXPECT((p) == NULL, "%s: expected NULL, got %p", #p,	\
	    (const void *)(p))

#define IDPF_EXPECT_NOT_NULL(p)						\
	IDPF_EXPECT((p) != NULL, "%s: unexpectedly NULL", #p)

#define IDPF_EXPECT_OK(expr) do {					\
	int _e = (expr);						\
	IDPF_EXPECT(_e == 0, "%s: expected 0, got %d", #expr, _e);	\
} while (0)

/* Externally visible driver calls return positive errno. */
#define IDPF_EXPECT_ERR(expr, want) do {				\
	int _e = (expr);						\
	IDPF_EXPECT(_e == (want), "%s: got %d want %d", #expr, _e,	\
	    (want));							\
} while (0)

struct idpf_test_case {
	const char	*name;
	void		(*fn)(void);
};

struct idpf_test_suite {
	const char			*name;
	const struct idpf_test_case	*cases;
	int				 ncases;
};

#define IDPF_TEST_SUITE(sname, carray)					\
	{ .name = (sname), .cases = (carray),				\
	  .ncases = nitems(carray) }

int idpf_test_run_suite(const struct idpf_test_suite *suite);

/* Per-suite registration. */
extern const struct idpf_test_suite idpf_test_suite_attach;
extern const struct idpf_test_suite idpf_test_suite_ctlq;

#endif /* _IDPF_TEST_H_ */
