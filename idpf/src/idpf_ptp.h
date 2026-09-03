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

enum idpf_ptp_access {
	IDPF_PTP_NONE = 0,
	IDPF_PTP_DIRECT,
	IDPF_PTP_MAILBOX,
};

struct idpf_ptp_cmd {
	uint32_t exec_cmd_mask;
	uint32_t shtime_enable_mask;
};

struct idpf_ptp_dev_clk_regs {
	uint32_t dev_clk_ns_l;
	uint32_t dev_clk_ns_h;
	uint32_t phy_clk_ns_l;
	uint32_t phy_clk_ns_h;
	uint32_t sys_time_ns_l;
	uint32_t sys_time_ns_h;
	uint32_t incval_l;
	uint32_t incval_h;
	uint32_t shadj_l;
	uint32_t shadj_h;
	uint32_t phy_incval_l;
	uint32_t phy_incval_h;
	uint32_t phy_shadj_l;
	uint32_t phy_shadj_h;
	uint32_t cmd;
	uint32_t phy_cmd;
	uint32_t cmd_sync;
};

struct idpf_ptp_secondary_mbx {
	uint16_t peer_mbx_q_id;
	uint8_t	 peer_id;
	uint8_t	 mbx_q_index;
	bool	 valid;
};

struct idpf_ptp_tx_tstamp {
	uint32_t tx_latch_reg_offset_l;
	uint32_t tx_latch_reg_offset_h;
	uint8_t	 idx;
};

struct idpf_ptp_vport_tx_tstamp_caps {
	uint32_t vport_id;
	uint16_t num_entries;
	uint8_t	 tstamp_ns_lo_bit;
	uint8_t	 tstamp_ns_hi_bit;
	uint32_t readiness_offset_l;
	uint32_t readiness_offset_h;
	uint8_t	 access;
	struct idpf_ptp_tx_tstamp *latches;
};

struct idpf_ptp_dev_timers {
	uint64_t sys_time_ns;
	uint64_t dev_clk_time_ns;
};

struct idpf_ptp {
	struct idpf_adapter *adapter;
	uint64_t base_incval;
	uint32_t max_adj;
	struct idpf_ptp_cmd cmd;
	uint64_t cached_phc_time;
	int	 cached_phc_ticks;
	struct idpf_ptp_dev_clk_regs dev_clk_regs;
	uint32_t caps;
	/* enum idpf_ptp_access; plain uint8_t avoids enum-bitfield warnings. */
	uint8_t	 get_dev_clk_time_access;
	uint8_t	 get_cross_tstamp_access;
	uint8_t	 tx_tstamp_access;
	struct idpf_ptp_secondary_mbx secondary_mbx;
	struct mtx read_dev_clk_lock;
};

int	idpf_ptp_init(struct idpf_adapter *adapter);
void	idpf_ptp_release(struct idpf_adapter *adapter);
int	idpf_ptp_get_dev_clk_time(struct idpf_adapter *adapter,
	    struct idpf_ptp_dev_timers *dev_clk_time);
int	idpf_ptp_get_vport_tstamps_caps(struct idpf_vport *vport);
void	idpf_ptp_release_vport_tstamps_caps(struct idpf_vport *vport);
uint64_t idpf_ptp_tstamp_extend_32b_to_64b(uint64_t cached_phc_time,
	    uint32_t in_timestamp);

bool	idpf_ptp_is_vport_tx_tstamp_ena(struct idpf_vport *vport);
bool	idpf_ptp_is_vport_rx_tstamp_ena(struct idpf_vport *vport);

#endif /* _IDPF_PTP_H_ */
