/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * PF register map and device operations.
 *
 * FreeBSD port notes
 * ------------------
 * MMIO model.  Linux ioremaps each BAR0 window separately and keeps the
 * geometry of the two static windows in dev_ops.static_reg_info[].  FreeBSD's
 * resource manager will not hand out the same PCI BAR twice, so BAR0 is
 * mapped once during attach and every window is a host-virtual offset into
 * that single mapping.  idpf_dev_ops_init() therefore publishes the mailbox
 * and reset-status window geometry directly into struct idpf_hw, which is
 * what idpf_get_mbx_reg_addr() and idpf_get_rstat_reg_addr() consume.
 * [FBSD15:A30-A31]
 *
 * Register access.  readl()/writel() become idpf_reg_rd32()/idpf_reg_wr32(),
 * the MMIO seam declared in idpf_txrx.h.
 *
 * Removed features.  The VFIO/mdev VDCM hooks and the IDC auxiliary-bus
 * registration have no FreeBSD counterpart and are not part of this port, so
 * idpf_idc_register() and the vdcm_init/vdcm_deinit assignments are gone.
 * notify_adi_reset is left NULL; it is populated only when ADI support is
 * negotiated.  [LOCAL:A22]
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>

#include "idpf.h"
#include "idpf_lan_pf_regs.h"
#include "idpf_virtchnl.h"
#include "idpf_ptp.h"

/*
 * The LAN driver does not own all of BAR0.  These bounds describe the hole
 * between the two PF regions that idpf_calc_remaining_mmio_regs() maps
 * around.
 */
#define IDPF_PF_BAR0_REGION1_END	0xC001000	/* 192MB + 4KB */
#define IDPF_PF_BAR0_REGION2_START	0x10000000	/* 256MB */

#define IDPF_PF_ITR_IDX_SPACING		0x4

/**
 * idpf_ctlq_reg_init - initialise the default mailbox registers
 * @adapter: driver private data
 * @cq: array of control queues to describe
 *
 * The offsets are made relative to the start of the mailbox window because
 * the control queue code reaches them through idpf_get_mbx_reg_addr().
 */
static void
idpf_ctlq_reg_init(struct idpf_adapter *adapter,
    struct idpf_ctlq_create_info *cq)
{
	bus_size_t mbx_start = adapter->hw.mbx.addr_start;
	int i;

	for (i = 0; i < IDPF_NUM_DFLT_MBX_Q; i++) {
		struct idpf_ctlq_create_info *ccq = cq + i;

		switch (ccq->type) {
		case IDPF_CTLQ_TYPE_MAILBOX_TX:
			ccq->reg.head = PF_FW_ATQH - mbx_start;
			ccq->reg.tail = PF_FW_ATQT - mbx_start;
			ccq->reg.len = PF_FW_ATQLEN - mbx_start;
			ccq->reg.bah = PF_FW_ATQBAH - mbx_start;
			ccq->reg.bal = PF_FW_ATQBAL - mbx_start;
			ccq->reg.len_mask = PF_FW_ATQLEN_ATQLEN_M;
			ccq->reg.len_ena_mask = PF_FW_ATQLEN_ATQENABLE_M;
			ccq->reg.head_mask = PF_FW_ATQH_ATQH_M;
			break;
		case IDPF_CTLQ_TYPE_MAILBOX_RX:
			ccq->reg.head = PF_FW_ARQH - mbx_start;
			ccq->reg.tail = PF_FW_ARQT - mbx_start;
			ccq->reg.len = PF_FW_ARQLEN - mbx_start;
			ccq->reg.bah = PF_FW_ARQBAH - mbx_start;
			ccq->reg.bal = PF_FW_ARQBAL - mbx_start;
			ccq->reg.len_mask = PF_FW_ARQLEN_ARQLEN_M;
			ccq->reg.len_ena_mask = PF_FW_ARQLEN_ARQENABLE_M;
			ccq->reg.head_mask = PF_FW_ARQH_ARQH_M;
			break;
		default:
			break;
		}
	}
}

/**
 * idpf_mb_intr_reg_init - initialise the mailbox interrupt registers
 * @adapter: driver private data
 */
