# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2024 Intel Corporation
#
# idpf/tdd_tests/Makefile
#
# BSD make(1) build file for the IDPF VF CppUTest userspace unit-test
# harness targeting FreeBSD >= 15.0.
#
# Toolchain discipline:
#   - clang++ only (LLVM, FreeBSD 15.0 base system compiler).
#   - BSD make(1) only (/usr/bin/make). GNU make must not be used.
#   - No -fpermissive: compilation errors must be fixed, not suppressed.
#   - -D_SYS_LIBKERN_H_ IS required, even natively on FreeBSD 15.0-RELEASE
#     (clang 19.1.7): see the NOTE below and idpf_utest.h for the full,
#     empirically-verified rationale.
#
# Design:
#   Driver logic is compiled for userspace with FreeBSD kernel and
#   hardware seams mocked via the reused common/ mock layer. Tests run
#   without kldload(8). See CI/README.md §3 for the execution boundary.
#
#   mock_src/*.cpp files are NOT compiled as separate objects. Per
#   common/mock_src/README.md, each test .cpp file #includes exactly
#   the mock_src files it needs, directly after idpf_utest.h, where
#   CppUTestExt's MockSupport.h (providing mock()) is already visible.
#
# Targets:
#   help                - print this help menu (default when no target is given)
#   all                 - build test binary without running
#   run                 - build and run tests with verbose output
#   junit                - build and run tests with JUnit XML output for CI
#   coverage            - build with LLVM coverage instrumentation, run, report
#   idpf-osdep-abi-check - Phase-0 OS-shim ABI/compile-only isolation gate
#   clean               - remove all build artefacts
#
# External release gating:
#   To strip innersource-only code sections, add to CXXFLAGS:
#     CXXFLAGS+= -DEXTERNAL_RELEASE
#   Leave undefined for internal/innersource builds (default).
# ── Toolchain ─────────────────────────────────────────────────────────
#
# CXX: clang++ from FreeBSD 15.0 base system.
# Justified: FreeBSD 15.0 ships LLVM/clang as the base system compiler.
# GNU g++ is not present in FreeBSD 15.0 base.
#
CXX=		clang++

# ── CppUTest installation prefix ──────────────────────────────────────
#
# CPPUTEST_PREFIX: Installation prefix for CppUTest headers and libraries.
# Default: /usr/local — correct for FreeBSD pkg install cpputest.
# Justified: FreeBSD pkg(8) installs third-party software under
# /usr/local by convention (hier(7)). pkg install cpputest places
# headers in /usr/local/include/CppUTest/ and libraries in
# /usr/local/lib/libCppUTest*.
# Override: make CPPUTEST_PREFIX=/path/to/cpputest
#
CPPUTEST_PREFIX?=	/usr/local

