/**
 * @file idpf_vc_common.c
 * @brief Control-plane (virtchnl2) placeholder implementation.
 *
 * Minimal, stub-level implementation only. No functional virtchnl2
 * message handling exists yet; both functions return success/no-op
 * without creating any real taskqueue or scheduling any real work.
 */

#include "idpf_vc_common.h"
#include "idpf_mmg_v_policy.h"

#include "virtchnl2.h"

/**
 * @brief Initialize the control-plane message-processing task.
 *
 * Stub-level placeholder for Phase A: returns success without creating
 * any taskqueue or scheduling any work. Real taskqueue creation (and
 * its paired, reverse-order drain-then-free teardown) is introduced by
 * a later, traceable expansion.
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 on success.
 */
int
idpf_vc_common_init(struct idpf_sc *sc __unused)
{
	return (0);
}

/**
 * @brief Tear down the control-plane message-processing task.
 *
 * Stub-level placeholder for Phase A: no-op, since idpf_vc_common_init()
 * does not yet create any real taskqueue to drain or free.
 *
 * @param sc Driver software context for the device instance.
 */
void
idpf_vc_common_deinit(struct idpf_sc *sc __unused)
{
}

/**
 * @brief Negotiate the VIRTCHNL2 version with the CP (FR-004, FR-016).
 *
 * Sends `VIRTCHNL2_OP_VERSION` synchronously through the shared
 * idpf_ctlq_xn_* transport. A no-response (transport) failure is retried at
 * least `IDPF_MMG_V_VERSION_RETRY_MAX` times with an
 * `IDPF_MMG_V_VERSION_RETRY_MS` gap; a CP major-version mismatch fails
 * immediately (`EPROTONOSUPPORT`, no retry) while a minor mismatch warns and
 * continues. Sets `IDPF_STATE_VERSION_DONE` on success.
 *
 * @param sc Driver software context.
 * @return 0 on success, `ETIMEDOUT` if the send never completes, or
 *         `EPROTONOSUPPORT` on a major-version mismatch.
 */
int
idpf_send_version(struct idpf_sc *sc)
{
	struct virtchnl2_version_info req;
	struct virtchnl2_version_info resp;
	struct idpf_ctlq_msg msg;
	struct idpf_ctlq_xn_send_params params;
	uint32_t resp_major, resp_minor;
	unsigned int attempt;
	int rc;

	memset(&req, 0, sizeof(req));
	req.major = htole32(IDPF_MMG_V_VC_VERSION_MAJOR);
	req.minor = htole32(IDPF_MMG_V_VC_VERSION_MINOR);

	rc = -1;
	for (attempt = 0; attempt < IDPF_MMG_V_VERSION_RETRY_MAX; attempt++) {
		memset(&resp, 0, sizeof(resp));
		memset(&msg, 0, sizeof(msg));
		msg.opcode = VIRTCHNL2_OP_VERSION;
		/* Opcode field the shared idpf_ctlq_xn transport reads back. */
		msg.cookie.mbx.chnl_opcode = VIRTCHNL2_OP_VERSION;
		msg.data_len = sizeof(req);

		memset(&params, 0, sizeof(params));
		params.hw = &sc->hw;
		params.xnm = sc->xnm;
		params.ctlq_info = sc->hw.asq;
		params.ctlq_msg = &msg;
		params.send_buf.iov_base = &req;
		params.send_buf.iov_len = sizeof(req);
		params.recv_buf.iov_base = &resp;
		params.recv_buf.iov_len = sizeof(resp);
		params.timeout_ms = IDPF_MMG_V_VC_TIMEOUT_MS;
		params.async_resp_cb = NULL;

		rc = idpf_ctlq_xn_send(&params);
		if (rc == 0)
			break;
		idpf_msec_delay(IDPF_MMG_V_VERSION_RETRY_MS);
	}
	if (rc != 0) {
		device_printf(sc->dev,
		    "idpf: VERSION negotiation failed after %u attempts\n",
		    IDPF_MMG_V_VERSION_RETRY_MAX);
		return (ETIMEDOUT);
	}

	resp_major = le32toh(resp.major);
	resp_minor = le32toh(resp.minor);
	if (resp_major != IDPF_MMG_V_VC_VERSION_MAJOR) {
		device_printf(sc->dev,
		    "idpf: VERSION major mismatch (cp=%u driver=%u)\n",
		    resp_major, IDPF_MMG_V_VC_VERSION_MAJOR);
		return (EPROTONOSUPPORT);
	}
	if (resp_minor != IDPF_MMG_V_VC_VERSION_MINOR)
		device_printf(sc->dev,
		    "idpf: VERSION minor mismatch, continuing (cp=%u driver=%u)\n",
		    resp_minor, IDPF_MMG_V_VC_VERSION_MINOR);

	atomic_set_32(&sc->state, IDPF_STATE_VERSION_DONE);
	return (0);
}

