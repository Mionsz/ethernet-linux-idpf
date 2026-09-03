/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * idpf_virtchnl.c - FreeBSD >= 15.0 IDPF VF-DPF control plane.
 *
 * OWNERSHIP MODEL
 *
 *   The control-queue transport itself (idpf_controlq.c, idpf_controlq_api.h)
 *   is carried source shared with other operating systems and is NOT ported
 *   here.  Everything below sits above it and speaks virtchnl2 1.0.
 *
 *   All descriptor-visible memory is obtained through the OS seam the carried
 *   source already defines - idpf_alloc_dma_mem() / idpf_free_dma_mem() - so
 *   that this file never touches busdma directly.  [FBSD15:A31]
 *
 * SYNCHRONISATION
 *
 *   Linux uses struct completion for the request/reply rendezvous.  FreeBSD's
 *   equivalent is a condvar plus its mutex plus an explicit predicate, so each
 *   transaction carries mtx/cv/done.  A useful side effect is that FreeBSD
 *   allows cv_broadcast() to be called with the mutex held, which removes the
 *   "we _cannot_ hold lock while calling complete" hazard of the Linux code.
 *   [FBSD15:A32-A33]
 *
 * LINK AND QUEUE STATE
 *
 *   There is no netdev.  Link transitions are published to the stack with
 *   iflib_link_state_change(), which is also what starts and stops the
 *   transmit path, so the netif_tx_{start,stop}_all_queues() pair has no
 *   direct counterpart.  [FBSD15:A30]
 *
 * MMIO REGIONS
 *
 *   Linux ioremaps each LAN register region separately.  FreeBSD's resource
 *   manager will not hand out the same PCI BAR twice, so the port maps BAR0
 *   once during attach - the resource lives in dev_ops.static_reg_info[0] -
 *   and every region records a virtual address computed as an offset into
 *   that single mapping.  idpf_lan_mmio_regs_rel() therefore only frees the
 *   bookkeeping array; the BAR is released by the PCI detach path.
 *   [FBSD15:A30-A31]
 *
 * REMOVED FROM THE PORT
 *
 *   PTP secondary-mailbox routing, OEM/P2P capabilities, ADI (VDCM/MDEV)
 *   messages, the IDC/RDMA synchronous send, uplink port statistics and the
 *   ethtool sideband flow-spec helpers.  Each depends on a Linux-only
 *   subsystem that the ported idpf.h no longer declares.
 *
 * ERROR CONVENTION
 *
 *   Every externally visible function returns a positive errno.  The single
 *   exception is idpf_vc_xn_exec(), which returns a reply size on success and
 *   a negated errno on failure because zero is a valid size.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bitstring.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/endian.h>
#include <sys/uio.h>

#include <machine/bus.h>

#include <dev/pci/pcivar.h>

#include "idpf.h"
#include "idpf_devids.h"
#include "idpf_virtchnl.h"
#include "idpf_ptp.h"

/**
 * idpf_vid_to_vport - translate a vport id to a vport pointer
 * @adapter: driver private data
 * @v_id: vport id to translate
 *
 * Return: the matching vport, or NULL when the id is unknown.
 */
static struct idpf_vport *
idpf_vid_to_vport(struct idpf_adapter *adapter, uint32_t v_id)
{
	uint16_t num_max_vports = idpf_get_max_vports(adapter);
	int i;

	for (i = 0; i < num_max_vports; i++)
		if (adapter->vport_ids[i] == v_id)
			return (adapter->vports[i]);

	return (NULL);
}

/**
 * idpf_handle_event_link - handle a link event message
 * @adapter: driver private data
 * @v2e: virtchnl event message
 *
 * Publishing the transition through iflib_link_state_change() is what gates
 * the transmit path, so it replaces the Linux carrier and queue calls.
 * [FBSD15:A30]
 *
 * Return: 0 on success, EINVAL when the vport id is unknown.
 */
static int
idpf_handle_event_link(struct idpf_adapter *adapter,
    const struct virtchnl2_event *v2e)
{
	struct idpf_netdev_priv *np;
	struct idpf_vport *vport;

	vport = idpf_vid_to_vport(adapter, le32toh(v2e->vport_id));
	if (vport == NULL) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "no vport for id %u in link event\n",
		    le32toh(v2e->vport_id));
		return (EINVAL);
	}

	np = iflib_get_softc(vport->ctx);
	np->link_speed_mbps = le32toh(v2e->link_speed);

	if (vport->link_up == (bool)v2e->link_status)
		return (0);

	vport->link_up = v2e->link_status;

	if ((np->state & (1u << IDPF_VPORT_UP)) == 0)
		return (0);

	iflib_link_state_change(vport->ctx,
	    vport->link_up ? LINK_STATE_UP : LINK_STATE_DOWN,
	    IF_Mbps(np->link_speed_mbps));

	return (0);
}

/**
 * idpf_recv_event_msg - dispatch an asynchronous virtchnl event
 * @adapter: driver private data
 * @ctlq_msg: message to inspect
 */
static void
idpf_recv_event_msg(struct idpf_adapter *adapter, struct idpf_ctlq_msg *ctlq_msg)
{
	struct virtchnl2_event *v2e;
	int payload_size;
	uint16_t adi_id;
	uint32_t event;

	payload_size = ctlq_msg->ctx.indirect.payload->size;
	if (payload_size < (int)sizeof(*v2e)) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "short payload for event msg (op %u len %d)\n",
		    ctlq_msg->cookie.mbx.chnl_opcode, payload_size);
		return;
	}

	v2e = (struct virtchnl2_event *)ctlq_msg->ctx.indirect.payload->va;
	event = le32toh(v2e->event);

	switch (event) {
	case VIRTCHNL2_EVENT_LINK_CHANGE:
		idpf_handle_event_link(adapter, v2e);
		break;
	case VIRTCHNL2_EVENT_START_RESET_ADI:
		adi_id = le16toh(v2e->adi_id);
		if (adapter->dev_ops.notify_adi_reset != NULL)
			adapter->dev_ops.notify_adi_reset(adapter, adi_id,
			    false);
		break;
	case VIRTCHNL2_EVENT_FINISH_RESET_ADI:
		adi_id = le16toh(v2e->adi_id);
		if (adapter->dev_ops.notify_adi_reset != NULL)
			adapter->dev_ops.notify_adi_reset(adapter, adi_id,
			    true);
		break;
	default:
		device_printf(idpf_adapter_to_dev(adapter),
		    "unknown event %u from PF\n", event);
		break;
	}
}

/**
 * idpf_mb_clean - reclaim completed send-mailbox entries
 * @adapter: driver private data
 * @asq: send control queue
 * @deinit: release every buffer, even the ones still outstanding
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_mb_clean(struct idpf_adapter *adapter, struct idpf_ctlq_info *asq,
    bool deinit)
{
	struct idpf_ctlq_msg **q_msg;
	struct idpf_dma_mem *dma_mem;
	uint16_t i, num_q_msg = IDPF_DFLT_MBX_Q_LEN;
	int err;

	q_msg = malloc(num_q_msg * sizeof(*q_msg), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (q_msg == NULL)
		return (ENOMEM);

	if (deinit)
		err = idpf_ctlq_clean_sq_force(asq, &num_q_msg, q_msg);
	else
		err = idpf_ctlq_clean_sq(asq, &num_q_msg, q_msg);
	if (err != 0)
		goto out;

	for (i = 0; i < num_q_msg; i++) {
		if (q_msg[i] == NULL)
			continue;
		dma_mem = q_msg[i]->ctx.indirect.payload;
		if (dma_mem != NULL) {
			idpf_free_dma_mem(&adapter->hw, dma_mem);
			free(dma_mem, M_DEVBUF);
		}
		free(q_msg[i], M_DEVBUF);
	}

out:
	free(q_msg, M_DEVBUF);

	return (err);
}

/**
 * idpf_send_mb_msg - hand a virtchnl message to the send mailbox
 * @adapter: driver private data
 * @asq: send control queue
 * @op: virtchnl opcode
 * @msg_size: payload size in bytes
 * @msg: payload, may be NULL for opcode-only messages
 * @cookie: transaction cookie echoed back in the reply
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_mb_msg(struct idpf_adapter *adapter, struct idpf_ctlq_info *asq,
    uint32_t op, uint16_t msg_size, uint8_t *msg, uint16_t cookie)
{
	struct idpf_ctlq_msg *ctlq_msg;
	struct idpf_dma_mem *dma_mem;
	int err;

	/*
	 * If a reset is already in flight there is nothing useful to do; the
	 * caller is expected to retry once the reset flow completes.
	 */
	if (idpf_is_reset_detected(adapter))
		return (0);

	err = idpf_mb_clean(adapter, asq, false);
	if (err != 0)
		return (err);

	ctlq_msg = malloc(sizeof(*ctlq_msg), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (ctlq_msg == NULL)
		return (ENOMEM);

	dma_mem = malloc(sizeof(*dma_mem), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (dma_mem == NULL) {
		err = ENOMEM;
		goto err_ctlq_msg;
	}

	ctlq_msg->func_id = 0;
	ctlq_msg->opcode = idpf_mbq_opc_send_msg_to_pf;
	ctlq_msg->data_len = msg_size;
	ctlq_msg->cookie.mbx.chnl_opcode = op;
	ctlq_msg->cookie.mbx.chnl_retval = VIRTCHNL2_STATUS_SUCCESS;

	if (idpf_alloc_dma_mem(&adapter->hw, dma_mem,
	    IDPF_CTLQ_MAX_BUF_LEN) == NULL) {
		err = ENOMEM;
		goto err_dma_mem;
	}

	/* An opcode with no payload is legitimate. */
	if (msg != NULL && msg_size != 0)
		memcpy(dma_mem->va, msg, msg_size);

	ctlq_msg->ctx.indirect.payload = dma_mem;
	ctlq_msg->ctx.sw_cookie.data = cookie;

	err = idpf_ctlq_send(&adapter->hw, asq, 1, ctlq_msg);
	if (err != 0)
		goto err_send;

	return (0);

err_send:
	idpf_free_dma_mem(&adapter->hw, dma_mem);
err_dma_mem:
	free(dma_mem, M_DEVBUF);
err_ctlq_msg:
	free(ctlq_msg, M_DEVBUF);

	return (err);
}

/* ---------------------------------------------------------------------
 * Virtchnl "transaction" (xn) layer
 * --------------------------------------------------------------------- */

/**
 * idpf_vc_xn_release_bufs - drop the reply reference and reset the state
 * @xn: transaction to update
 *
 * Caller must hold xn->lock.
 */
static void
idpf_vc_xn_release_bufs(struct idpf_vc_xn *xn)
{

	xn->reply.iov_base = NULL;
	xn->reply.iov_len = 0;

	if (xn->state != IDPF_VC_XN_SHUTDOWN)
		xn->state = IDPF_VC_XN_IDLE;
}

/**
 * idpf_init_vc_xn_completion - one-time construction of the sleep objects
 * @vcxn_mngr: transaction manager
 *
 * Split out from idpf_vc_xn_init() because mtx_init() and cv_init() must run
 * exactly once, while idpf_vc_xn_init() runs again after every reset.
 * [FBSD15:A32-A33]
 */
void
idpf_init_vc_xn_completion(struct idpf_vc_xn_manager *vcxn_mngr)
{
	struct idpf_vc_xn *xn;
	int i;

	mtx_init(&vcxn_mngr->xn_bm_lock, "idpf_vcxn_bm", NULL, MTX_DEF);

	for (i = 0; i < IDPF_VC_XN_RING_LEN; i++) {
		xn = &vcxn_mngr->ring[i];
		mtx_init(&xn->lock, "idpf_vcxn", NULL, MTX_DEF);
		cv_init(&xn->completed, "idpf_vcxn");
	}
}

/**
 * idpf_deinit_vc_xn_completion - tear the sleep objects back down
 * @vcxn_mngr: transaction manager
 *
 * No Linux counterpart: struct completion needs no destructor, FreeBSD mutexes
 * and condvars do.  [FBSD15:A32-A33]
 */
void
idpf_deinit_vc_xn_completion(struct idpf_vc_xn_manager *vcxn_mngr)
{
	struct idpf_vc_xn *xn;
	int i;

	for (i = 0; i < IDPF_VC_XN_RING_LEN; i++) {
		xn = &vcxn_mngr->ring[i];
		cv_destroy(&xn->completed);
		mtx_destroy(&xn->lock);
	}

	mtx_destroy(&vcxn_mngr->xn_bm_lock);
}

/**
 * idpf_vc_xn_init - make every transaction available again
 * @vcxn_mngr: transaction manager
 */
void
idpf_vc_xn_init(struct idpf_vc_xn_manager *vcxn_mngr)
{
	struct idpf_vc_xn *xn;
	int i;

	if (vcxn_mngr->active) {
		printf("idpf: attempt to init an already active vcxn_mngr\n");
		return;
	}

	for (i = 0; i < IDPF_VC_XN_RING_LEN; i++) {
		xn = &vcxn_mngr->ring[i];

		mtx_lock(&xn->lock);
		xn->state = IDPF_VC_XN_IDLE;
		xn->idx = (uint8_t)i;
		xn->done = false;
		idpf_vc_xn_release_bufs(xn);
		mtx_unlock(&xn->lock);
	}

	mtx_lock(&vcxn_mngr->xn_bm_lock);
	bit_nset(vcxn_mngr->free_xn_bm, 0, IDPF_VC_XN_RING_LEN - 1);
	vcxn_mngr->active = true;
	mtx_unlock(&vcxn_mngr->xn_bm_lock);
}

/**
 * idpf_vc_xn_shutdown - abort every transaction and refuse new ones
 * @vcxn_mngr: transaction manager
 *
 * Every waiter is woken and its transaction aborted.
 */
void
idpf_vc_xn_shutdown(struct idpf_vc_xn_manager *vcxn_mngr)
{
	struct idpf_vc_xn *xn;
	int i;

	if (!vcxn_mngr->active)
		return;

	mtx_lock(&vcxn_mngr->xn_bm_lock);
	bit_nclear(vcxn_mngr->free_xn_bm, 0, IDPF_VC_XN_RING_LEN - 1);
	vcxn_mngr->active = false;
	mtx_unlock(&vcxn_mngr->xn_bm_lock);

	for (i = 0; i < IDPF_VC_XN_RING_LEN; i++) {
		xn = &vcxn_mngr->ring[i];

		mtx_lock(&xn->lock);
		xn->state = IDPF_VC_XN_SHUTDOWN;
		idpf_vc_xn_release_bufs(xn);
		xn->done = true;
		cv_broadcast(&xn->completed);
		mtx_unlock(&xn->lock);
	}
}

/**
 * idpf_vc_xn_pop_free - take a transaction off the free list
 * @vcxn_mngr: transaction manager
 *
 * Return: an unowned transaction, or NULL when the ring is exhausted.
 */
static struct idpf_vc_xn *
idpf_vc_xn_pop_free(struct idpf_vc_xn_manager *vcxn_mngr)
{
	struct idpf_vc_xn *xn = NULL;
	int free_idx;

	mtx_lock(&vcxn_mngr->xn_bm_lock);
	bit_ffs(vcxn_mngr->free_xn_bm, IDPF_VC_XN_RING_LEN, &free_idx);
	if (free_idx < 0)
		goto out;

	bit_clear(vcxn_mngr->free_xn_bm, free_idx);
	xn = &vcxn_mngr->ring[free_idx];
	xn->salt = vcxn_mngr->salt++;

out:
	mtx_unlock(&vcxn_mngr->xn_bm_lock);

	return (xn);
}

/**
 * idpf_vc_xn_push_free - return a transaction to the free list
 * @vcxn_mngr: transaction manager
 * @xn: transaction to release
 *
 * Caller must hold xn->lock.
 */
static void
idpf_vc_xn_push_free(struct idpf_vc_xn_manager *vcxn_mngr,
    struct idpf_vc_xn *xn)
{

	idpf_vc_xn_release_bufs(xn);

	mtx_lock(&vcxn_mngr->xn_bm_lock);
	bit_set(vcxn_mngr->free_xn_bm, xn->idx);
	mtx_unlock(&vcxn_mngr->xn_bm_lock);
}

/**
 * idpf_vc_xn_exec - run one send/receive virtchnl transaction
 * @adapter: driver private data
 * @params: transaction description
 *
 * Return: on success the size of the reply as it arrived, which may be larger
 * than params->recv_buf.iov_len - the copy into the caller buffer is truncated
 * but the true size is still reported.  On failure a negated errno.
 */
ssize_t
idpf_vc_xn_exec(struct idpf_adapter *adapter,
    const struct idpf_vc_xn_params *params)
{
	const struct iovec *send_buf = &params->send_buf;
	struct idpf_vc_xn *xn;
	ssize_t retval;
	uint16_t cookie;
	int err, t0, timo;

	xn = idpf_vc_xn_pop_free(adapter->vcxn_mngr);
	if (xn == NULL)
		return (-ENOSPC);

	mtx_lock(&xn->lock);
	if (xn->state == IDPF_VC_XN_SHUTDOWN) {
		retval = -ENXIO;
		goto only_unlock;
	}
	if (xn->state != IDPF_VC_XN_IDLE) {
		/*
		 * Reuse it anyway.  Leaking the entry would eventually starve
		 * the ring and stop the driver from sending anything at all,
		 * which is strictly worse than clobbering one stale reply.
		 */
		device_printf(idpf_adapter_to_dev(adapter),
		    "non-idle transaction on free list (idx %u op %u)\n",
		    xn->idx, xn->vc_op);
	}

	xn->reply = params->recv_buf;
	xn->reply_sz = 0;
	xn->done = false;
	xn->state = params->async ? IDPF_VC_XN_ASYNC : IDPF_VC_XN_WAITING;
	xn->vc_op = params->vc_op;
	xn->async_handler = params->async_handler;
	cookie = IDPF_FIELD_PREP(IDPF_VC_XN_SALT_M, xn->salt) |
	    IDPF_FIELD_PREP(IDPF_VC_XN_IDX_M, xn->idx);
	mtx_unlock(&xn->lock);

	err = idpf_send_mb_msg(adapter, adapter->hw.asq, params->vc_op,
	    (uint16_t)send_buf->iov_len, (uint8_t *)send_buf->iov_base, cookie);
	if (err != 0) {
		retval = -err;
		mtx_lock(&xn->lock);
		goto release_and_unlock;
	}

	if (params->async)
		return (0);

	mtx_lock(&xn->lock);
	timo = idpf_msecs_to_ticks(params->timeout_ms);
	while (!xn->done && xn->state != IDPF_VC_XN_SHUTDOWN && timo > 0) {
		t0 = ticks;
		if (cv_timedwait(&xn->completed, &xn->lock, timo) ==
		    EWOULDBLOCK)
			break;
		timo -= ticks - t0;
	}

	switch (xn->state) {
	case IDPF_VC_XN_SHUTDOWN:
		retval = -ENXIO;
		goto only_unlock;
	case IDPF_VC_XN_WAITING:
		device_printf(idpf_adapter_to_dev(adapter),
		    "transaction timed out (op %u cookie %04x salt %02x "
		    "timeout %dms)\n", params->vc_op, cookie, xn->salt,
		    params->timeout_ms);
		retval = -ETIMEDOUT;
		break;
	case IDPF_VC_XN_COMPLETED_SUCCESS:
		retval = (ssize_t)xn->reply_sz;
		break;
	case IDPF_VC_XN_COMPLETED_FAILED:
		device_printf(idpf_adapter_to_dev(adapter),
		    "transaction failed (op %u)\n", params->vc_op);
		retval = -EIO;
		break;
	default:
		retval = -EIO;
		break;
	}

release_and_unlock:
	/* Any reply that arrives after this point is dropped. */
	idpf_vc_xn_push_free(adapter->vcxn_mngr, xn);
only_unlock:
	mtx_unlock(&xn->lock);

	return (retval);
}

/**
 * idpf_vc_xn_forward_async - consume a reply that nobody is waiting for
 * @adapter: driver private data
 * @xn: transaction the reply belongs to
 * @ctlq_msg: the reply
 *
 * Caller must hold xn->lock.
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_vc_xn_forward_async(struct idpf_adapter *adapter, struct idpf_vc_xn *xn,
    const struct idpf_ctlq_msg *ctlq_msg)
{
	int err = 0;

	if (ctlq_msg->cookie.mbx.chnl_opcode != xn->vc_op) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "async opcode mismatch (msg %u xn %u)\n",
		    ctlq_msg->cookie.mbx.chnl_opcode, xn->vc_op);
		xn->reply_sz = 0;
		err = EINVAL;
		goto release_bufs;
	}

	if (xn->async_handler != NULL) {
		err = xn->async_handler(adapter, xn, ctlq_msg);
		goto release_bufs;
	}

	if (ctlq_msg->cookie.mbx.chnl_retval != 0) {
		xn->reply_sz = 0;
		device_printf(idpf_adapter_to_dev(adapter),
		    "async message failed (op %u, control plane status %u)\n",
		    ctlq_msg->cookie.mbx.chnl_opcode,
		    ctlq_msg->cookie.mbx.chnl_retval);
		err = EINVAL;
	}

release_bufs:
	idpf_vc_xn_push_free(adapter->vcxn_mngr, xn);

	return (err);
}

/**
 * idpf_vc_xn_forward_reply - hand a reply back to the waiting thread
 * @adapter: driver private data
 * @ctlq_msg: reply taken off the receive mailbox
 *
 * Return: 0 on success, ENXIO once virtchnl has been shut down, otherwise an
 * errno.  The receive loop uses ENXIO to know it must stop touching registers.
 */
static int
idpf_vc_xn_forward_reply(struct idpf_adapter *adapter,
    const struct idpf_ctlq_msg *ctlq_msg)
{
	const void *payload = NULL;
	struct idpf_vc_xn *xn;
	size_t payload_size = 0;
	uint16_t msg_info, salt, xn_idx;
	int err = 0;

	msg_info = ctlq_msg->ctx.sw_cookie.data;
	xn_idx = IDPF_FIELD_GET(IDPF_VC_XN_IDX_M, msg_info);
	if (xn_idx >= IDPF_VC_XN_RING_LEN) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "out of bounds cookie received: %02x\n", xn_idx);
		return (EINVAL);
	}

	xn = &adapter->vcxn_mngr->ring[xn_idx];
	mtx_lock(&xn->lock);

	salt = IDPF_FIELD_GET(IDPF_VC_XN_SALT_M, msg_info);
	if (xn->salt != salt) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "transaction salt mismatch (exp %u@%02x(%d) got %u@%02x)\n",
		    xn->vc_op, xn->salt, xn->state,
		    ctlq_msg->cookie.mbx.chnl_opcode, salt);
		mtx_unlock(&xn->lock);
		return (EINVAL);
	}

	switch (xn->state) {
	case IDPF_VC_XN_WAITING:
		break;
	case IDPF_VC_XN_IDLE:
		device_printf(idpf_adapter_to_dev(adapter),
		    "unexpected or belated reply (op %u)\n",
		    ctlq_msg->cookie.mbx.chnl_opcode);
		err = EINVAL;
		goto out_unlock;
	case IDPF_VC_XN_SHUTDOWN:
		err = ENXIO;
		goto out_unlock;
	case IDPF_VC_XN_ASYNC:
		/* Record the size so the callback can evaluate the reply. */
		xn->reply_sz = ctlq_msg->data_len;
		err = idpf_vc_xn_forward_async(adapter, xn, ctlq_msg);
		mtx_unlock(&xn->lock);
		return (err);
	default:
		device_printf(idpf_adapter_to_dev(adapter),
		    "overwriting reply (op %u)\n",
		    ctlq_msg->cookie.mbx.chnl_opcode);
		err = EBUSY;
		goto out_unlock;
	}

	if (ctlq_msg->cookie.mbx.chnl_opcode != xn->vc_op) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "opcode mismatch (msg %u xn %u)\n",
		    ctlq_msg->cookie.mbx.chnl_opcode, xn->vc_op);
		xn->reply_sz = 0;
		xn->state = IDPF_VC_XN_COMPLETED_FAILED;
		err = EINVAL;
		goto out_unlock;
	}

	if (ctlq_msg->cookie.mbx.chnl_retval != 0) {
		/* The caller only sees EIO, so the reason is logged here. */
		device_printf(idpf_adapter_to_dev(adapter),
		    "control plane rejected op %u with status %u\n",
		    ctlq_msg->cookie.mbx.chnl_opcode,
		    ctlq_msg->cookie.mbx.chnl_retval);
		xn->reply_sz = 0;
		xn->state = IDPF_VC_XN_COMPLETED_FAILED;
		err = EINVAL;
		goto out_unlock;
	}

	if (ctlq_msg->data_len != 0) {
		payload = ctlq_msg->ctx.indirect.payload->va;
		payload_size = ctlq_msg->data_len;
	}

	xn->reply_sz = payload_size;
	xn->state = IDPF_VC_XN_COMPLETED_SUCCESS;

	if (xn->reply.iov_base != NULL && xn->reply.iov_len != 0 &&
	    payload_size != 0)
		memcpy(xn->reply.iov_base, payload,
		    min(xn->reply.iov_len, payload_size));

