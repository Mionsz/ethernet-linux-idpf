/**
 * @file test_if_idpf.cpp
 * @brief CppUTest tests for if_idpf.c (Phase B, stub-level).
 *
 * Test intents (Feature 001's approved set, re-targeted): the iflib
 * front-end's attach/detach sequence must acquire/release resources
 * in the correct order (Contract 6 rule 4), and a representative
 * sample of the 27 approved ifdi_* stub methods must return their
 * documented safe defaults. Uses the narrow #define static/#undef
 * static exposure pattern (Contract 2 rule 5) to reach if_idpf.c's
 * static functions.
 */
#include "idpf_utest.h"

/*
 * Pre-include the system/mock headers if_idpf.c itself includes,
 * OUTSIDE the namespace block below. Without this, these headers'
 * first (and therefore namespace-determining) processing would happen
 * INSIDE namespace test_if_idpf (via if_idpf.c's own #include lines),
 * which would incorrectly namespace externally-linked mock functions
 * like iflib_get_softc() as test_if_idpf::iflib_get_softc() -- a
 * different symbol than the one common/mock_src/mock_iflib.cpp
 * actually defines at global scope, causing link failures.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/module.h>
#include <sys/errno.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net/iflib.h>
#include "ifdi_if.h"
#include "idpf_drv.h"
#include "idpf_lib.h"
#include "idpf_osdep.h"
#include "idpf_txrx_common.h"
#include "idpf_vc_common.h"
#include "idpf_lan_vf_regs.h"
#include "idpf_mmg_v_policy.h"
#include "fake_idpf_ctlq_xn.h"

namespace test_if_idpf {
#define static
#include "../src/if_idpf.c"
#undef static
}

using namespace test_if_idpf;

/**
 * @struct fake_iflib_ctx
 * @brief Layout-compatible stand-in for common/mock_src/mock_iflib.cpp's
 * own (opaque, from this file's perspective) `struct iflib_ctx`.
 *
 * mock_iflib.cpp's iflib_get_softc()/iflib_get_dev()/iflib_get_sctx()/
 * iflib_get_softc_ctx()/iflib_get_ifp() directly dereference
 * `ctx->ifc_softc` etc. (they do not go through mock().actualCall(),
 * so mock().expectOneCall has no effect on them) -- so a real,
 * non-NULL if_ctx_t with these fields populated is required, not a
 * mocked expectation. Since the real struct iflib_ctx is defined only
 * inside mock_iflib.cpp (not in any header this file includes), this
 * local struct mirrors its leading field layout exactly (void
 * *ifc_softc, device_t ifc_dev, if_t ifc_ifp, in that order) so that a
 * pointer to it can stand in for a real if_ctx_t in these tests.
 */
struct fake_iflib_ctx {
	void *ifc_softc;
	device_t ifc_dev;
	if_t ifc_ifp;
};

TEST_GROUP(if_idpf)
{
	void teardown(void) override
	{
		idpf_common_teardown();
	}
};

/**
 * Test intent: idpf_register() returns a pointer to the module-level
 * idpf_sctx shared-context structure.
 */
TEST(if_idpf, register_returns_shared_context)
{
	void *result = idpf_register(NULL);

	POINTERS_EQUAL(&idpf_sctx, result);
}

/**
 * T016 (US1, contracts Rule 1): idpf_if_attach_pre() acquires the device
 * bring-up milestones in the exact forward order BAR0_MAPPED ->
 * CQ_INITIALIZED -> VERSION_DONE -> CAPS_DONE -> VPORT_CREATED, gated by the
 * FR-018 VFGEN_RSTAT reset-complete poll and the FR-017 VF_ATQLEN.ATQENABLE
 * FLR check, issuing VERSION/GET_CAPS/CREATE_VPORT in that mailbox order.
 */
