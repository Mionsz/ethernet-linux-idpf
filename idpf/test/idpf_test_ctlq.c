/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Control queue and simulated control plane.
 *
 * idpf_ctlq_send() publishes descriptors by writing the tail register and
 * idpf_ctlq_recv() consumes them by polling the DD flag, so a "control plane"
 * that reads the simulated register file and flips descriptor bits is
 * indistinguishable from hardware as far as the driver is concerned.  The
 * driver code under test is unmodified.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>

#include <machine/bus.h>

#include "idpf.h"
#include "idpf_controlq.h"
#include "idpf_test.h"
#include "idpf_test_env.h"

/* Arbitrary offsets inside the simulated register file. */
#define TEST_ASQ_HEAD	0x1000
#define TEST_ASQ_TAIL	0x1004
#define TEST_ASQ_LEN	0x1008
#define TEST_ASQ_BAH	0x100C
#define TEST_ASQ_BAL	0x1010
#define TEST_ARQ_HEAD	0x2000
#define TEST_ARQ_TAIL	0x2004
#define TEST_ARQ_LEN	0x2008
#define TEST_ARQ_BAH	0x200C
#define TEST_ARQ_BAL	0x2010

#define TEST_LEN_MASK		0x1FFF
#define TEST_LEN_ENA_MASK	(1U << 31)
#define TEST_HEAD_MASK		0x1FFF

/*
 * Deliberately small.  Each receive descriptor gets its own DMA buffer, tag
 * and map, so ring length multiplies straight into kernel resource pressure;
 * eight is enough to exercise wrap and full-ring behaviour.
 */
#define TEST_RING_LEN	8
#define TEST_BUF_LEN	IDPF_CTLQ_MAX_BUF_LEN

#define TEST_LO32(x)	((uint32_t)((x) & 0xffffffffULL))
#define TEST_HI32(x)	((uint32_t)(((uint64_t)(x)) >> 32))

struct ctlq_fixture {
	struct idpf_test_env	 env;
	struct idpf_ctlq_info	*asq;
	struct idpf_ctlq_info	*arq;
	uint16_t		 cp_next_reply;	/* CP's write cursor on the ARQ */
};

static void
ctlq_fill_create_info(struct idpf_ctlq_create_info *info)
{

	bzero(info, 2 * sizeof(*info));

	info[0].type = IDPF_CTLQ_TYPE_MAILBOX_TX;
	info[0].id = IDPF_DFLT_MBX_ID;
	info[0].len = TEST_RING_LEN;
	info[0].buf_size = TEST_BUF_LEN;
	info[0].reg.head = TEST_ASQ_HEAD;
	info[0].reg.tail = TEST_ASQ_TAIL;
	info[0].reg.len = TEST_ASQ_LEN;
	info[0].reg.bah = TEST_ASQ_BAH;
	info[0].reg.bal = TEST_ASQ_BAL;
	info[0].reg.len_mask = TEST_LEN_MASK;
	info[0].reg.len_ena_mask = TEST_LEN_ENA_MASK;
	info[0].reg.head_mask = TEST_HEAD_MASK;

	info[1].type = IDPF_CTLQ_TYPE_MAILBOX_RX;
	info[1].id = IDPF_DFLT_MBX_ID;
	info[1].len = TEST_RING_LEN;
	info[1].buf_size = TEST_BUF_LEN;
	info[1].reg.head = TEST_ARQ_HEAD;
	info[1].reg.tail = TEST_ARQ_TAIL;
	info[1].reg.len = TEST_ARQ_LEN;
	info[1].reg.bah = TEST_ARQ_BAH;
	info[1].reg.bal = TEST_ARQ_BAL;
	info[1].reg.len_mask = TEST_LEN_MASK;
	info[1].reg.len_ena_mask = TEST_LEN_ENA_MASK;
	info[1].reg.head_mask = TEST_HEAD_MASK;
}

/**
 * ctlq_find - locate a queue on the hw list
 * @hw: hardware handle
 * @type: queue type to match
 * @id: queue identifier to match
 *
 * The driver's own idpf_find_ctlq() is file-static, so the walk is repeated
 * here rather than widening the driver's linkage for a test.
 *
 * Return: the queue, or NULL.
 */