out_unlock:
	/*
	 * Unlike Linux, FreeBSD permits signalling with the mutex held, which
	 * closes the window where a waiter could miss the wakeup.
	 * [FBSD15:A33]
	 */
	xn->done = true;
	cv_broadcast(&xn->completed);
	mtx_unlock(&xn->lock);

	return (err);
}

/**
 * idpf_recv_mb_msg - drain the receive mailbox
 * @adapter: driver private data
 * @arq: receive control queue
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_recv_mb_msg(struct idpf_adapter *adapter, struct idpf_ctlq_info *arq)
{
	struct idpf_ctlq_msg ctlq_msg;
	struct idpf_dma_mem *dma_mem;
	uint16_t num_recv;
	int err = 0, post_err;

	while (err == 0) {
		/* Ask for one and learn how many actually came back. */
		num_recv = 1;
		err = idpf_ctlq_recv(arq, &num_recv, &ctlq_msg);
		if (err != 0 || num_recv == 0)
			break;

		if (ctlq_msg.data_len != 0) {
			dma_mem = ctlq_msg.ctx.indirect.payload;
		} else {
			dma_mem = NULL;
			num_recv = 0;
		}

		if (ctlq_msg.cookie.mbx.chnl_opcode == VIRTCHNL2_OP_EVENT)
			idpf_recv_event_msg(adapter, &ctlq_msg);
		else
			err = idpf_vc_xn_forward_reply(adapter, &ctlq_msg);

		post_err = idpf_ctlq_post_rx_buffs(&adapter->hw, arq, &num_recv,
		    &dma_mem);
		if (post_err != 0) {
			/* The buffer was not taken back, so release it here. */
			if (dma_mem != NULL)
				idpf_free_dma_mem(&adapter->hw, dma_mem);
			break;
		}
	}

	return (err);
}

/**
 * idpf_wait_for_marker_event - wait for the software marker completions
 * @vport: vport being quiesced
 *
 * Marking every transmit queue and waiting for the markers to come back is how
 * the driver knows the hardware has drained them.  [IDPF:A15]
 *
 * Return: 0 on success, ETIMEDOUT when the markers never arrived.
 */
static int
idpf_wait_for_marker_event(struct idpf_vport *vport)
{
	bool marked = false;
	int i, t0, timo;

	for (i = 0; i < vport->num_txq; i++)
		idpf_queue_set(SW_MARKER, vport->txqs[i]);

	/*
	 * iflib owns the poll loop, so nudge each transmit queue instead of
	 * scheduling NAPI by hand.  [FBSD15:A30]
	 */
	for (i = 0; i < vport->num_txq; i++)
		iflib_tx_intr_deferred(vport->ctx, i);

	mtx_lock(&vport->sw_marker_lock);
	timo = idpf_msecs_to_ticks(500);
	while ((vport->flags & (1u << IDPF_VPORT_SW_MARKER)) == 0 && timo > 0) {
		t0 = ticks;
		if (cv_timedwait(&vport->sw_marker_cv, &vport->sw_marker_lock,
		    timo) == EWOULDBLOCK)
			break;
		timo -= ticks - t0;
	}
	if ((vport->flags & (1u << IDPF_VPORT_SW_MARKER)) != 0) {
		vport->flags &= ~(1u << IDPF_VPORT_SW_MARKER);
		marked = true;
	}
	mtx_unlock(&vport->sw_marker_lock);

	for (i = 0; i < vport->num_txq; i++)
		idpf_queue_clear(POLL_MODE, vport->txqs[i]);

	if (marked)
		return (0);

	device_printf(idpf_adapter_to_dev(vport->adapter),
	    "failed to receive marker packets\n");

	return (ETIMEDOUT);
}

/**
 * idpf_send_ver_msg - negotiate the virtchnl version
 * @adapter: driver private data
 *
 * Return: 0 when the negotiated version is usable, EAGAIN when the peer
 * answered with a different version and the exchange must be repeated,
 * otherwise an errno.
 */
static int
idpf_send_ver_msg(struct idpf_adapter *adapter)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_version_info vvi;
	ssize_t reply_sz;
	uint32_t major, minor;
	int err = 0;

	if (adapter->virt_ver_maj != 0) {
		vvi.major = htole32(adapter->virt_ver_maj);
		vvi.minor = htole32(adapter->virt_ver_min);
	} else {
		vvi.major = htole32(IDPF_VIRTCHNL_VERSION_MAJOR);
		vvi.minor = htole32(IDPF_VIRTCHNL_VERSION_MINOR);
	}

	xn_params.vc_op = VIRTCHNL2_OP_VERSION;
	xn_params.send_buf.iov_base = &vvi;
	xn_params.send_buf.iov_len = sizeof(vvi);
	xn_params.recv_buf = xn_params.send_buf;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0)
		return (-reply_sz);
	if ((size_t)reply_sz < sizeof(vvi))
		return (EIO);

	major = le32toh(vvi.major);
	minor = le32toh(vvi.minor);

	if (major > IDPF_VIRTCHNL_VERSION_MAJOR) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "virtchnl major version greater than supported\n");
		return (EINVAL);
	}

	if (major == IDPF_VIRTCHNL_VERSION_MAJOR &&
	    minor > IDPF_VIRTCHNL_VERSION_MINOR)
		device_printf(idpf_adapter_to_dev(adapter),
		    "virtchnl minor version not matched\n");

	/* On a mismatch, resend so the peer learns the version we will use. */
	if (adapter->virt_ver_maj == 0 &&
	    major != IDPF_VIRTCHNL_VERSION_MAJOR &&
	    minor != IDPF_VIRTCHNL_VERSION_MINOR)
		err = EAGAIN;

	adapter->virt_ver_maj = major;
	adapter->virt_ver_min = minor;

	return (err);
}

/**
 * idpf_send_get_caps_msg - negotiate device capabilities
 * @adapter: driver private data
 *
 * The request advertises everything this driver can consume; the reply says
 * what the control plane actually grants.  [IDPF:A13-A14]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_get_caps_msg(struct idpf_adapter *adapter)
{
	struct virtchnl2_get_capabilities caps = { 0 };
	struct idpf_vc_xn_params xn_params = { 0 };
	ssize_t reply_sz;

	caps.csum_caps = htole32(
	    VIRTCHNL2_CAP_TX_CSUM_L3_IPV4          |
	    VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_TCP      |
	    VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_UDP      |
	    VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_SCTP     |
	    VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_TCP      |
	    VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_UDP      |
	    VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_SCTP     |
	    VIRTCHNL2_CAP_RX_CSUM_L3_IPV4          |
	    VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_TCP      |
	    VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_UDP      |
	    VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_SCTP     |
	    VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_TCP      |
	    VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_UDP      |
	    VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_SCTP     |
	    VIRTCHNL2_CAP_TX_CSUM_L3_SINGLE_TUNNEL |
	    VIRTCHNL2_CAP_RX_CSUM_L3_SINGLE_TUNNEL |
	    VIRTCHNL2_CAP_TX_CSUM_L4_SINGLE_TUNNEL |
	    VIRTCHNL2_CAP_RX_CSUM_L4_SINGLE_TUNNEL |
	    VIRTCHNL2_CAP_RX_CSUM_GENERIC);

	caps.seg_caps = htole32(
	    VIRTCHNL2_CAP_SEG_IPV4_TCP  |
	    VIRTCHNL2_CAP_SEG_IPV4_UDP  |
	    VIRTCHNL2_CAP_SEG_IPV4_SCTP |
	    VIRTCHNL2_CAP_SEG_IPV6_TCP  |
	    VIRTCHNL2_CAP_SEG_IPV6_UDP  |
	    VIRTCHNL2_CAP_SEG_IPV6_SCTP |
	    VIRTCHNL2_CAP_SEG_TX_SINGLE_TUNNEL);

	caps.rss_caps = htole64(IDPF_CAP_RSS);

	caps.hsplit_caps = htole32(IDPF_CAP_HSPLIT);

	caps.rsc_caps = htole32(IDPF_CAP_RSC);

	/*
	 * VIRTCHNL2_CAP_OEM is not requested: CONFIG_OEM_CAPS / CONFIG_P2P are
	 * Linux-only build options with no FreeBSD counterpart.
	 */
	caps.other_caps = htole64(
	    VIRTCHNL2_CAP_SRIOV               |
	    VIRTCHNL2_CAP_RDMA                |
	    VIRTCHNL2_CAP_LAN_MEMORY_REGIONS  |
	    VIRTCHNL2_CAP_MACFILTER           |
	    VIRTCHNL2_CAP_SPLITQ_QSCHED       |
	    VIRTCHNL2_CAP_PROMISC             |
	    VIRTCHNL2_CAP_EDT                 |
	    VIRTCHNL2_CAP_PTP                 |
	    VIRTCHNL2_CAP_TX_CMPL_TSTMP       |
	    VIRTCHNL2_CAP_MISS_COMPL_TAG      |
	    VIRTCHNL2_CAP_LOOPBACK            |
	    VIRTCHNL2_CAP_VLAN);

	xn_params.vc_op = VIRTCHNL2_OP_GET_CAPS;
	xn_params.send_buf.iov_base = &caps;
	xn_params.send_buf.iov_len = sizeof(caps);
	xn_params.recv_buf.iov_base = &adapter->caps;
	xn_params.recv_buf.iov_len = sizeof(adapter->caps);
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0)
		return (-reply_sz);
	if ((size_t)reply_sz < sizeof(adapter->caps))
		return (EIO);

	return (0);
}

/**
 * idpf_send_get_vlan_caps_msg - negotiate VLAN capabilities
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_get_vlan_caps_msg(struct idpf_adapter *adapter)
{
	struct virtchnl2_vlan_get_caps vlan_caps = { 0 };
	struct idpf_vc_xn_params xn_params = { 0 };
	ssize_t reply_sz;

	vlan_caps.ethertypes = htole32(VIRTCHNL2_VLAN_ETHERTYPE_8100);

	xn_params.vc_op = VIRTCHNL2_OP_GET_VLAN_CAPS;
	xn_params.send_buf.iov_base = &vlan_caps;
	xn_params.send_buf.iov_len = sizeof(vlan_caps);
	xn_params.recv_buf.iov_base = &adapter->vlan_caps;
	xn_params.recv_buf.iov_len = sizeof(adapter->vlan_caps);
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0)
		return (-reply_sz);
	if ((size_t)reply_sz < sizeof(adapter->vlan_caps))
		return (EIO);

	return (0);
}

/**
 * idpf_add_del_fsteer_filters - add or delete a flow steering rule
 * @adapter: driver private data
 * @rule: rule to add or delete
 * @opcode: VIRTCHNL2_OP_ADD_FLOW_RULE or VIRTCHNL2_OP_DEL_FLOW_RULE
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_add_del_fsteer_filters(struct idpf_adapter *adapter,
    struct virtchnl2_flow_rule_add_del *rule, enum virtchnl2_op opcode)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	uint32_t rule_count = le32toh(rule->count);
	ssize_t reply_sz;
	size_t buf_size;

	if (opcode != VIRTCHNL2_OP_ADD_FLOW_RULE &&
	    opcode != VIRTCHNL2_OP_DEL_FLOW_RULE)
		return (EINVAL);

	buf_size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_flow_rule_add_del,
	    rule_info, rule_count);

	xn_params.vc_op = opcode;
	xn_params.timeout_ms = IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC;
	xn_params.send_buf.iov_base = rule;
	xn_params.send_buf.iov_len = buf_size;
	xn_params.recv_buf.iov_base = rule;
	xn_params.recv_buf.iov_len = buf_size;

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);

	return (reply_sz < 0 ? -reply_sz : 0);
}

/* ---------------------------------------------------------------------
 * LAN MMIO regions
 * --------------------------------------------------------------------- */

/**
 * idpf_lan_mmio_regs_rel - drop the LAN MMIO region bookkeeping
 * @adapter: driver private data
 *
 * Nothing is unmapped here: BAR0 is a single resource owned by the attach
 * path, and every region is just an offset into it.  [FBSD15:A30-A31]
 */
static void
idpf_lan_mmio_regs_rel(struct idpf_adapter *adapter)
{
	struct idpf_hw *hw = &adapter->hw;

	free(hw->lan_regs, M_DEVBUF);
	hw->lan_regs = NULL;
	hw->num_lan_regs = 0;
}