/**
 * @brief Negotiate device capabilities with the CP (FR-005).
 *
 * Sends `VIRTCHNL2_OP_GET_CAPS` with an all-zero/masked request (the MVP asks
 * for nothing beyond the default vport/queue floor), caches the response in
 * `sc->caps`, and sets `IDPF_STATE_CAPS_DONE`. Fails closed (`EINVAL`) on a
 * truncated response or a zero `max_tx_q`/`max_rx_q` minimal required field.
 *
 * @param sc Driver software context.
 * @return 0 on success, `EIO` on transport failure, `EINVAL` on a truncated or
 *         insufficient capability response.
 */
int
idpf_get_caps(struct idpf_sc *sc)
{
	struct virtchnl2_get_capabilities req;
	struct virtchnl2_get_capabilities resp;
	struct idpf_ctlq_msg msg;
	struct idpf_ctlq_xn_send_params params;
	int rc;

	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	memset(&msg, 0, sizeof(msg));
	msg.opcode = VIRTCHNL2_OP_GET_CAPS;
	msg.cookie.mbx.chnl_opcode = VIRTCHNL2_OP_GET_CAPS;
	msg.data_len = sizeof(req);

	memset(&params, 0, sizeof(params));
	params.hw = &sc->hw;
	params.xnm = sc->xnm;
	params.ctlq_info = sc->hw.asq;
	params.ctlq_msg = &msg;
	params.send_buf.iov_base = &req;
	params.send_buf.iov_len = sizeof(req);
	params.recv_buf.iov_base = &resp;
	params.recv_buf.iov_len = sizeof(resp);
	params.timeout_ms = IDPF_MMG_V_VC_TIMEOUT_MS;
	params.async_resp_cb = NULL;

	rc = idpf_ctlq_xn_send(&params);
	if (rc != 0) {
		device_printf(sc->dev, "idpf: GET_CAPS transport failure: %d\n",
		    rc);
		return (EIO);
	}
	if (params.recv_len < sizeof(resp)) {
		device_printf(sc->dev,
		    "idpf: GET_CAPS truncated response\n");
		return (EINVAL);
	}
	if (le16toh(resp.max_tx_q) == 0 || le16toh(resp.max_rx_q) == 0) {
		device_printf(sc->dev,
		    "idpf: GET_CAPS missing required queue capability\n");
		return (EINVAL);
	}

	sc->caps = resp;
	atomic_set_32(&sc->state, IDPF_STATE_CAPS_DONE);
	return (0);
}

/**
 * @brief Create the single MVP vport (FR-006, FR-020).
 *
 * Sends `VIRTCHNL2_OP_CREATE_VPORT` requesting a default single-queue-model
 * vport with 1 TX + 1 RX queue, persists the full CP response (vport id, MTU,
 * MAC, flags, descriptor ids, RSS sizing, and chunks) in `sc->vport`, records
 * `sc->vport_id`, validates the queue register chunk count, and sets
 * `IDPF_STATE_VPORT_CREATED`. `OP_ALLOC_VECTORS` is never issued (FR-007).
 *
 * @param sc Driver software context.
 * @return 0 on success, `EIO` on transport failure, `EINVAL` on a truncated
 *         response or an out-of-range chunk count.
 */
int
idpf_create_vport(struct idpf_sc *sc)
{
	struct virtchnl2_create_vport req;
	struct virtchnl2_create_vport resp;
	struct idpf_ctlq_msg msg;
	struct idpf_ctlq_xn_send_params params;
	uint16_t num_chunks;
	int rc;

	memset(&req, 0, sizeof(req));
	req.vport_type = htole16(VIRTCHNL2_VPORT_TYPE_DEFAULT);
	req.txq_model = htole16(VIRTCHNL2_QUEUE_MODEL_SINGLE);
	req.rxq_model = htole16(VIRTCHNL2_QUEUE_MODEL_SINGLE);
	req.num_tx_q = htole16(IDPF_MMG_V_ATTACH_NUM_TXQ);
	req.num_rx_q = htole16(IDPF_MMG_V_ATTACH_NUM_RXQ);

	memset(&resp, 0, sizeof(resp));
	memset(&msg, 0, sizeof(msg));
	msg.opcode = VIRTCHNL2_OP_CREATE_VPORT;
	msg.cookie.mbx.chnl_opcode = VIRTCHNL2_OP_CREATE_VPORT;
	msg.data_len = sizeof(req);

	memset(&params, 0, sizeof(params));
	params.hw = &sc->hw;
	params.xnm = sc->xnm;
	params.ctlq_info = sc->hw.asq;
	params.ctlq_msg = &msg;
	params.send_buf.iov_base = &req;
	params.send_buf.iov_len = sizeof(req);
	params.recv_buf.iov_base = &resp;
	params.recv_buf.iov_len = sizeof(resp);
	params.timeout_ms = IDPF_MMG_V_VC_TIMEOUT_MS;
	params.async_resp_cb = NULL;

	rc = idpf_ctlq_xn_send(&params);
	if (rc != 0) {
		device_printf(sc->dev, "idpf: CREATE_VPORT transport failure: %d\n",
		    rc);
		return (EIO);
	}
	if (params.recv_len < sizeof(resp)) {
		device_printf(sc->dev, "idpf: CREATE_VPORT truncated response\n");
		return (EINVAL);
	}

	num_chunks = le16toh(resp.chunks.num_chunks);
	if (num_chunks == 0 || num_chunks > IDPF_MMG_V_MAX_QUEUE_CHUNKS) {
		device_printf(sc->dev,
		    "idpf: CREATE_VPORT invalid queue chunk count %u\n",
		    num_chunks);
		return (EINVAL);
	}

	sc->vport = resp;
	sc->vport_id = le32toh(resp.vport_id);
	atomic_set_32(&sc->state, IDPF_STATE_VPORT_CREATED);
	return (0);
}

