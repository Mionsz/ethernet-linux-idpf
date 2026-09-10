/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Intel Corporation
 *
 * idpf/tdd_tests/fake_idpf_vc_transport.h
 *
 * @file fake_idpf_vc_transport.h
 * @brief CppUTest-only test double for struct idpf_vc_transport_ops
 *        (Feature 015, User Story 2, T038; FR-047, FR-048).
 *
 * @details
 * Implements the abstract one-outstanding-request transport contract
 * (contracts/transport-adapter-contract.md) with deterministic
 * request capture, synthetic replies, unsolicited-event injection,
 * completion ordering, and reset injection -- no real hardware
 * mailbox. This double exists ONLY under idpf/tdd_tests/ and MUST
 * NEVER be linked into production idpf.ko (FR-048); no production
 * file under idpf/src references any symbol declared here (enforced
 * by test_idpf_vc_common.cpp's production_code_has_no_test_only_
 * mailbox_hooks test, T034).
 *
 * struct fake_idpf_vc_transport is intentionally a complete (not
 * opaque) type here so callers may declare it as a plain stack-local
 * value (test_idpf_vc_common.cpp's T027-T035 RED tests already do
 * exactly that), matching CppUTest test convention elsewhere in this
 * harness.
 */
#ifndef _FAKE_IDPF_VC_TRANSPORT_H_
#define _FAKE_IDPF_VC_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

#include "../src/idpf_vc_common.h"

/**
 * FAKE_IDPF_VC_TRANSPORT_MAX_PENDING: fixed capacity for queued
 * reply/unsolicited messages. Generous relative to this feature's own
 * 9-row minimum test matrix (SC-004), each of which queues at most a
 * couple of messages per test.
 */
#define FAKE_IDPF_VC_TRANSPORT_MAX_PENDING 8

/**
 * @struct fake_idpf_vc_transport_pending_msg
 * @brief One queued reply or unsolicited message awaiting delivery via
 *        poll_receive().
 */
struct fake_idpf_vc_transport_pending_msg {
	bool valid; /**< Slot populated by a queue_reply()/queue_unsolicited() call. */
	bool reposted; /**< Delivered via poll_receive() already (FR-043: never re-exposed). */
	bool is_reply; /**< true = queued via queue_reply() (cookie-correlated); false = unsolicited. */
	uint32_t cookie; /**< Meaningful only when is_reply is true. */
	IDPF_IOVEC
		payload; /**< Caller-supplied payload reference (may be {NULL, 0}). */
};

/**
 * @struct fake_idpf_vc_transport
 * @brief State for one fake transport instance (Entity 8).
 */
struct fake_idpf_vc_transport {
	bool started;
	bool ring_full;
	bool reset_pending;
	bool outstanding; /**< One-outstanding-request tracking (FR-041). */
	bool payload_owned; /**< Ownership-transfer tracking (FR-042). */
	uint32_t outstanding_cookie;
	int submit_call_count;
	size_t pending_count;
	struct fake_idpf_vc_transport_pending_msg
		pending[FAKE_IDPF_VC_TRANSPORT_MAX_PENDING];
};

void fake_idpf_vc_transport_init(struct fake_idpf_vc_transport *fake);
void fake_idpf_vc_transport_get_ops(struct fake_idpf_vc_transport *fake,
				    struct idpf_vc_transport_ops *ops);
bool fake_idpf_vc_transport_is_started(struct fake_idpf_vc_transport *fake);
int fake_idpf_vc_transport_outstanding_count(
	struct fake_idpf_vc_transport *fake);
int fake_idpf_vc_transport_submit_call_count(
	struct fake_idpf_vc_transport *fake);
void fake_idpf_vc_transport_set_ring_full(struct fake_idpf_vc_transport *fake,
					  bool full);
bool fake_idpf_vc_transport_payload_is_owned(
	struct fake_idpf_vc_transport *fake);
void fake_idpf_vc_transport_queue_reply(struct fake_idpf_vc_transport *fake,
					uint32_t cookie, const void *payload,
					size_t len);
void fake_idpf_vc_transport_queue_unsolicited(
	struct fake_idpf_vc_transport *fake, const void *payload, size_t len);
bool fake_idpf_vc_transport_buffer_reposted(struct fake_idpf_vc_transport *fake,
					    size_t index);
void fake_idpf_vc_transport_inject_reset(struct fake_idpf_vc_transport *fake);

#endif /* _FAKE_IDPF_VC_TRANSPORT_H_ */