/**
 * idpf_send_get_lan_memory_regions - ask the CP which BAR0 windows are ours
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_get_lan_memory_regions(struct idpf_adapter *adapter)
{
	struct virtchnl2_get_lan_memory_regions *rcvd_regions;
	struct idpf_vc_xn_params xn_params = { 0 };
	struct idpf_hw *hw = &adapter->hw;
	ssize_t reply_sz;
	size_t size;
	int err = 0, i, num_regions;

	rcvd_regions = malloc(IDPF_CTLQ_MAX_BUF_LEN, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (rcvd_regions == NULL)
		return (ENOMEM);

	rcvd_regions->num_memory_regions = htole16(1);

	xn_params.vc_op = VIRTCHNL2_OP_GET_LAN_MEMORY_REGIONS;
	xn_params.send_buf.iov_base = rcvd_regions;
	xn_params.send_buf.iov_len =
	    sizeof(struct virtchnl2_get_lan_memory_regions) +
	    sizeof(struct virtchnl2_mem_region);
	xn_params.recv_buf.iov_base = rcvd_regions;
	xn_params.recv_buf.iov_len = IDPF_CTLQ_MAX_BUF_LEN;
	xn_params.timeout_ms = IDPF_VC_XN_DEFAULT_TIMEOUT_MSEC;

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0) {
		err = -reply_sz;
		goto out;
	}

	num_regions = le16toh(rcvd_regions->num_memory_regions);
	size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_get_lan_memory_regions,
	    mem_reg, num_regions);
	if ((size_t)reply_sz < size) {
		err = EIO;
		goto out;
	}
	if (size > IDPF_CTLQ_MAX_BUF_LEN) {
		err = EINVAL;
		goto out;
	}

	idpf_lan_mmio_regs_rel(adapter);
	hw->lan_regs = malloc(num_regions * sizeof(*hw->lan_regs), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (hw->lan_regs == NULL) {
		err = ENOMEM;
		goto out;
	}

	for (i = 0; i < num_regions; i++) {
		hw->lan_regs[i].addr_len =
		    le64toh(rcvd_regions->mem_reg[i].size);
		hw->lan_regs[i].addr_start =
		    le64toh(rcvd_regions->mem_reg[i].start_offset);
	}
	hw->num_lan_regs = num_regions;

out:
	free(rcvd_regions, M_DEVBUF);

	return (err);
}

/**
 * idpf_calc_remaining_mmio_regs - derive the regions around mbx and rstat
 * @adapter: driver private data
 *
 * Fallback for control planes that do not implement GET_LAN_MEMORY_REGIONS.
 * The mailbox and rstat windows are already described by hw->mbx and
 * hw->rstat, so the remaining regions are the gaps before, between and after
 * them.  [IDPF:A13] [FBSD15:A31]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_calc_remaining_mmio_regs(struct idpf_adapter *adapter)
{
	struct idpf_hw *hw = &adapter->hw;
	struct idpf_mmio_reg *first = &hw->mbx;
	struct idpf_mmio_reg *second = &hw->rstat;
	bus_size_t bar0_len;

	bar0_len = rman_get_size(adapter->dev_ops.static_reg_info[0]);

	idpf_lan_mmio_regs_rel(adapter);
	hw->num_lan_regs = IDPF_MMIO_MAP_FALLBACK_MAX_REMAINING;
	hw->lan_regs = malloc(hw->num_lan_regs * sizeof(*hw->lan_regs),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (hw->lan_regs == NULL) {
		hw->num_lan_regs = 0;
		return (ENOMEM);
	}

	/* Swap in case rstat starts before the mailbox. */
	if (second->addr_start < first->addr_start) {
		first = &hw->rstat;
		second = &hw->mbx;
	}

	/* Region preceding the first window. */
	hw->lan_regs[0].addr_start = 0;
	hw->lan_regs[0].addr_len = first->addr_start;
	/* Region between the two windows. */
	hw->lan_regs[1].addr_start = first->addr_start + first->addr_len;
	hw->lan_regs[1].addr_len = second->addr_start -
	    hw->lan_regs[1].addr_start;
	/* Region after the second window. */
	hw->lan_regs[2].addr_start = second->addr_start + second->addr_len;
	hw->lan_regs[2].addr_len = bar0_len - hw->lan_regs[2].addr_start;

	return (0);
}

/**
 * idpf_map_lan_mmio_regs - resolve each LAN region to a virtual address
 * @adapter: driver private data
 *
 * BAR0 is mapped once during attach, so this only computes offsets into the
 * existing mapping - there is nothing to ioremap and nothing to undo.
 * [FBSD15:A30-A31]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_map_lan_mmio_regs(struct idpf_adapter *adapter)
{
	struct idpf_hw *hw = &adapter->hw;
	uint8_t *bar0_va;
	bus_size_t bar0_len;
	int i;

	bar0_va = rman_get_virtual(adapter->dev_ops.static_reg_info[0]);
	bar0_len = rman_get_size(adapter->dev_ops.static_reg_info[0]);

	for (i = 0; i < hw->num_lan_regs; i++) {
		if (hw->lan_regs[i].addr_len == 0)
			continue;

		if (hw->lan_regs[i].addr_start +
		    hw->lan_regs[i].addr_len > bar0_len) {
			device_printf(idpf_adapter_to_dev(adapter),
			    "LAN region %d [%#jx+%#jx] does not fit in BAR0\n",
			    i, (uintmax_t)hw->lan_regs[i].addr_start,
			    (uintmax_t)hw->lan_regs[i].addr_len);
			idpf_lan_mmio_regs_rel(adapter);
			return (EINVAL);
		}

		hw->lan_regs[i].vaddr = bar0_va + hw->lan_regs[i].addr_start;
	}

	return (0);
}

/* ---------------------------------------------------------------------
 * Queue budgeting
 * --------------------------------------------------------------------- */

/**
 * idpf_vport_alloc_max_qs - reserve the queue budget for one vport
 * @adapter: driver private data
 * @max_q: filled in with the granted maxima
 *
 * Return: 0 on success, EINVAL when the device cannot satisfy the request.
 */
int
idpf_vport_alloc_max_qs(struct idpf_adapter *adapter,
    struct idpf_vport_max_q *max_q)
{
	struct idpf_avail_queue_info *avail_queues = &adapter->avail_queues;
	struct virtchnl2_get_capabilities *caps = &adapter->caps;
	uint16_t default_vports = idpf_get_default_vports(adapter);
	uint16_t max_rx_q, max_tx_q, max_bufq, max_complq;

	if (default_vports == 0)
		return (EINVAL);

	sx_xlock(&adapter->queue_lock);

	max_rx_q = le16toh(caps->max_rx_q) / default_vports;
	max_tx_q = le16toh(caps->max_tx_q) / default_vports;
	max_bufq = le16toh(caps->max_rx_bufq) / default_vports;
	max_complq = le16toh(caps->max_tx_complq) / default_vports;

	if (adapter->num_alloc_vports < default_vports) {
		max_q->max_rxq = min(max_rx_q, (uint16_t)IDPF_MAX_RXQ);
		max_q->max_txq = min(max_tx_q, (uint16_t)IDPF_MAX_TXQ);
	} else {
		max_q->max_rxq = IDPF_MIN_Q;
		max_q->max_txq = IDPF_MIN_Q;
	}

	/*
	 * In the split model the RX and TX maxima are bounded by the buffer
	 * and completion queues.  Both are zero in the single model, which is
	 * why this is conditional.
	 */
	if (max_bufq != 0) {
		max_q->max_rxq = min(max_q->max_rxq,
		    (uint16_t)(max_bufq / IDPF_MAX_BUFQS_PER_RXQ_GRP));
		max_q->max_bufq = max_q->max_rxq * IDPF_MAX_BUFQS_PER_RXQ_GRP;
	}

	if (max_complq != 0) {
		max_q->max_txq = min(max_q->max_txq, max_complq);
		max_q->max_complq = max_q->max_txq;
	}

	if (avail_queues->avail_rxq < max_q->max_rxq ||
	    avail_queues->avail_txq < max_q->max_txq ||
	    avail_queues->avail_bufq < max_q->max_bufq ||
	    avail_queues->avail_complq < max_q->max_complq) {
		sx_xunlock(&adapter->queue_lock);
		return (EINVAL);
	}

	avail_queues->avail_rxq -= max_q->max_rxq;
	avail_queues->avail_txq -= max_q->max_txq;
	avail_queues->avail_bufq -= max_q->max_bufq;
	avail_queues->avail_complq -= max_q->max_complq;

	sx_xunlock(&adapter->queue_lock);

	return (0);
}

/**
 * idpf_vport_dealloc_max_qs - return a vport's queue budget to the pool
 * @adapter: driver private data
 * @max_q: budget being released
 */
void
idpf_vport_dealloc_max_qs(struct idpf_adapter *adapter,
    struct idpf_vport_max_q *max_q)
{
	struct idpf_avail_queue_info *avail_queues;

	sx_xlock(&adapter->queue_lock);
	avail_queues = &adapter->avail_queues;

	avail_queues->avail_rxq += max_q->max_rxq;
	avail_queues->avail_txq += max_q->max_txq;
	avail_queues->avail_bufq += max_q->max_bufq;
	avail_queues->avail_complq += max_q->max_complq;

	sx_xunlock(&adapter->queue_lock);
}

/**
 * idpf_init_avail_queues - seed the queue pool from the negotiated caps
 * @adapter: driver private data
 */
static void
idpf_init_avail_queues(struct idpf_adapter *adapter)
{
	struct idpf_avail_queue_info *avail_queues = &adapter->avail_queues;
	struct virtchnl2_get_capabilities *caps = &adapter->caps;

	avail_queues->avail_rxq = le16toh(caps->max_rx_q);
	avail_queues->avail_txq = le16toh(caps->max_tx_q);
	avail_queues->avail_bufq = le16toh(caps->max_rx_bufq);
	avail_queues->avail_complq = le16toh(caps->max_tx_complq);
}

/**
 * idpf_vport_init_queue_reg_chunks - convert wire chunks to the native form
 * @vport_config: vport configuration that stores the result
 * @schunks: source chunks from the control plane
 *
 * The caller releases the array with idpf_vport_deinit_queue_reg_chunks().
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_vport_init_queue_reg_chunks(struct idpf_vport_config *vport_config,
    struct virtchnl2_queue_reg_chunks *schunks)
{
	struct idpf_queue_id_reg_info *q_info = &vport_config->qid_reg_info;
	uint16_t i, num_chunks = le16toh(schunks->num_chunks);

	free(q_info->queue_chunks, M_DEVBUF);
	q_info->num_chunks = 0;

	q_info->queue_chunks = malloc(num_chunks * sizeof(*q_info->queue_chunks),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (q_info->queue_chunks == NULL)
		return (ENOMEM);

	q_info->num_chunks = num_chunks;

	for (i = 0; i < num_chunks; i++) {
		struct idpf_queue_id_reg_chunk *dchunk =
		    &q_info->queue_chunks[i];
		struct virtchnl2_queue_reg_chunk *schunk = &schunks->chunks[i];

		dchunk->qtail_reg_start = le64toh(schunk->qtail_reg_start);
		dchunk->qtail_reg_spacing =
		    le32toh(schunk->qtail_reg_spacing);
		dchunk->type = le32toh(schunk->type);
		dchunk->start_queue_id = le32toh(schunk->start_queue_id);
		dchunk->num_queues = le32toh(schunk->num_queues);
	}

	return (0);
}

/**
 * idpf_get_reg_intr_vecs - expand the vector chunks into per-vector registers
 * @adapter: driver private data
 * @reg_vals: array to populate
 *
 * Return: the number of entries written.
 */
int
idpf_get_reg_intr_vecs(struct idpf_adapter *adapter,
    struct idpf_vec_regs *reg_vals)
{
	struct virtchnl2_vector_chunks *chunks;
	struct idpf_vec_regs reg_val;
	uint16_t num_vchunks, num_vec;
	int num_regs = 0, i, j;

	chunks = &adapter->req_vec_chunks->vchunks;
	num_vchunks = le16toh(chunks->num_vchunks);

	for (j = 0; j < num_vchunks; j++) {
		struct virtchnl2_vector_chunk *chunk = &chunks->vchunks[j];
		uint32_t dynctl_reg_spacing, itrn_reg_spacing;

		num_vec = le16toh(chunk->num_vectors);
		reg_val.dyn_ctl_reg = le32toh(chunk->dynctl_reg_start);
		reg_val.itrn_reg = le32toh(chunk->itrn_reg_start);
		reg_val.itrn_index_spacing =
		    le32toh(chunk->itrn_index_spacing);

		dynctl_reg_spacing = le32toh(chunk->dynctl_reg_spacing);
		itrn_reg_spacing = le32toh(chunk->itrn_reg_spacing);

		for (i = 0; i < num_vec; i++) {
			reg_vals[num_regs].dyn_ctl_reg = reg_val.dyn_ctl_reg;
			reg_vals[num_regs].itrn_reg = reg_val.itrn_reg;
			reg_vals[num_regs].itrn_index_spacing =
			    reg_val.itrn_index_spacing;

			reg_val.dyn_ctl_reg += dynctl_reg_spacing;
			reg_val.itrn_reg += itrn_reg_spacing;
			num_regs++;
		}
	}

	return (num_regs);
}

/**
 * idpf_vport_get_q_reg - collect the tail register offsets of one queue type
 * @reg_vals: array to populate
 * @num_regs: capacity of @reg_vals
 * @q_type: queue type to select
 * @chunks: queue register chunks in native format
 *
 * Return: the number of entries written.
 */
static int
idpf_vport_get_q_reg(uint32_t *reg_vals, int num_regs, uint32_t q_type,
    struct idpf_queue_id_reg_info *chunks)
{
	uint16_t num_chunks = chunks->num_chunks;
	int reg_filled = 0, i;
	uint32_t reg_val;

	while (num_chunks-- > 0) {
		struct idpf_queue_id_reg_chunk *chunk;
		uint16_t num_q;

		chunk = &chunks->queue_chunks[num_chunks];
		if (chunk->type != q_type)
			continue;

		num_q = chunk->num_queues;
		reg_val = chunk->qtail_reg_start;
		for (i = 0; i < num_q && reg_filled < num_regs; i++) {
			reg_vals[reg_filled++] = reg_val;
			reg_val += chunk->qtail_reg_spacing;
		}
	}

	return (reg_filled);
}

/**
 * __idpf_queue_reg_init - bind tail doorbells to queues
 * @vport: vport being configured
 * @rsrc: queue and vector resources
 * @reg_vals: tail register offsets
 * @num_regs: number of valid entries in @reg_vals
 * @q_type: queue type the offsets belong to
 */
static void
__idpf_queue_reg_init(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc,
    uint32_t *reg_vals, int num_regs, uint32_t q_type)
{
	struct idpf_adapter *adapter = vport->adapter;
	struct idpf_queue *q;
	uint16_t i, j, k = 0;

	switch (q_type) {
	case VIRTCHNL2_QUEUE_TYPE_TX:
		for (i = 0; i < rsrc->num_txq_grp; i++) {
			struct idpf_txq_group *txq_grp = &rsrc->txq_grps[i];

			for (j = 0; j < txq_grp->num_txq && k < num_regs;
			    j++, k++)
				txq_grp->txqs[j]->tail =
				    idpf_get_reg_addr(adapter, reg_vals[k]);
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			uint16_t num_rxq = rx_qgrp->singleq.num_rxq;

			for (j = 0; j < num_rxq && k < num_regs; j++, k++) {
				q = rx_qgrp->singleq.rxqs[j];
				q->tail = idpf_get_reg_addr(adapter,
				    reg_vals[k]);
			}
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX_BUFFER:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			uint8_t num_bufqs = rsrc->num_bufqs_per_qgrp;

			for (j = 0; j < num_bufqs && k < num_regs; j++, k++) {
				q = &rx_qgrp->splitq.bufq_sets[j].bufq;
				q->tail = idpf_get_reg_addr(adapter,
				    reg_vals[k]);
			}
		}
		break;
	default:
		break;
	}
}

/**
 * idpf_queue_reg_init - resolve every queue's tail doorbell
 * @vport: vport being configured
 * @rsrc: queue and vector resources
 * @chunks: queue registers received over the mailbox
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_queue_reg_init(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc,
    struct idpf_queue_id_reg_info *chunks)
{
	uint32_t *reg_vals;
	int num_regs, err = 0;

	/* No queue type ever exceeds IDPF_LARGE_MAX_Q entries. */
	reg_vals = malloc(IDPF_LARGE_MAX_Q * sizeof(*reg_vals), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (reg_vals == NULL)
		return (ENOMEM);

	num_regs = idpf_vport_get_q_reg(reg_vals, IDPF_LARGE_MAX_Q,
	    VIRTCHNL2_QUEUE_TYPE_TX, chunks);
	if (num_regs < rsrc->num_txq) {
		err = EINVAL;
		goto out;
	}

	__idpf_queue_reg_init(vport, rsrc, reg_vals, num_regs,
	    VIRTCHNL2_QUEUE_TYPE_TX);

	if (idpf_is_queue_model_split(rsrc->rxq_model)) {
		num_regs = idpf_vport_get_q_reg(reg_vals, IDPF_LARGE_MAX_Q,
		    VIRTCHNL2_QUEUE_TYPE_RX_BUFFER, chunks);
		if (num_regs < rsrc->num_bufq) {
			err = EINVAL;
			goto out;
		}

		__idpf_queue_reg_init(vport, rsrc, reg_vals, num_regs,
		    VIRTCHNL2_QUEUE_TYPE_RX_BUFFER);
	} else {
		num_regs = idpf_vport_get_q_reg(reg_vals, IDPF_LARGE_MAX_Q,
		    VIRTCHNL2_QUEUE_TYPE_RX, chunks);
		if (num_regs < rsrc->num_rxq) {
			err = EINVAL;
			goto out;
		}

		__idpf_queue_reg_init(vport, rsrc, reg_vals, num_regs,
		    VIRTCHNL2_QUEUE_TYPE_RX);
	}

out:
	free(reg_vals, M_DEVBUF);

	return (err);
}

/**
 * idpf_send_get_edt_caps - negotiate earliest-departure-time capabilities
 * @adapter: driver private data
 *
 * On failure EDT is disabled locally so nothing later tries to use it.
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_get_edt_caps(struct idpf_adapter *adapter)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_edt_caps caps = { 0 };
	ssize_t reply_sz;
	int err;

	if (!idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_EDT))
		return (0);

	xn_params.vc_op = VIRTCHNL2_OP_GET_EDT_CAPS;
	xn_params.send_buf.iov_base = &caps;
	xn_params.send_buf.iov_len = sizeof(caps);
	xn_params.recv_buf.iov_base = &adapter->edt_caps;
	xn_params.recv_buf.iov_len = sizeof(adapter->edt_caps);
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0) {
		err = -reply_sz;
		goto err_out;
	}
	if ((size_t)reply_sz < sizeof(adapter->edt_caps)) {
		err = EIO;
		goto err_out;
	}

	return (0);

err_out:
	device_printf(idpf_adapter_to_dev(adapter),
	    "failed to get EDT capabilities, disabling EDT\n");
	adapter->edt_caps.tstamp_granularity_ns = 0;
	adapter->edt_caps.time_horizon_ns = 0;
	adapter->caps.other_caps &= htole64(~(uint64_t)VIRTCHNL2_CAP_EDT);

	return (err);
}

/* ---------------------------------------------------------------------
 * Vport lifecycle
 * --------------------------------------------------------------------- */