static void
idpf_mb_intr_reg_init(struct idpf_adapter *adapter)
{
	struct idpf_intr_reg *intr = &adapter->mb_vector.intr_reg;
	uint32_t dyn_ctl = le32toh(adapter->caps.mailbox_dyn_ctl);

	intr->dyn_ctl = idpf_get_reg_addr(adapter, dyn_ctl);
	intr->dyn_ctl_intena_m = PF_GLINT_DYN_CTL_INTENA_M;
	intr->dyn_ctl_itridx_m = PF_GLINT_DYN_CTL_ITR_INDX_M;
	intr->icr_ena = idpf_get_reg_addr(adapter, PF_INT_DIR_OICR_ENA);
	intr->icr_ena_ctlq_m = PF_INT_DIR_OICR_ENA_M;
}

/**
 * idpf_intr_reg_init - initialise the data queue interrupt registers
 * @vport: vport owning the vectors
 * @rsrc: queue and vector resources
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_intr_reg_init(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_adapter *adapter = vport->adapter;
	int num_vecs = rsrc->num_q_vectors;
	struct idpf_vec_regs *reg_vals;
	int num_regs, i, err = 0;
	uint32_t rx_itr, tx_itr;
	uint16_t total_vecs;

	total_vecs = idpf_get_reserved_vecs(adapter);
	reg_vals = malloc(total_vecs * sizeof(*reg_vals), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (reg_vals == NULL)
		return (ENOMEM);

	num_regs = idpf_get_reg_intr_vecs(adapter, reg_vals);
	if (num_regs < num_vecs) {
		err = EINVAL;
		goto free_reg_vals;
	}

	for (i = 0; i < num_vecs; i++) {
		struct idpf_q_vector *q_vector = &rsrc->q_vectors[i];
		uint16_t vec_id = rsrc->q_vector_idxs[i] - IDPF_MBX_Q_VEC;
		struct idpf_intr_reg *intr = &q_vector->intr_reg;
		uint32_t spacing;

		intr->dyn_ctl = idpf_get_reg_addr(adapter,
		    reg_vals[vec_id].dyn_ctl_reg);
		intr->dyn_ctl_intena_m = PF_GLINT_DYN_CTL_INTENA_M;
		intr->dyn_ctl_intena_msk_m = PF_GLINT_DYN_CTL_INTENA_MSK_M;
		intr->dyn_ctl_itridx_s = PF_GLINT_DYN_CTL_ITR_INDX_S;
		intr->dyn_ctl_intrvl_s = PF_GLINT_DYN_CTL_INTERVAL_S;
		intr->dyn_ctl_wb_on_itr_m = PF_GLINT_DYN_CTL_WB_ON_ITR_M;
		intr->dyn_ctl_itridx_m = PF_GLINT_DYN_CTL_ITR_INDX_M;
		intr->dyn_ctl_swint_trig_m = PF_GLINT_DYN_CTL_SWINT_TRIG_M;
		intr->dyn_ctl_sw_itridx_ena_m =
		    PF_GLINT_DYN_CTL_SW_ITR_INDX_ENA_M;

		spacing = IDPF_ITR_IDX_SPACING(
		    reg_vals[vec_id].itrn_index_spacing,
		    IDPF_PF_ITR_IDX_SPACING);
		rx_itr = PF_GLINT_ITR_ADDR(VIRTCHNL2_ITR_IDX_0,
		    reg_vals[vec_id].itrn_reg, spacing);
		tx_itr = PF_GLINT_ITR_ADDR(VIRTCHNL2_ITR_IDX_1,
		    reg_vals[vec_id].itrn_reg, spacing);
		intr->rx_itr = idpf_get_reg_addr(adapter, rx_itr);
		intr->tx_itr = idpf_get_reg_addr(adapter, tx_itr);
	}

free_reg_vals:
	free(reg_vals, M_DEVBUF);

	return (err);
}

/**
 * idpf_reset_reg_init - initialise the reset status register
 * @adapter: driver private data
 */
static void
idpf_reset_reg_init(struct idpf_adapter *adapter)
{

	adapter->reset_reg.rstat = idpf_get_rstat_reg_addr(adapter,
	    PFGEN_RSTAT);
	adapter->reset_reg.rstat_m = PFGEN_RSTAT_PFR_STATE_M;
}

/**
 * idpf_oicr_reset_reg_init - initialise the OICR cause register
 * @adapter: driver private data
 */
static void
idpf_oicr_reset_reg_init(struct idpf_adapter *adapter)
{

	adapter->reset_reg.oicr_cause = idpf_get_reg_addr(adapter,
	    PF_INT_DIR_OICR_CAUSE);
	adapter->reset_reg.oicr_cause_m = PF_INT_DIR_OICR_CAUSE_CAUSE_M;
}

