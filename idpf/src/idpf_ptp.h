/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (C) 2023 Intel Corporation */

#ifndef _IDPF_PTP_H_
#define _IDPF_PTP_H_

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>

struct idpf_adapter;
struct idpf_vport;

/*
 * Only the capability-negotiation half of PTP is implemented.  The TX
 * timestamp latch harvest is absent because struct idpf_tx_offload_params has
 * no timestamp request field and union idpf_flex_tx_ctx_desc exposes only
 * tsyn_reg_l/tsyn_reg_h, so a TX request cannot yet be expressed.
 */

/* Register offsets arrive from the control plane, so 0 means "not provided". */
#define IDPF_PTP_REG_INVALID	0

/* Bounds a control-plane supplied latch count before it sizes an allocation. */
#define IDPF_PTP_MAX_TX_TSTAMP_LATCHES	256
enum idpf_ptp_access {
	IDPF_PTP_NONE = 0,
	IDPF_PTP_DIRECT,
	IDPF_PTP_MAILBOX,
};

struct idpf_ptp_cmd {
	u32 exec_cmd_mask;
	u32 shtime_enable_mask;
};

struct idpf_ptp_dev_clk_regs {
	u32 dev_clk_ns_l;
	u32 dev_clk_ns_h;
	u32 phy_clk_ns_l;
	u32 phy_clk_ns_h;
	u32 sys_time_ns_l;
	u32 sys_time_ns_h;
	u32 incval_l;
	u32 incval_h;
	u32 shadj_l;
	u32 shadj_h;
	u32 phy_incval_l;
	u32 phy_incval_h;
	u32 phy_shadj_l;
	u32 phy_shadj_h;
	u32 cmd;
	u32 phy_cmd;
	u32 cmd_sync;
};

struct idpf_ptp_secondary_mbx {
	u16 peer_mbx_q_id;
	u8	 peer_id;
	u8	 mbx_q_index;
	bool	 valid;
};

struct idpf_ptp_tx_tstamp {
	u32 tx_latch_reg_offset_l;
	u32 tx_latch_reg_offset_h;
	u8	 idx;
};

struct idpf_ptp_vport_tx_tstamp_caps {
	u32 vport_id;
	u16 num_entries;
	u8	 tstamp_ns_lo_bit;
	u8	 tstamp_ns_hi_bit;
	u32 readiness_offset_l;
	u32 readiness_offset_h;
	u8	 access;
	struct idpf_ptp_tx_tstamp *latches;
};

struct idpf_ptp_dev_timers {
	u64 sys_time_ns;
	u64 dev_clk_time_ns;
};

struct idpf_ptp {
	struct idpf_adapter *adapter;
	u64 base_incval;
	u32 max_adj;
	struct idpf_ptp_cmd cmd;
	u64 cached_phc_time;
	int	 cached_phc_ticks;
	struct idpf_ptp_dev_clk_regs dev_clk_regs;
	u32 caps;
	/* enum idpf_ptp_access; plain u8 avoids enum-bitfield warnings. */
	u8	 get_dev_clk_time_access;
	u8	 get_cross_tstamp_access;
	u8	 tx_tstamp_access;
	struct idpf_ptp_secondary_mbx secondary_mbx;
	/* sx, not mtx: the mailbox read sleeps in cv_timedwait() while held. */
	struct sx read_dev_clk_lock;
};

int	idpf_ptp_init(struct idpf_adapter *adapter);
void	idpf_ptp_release(struct idpf_adapter *adapter);
void	idpf_ptp_sysctl_init(struct idpf_adapter *adapter);
int	idpf_ptp_get_dev_clk_time(struct idpf_adapter *adapter,
	    struct idpf_ptp_dev_timers *dev_clk_time);
int	idpf_ptp_get_vport_tstamps_caps(struct idpf_vport *vport);
void	idpf_ptp_release_vport_tstamps_caps(struct idpf_vport *vport);
u64 idpf_ptp_tstamp_extend_32b_to_64b(u64 cached_phc_time,
	    u32 in_timestamp);

bool	idpf_ptp_is_vport_tx_tstamp_ena(struct idpf_vport *vport);
bool	idpf_ptp_is_vport_rx_tstamp_ena(struct idpf_vport *vport);
void	idpf_ptp_get_tstamp_config(struct idpf_vport *vport,
	    struct idpf_tstamp_config *cfg);

#endif /* _IDPF_PTP_H_ */