/**
 * idpf_send_create_vport_msg - ask the control plane to create a vport
 * @adapter: driver private data
 * @max_q: queue budget granted to this vport
 *
 * The reply size is not known in advance, so a full control-queue buffer is
 * allocated to receive it.  [IDPF:A13-A14]
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_create_vport_msg(struct idpf_adapter *adapter,
    struct idpf_vport_max_q *max_q)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_create_vport *vport_msg;
	uint16_t idx = adapter->next_vport;
	ssize_t reply_sz;
	int err;

	vport_msg = malloc(sizeof(*vport_msg), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (vport_msg == NULL)
		return (ENOMEM);

	vport_msg->vport_type = htole16(VIRTCHNL2_VPORT_TYPE_DEFAULT);
	vport_msg->vport_index = htole16(idx);
	vport_msg->txq_model = htole16(adapter->req_tx_splitq ?
	    VIRTCHNL2_QUEUE_MODEL_SPLIT : VIRTCHNL2_QUEUE_MODEL_SINGLE);
	vport_msg->rxq_model = htole16(adapter->req_rx_splitq ?
	    VIRTCHNL2_QUEUE_MODEL_SPLIT : VIRTCHNL2_QUEUE_MODEL_SINGLE);

	idpf_vport_calc_total_qs(adapter, idx, vport_msg, max_q);

	adapter->vport_params_recvd[idx] = malloc(IDPF_CTLQ_MAX_BUF_LEN,
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter->vport_params_recvd[idx] == NULL) {
		err = ENOMEM;
		goto out;
	}

	xn_params.vc_op = VIRTCHNL2_OP_CREATE_VPORT;
	xn_params.send_buf.iov_base = vport_msg;
	xn_params.send_buf.iov_len = sizeof(*vport_msg);
	xn_params.recv_buf.iov_base = adapter->vport_params_recvd[idx];
	xn_params.recv_buf.iov_len = IDPF_CTLQ_MAX_BUF_LEN;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0) {
		err = -reply_sz;
		free(adapter->vport_params_recvd[idx], M_DEVBUF);
		adapter->vport_params_recvd[idx] = NULL;
		goto out;
	}

	err = 0;

out:
	free(vport_msg, M_DEVBUF);

	return (err);
}

/**
 * idpf_check_supported_desc_ids - verify the granted descriptor profiles
 * @vport: vport being brought up
 *
 * When the control plane did not grant the profile the driver needs, the
 * request is rewritten to the minimum this driver can decode.  [IDPF:A13-A14]
 *
 * Return: 0.
 */
int
idpf_check_supported_desc_ids(struct idpf_vport *vport)
{
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	struct virtchnl2_create_vport *vport_msg;
	uint64_t rx_desc_ids, tx_desc_ids;

	vport_msg = adapter->vport_params_recvd[vport->idx];
	rx_desc_ids = le64toh(vport_msg->rx_desc_ids);
	tx_desc_ids = le64toh(vport_msg->tx_desc_ids);

	if (rsrc->rxq_model == VIRTCHNL2_QUEUE_MODEL_SPLIT) {
		if ((rx_desc_ids & VIRTCHNL2_RXDID_2_FLEX_SPLITQ_M) == 0) {
			device_printf(idpf_adapter_to_dev(adapter),
			    "minimum RX descriptor support not provided, "
			    "using the default\n");
			vport_msg->rx_desc_ids =
			    htole64(VIRTCHNL2_RXDID_2_FLEX_SPLITQ_M);
		}
	} else if ((rx_desc_ids & VIRTCHNL2_RXDID_2_FLEX_SQ_NIC_M) == 0) {
		rsrc->base_rxd = true;
	}

	if (rsrc->txq_model != VIRTCHNL2_QUEUE_MODEL_SPLIT)
		return (0);

	if ((tx_desc_ids & MIN_SUPPORT_TXDID) != MIN_SUPPORT_TXDID) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "minimum TX descriptor support not provided, "
		    "using the default\n");
		vport_msg->tx_desc_ids = htole64(MIN_SUPPORT_TXDID);
	}

	return (0);
}

/**
 * idpf_send_vport_op - send an opcode that carries only a vport id
 * @adapter: driver private data
 * @vport_id: vport the opcode applies to
 * @op: virtchnl opcode
 * @timeout_ms: reply timeout
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_vport_op(struct idpf_adapter *adapter, uint32_t vport_id,
    uint32_t op, int timeout_ms)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_vport v_id = { 0 };
	ssize_t reply_sz;

	v_id.vport_id = htole32(vport_id);

	xn_params.vc_op = op;
	xn_params.send_buf.iov_base = &v_id;
	xn_params.send_buf.iov_len = sizeof(v_id);
	xn_params.timeout_ms = timeout_ms;

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);

	return (reply_sz < 0 ? -reply_sz : 0);
}

/**
 * idpf_send_destroy_vport_msg - destroy a vport
 * @adapter: driver private data
 * @vport_id: vport to destroy
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_destroy_vport_msg(struct idpf_adapter *adapter, uint32_t vport_id)
{

	return (idpf_send_vport_op(adapter, vport_id,
	    VIRTCHNL2_OP_DESTROY_VPORT,
	    idpf_get_vc_xn_min_timeout(adapter)));
}

/**
 * idpf_send_enable_vport_msg - enable a vport
 * @adapter: driver private data
 * @vport_id: vport to enable
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_enable_vport_msg(struct idpf_adapter *adapter, uint32_t vport_id)
{

	return (idpf_send_vport_op(adapter, vport_id,
	    VIRTCHNL2_OP_ENABLE_VPORT,
	    idpf_get_vc_xn_default_timeout(adapter)));
}

/**
 * idpf_send_disable_vport_msg - disable a vport
 * @adapter: driver private data
 * @vport_id: vport to disable
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_disable_vport_msg(struct idpf_adapter *adapter, uint32_t vport_id)
{

	return (idpf_send_vport_op(adapter, vport_id,
	    VIRTCHNL2_OP_DISABLE_VPORT,
	    idpf_get_vc_xn_min_timeout(adapter)));
}

/* ---------------------------------------------------------------------
 * Queue configuration
 * --------------------------------------------------------------------- */

/**
 * __idpf_set_txq_info - fill the fields common to TX and completion queues
 * @q: queue to describe
 * @qi: wire structure to fill
 * @txq_model: TX queue model
 */
static void
__idpf_set_txq_info(struct idpf_queue *q, struct virtchnl2_txq_info *qi,
    uint16_t txq_model)
{
	bool is_splitq = idpf_is_queue_model_split(txq_model);

	qi->queue_id = htole32(q->q_id);
	qi->model = htole16(txq_model);
	qi->type = htole32(q->q_type);
	qi->ring_len = htole16(q->desc_count);
	qi->dma_ring_addr = htole64(q->dma);
	qi->sched_mode = htole16(idpf_queue_has(FLOW_SCH_EN, q) && is_splitq ?
	    VIRTCHNL2_TXQ_SCHED_MODE_FLOW : VIRTCHNL2_TXQ_SCHED_MODE_QUEUE);
}

/**
 * idpf_set_txq_info - describe one TX data queue
 * @txq: queue to describe
 * @qi: wire structure to fill
 * @txq_model: TX queue model
 */
static void
idpf_set_txq_info(struct idpf_queue *txq, struct virtchnl2_txq_info *qi,
    uint16_t txq_model)
{

	__idpf_set_txq_info(txq, qi, txq_model);

	if (!idpf_is_queue_model_split(txq_model))
		return;

	qi->tx_compl_queue_id = htole16(txq->txq_grp->complq->q_id);
	qi->relative_queue_id = htole16(txq->tx.rel_qid);
}

/**
 * idpf_send_config_tx_queues_msg - publish the TX queue configuration
 * @adapter: driver private data
 * @rsrc: queue and vector resources
 * @vport_id: vport the queues belong to
 *
 * The descriptions are split across as many mailbox messages as the control
 * queue buffer size requires.  [IDPF:A13-A14]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_config_tx_queues_msg(struct idpf_adapter *adapter,
    struct idpf_q_vec_rsrc *rsrc, uint32_t vport_id)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_config_tx_queues *ctq = NULL;
	struct virtchnl2_txq_info *qi;
	uint16_t txq_model = rsrc->txq_model;
	uint32_t config_sz, chunk_sz;
	size_t buf_sz;
	int totqs, num_msgs, num_chunks, sent = 0;
	unsigned int i, j, n = 0;
	ssize_t reply_sz;
	int err = 0;

	totqs = rsrc->num_txq + rsrc->num_complq;
	qi = malloc(totqs * sizeof(*qi), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (qi == NULL)
		return (ENOMEM);

	for (i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *txq_grp = &rsrc->txq_grps[i];

		for (j = 0; j < txq_grp->num_txq; j++, n++)
			idpf_set_txq_info(txq_grp->txqs[j], &qi[n], txq_model);

		if (!idpf_is_queue_model_split(txq_model))
			continue;

		__idpf_set_txq_info(txq_grp->complq, &qi[n], txq_model);
		/*
		 * Rule-miss completions are only produced under flow
		 * scheduling, which this driver does not negotiate.
		 */
		if (idpf_queue_has(FLOW_SCH_EN, txq_grp->txqs[0]))
			qi[n].qflags |=
			    htole16(VIRTCHNL2_TXQ_ENABLE_MISS_COMPL);
		n++;
	}

	config_sz = sizeof(struct virtchnl2_config_tx_queues);
	chunk_sz = sizeof(struct virtchnl2_txq_info);

	num_chunks = min((int)IDPF_NUM_CHUNKS_PER_MSG(config_sz, chunk_sz),
	    totqs);
	if (num_chunks <= 0) {
		err = EINVAL;
		goto out;
	}
	num_msgs = howmany(totqs, num_chunks);

	buf_sz = IDPF_STRUCT_VAR_LEN(struct virtchnl2_config_tx_queues, qinfo,
	    num_chunks);
	ctq = malloc(buf_sz, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (ctq == NULL) {
		err = ENOMEM;
		goto out;
	}

	xn_params.vc_op = VIRTCHNL2_OP_CONFIG_TX_QUEUES;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	for (i = 0; i < (unsigned int)num_msgs; i++) {
		memset(ctq, 0, buf_sz);
		ctq->vport_id = htole32(vport_id);
		ctq->num_qinfo = htole16(num_chunks);
		memcpy(ctq->qinfo, &qi[sent], chunk_sz * num_chunks);

		xn_params.send_buf.iov_base = ctq;
		xn_params.send_buf.iov_len = buf_sz;
		reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
		if (reply_sz < 0) {
			err = -reply_sz;
			goto out;
		}

		sent += num_chunks;
		totqs -= num_chunks;
		num_chunks = min(num_chunks, totqs);
		if (num_chunks == 0)
			break;
		buf_sz = IDPF_STRUCT_VAR_LEN(
		    struct virtchnl2_config_tx_queues, qinfo, num_chunks);
	}

out:
	free(ctq, M_DEVBUF);
	free(qi, M_DEVBUF);

	return (err);
}

/**
 * __idpf_set_rxq_info - fill the fields common to RX and buffer queues
 * @q: queue to describe
 * @qi: wire structure to fill
 * @rxq_model: RX queue model
 */
static void
__idpf_set_rxq_info(struct idpf_queue *q, struct virtchnl2_rxq_info *qi,
    uint16_t rxq_model)
{

	qi->queue_id = htole32(q->q_id);
	qi->model = htole16(rxq_model);
	qi->type = htole32(q->q_type);
	qi->ring_len = htole16(q->desc_count);
	qi->dma_ring_addr = htole64(q->dma);
	qi->max_pkt_size = htole32(q->rx_max_pkt_size);
	qi->data_buffer_size = htole32(q->rx_buf_size);

	if (q->rx_hsplit_en) {
		qi->qflags |= htole16(VIRTCHNL2_RXQ_HDR_SPLIT);
		qi->hdr_buffer_size = htole16(q->rx_hbuf_size);
	}

	if (!idpf_is_queue_model_split(rxq_model))
		return;

	qi->rx_buffer_low_watermark = htole16(q->rx_buffer_low_watermark);
	if (idpf_queue_has(RSC_EN, q))
		qi->qflags |= htole16(VIRTCHNL2_RXQ_RSC);
}

/**
 * idpf_set_rxq_info - describe one RX queue
 * @rxq: queue to describe
 * @qi: wire structure to fill
 * @bufq_per_rxq: buffer queues per RX queue group
 * @rxq_model: RX queue model
 */
static void
idpf_set_rxq_info(struct idpf_queue *rxq, struct virtchnl2_rxq_info *qi,
    uint16_t bufq_per_rxq, uint16_t rxq_model)
{
	struct idpf_bufq_set *bufq_sets;

	__idpf_set_rxq_info(rxq, qi, rxq_model);

	qi->qflags |= htole16(VIRTCHNL2_RX_DESC_SIZE_32BYTE);
	qi->desc_ids = htole64(rxq->rxdids);

	if (!idpf_is_queue_model_split(rxq_model))
		return;

	bufq_sets = rxq->rxq_grp->splitq.bufq_sets;
	qi->rx_bufq1_id = htole16(bufq_sets[0].bufq.q_id);
	if (bufq_per_rxq > IDPF_SINGLE_BUFQ_PER_RXQ_GRP) {
		qi->bufq2_ena = true;
		qi->rx_bufq2_id = htole16(bufq_sets[1].bufq.q_id);
	}
}

/**
 * idpf_set_bufq_info - describe one buffer queue
 * @bufq: queue to describe
 * @qi: wire structure to fill
 * @rxq_model: RX queue model
 */
static void
idpf_set_bufq_info(struct idpf_queue *bufq, struct virtchnl2_rxq_info *qi,
    uint16_t rxq_model)
{

	__idpf_set_rxq_info(bufq, qi, rxq_model);

	qi->desc_ids = htole64(VIRTCHNL2_RXDID_2_FLEX_SPLITQ_M);
	qi->buffer_notif_stride = bufq->rx_buf_stride;
}

/**
 * idpf_send_config_rx_queues_msg - publish the RX queue configuration
 * @adapter: driver private data
 * @rsrc: queue and vector resources
 * @vport_id: vport the queues belong to
 *
 * Buffer queues must precede RX queues in the message because the device
 * derives the buffer size from them.  [IDPF:A13-A14]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_config_rx_queues_msg(struct idpf_adapter *adapter,
    struct idpf_q_vec_rsrc *rsrc, uint32_t vport_id)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_config_rx_queues *crq = NULL;
	struct virtchnl2_rxq_info *qi;
	uint16_t rxq_model = rsrc->rxq_model;
	uint32_t config_sz, chunk_sz;
	bool is_splitq = idpf_is_queue_model_split(rxq_model);
	size_t buf_sz;
	int totqs, num_msgs, num_chunks, sent = 0;
	unsigned int i, j, n = 0;
	ssize_t reply_sz;
	int err = 0;

	totqs = rsrc->num_rxq + rsrc->num_bufq;
	qi = malloc(totqs * sizeof(*qi), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (qi == NULL)
		return (ENOMEM);

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
		uint16_t num_rxq;

		if (is_splitq) {
			for (j = 0; j < rsrc->num_bufqs_per_qgrp; j++, n++)
				idpf_set_bufq_info(
				    &rx_qgrp->splitq.bufq_sets[j].bufq,
				    &qi[n], rxq_model);
		}

		num_rxq = is_splitq ? rx_qgrp->splitq.num_rxq_sets :
		    rx_qgrp->singleq.num_rxq;

		for (j = 0; j < num_rxq; j++, n++) {
			struct idpf_queue *rxq;

			rxq = is_splitq ? &rx_qgrp->splitq.rxq_sets[j]->rxq :
			    rx_qgrp->singleq.rxqs[j];
			idpf_set_rxq_info(rxq, &qi[n],
			    rsrc->num_bufqs_per_qgrp, rxq_model);
		}
	}

	config_sz = sizeof(struct virtchnl2_config_rx_queues);
	chunk_sz = sizeof(struct virtchnl2_rxq_info);

	num_chunks = min((int)IDPF_NUM_CHUNKS_PER_MSG(config_sz, chunk_sz),
	    totqs);
	if (num_chunks <= 0) {
		err = EINVAL;
		goto out;
	}
	num_msgs = howmany(totqs, num_chunks);

	buf_sz = IDPF_STRUCT_VAR_LEN(struct virtchnl2_config_rx_queues, qinfo,
	    num_chunks);
	crq = malloc(buf_sz, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (crq == NULL) {
		err = ENOMEM;
		goto out;
	}

	xn_params.vc_op = VIRTCHNL2_OP_CONFIG_RX_QUEUES;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	for (i = 0; i < (unsigned int)num_msgs; i++) {
		memset(crq, 0, buf_sz);
		crq->vport_id = htole32(vport_id);
		crq->num_qinfo = htole16(num_chunks);
		memcpy(crq->qinfo, &qi[sent], chunk_sz * num_chunks);

		xn_params.send_buf.iov_base = crq;
		xn_params.send_buf.iov_len = buf_sz;
		reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
		if (reply_sz < 0) {
			err = -reply_sz;
			goto out;
		}

		sent += num_chunks;
		totqs -= num_chunks;
		num_chunks = min(num_chunks, totqs);
		if (num_chunks == 0)
			break;
		buf_sz = IDPF_STRUCT_VAR_LEN(
		    struct virtchnl2_config_rx_queues, qinfo, num_chunks);
	}

out:
	free(crq, M_DEVBUF);
	free(qi, M_DEVBUF);

	return (err);
}

/**
 * idpf_send_config_queues_msg - publish the whole queue configuration
 * @adapter: driver private data
 * @rsrc: queue and vector resources
 * @vport_id: vport the queues belong to
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_config_queues_msg(struct idpf_adapter *adapter,
    struct idpf_q_vec_rsrc *rsrc, uint32_t vport_id)
{
	int err;

	err = idpf_send_config_tx_queues_msg(adapter, rsrc, vport_id);
	if (err != 0)
		return (err);

	return (idpf_send_config_rx_queues_msg(adapter, rsrc, vport_id));
}

/**
 * idpf_convert_reg_to_queue_chunks - convert native chunks back to wire form
 * @dchunks: destination wire chunks
 * @schunks: source native chunks
 * @num_chunks: number of chunks to convert
 */
static void
idpf_convert_reg_to_queue_chunks(struct virtchnl2_queue_chunk *dchunks,
    struct idpf_queue_id_reg_chunk *schunks, uint16_t num_chunks)
{
	uint16_t i;

	for (i = 0; i < num_chunks; i++) {
		dchunks[i].type = htole32(schunks[i].type);
		dchunks[i].start_queue_id =
		    htole32(schunks[i].start_queue_id);
		dchunks[i].num_queues = htole32(schunks[i].num_queues);
	}
}