# ── Compiler flags ────────────────────────────────────────────────────
#
# -std=c++14:
#   CppUTest minimum documented C++ standard. C++14 is used (not c++17)
#   because driver .c files are #included directly into test translation
#   units; C++17's stricter rules (e.g., removal of 'register' keyword,
#   stricter initialisation) can break driver C code that was not written
#   for C++17. C++14 is the correct and consistent choice across the
#   common/ layer and sibling drivers (iavf, ice).
#   Justified: CppUTest documentation §"Getting Started"; iavf reference
#   driver tdd_tests/Makefile.
#
# -g -O0:
#   Debug symbols, no optimisation. Required for detour_function to
#   correctly patch function prologues at runtime. Optimisation can
#   inline or reorder functions, breaking detour installation.
#   Justified: detour_function/README.md §"Build Requirements".
#
# -Wall -Wextra:
#   Enable comprehensive warnings. All warnings must be resolved; they
#   are not suppressed via -fpermissive (see below).
#
# -Wno-unused-parameter:
#   Driver and shared code have intentional unused parameters in stub
#   and mock functions. This is the only warning suppression permitted.
#
# -fno-builtin:
#   Prevents clang from replacing malloc/free/memset with compiler
#   builtins, which would bypass the mock stubs in common/mock_src/.
#   Justified: Without this flag, clang may optimise away calls to
#   mocked allocation functions, causing mock expectations to go unmet.
#
# -falign-functions=16:
#   Required by detour_function for reliable function patching on
#   x86_64. Functions must be at least 16 bytes to accommodate the
#   detour trampoline. Without this, short functions may be too small
#   to patch, causing detour installation failures.
#   Justified: detour_function/README.md §"Alignment Requirements";
#   Intel 64 and IA-32 Architectures SDM Vol.1 §6.4 "Procedure Calls".
#
# -fno-inline:
#   Prevents inlining of functions that detour_function needs to patch.
#   Inlined functions have no call site to redirect.
#   Justified: detour_function/README.md §"Build Requirements".
#
# NOTE: -fpermissive is intentionally absent.
#   -fpermissive silently downgrades errors to warnings, masking real
#   porting defects. Any error it would suppress must be fixed in source.
#
# NOTE: -D_SYS_LIBKERN_H_ IS required, on FreeBSD too (not Linux-only).
#   Reproduced directly on this FreeBSD 15.0-RELEASE-p4 host (clang
#   19.1.7): common/mock_inc/sys/libkern.h's own declarations of abs,
#   labs, ffs, ffsl, ffsll, fls, flsl, flsll, random, index, and rindex
#   collide with the REAL system's <stdlib.h>/<strings.h> declarations
#   of the same standard C functions when this userspace CppUTest
#   binary links against the real FreeBSD libc/libc++ -- "static
#   declaration follows non-static declaration" (13 errors without this
#   flag). This is NOT a Linux/glibc-only workaround; the collision is
#   real on native FreeBSD too, at least with this toolchain version.
#   Suppressing the whole header via this flag is safe for this test
#   binary specifically: none of the currently-used common/mock_src
#   files call bitcount/bitcount32/bitcount64/bitcountl (the only
#   symbols libkern.h uniquely provides that this binary might need);
#   powerof2/roundup2 (used by mock_sbuf.cpp) are defined in
#   common/mock_inc/sys/_param.h, not libkern.h, and remain available.
#   See idpf_utest.h for the full rationale.
#
CXXFLAGS=	-std=c++14 -g -O0 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS+=	-fno-builtin
CXXFLAGS+=	-falign-functions=16
CXXFLAGS+=	-fno-inline
CXXFLAGS+=	-D_SYS_LIBKERN_H_

# ── Preprocessor definitions ──────────────────────────────────────────
#
# -DFREEBSD_SUPPORT:
#   Signals the FreeBSD OS path to shared code, idpf_osdep.h, and
#   mock stubs. Required for correct conditional compilation in all
#   shared code and common/ mock layer files.
#
# -DIDPF_VF:
#   Selects the VF (IDPF/Virtchnl2) build variant. The IDPF VF driver
#   is the primary target.
#
# -DIDPF_TDD:
#   Enables test-only code paths in driver source files (e.g., exposing
#   internal state for assertion, disabling hardware-only paths that
#   cannot execute in userspace).
#
# -DIDPF_TDD_PHASE4:
#   Enables Feature 015 (User Story 2) transport-adapter tests in
#   test_idpf_vc_common.cpp. Folded into the default build here per
#   T040 now that struct idpf_vc_transport_ops, the received-message
#   record, and the fake transport (T036-T039) exist; previously
#   gated behind the now-retired phase4-red-check target while those
#   symbols did not yet exist (see git history for that target).
#
CXXFLAGS+=	-DFREEBSD_SUPPORT
CXXFLAGS+=	-DIDPF_VF
CXXFLAGS+=	-DIDPF_TDD
CXXFLAGS+=	-DIDPF_TDD_PHASE4