TEST(if_idpf, attach_pre_acquires_milestones_in_order)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	struct resource fake_resource;
	struct virtchnl2_version_info ver = {};
	struct virtchnl2_get_capabilities caps = {};
	struct virtchnl2_create_vport vport = {};
	int rc;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	/* idpf_init_controlq reads PCI identity (pci_get_*): informational. */
	mock().ignoreOtherCalls();

	/* Per-opcode CP replies for the three bring-up messages. */
	ver.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR);
	ver.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR);
	fake_xn_set_recv_for(VIRTCHNL2_OP_VERSION, &ver, sizeof(ver));
	caps.max_tx_q = htole16(1);
	caps.max_rx_q = htole16(1);
	fake_xn_set_recv_for(VIRTCHNL2_OP_GET_CAPS, &caps, sizeof(caps));
	vport.vport_id = htole32(1);
	vport.chunks.num_chunks = htole16(1);
	fake_xn_set_recv_for(VIRTCHNL2_OP_CREATE_VPORT, &vport, sizeof(vport));

	/* 1. BAR0 map. */
	mock().expectOneCall("bus_alloc_resource_any")
	      .ignoreOtherParameters()
	      .andReturnValue((void *)&fake_resource);
	/* FR-018: VFGEN_RSTAT read reports VF reset Completed (01b). */
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VFGEN_RSTAT)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)IDPF_MMG_V_VFR_STATE_COMPLETED);
	/* FR-017: VF_ATQLEN read reports the ATQ enabled (no FLR in flight). */
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VF_ATQLEN)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)VF_ATQLEN_ATQENABLE_M);

	rc = idpf_if_attach_pre((if_ctx_t)&fake_ctx);

	LONGS_EQUAL(0, rc);
	POINTERS_EQUAL((if_ctx_t)&fake_ctx, sc.ctx);
	POINTERS_EQUAL(&fake_resource, sc.pci_mem);

	/* All five forward-order milestones recorded. */
	CHECK((sc.state & IDPF_STATE_BAR0_MAPPED) != 0);
	CHECK((sc.state & IDPF_STATE_CQ_INITIALIZED) != 0);
	CHECK((sc.state & IDPF_STATE_VERSION_DONE) != 0);
	CHECK((sc.state & IDPF_STATE_CAPS_DONE) != 0);
	CHECK((sc.state & IDPF_STATE_VPORT_CREATED) != 0);
	/* ControlQ/xn manager was initialized and published. */
	CHECK(fake_xn_ctlq_xn_init_count() >= 1);
	/* Mailbox messages issued in contract order, no extras. */
	LONGS_EQUAL(3, fake_xn_call_count());
	LONGS_EQUAL(VIRTCHNL2_OP_VERSION, fake_xn_opcode_at(0));
	LONGS_EQUAL(VIRTCHNL2_OP_GET_CAPS, fake_xn_opcode_at(1));
	LONGS_EQUAL(VIRTCHNL2_OP_CREATE_VPORT, fake_xn_opcode_at(2));
}

/**
 * T017 (US1, FR-002/FR-007): idpf_if_attach_post() sets IDPF_STATE_ATTACHED
 * only after all five prior milestones are set, and issues no mailbox
 * message at all (in particular never VIRTCHNL2_OP_ALLOC_VECTORS).
 */
TEST(if_idpf, attach_post_sets_attached_and_allocs_no_vectors)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	int rc;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	sc.state = IDPF_STATE_BAR0_MAPPED | IDPF_STATE_CQ_INITIALIZED |
	    IDPF_STATE_VERSION_DONE | IDPF_STATE_CAPS_DONE |
	    IDPF_STATE_VPORT_CREATED;

	rc = idpf_if_attach_post((if_ctx_t)&fake_ctx);

	LONGS_EQUAL(0, rc);
	CHECK((sc.state & IDPF_STATE_ATTACHED) != 0);
	/* No OP_ALLOC_VECTORS (indeed no mailbox message) is ever issued. */
	LONGS_EQUAL(0, fake_xn_call_count());
}

/**
 * T017 (US1, FR-002): idpf_if_attach_post() fails closed with ENXIO and does
 * NOT set IDPF_STATE_ATTACHED when a prior milestone is missing.
 */
TEST(if_idpf, attach_post_fails_closed_when_milestone_missing)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	int rc;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	/* CREATE_VPORT milestone intentionally absent. */
	sc.state = IDPF_STATE_BAR0_MAPPED | IDPF_STATE_CQ_INITIALIZED |
	    IDPF_STATE_VERSION_DONE | IDPF_STATE_CAPS_DONE;

	rc = idpf_if_attach_post((if_ctx_t)&fake_ctx);

	LONGS_EQUAL(ENXIO, rc);
	CHECK((sc.state & IDPF_STATE_ATTACHED) == 0);
}

