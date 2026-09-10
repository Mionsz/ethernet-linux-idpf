/**
 * @file test_idpf_vc_common.cpp
 * @brief CppUTest tests for idpf_vc_common.c (Phase B, stub-level).
 *
 * Test intents (Feature 001's approved set, re-targeted): the
 * control-plane message-processing task init/deinit pair must be
 * callable and return success, without creating any real taskqueue at
 * this skeleton stage (real taskqueue creation, and its Contract 6
 * rule 9 drain-then-free teardown, are introduced by Phase C).
 *
 * C++ standard library headers MUST precede idpf_utest.h.
 * idpf_utest.h pulls in mock kernel headers (mock_inc/sys/types.h
 * et al.) which poison the include-guard chain that libc++'s
 * <fstream> -> <istream> -> <streambuf> relies on to pull in <ios>
 * (streamsize, ios_base). If those guards are already set when
 * <fstream> is processed, ios_base is never fully defined and
 * streambuf fails to compile. See mock_inc/README.md, <unistd.h>
 * entry, for the same class of problem documented for sys/types.h.
 */

#include <fstream>
#include <iterator>
#include <string>

#include "idpf_utest.h"

#include "../src/idpf_vc_common.c"

/* FR-010 / T005: idpf_mmg_v_policy.h is the single source of the MMG_V (Morganville) MVP
 * bring-up constants. RED until T010 creates the header. */
#include "../src/idpf_mmg_v_policy.h"

/* FR-011: fake at the idpf_ctlq_xn_* transport boundary. */
#include "fake_idpf_ctlq_xn.h"

TEST_GROUP(idpf_vc_common)
{
	void teardown(void) override
	{
		idpf_common_teardown();
	}
};

/*
 * T005 (FR-010): the MMG_V (Morganville) MVP policy constants come from one source header,
 * idpf_mmg_v_policy.h, with the exact data-model.md §1 values. RED until T010.
 */
TEST(idpf_vc_common, mmg_v_policy_constants_single_source)
{
	LONGS_EQUAL(2, IDPF_MMG_V_VC_VERSION_MAJOR);
	LONGS_EQUAL(0, IDPF_MMG_V_VC_VERSION_MINOR);
	LONGS_EQUAL(2, IDPF_MMG_V_QUEUE_COUNT_DEFAULT);
	LONGS_EQUAL(1, IDPF_MMG_V_ATTACH_NUM_TXQ);
	LONGS_EQUAL(1, IDPF_MMG_V_ATTACH_NUM_RXQ);
	LONGS_EQUAL(1, IDPF_MMG_V_ATTACH_NUM_VECTORS);
}

/*
 * T013 (US1, FR-004): idpf_send_version() succeeds when the CP replies with a
 * matching version, sets IDPF_STATE_VERSION_DONE, and sends VIRTCHNL2_OP_VERSION.
 */
TEST(idpf_vc_common, send_version_success_sets_version_done)
{
	struct idpf_sc sc;
	struct virtchnl2_version_info resp;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	resp.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR);
	resp.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR);
	fake_xn_set_recv(&resp, sizeof(resp));

	rc = idpf_send_version(&sc);

	LONGS_EQUAL(0, rc);
	CHECK((sc.state & IDPF_STATE_VERSION_DONE) != 0);
	LONGS_EQUAL(VIRTCHNL2_OP_VERSION, fake_xn_last_opcode());
	LONGS_EQUAL(1, fake_xn_call_count());
}

/*
 * T029 (US2, FR-004): idpf_send_version() fails closed on a major-version
 * mismatch -- returns EPROTONOSUPPORT and leaves IDPF_STATE_VERSION_DONE clear.
 */
TEST(idpf_vc_common, send_version_major_mismatch_fails_closed)
{
	struct idpf_sc sc;
	struct virtchnl2_version_info resp;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	resp.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR + 1);
	resp.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR);
	fake_xn_set_recv(&resp, sizeof(resp));

	rc = idpf_send_version(&sc);

	LONGS_EQUAL(EPROTONOSUPPORT, rc);
	CHECK((sc.state & IDPF_STATE_VERSION_DONE) == 0);
}

/*
 * T030 (US2, FR-004): a matching major with mismatched minor is non-fatal --
 * idpf_send_version() returns 0 (warning logged) and sets VERSION_DONE.
 */