/**
 * idpf_send_queue_chunk_op - send an opcode that carries a queue chunk list
 * @adapter: driver private data
 * @chunks: queue chunks to describe
 * @vport_id: vport the queues belong to
 * @op: virtchnl opcode
 * @timeout_ms: reply timeout
 *
 * Shared by ENABLE_QUEUES, DISABLE_QUEUES and DEL_QUEUES, which all take the
 * same payload.  [IDPF:A13-A14]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_queue_chunk_op(struct idpf_adapter *adapter,
    struct idpf_queue_id_reg_info *chunks, uint32_t vport_id, uint32_t op,
    int timeout_ms)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_del_ena_dis_queues *eq;
	uint16_t num_chunks = chunks->num_chunks;
	ssize_t reply_sz;
	size_t buf_sz;

	buf_sz = IDPF_STRUCT_VAR_LEN(struct virtchnl2_del_ena_dis_queues,
	    chunks.chunks, num_chunks);

	eq = malloc(buf_sz, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (eq == NULL)
		return (ENOMEM);

	eq->vport_id = htole32(vport_id);
	eq->chunks.num_chunks = htole16(num_chunks);
	idpf_convert_reg_to_queue_chunks(eq->chunks.chunks,
	    chunks->queue_chunks, num_chunks);

	xn_params.vc_op = op;
	xn_params.timeout_ms = timeout_ms;
	xn_params.send_buf.iov_base = eq;
	xn_params.send_buf.iov_len = buf_sz;

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	free(eq, M_DEVBUF);

	return (reply_sz < 0 ? -reply_sz : 0);
}

/**
 * idpf_send_enable_queues_msg - enable the vport's queues
 * @adapter: driver private data
 * @vport_id: vport the queues belong to
 * @chunks: queue chunks to enable
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_enable_queues_msg(struct idpf_adapter *adapter, uint32_t vport_id,
    struct idpf_queue_id_reg_info *chunks)
{

	return (idpf_send_queue_chunk_op(adapter, chunks, vport_id,
	    VIRTCHNL2_OP_ENABLE_QUEUES,
	    idpf_get_vc_xn_default_timeout(adapter)));
}

/**
 * idpf_send_disable_queues_msg - disable the vport's queues and drain them
 * @adapter: driver private data
 * @vport: vport being quiesced
 * @rsrc: queue and vector resources
 * @chunks: queue chunks to disable
 *
 * Interrupts stop once the queues are disabled, so the transmit queues are
 * switched to poll mode before waiting for the software markers.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_disable_queues_msg(struct idpf_adapter *adapter,
    struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc,
    struct idpf_queue_id_reg_info *chunks)
{
	int err;
	unsigned int i, j;

	err = idpf_send_queue_chunk_op(adapter, chunks, vport->vport_id,
	    VIRTCHNL2_OP_DISABLE_QUEUES,
	    idpf_get_vc_xn_min_timeout(adapter));
	if (err != 0)
		return (err);

	for (i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *txq_grp = &rsrc->txq_grps[i];

		for (j = 0; j < txq_grp->num_txq; j++)
			idpf_queue_set(POLL_MODE, txq_grp->txqs[j]);
	}

	return (idpf_wait_for_marker_event(vport));
}

/**
 * idpf_send_delete_queues_msg - delete the vport's queues
 * @adapter: driver private data
 * @chunks: queue chunks to delete
 * @vport_id: vport the queues belong to
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_delete_queues_msg(struct idpf_adapter *adapter,
    struct idpf_queue_id_reg_info *chunks, uint32_t vport_id)
{

	return (idpf_send_queue_chunk_op(adapter, chunks, vport_id,
	    VIRTCHNL2_OP_DEL_QUEUES,
	    idpf_get_vc_xn_min_timeout(adapter)));
}

/**
 * idpf_send_map_unmap_queue_vector_msg - bind queues to interrupt vectors
 * @adapter: driver private data
 * @rsrc: queue and vector resources
 * @vport_id: vport the queues belong to
 * @map: true to map, false to unmap
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_map_unmap_queue_vector_msg(struct idpf_adapter *adapter,
    struct idpf_q_vec_rsrc *rsrc, uint32_t vport_id, bool map)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_queue_vector_maps *vqvm = NULL;
	struct virtchnl2_queue_vector *vqv;
	bool is_splitq = idpf_is_queue_model_split(rsrc->rxq_model);
	uint32_t config_sz, chunk_sz;
	size_t buf_sz;
	int num_q, num_msgs, num_chunks, sent = 0;
	unsigned int i, j, n = 0;
	ssize_t reply_sz;
	int err = 0;

	num_q = rsrc->num_txq + rsrc->num_rxq;
	vqv = malloc(num_q * sizeof(*vqv), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (vqv == NULL)
		return (ENOMEM);

	for (i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *txq_grp = &rsrc->txq_grps[i];

		for (j = 0; j < txq_grp->num_txq; j++, n++) {
			struct idpf_queue *txq = txq_grp->txqs[j];
			struct idpf_q_vector *vec;

			vec = idpf_is_queue_model_split(rsrc->txq_model) ?
			    txq->txq_grp->complq->q_vector : txq->q_vector;

			vqv[n].queue_type = htole32(txq->q_type);
			vqv[n].queue_id = htole32(txq->q_id);
			vqv[n].vector_id = htole16(vec->v_idx);
			vqv[n].itr_idx = htole32(vec->tx_itr_idx);
		}
	}

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
		uint16_t num_rxq;

		num_rxq = is_splitq ? rx_qgrp->splitq.num_rxq_sets :
		    rx_qgrp->singleq.num_rxq;

		for (j = 0; j < num_rxq; j++, n++) {
			struct idpf_queue *rxq;

			rxq = is_splitq ? &rx_qgrp->splitq.rxq_sets[j]->rxq :
			    rx_qgrp->singleq.rxqs[j];

			vqv[n].queue_type = htole32(rxq->q_type);
			vqv[n].queue_id = htole32(rxq->q_id);
			vqv[n].vector_id = htole16(rxq->q_vector->v_idx);
			vqv[n].itr_idx = htole32(rxq->q_vector->rx_itr_idx);
		}
	}

	config_sz = sizeof(struct virtchnl2_queue_vector_maps);
	chunk_sz = sizeof(struct virtchnl2_queue_vector);

	num_chunks = min((int)IDPF_NUM_CHUNKS_PER_MSG(config_sz, chunk_sz),
	    num_q);
	if (num_chunks <= 0) {
		err = EINVAL;
		goto out;
	}
	num_msgs = howmany(num_q, num_chunks);

	buf_sz = IDPF_STRUCT_VAR_LEN(struct virtchnl2_queue_vector_maps,
	    qv_maps, num_chunks);
	vqvm = malloc(buf_sz, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (vqvm == NULL) {
		err = ENOMEM;
		goto out;
	}

	if (map) {
		xn_params.vc_op = VIRTCHNL2_OP_MAP_QUEUE_VECTOR;
		xn_params.timeout_ms =
		    idpf_get_vc_xn_default_timeout(adapter);
	} else {
		xn_params.vc_op = VIRTCHNL2_OP_UNMAP_QUEUE_VECTOR;
		xn_params.timeout_ms = idpf_get_vc_xn_min_timeout(adapter);
	}

	for (i = 0; i < (unsigned int)num_msgs; i++) {
		memset(vqvm, 0, buf_sz);
		vqvm->vport_id = htole32(vport_id);
		vqvm->num_qv_maps = htole16(num_chunks);
		memcpy(vqvm->qv_maps, &vqv[sent], chunk_sz * num_chunks);

		xn_params.send_buf.iov_base = vqvm;
		xn_params.send_buf.iov_len = buf_sz;
		reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
		if (reply_sz < 0) {
			err = -reply_sz;
			goto out;
		}

		sent += num_chunks;
		num_q -= num_chunks;
		num_chunks = min(num_chunks, num_q);
		if (num_chunks == 0)
			break;
		buf_sz = IDPF_STRUCT_VAR_LEN(
		    struct virtchnl2_queue_vector_maps, qv_maps, num_chunks);
	}

out:
	free(vqvm, M_DEVBUF);
	free(vqv, M_DEVBUF);

	return (err);
}

/**
 * idpf_send_add_queues_msg - request additional queues for a vport
 * @adapter: driver private data
 * @vport_config: vport configuration updated with the new chunks
 * @rsrc: queue and vector resources describing the request
 * @vport_id: vport the queues belong to
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_add_queues_msg(struct idpf_adapter *adapter,
    struct idpf_vport_config *vport_config, struct idpf_q_vec_rsrc *rsrc,
    uint32_t vport_id)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_add_queues aq = { 0 };
	struct virtchnl2_add_queues *vc_msg;
	ssize_t reply_sz;
	size_t size;
	int err = 0;

	vc_msg = malloc(IDPF_CTLQ_MAX_BUF_LEN, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (vc_msg == NULL)
		return (ENOMEM);

	aq.vport_id = htole32(vport_id);
	aq.num_tx_q = htole16(rsrc->num_txq);
	aq.num_tx_complq = htole16(rsrc->num_complq);
	aq.num_rx_q = htole16(rsrc->num_rxq);
	aq.num_rx_bufq = htole16(rsrc->num_bufq);

	xn_params.vc_op = VIRTCHNL2_OP_ADD_QUEUES;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);
	xn_params.send_buf.iov_base = &aq;
	xn_params.send_buf.iov_len = sizeof(aq);
	xn_params.recv_buf.iov_base = vc_msg;
	xn_params.recv_buf.iov_len = IDPF_CTLQ_MAX_BUF_LEN;

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0) {
		err = -reply_sz;
		goto out;
	}

	if (le16toh(vc_msg->num_tx_q) != rsrc->num_txq ||
	    le16toh(vc_msg->num_rx_q) != rsrc->num_rxq ||
	    le16toh(vc_msg->num_tx_complq) != rsrc->num_complq ||
	    le16toh(vc_msg->num_rx_bufq) != rsrc->num_bufq) {
		err = EINVAL;
		goto out;
	}

	size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_add_queues, chunks.chunks,
	    le16toh(vc_msg->chunks.num_chunks));
	if ((size_t)reply_sz < size) {
		err = EIO;
		goto out;
	}

	err = idpf_vport_init_queue_reg_chunks(vport_config, &vc_msg->chunks);

out:
	free(vc_msg, M_DEVBUF);

	return (err);
}

/* ---------------------------------------------------------------------
 * Interrupt vectors
 * --------------------------------------------------------------------- */

/**
 * idpf_send_alloc_vectors_msg - request MSI-X vectors from the control plane
 * @adapter: driver private data
 * @num_vectors: number of vectors required
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_alloc_vectors_msg(struct idpf_adapter *adapter, uint16_t num_vectors)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_alloc_vectors ac = { 0 };
	struct virtchnl2_alloc_vectors *rcvd_vec;
	ssize_t reply_sz;
	size_t size;
	uint16_t num_vchunks;
	int err = 0;

	ac.num_vectors = htole16(num_vectors);

	rcvd_vec = malloc(IDPF_CTLQ_MAX_BUF_LEN, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rcvd_vec == NULL)
		return (ENOMEM);

	xn_params.vc_op = VIRTCHNL2_OP_ALLOC_VECTORS;
	xn_params.send_buf.iov_base = &ac;
	xn_params.send_buf.iov_len = sizeof(ac);
	xn_params.recv_buf.iov_base = rcvd_vec;
	xn_params.recv_buf.iov_len = IDPF_CTLQ_MAX_BUF_LEN;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0) {
		err = -reply_sz;
		goto out;
	}

	num_vchunks = le16toh(rcvd_vec->vchunks.num_vchunks);
	size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_alloc_vectors,
	    vchunks.vchunks, num_vchunks);
	if ((size_t)reply_sz < size) {
		err = EIO;
		goto out;
	}
	if (size > IDPF_CTLQ_MAX_BUF_LEN) {
		err = EINVAL;
		goto out;
	}

	free(adapter->req_vec_chunks, M_DEVBUF);
	adapter->req_vec_chunks = malloc(size, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter->req_vec_chunks == NULL) {
		err = ENOMEM;
		goto out;
	}
	memcpy(adapter->req_vec_chunks, rcvd_vec, size);

	if (le16toh(adapter->req_vec_chunks->num_vectors) < num_vectors) {
		free(adapter->req_vec_chunks, M_DEVBUF);
		adapter->req_vec_chunks = NULL;
		err = EINVAL;
	}

out:
	free(rcvd_vec, M_DEVBUF);

	return (err);
}

/**
 * idpf_send_dealloc_vectors_msg - return every allocated vector
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_dealloc_vectors_msg(struct idpf_adapter *adapter)
{
	struct virtchnl2_alloc_vectors *ac = adapter->req_vec_chunks;
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_vector_chunks *vcs;
	ssize_t reply_sz;
	size_t buf_size;

	if (ac == NULL)
		return (0);

	vcs = &ac->vchunks;
	buf_size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_vector_chunks, vchunks,
	    le16toh(vcs->num_vchunks));

	xn_params.vc_op = VIRTCHNL2_OP_DEALLOC_VECTORS;
	xn_params.send_buf.iov_base = vcs;
	xn_params.send_buf.iov_len = buf_size;
	xn_params.timeout_ms = idpf_get_vc_xn_min_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0)
		return (-reply_sz);

	free(adapter->req_vec_chunks, M_DEVBUF);
	adapter->req_vec_chunks = NULL;

	return (0);
}

/*
 * Linux publishes the device's VF budget with pci_sriov_set_totalvfs() during
 * core init.  FreeBSD instead advertises it through the SR-IOV configuration
 * schema built by pci_iov_attach(), so caps.max_sriov_vfs is read there rather
 * than here.  [FBSD15:A31]
 */

/**
 * idpf_send_set_sriov_vfs_msg - tell the control plane how many VFs to create
 * @adapter: driver private data
 * @num_vfs: number of virtual functions
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_set_sriov_vfs_msg(struct idpf_adapter *adapter, uint16_t num_vfs)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_sriov_vfs_info svi = { 0 };
	ssize_t reply_sz;

	svi.num_vfs = htole16(num_vfs);

	xn_params.vc_op = VIRTCHNL2_OP_SET_SRIOV_VFS;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);
	xn_params.send_buf.iov_base = &svi;
	xn_params.send_buf.iov_len = sizeof(svi);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);

	return (reply_sz < 0 ? -reply_sz : 0);
}

/* ---------------------------------------------------------------------
 * Statistics
 * --------------------------------------------------------------------- */

/**
 * idpf_send_get_stats_msg - fetch the vport counters from the control plane
 * @np: per-vport private data
 * @port_stats: destination for the raw wire counters
 *
 * The counters are mirrored into the interface's struct if_data so that
 * if_get_counter() can report them without another mailbox round trip.
 * [FBSD15:A30]
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_get_stats_msg(struct idpf_netdev_priv *np,
    struct idpf_port_stats *port_stats)
{
	struct virtchnl2_vport_stats stats_msg = { 0 };
	struct idpf_vc_xn_params xn_params = { 0 };
	struct if_data *netstats = &np->netstats;
	ssize_t reply_sz;

	if ((np->state & (1u << IDPF_VPORT_UP)) == 0)
		return (0);

	stats_msg.vport_id = htole32(np->vport_id);

	xn_params.vc_op = VIRTCHNL2_OP_GET_STATS;
	xn_params.send_buf.iov_base = &stats_msg;
	xn_params.send_buf.iov_len = sizeof(stats_msg);
	xn_params.recv_buf = xn_params.send_buf;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(np->adapter);

	reply_sz = idpf_vc_xn_exec(np->adapter, &xn_params);
	if (reply_sz < 0)
		return (-reply_sz);
	if ((size_t)reply_sz < sizeof(stats_msg))
		return (EIO);

	mtx_lock(&np->stats_lock);

	netstats->ifi_ipackets = le64toh(stats_msg.rx_unicast) +
	    le64toh(stats_msg.rx_multicast) +
	    le64toh(stats_msg.rx_broadcast);
	netstats->ifi_ibytes = le64toh(stats_msg.rx_bytes);
	netstats->ifi_iqdrops = le64toh(stats_msg.rx_discards);
	netstats->ifi_ierrors = le64toh(stats_msg.rx_overflow_drop) +
	    le64toh(stats_msg.rx_invalid_frame_length);

	netstats->ifi_opackets = le64toh(stats_msg.tx_unicast) +
	    le64toh(stats_msg.tx_multicast) +
	    le64toh(stats_msg.tx_broadcast);
	netstats->ifi_obytes = le64toh(stats_msg.tx_bytes);
	netstats->ifi_oerrors = le64toh(stats_msg.tx_errors);
	netstats->ifi_oqdrops = le64toh(stats_msg.tx_discards);

	mtx_unlock(&np->stats_lock);

	mtx_lock(&port_stats->stats_lock);
	port_stats->vport_stats = stats_msg;
	mtx_unlock(&port_stats->stats_lock);

	return (0);
}

/* ---------------------------------------------------------------------
 * Receive side scaling
 * --------------------------------------------------------------------- */