/**
 * T031 (US2, contracts Rule 3): a major-version mismatch during
 * idpf_if_attach_pre() stops the bring-up immediately -- GET_CAPS and
 * CREATE_VPORT are never issued and their milestone flags stay clear.
 */
TEST(if_idpf, attach_pre_major_mismatch_stops_before_caps_and_vport)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	struct resource fake_resource;
	struct virtchnl2_version_info ver = {};
	int rc;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	mock().ignoreOtherCalls();

	/* VERSION reply carries a mismatched major -> fail closed. */
	ver.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR + 1);
	ver.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR);
	fake_xn_set_recv_for(VIRTCHNL2_OP_VERSION, &ver, sizeof(ver));

	mock().expectOneCall("bus_alloc_resource_any")
	      .ignoreOtherParameters()
	      .andReturnValue((void *)&fake_resource);
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VFGEN_RSTAT)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)IDPF_MMG_V_VFR_STATE_COMPLETED);
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VF_ATQLEN)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)VF_ATQLEN_ATQENABLE_M);
	/* Attach-failure unwind releases BAR0. */
	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	rc = idpf_if_attach_pre((if_ctx_t)&fake_ctx);

	LONGS_EQUAL(EPROTONOSUPPORT, rc);
	/* VERSION issued, then RESET_VF during unwind; no GET_CAPS/CREATE_VPORT. */
	LONGS_EQUAL(2, fake_xn_call_count());
	LONGS_EQUAL(VIRTCHNL2_OP_VERSION, fake_xn_opcode_at(0));
	LONGS_EQUAL(VIRTCHNL2_OP_RESET_VF, fake_xn_opcode_at(1));
	/* Partial state fully unwound. */
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
}

/**
 * T038 (US3, contracts Rule 4): a GET_CAPS failure during
 * idpf_if_attach_pre() stops the bring-up before CREATE_VPORT -- only
 * VERSION and GET_CAPS reach the transport, and VPORT_CREATED stays clear.
 */
TEST(if_idpf, attach_pre_caps_failure_stops_before_vport)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	struct resource fake_resource;
	struct virtchnl2_version_info ver = {};
	struct virtchnl2_get_capabilities caps = {};
	int rc;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	mock().ignoreOtherCalls();

	ver.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR);
	ver.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR);
	fake_xn_set_recv_for(VIRTCHNL2_OP_VERSION, &ver, sizeof(ver));
	/* GET_CAPS reports a missing required queue capability -> EINVAL. */
	caps.max_tx_q = htole16(0);
	caps.max_rx_q = htole16(1);
	fake_xn_set_recv_for(VIRTCHNL2_OP_GET_CAPS, &caps, sizeof(caps));

	mock().expectOneCall("bus_alloc_resource_any")
	      .ignoreOtherParameters()
	      .andReturnValue((void *)&fake_resource);
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VFGEN_RSTAT)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)IDPF_MMG_V_VFR_STATE_COMPLETED);
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VF_ATQLEN)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)VF_ATQLEN_ATQENABLE_M);
	/* Attach-failure unwind releases BAR0. */
	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	rc = idpf_if_attach_pre((if_ctx_t)&fake_ctx);

	LONGS_EQUAL(EINVAL, rc);
	/* VERSION, GET_CAPS, then RESET_VF during unwind; no CREATE_VPORT. */
	LONGS_EQUAL(3, fake_xn_call_count());
	LONGS_EQUAL(VIRTCHNL2_OP_VERSION, fake_xn_opcode_at(0));
	LONGS_EQUAL(VIRTCHNL2_OP_GET_CAPS, fake_xn_opcode_at(1));
	LONGS_EQUAL(VIRTCHNL2_OP_RESET_VF, fake_xn_opcode_at(2));
	/* Partial state fully unwound. */
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
}

/**
 * T045 (US4, contracts Rule 2): after a full attach, idpf_if_detach() tears
 * down in reverse order -- clear ATTACHED, DESTROY_VPORT (vport created),
 * RESET_VF (mailbox up), deinit ControlQ/idpf_xn, release BAR0 last -- and
 * clears every state flag.
 */