# ── Include paths ─────────────────────────────────────────────────────
#
# common/mock_inc MUST be added as a plain -I, not -idirafter.
#
# -idirafter places a directory strictly after the compiler's built-in
# default system include path (clang/gcc documented search order: -I
# and -isystem dirs, then default system dirs, then -idirafter dirs).
# On native FreeBSD, /usr/include ships real copies of several headers
# common/mock_inc intentionally stands in for (net/iflib.h,
# sys/gtaskqueue.h, sys/taskqueue.h, machine/bus.h -> x86/bus.h ->
# x86/bus_dma.h -> x86/busdma_impl.h). Those real headers are
# kernel-only (require _KERNEL) and cannot compile in this userspace
# CppUTest binary. With -idirafter, the real system headers are found
# first and the build fails; a plain -I ranks common/mock_inc ahead of
# the default system path, so the intentional mock stand-ins are found
# instead. This mirrors the already-working reference driver's
# precedent: iavf/iavf/src/TDD/Makefile's INCLUDES uses
# -Icommon/mock_inc (plain -I), not -idirafter.
#
CXXFLAGS+=	-I${CPPUTEST_PREFIX}/include
CXXFLAGS+=	-I../src
CXXFLAGS+=	-I../shared
CXXFLAGS+=	-I../shared/virtchnl
CXXFLAGS+=	-I../../common
CXXFLAGS+=	-I ../../common/mock_inc
CXXFLAGS+=	-Ikobjs
CXXFLAGS+=	-IFREEBSD_KERNEL_MOCKS
CXXFLAGS+=	-ICORE_MOCKS

# ── Linker flags ──────────────────────────────────────────────────────
LDFLAGS=	-L${CPPUTEST_PREFIX}/lib
LIBS=		-lCppUTest -lCppUTestExt

# ── LLVM coverage flags ───────────────────────────────────────────────
#
# Used by the 'coverage' target only. Not applied to the default build.
#
# -fprofile-instr-generate:
#   Enables LLVM instrumentation-based profiling (source-based coverage).
#   Justified: LLVM documentation §CommandGuide/llvm-cov; this is the
#   correct flag for clang's source-based coverage on FreeBSD 15.0.
#   gcov-compatible coverage (-fprofile-arcs -ftest-coverage) must NOT
#   be used; FreeBSD 15.0 base is LLVM-only and does not ship gcov.
#
# -fcoverage-mapping:
#   Emits coverage mapping data required by llvm-cov for source
#   annotation. Must be used together with -fprofile-instr-generate.
#   Justified: LLVM documentation §CommandGuide/llvm-cov §"Source-based
#   Code Coverage".
#
COVERAGE_FLAGS=	-fprofile-instr-generate -fcoverage-mapping

# ── LLVM coverage tools ────────────────────────────────────────────────
#
# LLVM_PROFDATA / LLVM_COV: external tools invoked by the 'coverage'
# target to merge raw profile data and render coverage reports.
#
# Default to the versioned devel/llvm19 port binaries (llvm-cov19,
# llvm-profdata19 -- pkg install llvm19), NOT the base system's
# /usr/bin/llvm-cov and /usr/bin/llvm-profdata.
#
# Justified: on FreeBSD 15.0, /usr/bin/llvm-cov is a combined
# "multicall driver" binary hardlinked to /usr/bin/gcov (same inode --
# confirmed via `find /usr/bin -samefile /usr/bin/llvm-cov`). This
# combined binary has a real toolchain bug: `llvm-cov show` (any
# arguments, any format) aborts with "CommandLine Error: Option 'o'
# registered more than once!" inside llvm::cl::alias::done(), and
# `llvm-cov report` misreports "No filenames specified!" even with a
# valid object file positional argument. Both symptoms are absent from
# the standalone, non-multicall llvm-cov built by devel/llvm19
# (confirmed link count 1, no hardlinks) -- `pkg install llvm19`
# provides /usr/local/bin/llvm-cov19 and /usr/local/bin/llvm-profdata19
# as thin wrappers around it.
# Override if a fixed base-system build ever becomes available:
#   make LLVM_COV=llvm-cov LLVM_PROFDATA=llvm-profdata coverage
#
LLVM_PROFDATA?=	llvm-profdata19
LLVM_COV?=		llvm-cov19

# ── Test binary name ──────────────────────────────────────────────────
TARGET=		idpf_tests