/**
 * idpf_send_get_set_rss_hash_msg - get or set the RSS hash configuration
 * @adapter: driver private data
 * @rss_data: vport RSS state
 * @vport_id: vport identifier
 * @get: true to read the current value, false to program rss_data
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_get_set_rss_hash_msg(struct idpf_adapter *adapter,
    struct idpf_rss_data *rss_data, uint32_t vport_id, bool get)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_rss_hash rh = { 0 };
	ssize_t reply_sz;

	rh.vport_id = htole32(vport_id);
	rh.ptype_groups = htole64(rss_data->rss_hash);

	xn_params.send_buf.iov_base = &rh;
	xn_params.send_buf.iov_len = sizeof(rh);

	if (get) {
		xn_params.vc_op = VIRTCHNL2_OP_GET_RSS_HASH;
		xn_params.recv_buf.iov_base = &rh;
		xn_params.recv_buf.iov_len = sizeof(rh);
	} else {
		xn_params.vc_op = VIRTCHNL2_OP_SET_RSS_HASH;
	}
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0)
		return (-reply_sz);
	if (!get)
		return (0);
	if ((size_t)reply_sz < sizeof(rh))
		return (EIO);

	rss_data->rss_hash = le64toh(rh.ptype_groups);

	return (0);
}

/**
 * idpf_send_get_set_rss_lut_msg - get or set the RSS redirection table
 * @adapter: driver private data
 * @rss_data: vport RSS state
 * @vport_id: vport identifier
 * @get: true to read the current table, false to program rss_data
 *
 * When RSS is not enabled on the interface the table is programmed with
 * zeroes so that every packet lands on queue 0.  [FBSD15:A30]
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_get_set_rss_lut_msg(struct idpf_adapter *adapter,
    struct idpf_rss_data *rss_data, uint32_t vport_id, bool get)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_rss_lut *recv_rl = NULL;
	struct virtchnl2_rss_lut *rl;
	struct idpf_vport *vport;
	uint32_t *lut;
	size_t buf_size;
	uint16_t lut_entries;
	ssize_t reply_sz;
	bool rxhash_ena;
	int err = 0;
	uint16_t i;

	vport = idpf_vid_to_vport(adapter, vport_id);
	if (vport == NULL)
		return (EINVAL);

	rxhash_ena = idpf_is_cap_ena_all(adapter, IDPF_RSS_CAPS, IDPF_CAP_RSS);

	buf_size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_rss_lut, lut,
	    rss_data->rss_lut_size);
	rl = malloc(buf_size, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rl == NULL)
		return (ENOMEM);

	rl->vport_id = htole32(vport_id);

	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);
	xn_params.send_buf.iov_base = rl;
	xn_params.send_buf.iov_len = buf_size;

	if (get) {
		recv_rl = malloc(IDPF_CTLQ_MAX_BUF_LEN, M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (recv_rl == NULL) {
			free(rl, M_DEVBUF);
			return (ENOMEM);
		}
		xn_params.vc_op = VIRTCHNL2_OP_GET_RSS_LUT;
		xn_params.recv_buf.iov_base = recv_rl;
		xn_params.recv_buf.iov_len = IDPF_CTLQ_MAX_BUF_LEN;
	} else {
		rl->lut_entries = htole16(rss_data->rss_lut_size);
		for (i = 0; i < rss_data->rss_lut_size; i++)
			rl->lut[i] = rxhash_ena ?
			    htole32(rss_data->rss_lut[i]) : 0;

		xn_params.vc_op = VIRTCHNL2_OP_SET_RSS_LUT;
	}

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	free(rl, M_DEVBUF);

	if (reply_sz < 0) {
		err = -reply_sz;
		goto out;
	}
	if (!get)
		goto out;
	if ((size_t)reply_sz < sizeof(struct virtchnl2_rss_lut)) {
		err = EIO;
		goto out;
	}

	lut_entries = le16toh(recv_rl->lut_entries);
	if ((size_t)reply_sz < IDPF_STRUCT_VAR_LEN(struct virtchnl2_rss_lut,
	    lut, lut_entries)) {
		err = EIO;
		goto out;
	}

	if (rss_data->rss_lut_size != lut_entries) {
		lut = malloc(lut_entries * sizeof(*lut), M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (lut == NULL) {
			free(rss_data->rss_lut, M_DEVBUF);
			rss_data->rss_lut = NULL;
			rss_data->rss_lut_size = 0;
			err = ENOMEM;
			goto out;
		}
		free(rss_data->rss_lut, M_DEVBUF);
		rss_data->rss_lut = lut;
		rss_data->rss_lut_size = lut_entries;
	}

	for (i = 0; i < rss_data->rss_lut_size; i++)
		rss_data->rss_lut[i] = le32toh(recv_rl->lut[i]);

out:
	free(recv_rl, M_DEVBUF);

	return (err);
}

/**
 * idpf_send_get_set_rss_key_msg - get or set the RSS hash key
 * @adapter: driver private data
 * @rss_data: vport RSS state
 * @vport_id: vport identifier
 * @get: true to read the current key, false to program rss_data
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_get_set_rss_key_msg(struct idpf_adapter *adapter,
    struct idpf_rss_data *rss_data, uint32_t vport_id, bool get)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_rss_key *recv_rk = NULL;
	struct virtchnl2_rss_key *rk;
	uint8_t *key;
	size_t buf_size;
	uint16_t key_size;
	ssize_t reply_sz;
	int err = 0;
	uint16_t i;

	buf_size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_rss_key, key,
	    rss_data->rss_key_size);
	rk = malloc(buf_size, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rk == NULL)
		return (ENOMEM);

	rk->vport_id = htole32(vport_id);

	xn_params.send_buf.iov_base = rk;
	xn_params.send_buf.iov_len = buf_size;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	if (get) {
		recv_rk = malloc(IDPF_CTLQ_MAX_BUF_LEN, M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (recv_rk == NULL) {
			free(rk, M_DEVBUF);
			return (ENOMEM);
		}
		xn_params.vc_op = VIRTCHNL2_OP_GET_RSS_KEY;
		xn_params.recv_buf.iov_base = recv_rk;
		xn_params.recv_buf.iov_len = IDPF_CTLQ_MAX_BUF_LEN;
	} else {
		rk->key_len = htole16(rss_data->rss_key_size);
		for (i = 0; i < rss_data->rss_key_size; i++)
			rk->key[i] = rss_data->rss_key[i];

		xn_params.vc_op = VIRTCHNL2_OP_SET_RSS_KEY;
	}

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	free(rk, M_DEVBUF);

	if (reply_sz < 0) {
		err = -reply_sz;
		goto out;
	}
	if (!get)
		goto out;
	if ((size_t)reply_sz < sizeof(struct virtchnl2_rss_key)) {
		err = EIO;
		goto out;
	}

	key_size = min((uint16_t)IDPF_MAX_RSS_KEY_LEN,
	    le16toh(recv_rk->key_len));
	if ((size_t)reply_sz < IDPF_STRUCT_VAR_LEN(struct virtchnl2_rss_key,
	    key, key_size)) {
		err = EIO;
		goto out;
	}

	if (rss_data->rss_key_size != key_size) {
		key = malloc(key_size, M_DEVBUF, M_NOWAIT | M_ZERO);
		if (key == NULL) {
			free(rss_data->rss_key, M_DEVBUF);
			rss_data->rss_key = NULL;
			rss_data->rss_key_size = 0;
			err = ENOMEM;
			goto out;
		}
		free(rss_data->rss_key, M_DEVBUF);
		rss_data->rss_key = key;
		rss_data->rss_key_size = key_size;
	}

	memcpy(rss_data->rss_key, recv_rk->key, rss_data->rss_key_size);

out:
	free(recv_rk, M_DEVBUF);

	return (err);
}

/* ---------------------------------------------------------------------
 * Packet type lookup tables
 * --------------------------------------------------------------------- */

/**
 * idpf_fill_ptype_lookup - record the L3 properties of a packet type
 * @ptype: decoded packet type being built
 * @pstate: parser state carried across protocol IDs
 * @ipv4: true for IPv4, false for IPv6
 * @frag: true when the header describes a fragment
 *
 * The first IP header seen describes the outer packet; a second one means the
 * packet is IP-in-IP tunnelled.
 */
static void
idpf_fill_ptype_lookup(struct idpf_rx_ptype_decoded *ptype,
    struct idpf_ptype_state *pstate, bool ipv4, bool frag)
{

	if (!pstate->outer_ip || !pstate->outer_frag) {
		ptype->outer_ip = IDPF_RX_PTYPE_OUTER_IP;
		pstate->outer_ip = true;

		if (ipv4)
			ptype->outer_ip_ver = IDPF_RX_PTYPE_OUTER_IPV4;
		else
			ptype->outer_ip_ver = IDPF_RX_PTYPE_OUTER_IPV6;

		if (frag) {
			ptype->outer_frag = IDPF_RX_PTYPE_FRAG;
			pstate->outer_frag = true;
		}

		return;
	}

	ptype->tunnel_type = IDPF_RX_PTYPE_TUNNEL_IP_IP;
	pstate->tunnel_state = IDPF_PTYPE_TUNNEL_IP;

	if (ipv4)
		ptype->tunnel_end_prot = IDPF_RX_PTYPE_TUNNEL_END_IPV4;
	else
		ptype->tunnel_end_prot = IDPF_RX_PTYPE_TUNNEL_END_IPV6;

	if (frag)
		ptype->tunnel_end_frag = IDPF_RX_PTYPE_FRAG;
}

/**
 * idpf_parse_protocol_ids - decode one packet type's protocol ID stack
 * @ptype: wire description of the packet type
 * @rx_pt: decoded packet type to fill
 *
 * Protocol IDs the datapath does not act on are ignored; the decoded entry
 * stays valid because idpf_rx_csum() only consults the fields set here.
 */
static void
idpf_parse_protocol_ids(struct virtchnl2_ptype *ptype,
    struct idpf_rx_ptype_decoded *rx_pt)
{
	struct idpf_ptype_state pstate = { 0 };
	int j;

	for (j = 0; j < ptype->proto_id_count; j++) {
		uint16_t id = le16toh(ptype->proto_id[j]);

		switch (id) {
		case VIRTCHNL2_PROTO_HDR_GRE:
			if (pstate.tunnel_state == IDPF_PTYPE_TUNNEL_IP) {
				rx_pt->tunnel_type =
				    IDPF_RX_PTYPE_TUNNEL_IP_GRENAT;
				pstate.tunnel_state |=
				    IDPF_PTYPE_TUNNEL_IP_GRENAT;
			}
			break;
		case VIRTCHNL2_PROTO_HDR_MAC:
			rx_pt->outer_ip = IDPF_RX_PTYPE_OUTER_L2;
			if (pstate.tunnel_state == IDPF_TUN_IP_GRE) {
				rx_pt->tunnel_type =
				    IDPF_RX_PTYPE_TUNNEL_IP_GRENAT_MAC;
				pstate.tunnel_state |=
				    IDPF_PTYPE_TUNNEL_IP_GRENAT_MAC;
			}
			break;
		case VIRTCHNL2_PROTO_HDR_IPV4:
			idpf_fill_ptype_lookup(rx_pt, &pstate, true, false);
			break;
		case VIRTCHNL2_PROTO_HDR_IPV6:
			idpf_fill_ptype_lookup(rx_pt, &pstate, false, false);
			break;
		case VIRTCHNL2_PROTO_HDR_IPV4_FRAG:
			idpf_fill_ptype_lookup(rx_pt, &pstate, true, true);
			break;
		case VIRTCHNL2_PROTO_HDR_IPV6_FRAG:
			idpf_fill_ptype_lookup(rx_pt, &pstate, false, true);
			break;
		case VIRTCHNL2_PROTO_HDR_UDP:
			rx_pt->inner_prot = IDPF_RX_PTYPE_INNER_PROT_UDP;
			break;
		case VIRTCHNL2_PROTO_HDR_TCP:
			rx_pt->inner_prot = IDPF_RX_PTYPE_INNER_PROT_TCP;
			break;
		case VIRTCHNL2_PROTO_HDR_SCTP:
			rx_pt->inner_prot = IDPF_RX_PTYPE_INNER_PROT_SCTP;
			break;
		case VIRTCHNL2_PROTO_HDR_ICMP:
			rx_pt->inner_prot = IDPF_RX_PTYPE_INNER_PROT_ICMP;
			break;
		case VIRTCHNL2_PROTO_HDR_PAY:
			rx_pt->payload_layer =
			    IDPF_RX_PTYPE_PAYLOAD_LAYER_PAY2;
			break;
		default:
			/* Not consumed by the FreeBSD datapath. */
			break;
		}
	}
}

/**
 * idpf_send_get_rx_ptype_msg - download the packet type lookup tables
 * @adapter: driver private data
 *
 * The device reports packet types in batches that must fit a single control
 * queue buffer, so the table is requested in a loop.  Two tables are built:
 * a 10-bit indexed one for the split queue model and an 8-bit indexed one for
 * the single queue model.  [IDPF:A13-A14]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_get_rx_ptype_msg(struct idpf_adapter *adapter)
{
	struct idpf_rx_ptype_decoded *singleq_pt_lkup, *splitq_pt_lkup;
	struct virtchnl2_get_ptype_info *get_ptype_info, *ptype_info;
	struct idpf_vc_xn_params xn_params = { 0 };
	uint32_t max_ptype = IDPF_RX_MAX_PTYPE;
	int ptypes_recvd = 0, ptype_offset;
	uint16_t next_ptype_id = 0;
	ssize_t reply_sz;
	int err = 0, i;

	singleq_pt_lkup = malloc(IDPF_RX_MAX_BASE_PTYPE *
	    sizeof(*singleq_pt_lkup), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (singleq_pt_lkup == NULL)
		return (ENOMEM);

	splitq_pt_lkup = malloc(max_ptype * sizeof(*splitq_pt_lkup), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (splitq_pt_lkup == NULL) {
		err = ENOMEM;
		goto free_singleq;
	}

	get_ptype_info = malloc(sizeof(*get_ptype_info), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (get_ptype_info == NULL) {
		err = ENOMEM;
		goto free_splitq;
	}

	ptype_info = malloc(IDPF_CTLQ_MAX_BUF_LEN, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (ptype_info == NULL) {
		err = ENOMEM;
		goto free_get_ptype;
	}

	xn_params.vc_op = VIRTCHNL2_OP_GET_PTYPE_INFO;
	xn_params.send_buf.iov_base = get_ptype_info;
	xn_params.send_buf.iov_len = sizeof(*get_ptype_info);
	xn_params.recv_buf.iov_base = ptype_info;
	xn_params.recv_buf.iov_len = IDPF_CTLQ_MAX_BUF_LEN;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	while (next_ptype_id < max_ptype) {
		get_ptype_info->start_ptype_id = htole16(next_ptype_id);

		if ((uint32_t)(next_ptype_id + IDPF_RX_MAX_PTYPES_PER_BUF) >
		    max_ptype)
			get_ptype_info->num_ptypes =
			    htole16(max_ptype - next_ptype_id);
		else
			get_ptype_info->num_ptypes =
			    htole16(IDPF_RX_MAX_PTYPES_PER_BUF);

		reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
		if (reply_sz < 0) {
			/* Preserve the transaction error rather than masking it. */
			err = -reply_sz;
			goto ptype_rel;
		}

		ptypes_recvd += le16toh(ptype_info->num_ptypes);
		if ((uint32_t)ptypes_recvd > max_ptype) {
			device_printf(idpf_adapter_to_dev(adapter),
			    "ptype: received %d ptypes, more than the %u the "
			    "driver supports (this reply had %u, reply_sz %zd)\n",
			    ptypes_recvd, max_ptype,
			    le16toh(ptype_info->num_ptypes), reply_sz);
			err = EINVAL;
			goto ptype_rel;
		}

		next_ptype_id = le16toh(get_ptype_info->start_ptype_id) +
		    le16toh(get_ptype_info->num_ptypes);
		ptype_offset = IDPF_RX_PTYPE_HDR_SZ;

		for (i = 0; i < le16toh(ptype_info->num_ptypes); i++) {
			struct idpf_rx_ptype_decoded rx_pt = { 0 };
			struct virtchnl2_ptype *ptype;
			uint16_t pt_10, pt_8;

			ptype = (struct virtchnl2_ptype *)
			    ((uint8_t *)ptype_info + ptype_offset);

			pt_10 = le16toh(ptype->ptype_id_10);
			pt_8 = ptype->ptype_id_8;

			ptype_offset += IDPF_GET_PTYPE_SIZE(ptype);
			if (ptype_offset > IDPF_CTLQ_MAX_BUF_LEN) {
				device_printf(idpf_adapter_to_dev(adapter),
				    "ptype: entry %d of %u ran off the %d byte "
				    "buffer (offset %d, proto_id_count %u)\n",
				    i, le16toh(ptype_info->num_ptypes),
				    IDPF_CTLQ_MAX_BUF_LEN, ptype_offset,
				    ptype->proto_id_count);
				err = EINVAL;
				goto ptype_rel;
			}

			/* 0xFFFF marks the end of the table. */
			if (pt_10 == IDPF_INVALID_PTYPE_ID)
				goto out;
			if (pt_10 >= max_ptype) {
				device_printf(idpf_adapter_to_dev(adapter),
				    "ptype: entry %d reports id %u, beyond the %u "
				    "the driver tracks\n", i, pt_10, max_ptype);
				err = EINVAL;
				goto ptype_rel;
			}

			if (ptype->proto_id_count != 0)
				rx_pt.known = 1;

			idpf_parse_protocol_ids(ptype, &rx_pt);

			/*
			 * The same protocol stack can carry a different id in
			 * the 10-bit and 8-bit spaces, so both tables are
			 * populated.  Duplicate 8-bit ids are ignored.
			 */
			splitq_pt_lkup[pt_10] = rx_pt;
			if (pt_8 < IDPF_RX_MAX_BASE_PTYPE &&
			    !singleq_pt_lkup[pt_8].known)
				singleq_pt_lkup[pt_8] = rx_pt;
		}
	}

out:
	adapter->splitq_pt_lkup = splitq_pt_lkup;
	adapter->singleq_pt_lkup = singleq_pt_lkup;

	free(ptype_info, M_DEVBUF);
	free(get_ptype_info, M_DEVBUF);

	return (0);

ptype_rel:
	free(ptype_info, M_DEVBUF);
free_get_ptype:
	free(get_ptype_info, M_DEVBUF);
free_splitq:
	free(splitq_pt_lkup, M_DEVBUF);
free_singleq:
	free(singleq_pt_lkup, M_DEVBUF);

	return (err);
}

/**
 * idpf_rel_rx_pt_lkup - release the packet type lookup tables
 * @adapter: driver private data
 */
static void
idpf_rel_rx_pt_lkup(struct idpf_adapter *adapter)
{

	free(adapter->splitq_pt_lkup, M_DEVBUF);
	adapter->splitq_pt_lkup = NULL;

	free(adapter->singleq_pt_lkup, M_DEVBUF);
	adapter->singleq_pt_lkup = NULL;
}

/**
 * idpf_send_ena_dis_loopback_msg - enable or disable vport loopback
 * @adapter: driver private data
 * @vport_id: vport identifier
 * @loopback_ena: true to enable loopback
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_send_ena_dis_loopback_msg(struct idpf_adapter *adapter, uint32_t vport_id,
    bool loopback_ena)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_loopback loopback = { 0 };
	ssize_t reply_sz;

	loopback.vport_id = htole32(vport_id);
	loopback.enable = loopback_ena;

	xn_params.vc_op = VIRTCHNL2_OP_LOOPBACK;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);
	xn_params.send_buf.iov_base = &loopback;
	xn_params.send_buf.iov_len = sizeof(loopback);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);

	return (reply_sz < 0 ? -reply_sz : 0);
}

/* ---------------------------------------------------------------------
 * Default mailbox
 * --------------------------------------------------------------------- */

/**
 * idpf_find_ctlq - locate a control queue by type and identifier
 * @hw: hardware struct
 * @type: control queue type to match
 * @id: control queue identifier to match
 *
 * Return: the matching control queue, or NULL.
 */
static struct idpf_ctlq_info *
idpf_find_ctlq(struct idpf_hw *hw, enum idpf_ctlq_type type, int id)
{
	struct idpf_ctlq_info *cq;

	TAILQ_FOREACH(cq, &hw->cq_list_head, cq_list) {
		if (cq->q_id == id && cq->cq_type == type)
			return (cq);
	}

	return (NULL);
}

/**
 * idpf_init_dflt_mbx - create the default mailbox and start polling it
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_init_dflt_mbx(struct idpf_adapter *adapter)
{
	struct idpf_ctlq_create_info ctlq_info[] = {
		{
			.type = IDPF_CTLQ_TYPE_MAILBOX_TX,
			.id = IDPF_DFLT_MBX_ID,
			.len = IDPF_DFLT_MBX_Q_LEN,
			.buf_size = IDPF_CTLQ_MAX_BUF_LEN
		},
		{
			.type = IDPF_CTLQ_TYPE_MAILBOX_RX,
			.id = IDPF_DFLT_MBX_ID,
			.len = IDPF_DFLT_MBX_Q_LEN,
			.buf_size = IDPF_CTLQ_MAX_BUF_LEN
		}
	};
	struct idpf_hw *hw = &adapter->hw;
	int err;

	adapter->dev_ops.reg_ops.ctlq_reg_init(adapter, ctlq_info);

	err = idpf_ctlq_init(hw, IDPF_NUM_DFLT_MBX_Q, ctlq_info);
	if (err != 0)
		return (err);

	hw->asq = idpf_find_ctlq(hw, IDPF_CTLQ_TYPE_MAILBOX_TX,
	    IDPF_DFLT_MBX_ID);
	hw->arq = idpf_find_ctlq(hw, IDPF_CTLQ_TYPE_MAILBOX_RX,
	    IDPF_DFLT_MBX_ID);

	if (hw->asq == NULL || hw->arq == NULL) {
		idpf_ctlq_deinit(hw);

		return (ENOENT);
	}

	adapter->state = __IDPF_VER_CHECK;
	taskqueue_enqueue(adapter->mbx_wq, &adapter->mbx_task);

	return (0);
}

/**
 * idpf_deinit_dflt_mbx - tear the default mailbox down
 * @adapter: driver private data
 *
 * The queue pointers are cleared before the mailbox task is drained so that
 * the task cannot re-arm itself while it is being stopped.
 */
