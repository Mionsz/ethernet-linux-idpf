/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Control queue tests with a simulated control plane.
 *
 * idpf_ctlq_send() publishes work by writing a tail register and
 * idpf_ctlq_recv() consumes it by polling a descriptor's DD flag, so a
 * "control plane" that reads a simulated register file and flips descriptor
 * bits is indistinguishable from hardware to the driver.  The driver sources
 * are compiled unmodified.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <machine/bus.h>

#include "idpf_controlq.h"

/* Defined in idpf.h, which is not pulled in here: it would drag the whole
 * driver in for two constants. */
#define IDPF_DFLT_MBX_ID	(-1)
#define IDPF_CTLQ_MAX_BUF_LEN	4096

/* ------------------------------------------------------------------ */
/* assertions                                                          */

static int checks;
static int failures;
static const char *current_case;

#define EXPECT(cond, ...) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		printf("  FAIL %s:%d: ", current_case, __LINE__);	\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

#define EXPECT_EQ(got, want) do {					\
	intmax_t g_ = (intmax_t)(got), w_ = (intmax_t)(want);		\
	EXPECT(g_ == w_, "%s: got %jd want %jd", #got, g_, w_);		\
} while (0)

#define EXPECT_OK(expr) do {						\
	int e_ = (expr);						\
	EXPECT(e_ == 0, "%s: expected 0, got %d", #expr, e_);		\
} while (0)

#define EXPECT_ERR(expr, want) do {					\
	int e_ = (expr);						\
	EXPECT(e_ == (want), "%s: got %d want %d", #expr, e_, (want));	\
} while (0)

#define EXPECT_NOT_NULL(p)	EXPECT((p) != NULL, "%s is NULL", #p)

/* ------------------------------------------------------------------ */
/* fixture                                                             */

#define REGFILE_SIZE	(64 * 1024)

#define ASQ_HEAD	0x1000
#define ASQ_TAIL	0x1004
#define ASQ_LEN		0x1008
#define ASQ_BAH		0x100C
#define ASQ_BAL		0x1010
#define ARQ_HEAD	0x2000
#define ARQ_TAIL	0x2004
#define ARQ_LEN		0x2008
#define ARQ_BAH		0x200C
#define ARQ_BAL		0x2010

#define LEN_MASK	0x1FFF
#define LEN_ENA_MASK	(1U << 31)
#define HEAD_MASK	0x1FFF

#define RING_LEN	8
#define BUF_LEN		IDPF_CTLQ_MAX_BUF_LEN

#define LO32(x)		((uint32_t)((x) & 0xffffffffULL))
#define HI32(x)		((uint32_t)(((uint64_t)(x)) >> 32))

struct fixture {
	struct idpf_hw		 hw;
	uint8_t			*regfile;
	struct idpf_ctlq_info	*asq;
	struct idpf_ctlq_info	*arq;
	uint16_t		 cp_next_reply;
};

static uint32_t
reg_get(struct fixture *f, uint32_t off)
{

	return (le32toh(*(volatile uint32_t *)(f->regfile + off)));
}

static void
fill_create_info(struct idpf_ctlq_create_info *info)
{

	memset(info, 0, 2 * sizeof(*info));

	info[0].type = IDPF_CTLQ_TYPE_MAILBOX_TX;
	info[0].id = IDPF_DFLT_MBX_ID;
	info[0].len = RING_LEN;
	info[0].buf_size = BUF_LEN;
	info[0].reg.head = ASQ_HEAD;
	info[0].reg.tail = ASQ_TAIL;
	info[0].reg.len = ASQ_LEN;
	info[0].reg.bah = ASQ_BAH;
	info[0].reg.bal = ASQ_BAL;
	info[0].reg.len_mask = LEN_MASK;
	info[0].reg.len_ena_mask = LEN_ENA_MASK;
	info[0].reg.head_mask = HEAD_MASK;

	info[1].type = IDPF_CTLQ_TYPE_MAILBOX_RX;
	info[1].id = IDPF_DFLT_MBX_ID;
	info[1].len = RING_LEN;
	info[1].buf_size = BUF_LEN;
	info[1].reg.head = ARQ_HEAD;
	info[1].reg.tail = ARQ_TAIL;
	info[1].reg.len = ARQ_LEN;
	info[1].reg.bah = ARQ_BAH;
	info[1].reg.bal = ARQ_BAL;
	info[1].reg.len_mask = LEN_MASK;
	info[1].reg.len_ena_mask = LEN_ENA_MASK;
	info[1].reg.head_mask = HEAD_MASK;
}