# ── Test source files (alphabetical order) ────────────────────────────
#
# Add new test files here, maintaining alphabetical order.
# One test file per driver source file (test_idpf_<mod>.cpp), plus
# support files compiled and linked the same way (mocks.cpp;
# fake_idpf_vc_transport.cpp -- Feature 015 T038/T039, the
# idpf_vc_transport_ops test double consumed by test_idpf_vc_common.cpp,
# never linked into production idpf.ko).
#
TEST_SRCS=	all_tests_main.cpp \
		fake_idpf_vc_transport.cpp \
		fake_idpf_ctlq_xn.cpp \
		mocks.cpp \
		test_idpf_lib.cpp \
		test_idpf_osdep.cpp \
		test_idpf_txrx.cpp \
		test_idpf_vc.cpp \
		test_idpf_vc_common.cpp \
		test_if_idpf.cpp

TEST_OBJS=	${TEST_SRCS:.cpp=.o}

# ── kobjs: kernel object stubs ────────────────────────────────────────
#
# kobjs/ contains C++ wrappers for FreeBSD kernel object (kobj) stubs
# required by the driver's newbus method table. These are compiled
# separately because they require additional -include flags for
# CppUTestExt/MockSupport.h and FreeBSD system headers.
#
KOBJ_OBJS=	kobjs/standard.o kobjs/iflib.o

# ── Suffix rule ───────────────────────────────────────────────────────
.SUFFIXES: .cpp .o

.cpp.o:
	${CXX} ${CXXFLAGS} -c ${.IMPSRC} -o ${.TARGET}

# ── Default target: help ──────────────────────────────────────────────
#
# 'help' is the default target (listed first). Invoking 'make' with no
# arguments prints the help menu rather than silently starting a build
# or test run. This prevents accidental full test execution on machines
# that are not FreeBSD VMs or that lack the required dependencies.
#
# BSD make(1) note: @echo is the correct portable way to print output
# from a Makefile recipe. GNU make's $(info ...) and $(warning ...)
# functions are not available in BSD make(1) and must not be used.
# The leading @ suppresses echoing of the echo command itself, so only
# the message text is printed.
#
# Column layout:
#   Target name    left-aligned, padded to 12 characters with spaces.
#   Description    follows after the padding.
# Justified: consistent column width makes the menu scannable and
# matches the style used by FreeBSD base system Makefiles (e.g.,
# /usr/src/Makefile help target).
#
help:
	@echo ""
	@echo "IDPF VF CppUTest Harness — idpf/tdd_tests/Makefile"
	@echo "===================================================="
	@echo ""
	@echo "Toolchain:  clang++ (FreeBSD 15.0 base)  |  BSD make(1)"
	@echo "Framework:  CppUTest  (pkg install cpputest)"
	@echo "Coverage:   llvm-cov19 / llvm-profdata19  (pkg install llvm19)"
	@echo "            NOT the base system llvm-cov/llvm-profdata -- see"
	@echo "            LLVM_COV/LLVM_PROFDATA comments in this Makefile."
	@echo ""
	@echo "Targets:"
	@echo ""
	@echo "  help         Print this help menu (default)."
	@echo ""
	@echo "  all          Build the test binary without running it."
	@echo "               Used by CI compile-only smoke jobs."
	@echo "               Output: ${TARGET}"
	@echo ""
	@echo "  run          Build and run all tests with verbose output."
	@echo "               Requires: FreeBSD >= 15.0 VM, pkg cpputest."
	@echo "               Full test execution — not Linux-safe."
	@echo ""
	@echo "  junit        Build and run tests with JUnit XML output."
	@echo "               Output: test_results.xml"
	@echo "               Used by: CI/idpf.groovy tdd-full-execution stage."
	@echo ""
	@echo "  coverage     Build with LLVM coverage instrumentation,"
	@echo "               run tests, merge profile data, and generate"
	@echo "               a coverage report."
	@echo "               Output: coverage_report.txt"
	@echo "                       coverage_html/index.html"
	@echo "               Tools:  llvm-profdata, llvm-cov (base system)."
	@echo "               Used by: CI/idpf.groovy coverage stage."
	@echo ""
	@echo "  idpf-osdep-abi-check"
	@echo "               Phase-0 OS-shim ABI/compile-only isolation gate."
	@echo "               Compiles idpf_osdep_abi_check.cpp (+ idpf_osdep.c)"
	@echo "               with -DIDPF_NO_IFLIB, proving idpf_drv.h's"
	@echo "               #ifndef IDPF_NO_IFLIB guard (T015) excludes"
	@echo "               <net/iflib.h> and every PCI/iflib-specific field."
	@echo "               Wires and references all 14 S1.2 shim symbols"
	@echo "               (T025); links against mocks.o + kobjs so the"
	@echo "               shim's own bus_dma(9)/malloc(9)/DELAY(9) calls"
	@echo "               resolve with zero undefined symbols."
	@echo "               See specs/015-idpf-vc-transport-foundation/"
	@echo "               contracts/os-shim-contract.md."
	@echo ""
	@echo "  clean        Remove all build artefacts:"
	@echo "               *.o, ${TARGET}, test_results.xml,"
	@echo "               *.profraw, *.profdata, coverage_report.txt,"
	@echo "               coverage_html/, kobjs/*.o, idpf_osdep_abi_check"
	@echo ""
	@echo "Variables (override on command line):"
	@echo ""
	@echo "  CPPUTEST_PREFIX  CppUTest install prefix."
	@echo "                   Default: /usr/local"
	@echo "                   Example: make CPPUTEST_PREFIX=/opt/cpputest"
	@echo ""
	@echo "  CXX              C++ compiler."
	@echo "                   Default: clang++"
	@echo "                   Must not be set to g++ (not in FreeBSD 15.0 base)."
	@echo ""
	@echo "Innersource gating:"
	@echo ""
	@echo "  To strip innersource-only code sections for external release:"
	@echo "    make CXXFLAGS+=-DEXTERNAL_RELEASE all"
	@echo ""
	@echo "Execution boundary:"
	@echo ""
	@echo "  'all' and 'junit' are safe to run on Linux CI runners"
	@echo "  for compile verification only."
	@echo "  'run', 'junit' (full execution), and 'coverage' require"
	@echo "  a FreeBSD >= 15.0 VM. See CI/README.md §3."
	@echo ""
	@echo "Examples:"
	@echo ""
	@echo "  make                        Print this help menu."
	@echo "  make all                    Build test binary only."
	@echo "  make run                    Build and run (verbose)."
	@echo "  make junit                  Build and run (JUnit XML)."
	@echo "  make coverage               Build, run, and report coverage."
	@echo "  make clean                  Remove all build artefacts."
	@echo "  make CPPUTEST_PREFIX=/opt/cpputest run"
	@echo "                              Run with custom CppUTest path."
	@echo ""

