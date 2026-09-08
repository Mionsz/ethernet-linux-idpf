/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2023 Intel Corporation */

/*
 * PTP capability negotiation for the FreeBSD IDPF driver.
 *
 * Scope: this file negotiates PTP capabilities with the control plane and
 * exposes the device clock.  TX timestamp latch harvesting is deliberately
 * absent -- see the note in idpf_ptp.h.
 *
 * Every register offset used here is supplied by the control plane, so all
 * MMIO goes through idpf_ptp_reg_addr(), which returns NULL for an unmapped
 * offset.  idpf_get_reg_addr() must not be used: it panics on a bad offset.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/endian.h>
#include <sys/limits.h>
#include <sys/sysctl.h>

#include "idpf.h"
#include "idpf_ptp.h"
#include "idpf_virtchnl.h"

#define IDPF_PTP_PHC_CACHE_TICKS	max(hz / 10, 1)
#define IDPF_PTP_CMD_SYNC_RETRIES	1000

/**
 * idpf_ptp_tstamp_extend_32b_to_64b - widen a 32-bit timestamp
 * @cached_phc_time: last known PHC time in nanoseconds
 * @in_timestamp: 32-bit nanosecond timestamp from hardware
 *
 * Returns the 64-bit nanosecond value nearest @cached_phc_time.
 */
u64
idpf_ptp_tstamp_extend_32b_to_64b(u64 cached_phc_time,
    u32 in_timestamp)
{
	u32 delta, phc_time_lo;

	phc_time_lo = (u32)cached_phc_time;
	delta = in_timestamp - phc_time_lo;

	/* A delta past the half-range means the low word wrapped backwards. */
	if (delta > UINT32_MAX / 2) {
		delta = phc_time_lo - in_timestamp;
		return (cached_phc_time - delta);
	}

	return (cached_phc_time + delta);
}

/**
 * idpf_ptp_reg_addr - map a control-plane register offset without panicking
 * @adapter: driver private data
 * @reg_offset: offset reported by the control plane
 *
 * Returns NULL when the offset was not reported or falls outside every mapped
 * BAR region.
 */
static void *
idpf_ptp_reg_addr(struct idpf_adapter *adapter, u32 reg_offset)
{
	struct idpf_hw *hw = &adapter->hw;
	int i;

	if (reg_offset == IDPF_PTP_REG_INVALID)
		return (NULL);

	for (i = 0; i < hw->num_lan_regs; i++) {
		struct idpf_mmio_reg *region = &hw->lan_regs[i];

		if (idpf_reg_offset_in_region(region, reg_offset))
			return ((u8 *)region->vaddr +
			    (reg_offset - region->addr_start));
	}

	return (NULL);
}

/**
 * idpf_ptp_rd64_split - read a 64-bit value from a low/high register pair
 * @adapter: driver private data
 * @lo: low half offset
 * @hi: high half offset
 * @val: result in host byte order
 *
 * Re-reads until the high half is stable so the halves cannot straddle a
 * carry.  Returns EIO if either offset is unmapped.
 */
static int
idpf_ptp_rd64_split(struct idpf_adapter *adapter, u32 lo, u32 hi,
    u64 *val)
{
	void *lo_addr, *hi_addr;
	u32 hi1, hi2, lo32;
	int retry;

	lo_addr = idpf_ptp_reg_addr(adapter, lo);
	hi_addr = idpf_ptp_reg_addr(adapter, hi);
	if (lo_addr == NULL || hi_addr == NULL)
		return (EIO);

	hi2 = idpf_reg_rd32(hi_addr);
	for (retry = 0; retry < IDPF_PTP_CMD_SYNC_RETRIES; retry++) {
		hi1 = hi2;
		lo32 = idpf_reg_rd32(lo_addr);
		hi2 = idpf_reg_rd32(hi_addr);
		if (hi1 == hi2) {
			*val = ((u64)hi2 << 32) | lo32;
			return (0);
		}
	}

	return (EIO);
}