TEST(if_idpf, detach_full_reverse_order_teardown)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	struct resource fake_resource;
	struct idpf_ctlq_xn_manager dummy_xnm;
	int rc;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	sc.pci_mem = &fake_resource;
	sc.xnm = &dummy_xnm;
	sc.vport_id = 0x1234;
	sc.state = IDPF_STATE_BAR0_MAPPED | IDPF_STATE_CQ_INITIALIZED |
	    IDPF_STATE_VERSION_DONE | IDPF_STATE_CAPS_DONE |
	    IDPF_STATE_VPORT_CREATED | IDPF_STATE_ATTACHED;

	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	rc = idpf_if_detach((if_ctx_t)&fake_ctx);

	LONGS_EQUAL(0, rc);
	/* DESTROY_VPORT then RESET_VF issued, in that order. */
	LONGS_EQUAL(2, fake_xn_call_count());
	LONGS_EQUAL(VIRTCHNL2_OP_DESTROY_VPORT, fake_xn_opcode_at(0));
	LONGS_EQUAL(VIRTCHNL2_OP_RESET_VF, fake_xn_opcode_at(1));
	/* All state flags cleared; BAR0 released. */
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
	POINTERS_EQUAL(NULL, sc.xnm);
}

/**
 * T046 (US4, FR-003 scenario 2): when DESTROY_VPORT/RESET_VF report a
 * warning/failure status, idpf_if_detach() logs and still completes local
 * ControlQ deinit and BAR0 release rather than aborting.
 */
TEST(if_idpf, detach_completes_despite_teardown_message_failure)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	struct resource fake_resource;
	struct idpf_ctlq_xn_manager dummy_xnm;
	int rc;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	fake_xn_set_rc(EIO);	/* both DESTROY_VPORT and RESET_VF fail */
	sc.pci_mem = &fake_resource;
	sc.xnm = &dummy_xnm;
	sc.state = IDPF_STATE_BAR0_MAPPED | IDPF_STATE_CQ_INITIALIZED |
	    IDPF_STATE_VPORT_CREATED | IDPF_STATE_ATTACHED;

	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	rc = idpf_if_detach((if_ctx_t)&fake_ctx);

	LONGS_EQUAL(0, rc);
	/* Both teardown messages were attempted despite failing. */
	LONGS_EQUAL(2, fake_xn_call_count());
	/* Local teardown still ran to completion. */
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
	POINTERS_EQUAL(NULL, sc.xnm);
}

/**
 * T055 (US5, contracts Rule 2 / SC-004): the partial-attach fault-injection
 * matrix. idpf_attach_unwind() releases exactly the resources acquired for a
 * given state, in reverse order, with no double-free.
 *
 * Case A -- failure after BAR0 only: no ControlQ, no RESET_VF, no
 * DESTROY_VPORT; BAR0 released; state cleared.
 */
TEST(if_idpf, unwind_after_bar0_only)
{
	struct idpf_sc sc = {};
	struct resource fake_resource;

	fake_xn_reset();
	sc.pci_mem = &fake_resource;
	sc.state = IDPF_STATE_BAR0_MAPPED;

	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	idpf_attach_unwind(&sc);

	LONGS_EQUAL(0, fake_xn_call_count());
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
}

/**
 * T055 Case B -- failure after CQ init (before VERSION): RESET_VF sent
 * unconditionally, ControlQ deinitialized, BAR0 released; no DESTROY_VPORT.
 */
TEST(if_idpf, unwind_after_cq_init)
{
	struct idpf_sc sc = {};
	struct resource fake_resource;
	struct idpf_ctlq_xn_manager dummy_xnm;

	fake_xn_reset();
	sc.pci_mem = &fake_resource;
	sc.xnm = &dummy_xnm;
	sc.state = IDPF_STATE_BAR0_MAPPED | IDPF_STATE_CQ_INITIALIZED;

	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	idpf_attach_unwind(&sc);

	LONGS_EQUAL(1, fake_xn_call_count());
	LONGS_EQUAL(VIRTCHNL2_OP_RESET_VF, fake_xn_opcode_at(0));
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
	POINTERS_EQUAL(NULL, sc.xnm);
}

/**
 * T055 Case C -- failure after VERSION (before CAPS): same teardown as Case B,
 * with VERSION_DONE cleared; no DESTROY_VPORT.
 */