# ── Build target: compile only, do not run ────────────────────────────
#
# 'all' builds the test binary without executing it.
# This is the target used by idpf/CI/scripts/freebsd-idpf-tdd-smoke.sh for
# compile-only verification and by Jenkins before 'run'.
#
all: ${TARGET}

# ── kobjs sub-directory build ─────────────────────────────────────────
#
# The kobjs/ sub-directory has its own Makefile, which only generates
# the KOBJ interface headers/sources and the standard.cpp/iflib.cpp
# wrapper files (device_if.h/bus_if.h/pci_if.h/ifdi_if.h, via
# common/kobj.mk's boilerplate) -- it does NOT compile them to .o.
# kobjs/generate is therefore a prerequisite of the actual compile
# rules below, not a substitute for them.
#
# BSD make(1) note: ${MAKE} correctly propagates the current make
# invocation (including -j flags) to the sub-make. The .MAKE special
# source is not needed here because kobjs is a real directory target,
# not a phony target that needs forced re-evaluation.
#
# IMPORTANT: ${TEST_OBJS} (mocks.o in particular, via
# common/mock_src/mock_iflib.cpp -> common/mock_inc/sys/bus.h, which
# #includes "device_if.h") also need the generated kobjs/ headers, even
# though they are compiled by the generic .cpp.o suffix rule rather
# than by the explicit kobjs/*.o rules below. Without this
# prerequisite, a from-clean build fails with "device_if.h file not
# found" whenever BSD make happens to build a TEST_OBJS member before
# KOBJ_OBJS (the two object lists have no relative ordering guarantee
# in ${TARGET}: ${TEST_OBJS} ${KOBJ_OBJS}).
${TEST_OBJS}: kobjs/generate