/**
 * idpf_ptp_send_msg - issue one synchronous PTP virtchnl transaction
 * @adapter: driver private data
 * @op: VIRTCHNL2_OP_PTP_* opcode
 * @req: request payload, may be NULL
 * @req_len: request length
 * @rsp: response buffer
 * @rsp_len: response buffer size
 * @min_len: shortest reply the caller can parse
 *
 * Returns 0 on success or a positive errno.
 */
static int
idpf_ptp_send_msg(struct idpf_adapter *adapter, u32 op, void *req,
    size_t req_len, void *rsp, size_t rsp_len, size_t min_len,
    size_t *reply_len)
{
	struct idpf_vc_xn_params xn_params = { 0 };
	ssize_t reply_sz;

	xn_params.vc_op = op;
	xn_params.send_buf.iov_base = req;
	xn_params.send_buf.iov_len = req_len;
	xn_params.recv_buf.iov_base = rsp;
	xn_params.recv_buf.iov_len = rsp_len;
	xn_params.timeout_ms = idpf_get_vc_xn_default_timeout(adapter);

	reply_sz = idpf_vc_xn_exec(adapter, &xn_params);
	if (reply_sz < 0)
		return (-reply_sz);
	if ((size_t)reply_sz < min_len)
		return (EIO);

	if (reply_len != NULL)
		*reply_len = (size_t)reply_sz;

	return (0);
}

/**
 * idpf_ptp_get_caps - negotiate PTP capabilities with the control plane
 * @adapter: driver private data
 */
static int
idpf_ptp_get_caps(struct idpf_adapter *adapter)
{
	struct virtchnl2_ptp_get_caps req = { 0 }, rsp = { 0 };
	struct idpf_ptp *ptp = adapter->ptp;
	int err;

	/* The control plane only reports capabilities the driver asks for. */
	req.caps = htole32(VIRTCHNL2_CAP_PTP_GET_DEVICE_CLK_TIME |
	    VIRTCHNL2_CAP_PTP_GET_DEVICE_CLK_TIME_MB |
	    VIRTCHNL2_CAP_PTP_GET_CROSS_TIME |
	    VIRTCHNL2_CAP_PTP_GET_CROSS_TIME_MB |
	    VIRTCHNL2_CAP_PTP_TX_TSTAMPS |
	    VIRTCHNL2_CAP_PTP_TX_TSTAMPS_MB);

	err = idpf_ptp_send_msg(adapter, VIRTCHNL2_OP_PTP_GET_CAPS, &req,
	    sizeof(req), &rsp, sizeof(rsp), sizeof(rsp), NULL);
	if (err != 0)
		return (err);

	ptp->caps = le32toh(rsp.caps);
	ptp->max_adj = le32toh(rsp.max_adj);
	ptp->base_incval = le64toh(rsp.base_incval);

	ptp->secondary_mbx.peer_mbx_q_id = le16toh(rsp.peer_mbx_q_id);
	ptp->secondary_mbx.peer_id = rsp.peer_id;
	ptp->secondary_mbx.mbx_q_index = rsp.mbx_q_index;
	/*
	 * A secondary mailbox needs both a queue index and a real peer id;
	 * the control plane reports 0xffff when it stays on the primary.
	 */
	ptp->secondary_mbx.valid = (rsp.mbx_q_index != 0 &&
	    ptp->secondary_mbx.peer_mbx_q_id != 0xffff);

	ptp->dev_clk_regs.dev_clk_ns_l = le32toh(rsp.clk_offsets.dev_clk_ns_l);
	ptp->dev_clk_regs.dev_clk_ns_h = le32toh(rsp.clk_offsets.dev_clk_ns_h);
	ptp->dev_clk_regs.phy_clk_ns_l = le32toh(rsp.clk_offsets.phy_clk_ns_l);
	ptp->dev_clk_regs.phy_clk_ns_h = le32toh(rsp.clk_offsets.phy_clk_ns_h);
	ptp->dev_clk_regs.cmd_sync =
	    le32toh(rsp.clk_offsets.cmd_sync_trigger);

	ptp->dev_clk_regs.sys_time_ns_l =
	    le32toh(rsp.cross_time_offsets.sys_time_ns_l);
	ptp->dev_clk_regs.sys_time_ns_h =
	    le32toh(rsp.cross_time_offsets.sys_time_ns_h);

	ptp->dev_clk_regs.cmd = le32toh(rsp.clk_adj_offsets.dev_clk_cmd_type);
	ptp->dev_clk_regs.incval_l =
	    le32toh(rsp.clk_adj_offsets.dev_clk_incval_l);
	ptp->dev_clk_regs.incval_h =
	    le32toh(rsp.clk_adj_offsets.dev_clk_incval_h);
	ptp->dev_clk_regs.shadj_l =
	    le32toh(rsp.clk_adj_offsets.dev_clk_shadj_l);
	ptp->dev_clk_regs.shadj_h =
	    le32toh(rsp.clk_adj_offsets.dev_clk_shadj_h);
	ptp->dev_clk_regs.phy_cmd =
	    le32toh(rsp.clk_adj_offsets.phy_clk_cmd_type);
	ptp->dev_clk_regs.phy_incval_l =
	    le32toh(rsp.clk_adj_offsets.phy_clk_incval_l);
	ptp->dev_clk_regs.phy_incval_h =
	    le32toh(rsp.clk_adj_offsets.phy_clk_incval_h);
	ptp->dev_clk_regs.phy_shadj_l =
	    le32toh(rsp.clk_adj_offsets.phy_clk_shadj_l);
	ptp->dev_clk_regs.phy_shadj_h =
	    le32toh(rsp.clk_adj_offsets.phy_clk_shadj_h);

	return (0);
}