static struct idpf_ctlq_info *
ctlq_find(struct idpf_hw *hw, enum idpf_ctlq_type type, int id)
{
	struct idpf_ctlq_info *cq;

	LIST_FOREACH(cq, &hw->cq_list_head, cq_list) {
		if (cq->cq_type == type && cq->q_id == id)
			return (cq);
	}

	return (NULL);
}

/**
 * ctlq_fixture_setup - stand up a mailbox pair on the simulated register file
 * @f: fixture to populate
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
ctlq_fixture_setup(struct ctlq_fixture *f)
{
	struct idpf_ctlq_create_info info[2];
	struct idpf_hw *hw;
	int err;

	bzero(f, sizeof(*f));

	err = idpf_test_env_setup(&f->env);
	if (err != 0)
		return (err);

	hw = &f->env.adapter->hw;
	ctlq_fill_create_info(info);

	err = idpf_ctlq_init(hw, 2, info);
	if (err != 0) {
		idpf_test_env_teardown(&f->env);
		return (err);
	}

	f->asq = ctlq_find(hw, IDPF_CTLQ_TYPE_MAILBOX_TX, IDPF_DFLT_MBX_ID);
	f->arq = ctlq_find(hw, IDPF_CTLQ_TYPE_MAILBOX_RX, IDPF_DFLT_MBX_ID);

	return (0);
}

static void
ctlq_fixture_teardown(struct ctlq_fixture *f)
{

	if (f->env.adapter != NULL)
		idpf_ctlq_deinit(&f->env.adapter->hw);
	idpf_test_env_teardown(&f->env);
}

/**
 * cp_complete_sends - act as the control plane completing posted commands
 * @f: fixture
 *
 * Marks every descriptor the driver has published as done, the way firmware
 * would after consuming it.
 *
 * Return: number of descriptors completed.
 */
static int
cp_complete_sends(struct ctlq_fixture *f)
{
	struct idpf_ctlq_info *cq = f->asq;
	uint32_t tail;
	uint16_t i;
	int n = 0;

	tail = idpf_test_reg_get(&f->env, cq->reg.tail);

	for (i = 0; i != tail; i = (i + 1) % cq->ring_size) {
		struct idpf_ctlq_desc *desc = IDPF_CTLQ_DESC(cq, i);

		desc->flags |= htole16(IDPF_CTLQ_FLAG_DD |
		    IDPF_CTLQ_FLAG_CMP);
		n++;
		if (n > cq->ring_size)
			break;
	}

	return (n);
}

/**
 * cp_post_reply - act as the control plane delivering an event
 * @f: fixture
 * @chnl_opcode: virtchnl opcode to report
 * @chnl_retval: virtchnl status to report
 * @payload: bytes to place in the receive buffer, may be NULL
 * @len: length of @payload
 */
static void
cp_post_reply(struct ctlq_fixture *f, uint32_t chnl_opcode,
    uint32_t chnl_retval, const void *payload, uint16_t len)
{
	struct idpf_ctlq_info *cq = f->arq;
	struct idpf_ctlq_desc *desc;
	uint16_t idx = f->cp_next_reply;

	desc = IDPF_CTLQ_DESC(cq, idx);
	bzero(desc, sizeof(*desc));

	desc->opcode = htole16(idpf_mbq_opc_send_msg_to_peer_drv);
	desc->cookie_high = htole32(chnl_opcode);
	desc->cookie_low = htole32(chnl_retval);
	desc->datalen = htole16(len);

	if (payload != NULL && len != 0 && cq->bi.rx_buff[idx] != NULL)
		memcpy(cq->bi.rx_buff[idx]->va, payload, len);

	/* DD last: the driver reads no other field until it is set. */
	atomic_thread_fence_rel();
	desc->flags = htole16(IDPF_CTLQ_FLAG_DD);

	f->cp_next_reply = (idx + 1) % cq->ring_size;
}

/* ------------------------------------------------------------------ */