/**
 * idpf_trigger_reset - request a PF software reset
 * @adapter: driver private data
 * @trig_cause: reason the reset was requested
 *
 * The PF only supports a software reset, so the cause is recorded by the
 * caller and not consulted here.
 */
static void
idpf_trigger_reset(struct idpf_adapter *adapter,
    enum idpf_flags trig_cause __unused)
{
	uint32_t reset_reg;

	reset_reg = idpf_reg_rd32(idpf_get_rstat_reg_addr(adapter, PFGEN_CTRL));
	idpf_reg_wr32(idpf_get_rstat_reg_addr(adapter, PFGEN_CTRL),
	    reset_reg | PFGEN_CTRL_PFSWR);
}

/**
 * idpf_read_master_time_ns - read the device master time
 * @hw: hardware struct
 *
 * The shadow time latch and the execute command must be written as two
 * separate transactions, in that order, per the hardware specification.
 *
 * Return: the master time in nanoseconds.
 */
static uint64_t
idpf_read_master_time_ns(const struct idpf_hw *hw)
{
	struct idpf_adapter *adapter = hw->back;
	uint32_t ts_lo, ts_hi;
	uint64_t ns_time;

	idpf_reg_wr32(idpf_get_reg_addr(adapter, PF_GLTSYN_CMD_SYNC),
	    PF_GLTSYN_CMD_SYNC_SHTIME_EN_M);
	idpf_reg_wr32(idpf_get_reg_addr(adapter, PF_GLTSYN_CMD_SYNC),
	    PF_GLTSYN_CMD_SYNC_EXEC_CMD_M | PF_GLTSYN_CMD_SYNC_SHTIME_EN_M);

	ts_lo = idpf_reg_rd32(idpf_get_reg_addr(adapter, PF_GLTSYN_SHTIME_L));
	ts_hi = idpf_reg_rd32(idpf_get_reg_addr(adapter, PF_GLTSYN_SHTIME_H));

	ns_time = (uint64_t)ts_hi << 32;
	ns_time |= (uint64_t)ts_lo;

	return (ns_time);
}

/**
 * idpf_ptp_reg_init - record the PTP command register masks
 * @adapter: driver private data
 */
static void
idpf_ptp_reg_init(const struct idpf_adapter *adapter)
{

	adapter->ptp->cmd.shtime_enable_mask = PF_GLTSYN_CMD_SYNC_SHTIME_EN_M;
	adapter->ptp->cmd.exec_cmd_mask = PF_GLTSYN_CMD_SYNC_EXEC_CMD_M;
}

/**
 * idpf_reg_ops_init - install the PF register operations
 * @adapter: driver private data
 */
static void
idpf_reg_ops_init(struct idpf_adapter *adapter)
{

	adapter->dev_ops.reg_ops.ctlq_reg_init = idpf_ctlq_reg_init;
	adapter->dev_ops.reg_ops.intr_reg_init = idpf_intr_reg_init;
	adapter->dev_ops.reg_ops.mb_intr_reg_init = idpf_mb_intr_reg_init;
	adapter->dev_ops.reg_ops.reset_reg_init = idpf_reset_reg_init;
	adapter->dev_ops.reg_ops.oicr_reset_reg_init = idpf_oicr_reset_reg_init;
	adapter->dev_ops.reg_ops.trigger_reset = idpf_trigger_reset;
	adapter->dev_ops.reg_ops.read_master_time = idpf_read_master_time_ns;
	adapter->dev_ops.reg_ops.ptp_reg_init = idpf_ptp_reg_init;
}

/**
 * idpf_dev_ops_init - install the PF device operations
 * @adapter: driver private data
 *
 * Publishes the geometry of the two static BAR0 windows.  Attach fills each
 * window's vaddr in once BAR0 itself is mapped.
 */
void
idpf_dev_ops_init(struct idpf_adapter *adapter)
{
	struct idpf_hw *hw = &adapter->hw;

	idpf_reg_ops_init(adapter);

	hw->mbx.addr_start = PF_FW_BASE;
	hw->mbx.addr_len = IDPF_PF_MBX_REGION_SZ;
	hw->rstat.addr_start = PFGEN_RTRIG;
	hw->rstat.addr_len = IDPF_PF_RSTAT_REGION_SZ;
}