kobjs/generate: .PHONY
	${MAKE} -C kobjs

# kobjs/standard.cpp wraps bus_if.c/device_if.c/pci_if.c. Its
# PCI_ACCESSOR macro (via dev/pci/pcivar.h) expands to mock().actualCall(),
# so CppUTestExt/MockSupport.h must be force-included even though
# standard.cpp never includes it directly itself.
kobjs/standard.o: kobjs/generate
	${CXX} ${CXXFLAGS} -include CppUTestExt/MockSupport.h \
	    -c kobjs/standard.cpp -o kobjs/standard.o

# kobjs/iflib.cpp wraps ifdi_if.c and needs the same MockSupport.h
# force-include as standard.cpp, plus sys/param.h + sys/types.h +
# machine/_bus.h: common/mock_inc never typedefs bus_addr_t/bus_size_t,
# relying instead on the real FreeBSD system's own machine/_bus.h to
# provide them (confirmed: typedef uint64_t bus_addr_t; bus_size_t;).
kobjs/iflib.o: kobjs/generate
	${CXX} ${CXXFLAGS} -include CppUTestExt/MockSupport.h \
	    -include sys/param.h -include sys/types.h -include machine/_bus.h \
	    -c kobjs/iflib.cpp -o kobjs/iflib.o

# ── Test binary link ──────────────────────────────────────────────────
${TARGET}: ${TEST_OBJS} ${KOBJ_OBJS}
	${CXX} ${CXXFLAGS} ${LDFLAGS} -o ${TARGET} \
	    ${TEST_OBJS} ${KOBJ_OBJS} ${LIBS}

# ── Run target: build and execute with verbose output ─────────────────
run: ${TARGET}
	./${TARGET} -v

# ── JUnit XML target: build and execute with JUnit output for CI ──────
#
# Produces test_results.xml consumed by Jenkins junit() step in
# CI/idpf.groovy stage 'tdd-full-execution'.
#
junit: ${TARGET}
	./${TARGET} -v -ojunit > test_results.xml
	@echo "==> JUnit output written to test_results.xml"
	cat *.xml > test_results.log
	cat test_results.log

# ── Coverage target: LLVM instrumented build, run, and report ─────────
#
# Builds with LLVM source-based coverage instrumentation, runs the
# test binary, merges profile data, and generates a coverage report.
#
# Tools required (FreeBSD 15.0 base or pkg llvm):
#   llvm-profdata  - merges raw profile data files
#   llvm-cov       - generates coverage reports from merged data
#
# Coverage thresholds are enforced by run_idpf_tests.sh when called
# from CI. This target produces the data; threshold enforcement is
# in the shell script to keep the Makefile simple.
#
# Output files:
#   default.profraw     - raw LLVM profile data (generated by test run)
#   idpf.profdata       - merged profile data (input to llvm-cov)
#   coverage_report.txt - text coverage summary
#   coverage_html/      - HTML coverage report directory
#
coverage: .PHONY
	${MAKE} clean
	${MAKE} CXXFLAGS="${CXXFLAGS} ${COVERAGE_FLAGS}" \
	        LDFLAGS="${LDFLAGS} ${COVERAGE_FLAGS}" \
	        ${TARGET}
	LLVM_PROFILE_FILE="default.profraw" ./${TARGET} -v
	${LLVM_PROFDATA} merge -sparse default.profraw -o idpf.profdata
	${LLVM_COV} report ./${TARGET} \
	    -instr-profile=idpf.profdata \
	    --ignore-filename-regex='tdd_tests|FREEBSD_KERNEL_MOCKS|CORE_MOCKS|mock_inc|mock_src|CppUTest|bus_if|device_if|pci_if|ifdi_if' \
	    | tee coverage_report.txt
	${LLVM_COV} show ./${TARGET} \
	    -instr-profile=idpf.profdata \
	    -format=html \
	    -output-dir=coverage_html \
	    --ignore-filename-regex='tdd_tests|FREEBSD_KERNEL_MOCKS|CORE_MOCKS|mock_inc|mock_src|CppUTest|bus_if|device_if|pci_if|ifdi_if'
	${LLVM_COV} export ./${TARGET} \
	    -instr-profile=idpf.profdata \
		-format=lcov \
		--ignore-filename-regex='tdd_tests|FREEBSD_KERNEL_MOCKS|CORE_MOCKS|mock_inc|mock_src|CppUTest|bus_if|device_if|pci_if|ifdi_if' \
		> coverage_export.lcov
	@echo "==> Coverage report: coverage_report.txt"
	@echo "==> LCOV report: coverage_export.lcov"
	@echo "==> HTML report: coverage_html/index.html"