void
idpf_deinit_dflt_mbx(struct idpf_adapter *adapter)
{
	struct idpf_ctlq_info *asq = adapter->hw.asq;
	struct idpf_ctlq_info *arq = adapter->hw.arq;

	idpf_mb_intr_rel_irq(adapter);

	adapter->hw.arq = NULL;
	adapter->hw.asq = NULL;
	taskqueue_drain(adapter->mbx_wq, &adapter->mbx_task);

	if (arq != NULL && asq != NULL) {
		idpf_mb_clean(adapter, asq, true);
		idpf_ctlq_deinit(&adapter->hw);
	}
}

/* ---------------------------------------------------------------------
 * Core bring-up and teardown
 * --------------------------------------------------------------------- */

/**
 * idpf_vport_params_buf_rel - release the vport parameter buffers
 * @adapter: driver private data
 */
static void
idpf_vport_params_buf_rel(struct idpf_adapter *adapter)
{

	free(adapter->vport_params_recvd, M_DEVBUF);
	adapter->vport_params_recvd = NULL;
	free(adapter->vport_ids, M_DEVBUF);
	adapter->vport_ids = NULL;
}

/**
 * idpf_vport_params_buf_alloc - allocate the vport parameter buffers
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_vport_params_buf_alloc(struct idpf_adapter *adapter)
{
	uint16_t num_max_vports = idpf_get_max_vports(adapter);

	adapter->vport_params_recvd = malloc(num_max_vports *
	    sizeof(*adapter->vport_params_recvd), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (adapter->vport_params_recvd == NULL)
		return (ENOMEM);

	adapter->vport_ids = malloc(num_max_vports *
	    sizeof(*adapter->vport_ids), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter->vport_ids == NULL)
		goto err_mem;

	/* Preserved across hard resets; only allocated on the first pass. */
	if (adapter->vport_config != NULL)
		return (0);

	adapter->vport_config = malloc(num_max_vports *
	    sizeof(*adapter->vport_config), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter->vport_config == NULL)
		goto err_mem;

	return (0);

err_mem:
	idpf_vport_params_buf_rel(adapter);

	return (ENOMEM);
}

/**
 * idpf_vc_core_init - negotiate with the control plane and claim resources
 * @adapter: driver private data
 *
 * Drives the version and capability handshake, maps the LAN register regions,
 * then allocates everything that is sized from the negotiated capabilities.
 *
 * Return: 0 on success, EAGAIN when the caller should retry after the queued
 * reset, otherwise an errno.
 */
int
idpf_vc_core_init(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	struct idpf_hw *hw = &adapter->hw;
	int task_delay = 30;
	uint16_t num_max_vports;
	int err = 0;

	if (IS_EMR_DEVICE(hw->subsystem_device_id))
		task_delay = 30000;

	while (adapter->state != __IDPF_INIT_SW) {
		switch (adapter->state) {
		case __IDPF_VER_CHECK:
			err = idpf_send_ver_msg(adapter);
			switch (err) {
			case 0:
				/* Success: move the state machine forward. */
				adapter->state = __IDPF_GET_CAPS;
				/* FALLTHROUGH */
			case EAGAIN:
				goto restart;
			default:
				goto init_failed;
			}
		case __IDPF_GET_CAPS:
			err = idpf_send_get_caps_msg(adapter);
			if (err != 0)
				goto init_failed;
			adapter->state = __IDPF_INIT_SW;
			break;
		default:
			device_printf(dev, "device is in bad state: %d\n",
			    adapter->state);
			err = EINVAL;
			goto init_failed;
		}
		break;
restart:
		/* Give the control plane time before trying again. */
		pause("idpfvc", idpf_msecs_to_ticks(task_delay));
	}

	if (hw->lan_regs == NULL) {
		if (idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS,
		    VIRTCHNL2_CAP_LAN_MEMORY_REGIONS)) {
			if (pci_get_device(dev) == IDPF_DEV_ID_VF_SIOV)
				err = idpf_calc_remaining_mmio_regs(adapter);
			else
				err = idpf_send_get_lan_memory_regions(adapter);
			if (err != 0) {
				device_printf(dev,
				    "failed to get LAN memory regions: %d\n",
				    err);
				return (EINVAL);
			}
		} else {
			/* Fall back to the remaining regions of the BAR. */
			err = idpf_calc_remaining_mmio_regs(adapter);
			if (err != 0) {
				device_printf(dev,
				    "failed to derive BAR0 region(s): %d\n",
				    err);
				return (ENOMEM);
			}
		}

		err = idpf_map_lan_mmio_regs(adapter);
		if (err != 0) {
			device_printf(dev,
			    "failed to map BAR0 region(s): %d\n", err);
			return (ENOMEM);
		}
	}

	if (adapter->dev_ops.reg_ops.oicr_reset_reg_init != NULL)
		adapter->dev_ops.reg_ops.oicr_reset_reg_init(adapter);

	num_max_vports = idpf_get_max_vports(adapter);
	adapter->vports = malloc(num_max_vports * sizeof(*adapter->vports),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter->vports == NULL)
		return (ENOMEM);

	if (adapter->iflib_ctxs == NULL) {
		adapter->iflib_ctxs = malloc(num_max_vports *
		    sizeof(*adapter->iflib_ctxs), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (adapter->iflib_ctxs == NULL) {
			err = ENOMEM;
			goto err_ctx_alloc;
		}
	}

	/* The interface being attached owns vport 0. */
	if (adapter->iflib_ctxs[0] == NULL)
		adapter->iflib_ctxs[0] = adapter->attach_ctx;

	err = idpf_vport_params_buf_alloc(adapter);
	if (err != 0) {
		device_printf(dev,
		    "failed to alloc vport params buffer: %d\n", err);
		goto err_ctx_alloc;
	}

	/*
	 * Publish max_vports only once every buffer indexed by it exists, so
	 * that error paths cannot leave a bounded loop walking off the end.
	 */
	adapter->max_vports = num_max_vports;

	idpf_send_get_edt_caps(adapter);

	err = idpf_intr_req(adapter);
	if (err != 0) {
		device_printf(dev,
		    "failed to enable interrupt vectors: %d\n", err);
		goto err_intr_req;
	}

	if (idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_VLAN)) {
		err = idpf_send_get_vlan_caps_msg(adapter);
		if (err != 0) {
			device_printf(dev,
			    "failed to get VLAN capabilities: %d\n", err);
			goto intr_rel;
		}
	}

	err = idpf_send_get_rx_ptype_msg(adapter);
	if (err != 0) {
		device_printf(dev, "failed to get RX ptypes: %d\n", err);
		goto intr_rel;
	}

	err = idpf_ptp_init(adapter);
	if (err != 0 && err != EOPNOTSUPP)
		device_printf(dev, "PTP init failed: %d\n", err);
	else if (err == 0 && idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS,
	    VIRTCHNL2_CAP_TX_CMPL_TSTMP))
		adapter->tx_compl_tstamp_gran_s =
		    adapter->caps.tx_cmpl_tstamp_ns_s;

	idpf_init_avail_queues(adapter);

	/*
	 * Skew the per-function init delay so that the functions of a
	 * multi-function device do not all call the control plane at once.
	 */
	taskqueue_enqueue_timeout(adapter->init_wq, &adapter->init_task,
	    idpf_msecs_to_ticks(5 * (pci_get_function(dev) & 0x07)));
	adapter->mb_wait_count = 0;

	adapter->flags |= (1u << IDPF_VC_CORE_INIT);

	return (0);

intr_rel:
	idpf_intr_rel(adapter);
err_intr_req:
	idpf_vport_params_buf_rel(adapter);
err_ctx_alloc:
	/*
	 * iflib_ctxs is deliberately not freed here: the contexts must survive
	 * hard resets and are torn down once, on detach.
	 */
	free(adapter->vports, M_DEVBUF);
	adapter->vports = NULL;

	return (err);

init_failed:
	/* Do not retry while the driver is going away. */
	if ((adapter->flags & (1u << IDPF_REMOVE_IN_PROG)) != 0)
		return (err);

	if (++adapter->mb_wait_count > IDPF_MB_MAX_ERR) {
		device_printf(dev,
		    "failed to establish mailbox communications with hardware\n");
		return (EFAULT);
	}

	device_printf(dev,
	    "failed to initialize virtchnl, wait_count: %d state: %d err: %d, "
	    "triggering reset\n", adapter->mb_wait_count, adapter->state, err);

	/*
	 * The mailbox register writes may not have taken effect; drop the
	 * transaction manager and drive a reset so the mailbox is rebuilt.
	 */
	adapter->state = __IDPF_VER_CHECK;
	if (adapter->vcxn_mngr != NULL)
		idpf_vc_xn_shutdown(adapter->vcxn_mngr);
	adapter->flags |= (1u << IDPF_HR_DRV_LOAD);
	taskqueue_enqueue_timeout(adapter->vc_event_wq, &adapter->vc_event_task,
	    idpf_msecs_to_ticks(task_delay));

	return (EAGAIN);
}

/**
 * idpf_vc_core_deinit - release everything idpf_vc_core_init() claimed
 * @adapter: driver private data
 */
void
idpf_vc_core_deinit(struct idpf_adapter *adapter)
{

	if ((adapter->flags & (1u << IDPF_VC_CORE_INIT)) == 0)
		return;

	idpf_ptp_release(adapter);
	idpf_deinit_task(adapter);
	idpf_rel_rx_pt_lkup(adapter);
	idpf_intr_rel(adapter);

	idpf_vc_xn_shutdown(adapter->vcxn_mngr);

	callout_drain(&adapter->serv_task);

	idpf_vport_params_buf_rel(adapter);
	idpf_lan_mmio_regs_rel(adapter);

	free(adapter->vports, M_DEVBUF);
	adapter->vports = NULL;

	adapter->flags &= ~(1u << IDPF_VC_CORE_INIT);
}

/* ---------------------------------------------------------------------
 * Vport capabilities and initialisation
 * --------------------------------------------------------------------- */

/**
 * idpf_vport_is_cap_ena - test a vport capability flag
 * @vport: vport to inspect
 * @flag: VIRTCHNL2_VPORT_* flag to test
 *
 * Return: true when the flag is set in the create-vport reply.
 */
bool
idpf_vport_is_cap_ena(struct idpf_vport *vport, uint16_t flag)
{
	struct virtchnl2_create_vport *vport_msg;

	vport_msg = vport->adapter->vport_params_recvd[vport->idx];

	return ((le16toh(vport_msg->vport_flags) & flag) != 0);
}

/**
 * idpf_fsteer_max_rules - maximum number of flow steering rules
 * @vport: vport to inspect
 */
unsigned int
idpf_fsteer_max_rules(struct idpf_vport *vport)
{
	struct virtchnl2_create_vport *vport_msg;

	vport_msg = vport->adapter->vport_params_recvd[vport->idx];

	return (le32toh(vport_msg->flow_steer_max_rules));
}

/**
 * idpf_vport_alloc_vec_indexes - claim relative interrupt vector indexes
 * @vport: vport requesting vectors
 * @rsrc: queue and vector resources to populate
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_vport_alloc_vec_indexes(struct idpf_vport *vport,
    struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_vector_info vec_info;
	int num_alloc_vecs;

	vec_info.num_curr_vecs = rsrc->num_q_vectors;
	vec_info.num_req_vecs = max(rsrc->num_txq, rsrc->num_rxq);
	vec_info.default_vport = vport->default_vport;
	vec_info.index = vport->idx;

	num_alloc_vecs = idpf_req_rel_vector_indexes(vport->adapter,
	    rsrc->q_vector_idxs, &vec_info);
	if (num_alloc_vecs <= 0) {
		device_printf(idpf_adapter_to_dev(vport->adapter),
		    "vector distribution failed: %d\n", num_alloc_vecs);
		return (EINVAL);
	}

	rsrc->num_q_vectors = num_alloc_vecs;

	return (0);
}

/**
 * idpf_vport_dealloc_vec_indexes - return the relative vector indexes
 * @vport: vport releasing vectors
 * @rsrc: queue and vector resources
 */
void
idpf_vport_dealloc_vec_indexes(struct idpf_vport *vport,
    struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_vector_info vec_info;

	vec_info.num_curr_vecs = rsrc->num_q_vectors;
	vec_info.num_req_vecs = 0;
	vec_info.default_vport = vport->default_vport;
	vec_info.index = vport->idx;

	idpf_req_rel_vector_indexes(vport->adapter, rsrc->q_vector_idxs,
	    &vec_info);

	free(rsrc->q_vector_idxs, M_DEVBUF);
	rsrc->q_vector_idxs = NULL;
}

/**
 * idpf_vport_edt_init - derive the earliest departure time parameters
 * @vport: vport to initialise
 *
 * The timestamp granularity is turned into a shift so that the transmit path
 * can scale a timestamp without a division.
 */
static void
idpf_vport_edt_init(struct idpf_vport *vport)
{
	struct idpf_adapter *adapter = vport->adapter;
	uint64_t tw_gran_m;

	if (!idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_EDT))
		return;

	tw_gran_m = le64toh(adapter->edt_caps.tstamp_granularity_ns) - 1;

	while ((tw_gran_m >> 1) != 0) {
		vport->tw_ts_gran_s++;
		tw_gran_m = tw_gran_m >> 1;
	}

	vport->tw_ts_gran_s++;
	vport->tw_horizon = le64toh(adapter->edt_caps.time_horizon_ns);
}

/**
 * idpf_vport_init - initialise a vport from the create-vport reply
 * @vport: vport to initialise
 * @max_q: queue budget granted to this vport
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_vport_init(struct idpf_vport *vport, struct idpf_vport_max_q *max_q)
{
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	struct virtchnl2_create_vport *vport_msg;
	struct idpf_vport_config *vport_config;
	uint16_t tx_itr[] = { 2, 8, 64, 128, 256 };
	uint16_t rx_itr[] = { 2, 8, 32, 96, 128 };
	struct idpf_rss_data *rss_data;
	uint16_t idx = vport->idx;
	int err;

	vport_config = adapter->vport_config[idx];
	rss_data = &vport_config->user_config.rss_data;
	vport_msg = adapter->vport_params_recvd[idx];

	err = idpf_vport_init_queue_reg_chunks(vport_config,
	    &vport_msg->chunks);
	if (err != 0)
		return (err);

	vport->max_mtu = le16toh(vport_msg->max_mtu) - IDPF_PACKET_HDR_PAD;
	if (vport->max_mtu < ETHERMIN) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "invalid value for maximum MTU: %d\n", vport->max_mtu);
		idpf_vport_deinit_queue_reg_chunks(vport_config);
		return (EINVAL);
	}

	if ((le16toh(vport_msg->vport_flags) & VIRTCHNL2_VPORT_UPLINK_PORT) !=
	    0)
		vport_config->flags |= (1u << IDPF_VPORT_UPLINK_PORT);

	vport_config->max_q.max_txq = max_q->max_txq;
	vport_config->max_q.max_rxq = max_q->max_rxq;
	vport_config->max_q.max_complq = max_q->max_complq;
	vport_config->max_q.max_bufq = max_q->max_bufq;

	rsrc->txq_model = le16toh(vport_msg->txq_model);
	rsrc->rxq_model = le16toh(vport_msg->rxq_model);
	vport->vport_type = le16toh(vport_msg->vport_type);
	vport->vport_id = le32toh(vport_msg->vport_id);

	rss_data->rss_key_size = min((uint16_t)IDPF_MAX_RSS_KEY_LEN,
	    le16toh(vport_msg->rss_key_size));
	rss_data->rss_lut_size = le16toh(vport_msg->rss_lut_size);

	memcpy(vport->default_mac_addr, vport_msg->default_mac_addr,
	    ETHER_ADDR_LEN);

	/* Dynamic interrupt moderation profiles. */
	memcpy(vport->rx_itr_profile, rx_itr, sizeof(rx_itr));
	memcpy(vport->tx_itr_profile, tx_itr, sizeof(tx_itr));

	idpf_vport_set_hsplit(vport, true);

	idpf_vport_init_num_qs(vport, vport_msg, rsrc);
	idpf_vport_calc_num_q_desc(vport, rsrc);
	idpf_vport_calc_num_q_groups(rsrc);
	idpf_vport_alloc_vec_indexes(vport, rsrc);

	vport->crc_enable = adapter->crc_enable;

	idpf_vport_edt_init(vport);

	if ((le16toh(vport_msg->vport_flags) & VIRTCHNL2_VPORT_UPLINK_PORT) ==
	    0)
		return (0);

	err = idpf_ptp_get_vport_tstamps_caps(vport);
	if (err == EOPNOTSUPP) {
		/*
		 * The control plane policy may leave TX timestamping
		 * disabled; that is not a bring-up failure.
		 */
		return (0);
	}

	return (err);
}

/* ---------------------------------------------------------------------
 * Vector and queue identifier discovery
 * --------------------------------------------------------------------- */

/**
 * idpf_get_vec_ids - expand the vector chunks into a flat index array
 * @adapter: driver private data
 * @vecids: destination array
 * @num_vecids: capacity of @vecids
 * @chunks: vector chunks received over the mailbox
 *
 * Slot 0 is always the mailbox vector.
 *
 * Return: the number of identifiers written.
 */
int
idpf_get_vec_ids(struct idpf_adapter *adapter, uint16_t *vecids,
    int num_vecids, struct virtchnl2_vector_chunks *chunks)
{
	uint16_t num_chunks = le16toh(chunks->num_vchunks);
	int num_vecid_filled = 0;
	int i = 0, j;

	vecids[num_vecid_filled] = adapter->mb_vector.v_idx;
	num_vecid_filled++;

	for (j = 0; j < num_chunks; j++) {
		struct virtchnl2_vector_chunk *chunk;
		uint16_t start_vecid, num_vec;

		chunk = &chunks->vchunks[j];
		num_vec = le16toh(chunk->num_vectors);
		start_vecid = le16toh(chunk->start_vector_id);

		for (i = 0; i < num_vec; i++) {
			if ((num_vecid_filled + i) >= num_vecids)
				break;
			vecids[num_vecid_filled + i] = start_vecid;
			start_vecid++;
		}

		num_vecid_filled += i;
	}

	return (num_vecid_filled);
}

/**
 * idpf_vport_get_queue_ids - expand queue chunks of one type
 * @qids: destination array
 * @num_qids: capacity of @qids
 * @q_type: queue type to collect
 * @chunks: queue chunks received over the mailbox
 *
 * Return: the number of identifiers written.
 */
static int
idpf_vport_get_queue_ids(uint32_t *qids, int num_qids, uint16_t q_type,
    struct idpf_queue_id_reg_info *chunks)
{
	uint16_t num_chunks = chunks->num_chunks;
	uint32_t num_q_id_filled = 0, i = 0;
	uint32_t start_q_id, num_q;

	while (num_chunks-- != 0) {
		struct idpf_queue_id_reg_chunk *chunk;

		chunk = &chunks->queue_chunks[num_chunks];
		if (chunk->type != q_type)
			continue;

		num_q = chunk->num_queues;
		start_q_id = chunk->start_queue_id;

		for (i = 0; i < num_q; i++) {
			if ((num_q_id_filled + i) >= (uint32_t)num_qids)
				break;
			qids[num_q_id_filled + i] = start_q_id;
			start_q_id++;
		}

		num_q_id_filled += i;
	}

