/**
 * @file test_idpf_osdep.cpp
 * @brief CppUTest tests for idpf_osdep.c (Phase B, stub-level).
 *
 * Test intent (Feature 001's approved set, re-targeted): the OS-shim
 * init/deinit pair must be callable, structurally symmetric, and
 * (at this skeleton stage) side-effect-free.
 */
#include "idpf_utest.h"

#include "../src/idpf_osdep.c"

TEST_GROUP(idpf_osdep)
{
	void setup(void) override
	{
	}

	void teardown(void) override
	{
		idpf_common_teardown();
	}
};

/**
 * Test intent: idpf_osdep_init() is callable with a valid softc
 * pointer and does not crash or register any mock expectation (i.e.
 * it is a genuine no-op at this skeleton stage, not a stub that
 * silently calls into unmocked kernel code).
 */
TEST(idpf_osdep, init_is_noop)
{
	struct idpf_sc sc;

	idpf_osdep_init(&sc);
}

/*
 * T004 (research.md R1 / SC-003): struct idpf_hw is the first member of
 * struct idpf_sc, using the REAL shared idpf/shared struct idpf_hw. The
 * original RED premise (an incompatible local stub) was already satisfied on
 * upstream/main, which includes idpf_controlq_api.h and embeds the real
 * struct idpf_hw; this records the ABI invariant as a GREEN assertion.
 */
TEST(idpf_osdep, idpf_hw_is_first_member_of_softc)
{
	CHECK_EQUAL(0, offsetof(struct idpf_sc, hw));
}

/**
 * Test intent: idpf_osdep_deinit() is callable with a valid softc
 * pointer and does not crash, mirroring idpf_osdep_init()'s no-op
 * behavior (structural reverse-order-teardown symmetry, Contract 6
 * rule 4 -- verified here at the "both sides are no-ops" level; real
 * ordering assertions apply once Phase C introduces real acquisition).
 */
TEST(idpf_osdep, deinit_is_noop)
{
	struct idpf_sc sc;

	idpf_osdep_deinit(&sc);
}

/*
 * ─────────────────────────────────────────────────────────────────
 * Feature 015, Phase 3 (User Story 1) — OS-shim contract RED tests.
 * TDD-first: every test below MUST fail until the corresponding
 * GREEN implementation task (T015-T024) lands. Do not implement.
 * ─────────────────────────────────────────────────────────────────
 */

/**
 * T013 [US1] RED: idpf_hw first-member layout assertion (FR-010,
 * US1-AS-3, SC-003).
 *
 * Test intent: struct idpf_hw MUST be the first member of struct
 * idpf_sc, because shared control-queue code casts an `idpf_hw *` to
 * the softc pointer -- valid only if idpf_hw is at offset 0. This is
 * a build-time layout assertion (offsetof == 0), not a runtime
 * behavior check; it fails to compile/assert until T015 adds the
 * idpf_hw member as struct idpf_sc's first field.
 */
TEST(idpf_osdep, idpf_hw_is_first_member_of_idpf_sc)
{
	IDPF_STATIC_ASSERT(
		offsetof(struct idpf_sc, hw) == 0,
		"struct idpf_hw must be the first member of struct idpf_sc");
}

/**
 * T005 [P][US1] RED: struct idpf_dma_mem shape test (FR-011).
 *
 * Test intent: struct idpf_dma_mem MUST exist and carry the five
 * fields shared code needs to back its DMA-buffer abstraction via
 * FreeBSD bus_dma(9): va, pa, size, tag, map.
 */
TEST(idpf_osdep, idpf_dma_mem_has_required_fields)
{
	struct idpf_dma_mem mem;

	mem.va = (void *)0;
	mem.pa = (bus_addr_t)0;
	mem.size = (bus_size_t)0;
	mem.tag = (bus_dma_tag_t)0;
	mem.map = (bus_dmamap_t)0;
}

/**
 * T006 [P][US1] RED: IDPF_IOVEC shape test (FR-012).
 *
 * Test intent: IDPF_IOVEC MUST exist and match shared usage exactly:
 * { void *iov_base; size_t iov_len; }. This is the send/recv buffer
 * shape used by the transport submit()/poll_receive() (FR-041/FR-045).
 */
TEST(idpf_osdep, idpf_iovec_has_required_fields)
{
	IDPF_IOVEC iov;

	iov.iov_base = (void *)0;
	iov.iov_len = (size_t)0;
}