/**
 * idpf_ptp_classify - pick the access method for one capability pair
 * @caps: negotiated capability word
 * @direct: direct-access capability bit
 * @mailbox: mailbox-access capability bit
 * @have_regs: whether the registers the direct path needs were reported
 */
static u8
idpf_ptp_classify(u32 caps, u32 direct, u32 mailbox,
    bool have_regs)
{
	if ((caps & direct) != 0 && have_regs)
		return (IDPF_PTP_DIRECT);
	if ((caps & mailbox) != 0)
		return (IDPF_PTP_MAILBOX);

	return (IDPF_PTP_NONE);
}

/**
 * idpf_ptp_get_features_access - record how each PTP feature is reached
 * @adapter: driver private data
 *
 * The direct path is only selected when the control plane also supplied the
 * register offsets it needs.
 */
static void
idpf_ptp_get_features_access(struct idpf_adapter *adapter)
{
	struct idpf_ptp *ptp = adapter->ptp;
	struct idpf_ptp_dev_clk_regs *r = &ptp->dev_clk_regs;

	ptp->get_dev_clk_time_access = idpf_ptp_classify(ptp->caps,
	    VIRTCHNL2_CAP_PTP_GET_DEVICE_CLK_TIME,
	    VIRTCHNL2_CAP_PTP_GET_DEVICE_CLK_TIME_MB,
	    r->dev_clk_ns_l != IDPF_PTP_REG_INVALID &&
	    r->dev_clk_ns_h != IDPF_PTP_REG_INVALID);

	ptp->get_cross_tstamp_access = idpf_ptp_classify(ptp->caps,
	    VIRTCHNL2_CAP_PTP_GET_CROSS_TIME,
	    VIRTCHNL2_CAP_PTP_GET_CROSS_TIME_MB,
	    r->sys_time_ns_l != IDPF_PTP_REG_INVALID &&
	    r->sys_time_ns_h != IDPF_PTP_REG_INVALID);

	ptp->tx_tstamp_access = idpf_ptp_classify(ptp->caps,
	    VIRTCHNL2_CAP_PTP_TX_TSTAMPS,
	    VIRTCHNL2_CAP_PTP_TX_TSTAMPS_MB, true);
}