	return (num_q_id_filled);
}

/**
 * __idpf_vport_queue_ids_init - stamp identifiers onto queues of one type
 * @rsrc: queue and vector resources
 * @qids: identifiers to assign
 * @num_qids: number of identifiers available
 * @q_type: queue type being assigned
 */
static void
__idpf_vport_queue_ids_init(struct idpf_q_vec_rsrc *rsrc, const uint32_t *qids,
    int num_qids, uint32_t q_type)
{
	struct idpf_queue *q;
	int i, j, k = 0;

	switch (q_type) {
	case VIRTCHNL2_QUEUE_TYPE_TX:
		for (i = 0; i < rsrc->num_txq_grp; i++) {
			struct idpf_txq_group *txq_grp = &rsrc->txq_grps[i];

			for (j = 0; j < txq_grp->num_txq && k < num_qids;
			    j++, k++) {
				txq_grp->txqs[j]->q_id = qids[k];
				txq_grp->txqs[j]->q_type = q_type;
			}
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			uint16_t num_rxq;

			if (idpf_is_queue_model_split(rsrc->rxq_model))
				num_rxq = rx_qgrp->splitq.num_rxq_sets;
			else
				num_rxq = rx_qgrp->singleq.num_rxq;

			for (j = 0; j < num_rxq && k < num_qids; j++, k++) {
				if (idpf_is_queue_model_split(rsrc->rxq_model))
					q = &rx_qgrp->splitq.rxq_sets[j]->rxq;
				else
					q = rx_qgrp->singleq.rxqs[j];
				q->q_id = qids[k];
				q->q_type = q_type;
			}
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_TX_COMPLETION:
		for (i = 0; i < rsrc->num_txq_grp && i < num_qids; i++) {
			struct idpf_txq_group *txq_grp = &rsrc->txq_grps[i];

			txq_grp->complq->q_id = qids[i];
			txq_grp->complq->q_type = q_type;
		}
		break;
	case VIRTCHNL2_QUEUE_TYPE_RX_BUFFER:
		for (i = 0; i < rsrc->num_rxq_grp; i++) {
			struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
			uint8_t num_bufqs = rsrc->num_bufqs_per_qgrp;

			for (j = 0; j < num_bufqs && k < num_qids; j++, k++) {
				q = &rx_qgrp->splitq.bufq_sets[j].bufq;
				q->q_id = qids[k];
				q->q_type = q_type;
			}
		}
		break;
	default:
		break;
	}
}

/**
 * idpf_vport_queue_ids_init - assign every queue its device identifier
 * @rsrc: queue and vector resources
 * @chunks: queue register information received over the mailbox
 *
 * Return: 0 on success, EINVAL when the control plane granted fewer
 * identifiers than the driver configured queues, ENOMEM on allocation
 * failure.
 */
int
idpf_vport_queue_ids_init(struct idpf_q_vec_rsrc *rsrc,
    struct idpf_queue_id_reg_info *chunks)
{
	/* The driver never handles more than 256 queues of one type. */
#define IDPF_MAX_QIDS	256
	int num_ids, ret = 0;
	uint16_t q_type;
	uint32_t *qids;

	qids = malloc(IDPF_MAX_QIDS * sizeof(*qids), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (qids == NULL)
		return (ENOMEM);

	q_type = VIRTCHNL2_QUEUE_TYPE_TX;
	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS, q_type,
	    chunks);
	if (num_ids < rsrc->num_txq) {
		ret = EINVAL;
		goto out;
	}
	__idpf_vport_queue_ids_init(rsrc, qids, num_ids, q_type);

	q_type = VIRTCHNL2_QUEUE_TYPE_RX;
	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS, q_type,
	    chunks);
	if (num_ids < rsrc->num_rxq) {
		ret = EINVAL;
		goto out;
	}
	__idpf_vport_queue_ids_init(rsrc, qids, num_ids, q_type);

	if (!idpf_is_queue_model_split(rsrc->txq_model))
		goto check_rx;

	q_type = VIRTCHNL2_QUEUE_TYPE_TX_COMPLETION;
	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS, q_type,
	    chunks);
	if (num_ids < rsrc->num_complq) {
		ret = EINVAL;
		goto out;
	}
	__idpf_vport_queue_ids_init(rsrc, qids, num_ids, q_type);

check_rx:
	if (!idpf_is_queue_model_split(rsrc->rxq_model))
		goto out;

	q_type = VIRTCHNL2_QUEUE_TYPE_RX_BUFFER;
	num_ids = idpf_vport_get_queue_ids(qids, IDPF_MAX_QIDS, q_type,
	    chunks);
	if (num_ids < rsrc->num_bufq) {
		ret = EINVAL;
		goto out;
	}
	__idpf_vport_queue_ids_init(rsrc, qids, num_ids, q_type);

out:
	free(qids, M_DEVBUF);

	return (ret);
#undef IDPF_MAX_QIDS
}

/**
 * idpf_vport_adjust_qs - recompute the queue layout after a renegotiation
 * @vport: vport being reconfigured
 * @rsrc: queue and vector resources to update
 */
void
idpf_vport_adjust_qs(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{
	struct virtchnl2_create_vport vport_msg;

	vport_msg.txq_model = htole16(rsrc->txq_model);
	vport_msg.rxq_model = htole16(rsrc->rxq_model);

	idpf_vport_calc_total_qs(vport->adapter, vport->idx, &vport_msg, NULL);

	idpf_vport_init_num_qs(vport, &vport_msg, rsrc);
	idpf_vport_calc_num_q_groups(rsrc);
}

/* ---------------------------------------------------------------------
 * MAC filters and receive modes
 * --------------------------------------------------------------------- */

/**
 * idpf_set_mac_type - classify a MAC address as primary or extra
 * @default_mac_addr: the vport's primary address
 * @mac_addr: wire entry to classify
 */
static void
idpf_set_mac_type(const uint8_t *default_mac_addr,
    struct virtchnl2_mac_addr *mac_addr)
{
	bool is_primary;

	is_primary = memcmp(default_mac_addr, mac_addr->addr,
	    ETHER_ADDR_LEN) == 0;
	mac_addr->type = is_primary ? VIRTCHNL2_MAC_ADDR_PRIMARY :
	    VIRTCHNL2_MAC_ADDR_EXTRA;
}

/**
 * idpf_mac_filter_async_handler - reconcile a failed asynchronous filter add
 * @adapter: driver private data
 * @xn: transaction the reply belongs to
 * @ctlq_msg: received message
 *
 * Filter changes are sometimes issued from a context that cannot sleep, so the
 * reply is handled here.  Nothing can undo a rejected filter, but the entries
 * are dropped from the software list so it keeps describing what the device
 * actually has.
 *
 * Return: 0 when handled, EINVAL when the payload is malformed.
 */
static int
idpf_mac_filter_async_handler(struct idpf_adapter *adapter,
    struct idpf_vc_xn *xn, const struct idpf_ctlq_msg *ctlq_msg)
{
	struct virtchnl2_mac_addr_list *ma_list;
	struct idpf_vport_config *vport_config;
	struct virtchnl2_mac_addr *mac_addr;
	struct idpf_mac_filter *f, *tmp;
	struct idpf_vport *vport;
	uint32_t vport_id;
	uint16_t num_entries;
	int i;

	/* Nothing to do when the request succeeded. */
	if (ctlq_msg->cookie.mbx.chnl_retval == 0)
		return (0);

	if (xn->reply_sz < sizeof(*ma_list))
		goto invalid_payload;

	ma_list = ctlq_msg->ctx.indirect.payload->va;
	mac_addr = ma_list->mac_addr_list;
	num_entries = le16toh(ma_list->num_mac_addr);

	if (xn->reply_sz < (ssize_t)(sizeof(*ma_list) +
	    sizeof(*mac_addr) * num_entries))
		goto invalid_payload;

	vport_id = le32toh(ma_list->vport_id);
	vport = idpf_vid_to_vport(adapter, vport_id);
	if (vport == NULL)
		goto invalid_payload;

	vport_config = adapter->vport_config[vport->idx];

	mtx_lock(&vport_config->mac_filter_list_lock);
	TAILQ_FOREACH_SAFE(f, &vport_config->user_config.mac_filter_list, list,
	    tmp) {
		for (i = 0; i < num_entries; i++) {
			if (memcmp(mac_addr[i].addr, f->macaddr,
			    ETHER_ADDR_LEN) != 0)
				continue;
			TAILQ_REMOVE(&vport_config->user_config.mac_filter_list,
			    f, list);
			free(f, M_DEVBUF);
			break;
		}
	}
	mtx_unlock(&vport_config->mac_filter_list_lock);

	device_printf(idpf_adapter_to_dev(adapter),
	    "received error sending mac filter request (op %d)\n", xn->vc_op);

	return (0);

invalid_payload:
	device_printf(idpf_adapter_to_dev(adapter),
	    "received invalid mac filter payload (op %d) (len %zd)\n",
	    xn->vc_op, xn->reply_sz);

	return (EINVAL);
}

/**
 * idpf_add_del_mac_filters - push pending MAC filter changes to the device
 * @adapter: driver private data
 * @vport_config: vport configuration holding the filter list
 * @default_mac_addr: the vport's primary address
 * @vport_id: vport identifier
 * @add: true to add filters, false to delete them
 * @async: true to send without waiting for the reply
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_add_del_mac_filters(struct idpf_adapter *adapter,
    struct idpf_vport_config *vport_config, const uint8_t *default_mac_addr,
    uint32_t vport_id, bool add, bool async)
{
	struct virtchnl2_mac_addr_list *ma_list = NULL;
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_mac_addr *mac_addr = NULL;
	struct idpf_mac_filter *f, *tmp;
	uint32_t num_msgs, total_filters = 0;
	uint32_t i, k = 0;
	ssize_t reply_sz;
	int err = 0;

	xn_params.vc_op = add ? VIRTCHNL2_OP_ADD_MAC_ADDR :
	    VIRTCHNL2_OP_DEL_MAC_ADDR;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);
	xn_params.async = async;
	xn_params.async_handler = idpf_mac_filter_async_handler;

	mtx_lock(&vport_config->mac_filter_list_lock);

	TAILQ_FOREACH(f, &vport_config->user_config.mac_filter_list, list) {
		if (add && f->add)
			total_filters++;
		else if (!add && f->remove)
			total_filters++;
	}

	if (total_filters == 0) {
		mtx_unlock(&vport_config->mac_filter_list_lock);
		return (0);
	}

	mac_addr = malloc(total_filters * sizeof(*mac_addr), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (mac_addr == NULL) {
		mtx_unlock(&vport_config->mac_filter_list_lock);
		err = ENOMEM;
		goto error;
	}

	i = 0;
	TAILQ_FOREACH_SAFE(f, &vport_config->user_config.mac_filter_list, list,
	    tmp) {
		if (add && f->add) {
			memcpy(mac_addr[i].addr, f->macaddr, ETHER_ADDR_LEN);
			idpf_set_mac_type(default_mac_addr, &mac_addr[i]);
			i++;
			f->add = false;
		} else if (!add && f->remove) {
			memcpy(mac_addr[i].addr, f->macaddr, ETHER_ADDR_LEN);
			idpf_set_mac_type(default_mac_addr, &mac_addr[i]);
			i++;
			f->remove = false;
		}

		if (i == total_filters)
			break;
	}

	mtx_unlock(&vport_config->mac_filter_list_lock);

	/* Split the filters so that no message exceeds the buffer size. */
	num_msgs = howmany(total_filters, IDPF_NUM_FILTERS_PER_MSG);

	for (i = 0, k = 0; i < num_msgs; i++) {
		uint32_t entries_size, num_entries;
		size_t buf_size;

		num_entries = min(total_filters,
		    (uint32_t)IDPF_NUM_FILTERS_PER_MSG);
		entries_size = sizeof(*mac_addr) * num_entries;
		buf_size = IDPF_STRUCT_VAR_LEN(struct virtchnl2_mac_addr_list,
		    mac_addr_list, num_entries);

		if (ma_list == NULL ||
		    num_entries != IDPF_NUM_FILTERS_PER_MSG) {
			free(ma_list, M_DEVBUF);
			ma_list = malloc(buf_size, M_DEVBUF,
			    M_NOWAIT | M_ZERO);
			if (ma_list == NULL) {
				err = ENOMEM;
				goto list_prep_error;
			}
		} else {
			memset(ma_list, 0, buf_size);
		}

		ma_list->vport_id = htole32(vport_id);
		ma_list->num_mac_addr = htole16(num_entries);
		memcpy(ma_list->mac_addr_list, &mac_addr[k], entries_size);

		xn_params.send_buf.iov_base = ma_list;
		xn_params.send_buf.iov_len = buf_size;
		reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
		if (reply_sz < 0) {
			err = -reply_sz;
			goto mbx_error;
		}

		k += num_entries;
		total_filters -= num_entries;
	}

mbx_error:
	free(ma_list, M_DEVBUF);
list_prep_error:
	free(mac_addr, M_DEVBUF);
error:
	if (err != 0)
		device_printf(idpf_adapter_to_dev(adapter),
		    "failed to add or del mac filters: %d\n", err);

	return (err);
}

/**
 * idpf_set_promiscuous - program the vport's promiscuous modes
 * @adapter: driver private data
 * @config_data: vport user configuration
 * @vport_id: vport identifier
 *
 * Always sent asynchronously: the caller may hold locks that forbid sleeping.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_set_promiscuous(struct idpf_adapter *adapter,
    struct idpf_vport_user_config_data *config_data, uint32_t vport_id)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	struct virtchnl2_promisc_info vpi = { 0 };
	ssize_t reply_sz;
	uint16_t flags = 0;

	if ((config_data->user_flags & (1ULL << __IDPF_PROMISC_UC)) != 0)
		flags |= VIRTCHNL2_UNICAST_PROMISC;
	if ((config_data->user_flags & (1ULL << __IDPF_PROMISC_MC)) != 0)
		flags |= VIRTCHNL2_MULTICAST_PROMISC;

	vpi.vport_id = htole32(vport_id);
	vpi.flags = htole16(flags);

	xn_params.vc_op = VIRTCHNL2_OP_CONFIG_PROMISCUOUS_MODE;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);
	xn_params.send_buf.iov_base = &vpi;
	xn_params.send_buf.iov_len = sizeof(vpi);
	xn_params.async = true;

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);

	return (reply_sz < 0 ? -reply_sz : 0);
}

/* ---------------------------------------------------------------------
 * VLAN offloads
 * --------------------------------------------------------------------- */

/**
 * idpf_send_ena_dis_vlan_offload - enable or disable one VLAN offload
 * @adapter: driver private data
 * @vport_id: vport identifier
 * @ethertype: VLAN ethertype the offload applies to
 * @strip: true for stripping, false for insertion
 * @ena: true to enable, false to disable
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_send_ena_dis_vlan_offload(struct idpf_adapter *adapter, uint32_t vport_id,
    uint32_t ethertype, bool strip, bool ena)
{
	struct virtchnl2_vlan_setting vlano = { 0 };
	struct idpf_vc_xn_params xn_params = { 0 };
	ssize_t reply_sz;
	int timeout_ms;
	uint32_t vc_op;

	if (ena) {
		vc_op = strip ? VIRTCHNL2_OP_ENABLE_VLAN_STRIPPING :
		    VIRTCHNL2_OP_ENABLE_VLAN_INSERTION;
		timeout_ms = idpf_get_vc_xn_default_timeout(adapter);
	} else {
		vc_op = strip ? VIRTCHNL2_OP_DISABLE_VLAN_STRIPPING :
		    VIRTCHNL2_OP_DISABLE_VLAN_INSERTION;
		timeout_ms = idpf_get_vc_xn_min_timeout(adapter);
	}

	vlano.vport_id = htole32(vport_id);
	vlano.outer_ethertype = htole32(ethertype);

	xn_params.vc_op = vc_op;
	xn_params.timeout_ms = timeout_ms;
	xn_params.send_buf.iov_base = &vlano;
	xn_params.send_buf.iov_len = sizeof(vlano);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);

	return (reply_sz < 0 ? -reply_sz : 0);
}

/**
 * idpf_set_rxq_vlan_proto - record the VLAN ethertype on every RX queue
 * @rsrc: queue and vector resources
 * @vlan_proto: big-endian ethertype, or 0 when stripping is disabled
 *
 * The receive path uses this to decide whether a stripped tag should be
 * reported to the stack.
 */
static void
idpf_set_rxq_vlan_proto(struct idpf_q_vec_rsrc *rsrc, uint16_t vlan_proto)
{
	bool is_splitq = idpf_is_queue_model_split(rsrc->rxq_model);
	uint16_t i, j;

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
		uint16_t num_rxq;

		num_rxq = is_splitq ? rx_qgrp->splitq.num_rxq_sets :
		    rx_qgrp->singleq.num_rxq;

		for (j = 0; j < num_rxq; j++) {
			struct idpf_queue *q;

			q = is_splitq ? &rx_qgrp->splitq.rxq_sets[j]->rxq :
			    rx_qgrp->singleq.rxqs[j];
			q->rx.vlan_proto = vlan_proto;
		}
	}
}

/**
 * idpf_set_vlan_features - apply an interface capability change to the device
 * @vport: vport to reconfigure
 * @capmask: the IFCAP_* bits that changed
 *
 * FreeBSD reports hardware VLAN tagging as a single capability covering both
 * directions, so a change to IFCAP_VLAN_HWTAGGING reprograms stripping and
 * insertion together.  The current state is read back from the interface,
 * which iflib has already updated.  [FBSD15:A30]
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_set_vlan_features(struct idpf_vport *vport, int capmask)
{
	struct idpf_adapter *adapter = vport->adapter;
	struct virtchnl2_vlan_get_caps *vlan_caps;
	struct virtchnl2_vlan_supported_caps *strip, *insert;
	uint32_t ethertype;
	bool enable;
	int err;

	if ((capmask & IFCAP_VLAN_HWTAGGING) == 0)
		return (0);

	vlan_caps = &adapter->vlan_caps;
	strip = &vlan_caps->strip;
	insert = &vlan_caps->insert;
	enable = idpf_is_feature_ena(vport, IFCAP_VLAN_HWTAGGING);

	ethertype = 0;
	if ((le32toh(strip->outer) & VIRTCHNL2_VLAN_ETHERTYPE_8100) != 0)
		ethertype = VIRTCHNL2_VLAN_ETHERTYPE_8100;

	idpf_set_rxq_vlan_proto(&vport->dflt_qv_rsrc,
	    enable ? htons(ETHERTYPE_VLAN) : 0);

	err = idpf_send_ena_dis_vlan_offload(adapter, vport->vport_id,
	    ethertype, true, enable);
	if (err != 0)
		return (err);

	ethertype = 0;
	if ((le32toh(insert->outer) & VIRTCHNL2_VLAN_ETHERTYPE_8100) != 0)
		ethertype = VIRTCHNL2_VLAN_ETHERTYPE_8100;

	return (idpf_send_ena_dis_vlan_offload(adapter, vport->vport_id,
	    ethertype, false, enable));
}