static struct idpf_ctlq_info *
ctlq_find(struct idpf_hw *hw, enum idpf_ctlq_type type, int id)
{
	struct idpf_ctlq_info *cq;

	TAILQ_FOREACH(cq, &hw->cq_list_head, cq_list) {
		if (cq->cq_type == type && cq->q_id == id)
			return (cq);
	}

	return (NULL);
}

static int
fixture_setup(struct fixture *f)
{
	struct idpf_ctlq_create_info info[2];
	int err;

	memset(f, 0, sizeof(*f));

	f->regfile = idpf_test_kmalloc(REGFILE_SIZE, M_ZERO);
	if (f->regfile == NULL)
		return (ENOMEM);

	/* MMIO is reached only through these pointers, so plain memory works. */
	f->hw.mbx.vaddr = f->regfile;
	f->hw.mbx.addr_start = 0;
	f->hw.mbx.addr_len = REGFILE_SIZE;

	fill_create_info(info);

	err = idpf_ctlq_init(&f->hw, 2, info);
	if (err != 0) {
		idpf_test_kfree(f->regfile);
		f->regfile = NULL;
		return (err);
	}

	f->asq = ctlq_find(&f->hw, IDPF_CTLQ_TYPE_MAILBOX_TX, IDPF_DFLT_MBX_ID);
	f->arq = ctlq_find(&f->hw, IDPF_CTLQ_TYPE_MAILBOX_RX, IDPF_DFLT_MBX_ID);

	return (0);
}

static void
fixture_teardown(struct fixture *f)
{

	idpf_ctlq_deinit(&f->hw);
	idpf_test_kfree(f->regfile);
	f->regfile = NULL;
}

/* ------------------------------------------------------------------ */
/* simulated control plane                                             */

static int
cp_complete_sends(struct fixture *f)
{
	struct idpf_ctlq_info *cq = f->asq;
	uint32_t tail = reg_get(f, cq->reg.tail);
	uint16_t i;
	int n = 0;

	for (i = 0; i != tail && n <= cq->ring_size;
	    i = (i + 1) % cq->ring_size) {
		struct idpf_ctlq_desc *desc = IDPF_CTLQ_DESC(cq, i);

		desc->flags |= htole16(IDPF_CTLQ_FLAG_DD |
		    IDPF_CTLQ_FLAG_CMP);
		n++;
	}

	return (n);
}

static void
cp_post_reply(struct fixture *f, uint32_t chnl_opcode, uint32_t chnl_retval,
    const void *payload, uint16_t len)
{
	struct idpf_ctlq_info *cq = f->arq;
	uint16_t idx = f->cp_next_reply;
	struct idpf_ctlq_desc *desc;

	desc = IDPF_CTLQ_DESC(cq, idx);
	memset(desc, 0, sizeof(*desc));

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
/* cases                                                               */

static void
test_init_programs_registers(void)
{
	struct fixture f;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}

	EXPECT_NOT_NULL(f.asq);
	EXPECT_NOT_NULL(f.arq);
	if (f.asq == NULL || f.arq == NULL)
		goto out;

	EXPECT_EQ(f.asq->ring_size, RING_LEN);
	EXPECT_EQ(reg_get(&f, ASQ_BAL), LO32(f.asq->desc_ring.pa));
	EXPECT_EQ(reg_get(&f, ASQ_BAH), HI32(f.asq->desc_ring.pa));
	EXPECT_EQ(reg_get(&f, ASQ_LEN), (RING_LEN | LEN_ENA_MASK));
	EXPECT_EQ(reg_get(&f, ARQ_BAL), LO32(f.arq->desc_ring.pa));
	EXPECT_EQ(reg_get(&f, ARQ_LEN), (RING_LEN | LEN_ENA_MASK));

	/* Receive buffers are posted during init, so the ring starts full. */
	EXPECT_EQ(reg_get(&f, ARQ_TAIL), RING_LEN - 1);
out:
	fixture_teardown(&f);
}