/**
 * idpf_ptp_read_dev_clk_direct - read the main timer through MMIO
 * @adapter: driver private data
 * @dev_clk_time: result
 *
 * Caller holds read_dev_clk_lock.
 */
static int
idpf_ptp_read_dev_clk_direct(struct idpf_adapter *adapter,
    struct idpf_ptp_dev_timers *dev_clk_time)
{
	struct idpf_ptp *ptp = adapter->ptp;
	void *sync_addr;
	int err;

	/* Latch a coherent snapshot before reading the halves. */
	sync_addr = idpf_ptp_reg_addr(adapter, ptp->dev_clk_regs.cmd_sync);
	if (sync_addr != NULL) {
		idpf_reg_wr32(sync_addr, ptp->cmd.shtime_enable_mask);
		idpf_reg_wr32(sync_addr,
		    ptp->cmd.exec_cmd_mask | ptp->cmd.shtime_enable_mask);
	}

	err = idpf_ptp_rd64_split(adapter, ptp->dev_clk_regs.dev_clk_ns_l,
	    ptp->dev_clk_regs.dev_clk_ns_h, &dev_clk_time->dev_clk_time_ns);
	if (err != 0)
		return (err);

	dev_clk_time->sys_time_ns = 0;
	if (ptp->get_cross_tstamp_access == IDPF_PTP_DIRECT) {
		/* Cross timestamp is advisory; ignore a read failure. */
		(void)idpf_ptp_rd64_split(adapter,
		    ptp->dev_clk_regs.sys_time_ns_l,
		    ptp->dev_clk_regs.sys_time_ns_h,
		    &dev_clk_time->sys_time_ns);
	}

	ptp->cached_phc_time = dev_clk_time->dev_clk_time_ns;
	ptp->cached_phc_ticks = ticks;

	return (0);
}

/**
 * idpf_ptp_read_dev_clk_mbx - read the main timer over the mailbox
 * @adapter: driver private data
 * @dev_clk_time: result
 */
static int
idpf_ptp_read_dev_clk_mbx(struct idpf_adapter *adapter,
    struct idpf_ptp_dev_timers *dev_clk_time)
{
	struct virtchnl2_ptp_get_dev_clk_time rsp = { 0 }, req = { 0 };
	struct idpf_ptp *ptp = adapter->ptp;
	int err;

	/* Distinct buffers: the reply is written while the request is read. */
	err = idpf_ptp_send_msg(adapter, VIRTCHNL2_OP_PTP_GET_DEV_CLK_TIME,
	    &req, sizeof(req), &rsp, sizeof(rsp), sizeof(rsp), NULL);
	if (err != 0)
		return (err);

	dev_clk_time->dev_clk_time_ns = le64toh(rsp.dev_time_ns);
	dev_clk_time->sys_time_ns = 0;

	ptp->cached_phc_time = dev_clk_time->dev_clk_time_ns;
	ptp->cached_phc_ticks = ticks;

	return (0);
}

/**
 * idpf_ptp_get_dev_clk_time - read the device clock
 * @adapter: driver private data
 * @dev_clk_time: result
 *
 * Returns 0, EOPNOTSUPP when the clock is not reachable, or an errno.
 */
int
idpf_ptp_get_dev_clk_time(struct idpf_adapter *adapter,
    struct idpf_ptp_dev_timers *dev_clk_time)
{
	struct idpf_ptp *ptp;
	int err;

	if (adapter == NULL || adapter->ptp == NULL || dev_clk_time == NULL)
		return (EINVAL);