TEST(if_idpf, unwind_after_version)
{
	struct idpf_sc sc = {};
	struct resource fake_resource;
	struct idpf_ctlq_xn_manager dummy_xnm;

	fake_xn_reset();
	sc.pci_mem = &fake_resource;
	sc.xnm = &dummy_xnm;
	sc.state = IDPF_STATE_BAR0_MAPPED | IDPF_STATE_CQ_INITIALIZED |
	    IDPF_STATE_VERSION_DONE;

	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	idpf_attach_unwind(&sc);

	LONGS_EQUAL(1, fake_xn_call_count());
	LONGS_EQUAL(VIRTCHNL2_OP_RESET_VF, fake_xn_opcode_at(0));
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
}

/**
 * T055 Case D -- failure after CAPS (before VPORT): same teardown; no
 * DESTROY_VPORT since IDPF_STATE_VPORT_CREATED was never set.
 */
TEST(if_idpf, unwind_after_caps)
{
	struct idpf_sc sc = {};
	struct resource fake_resource;
	struct idpf_ctlq_xn_manager dummy_xnm;

	fake_xn_reset();
	sc.pci_mem = &fake_resource;
	sc.xnm = &dummy_xnm;
	sc.state = IDPF_STATE_BAR0_MAPPED | IDPF_STATE_CQ_INITIALIZED |
	    IDPF_STATE_VERSION_DONE | IDPF_STATE_CAPS_DONE;

	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	idpf_attach_unwind(&sc);

	LONGS_EQUAL(1, fake_xn_call_count());
	LONGS_EQUAL(VIRTCHNL2_OP_RESET_VF, fake_xn_opcode_at(0));
	LONGS_EQUAL(0U, sc.state);
	POINTERS_EQUAL(NULL, sc.pci_mem);
}

/**
 * T062 (Polish, contracts Rule 6 / FR-007): across a full attach + detach
 * cycle, neither VIRTCHNL2_OP_ALLOC_VECTORS nor VIRTCHNL2_OP_DEALLOC_VECTORS is
 * ever submitted -- the MVP relies solely on the mailbox-vector default.
 */
TEST(if_idpf, full_cycle_never_allocates_vectors)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	struct resource fake_resource;
	struct virtchnl2_version_info ver = {};
	struct virtchnl2_get_capabilities caps = {};
	struct virtchnl2_create_vport vport = {};
	int rc, i;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	mock().ignoreOtherCalls();
	ver.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR);
	ver.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR);
	fake_xn_set_recv_for(VIRTCHNL2_OP_VERSION, &ver, sizeof(ver));
	caps.max_tx_q = htole16(1);
	caps.max_rx_q = htole16(1);
	fake_xn_set_recv_for(VIRTCHNL2_OP_GET_CAPS, &caps, sizeof(caps));
	vport.vport_id = htole32(1);
	vport.chunks.num_chunks = htole16(1);
	fake_xn_set_recv_for(VIRTCHNL2_OP_CREATE_VPORT, &vport, sizeof(vport));

	/* attach_pre: BAR0 + FR-018 + FR-017 reads. */
	mock().expectOneCall("bus_alloc_resource_any")
	      .ignoreOtherParameters().andReturnValue((void *)&fake_resource);
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VFGEN_RSTAT)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)IDPF_MMG_V_VFR_STATE_COMPLETED);
	mock().expectOneCall("rman_get_size")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue((long)0x10000);
	mock().expectOneCall("bus_space_read_4")
	      .withParameter("offset", (bus_size_t)VF_ATQLEN)
	      .ignoreOtherParameters()
	      .andReturnValue((unsigned int)VF_ATQLEN_ATQENABLE_M);
	/* detach: BAR0 release. */
	mock().expectOneCall("rman_get_rid").ignoreOtherParameters();
	mock().expectOneCall("bus_release_resource_old").ignoreOtherParameters();

	rc = idpf_if_attach_pre((if_ctx_t)&fake_ctx);
	LONGS_EQUAL(0, rc);
	rc = idpf_if_attach_post((if_ctx_t)&fake_ctx);
	LONGS_EQUAL(0, rc);
	idpf_if_detach((if_ctx_t)&fake_ctx);

	/* Exactly VERSION, GET_CAPS, CREATE_VPORT, DESTROY_VPORT, RESET_VF. */
	LONGS_EQUAL(5, fake_xn_call_count());
	for (i = 0; i < fake_xn_call_count(); i++) {
		uint16_t op = fake_xn_opcode_at(i);

		CHECK(op != VIRTCHNL2_OP_ALLOC_VECTORS);
		CHECK(op != VIRTCHNL2_OP_DEALLOC_VECTORS);
	}
}