static void
test_ctlq_init_programs_registers(void)
{
	struct ctlq_fixture f;

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}

	IDPF_EXPECT_NOT_NULL(f.asq);
	IDPF_EXPECT_NOT_NULL(f.arq);
	if (f.asq == NULL || f.arq == NULL)
		goto out;

	IDPF_EXPECT_EQ(f.asq->ring_size, TEST_RING_LEN);
	IDPF_EXPECT_EQ(f.arq->ring_size, TEST_RING_LEN);

	/* Ring base must reach the register file as a split physical address. */
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ASQ_BAL),
	    TEST_LO32(f.asq->desc_ring.pa));
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ASQ_BAH),
	    TEST_HI32(f.asq->desc_ring.pa));
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ASQ_LEN),
	    (TEST_RING_LEN | TEST_LEN_ENA_MASK));

	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ARQ_BAL),
	    TEST_LO32(f.arq->desc_ring.pa));
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ARQ_LEN),
	    (TEST_RING_LEN | TEST_LEN_ENA_MASK));

	/* Receive buffers are posted at init, so the ring starts full. */
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ARQ_TAIL),
	    TEST_RING_LEN - 1);

	/* A physical address of zero would mean the DMA load silently failed. */
	IDPF_EXPECT(f.asq->desc_ring.pa != 0, "ASQ ring has no bus address");
	IDPF_EXPECT(f.arq->desc_ring.pa != 0, "ARQ ring has no bus address");

out:
	ctlq_fixture_teardown(&f);
}

static void
test_ctlq_deinit_clears_registers(void)
{
	struct ctlq_fixture f;

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}

	idpf_ctlq_deinit(&f.env.adapter->hw);

	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ASQ_LEN), 0);
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ASQ_BAL), 0);
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ASQ_BAH), 0);
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ARQ_LEN), 0);

	f.env.adapter->hw.asq = NULL;
	f.env.adapter->hw.arq = NULL;
	idpf_test_env_teardown(&f.env);
}