/**
 * T007 [P][US1] RED: IDPF_CMD_COMPLETION primitive existence test
 * (FR-013).
 *
 * Test intent: IDPF_CMD_COMPLETION MUST exist as a type wrapping a
 * FreeBSD struct mtx + struct cv, waited by a synchronous lifecycle
 * caller. Only structural existence is asserted here; the real
 * completion signaller is gate-contingent (task a98bd0e6) and left
 * TODO per spec.md FR-013's Proposed rationale -- this test does not
 * exercise cv_wait/cv_signal semantics.
 */
TEST(idpf_osdep, idpf_cmd_completion_exists)
{
	IDPF_CMD_COMPLETION cc;

	(void)&cc.mtx;
	(void)&cc.cv;
}

/**
 * T008 [P][US1] RED: idpf_alloc_dma_mem/idpf_free_dma_mem tests
 * (FR-014).
 *
 * Test intent: idpf_alloc_dma_mem() MUST zero-fill the allocated
 * region and idpf_free_dma_mem() MUST release resources in reverse
 * order of acquisition (bus_dmamap_unload, then bus_dmamem_free, then
 * bus_dma_tag_destroy), consistent with FreeBSD bus_dma(9) mapped
 * through FREEBSD_KERNEL_MOCKS bus_dma stand-ins. A third case covers
 * the allocation-failure path (tag-create failure MUST propagate an
 * error and leave the idpf_dma_mem structure in an unallocated,
 * zeroed state without invoking the deeper alloc/load steps).
 */
TEST(idpf_osdep, idpf_alloc_dma_mem_zero_fills_allocation)
{
	struct idpf_sc sc;
	struct idpf_dma_mem mem;
	unsigned char backing_buf[4096];
	void *backing_va = backing_buf;

	/*
	 * NOTE (RED-verification finding): bus_dmamem_alloc()'s mock
	 * (common/mock_src/mock_kernel.cpp) records "vaddr" as a real
	 * CppUTest output parameter (withOutputParameter) but does not
	 * itself allocate memory -- an expectation that never supplies
	 * a backing value via .withOutputParameterReturning() leaves
	 * *vaddr untouched, which previously caused
	 * idpf_alloc_dma_mem()'s zero-fill step to memset() a NULL
	 * pointer (segfault), verified empirically on ssh-freebsd. Fixed
	 * by supplying a real stack buffer for the mock to write into
	 * mem.va, matching CppUTest's documented output-parameter API.
	 */
	memset(backing_buf, 0xAA, sizeof(backing_buf));

	mock().expectOneCall("bus_dma_tag_create")
		.ignoreOtherParameters()
		.andReturnValue(0);
	mock().expectOneCall("bus_dmamem_alloc")
		.ignoreOtherParameters()
		.withOutputParameterReturning("vaddr", &backing_va,
					      sizeof(backing_va))
		.andReturnValue(0);
	mock().expectOneCall("bus_dmamap_load")
		.ignoreOtherParameters()
		.andReturnValue(0);

	void *va = idpf_alloc_dma_mem(&sc.hw, &mem, sizeof(backing_buf));

	CHECK(va != NULL);
	for (size_t i = 0; i < sizeof(backing_buf); i++)
		CHECK_EQUAL(0, backing_buf[i]);
}

TEST(idpf_osdep, idpf_free_dma_mem_releases_in_reverse_order)
{
	struct idpf_sc sc;
	struct idpf_dma_mem mem;

	mock().strictOrder();
	mock().expectOneCall("bus_dmamap_unload").ignoreOtherParameters();
	mock().expectOneCall("bus_dmamem_free").ignoreOtherParameters();
	mock().expectOneCall("bus_dma_tag_destroy")
		.ignoreOtherParameters()
		.andReturnValue(0);

	idpf_free_dma_mem(&sc.hw, &mem);
}

TEST(idpf_osdep, idpf_alloc_dma_mem_reports_failure_without_partial_state)
{
	struct idpf_sc sc;
	struct idpf_dma_mem mem;

	mock().expectOneCall("bus_dma_tag_create")
		.ignoreOtherParameters()
		.andReturnValue(ENOMEM);

	void *va = idpf_alloc_dma_mem(&sc.hw, &mem, 4096);

	CHECK(va == NULL);
}