/**
 * Test intent: idpf_if_detach() releases resources in the exact
 * reverse order of idpf_if_attach_pre()/idpf_if_attach_post() --
 * control-plane task first, then OS-shim, then PCI resources last
 * (Contract 6 rule 4: reverse-order teardown). This test verifies the
 * PCI-release call happens and the softc's pci_mem is cleared.
 */
TEST(if_idpf, detach_releases_pci_resource)
{
	struct idpf_sc sc = {};
	struct fake_iflib_ctx fake_ctx = {};
	struct resource fake_resource;

	fake_ctx.ifc_softc = &sc;
	fake_xn_reset();
	sc.pci_mem = &fake_resource;
	sc.state = IDPF_STATE_BAR0_MAPPED;

	mock().expectOneCall("rman_get_rid")
	      .ignoreOtherParameters();

	mock().expectOneCall("bus_release_resource_old")
	      .ignoreOtherParameters();

	idpf_if_detach((if_ctx_t)&fake_ctx);

	POINTERS_EQUAL(NULL, sc.pci_mem);
	/* No mailbox teardown when the ControlQ never came up. */
	LONGS_EQUAL(0, fake_xn_call_count());
}

/**
 * Test intent: idpf_if_media_status() reports link-down (IFM_AVALID |
 * IFM_ETHER, no IFM_ACTIVE bit) at this skeleton stage -- no real
 * media-state query exists yet.
 */
TEST(if_idpf, media_status_reports_link_down)
{
	struct ifmediareq ifmr = {};

	idpf_if_media_status(NULL, &ifmr);

	LONGS_EQUAL(IFM_AVALID, ifmr.ifm_status);
	LONGS_EQUAL(IFM_ETHER, ifmr.ifm_active);
}

/**
 * Phase C, third increment (spec.md S5 item #26 ifdi_get_counter;
 * execution-review-package.md S2a). Test intent (supersedes the prior
 * "always returns 0" stub-level intent): idpf_if_get_counter()
 * delegates every counter request to the generic, non-hardware-
 * specific iflib/OS-level if_get_counter_default() fallback, since
 * idpf does not yet track any real per-VSI hardware statistics --
 * directly modeled on the proven iavf/iavf/src/CORE/if_iavf_iflib.c
 * reference's own "default: return (if_get_counter_default(ifp,
 * cnt));" fallback case, generalized to every counter identifier.
 * Retired rather than kept alongside the old "always 0" test, since
 * the two assertions are mutually exclusive descriptions of the same
 * function's behavior (this is not a weakened assertion -- it is the
 * old stub-level intent being intentionally superseded, exactly as
 * Phase C's own purpose requires).
 */
TEST(if_idpf, get_counter_delegates_to_if_get_counter_default)
{
	struct fake_iflib_ctx fake_ctx = {};
	uint64_t result;

	mock().expectOneCall("if_get_counter_default")
	      .ignoreOtherParameters()
	      .andReturnValue(42L);

	result = idpf_if_get_counter((if_ctx_t)&fake_ctx, IFCOUNTER_IPACKETS);

	LONGS_EQUAL(42, result);
}

/**
 * Test intent: idpf_if_needs_restart() always returns false at this
 * skeleton stage, for any restart-event type.
 */
TEST(if_idpf, needs_restart_returns_false)
{
	bool result = idpf_if_needs_restart(NULL, IFLIB_RESTART_VLAN_CONFIG);

	CHECK_FALSE(result);
}

/**
 * Test intent: idpf_if_mtu_set() accepts any MTU value and returns
 * success (0) at this skeleton stage.
 */
TEST(if_idpf, mtu_set_accepts_any_value)
{
	int rc = idpf_if_mtu_set(NULL, 9000);

	LONGS_EQUAL(0, rc);
}

/**
 * Phase C, first increment (spec.md S5 item #19 ifdi_mtu_set;
 * execution-review-package.md S2a). Test intent: idpf_if_mtu_set()
 * rejects MTU values below ETHERMIN -- a generic, universal FreeBSD/
 * IEEE-802.3 constant from net/ethernet.h, not a hardware/vendor-
 * specific value (already-validated, no Co-Design citation required)
 * -- returning EINVAL, rather than accepting any value unconditionally
 * as the Phase A/B stub previously did.
 */