# ── Phase-0 OS-shim ABI/compile-only isolation gate ───────────────────
#
# idpf-osdep-abi-check (Feature 015 T002 scaffold + T025 wiring;
# spec.md FR-002/FR-080; contracts/os-shim-contract.md Acceptance gate
# item 1):
#
#   Compiles idpf_osdep_abi_check.cpp (which #includes idpf_osdep.c
#   directly, same convention as the CppUTest test files) with
#   -DIDPF_NO_IFLIB defined -- proving idpf_drv.h's own
#   #ifndef IDPF_NO_IFLIB guard (T015) genuinely excludes <net/iflib.h>
#   and every PCI/iflib-specific struct idpf_sc field, independent of
#   the always-with-iflib-present behavioral CppUTest suite.
#
#   T025 UPDATE (superseding T002's scaffold-era plan): T002 originally
#   scoped this target with only "-I../src" (no mock headers at all),
#   on the theory that "No CppUTest linkage is required for this gate
#   (a plain compile/link ABI probe)". That theory did not survive
#   contact with T015-T024's actual GREEN implementation -- verified
#   empirically on ssh-freebsd:
#     1. idpf_osdep.h's bus_dma(9)/bus_space(9)/malloc(9)/DELAY(9)
#        primitives are kernel-only. Real FreeBSD's own userspace
#        /usr/include headers reject them without -D_KERNEL.
#     2. Defining -D_KERNEL outside an actual kernel build tree fails
#        on a build-generated file (offset.inc, from FreeBSD's
#        genoffset(8)) that only exists under
#        /usr/src/sys/<ARCH>/compile/<KERNCONF> -- not a fixable gap
#        from this Makefile alone.
#     3. common/mock_inc's userspace stand-ins for these same
#        primitives (already proven correct by the main CppUTest
#        harness) are therefore the only feasible resolution path --
#        and they use CppUTest's mock() internally (e.g.
#        machine/bus.h's bus_space_read_4), so CppUTest linkage IS
#        required after all.
#   This remains a genuine ABI-isolation check: it is idpf_drv.h's own
#   #ifndef IDPF_NO_IFLIB guard that excludes iflib here, not the
#   choice of mock vs. real headers for kernel primitives. The probe
#   links against mocks.o (already built by "all"/${TARGET}, which
#   aggregates every common/mock_src/*.cpp this shim needs) and
#   ${KOBJ_OBJS}, so every one of the 14 S1.2 shim symbols referenced
#   in idpf_osdep_abi_check.cpp resolves with zero undefined symbols.
#
IDPF_NO_IFLIB_CXXFLAGS=	${CXXFLAGS}
IDPF_NO_IFLIB_CXXFLAGS+=	-DIDPF_NO_IFLIB
IDPF_NO_IFLIB_CXXFLAGS+=	-include CppUTestExt/MockSupport.h

idpf-osdep-abi-check: mocks.o ${KOBJ_OBJS}
	${CXX} ${IDPF_NO_IFLIB_CXXFLAGS} idpf_osdep_abi_check.cpp \
	    mocks.o ${KOBJ_OBJS} ${LDFLAGS} ${LIBS} \
	    -o idpf_osdep_abi_check
	./idpf_osdep_abi_check

# ── Clean target ──────────────────────────────────────────────────────
clean:
	rm -f ${TEST_OBJS} ${TARGET}
	rm -f test_results.xml
	rm -f default.profraw idpf.profdata coverage_report.txt
	rm -rf coverage_html
	rm -f idpf_osdep_abi_check.o idpf_osdep_abi_check
	${MAKE} -C kobjs clean

.PHONY: help all run junit coverage idpf-osdep-abi-check clean