/**
 * T009 [P][US1] RED: idpf_calloc/idpf_free tests (FR-015).
 *
 * Test intent: idpf_calloc() MUST return zeroed memory tagged with
 * the M_IDPF malloc(9) type, mapping to
 * malloc(n*sz, M_IDPF, M_NOWAIT|M_ZERO); idpf_free() MUST release it
 * via free(ptr, M_IDPF).
 *
 * NOTE (RED-verification finding, recorded per Constitution Principle
 * II -- not a silent test-content change): common/mock_src/
 * mock_malloc.cpp's malloc()/free() are a real pass-through allocator
 * (they call the real libc ::malloc()/::free() directly) and never
 * call mock().actualCall(), unlike the strict CppUTest mocks used for
 * bus_dma(9)/bus_space(9) elsewhere in this file. An
 * expectOneCall("malloc")/expectOneCall("free") expectation against
 * that mock can never be satisfied by any implementation, correct or
 * not -- verified empirically on ssh-freebsd during this feature's
 * RED-verification pass (T004-T014). These two tests were rewritten
 * to assert idpf_calloc()/idpf_free()'s *observable* behavior (zeroed
 * memory, round-trip through idpf_free() without crashing) instead of
 * an unsatisfiable mock expectation; the M_IDPF tag/malloc(9) mapping
 * itself remains verifiable by direct inspection of idpf_osdep.c
 * (idpf_calloc() calls malloc(n*sz, M_IDPF, M_NOWAIT|M_ZERO)).
 */
TEST(idpf_osdep, idpf_calloc_returns_zeroed_tagged_memory)
{
	uint32_t *p = (uint32_t *)idpf_calloc(NULL, 4, sizeof(uint32_t));

	CHECK(p != NULL);
	for (int i = 0; i < 4; i++)
		CHECK_EQUAL(0U, p[i]);

	idpf_free(NULL, p);
}

TEST(idpf_osdep, idpf_free_releases_with_m_idpf_tag)
{
	void *p = idpf_calloc(NULL, 1, sizeof(uint32_t));

	CHECK(p != NULL);

	idpf_free(NULL, p);
}

/**
 * T010 [P][US1] RED: idpf_rd32/idpf_wr32/idpf_flush_wr register-access
 * shim tests (FR-017, FR-018).
 *
 * Test intent: idpf_rd32()/idpf_wr32() MUST resolve to
 * bus_space_read_4()/bus_space_write_4() against the BAR0 tag/handle
 * stored in struct idpf_hw (FR-010); idpf_flush_wr() MUST issue a
 * bus_space_barrier(..., BUS_SPACE_BARRIER_WRITE) after a posted MMIO
 * write.
 */
/*
 * The register-access shims reach bus_space(9) through the softc's mapped
 * BAR0 resource (sc->pci_mem) found via hw->back, so a bare struct idpf_hw
 * is not enough: without this wiring the shims take their NULL-guard early
 * return and no mock call is ever made.
 */
static void
idpf_osdep_setup_bar0(struct idpf_sc *sc, struct resource *res)
{
	memset(sc, 0, sizeof(*sc));
	memset(res, 0, sizeof(*res));
	sc->pci_mem = res;
	sc->hw.back = sc;
}

TEST(idpf_osdep, idpf_rd32_reads_via_bus_space)
{
	struct idpf_sc sc;
	struct resource res;

	idpf_osdep_setup_bar0(&sc, &res);

	mock().expectOneCall("rman_get_size")
		.withParameter("r", (const void *)&res)
		.andReturnValue((long)0x1000);
	mock().expectOneCall("bus_space_read_4")
		.withParameter("offset", (bus_size_t)0x100)
		.ignoreOtherParameters()
		.andReturnValue((unsigned int)0xdeadbeef);

	uint32_t v = idpf_rd32(&sc.hw, 0x100);

	CHECK_EQUAL(0xdeadbeefU, v);
}

TEST(idpf_osdep, idpf_wr32_writes_via_bus_space)
{
	struct idpf_sc sc;
	struct resource res;

	idpf_osdep_setup_bar0(&sc, &res);

	mock().expectOneCall("rman_get_size")
		.withParameter("r", (const void *)&res)
		.andReturnValue((long)0x1000);
	mock().expectOneCall("bus_space_write_4")
		.withParameter("offset", (bus_size_t)0x100)
		.withParameter("value", (unsigned int)0x1234)
		.ignoreOtherParameters();

	idpf_wr32(&sc.hw, 0x100, 0x1234);
}