TEST(if_idpf, mtu_set_rejects_value_below_ethermin)
{
	int rc = idpf_if_mtu_set(NULL, ETHERMIN - 1);

	LONGS_EQUAL(EINVAL, rc);
}

/**
 * Phase C, second increment (spec.md S5 items #24/#25 ifdi_vlan_
 * register/ifdi_vlan_unregister; execution-review-package.md S2a).
 * Test intent: idpf_vlan_tag_is_valid() -- a new, standalone pure
 * calculation/validation helper, not yet wired into idpf_if_vlan_
 * register()/idpf_if_vlan_unregister() -- rejects VLAN tag 0 (reserved
 * for untagged/priority-tagged traffic per IEEE 802.1Q) and any value
 * above EVL_VLID_MASK (0x0FFF/4095, the 12-bit VLAN ID field width),
 * both generic, universal FreeBSD/IEEE-802.1Q constants from
 * net/ethernet.h, not hardware/vendor-specific (already-validated, no
 * Co-Design citation required) -- directly modeled on the proven
 * iavf/iavf/src/CORE/if_iavf_iflib.c reference's own
 * "if ((vtag == 0) || (vtag > 4095)) return;" boundary check.
 */
TEST(if_idpf, vlan_tag_is_valid_rejects_reserved_zero)
{
	CHECK_FALSE(idpf_vlan_tag_is_valid(0));
}

TEST(if_idpf, vlan_tag_is_valid_rejects_above_evl_vlid_mask)
{
	CHECK_FALSE(idpf_vlan_tag_is_valid(EVL_VLID_MASK + 1));
}

TEST(if_idpf, vlan_tag_is_valid_accepts_boundary_and_typical_values)
{
	CHECK_TRUE(idpf_vlan_tag_is_valid(1));
	CHECK_TRUE(idpf_vlan_tag_is_valid(100));
	CHECK_TRUE(idpf_vlan_tag_is_valid(EVL_VLID_MASK));
}

/**
 * Phase C, fourth increment (spec.md S5 item #12 ifdi_rx_queue_intr_
 * enable; execution-review-package.md S2a). Test intent: idpf_if_rx_
 * queue_intr_enable() returns ENOTSUP rather than falsely claiming
 * success (0), since idpf does not implement any real per-queue
 * interrupt enable logic yet (no queues/interrupts exist at this
 * skeleton stage) -- directly modeled on FreeBSD's own canonical
 * generic default for this exact method, defined in the real kernel's
 * sys/net/ifdi_if.m ("null_queue_intr_enable"):
 *   static int
 *   null_queue_intr_enable(if_ctx_t _ctx __unused, uint16_t _qid __unused)
 *   {
 *           return (ENOTSUP);
 *   }
 * This is a pure return-code correction (matching the framework's own
 * "not implemented" convention for this scenario), not a queue-index
 * bounds check -- no real hardware queue-count fact is involved or
 * required.
 */
TEST(if_idpf, rx_queue_intr_enable_returns_enotsup)
{
	int rc = idpf_if_rx_queue_intr_enable(NULL, 0);

	LONGS_EQUAL(ENOTSUP, rc);
}

/**
 * Phase C, fifth increment (spec.md S5 item #13 ifdi_tx_queue_intr_
 * enable; execution-review-package.md S2a). Symmetric to increment 4
 * (idpf_if_rx_queue_intr_enable): the real kernel's own
 * sys/net/ifdi_if.m declares the identical
 * "DEFAULT null_queue_intr_enable" clause for both tx_queue_intr_
 * enable and rx_queue_intr_enable (confirmed by direct inspection on
 * the FreeBSD host), so the same ENOTSUP correction applies here for
 * the same reason: idpf implements no real per-queue interrupt logic
 * yet, and ENOTSUP (not a false success 0) is the framework's own
 * proven default for this exact, unimplemented scenario.
 */
TEST(if_idpf, tx_queue_intr_enable_returns_enotsup)
{
	int rc = idpf_if_tx_queue_intr_enable(NULL, 0);

	LONGS_EQUAL(ENOTSUP, rc);
}
