/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * VF register map and device operations.
 *
 * FreeBSD port notes: the same three changes as the PF side in idpf_dev.c.
 * readl()/writel() become the idpf_reg_rd32()/idpf_reg_wr32() MMIO seam,
 * kcalloc()/kfree() become malloc()/free() on M_DEVBUF, and the static BAR0
 * window geometry is published straight into struct idpf_hw because FreeBSD
 * maps BAR0 once rather than ioremapping each window.  The IDC registration
 * hook is gone with the rest of the auxiliary-bus support.
 * [FBSD15:A30-A31] [LOCAL:A22]
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>

#include <machine/bus.h>

#include <dev/pci/pcivar.h>

#include "idpf.h"
#include "idpf_lan_vf_regs.h"
#include "idpf_virtchnl.h"

#define IDPF_VF_ITR_IDX_SPACING		0x40

/**
 * idpf_vf_is_siov - whether this function is a Scalable IOV VF
 * @adapter: driver private data
 */
static bool
idpf_vf_is_siov(struct idpf_adapter *adapter)
{

	return (pci_get_device(idpf_adapter_to_dev(adapter)) ==
	    IDPF_DEV_ID_VF_SIOV);
}

/**
 * idpf_vf_ctlq_reg_init - initialise the default mailbox registers
 * @adapter: driver private data
 * @cq: array of control queues to describe
 *
 * Offsets are made relative to the mailbox window because the control queue
 * code reaches them through idpf_get_mbx_reg_addr().
 */
static void
idpf_vf_ctlq_reg_init(struct idpf_adapter *adapter,
    struct idpf_ctlq_create_info *cq)
{
	bus_size_t mbx_start = adapter->hw.mbx.addr_start;
	bool siov = idpf_vf_is_siov(adapter);
	int i;

	for (i = 0; i < IDPF_NUM_DFLT_MBX_Q; i++) {
		struct idpf_ctlq_create_info *ccq = cq + i;

		switch (ccq->type) {
		case IDPF_CTLQ_TYPE_MAILBOX_TX:
			if (siov) {
				ccq->reg.head = VDEV_MBX_ATQH - mbx_start;
				ccq->reg.tail = VDEV_MBX_ATQT - mbx_start;
				ccq->reg.len = VDEV_MBX_ATQLEN - mbx_start;
				ccq->reg.bah = VDEV_MBX_ATQBAH - mbx_start;
				ccq->reg.bal = VDEV_MBX_ATQBAL - mbx_start;
			} else {
				ccq->reg.head = VF_ATQH - mbx_start;
				ccq->reg.tail = VF_ATQT - mbx_start;
				ccq->reg.len = VF_ATQLEN - mbx_start;
				ccq->reg.bah = VF_ATQBAH - mbx_start;
				ccq->reg.bal = VF_ATQBAL - mbx_start;
			}
			ccq->reg.len_mask = VF_ATQLEN_ATQLEN_M;
			ccq->reg.len_ena_mask = VF_ATQLEN_ATQENABLE_M;
			ccq->reg.head_mask = VF_ATQH_ATQH_M;
			break;
		case IDPF_CTLQ_TYPE_MAILBOX_RX:
			if (siov) {
				ccq->reg.head = VDEV_MBX_ARQH - mbx_start;
				ccq->reg.tail = VDEV_MBX_ARQT - mbx_start;
				ccq->reg.len = VDEV_MBX_ARQLEN - mbx_start;
				ccq->reg.bah = VDEV_MBX_ARQBAH - mbx_start;
				ccq->reg.bal = VDEV_MBX_ARQBAL - mbx_start;
			} else {
				ccq->reg.head = VF_ARQH - mbx_start;
				ccq->reg.tail = VF_ARQT - mbx_start;
				ccq->reg.len = VF_ARQLEN - mbx_start;
				ccq->reg.bah = VF_ARQBAH - mbx_start;
				ccq->reg.bal = VF_ARQBAL - mbx_start;
			}
			ccq->reg.len_mask = VF_ARQLEN_ARQLEN_M;
			ccq->reg.len_ena_mask = VF_ARQLEN_ARQENABLE_M;
			ccq->reg.head_mask = VF_ARQH_ARQH_M;
			break;
		default:
			break;
		}
	}
}

/**
 * idpf_vf_mb_intr_reg_init - initialise the mailbox interrupt registers
 * @adapter: driver private data
 */
static void
idpf_vf_mb_intr_reg_init(struct idpf_adapter *adapter)
{
	struct idpf_intr_reg *intr = &adapter->mb_vector.intr_reg;
	uint32_t dyn_ctl = le32toh(adapter->caps.mailbox_dyn_ctl);

	intr->dyn_ctl = idpf_get_reg_addr(adapter, dyn_ctl);
	intr->dyn_ctl_intena_m = VF_INT_DYN_CTL0_INTENA_M;
	intr->dyn_ctl_itridx_m = VF_INT_DYN_CTL0_ITR_INDX_M;
	intr->icr_ena = idpf_get_reg_addr(adapter, VF_INT_ICR0_ENA1);
	intr->icr_ena_ctlq_m = VF_INT_ICR0_ENA1_ADMINQ_M;
}