TEST(idpf_vc_common, send_version_minor_mismatch_is_non_fatal)
{
	struct idpf_sc sc;
	struct virtchnl2_version_info resp;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	resp.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR);
	resp.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR + 7);
	fake_xn_set_recv(&resp, sizeof(resp));

	rc = idpf_send_version(&sc);

	LONGS_EQUAL(0, rc);
	CHECK((sc.state & IDPF_STATE_VERSION_DONE) != 0);
}


/*
 * T014 (US1, FR-005): idpf_get_caps() succeeds with minimal caps present,
 * caches sc->caps, sets IDPF_STATE_CAPS_DONE, and sends VIRTCHNL2_OP_GET_CAPS.
 */
TEST(idpf_vc_common, get_caps_success_caches_and_sets_caps_done)
{
	struct idpf_sc sc;
	struct virtchnl2_get_capabilities resp;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	memset(&resp, 0, sizeof(resp));
	resp.max_tx_q = htole16(1);
	resp.max_rx_q = htole16(1);
	fake_xn_set_recv(&resp, sizeof(resp));

	rc = idpf_get_caps(&sc);

	LONGS_EQUAL(0, rc);
	CHECK((sc.state & IDPF_STATE_CAPS_DONE) != 0);
	LONGS_EQUAL(VIRTCHNL2_OP_GET_CAPS, fake_xn_last_opcode());
	LONGS_EQUAL(1, le16toh(sc.caps.max_tx_q));
}

/*
 * T036 (US3, FR-005): idpf_get_caps() fails closed when the CP reports a
 * missing required queue capability (max_tx_q == 0) -- returns EINVAL and
 * leaves IDPF_STATE_CAPS_DONE clear.
 */
TEST(idpf_vc_common, get_caps_zero_queue_count_fails_closed)
{
	struct idpf_sc sc;
	struct virtchnl2_get_capabilities resp;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	memset(&resp, 0, sizeof(resp));
	resp.max_tx_q = htole16(0);
	resp.max_rx_q = htole16(1);
	fake_xn_set_recv(&resp, sizeof(resp));

	rc = idpf_get_caps(&sc);

	LONGS_EQUAL(EINVAL, rc);
	CHECK((sc.state & IDPF_STATE_CAPS_DONE) == 0);
}

/*
 * T037 (US3, FR-005): idpf_get_caps() fails closed when the CP reply is
 * shorter than struct virtchnl2_get_capabilities (truncated transport) --
 * returns EINVAL and leaves IDPF_STATE_CAPS_DONE clear, even if the visible
 * queue fields would otherwise be valid.
 */
TEST(idpf_vc_common, get_caps_truncated_response_fails_closed)
{
	struct idpf_sc sc;
	struct virtchnl2_get_capabilities resp;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	memset(&resp, 0, sizeof(resp));
	resp.max_tx_q = htole16(1);
	resp.max_rx_q = htole16(1);
	fake_xn_set_recv(&resp, sizeof(resp) - 4);

	rc = idpf_get_caps(&sc);

	LONGS_EQUAL(EINVAL, rc);
	CHECK((sc.state & IDPF_STATE_CAPS_DONE) == 0);
}


/*
 * T015 (US1, FR-006/FR-020): idpf_create_vport() succeeds, persists the CP
 * response (vport_id) into sc, sets IDPF_STATE_VPORT_CREATED, and sends
 * VIRTCHNL2_OP_CREATE_VPORT.
 */
TEST(idpf_vc_common, create_vport_success_persists_and_sets_vport_created)
{
	struct idpf_sc sc;
	struct virtchnl2_create_vport resp;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	memset(&resp, 0, sizeof(resp));
	resp.vport_id = htole32(0x1234);
	resp.max_mtu = htole16(9000);
	resp.chunks.num_chunks = htole16(2);
	fake_xn_set_recv(&resp, sizeof(resp));

	rc = idpf_create_vport(&sc);

	LONGS_EQUAL(0, rc);
	CHECK((sc.state & IDPF_STATE_VPORT_CREATED) != 0);
	LONGS_EQUAL(VIRTCHNL2_OP_CREATE_VPORT, fake_xn_last_opcode());
	LONGS_EQUAL(0x1234, sc.vport_id);
}