/**
 * @brief Destroy the vport created at attach (FR-003, detach path).
 *
 * Sends `VIRTCHNL2_OP_DESTROY_VPORT` for the cached `vport_id`. A non-success
 * status is logged and returned but is non-fatal: the caller (idpf_if_detach)
 * continues local teardown regardless.
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 on success, a FreeBSD errno on transport failure (advisory only).
 */
int
idpf_destroy_vport(struct idpf_sc *sc)
{
	struct virtchnl2_vport req;
	struct idpf_ctlq_msg msg;
	struct idpf_ctlq_xn_send_params params;
	int rc;

	memset(&req, 0, sizeof(req));
	req.vport_id = htole32(sc->vport_id);

	memset(&msg, 0, sizeof(msg));
	msg.opcode = VIRTCHNL2_OP_DESTROY_VPORT;
	msg.cookie.mbx.chnl_opcode = VIRTCHNL2_OP_DESTROY_VPORT;
	msg.data_len = sizeof(req);

	memset(&params, 0, sizeof(params));
	params.hw = &sc->hw;
	params.xnm = sc->xnm;
	params.ctlq_info = sc->hw.asq;
	params.ctlq_msg = &msg;
	params.send_buf.iov_base = &req;
	params.send_buf.iov_len = sizeof(req);
	params.recv_buf.iov_base = NULL;
	params.recv_buf.iov_len = 0;
	params.timeout_ms = IDPF_MMG_V_VC_TIMEOUT_MS;
	params.async_resp_cb = NULL;

	rc = idpf_ctlq_xn_send(&params);
	if (rc != 0)
		device_printf(sc->dev,
		    "idpf: DESTROY_VPORT failed (%d), continuing detach\n", rc);
	return (rc);
}

/**
 * @brief Reset the VF unconditionally on detach (FR-003, research.md R3).
 *
 * Sends a zero-length `VIRTCHNL2_OP_RESET_VF` request. Per Co-Design MMG_V the
 * VF issues this via mailbox and the CP drives VFGEN_RSTAT; it is sent
 * unconditionally whenever the mailbox came up (no SRIOV-capability gate). A
 * failure/timeout is logged and returned but never aborts detach.
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 on success, a FreeBSD errno on transport failure (advisory only).
 */
int
idpf_reset_vf(struct idpf_sc *sc)
{
	struct idpf_ctlq_msg msg;
	struct idpf_ctlq_xn_send_params params;
	int rc;

	memset(&msg, 0, sizeof(msg));
	msg.opcode = VIRTCHNL2_OP_RESET_VF;
	msg.cookie.mbx.chnl_opcode = VIRTCHNL2_OP_RESET_VF;
	msg.data_len = 0;

	memset(&params, 0, sizeof(params));
	params.hw = &sc->hw;
	params.xnm = sc->xnm;
	params.ctlq_info = sc->hw.asq;
	params.ctlq_msg = &msg;
	params.send_buf.iov_base = NULL;
	params.send_buf.iov_len = 0;
	params.recv_buf.iov_base = NULL;
	params.recv_buf.iov_len = 0;
	params.timeout_ms = IDPF_MMG_V_VC_TIMEOUT_MS;
	params.async_resp_cb = NULL;

	rc = idpf_ctlq_xn_send(&params);
	if (rc != 0)
		device_printf(sc->dev,
		    "idpf: RESET_VF failed (%d), continuing detach\n", rc);
	return (rc);
}