/*
 * idpf_ctlq_shutdown() only zeroes the queue registers on Simics; on silicon
 * they are deliberately left programmed and a function reset is relied on
 * instead.  Both sides are pinned so the asymmetry cannot change by accident.
 */
static void
test_deinit_clears_registers_on_simics(void)
{
	struct fixture f;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}

	f.hw.subsystem_device_id = IDPF_SUBDEV_ID_SIMICS;
	idpf_ctlq_deinit(&f.hw);

	EXPECT_EQ(reg_get(&f, ASQ_LEN), 0);
	EXPECT_EQ(reg_get(&f, ASQ_BAL), 0);
	EXPECT_EQ(reg_get(&f, ASQ_BAH), 0);
	EXPECT_EQ(reg_get(&f, ASQ_HEAD), 0);
	EXPECT_EQ(reg_get(&f, ARQ_LEN), 0);
	EXPECT_EQ(reg_get(&f, ARQ_BAL), 0);

	idpf_test_kfree(f.regfile);
}

static void
test_deinit_leaves_registers_on_silicon(void)
{
	struct fixture f;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}

	/* Neither Simics nor EMR, so silicon. */
	f.hw.subsystem_device_id = 0;
	idpf_ctlq_deinit(&f.hw);

	EXPECT_EQ(reg_get(&f, ASQ_LEN), (RING_LEN | LEN_ENA_MASK));
	EXPECT(reg_get(&f, ASQ_BAL) != 0, "ASQ base unexpectedly cleared");

	idpf_test_kfree(f.regfile);
}