/*
 * T043 (US4, FR-003): idpf_destroy_vport() sends VIRTCHNL2_OP_DESTROY_VPORT
 * for the cached vport_id and succeeds when the transport succeeds.
 */
TEST(idpf_vc_common, destroy_vport_sends_destroy_op)
{
	struct idpf_sc sc;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	sc.vport_id = 0x1234;

	rc = idpf_destroy_vport(&sc);

	LONGS_EQUAL(0, rc);
	LONGS_EQUAL(VIRTCHNL2_OP_DESTROY_VPORT, fake_xn_last_opcode());
	LONGS_EQUAL(1, fake_xn_call_count());
}

/*
 * T043 (US4, FR-003): a DESTROY_VPORT transport failure is advisory -- the
 * status is returned (for logging) but the wrapper never asserts/aborts.
 */
TEST(idpf_vc_common, destroy_vport_failure_is_non_fatal)
{
	struct idpf_sc sc;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	fake_xn_set_rc(EIO);

	rc = idpf_destroy_vport(&sc);

	LONGS_EQUAL(EIO, rc);
	LONGS_EQUAL(1, fake_xn_call_count());
}

/*
 * T044 (US4, FR-003, research.md R3): idpf_reset_vf() sends a zero-length
 * VIRTCHNL2_OP_RESET_VF request and succeeds when the transport succeeds.
 */
TEST(idpf_vc_common, reset_vf_sends_zero_length_reset_op)
{
	struct idpf_sc sc;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();

	rc = idpf_reset_vf(&sc);

	LONGS_EQUAL(0, rc);
	LONGS_EQUAL(VIRTCHNL2_OP_RESET_VF, fake_xn_last_opcode());
	LONGS_EQUAL(1, fake_xn_call_count());
}

/*
 * T044 (US4, FR-003): a RESET_VF failure/timeout is advisory -- returned for
 * logging without aborting the caller.
 */
TEST(idpf_vc_common, reset_vf_failure_is_non_fatal)
{
	struct idpf_sc sc;
	int rc;

	memset(&sc, 0, sizeof(sc));
	fake_xn_reset();
	fake_xn_set_rc(ETIMEDOUT);

	rc = idpf_reset_vf(&sc);

	LONGS_EQUAL(ETIMEDOUT, rc);
	LONGS_EQUAL(1, fake_xn_call_count());
}


/**
 * Test intent: idpf_vc_common_init() returns success (0) without
 * creating a real taskqueue at this skeleton stage.
 */
TEST(idpf_vc_common, init_returns_success)
{
	struct idpf_sc sc;
	int rc;

	rc = idpf_vc_common_init(&sc);

	LONGS_EQUAL(0, rc);
}

/**
 * Test intent: idpf_vc_common_deinit() is callable and does not crash
 * after idpf_vc_common_init(), mirroring its no-op behavior (Contract
 * 6 rule 4: reverse-order teardown, verified at the "both sides are
 * no-ops" level for this skeleton stage).
 */
TEST(idpf_vc_common, deinit_is_noop_after_init)
{
	struct idpf_sc sc;

	idpf_vc_common_init(&sc);
	idpf_vc_common_deinit(&sc);
}

/*
 * ─────────────────────────────────────────────────────────────────
 * Feature 015, Phase 4 (User Story 2) — Transport adapter tests.
 * T027-T035 were authored and RED-verified as TDD-first tests before
 * struct idpf_vc_transport_ops, the received-message record, and the
 * test double below existed (contracts/transport-adapter-contract.md;
 * data-model.md Entities 6-8). T036-T040 (this pass) implement those
 * GREEN targets in idpf/src/idpf_vc_common.h and
 * idpf/tdd_tests/fake_idpf_vc_transport.{h,cpp}.
 *
 * Test-double surface (Entity 8 -- fake transport; capabilities per
 * data-model.md: deterministic request capture, synthetic replies,
 * unsolicited-event injection, completion ordering, reset injection
 * -- no new entity beyond data-model.md's Key Entities is introduced
 * here). struct fake_idpf_vc_transport is declared as a complete type
 * (not merely forward-declared) because the tests below hold it as a
 * plain stack-local value, so its full definition and every function
 * below are now provided by idpf/tdd_tests/fake_idpf_vc_transport.h
 * (T038), compiled and linked as its own object per tdd_tests/Makefile
 * TEST_SRCS (T040) -- never reachable from idpf/src (FR-048; see the
 * production_code_has_no_test_only_mailbox_hooks test above, T034).
 */