/**
 * idpf_vf_intr_reg_init - initialise the data queue interrupt registers
 * @vport: vport owning the vectors
 * @rsrc: queue and vector resources
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_vf_intr_reg_init(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
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
		intr->dyn_ctl_intena_m = VF_INT_DYN_CTLN_INTENA_M;
		intr->dyn_ctl_intena_msk_m = VF_INT_DYN_CTLN_INTENA_MSK_M;
		intr->dyn_ctl_itridx_s = VF_INT_DYN_CTLN_ITR_INDX_S;
		intr->dyn_ctl_intrvl_s = VF_INT_DYN_CTLN_INTERVAL_S;
		intr->dyn_ctl_wb_on_itr_m = VF_INT_DYN_CTLN_WB_ON_ITR_M;
		intr->dyn_ctl_itridx_m = VF_INT_DYN_CTLN_ITR_INDX_M;
		intr->dyn_ctl_swint_trig_m = VF_INT_DYN_CTLN_SWINT_TRIG_M;
		intr->dyn_ctl_sw_itridx_ena_m =
		    VF_INT_DYN_CTLN_SW_ITR_INDX_ENA_M;

		spacing = IDPF_ITR_IDX_SPACING(
		    reg_vals[vec_id].itrn_index_spacing,
		    IDPF_VF_ITR_IDX_SPACING);
		rx_itr = VF_INT_ITRN_ADDR(VIRTCHNL2_ITR_IDX_0,
		    reg_vals[vec_id].itrn_reg, spacing);
		tx_itr = VF_INT_ITRN_ADDR(VIRTCHNL2_ITR_IDX_1,
		    reg_vals[vec_id].itrn_reg, spacing);
		intr->rx_itr = idpf_get_reg_addr(adapter, rx_itr);
		intr->tx_itr = idpf_get_reg_addr(adapter, tx_itr);
	}

free_reg_vals:
	free(reg_vals, M_DEVBUF);

	return (err);
}

/**
 * idpf_vf_reset_reg_init - initialise the reset status register
 * @adapter: driver private data
 */
static void
idpf_vf_reset_reg_init(struct idpf_adapter *adapter)
{

	adapter->reset_reg.rstat = idpf_get_rstat_reg_addr(adapter,
	    VFGEN_RSTAT);
	adapter->reset_reg.rstat_m = VFGEN_RSTAT_VFR_STATE_M;
}

/**
 * idpf_vf_trigger_reset - ask the control plane to reset this function
 * @adapter: driver private data
 * @trig_cause: reason the reset was requested
 *
 * A VF has no reset-trigger register: the reset is a mailbox request.
 */
static void
idpf_vf_trigger_reset(struct idpf_adapter *adapter,
    enum idpf_flags trig_cause)
{
	int err;

	if (trig_cause != IDPF_HR_FUNC_RESET)
		return;

	err = idpf_send_mb_msg(adapter, adapter->hw.asq,
	    VIRTCHNL2_OP_RESET_VF, 0, NULL, 0);
	if (err != 0)
		device_printf(idpf_adapter_to_dev(adapter),
		    "failed to send Reset VF: %d\n", err);
}

/**
 * idpf_vf_reg_ops_init - install the VF register operations
 * @adapter: driver private data
 */
static void
idpf_vf_reg_ops_init(struct idpf_adapter *adapter)
{

	adapter->dev_ops.reg_ops.ctlq_reg_init = idpf_vf_ctlq_reg_init;
	adapter->dev_ops.reg_ops.intr_reg_init = idpf_vf_intr_reg_init;
	adapter->dev_ops.reg_ops.mb_intr_reg_init = idpf_vf_mb_intr_reg_init;
	adapter->dev_ops.reg_ops.reset_reg_init = idpf_vf_reset_reg_init;
	adapter->dev_ops.reg_ops.trigger_reset = idpf_vf_trigger_reset;
}

/**
 * idpf_vf_dev_ops_init - install the VF device operations
 * @adapter: driver private data
 *
 * Publishes the geometry of the two static BAR0 windows; attach fills each
 * window's vaddr in once BAR0 itself is mapped.
 */
void
idpf_vf_dev_ops_init(struct idpf_adapter *adapter)
{
	struct idpf_hw *hw = &adapter->hw;

	idpf_vf_reg_ops_init(adapter);

	if (idpf_vf_is_siov(adapter)) {
		hw->mbx.addr_start = VDEV_MBX_START;
		hw->mbx.addr_len = IDPF_SIOV_MBX_REGION_SZ;
		hw->rstat.addr_start = VFGEN_RSTAT;
		hw->rstat.addr_len = IDPF_SIOV_RSTAT_REGION_SZ;
		return;
	}

	hw->mbx.addr_start = VF_BASE;
	hw->mbx.addr_len = IDPF_VF_MBX_REGION_SZ;
	hw->rstat.addr_start = VFGEN_RSTAT;
	hw->rstat.addr_len = IDPF_VF_RSTAT_REGION_SZ;
}
