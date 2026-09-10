/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Intel Corporation
 *
 * idpf/tdd_tests/fake_idpf_vc_transport.cpp
 *
 * @file fake_idpf_vc_transport.cpp
 * @brief CppUTest-only test double for struct idpf_vc_transport_ops
 *        (Feature 015, User Story 2, T038/T039; FR-041, FR-042,
 *        FR-043, FR-044, FR-045, FR-047, FR-048).
 *
 * @details
 * Never linked into production idpf.ko; compiled as its own object
 * and linked into the idpf_tests binary (see tdd_tests/Makefile
 * TEST_SRCS) alongside the CppUTest test files. Uses no CppUTest
 * mocking primitives itself -- it is a plain, deterministic state
 * machine so that the FR-046 clause (b) test
 * (fr046_admin_interrupt_callback_is_reset_pending_does_not_block)
 * can rely on CppUTest's strict-mock unexpected-call enforcement to
 * catch any future regression (this double makes zero mock() calls,
 * so there is nothing here to trip that enforcement).
 */
#include <string.h>

#include "idpf_utest.h"

#include "fake_idpf_vc_transport.h"

void fake_idpf_vc_transport_init(struct fake_idpf_vc_transport *fake)
{
	memset(fake, 0, sizeof(*fake));
}

/**
 * One-outstanding-request invariant (T039, FR-041): a submit() while
 * one request is already outstanding, the send ring is simulated
 * full, or a reset is pending, is rejected (non-zero return), never
 * queued.
 */
static int fake_idpf_vc_transport_start(void *transport)
{
	struct fake_idpf_vc_transport *fake =
		(struct fake_idpf_vc_transport *)transport;

	fake->started = true;
	return (0);
}

static void fake_idpf_vc_transport_stop(void *transport)
{
	struct fake_idpf_vc_transport *fake =
		(struct fake_idpf_vc_transport *)transport;

	fake->started = false;
}

static int fake_idpf_vc_transport_submit(void *transport, uint16_t opcode,
					 const void *request,
					 size_t request_length,
					 uint32_t request_cookie)
{
	struct fake_idpf_vc_transport *fake =
		(struct fake_idpf_vc_transport *)transport;

	(void)opcode;
	(void)request;
	(void)request_length;

	fake->submit_call_count++;

	if (fake->reset_pending)
		return (-1);
	if (fake->ring_full)
		return (-1);
	if (fake->outstanding)
		return (-1); /* T028/T035a: one-outstanding-request invariant. */

	fake->outstanding = true;
	fake->outstanding_cookie = request_cookie;
	fake->payload_owned =
		true; /* FR-042: ownership transfers on success. */

	return (0);
}

/**
 * poll_receive(): delivers every queued, not-yet-reposted message up
 * to message_capacity. A reply (queue_reply()) whose cookie matches
 * the active request's cookie completes that request (matched =
 * true, outstanding cleared); any other reply, or an unsolicited
 * message, is delivered with matched = false and never completes a
 * request (FR-044, FR-045). Delivered messages are marked reposted
 * and are never exposed again (FR-043).
 */
static int fake_idpf_vc_transport_poll_receive(void *transport,
					       struct idpf_vc_rx *messages,
					       size_t message_capacity,
					       size_t *message_count)
{
	struct fake_idpf_vc_transport *fake =
		(struct fake_idpf_vc_transport *)transport;
	size_t delivered = 0;

	for (size_t i = 0;
	     i < fake->pending_count && delivered < message_capacity; i++) {
		struct fake_idpf_vc_transport_pending_msg *m =
			&fake->pending[i];
		bool matched = false;

		if (!m->valid || m->reposted)
			continue;

		if (m->is_reply && fake->outstanding &&
		    m->cookie == fake->outstanding_cookie) {
			matched = true;
			fake->outstanding = false;
		}

		messages[delivered].matched = matched;
		messages[delivered].payload = m->payload;
		m->reposted = true;
		delivered++;
	}

	*message_count = delivered;
	return (0);
}

static void fake_idpf_vc_transport_reclaim_tx(void *transport)
{
	struct fake_idpf_vc_transport *fake =
		(struct fake_idpf_vc_transport *)transport;

	fake->payload_owned = false; /* FR-042: sole release path. */
}

static bool fake_idpf_vc_transport_is_reset_pending(void *transport)
{
	struct fake_idpf_vc_transport *fake =
		(struct fake_idpf_vc_transport *)transport;

	return (fake->reset_pending);
}

void fake_idpf_vc_transport_get_ops(struct fake_idpf_vc_transport *fake,
				    struct idpf_vc_transport_ops *ops)
{
	(void)fake;

	ops->start = fake_idpf_vc_transport_start;
	ops->stop = fake_idpf_vc_transport_stop;
	ops->submit = fake_idpf_vc_transport_submit;
	ops->poll_receive = fake_idpf_vc_transport_poll_receive;
	ops->reclaim_tx = fake_idpf_vc_transport_reclaim_tx;
	ops->is_reset_pending = fake_idpf_vc_transport_is_reset_pending;
}

bool fake_idpf_vc_transport_is_started(struct fake_idpf_vc_transport *fake)
{
	return (fake->started);
}

int fake_idpf_vc_transport_outstanding_count(struct fake_idpf_vc_transport *fake)
{
	return (fake->outstanding ? 1 : 0);
}

int fake_idpf_vc_transport_submit_call_count(struct fake_idpf_vc_transport *fake)
{
	return (fake->submit_call_count);
}

void fake_idpf_vc_transport_set_ring_full(struct fake_idpf_vc_transport *fake,
					  bool full)
{
	fake->ring_full = full;
}

bool fake_idpf_vc_transport_payload_is_owned(struct fake_idpf_vc_transport *fake)
{
	return (fake->payload_owned);
}

static void fake_idpf_vc_transport_queue(struct fake_idpf_vc_transport *fake,
					 bool is_reply, uint32_t cookie,
					 const void *payload, size_t len)
{
	struct fake_idpf_vc_transport_pending_msg *m;

	if (fake->pending_count >= FAKE_IDPF_VC_TRANSPORT_MAX_PENDING)
		return; /* Defensive: generous fixed capacity, never hit by this feature's test matrix. */

	m = &fake->pending[fake->pending_count++];
	m->valid = true;
	m->reposted = false;
	m->is_reply = is_reply;
	m->cookie = cookie;
	m->payload.iov_base = (void *)(uintptr_t)payload;
	m->payload.iov_len = len;
}

void fake_idpf_vc_transport_queue_reply(struct fake_idpf_vc_transport *fake,
					uint32_t cookie, const void *payload,
					size_t len)
{
	fake_idpf_vc_transport_queue(fake, true, cookie, payload, len);
}

void fake_idpf_vc_transport_queue_unsolicited(
	struct fake_idpf_vc_transport *fake, const void *payload, size_t len)
{
	fake_idpf_vc_transport_queue(fake, false, 0, payload, len);
}

bool fake_idpf_vc_transport_buffer_reposted(struct fake_idpf_vc_transport *fake,
					    size_t index)
{
	if (index >= fake->pending_count)
		return (false);

	return (fake->pending[index].reposted);
}

/**
 * inject_reset(): a reset cancels the one active request (it no
 * longer counts as outstanding) and the transport accepts no new
 * request while is_reset_pending() reports true (T033).
 */
void fake_idpf_vc_transport_inject_reset(struct fake_idpf_vc_transport *fake)
{
	fake->reset_pending = true;
	fake->outstanding = false;
}