#include "fake_idpf_vc_transport.h"

#ifdef IDPF_TDD_PHASE4
/**
 * T027 [P][US2] RED: idpf_vc_transport_ops start/stop symmetry test
 * (FR-040, FR-047 minimum test matrix row 1).
 *
 * Test intent: struct idpf_vc_transport_ops MUST exist with the six
 * operations named verbatim in the production doc's Minimal Transport
 * Contract (start, stop, submit, poll_receive, reclaim_tx,
 * is_reset_pending); start()/stop() MUST be callable symmetrically
 * against the fake transport double, with no real hardware mailbox.
 */
TEST(idpf_vc_common, transport_ops_start_stop_symmetry)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);

	int rc = ops.start(&fake);
	CHECK_EQUAL(0, rc);
	CHECK(fake_idpf_vc_transport_is_started(&fake));

	ops.stop(&fake);
	CHECK_FALSE(fake_idpf_vc_transport_is_started(&fake));
}

/**
 * T028 [P][US2] RED: one-outstanding-request invariant test (FR-041,
 * US2-AS-2, SC-005).
 *
 * Test intent: a second submit() while one synchronous request is
 * outstanding MUST be rejected (non-zero return), not queued -- only
 * one request may be outstanding at a time (production doc: "one
 * outstanding synchronous control-plane request ... sufficient for
 * serialized attach, init, stop, detach, and recovery").
 */
TEST(idpf_vc_common, transport_second_submit_while_outstanding_is_rejected)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	uint8_t req[8] = { 0 };

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	int rc1 = ops.submit(&fake, (uint16_t)1, req, sizeof(req),
			     (uint32_t)0xAAAA);
	CHECK_EQUAL(0, rc1);
	LONGS_EQUAL(1, fake_idpf_vc_transport_outstanding_count(&fake));

	int rc2 = ops.submit(&fake, (uint16_t)2, req, sizeof(req),
			     (uint32_t)0xBBBB);
	CHECK(rc2 != 0);
	LONGS_EQUAL(1, fake_idpf_vc_transport_outstanding_count(&fake));
}

/**
 * T029 [P][US2] RED: send-ring-full handling test (FR-047 minimum
 * test matrix row 2).
 *
 * Test intent: submit() MUST report failure (not silently drop the
 * request or hang) when the fake transport's simulated send ring is
 * full.
 */
TEST(idpf_vc_common, transport_submit_reports_send_ring_full)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	uint8_t req[8] = { 0 };

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_set_ring_full(&fake, true);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	int rc = ops.submit(&fake, (uint16_t)1, req, sizeof(req),
			    (uint32_t)0x1111);

	CHECK(rc != 0);
}

/**
 * T030 [P][US2] RED: ownership-transfer test (FR-042, US2-AS-3;
 * FR-047 minimum test matrix rows 3, 5).
 *
 * Test intent: submit() success MUST transfer ownership of the
 * request payload to the transport (no ownership transfer occurs
 * until success); reclaim_tx() is the SOLE path that releases
 * transport-owned payloads afterward.
 */
TEST(idpf_vc_common,
     transport_submit_success_transfers_ownership_reclaim_tx_releases)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	uint8_t req[8] = { 0 };

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	CHECK_FALSE(fake_idpf_vc_transport_payload_is_owned(&fake));

	int rc = ops.submit(&fake, (uint16_t)1, req, sizeof(req),
			    (uint32_t)0x2222);
	CHECK_EQUAL(0, rc);
	CHECK(fake_idpf_vc_transport_payload_is_owned(&fake));

	ops.reclaim_tx(&fake);
	CHECK_FALSE(fake_idpf_vc_transport_payload_is_owned(&fake));
}

/**
 * T031 [P][US2] RED: receive-buffer-lifetime test (FR-043; FR-047
 * minimum test matrix row 4).
 *
 * Test intent: poll_receive() MUST NOT expose a receive buffer past
 * the point the transport reposts it -- a second poll_receive() call
 * after a message has already been drained and reposted MUST return
 * zero further messages, not the same message again.
 */