static void
test_send_writes_descriptor(void)
{
	static const uint8_t ctx[IDPF_INDIRECT_CTX_SIZE] = {
		0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04
	};
	struct idpf_dma_mem payload;
	struct idpf_ctlq_desc *desc;
	struct idpf_ctlq_msg msg;
	struct fixture f;
	uint16_t flags;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	memset(&payload, 0, sizeof(payload));
	if (idpf_alloc_dma_mem(&f.hw, &payload, 256) == NULL) {
		EXPECT(false, "payload allocation failed");
		goto out;
	}

	memset(&msg, 0, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;
	msg.data_len = 64;
	msg.cookie.mbx.chnl_opcode = 0x1234;
	memcpy(msg.ctx.indirect.context, ctx, sizeof(ctx));
	msg.ctx.indirect.payload = &payload;

	EXPECT_OK(idpf_ctlq_send(&f.hw, f.asq, 1, &msg));

	/* The doorbell is all hardware would have observed. */
	EXPECT_EQ(reg_get(&f, ASQ_TAIL), 1);
	EXPECT_EQ(f.asq->next_to_use, 1);

	desc = IDPF_CTLQ_DESC(f.asq, 0);
	EXPECT_EQ(le16toh(desc->opcode), idpf_mbq_opc_send_msg_to_pf);
	EXPECT_EQ(le16toh(desc->datalen), 64);
	EXPECT_EQ(le32toh(desc->cookie_high), 0x1234);

	flags = le16toh(desc->flags);
	EXPECT((flags & IDPF_CTLQ_FLAG_BUF) != 0, "BUF flag not set");
	EXPECT((flags & IDPF_CTLQ_FLAG_RD) != 0, "RD flag not set");

	EXPECT_EQ(le32toh(desc->params.indirect.addr_low), LO32(payload.pa));
	EXPECT_EQ(le32toh(desc->params.indirect.addr_high), HI32(payload.pa));

	idpf_free_dma_mem(&f.hw, &payload);
out:
	fixture_teardown(&f);
}

static void
test_send_rejects_missing_payload(void)
{
	struct idpf_ctlq_msg msg;
	struct fixture f;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	memset(&msg, 0, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;
	msg.data_len = 64;
	msg.ctx.indirect.payload = NULL;

	EXPECT_ERR(idpf_ctlq_send(&f.hw, f.asq, 1, &msg), EBADMSG);
out:
	fixture_teardown(&f);
}

static void
test_send_full_ring(void)
{
	struct idpf_ctlq_msg msg;
	struct fixture f;
	int i, err = 0;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	memset(&msg, 0, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;

	/* One slot stays free so full is distinguishable from empty. */
	for (i = 0; i < RING_LEN - 1; i++) {
		err = idpf_ctlq_send(&f.hw, f.asq, 1, &msg);
		if (err != 0)
			break;
	}
	EXPECT_EQ(err, 0);
	EXPECT_EQ(i, RING_LEN - 1);

	EXPECT_ERR(idpf_ctlq_send(&f.hw, f.asq, 1, &msg), ENOSPC);
out:
	fixture_teardown(&f);
}

static void
test_clean_sq_reclaims(void)
{
	struct idpf_ctlq_msg *status[8];
	struct idpf_ctlq_msg msg;
	struct fixture f;
	uint16_t clean;
	int i;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.asq == NULL)
		goto out;

	memset(&msg, 0, sizeof(msg));
	msg.opcode = idpf_mbq_opc_send_msg_to_pf;

	for (i = 0; i < 4; i++)
		EXPECT_OK(idpf_ctlq_send(&f.hw, f.asq, 1, &msg));

	/* Nothing is reclaimable until the control plane completes them. */
	clean = nitems(status);
	idpf_ctlq_clean_sq(f.asq, &clean, status);
	EXPECT_EQ(clean, 0);

	EXPECT_EQ(cp_complete_sends(&f), 4);

	clean = nitems(status);
	EXPECT_OK(idpf_ctlq_clean_sq(f.asq, &clean, status));
	EXPECT_EQ(clean, 4);
	EXPECT_EQ(f.asq->next_to_clean, 4);
out:
	fixture_teardown(&f);
}

static void
test_recv_empty(void)
{
	struct idpf_ctlq_msg msgs[4];
	struct fixture f;
	uint16_t n = nitems(msgs);

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	EXPECT_ERR(idpf_ctlq_recv(f.arq, &n, msgs), ENOMSG);
	EXPECT_EQ(n, 0);
out:
	fixture_teardown(&f);
}

static void
test_cp_reply_is_received(void)
{
	static const uint8_t body[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	struct idpf_ctlq_msg msgs[4];
	struct fixture f;
	uint16_t n = nitems(msgs);

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	cp_post_reply(&f, 0x5678, 0, body, sizeof(body));

	EXPECT_OK(idpf_ctlq_recv(f.arq, &n, msgs));
	EXPECT_EQ(n, 1);
	if (n != 1)
		goto out;

	EXPECT_EQ(msgs[0].cookie.mbx.chnl_opcode, 0x5678);
	EXPECT_EQ(msgs[0].data_len, sizeof(body));
	EXPECT_NOT_NULL(msgs[0].ctx.indirect.payload);

	if (msgs[0].ctx.indirect.payload != NULL)
		EXPECT_EQ(memcmp(msgs[0].ctx.indirect.payload->va, body,
		    sizeof(body)), 0);

	EXPECT_EQ(f.arq->next_to_clean, 1);

	/* recv() hands the buffer to the caller and must not reuse it. */
	EXPECT(f.arq->bi.rx_buff[0] == NULL,
	    "receive buffer not detached after recv");
out:
	fixture_teardown(&f);
}

static void
test_cp_multiple_replies_in_order(void)
{
	struct idpf_ctlq_msg msgs[4];
	struct fixture f;
	uint16_t n = nitems(msgs);
	int i;

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	for (i = 0; i < 3; i++)
		cp_post_reply(&f, 0x100 + i, i, NULL, 0);

	EXPECT_OK(idpf_ctlq_recv(f.arq, &n, msgs));
	EXPECT_EQ(n, 3);

	for (i = 0; i < (int)n && i < 3; i++) {
		EXPECT_EQ(msgs[i].cookie.mbx.chnl_opcode, 0x100 + i);
		EXPECT_EQ(msgs[i].cookie.mbx.chnl_retval, i);
	}
out:
	fixture_teardown(&f);
}

static void
test_cp_error_flag_surfaces(void)
{
	struct idpf_ctlq_desc *desc;
	struct idpf_ctlq_msg msgs[2];
	struct fixture f;
	uint16_t n = nitems(msgs);

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	cp_post_reply(&f, 0x9999, 0, NULL, 0);
	desc = IDPF_CTLQ_DESC(f.arq, 0);
	desc->flags |= htole16(IDPF_CTLQ_FLAG_ERR);

	EXPECT_ERR(idpf_ctlq_recv(f.arq, &n, msgs), EBADMSG);
	EXPECT_EQ(n, 1);
out:
	fixture_teardown(&f);
}

static void
test_recv_stops_at_first_undelivered(void)
{
	struct idpf_ctlq_msg msgs[4];
	struct fixture f;
	uint16_t n = nitems(msgs);

	if (fixture_setup(&f) != 0) {
		EXPECT(false, "fixture setup failed");
		return;
	}
	if (f.arq == NULL)
		goto out;

	/* Leave a gap: only slot 0 is marked done, slot 1 is not. */
	cp_post_reply(&f, 0xAAAA, 0, NULL, 0);

	EXPECT_OK(idpf_ctlq_recv(f.arq, &n, msgs));
	EXPECT_EQ(n, 1);
	EXPECT_EQ(msgs[0].cookie.mbx.chnl_opcode, 0xAAAA);
out:
	fixture_teardown(&f);
}

static void
test_setup_teardown_leaks_nothing(void)
{
	long before, after;
	struct fixture f;
	int i;

	before = idpf_test_alloc_count();

	for (i = 0; i < 5; i++) {
		if (fixture_setup(&f) != 0) {
			EXPECT(false, "fixture setup failed at cycle %d", i);
			return;
		}
		fixture_teardown(&f);
	}

	after = idpf_test_alloc_count();
	EXPECT_EQ(after, before);
}

/* ------------------------------------------------------------------ */

struct test_case {
	const char	*name;
	void		(*fn)(void);
};

static const struct test_case cases[] = {
	{ "init_programs_registers", test_init_programs_registers },
	{ "deinit_clears_registers_on_simics",
	  test_deinit_clears_registers_on_simics },
	{ "deinit_leaves_registers_on_silicon",
	  test_deinit_leaves_registers_on_silicon },
	{ "send_writes_descriptor", test_send_writes_descriptor },
	{ "send_rejects_missing_payload", test_send_rejects_missing_payload },
	{ "send_full_ring", test_send_full_ring },
	{ "clean_sq_reclaims", test_clean_sq_reclaims },
	{ "recv_empty", test_recv_empty },
	{ "cp_reply_is_received", test_cp_reply_is_received },
	{ "cp_multiple_replies_in_order", test_cp_multiple_replies_in_order },
	{ "cp_error_flag_surfaces", test_cp_error_flag_surfaces },
	{ "recv_stops_at_first_undelivered",
	  test_recv_stops_at_first_undelivered },
	{ "setup_teardown_leaks_nothing", test_setup_teardown_leaks_nothing },
};

int
main(void)
{
	unsigned int i;

	printf("idpf ctlq tests (%zu cases)\n", nitems(cases));

	for (i = 0; i < nitems(cases); i++) {
		int before = failures;

		current_case = cases[i].name;
		cases[i].fn();
		printf("  %-34s %s\n", cases[i].name,
		    failures == before ? "ok" : "FAIL");
	}

	printf("\ncases=%zu checks=%d failures=%d\n", nitems(cases), checks,
	    failures);
	printf("RESULT %s\n", failures == 0 ? "PASS" : "FAIL");

	return (failures == 0 ? 0 : 1);
}