	ptp = adapter->ptp;
	sx_xlock(&ptp->read_dev_clk_lock);
	switch (ptp->get_dev_clk_time_access) {
	case IDPF_PTP_DIRECT:
		err = idpf_ptp_read_dev_clk_direct(adapter, dev_clk_time);
		break;
	case IDPF_PTP_MAILBOX:
		err = idpf_ptp_read_dev_clk_mbx(adapter, dev_clk_time);
		break;
	default:
		err = EOPNOTSUPP;
		break;
	}
	sx_xunlock(&ptp->read_dev_clk_lock);

	return (err);
}

/**
 * idpf_ptp_release_vport_tstamps_caps - free per-vport TX timestamp state
 * @vport: vport being torn down
 */
void
idpf_ptp_release_vport_tstamps_caps(struct idpf_vport *vport)
{
	struct idpf_ptp_vport_tx_tstamp_caps *caps;

	if (vport == NULL || vport->tx_tstamp_caps == NULL)
		return;

	caps = vport->tx_tstamp_caps;
	vport->tx_tstamp_caps = NULL;

	if (caps->latches != NULL)
		free(caps->latches, M_DEVBUF);
	free(caps, M_DEVBUF);
}

/**
 * idpf_ptp_get_vport_tstamps_caps - negotiate TX timestamp latches
 * @vport: vport to negotiate for
 *
 * Returns 0, or EOPNOTSUPP when the control plane offers no TX timestamping
 * for this vport, which the caller treats as non-fatal.
 */
