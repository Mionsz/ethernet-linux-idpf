/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Intel Corporation
 *
 * idpf/tdd_tests/idpf_osdep_abi_check.cpp
 *
 * @file idpf_osdep_abi_check.cpp
 * @brief Phase-0 ABI/compile-only isolation-compile probe for the
 *        OS-shim boundary.
 *
 * @details
 * Governs: specs/015-idpf-vc-transport-foundation/contracts/
 * os-shim-contract.md, Acceptance gate item 1 (FR-002, FR-080).
 *
 * This translation unit is compiled by the `idpf-osdep-abi-check`
 * Makefile target with `-DIDPF_NO_IFLIB` defined, so `idpf_drv.h`'s
 * `#ifndef IDPF_NO_IFLIB` guard (task T015) excludes `<net/iflib.h>`
 * and every other iflib/PCI-specific include and struct idpf_sc
 * field -- proving the OS-shim boundary itself carries no accidental
 * iflib dependency, independent of the behavioral CppUTest suite in
 * test_idpf_osdep.cpp (which always builds WITH iflib present).
 *
 * T025 (Feature 015, Phase 3): wires every non-gate-contingent S1.2
 * shim symbol below, referencing each one so that a symbol removed
 * or renamed by a future change would fail this probe to compile or
 * link, not just the behavioral suite.
 *
 * RED-verification finding, recorded per Constitution Principle II
 * (not a silent scope change): T002's scaffold used only `-I../src`
 * (no mock headers at all), on the theory that this probe needed no
 * CppUTest linkage ("a plain compile/link ABI probe, not a functional
 * test"). Empirically, that theory does not hold: idpf_osdep.h's
 * FreeBSD bus_dma(9)/bus_space(9)/malloc(9)/DELAY(9) primitives are
 * kernel-only constructs -- real FreeBSD's own userspace
 * /usr/include headers reject them without `_KERNEL`, and defining
 * `_KERNEL` outside an actual kernel build tree fails on a
 * build-generated file (`offset.inc`, from FreeBSD's genoffset(8))
 * that does not exist outside `/usr/src/sys/<ARCH>/compile/<KERNCONF>`
 * -- verified empirically on ssh-freebsd. This probe therefore reuses
 * the same common/mock_inc + CppUTest MockSupport machinery as the
 * main CppUTest harness (verified: this remains a genuine isolation
 * check because idpf_drv.h's own `#ifndef IDPF_NO_IFLIB` guard, not
 * the mock layer, is what actually excludes iflib here).
 */

#include "idpf_osdep.h"
#include "../src/idpf_osdep.c"

int main(void)
{
	/* Entity 1 (FR-010): struct idpf_hw, first member of idpf_sc. */
	struct idpf_sc sc;
	struct idpf_hw *hw = &sc.hw;

	hw->hw_bar0_tag = (bus_space_tag_t)0;
	hw->hw_bar0_handle = (bus_space_handle_t)0;
	IDPF_STATIC_ASSERT(
		offsetof(struct idpf_sc, hw) == 0,
		"struct idpf_hw must be the first member of struct idpf_sc");

	/* Entity 2 (FR-011): struct idpf_dma_mem. */
	struct idpf_dma_mem mem;
	mem.va = (void *)0;
	mem.pa = (bus_addr_t)0;
	mem.size = (bus_size_t)0;
	mem.tag = (bus_dma_tag_t)0;
	mem.map = (bus_dmamap_t)0;

	/* Entity 3 (FR-012): IDPF_IOVEC. */
	IDPF_IOVEC iov;
	iov.iov_base = (void *)0;
	iov.iov_len = (size_t)0;

	/* Entity 4 (FR-013, Proposed): IDPF_CMD_COMPLETION. */
	IDPF_CMD_COMPLETION cc;
	(void)&cc.mtx;
	(void)&cc.cv;

	/* FR-014: idpf_alloc_dma_mem/idpf_free_dma_mem (declared+linked;
	 * not invoked here -- real bus_dma(9) is unavailable outside a
	 * kernel build/mocked context, and this probe's only purpose is
	 * to prove zero undefined symbols for the shim's own ABI). */
	(void)idpf_alloc_dma_mem;
	(void)idpf_free_dma_mem;

	/* FR-015: idpf_calloc/idpf_malloc/idpf_free (referenced, not
	 * invoked -- see above). */
	(void)idpf_calloc;
	(void)idpf_malloc;
	(void)idpf_free;

	/* FR-016: idpf_memcpy/idpf_memset. The trailing type arguments are
	 * shared-code enums (shared/idpf_alloc.h), out of the shim's own
	 * closed contract, so a bare 0 stands in for them here. */
	char src_buf[4] = { 0 };
	char dst_buf[4];
	idpf_memcpy(dst_buf, src_buf, sizeof(src_buf), 0);
	idpf_memset(dst_buf, 0, sizeof(dst_buf), 0);

	idpf_wmb();

	/* FR-017/FR-018: idpf_rd32/idpf_wr32/idpf_flush_wr (referenced,
	 * not invoked -- see above). */
	(void)idpf_rd32;
	(void)idpf_wr32;
	(void)idpf_flush_wr;

	/* FR-019: byte-order macros. */
	uint16_t s = IDPF_HTONS(IDPF_NTOHS((uint16_t)0x1234));
	uint32_t l = IDPF_HTONL(IDPF_NTOHL((uint32_t)0x12345678));
	(void)s;
	(void)l;

	/* FR-020: idpf_usec_delay/idpf_msec_delay (referenced, not
	 * invoked -- see above). */
	(void)idpf_usec_delay;
	(void)idpf_msec_delay;

	/* FR-021/FR-022: IDPF_STATIC_ASSERT (already exercised above via
	 * the layout assertion) and IDPF_DFLT_MBX_BUF_SIZE. */
	IDPF_STATIC_ASSERT(IDPF_DFLT_MBX_BUF_SIZE == 4096,
			   "IDPF_DFLT_MBX_BUF_SIZE must be the named constant");

	return (0);
}
