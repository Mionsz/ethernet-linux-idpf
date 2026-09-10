/**
 * @file fake_idpf_ctlq_xn.cpp
 * @brief Link-time test double for idpf_ctlq_xn_send() (spec.md FR-011).
 */
#include <string.h>

#include "idpf_utest.h"

#include "../src/idpf_drv.h"

#include "fake_idpf_ctlq_xn.h"

#define FAKE_XN_MAX_RESP	8
#define FAKE_XN_MAX_OPLOG	16

static int		g_xn_rc;
static unsigned char	g_xn_recv[512];
static size_t		g_xn_recv_len;
static uint16_t		g_xn_last_opcode;
static int		g_xn_calls;

/* Per-opcode response table (overrides the single g_xn_recv payload). */
static struct {
	uint16_t	opcode;
	unsigned char	buf[512];
	size_t		len;
	int		set;
} g_xn_resp[FAKE_XN_MAX_RESP];

/* Ordered log of request opcodes seen since the last reset. */
static uint16_t		g_xn_oplog[FAKE_XN_MAX_OPLOG];

/* ControlQ init doubles. */
static int		g_ctlq_init_rc;
static int		g_ctlq_xn_init_rc;
static int		g_ctlq_init_calls;
static int		g_ctlq_xn_init_calls;

void
fake_xn_reset(void)
{
	g_xn_rc = 0;
	g_xn_recv_len = 0;
	g_xn_last_opcode = 0;
	g_xn_calls = 0;
	memset(g_xn_resp, 0, sizeof(g_xn_resp));
	memset(g_xn_oplog, 0, sizeof(g_xn_oplog));
	g_ctlq_init_rc = 0;
	g_ctlq_xn_init_rc = 0;
	g_ctlq_init_calls = 0;
	g_ctlq_xn_init_calls = 0;
}

void
fake_xn_set_rc(int rc)
{
	g_xn_rc = rc;
}

void
fake_xn_set_recv(const void *buf, size_t len)
{
	if (len > sizeof(g_xn_recv))
		len = sizeof(g_xn_recv);
	memcpy(g_xn_recv, buf, len);
	g_xn_recv_len = len;
}

void
fake_xn_set_recv_for(uint16_t opcode, const void *buf, size_t len)
{
	for (int i = 0; i < FAKE_XN_MAX_RESP; i++) {
		if (!g_xn_resp[i].set) {
			if (len > sizeof(g_xn_resp[i].buf))
				len = sizeof(g_xn_resp[i].buf);
			g_xn_resp[i].opcode = opcode;
			memcpy(g_xn_resp[i].buf, buf, len);
			g_xn_resp[i].len = len;
			g_xn_resp[i].set = 1;
			return;
		}
	}
}

uint16_t
fake_xn_last_opcode(void)
{
	return (g_xn_last_opcode);
}

uint16_t
fake_xn_opcode_at(int idx)
{
	if (idx < 0 || idx >= FAKE_XN_MAX_OPLOG || idx >= g_xn_calls)
		return (0);
	return (g_xn_oplog[idx]);
}

int
fake_xn_call_count(void)
{
	return (g_xn_calls);
}

void
fake_xn_set_ctlq_init_rc(int rc)
{
	g_ctlq_init_rc = rc;
}

void
fake_xn_set_ctlq_xn_init_rc(int rc)
{
	g_ctlq_xn_init_rc = rc;
}

int
fake_xn_ctlq_init_count(void)
{
	return (g_ctlq_init_calls);
}

int
fake_xn_ctlq_xn_init_count(void)
{
	return (g_ctlq_xn_init_calls);
}

/*
 * Test double: records the request opcode and, on a configured success,
 * copies the configured response bytes into the caller's recv_buf.
 */
int
idpf_ctlq_xn_send(struct idpf_ctlq_xn_send_params *params)
{
	uint16_t opcode = 0;

	if (params != NULL && params->ctlq_msg != NULL)
		opcode = params->ctlq_msg->opcode;
	g_xn_last_opcode = opcode;
	if (g_xn_calls < FAKE_XN_MAX_OPLOG)
		g_xn_oplog[g_xn_calls] = opcode;
	g_xn_calls++;

	if (g_xn_rc == 0 && params != NULL &&
	    params->recv_buf.iov_base != NULL) {
		const unsigned char *src = NULL;
		size_t srclen = 0;

		/* Prefer a per-opcode response; fall back to the single one. */
		for (int i = 0; i < FAKE_XN_MAX_RESP; i++) {
			if (g_xn_resp[i].set && g_xn_resp[i].opcode == opcode) {
				src = g_xn_resp[i].buf;
				srclen = g_xn_resp[i].len;
				break;
			}
		}
		if (src == NULL && g_xn_recv_len > 0) {
			src = g_xn_recv;
			srclen = g_xn_recv_len;
		}
		if (src != NULL && srclen > 0) {
			size_t n = srclen;

			if (n > params->recv_buf.iov_len)
				n = params->recv_buf.iov_len;
			memcpy(params->recv_buf.iov_base, src, n);
			params->recv_len = n;
		}
	}
	return (g_xn_rc);
}

/*
 * ControlQ init doubles (spec.md FR-011 boundary). The T019 helper calls
 * idpf_ctlq_init() then idpf_ctlq_xn_init(); tests exercise the attach path
 * without a real ControlQ/DMA. idpf_ctlq_xn_init() publishes the caller's
 * xnm OUT pointer so downstream code has a non-NULL manager.
 */
int
idpf_ctlq_init(struct idpf_hw *hw, u8 num_q, struct idpf_ctlq_create_info *q_info)
{
	(void)hw;
	(void)num_q;
	(void)q_info;
	g_ctlq_init_calls++;
	return (g_ctlq_init_rc);
}

int
idpf_ctlq_xn_init(struct idpf_ctlq_xn_init_params *params)
{
	static struct idpf_ctlq_xn_manager g_fake_xnm;

	g_ctlq_xn_init_calls++;
	if (params != NULL)
		params->xnm = &g_fake_xnm;
	return (g_ctlq_xn_init_rc);
}

int
idpf_ctlq_xn_deinit(struct idpf_ctlq_xn_init_params *params)
{
	(void)params;
	return (0);
}