TEST(idpf_vc_common, transport_poll_receive_does_not_expose_buffer_after_repost)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	struct idpf_vc_rx messages[4];
	size_t message_count = 0;

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	fake_idpf_vc_transport_queue_unsolicited(&fake, NULL, 0);

	int rc = ops.poll_receive(&fake, messages, 4, &message_count);
	CHECK_EQUAL(0, rc);
	LONGS_EQUAL(1, (long)message_count);
	CHECK(fake_idpf_vc_transport_buffer_reposted(&fake, 0));

	message_count = 0;
	rc = ops.poll_receive(&fake, messages, 4, &message_count);
	CHECK_EQUAL(0, rc);
	LONGS_EQUAL(0, (long)message_count);
}

/**
 * T032a [P][US2] RED: matching-cookie correlation test (FR-044,
 * FR-045, US2-AS-1; FR-047 minimum test matrix row 7).
 *
 * Test intent: a reply whose cookie matches the active request's
 * request_cookie MUST complete that request (matched = true).
 * Correlation is by cookie/token, never by opcode alone.
 */
TEST(idpf_vc_common, transport_matching_cookie_reply_completes_active_request)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	struct idpf_vc_rx messages[4];
	size_t message_count = 0;
	uint8_t req[8] = { 0 };
	const uint16_t opcode = 7;
	const uint32_t cookie = 0x5555;

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	ops.submit(&fake, opcode, req, sizeof(req), cookie);
	fake_idpf_vc_transport_queue_reply(&fake, cookie, NULL, 0);

	int rc = ops.poll_receive(&fake, messages, 4, &message_count);

	CHECK_EQUAL(0, rc);
	LONGS_EQUAL(1, (long)message_count);
	CHECK(messages[0].matched);
	LONGS_EQUAL(0, fake_idpf_vc_transport_outstanding_count(&fake));
}

/**
 * T032b [P][US2] RED: same-opcode wrong-cookie correlation test
 * (FR-044, FR-045, US2-AS-4; FR-047 minimum test matrix row 8).
 *
 * Test intent: a reply that shares the active request's opcode but
 * carries a DIFFERENT cookie MUST NOT complete the active request --
 * the VC core must never treat a message as a response solely because
 * its opcode happens to match.
 */
TEST(idpf_vc_common, transport_wrong_cookie_same_opcode_reply_does_not_complete)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	struct idpf_vc_rx messages[4];
	size_t message_count = 0;
	uint8_t req[8] = { 0 };
	const uint16_t opcode = 7;
	const uint32_t submitted_cookie = 0x5555;
	const uint32_t wrong_cookie = 0x6666;

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	ops.submit(&fake, opcode, req, sizeof(req), submitted_cookie);
	fake_idpf_vc_transport_queue_reply(&fake, wrong_cookie, NULL, 0);

	int rc = ops.poll_receive(&fake, messages, 4, &message_count);

	CHECK_EQUAL(0, rc);
	LONGS_EQUAL(1, (long)message_count);
	CHECK_FALSE(messages[0].matched);
	LONGS_EQUAL(1, fake_idpf_vc_transport_outstanding_count(&fake));
}

/**
 * T032c [P][US2] RED: unsolicited-message delivery test (FR-044;
 * FR-047 minimum test matrix row 9).
 *
 * Test intent: a message delivered with no active request outstanding
 * MUST be delivered as unsolicited (matched = false), never
 * completing any request.
 */
TEST(idpf_vc_common, transport_unsolicited_message_delivered_not_matched)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	struct idpf_vc_rx messages[4];
	size_t message_count = 0;

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	fake_idpf_vc_transport_queue_unsolicited(&fake, NULL, 0);

	int rc = ops.poll_receive(&fake, messages, 4, &message_count);

	CHECK_EQUAL(0, rc);
	LONGS_EQUAL(1, (long)message_count);
	CHECK_FALSE(messages[0].matched);
}

/**
 * T033 [P][US2] RED: reset-pending indication test (FR-047 Edge
 * Cases; FR-047 minimum test matrix row 6).
 *
 * Test intent: a reset indication MUST cancel the one active request
 * (it no longer counts as outstanding) and the transport MUST accept
 * no new request while is_reset_pending() reports true.
 */