int
idpf_ptp_get_vport_tstamps_caps(struct idpf_vport *vport)
{
	struct virtchnl2_ptp_get_vport_tx_tstamp_caps *rsp, req = { 0 };
	struct idpf_ptp_vport_tx_tstamp_caps *caps;
	struct idpf_adapter *adapter;
	size_t rsp_len, reply_len;
	u16 i, num_latches;
	int err;

	if (vport == NULL || vport->adapter == NULL)
		return (EINVAL);

	adapter = vport->adapter;
	if (adapter->ptp == NULL)
		return (EOPNOTSUPP);
	/* Extending a TX timestamp needs the device clock, so require both. */
	if (adapter->ptp->tx_tstamp_access == IDPF_PTP_NONE ||
	    adapter->ptp->get_dev_clk_time_access == IDPF_PTP_NONE)
		return (EOPNOTSUPP);

	rsp_len = IDPF_CTLQ_MAX_BUF_LEN;
	rsp = malloc(rsp_len, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rsp == NULL)
		return (ENOMEM);

	req.vport_id = htole32(vport->vport_id);
	err = idpf_ptp_send_msg(adapter,
	    VIRTCHNL2_OP_PTP_GET_VPORT_TX_TSTAMP_CAPS, &req, sizeof(req),
	    rsp, rsp_len, sizeof(*rsp), &reply_len);
	if (err != 0)
		goto out;

	num_latches = le16toh(rsp->num_latches);
	if (num_latches == 0) {
		err = EOPNOTSUPP;
		goto out;
	}
	if (num_latches > IDPF_PTP_MAX_TX_TSTAMP_LATCHES) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "PTP: control plane declares %u TX latches, max %u\n",
		    num_latches, IDPF_PTP_MAX_TX_TSTAMP_LATCHES);
		err = EIO;
		goto out;
	}
	if (le32toh(rsp->vport_id) != vport->vport_id) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "PTP: TX latch caps reply is for vport %u, expected %u\n",
		    le32toh(rsp->vport_id), vport->vport_id);
		err = EIO;
		goto out;
	}
	/* The bits index a 64-bit timestamp and are used as shift counts. */
	if (rsp->tstamp_ns_lo_bit >= rsp->tstamp_ns_hi_bit ||
	    rsp->tstamp_ns_hi_bit > 63) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "PTP: invalid timestamp bit range %u..%u\n",
		    rsp->tstamp_ns_lo_bit, rsp->tstamp_ns_hi_bit);
		err = EIO;
		goto out;
	}
	/* The reply must be exactly as long as the latch count it declares. */
	if (reply_len != struct_size_t(struct virtchnl2_ptp_get_vport_tx_tstamp_caps,
	    tstamp_latches, num_latches)) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "PTP: TX latch caps reply is %zu bytes but declares %u "
		    "latches\n", reply_len, num_latches);
		err = EIO;
		goto out;
	}

	caps = malloc(sizeof(*caps), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (caps == NULL) {
		err = ENOMEM;
		goto out;
	}

	caps->latches = malloc(num_latches * sizeof(*caps->latches), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (caps->latches == NULL) {
		free(caps, M_DEVBUF);
		err = ENOMEM;
		goto out;
	}

	caps->vport_id = le32toh(rsp->vport_id);
	caps->num_entries = num_latches;
	caps->tstamp_ns_lo_bit = rsp->tstamp_ns_lo_bit;
	caps->tstamp_ns_hi_bit = rsp->tstamp_ns_hi_bit;
	caps->readiness_offset_l = le32toh(rsp->readiness_offset_l);
	caps->readiness_offset_h = le32toh(rsp->readiness_offset_h);
	caps->access = adapter->ptp->tx_tstamp_access;

	for (i = 0; i < num_latches; i++) {
		caps->latches[i].idx = rsp->tstamp_latches[i].index;
		caps->latches[i].tx_latch_reg_offset_l =
		    le32toh(rsp->tstamp_latches[i].tx_latch_reg_offset_l);
		caps->latches[i].tx_latch_reg_offset_h =
		    le32toh(rsp->tstamp_latches[i].tx_latch_reg_offset_h);
	}

	idpf_ptp_release_vport_tstamps_caps(vport);
	vport->tx_tstamp_caps = caps;

out:
	free(rsp, M_DEVBUF);
	return (err);
}

/**
 * idpf_ptp_is_vport_tx_tstamp_ena - whether TX timestamping was negotiated
 * @vport: vport to test
 */
bool
idpf_ptp_is_vport_tx_tstamp_ena(struct idpf_vport *vport)
{
	return (vport != NULL && vport->tx_tstamp_caps != NULL);
}

/**
 * idpf_ptp_is_vport_rx_tstamp_ena - whether RX timestamps can be interpreted
 * @vport: vport to test
 *
 * RX timestamps ride in the descriptor, so they are usable whenever the
 * device clock can be read to extend them.
 */
bool
idpf_ptp_is_vport_rx_tstamp_ena(struct idpf_vport *vport)
{
	if (vport == NULL || vport->adapter == NULL ||
	    vport->adapter->ptp == NULL)
		return (false);

	return (vport->adapter->ptp->get_dev_clk_time_access != IDPF_PTP_NONE);
}

/**
 * idpf_ptp_get_tstamp_config - report the negotiated timestamping state
 * @vport: vport to report on
 * @cfg: caller-supplied kernel buffer to fill
 *
 * capable is false when the control plane never offered PTP, which is what
 * distinguishes "no PTP on this device" from "PTP present but disabled".
 */
void
idpf_ptp_get_tstamp_config(struct idpf_vport *vport,
    struct idpf_tstamp_config *cfg)
{

	memset(cfg, 0, sizeof(*cfg));

	if (vport == NULL || vport->adapter == NULL ||
	    vport->adapter->ptp == NULL)
		return;

	cfg->capable = 1;
	cfg->tx_type = idpf_ptp_is_vport_tx_tstamp_ena(vport) ? 1 : 0;
	cfg->rx_filter = idpf_ptp_is_vport_rx_tstamp_ena(vport) ? 1 : 0;
}

/**
 * idpf_ptp_access_str - name an access method for reporting
 * @access: enum idpf_ptp_access value
 */
static const char *
idpf_ptp_access_str(u8 access)
{
	switch (access) {
	case IDPF_PTP_DIRECT:
		return ("direct");
	case IDPF_PTP_MAILBOX:
		return ("mailbox");
	default:
		return ("unavailable");
	}
}

/**
 * idpf_ptp_init - allocate PTP state and negotiate capabilities
 * @adapter: driver private data
 *
 * Returns 0, or EOPNOTSUPP when the device does not offer PTP.  The caller
 * treats any failure as non-fatal.
 */
int
idpf_ptp_init(struct idpf_adapter *adapter)
{
	struct idpf_ptp *ptp;
	int err;

	if (adapter == NULL)
		return (EINVAL);
	if (adapter->ptp != NULL)
		return (0);

	if (!idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_PTP)) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "PTP: not offered by the control plane (other_caps 0x%jx)\n",
		    (uintmax_t)le64toh(adapter->caps.other_caps));
		return (EOPNOTSUPP);
	}

	ptp = malloc(sizeof(*ptp), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (ptp == NULL)
		return (ENOMEM);

	ptp->adapter = adapter;
	/* Force the first cached-PHC consumer to refresh. */
	ptp->cached_phc_ticks = ticks - (int)IDPF_PTP_PHC_CACHE_TICKS - 1;
	sx_init(&ptp->read_dev_clk_lock, "idpf_ptp_clk");
	adapter->ptp = ptp;

	err = idpf_ptp_get_caps(adapter);
	if (err != 0) {
		sx_destroy(&ptp->read_dev_clk_lock);
		free(ptp, M_DEVBUF);
		adapter->ptp = NULL;
		return (err);
	}

	/* Register masks come from the device layer before access classing. */
	if (adapter->dev_ops.reg_ops.ptp_reg_init != NULL)
		adapter->dev_ops.reg_ops.ptp_reg_init(adapter);

	idpf_ptp_get_features_access(adapter);

	device_printf(idpf_adapter_to_dev(adapter),
	    "PTP: caps 0x%x, device clock %s, TX timestamp %s\n", ptp->caps,
	    idpf_ptp_access_str(ptp->get_dev_clk_time_access),
	    idpf_ptp_access_str(ptp->tx_tstamp_access));

	return (0);
}