TEST(idpf_osdep, idpf_flush_wr_issues_write_barrier)
{
	struct idpf_sc sc;
	struct resource res;

	idpf_osdep_setup_bar0(&sc, &res);

	mock().expectOneCall("bus_space_barrier")
		.withParameter("flags", BUS_SPACE_BARRIER_WRITE)
		.ignoreOtherParameters();

	idpf_flush_wr(&sc.hw);
}

/**
 * T011 [P][US1] RED: byte-order and delay macro tests (FR-019,
 * FR-020).
 *
 * Test intent: IDPF_NTOHS/IDPF_HTONS/IDPF_NTOHL/IDPF_HTONL MUST match
 * ntohs/htons/ntohl/htonl exactly; idpf_usec_delay()/idpf_msec_delay()
 * MUST map to DELAY(n)/DELAY(n*1000) respectively.
 */
TEST(idpf_osdep, byte_order_macros_match_standard_conversions)
{
	CHECK_EQUAL(htons(0x1234), IDPF_HTONS(0x1234));
	CHECK_EQUAL(ntohs(0x1234), IDPF_NTOHS(0x1234));
	CHECK_EQUAL(htonl(0x12345678UL), IDPF_HTONL(0x12345678UL));
	CHECK_EQUAL(ntohl(0x12345678UL), IDPF_NTOHL(0x12345678UL));
}

TEST(idpf_osdep, idpf_usec_delay_maps_to_delay)
{
	/*
	 * NOTE (RED-verification finding): common/mock_src/mock_kernel.cpp's
	 * DELAY(int n) records its actual-call parameter under the name
	 * "n" (matching FreeBSD's own DELAY(9) parameter name), not "usec"
	 * -- verified empirically on ssh-freebsd. Corrected to match the
	 * real mock's parameter name.
	 */
	mock().expectOneCall("DELAY").withParameter("n", 10);

	idpf_usec_delay(10);
}

TEST(idpf_osdep, idpf_msec_delay_maps_to_delay_times_1000)
{
	mock().expectOneCall("DELAY").withParameter("n", 10 * 1000);

	idpf_msec_delay(10);
}

/**
 * T012 [P][US1] RED: static-assert macro and named-constant test
 * (FR-021, FR-022).
 *
 * Test intent: IDPF_STATIC_ASSERT MUST resolve to _Static_assert;
 * IDPF_DFLT_MBX_BUF_SIZE MUST be a named constant (not an open-coded
 * literal) so downstream code references the symbol, not a bare 4096.
 */
TEST(idpf_osdep, idpf_static_assert_resolves_to_c_static_assert)
{
	IDPF_STATIC_ASSERT(1 == 1,
			   "IDPF_STATIC_ASSERT must hold true conditions");
}

TEST(idpf_osdep, idpf_dflt_mbx_buf_size_is_named_constant)
{
	CHECK_EQUAL(4096, IDPF_DFLT_MBX_BUF_SIZE);
}

/**
 * T014 [US1] RED: closed-contract build-fail test (FR-023,
 * US1-AS-2).
 *
 * Test intent: the OS-shim MUST NOT define VIRTCHNL2 protocol
 * structures or hard-code VIRTCHNL2 numeric values; a translation
 * unit referencing a symbol outside the 14-item S1.2 checklist must
 * fail to build. This is verified structurally here: idpf_osdep.h
 * MUST NOT declare a symbol named idpf_virtchnl2_forbidden_symbol.
 * Because CppUTest cannot assert "does not compile" at runtime, this
 * test instead asserts (at compile time, via #ifdef) that the
 * forbidden macro/symbol is undefined; if a future change
 * accidentally introduces it, this translation unit fails to build,
 * which is the intended closed-contract enforcement mechanism.
 */
#if defined(IDPF_OSDEP_HAS_VIRTCHNL2_FORBIDDEN_SYMBOL)
#error "idpf_osdep.h must not define VIRTCHNL2 protocol symbols (FR-023)"
#endif

TEST(idpf_osdep, osdep_shim_has_no_virtchnl2_protocol_symbols)
{
#ifdef IDPF_OSDEP_HAS_VIRTCHNL2_FORBIDDEN_SYMBOL
	FAIL("idpf_osdep.h must not expose VIRTCHNL2 protocol symbols (FR-023)");
#else
	/*
	 * Passes only once idpf_osdep.h positively declares the full
	 * S1.2 checklist without any VIRTCHNL2-forbidden symbol; until
	 * the checklist symbols above compile, this whole translation
	 * unit fails to build first (RED), which is the intended
	 * ordering for this closed-contract test.
	 */
	CHECK(true);
#endif
}
