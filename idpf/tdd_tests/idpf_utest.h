/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2024 Intel Corporation
 *
 * idpf/tdd_tests/idpf_utest.h
 *
 * @file idpf_utest.h
 * @brief Central test-support header for the IDPF VF CppUTest harness
 *        targeting FreeBSD >= 15.0.
 *
 * NOTE: If your test file uses any C++ standard library headers
 * (<fstream>, <string>, <vector>, etc.), include them BEFORE this
 * header. The mock kernel headers pulled in here set sys/types.h
 * guards that libc++ depends on for ios_base/streamsize resolution.
 *
 * @details
 * Thin wrapper around the shared common/utest.h layer (CppUTest +
 * detour_function support: USE_MOCK / USE_STD_MOCK / TGN macros).
 * Reuse-first principle: the common/ layer is the proven, shared
 * foundation used across sibling FreeBSD NIC drivers (iavf, ice).
 * This header adds only IDPF-specific teardown and FreeBSD-specific
 * symbol-collision resolution.
 *
 * @par FreeBSD vs Linux hosting differences
 *
 * The original Linux-hosted version of this header pre-included
 * <cstdlib>/<cstring>/<strings.h> to force glibc symbol resolution
 * before common/mock_inc/sys/libkern.h's own declarations. That
 * specific pre-include is unnecessary on FreeBSD (the FreeBSD C++
 * standard library does not need it), but the underlying collision
 * itself is NOT Linux/glibc-specific:
 *   - Reproduced directly on FreeBSD 15.0-RELEASE-p4 (clang 19.1.7):
 *     common/mock_inc/sys/libkern.h's own declarations of abs, labs,
 *     ffs, ffsl, ffsll, fls, flsl, flsll, random, index, and rindex
 *     collide with the REAL FreeBSD system's <stdlib.h>/<strings.h>
 *     declarations of the same standard C functions when this
 *     userspace CppUTest binary links against the real FreeBSD libc --
 *     "static declaration follows non-static declaration" (13 errors
 *     without -D_SYS_LIBKERN_H_).
 *
 *   - -D_SYS_LIBKERN_H_ (set in tdd_tests/Makefile) is therefore
 *     required on FreeBSD too, not just as a Linux workaround.
 *     Suppressing the whole header is safe for this specific test
 *     binary: none of the currently-used common/mock_src files call
 *     bitcount/bitcount32/bitcount64/bitcountl (the symbols libkern.h
 *     uniquely provides); powerof2/roundup2 (used by mock_sbuf.cpp)
 *     are defined in common/mock_inc/sys/_param.h, not libkern.h, and
 *     remain available regardless.
 *
 *   - The glibc-specific pre-include of <cstdlib>/<cstring>/<strings.h>
 *     is still unnecessary on FreeBSD -- only the header-suppression
 *     flag is needed here, not the pre-include workaround.
 *
 * @par Toolchain
 * LLVM clang (FreeBSD 15.0 base system compiler).
 * BSD make(1) — /usr/bin/make.
 * CppUTest: pkg install cpputest (devel/cpputest in ports).
 *
 * @par Build flags
 * The following flags are set in tdd_tests/Makefile and must NOT be
 * set here to avoid double-definition:
 *   -DFREEBSD_SUPPORT  Signal FreeBSD OS to shared code and mock stubs.
 *   -DIDPF_TDD       Enable test-only code paths in driver source.
 *   -DIDPF_VF        Build the VF (IDPF) variant.
 *   -D_SYS_LIBKERN_H_  Required on FreeBSD too -- see above; suppresses
 *                       common/mock_inc/sys/libkern.h to avoid its
 *                       collision with the real system's libc.
 *
 * @see common/utest.h         Shared CppUTest + detour_function layer.
 * @see tdd_tests/Makefile      Build system entry point.
 * @see CI/README.md §3         Execution boundary documentation.
 */

#ifndef _IDPF_UTEST_H_
#define _IDPF_UTEST_H_

/*
 * On FreeBSD >= 15.0, the CppUTest harness runs in userspace against
 * FreeBSD libc and libc++. No pre-emptive glibc symbol pre-include
 * (<cstdlib>/<cstring>/<strings.h>) is needed. The common/ layer's
 * mock_inc/sys/libkern.h header IS suppressed via -D_SYS_LIBKERN_H_
 * (set in tdd_tests/Makefile) -- required on FreeBSD too, since its
 * declarations collide with the real system's own libc declarations
 * of the same standard C functions (see the @par FreeBSD vs Linux
 * hosting differences block above for the full, empirically-verified
 * rationale).
 *
 * Include order: common/utest.h must be included before any driver
 * source or mock_src files to ensure CppUTest and detour_function
 * types are fully defined before use.
 */
#include "../../common/utest.h"

#define hex2ascii(digit) ((digit) <= 9 ? '0' + (digit) : 'a' - 10 + (digit))
/*
 * imax compatibility shim.
 *
 * mock_sbuf.cpp calls imax(). FreeBSD's <sys/param.h> already defines
 * imax as a macro; common/mock_inc/sys/libkern.h would also define it,
 * but that header is suppressed via -D_SYS_LIBKERN_H_. Since <sys/param.h>
 * is transitively included before this header, we must #undef before
 * redefining to avoid -Wmacro-redefined. The redefinition routes through
 * an inline function to avoid double-evaluation of side-effecting arguments.
 */
static inline int
idpf_utest_imax(int a, int b)
{
    return (a > b ? a : b);
}
#undef imax
#define imax(a, b) idpf_utest_imax((a), (b))

/**
 * @brief Standard per-test teardown for the IDPF CppUTest harness.
 *
 * @details
 * Verifies that all CppUTest mock expectations registered during a
 * test were satisfied, then clears all mock state in preparation for
 * the next test. Must be called at the end of every TEST_GROUP's
 * teardown() method without exception.
 *
 * Failure to call this function will silently suppress unmet
 * expectation failures, masking real test regressions.
 *
 * Follows the teardown convention documented in common/utest.h:
 *   mock().checkExpectations();
 *   mock().clear();
 *
 * @par Usage
 * @code
 * TEST_GROUP(my_group) {
 *     void teardown(void) {
 *         idpf_common_teardown();
 *     }
 * };
 * @endcode
 *
 * @note This function does not acquire any locks and does not interact
 *       with driver hardware state. It is safe to call from any
 *       CppUTest teardown() context.
 */
static inline void
idpf_common_teardown(void)
{
	mock().checkExpectations();
	mock().clear();
}

#endif /* _IDPF_UTEST_H_ */