/**
 * idpf_ptp_sysctl_clock - read the device clock through sysctl
 *
 * FreeBSD has no SO_TIMESTAMPING and iflib's if_rxd_info carries no timestamp
 * field, so sysctl is the only way to hand the PHC to userspace.
 */
static int
idpf_ptp_sysctl_clock(SYSCTL_HANDLER_ARGS)
{
	struct idpf_adapter *adapter = arg1;
	struct idpf_ptp_dev_timers timers;
	u64 ns;
	int err;

	err = idpf_ptp_get_dev_clk_time(adapter, &timers);
	if (err != 0)
		return (err);

	ns = timers.dev_clk_time_ns;

	return (sysctl_handle_64(oidp, &ns, 0, req));
}

/**
 * idpf_ptp_sysctl_init - publish the PTP clock node
 * @adapter: driver private data
 *
 * Does nothing when PTP was not negotiated.
 */
void
idpf_ptp_sysctl_init(struct idpf_adapter *adapter)
{
	device_t dev;

	if (adapter == NULL || adapter->ptp == NULL)
		return;
	if (adapter->ptp->get_dev_clk_time_access == IDPF_PTP_NONE)
		return;

	dev = idpf_adapter_to_dev(adapter);
	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	    "ptp_clock_ns", CTLTYPE_U64 | CTLFLAG_RD | CTLFLAG_MPSAFE,
	    adapter, 0, idpf_ptp_sysctl_clock, "QU",
	    "PTP device clock in nanoseconds");
}

/**
 * idpf_ptp_release - tear down PTP state
 * @adapter: driver private data
 */
void
idpf_ptp_release(struct idpf_adapter *adapter)
{
	struct idpf_ptp *ptp;

	if (adapter == NULL || adapter->ptp == NULL)
		return;

	ptp = adapter->ptp;
	adapter->ptp = NULL;

	sx_destroy(&ptp->read_dev_clk_lock);
	free(ptp, M_DEVBUF);
}