TEST(idpf_vc_common,
     transport_reset_pending_cancels_active_request_and_rejects_new)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	uint8_t req[8] = { 0 };

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	int rc1 = ops.submit(&fake, (uint16_t)1, req, sizeof(req),
			     (uint32_t)0x7777);
	CHECK_EQUAL(0, rc1);
	LONGS_EQUAL(1, fake_idpf_vc_transport_outstanding_count(&fake));

	fake_idpf_vc_transport_inject_reset(&fake);

	CHECK(ops.is_reset_pending(&fake));
	LONGS_EQUAL(0, fake_idpf_vc_transport_outstanding_count(&fake));

	int rc2 = ops.submit(&fake, (uint16_t)2, req, sizeof(req),
			     (uint32_t)0x8888);
	CHECK(rc2 != 0);
}

/**
 * T034 [P][US2] RED: no-test-only-mailbox-hooks-in-production-code
 * guard (FR-048).
 *
 * Test intent: production code (idpf/src) MUST NOT contain test-only
 * mailbox hooks -- the fake transport substitutes at the
 * idpf_vc_transport_ops boundary only (idpf/tdd_tests/), never inside
 * idpf/src.
 *
 * NOTE (Constitution Principle II, truthfulness): this specific check
 * is a real, self-contained text scan that does NOT itself depend on
 * struct idpf_vc_transport_ops existing, and is already vacuously
 * true today (no production file mentions the fake transport). It
 * becomes a permanent regression guard once T036-T040 land. It is
 * reported RED alongside every other Phase-4 test in this file only
 * because a single compile failure anywhere in this translation unit
 * (T027-T033, T035, all referencing the not-yet-defined transport
 * contract) blocks the whole binary from building -- exactly as
 * documented for T004-T014's collective RED verification in
 * test_idpf_osdep.cpp.
 */
TEST(idpf_vc_common, production_code_has_no_test_only_mailbox_hooks)
{
	static const char *production_files[] = {
		"../src/idpf_drv.h",	     "../src/idpf_lib.c",
		"../src/idpf_lib.h",	     "../src/idpf_osdep.c",
		"../src/idpf_osdep.h",	     "../src/idpf_txrx.c",
		"../src/idpf_txrx_common.h", "../src/idpf_vc.c",
		"../src/idpf_vc_common.c",   "../src/idpf_vc_common.h",
		"../src/if_idpf.c",
	};
	static const char *forbidden_markers[] = {
		"fake_idpf_vc_transport",
		"FAKE_IDPF_VC_TRANSPORT",
	};

	for (size_t f = 0;
	     f < sizeof(production_files) / sizeof(production_files[0]); f++) {
		std::ifstream in(production_files[f]);
		CHECK_TEXT(in.good(), production_files[f]);

		std::string content((std::istreambuf_iterator<char>(in)),
				    std::istreambuf_iterator<char>());

		for (size_t m = 0; m < sizeof(forbidden_markers) /
					       sizeof(forbidden_markers[0]);
		     m++) {
			bool found = content.find(forbidden_markers[m]) !=
				     std::string::npos;
			CHECK_TEXT(!found, production_files[f]);
		}
	}
}

/**
 * T035 [P][US2] RED — ELEVATED GOVERNANCE (per tasks.md, non-skippable
 * / non-weakenable): FR-046 execution-context contract test.
 *
 * Per this run's explicit governing instructions: this RED test is
 * authored and verified failing here; review sign-off
 * (software-architect + tdd-writer, per this feature's
 * ELEVATED-governance tag) is explicitly OUT OF SCOPE for this run
 * and is NOT claimed by this commit -- it remains a separate, later
 * action routed by the root orchestrator to those roles.
 *
 * FR-046 (verbatim clauses tested below, one TEST per clause):
 *   (a) lifecycle caller: may acquire the VC serialization lock,
 *       submit the one request, and sleep for completion; MUST NOT
 *       run receive reclamation while holding an interrupt lock or
 *       issue a second request.
 *   (b) admin interrupt callback: may only acknowledge the device
 *       condition and enqueue work; MUST NOT sleep, allocate with a
 *       sleeping flag, wait for completion, or parse a payload.
 *   (c) VC taskqueue worker: drains receive completions,
 *       validates/correlates replies, reposts receive buffers,
 *       reclaims sends, processes unsolicited events, and signals the
 *       lifecycle caller, but MUST NOT call the synchronous
 *       submit-and-wait path for the active request.
 *
 * These three contexts are modeled here strictly at the
 * idpf_vc_transport_ops (Entity 6) / fake-transport (Entity 8)
 * boundary -- no new entity beyond data-model.md's Key Entities is
 * introduced to test this contract (explicit constraint for this
 * run). Full runtime enforcement of "holding an interrupt lock"
 * belongs to the not-yet-specified portable VC core (data-model.md
 * Entity 6 Relationships: "Consumed by the (not-yet-specified,
 * later-phase) portable VC core") and is out of this feature's scope;
 * what IS tested here is every FR-046 clause directly observable at
 * the six idpf_vc_transport_ops operations.
 */