static void
test_ctlq_send_writes_descriptor(void)
{
	static const uint8_t ctx[IDPF_INDIRECT_CTX_SIZE] = {
		0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04
	};
	struct idpf_dma_mem payload;
	struct ctlq_fixture f;
	struct idpf_ctlq_msg msg;
	struct idpf_ctlq_desc *desc;
	uint16_t flags;

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	bzero(&payload, sizeof(payload));
	if (idpf_alloc_dma_mem(&f.env.adapter->hw, &payload, 256) == NULL) {
		IDPF_EXPECT(false, "payload allocation failed");
		goto out;
	}

	bzero(&msg, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;
	msg.func_id = 0;
	msg.data_len = 64;
	msg.cookie.mbx.chnl_opcode = VIRTCHNL2_OP_GET_CAPS;
	msg.cookie.mbx.chnl_retval = 0;
	memcpy(msg.ctx.indirect.context, ctx, sizeof(ctx));
	msg.ctx.indirect.payload = &payload;

	IDPF_EXPECT_OK(idpf_ctlq_send(&f.env.adapter->hw, f.asq, 1, &msg));

	/* The doorbell is the only thing hardware would have seen. */
	IDPF_EXPECT_EQ(idpf_test_reg_get(&f.env, TEST_ASQ_TAIL), 1);
	IDPF_EXPECT_EQ(f.asq->next_to_use, 1);

	desc = IDPF_CTLQ_DESC(f.asq, 0);
	IDPF_EXPECT_EQ(le16toh(desc->opcode), idpf_mbq_opc_send_msg_to_pf);
	IDPF_EXPECT_EQ(le16toh(desc->datalen), 64);
	IDPF_EXPECT_EQ(le32toh(desc->cookie_high), VIRTCHNL2_OP_GET_CAPS);
	IDPF_EXPECT_EQ(le32toh(desc->cookie_low), 0);

	flags = le16toh(desc->flags);
	IDPF_EXPECT((flags & IDPF_CTLQ_FLAG_BUF) != 0, "BUF flag not set");
	IDPF_EXPECT((flags & IDPF_CTLQ_FLAG_RD) != 0, "RD flag not set");

	IDPF_EXPECT_EQ(le32toh(desc->params.indirect.addr_low),
	    TEST_LO32(payload.pa));
	IDPF_EXPECT_EQ(le32toh(desc->params.indirect.addr_high),
	    TEST_HI32(payload.pa));

	idpf_free_dma_mem(&f.env.adapter->hw, &payload);
out:
	ctlq_fixture_teardown(&f);
}

static void
test_ctlq_send_rejects_missing_payload(void)
{
	struct ctlq_fixture f;
	struct idpf_ctlq_msg msg;

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	bzero(&msg, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;
	msg.data_len = 64;
	msg.ctx.indirect.payload = NULL;

	IDPF_EXPECT_ERR(idpf_ctlq_send(&f.env.adapter->hw, f.asq, 1, &msg),
	    EBADMSG);
out:
	ctlq_fixture_teardown(&f);
}

static void
test_ctlq_send_full_ring(void)
{
	struct ctlq_fixture f;
	struct idpf_ctlq_msg msg;
	int i, err = 0;

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	bzero(&msg, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;
	msg.data_len = 0;

	/* One slot is always kept free to distinguish full from empty. */
	for (i = 0; i < TEST_RING_LEN - 1; i++) {
		err = idpf_ctlq_send(&f.env.adapter->hw, f.asq, 1, &msg);
		if (err != 0)
			break;
	}
	IDPF_EXPECT_EQ(err, 0);
	IDPF_EXPECT_EQ(i, TEST_RING_LEN - 1);

	IDPF_EXPECT_ERR(idpf_ctlq_send(&f.env.adapter->hw, f.asq, 1, &msg),
	    ENOSPC);
out:
	ctlq_fixture_teardown(&f);
}

static void
test_ctlq_clean_sq_reclaims(void)
{
	struct ctlq_fixture f;
	struct idpf_ctlq_msg msg;
	struct idpf_ctlq_msg *status[8];
	uint16_t clean = nitems(status);
	int i;

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	bzero(&msg, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;

	for (i = 0; i < 4; i++)
		IDPF_EXPECT_OK(idpf_ctlq_send(&f.env.adapter->hw, f.asq, 1,
		    &msg));

	/* Nothing is reclaimable until the control plane completes them. */
	clean = nitems(status);
	IDPF_EXPECT_OK(idpf_ctlq_clean_sq(f.asq, &clean, status));
	IDPF_EXPECT_EQ(clean, 0);

	IDPF_EXPECT_EQ(cp_complete_sends(&f), 4);

	clean = nitems(status);
	IDPF_EXPECT_OK(idpf_ctlq_clean_sq(f.asq, &clean, status));
	IDPF_EXPECT_EQ(clean, 4);
	IDPF_EXPECT_EQ(f.asq->next_to_clean, 4);
out:
	ctlq_fixture_teardown(&f);
}

static void
test_ctlq_recv_empty(void)
{
	struct idpf_ctlq_msg msgs[4];
	struct ctlq_fixture f;
	uint16_t n = nitems(msgs);

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	IDPF_EXPECT_ERR(idpf_ctlq_recv(f.arq, &n, msgs), ENOMSG);
	IDPF_EXPECT_EQ(n, 0);
out:
	ctlq_fixture_teardown(&f);
}

static void
test_cp_reply_is_received(void)
{
	static const uint8_t body[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	struct idpf_ctlq_msg msgs[4];
	struct ctlq_fixture f;
	uint16_t n = nitems(msgs);

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	cp_post_reply(&f, VIRTCHNL2_OP_GET_CAPS, 0, body, sizeof(body));

	IDPF_EXPECT_OK(idpf_ctlq_recv(f.arq, &n, msgs));
	IDPF_EXPECT_EQ(n, 1);
	if (n != 1)
		goto out;

	IDPF_EXPECT_EQ(msgs[0].cookie.mbx.chnl_opcode, VIRTCHNL2_OP_GET_CAPS);
	IDPF_EXPECT_EQ(msgs[0].cookie.mbx.chnl_retval, 0);
	IDPF_EXPECT_EQ(msgs[0].data_len, sizeof(body));
	IDPF_EXPECT_NOT_NULL(msgs[0].ctx.indirect.payload);

	if (msgs[0].ctx.indirect.payload != NULL)
		IDPF_EXPECT_EQ(memcmp(msgs[0].ctx.indirect.payload->va, body,
		    sizeof(body)), 0);

	IDPF_EXPECT_EQ(f.arq->next_to_clean, 1);
out:
	ctlq_fixture_teardown(&f);
}

static void
test_cp_multiple_replies_in_order(void)
{
	struct idpf_ctlq_msg msgs[4];
	struct ctlq_fixture f;
	uint16_t n = nitems(msgs);
	int i;

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	for (i = 0; i < 3; i++)
		cp_post_reply(&f, VIRTCHNL2_OP_GET_CAPS + i, i, NULL, 0);

	IDPF_EXPECT_OK(idpf_ctlq_recv(f.arq, &n, msgs));
	IDPF_EXPECT_EQ(n, 3);

	for (i = 0; i < (int)n && i < 3; i++) {
		IDPF_EXPECT_EQ(msgs[i].cookie.mbx.chnl_opcode,
		    VIRTCHNL2_OP_GET_CAPS + i);
		IDPF_EXPECT_EQ(msgs[i].cookie.mbx.chnl_retval, i);
	}
out:
	ctlq_fixture_teardown(&f);
}

static void
test_cp_error_flag_surfaces(void)
{
	struct idpf_ctlq_msg msgs[2];
	struct ctlq_fixture f;
	struct idpf_ctlq_desc *desc;
	uint16_t n = nitems(msgs);

	if (ctlq_fixture_setup(&f) != 0) {
		IDPF_EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	cp_post_reply(&f, VIRTCHNL2_OP_GET_CAPS, 0, NULL, 0);
	desc = IDPF_CTLQ_DESC(f.arq, 0);
	desc->flags |= htole16(IDPF_CTLQ_FLAG_ERR);

	IDPF_EXPECT_ERR(idpf_ctlq_recv(f.arq, &n, msgs), EBADMSG);
	IDPF_EXPECT_EQ(n, 1);
out:
	ctlq_fixture_teardown(&f);
}

static void
test_ctlq_setup_teardown_cycles(void)
{
	struct ctlq_fixture f;
	int i;

	for (i = 0; i < 3; i++) {
		if (ctlq_fixture_setup(&f) != 0) {
			IDPF_EXPECT(false, "fixture setup failed at cycle %d",
			    i);
			return;
		}
		if (f.asq == NULL || f.arq == NULL) {
			IDPF_EXPECT(false, "queues missing at cycle %d", i);
			ctlq_fixture_teardown(&f);
			return;
		}
		ctlq_fixture_teardown(&f);
	}
	IDPF_EXPECT_EQ(i, 3);
}

static const struct idpf_test_case ctlq_cases[] = {
	{ "ctlq_init_programs_registers", test_ctlq_init_programs_registers },
	{ "ctlq_deinit_clears_registers", test_ctlq_deinit_clears_registers },
	{ "ctlq_send_writes_descriptor", test_ctlq_send_writes_descriptor },
	{ "ctlq_send_rejects_missing_payload",
	  test_ctlq_send_rejects_missing_payload },
	{ "ctlq_send_full_ring", test_ctlq_send_full_ring },
	{ "ctlq_clean_sq_reclaims", test_ctlq_clean_sq_reclaims },
	{ "ctlq_recv_empty", test_ctlq_recv_empty },
	{ "cp_reply_is_received", test_cp_reply_is_received },
	{ "cp_multiple_replies_in_order", test_cp_multiple_replies_in_order },
	{ "cp_error_flag_surfaces", test_cp_error_flag_surfaces },
	{ "ctlq_setup_teardown_cycles", test_ctlq_setup_teardown_cycles },
};

const struct idpf_test_suite idpf_test_suite_ctlq =
    IDPF_TEST_SUITE("ctlq", ctlq_cases);