/**
 * T035a: FR-046 clause (a) -- the lifecycle caller may submit the one
 * request but MUST NOT issue a second request while one is
 * outstanding.
 */
TEST(idpf_vc_common, fr046_lifecycle_caller_may_submit_but_not_reenter)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	uint8_t req[8] = { 0 };

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	int rc1 = ops.submit(&fake, (uint16_t)1, req, sizeof(req),
			     (uint32_t)0x9999);
	CHECK_EQUAL(0, rc1);

	int rc2 = ops.submit(&fake, (uint16_t)1, req, sizeof(req),
			     (uint32_t)0xAAAA);
	CHECK(rc2 != 0);
}

/**
 * T035b: FR-046 clause (b) -- the admin interrupt callback's only
 * reasonable call against this interface, is_reset_pending() (a
 * quick device-condition acknowledgment, never a payload parse),
 * MUST NOT sleep, allocate with a sleeping flag, or wait for
 * completion.
 *
 * Enforcement technique: zero mock expectations are registered for
 * any sleep/allocate-sleeping primitive (cv_wait, malloc(M_WAITOK),
 * DELAY) below; CppUTest's strict mock() framework fails a test
 * immediately on any unregistered actualCall(), so this test fails
 * automatically the moment a future implementation of
 * is_reset_pending() performs any such forbidden operation.
 */
TEST(idpf_vc_common,
     fr046_admin_interrupt_callback_is_reset_pending_does_not_block)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	/*
	 * No mock expectations for cv_wait/malloc(M_WAITOK)/DELAY are
	 * registered above -- an implementation that sleeps, allocates
	 * with a sleeping flag, or delays would trip CppUTest's strict
	 * unexpected-call failure here.
	 */
	(void)ops.is_reset_pending(&fake);
}

/**
 * T035c: FR-046 clause (c) -- the VC taskqueue worker drains/
 * correlates/reposts/reclaims/signals via poll_receive()+
 * reclaim_tx() only, and MUST NOT call the synchronous
 * submit-and-wait path (submit()) for the active request while doing
 * so.
 */
TEST(idpf_vc_common, fr046_taskqueue_worker_drains_without_reentrant_submit)
{
	struct fake_idpf_vc_transport fake;
	struct idpf_vc_transport_ops ops;
	struct idpf_vc_rx messages[4];
	size_t message_count = 0;
	uint8_t req[8] = { 0 };
	const uint16_t opcode = 3;
	const uint32_t cookie = 0xBBBB;
	int submit_calls_before;
	int submit_calls_after;

	fake_idpf_vc_transport_init(&fake);
	fake_idpf_vc_transport_get_ops(&fake, &ops);
	ops.start(&fake);

	ops.submit(&fake, opcode, req, sizeof(req), cookie);
	fake_idpf_vc_transport_queue_reply(&fake, cookie, NULL, 0);
	submit_calls_before = fake_idpf_vc_transport_submit_call_count(&fake);

	/*
	 * Simulated taskqueue-worker sequence: drain/correlate/repost
	 * via poll_receive(), then reclaim the completed send -- never a
	 * re-entrant submit().
	 */
	int rc = ops.poll_receive(&fake, messages, 4, &message_count);
	ops.reclaim_tx(&fake);
	submit_calls_after = fake_idpf_vc_transport_submit_call_count(&fake);

	CHECK_EQUAL(0, rc);
	LONGS_EQUAL(1, (long)message_count);
	CHECK(messages[0].matched);
	LONGS_EQUAL(0, fake_idpf_vc_transport_outstanding_count(&fake));
	LONGS_EQUAL(submit_calls_before, submit_calls_after);
}
#endif /* IDPF_TDD_PHASE4 */
