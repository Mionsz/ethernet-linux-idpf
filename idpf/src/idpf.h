/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * idpf.h — FreeBSD >= 15.0 IDPF VF-DPF driver master header.
 *
 * CONVERSION NOTES (evidence class per Human Reference Guide Appendix A):
 *
 *  [FBSD15:A30-A34]  FreeBSD 15 kernel interfaces are authoritative.
 *  [IDPF:A13-A14]    Virtchnl2 / IDPF 1.0 protocol structures retained.
 *  [LOCAL:A20-A25]   Current skeleton file shapes preserved.
 *  [PROPOSED:A38]    TARGET DESIGN items marked; not yet implemented.
 *  [ON_HOLD]         Items blocked by named contradictions in Appendix L.
 *
 * Removed entirely (Linux-only, no FreeBSD equivalent in scope):
 *   kcompat.h, all linux/ and net/ Linux headers, XDP/AF_XDP, VDCM/MDEV,
 *   devlink, IDC/IIDC, ethtool_netlink, TC/MQPRIO, tunnel offload guards,
 *   HAVE_* / CONFIG_* / DEVLINK_ENABLED / IDPF_ADD_PROBES conditional blocks,
 *   NETIF_MSG_*, NETIF_F_*, u64_stats_sync, DECLARE_BITMAP, spinlock_t,
 *   wait_queue_head_t, work_struct/delayed_work (replaced by FreeBSD
 *   taskqueue/callout), struct net_device (replaced by if_ctx_t / ifnet *),
 *   struct msix_entry (replaced by struct resource *), hwtstamp_config,
 *   ethtool_rx_flow_spec, cpumask_t/irq_affinity_notify, list_head
 *   (replaced by TAILQ), rtnl_link_stats64 (replaced by if_data).
 *
 * Core method names are NOT renamed per architectural constraint.
 */

#ifndef _IDPF_H_
#define _IDPF_H_

/* -----------------------------------------------------------------------
 * FreeBSD kernel headers — authoritative for all OS-facing contracts.
 * [FBSD15:A30-A34]
 * ----------------------------------------------------------------------- */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/callout.h>
#include <sys/condvar.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/iflib.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

/* -----------------------------------------------------------------------
 * Shared IDPF / Virtchnl2 protocol headers.
 * These are carried source; edits must comply with carried_source_edit_policy.
 * [IDPF:A13-A14] [LOCAL:A17]
 * ----------------------------------------------------------------------- */
#include "virtchnl2.h"
#include "idpf_txrx.h"
#include "idpf_controlq.h"
#include "idpf_devids.h"

/*
 * Attach-path tracing.  Build with -DIDPF_DEBUG to enable; these are progress
 * markers, not error reports, so they stay silent in a normal build.
 */
#ifdef IDPF_DEBUG
#define idpf_dbg(dev, ...)	device_printf((dev), __VA_ARGS__)
#else
#define idpf_dbg(dev, ...)	do { } while (0)
#endif

/*
 * idpf_adi.h is retained: ADI lifecycle events are CONDITIONAL but the
 * header is shared protocol material.  [LOCAL:A18]
 */
#include "idpf_adi.h"

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
struct idpf_adapter;
struct idpf_vport;
struct idpf_vport_max_q;
struct idpf_q_vec_rsrc;
struct idpf_rss_data;

/* -----------------------------------------------------------------------
 * Driver identity
 * ----------------------------------------------------------------------- */
#define IDPF_DRV_NAME   "idpf"
#define IDPF_DRV_VER    "1.0.15"

/* Bit-field helper — protocol-internal, retained from Linux baseline. */
#define IDPF_M(m, s)    ((m) << (s))

/* -----------------------------------------------------------------------
 * Mailbox / control-queue constants  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_CTLQ_MAX_BUF_LEN           4096    /* SZ_4K equivalent */
#define IDPF_NUM_FILTERS_PER_MSG        20
#define IDPF_NUM_DFLT_MBX_Q            2       /* TX + RX */
#define IDPF_DFLT_MBX_Q_LEN            64
#define IDPF_DFLT_MBX_ID               (-1)
#define IDPF_MB_MAX_ERR                 20
#define IDPF_NUM_CHUNKS_PER_MSG(struct_sz, chunk_sz) \
        ((IDPF_CTLQ_MAX_BUF_LEN - (struct_sz)) / (chunk_sz))

/* -----------------------------------------------------------------------
 * Reset / recovery timing constants
 * NOTE: IDPF_HARD_RESET_TIMEOUT_MSEC and retry policy are subject to
 * Appendix L contradiction L-C03 (short-retry vs. 10-second readiness).
 * These values are placeholders; the firmware/driver policy owner must
 * resolve L-C03 before these constants enter production.  [ON_HOLD:L-C03]
 * ----------------------------------------------------------------------- */
#define IDPF_HARD_RESET_TIMEOUT_MSEC    (120 * 1000)   /* [ON_HOLD:L-C03] */
#define IDPF_CORER_TIMEOUT_MSEC         (120 * 1000)   /* [ON_HOLD:L-C03] */
#define IDPF_RESET_POLL_COUNT           (2 * 1000)

/* -----------------------------------------------------------------------
 * Miscellaneous constants
 * ----------------------------------------------------------------------- */
#define IDPF_NO_FREE_SLOT               0xffff
#define IDPF_RSTAT_COMPLETE             0x01
#define IDPF_DIM_PROFILE_SLOTS          5

/* Virtchnl2 version negotiated by this driver  [IDPF:A13-A14] */
#define IDPF_VIRTCHNL_VERSION_MAJOR     VIRTCHNL2_VERSION_MAJOR_2
#define IDPF_VIRTCHNL_VERSION_MINOR     VIRTCHNL2_VERSION_MINOR_0

/* BAR region size constants  [IDPF:A13] */
#define IDPF_MMIO_REG_NUM_STATIC        2
#define IDPF_PF_MBX_REGION_SZ          4096
#define IDPF_PF_RSTAT_REGION_SZ         2048
#define IDPF_VF_MBX_REGION_SZ          10240
#define IDPF_VF_RSTAT_REGION_SZ         2048
#define IDPF_SIOV_MBX_REGION_SZ         4096
#define IDPF_SIOV_RSTAT_REGION_SZ       2048

/* -----------------------------------------------------------------------
 * Capability-check macros  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define idpf_is_cap_ena(adapter, field, flag) \
        idpf_is_capability_ena(adapter, false, field, flag)
#define idpf_is_cap_ena_all(adapter, field, flag) \
        idpf_is_capability_ena(adapter, true, field, flag)

/* -----------------------------------------------------------------------
 * RSS capability bitmask composites  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_CAP_RSS ( \
        VIRTCHNL2_FLOW_IPV4_TCP         | \
        VIRTCHNL2_FLOW_IPV4_UDP         | \
        VIRTCHNL2_FLOW_IPV4_SCTP        | \
        VIRTCHNL2_FLOW_IPV4_OTHER       | \
        VIRTCHNL2_FLOW_IPV6_TCP         | \
        VIRTCHNL2_FLOW_IPV6_UDP         | \
        VIRTCHNL2_FLOW_IPV6_SCTP        | \
        VIRTCHNL2_FLOW_IPV6_OTHER)

#define IDPF_CAP_RSC ( \
        VIRTCHNL2_CAP_RSC_IPV4_TCP      | \
        VIRTCHNL2_CAP_RSC_IPV6_TCP)

#define IDPF_CAP_HSPLIT ( \
        VIRTCHNL2_CAP_RX_HSPLIT_AT_L4V4 | \
        VIRTCHNL2_CAP_RX_HSPLIT_AT_L4V6)

#define IDPF_CAP_TX_CSUM_L4V4 ( \
        VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_TCP | \
        VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_UDP)

#define IDPF_CAP_TX_CSUM_L4V6 ( \
        VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_TCP | \
        VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_UDP)

#define IDPF_CAP_RX_CSUM ( \
        VIRTCHNL2_CAP_RX_CSUM_L3_IPV4          | \
        VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_TCP      | \
        VIRTCHNL2_CAP_RX_CSUM_L4_IPV4_UDP      | \
        VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_TCP      | \
        VIRTCHNL2_CAP_RX_CSUM_L4_IPV6_UDP)

#define IDPF_CAP_TX_SCTP_CSUM ( \
        VIRTCHNL2_CAP_TX_CSUM_L4_IPV4_SCTP | \
        VIRTCHNL2_CAP_TX_CSUM_L4_IPV6_SCTP)

#define IDPF_CAP_TUNNEL_TX_CSUM ( \
        VIRTCHNL2_CAP_TX_CSUM_L3_SINGLE_TUNNEL | \
        VIRTCHNL2_CAP_TX_CSUM_L4_SINGLE_TUNNEL)

/* -----------------------------------------------------------------------
 * Device-simulation detection macros.
 *
 * IS_EMR_DEVICE / IS_SIMICS_DEVICE are referenced by the timeout helpers
 * below and come from idpf_devids.h, included above.
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * struct idpf_mac_filter
 *
 * Per-MAC-address filter entry.  list_head replaced by TAILQ_ENTRY.
 * [FBSD15:A30] [LOCAL:A20]
 * ----------------------------------------------------------------------- */
struct idpf_mac_filter {
        TAILQ_ENTRY(idpf_mac_filter) list;
        uint8_t  macaddr[ETHER_ADDR_LEN];
        bool     remove;
        bool     add;
};

/* -----------------------------------------------------------------------
 * enum idpf_state — control-plane bring-up state machine
 * [IDPF:A13-A14] [LOCAL:A24]
 * ----------------------------------------------------------------------- */
enum idpf_state {
        __IDPF_VER_CHECK,
        __IDPF_GET_CAPS,
        __IDPF_INIT_SW,
        __IDPF_STATE_LAST,
};

/* -----------------------------------------------------------------------
 * enum idpf_flags — adapter-level flags stored in adapter->flags (uint32_t).
 *
 * Linux DECLARE_BITMAP replaced by plain uint32_t bit positions.
 * Use (adapter->flags & (1u << IDPF_HR_RESET_IN_PROG)) etc.
 * [FBSD15:A32] [LOCAL:A21]
 * ----------------------------------------------------------------------- */
enum idpf_flags {
        IDPF_HR_FUNC_RESET,
        IDPF_HR_DRV_LOAD,
        IDPF_HR_RESET_IN_PROG,
        IDPF_REMOVE_IN_PROG,
        IDPF_MB_INTR_MODE,
        IDPF_VC_CORE_INIT,
        IDPF_CORER_IN_PROG,
        IDPF_PCI_CB_RESET,
        IDPF_FLAGS_NBITS,       /* must be last; must fit in uint32_t */
};

_Static_assert(IDPF_FLAGS_NBITS <= 32,
    "idpf_flags exceeds uint32_t width");

/* -----------------------------------------------------------------------
 * enum idpf_cap_field — offsets into virtchnl2_get_capabilities
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
enum idpf_cap_field {
        IDPF_BASE_CAPS   = -1,
        IDPF_CSUM_CAPS   = offsetof(struct virtchnl2_get_capabilities,
                                    csum_caps),
        IDPF_SEG_CAPS    = offsetof(struct virtchnl2_get_capabilities,
                                    seg_caps),
        IDPF_RSS_CAPS    = offsetof(struct virtchnl2_get_capabilities,
                                    rss_caps),
        IDPF_HSPLIT_CAPS = offsetof(struct virtchnl2_get_capabilities,
                                    hsplit_caps),
        IDPF_RSC_CAPS    = offsetof(struct virtchnl2_get_capabilities,
                                    rsc_caps),
        IDPF_OTHER_CAPS  = offsetof(struct virtchnl2_get_capabilities,
                                    other_caps),
};

/* -----------------------------------------------------------------------
 * enum idpf_vport_state — per-vport state bits stored in vport->state
 * (uint32_t, same pattern as adapter->flags).
 * [LOCAL:A20]
 * ----------------------------------------------------------------------- */
enum idpf_vport_state {
        IDPF_VPORT_UP,
        IDPF_VPORT_STATE_NBITS,
};

_Static_assert(IDPF_VPORT_STATE_NBITS <= 32,
    "idpf_vport_state exceeds uint32_t width");

/* -----------------------------------------------------------------------
 * struct idpf_netdev_priv — per-vport back-pointer stored in iflib ctx.
 *
 * net_device removed; FreeBSD uses if_ctx_t / struct ifnet *.
 * spinlock_t replaced by struct mtx.
 * rtnl_link_stats64 replaced by struct if_data (FreeBSD per-interface stats).
 * DECLARE_BITMAP replaced by uint32_t.
 * [FBSD15:A30] [FBSD15:A32] [LOCAL:A20-A21]
 * ----------------------------------------------------------------------- */
struct idpf_netdev_priv {
        struct idpf_adapter     *adapter;
        struct idpf_vport       *vport;
        uint32_t                 vport_id;
        uint32_t                 link_speed_mbps;
        uint16_t                 vport_idx;
        uint32_t                 state;         /* idpf_vport_state bits */
        uint16_t                 tx_max_bufs;
        struct mtx               stats_lock;    /* protects netstats */
        struct if_data           netstats;      /* [FBSD15:A30] */
};

/* -----------------------------------------------------------------------
 * struct idpf_reset_reg — reset and OICR register descriptors.
 *
 * void __iomem * replaced by void * (bus_space accessor model; raw pointer
 * is only valid while the BAR resource is mapped).  [FBSD15:A31]
 * ----------------------------------------------------------------------- */
struct idpf_reset_reg {
        void    *rstat;         /* mapped reset-status register address */
        void    *oicr_cause;    /* mapped OICR cause register address */
        uint32_t rstat_m;
        uint32_t oicr_cause_m;
};

/* -----------------------------------------------------------------------
 * struct idpf_vport_max_q — queue count limits per vport  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_vport_max_q {
        uint16_t max_rxq;
        uint16_t max_txq;
        uint16_t max_bufq;
        uint16_t max_complq;
};

/* -----------------------------------------------------------------------
 * struct idpf_reg_ops — device-specific register operation callbacks.
 *
 * ptp_reg_init and read_master_time are CONDITIONAL (feature 217).
 * They are retained as nullable function pointers; callers must check
 * for NULL before invoking.  [IDPF:A13-A14] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
struct idpf_reg_ops {
        void (*ctlq_reg_init)(struct idpf_adapter *adapter,
                              struct idpf_ctlq_create_info *cq);
        int  (*intr_reg_init)(struct idpf_vport *vport,
                              struct idpf_q_vec_rsrc *rsrc);
        void (*mb_intr_reg_init)(struct idpf_adapter *adapter);
        void (*reset_reg_init)(struct idpf_adapter *adapter);
        void (*oicr_reset_reg_init)(struct idpf_adapter *adapter);
        void (*trigger_reset)(struct idpf_adapter *adapter,
                              enum idpf_flags trig_cause);
        /*
         * read_master_time / ptp_reg_init: CONDITIONAL (feature 217).
         * May be NULL when PTP is not negotiated.  [ON_HOLD — see §23.4]
         */
        uint64_t (*read_master_time)(const struct idpf_hw *hw);
        void     (*ptp_reg_init)(const struct idpf_adapter *adapter);
};

/* -----------------------------------------------------------------------
 * struct idpf_dev_ops — device-specific operations.
 *
 * vdcm_init / vdcm_deinit (VFIO/MDEV) removed — Linux-only.
 * idc_init removed — IDC/IIDC is DELEGATED.
 * notify_adi_reset retained: ADI lifecycle is CONDITIONAL but the
 * callback pointer is part of the shared device-ops table.
 * static_reg_info uses struct resource * (FreeBSD BAR resource handle).
 * [FBSD15:A30] [LOCAL:A22] [IDPF:A13]
 * ----------------------------------------------------------------------- */
struct idpf_dev_ops {
        /*
         * notify_adi_reset: CONDITIONAL (feature 084).
         * May be NULL when ADI is not negotiated.
         */
        void (*notify_adi_reset)(struct idpf_adapter *adapter,
                                 uint16_t adi_id, bool reset);

        struct idpf_reg_ops reg_ops;

        /*
         * static_reg_info[0]: the BAR0 memory resource.
         *
         * FreeBSD's resource manager will not hand out the same PCI BAR
         * twice, so unlike Linux - which ioremaps the mailbox, rstat and LAN
         * windows independently - the port maps BAR0 once during attach-pre
         * and every region in struct idpf_hw records a vaddr computed as an
         * offset into that single mapping.  static_reg_info[1] is reserved.
         * Released during detach.
         * [FBSD15:A30-A31] [IDPF:A13]
         */
        struct resource *static_reg_info[IDPF_MMIO_REG_NUM_STATIC];
};

/* -----------------------------------------------------------------------
 * enum idpf_vport_reset_cause — soft-reset trigger reasons
 * XDP cause removed (XDP not in scope).  [LOCAL:A18]
 * ----------------------------------------------------------------------- */
enum idpf_vport_reset_cause {
        IDPF_SR_Q_CHANGE,
        IDPF_SR_Q_DESC_CHANGE,
        IDPF_SR_Q_SCH_CHANGE,
        IDPF_SR_MTU_CHANGE,
        IDPF_SR_RSC_CHANGE,
        IDPF_SR_HSPLIT_CHANGE,
};

/* -----------------------------------------------------------------------
 * enum idpf_vport_flags — per-vport flag bits (stored in vport->flags,
 * uint32_t).  [LOCAL:A20]
 * ----------------------------------------------------------------------- */
enum idpf_vport_flags {
        IDPF_VPORT_DEL_QUEUES,
        IDPF_VPORT_SW_MARKER,
        IDPF_VPORT_FLAGS_NBITS,
};

_Static_assert(IDPF_VPORT_FLAGS_NBITS <= 32,
    "idpf_vport_flags exceeds uint32_t width");

/* -----------------------------------------------------------------------
 * struct idpf_port_stats — per-vport statistics.
 *
 * u64_stats_sync removed (Linux-only); counters are plain uint64_t.
 * A single stats_lock (struct mtx) protects the entire struct.
 * CONFIG_UPLINK_PORT_STATS and IDPF_ADD_PROBES blocks removed.
 * [FBSD15:A32] [LOCAL:A20]
 * ----------------------------------------------------------------------- */
struct idpf_port_stats {
        struct mtx                      stats_lock;     /* [FBSD15:A32] */
        uint64_t                        rx_hw_csum_err;
        uint64_t                        rx_hsplit;
        uint64_t                        rx_hsplit_hbo;
        uint64_t                        rx_bad_descs;
        uint64_t                        tx_linearize;
        uint64_t                        tx_busy;
        uint64_t                        tx_drops;
        uint64_t                        tx_dma_map_errs;
        uint64_t                        tx_reinjection_timeouts;
        struct virtchnl2_vport_stats    vport_stats;    /* [IDPF:A13-A14] */
        uint64_t                        tx_lso_pkts;
        uint64_t                        tx_lso_bytes;
        uint64_t                        tx_lso_segs_tot;
        uint64_t                        rx_page_recycles;
        uint64_t                        rx_page_reallocs;
        uint64_t                        rx_rsc_pkts;
        uint64_t                        rx_rsc_bytes;
        uint64_t                        rx_rsc_segs_tot;
        uint64_t                        lso_seg[IDPF_MAX_SEGS];
        uint64_t                        rsc_seg[IDPF_MAX_SEGS];
};

/* -----------------------------------------------------------------------
 * struct idpf_q_vec_rsrc — queue and vector resource bundle.
 *
 * struct device * replaced by device_t (FreeBSD newbus device handle).
 * [FBSD15:A30] [LOCAL:A21]
 * ----------------------------------------------------------------------- */
struct idpf_q_vec_rsrc {
        device_t                 dev;           /* [FBSD15:A30] */
        struct idpf_q_vector    *q_vectors;
        uint16_t                *q_vector_idxs;
        uint16_t                 num_q_vectors;

        struct idpf_txq_group   *txq_grps;
        uint32_t                 txq_desc_count;
        uint32_t                 complq_desc_count;
        uint32_t                 txq_model;
        uint16_t                 num_txq;
        uint16_t                 num_complq;
        uint16_t                 num_txq_grp;

        uint16_t                 num_rxq_grp;
        uint32_t                 rxq_model;
        struct idpf_rxq_group   *rxq_grps;
        uint16_t                 num_rxq;
        uint16_t                 num_bufq;
        uint32_t                 rxq_desc_count;
        uint32_t                 bufq_desc_count[IDPF_MAX_BUFQS_PER_RXQ_GRP];
        uint8_t                  num_bufqs_per_qgrp;
        bool                     base_rxd;
        uint32_t                 bufq_size[IDPF_MAX_BUFQS_PER_RXQ_GRP];
};

/* -----------------------------------------------------------------------
 * struct idpf_fsteer_fltr — flow-steering filter entry.
 *
 * ethtool_rx_flow_spec removed (no ethtool in FreeBSD).
 * Replaced by an opaque placeholder pending a FreeBSD flow-steering
 * contract.  Feature 239 is CONDITIONAL.  [LOCAL:A18] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
struct idpf_fsteer_fltr {
        TAILQ_ENTRY(idpf_fsteer_fltr) list;
        /*
         * fs: flow-steering rule specification.
         * TARGET DESIGN: replace with a FreeBSD-native flow rule struct
         * once feature 239 (sideband flow steering) contract is approved.
         * [CONDITIONAL — feature 239]
         */
        uint8_t  fs_opaque[64];         /* placeholder; do not decode */
};

/* -----------------------------------------------------------------------
 * struct idpf_vport — per-vport handle.
 *
 * XDP fields removed (XDP not in scope).
 * IIDC/RDMA vdev_info removed (DELEGATED).
 * net_device replaced by if_ctx_t (iflib context) and struct ifnet *.
 * DECLARE_BITMAP replaced by uint32_t.
 * wait_queue_head_t replaced by struct cv + struct mtx (sw_marker_cv /
 *   sw_marker_lock) for the software-marker drain wait.
 * hwtstamp_config / tstamp_task / tstamp_stats removed (PTP CONDITIONAL).
 * tx_tstamp_caps retained as a nullable pointer (PTP CONDITIONAL).
 * ptype_stats removed (IDPF_ADD_PROBES Linux-only).
 * [FBSD15:A30] [LOCAL:A20-A25] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
struct idpf_vport {
        struct idpf_q_vec_rsrc   dflt_qv_rsrc;
        struct idpf_queue      **txqs;          /* fast-path TX queue array */
        uint16_t                 num_txq;
        uint16_t                 tw_ts_gran_s;
        uint64_t                 tw_horizon;

        struct idpf_adapter     *adapter;

        /*
         * iflib integration:
         *   ctx  — iflib software context (replaces struct net_device *).
         *   ifp  — underlying ifnet pointer; obtained via iflib_get_ifp().
         * [FBSD15:A30]
         */
        if_ctx_t                 ctx;
        struct ifnet            *ifp;

        uint32_t                 flags;         /* idpf_vport_flags bits */
        uint32_t                 compln_clean_budget;
        uint32_t                 vport_id;
        uint16_t                 vport_type;
        uint16_t                 idx;

        uint16_t                 max_mtu;
        uint8_t                  default_mac_addr[ETHER_ADDR_LEN];
        uint16_t                 rx_itr_profile[IDPF_DIM_PROFILE_SLOTS];
        uint16_t                 tx_itr_profile[IDPF_DIM_PROFILE_SLOTS];

        struct idpf_port_stats   port_stats;
        bool                     default_vport;
        bool                     crc_enable;
        bool                     link_up;

        /*
         * Software-marker drain wait.
         * Linux wait_queue_head_t replaced by condvar + mutex pair.
         * [FBSD15:A32-A33]
         */
        struct mtx               sw_marker_lock;
        struct cv                sw_marker_cv;

        /*
         * tx_tstamp_caps: CONDITIONAL (feature 253 — TX completion
         * timestamps).  NULL when not negotiated.  [ON_HOLD — see §21.8]
         */
        struct idpf_ptp_vport_tx_tstamp_caps *tx_tstamp_caps;
};

/* -----------------------------------------------------------------------
 * enum idpf_user_flags — user-visible configuration flags.
 * Stored in idpf_vport_user_config_data.user_flags (uint64_t bitmask).
 * DECLARE_BITMAP replaced by uint64_t.  [LOCAL:A18]
 * ----------------------------------------------------------------------- */
enum idpf_user_flags {
        __IDPF_PRIV_FLAGS_HDR_SPLIT = 0,
        __IDPF_USER_FLAG_HSPLIT     = 0,
        __IDPF_PROMISC_UC           = 32,
        __IDPF_PROMISC_MC,
        __IDPF_USER_FLAGS_NBITS,
};

_Static_assert(__IDPF_USER_FLAGS_NBITS <= 64,
    "idpf_user_flags exceeds uint64_t width");

/* -----------------------------------------------------------------------
 * struct idpf_rss_data — RSS key, LUT, and hash configuration.
 * [IDPF:A13-A14] [LOCAL:A24]
 * ----------------------------------------------------------------------- */
struct idpf_rss_data {
        uint64_t  rss_hash;
        uint16_t  rss_key_size;
        uint8_t  *rss_key;
        uint16_t  rss_lut_size;
        uint32_t *rss_lut;
        uint32_t *cached_lut;
};

/* -----------------------------------------------------------------------
 * struct idpf_q_coalesce — per-queue interrupt coalescing configuration.
 * Used to restore user settings after a reset.  [LOCAL:A18]
 * ----------------------------------------------------------------------- */
struct idpf_q_coalesce {
        uint32_t tx_intr_mode;
        uint32_t rx_intr_mode;
        uint32_t tx_coalesce_usecs;
        uint32_t rx_coalesce_usecs;
};

/* -----------------------------------------------------------------------
 * struct idpf_vport_user_config_data — user-visible per-vport configuration.
 *
 * XDP / AF_XDP fields removed (not in scope).
 * ETF fields removed (Linux-only).
 * DECLARE_BITMAP replaced by uint64_t bitmasks.
 * list_head replaced by TAILQ_HEAD.
 * [LOCAL:A18] [FBSD15:A30]
 * ----------------------------------------------------------------------- */
struct idpf_vport_user_config_data {
        struct idpf_rss_data     rss_data;
        struct idpf_q_coalesce  *q_coalesce;
        uint16_t                 num_req_tx_qs;
        uint16_t                 num_req_rx_qs;
        uint32_t                 num_req_txq_desc;
        uint32_t                 num_req_rxq_desc;
        uint64_t                 user_flags;    /* idpf_user_flags bits */
        TAILQ_HEAD(idpf_mac_filter_head, idpf_mac_filter) mac_filter_list;
        uint32_t                 num_fsteer_fltrs;
        TAILQ_HEAD(idpf_fsteer_fltr_head, idpf_fsteer_fltr) flow_steer_list;
};

/* -----------------------------------------------------------------------
 * enum idpf_vport_config_flags — vport configuration flags.
 * Stored in idpf_vport_config.flags (uint32_t).  [LOCAL:A18]
 * ----------------------------------------------------------------------- */
enum idpf_vport_config_flags {
        IDPF_VPORT_REG_NETDEV,
        IDPF_VPORT_UP_REQUESTED,
        IDPF_VPORT_UPLINK_PORT,
        IDPF_VPORT_CONFIG_FLAGS_NBITS,
};

_Static_assert(IDPF_VPORT_CONFIG_FLAGS_NBITS <= 32,
    "idpf_vport_config_flags exceeds uint32_t width");

/* -----------------------------------------------------------------------
 * struct idpf_avail_queue_info — available queue counts after vport alloc.
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_avail_queue_info {
        uint16_t avail_rxq;
        uint16_t avail_txq;
        uint16_t avail_bufq;
        uint16_t avail_complq;
};

/* -----------------------------------------------------------------------
 * struct idpf_vector_info — utility struct for vector distribution.
 * [LOCAL:A21]
 * ----------------------------------------------------------------------- */
struct idpf_vector_info {
        uint16_t num_req_vecs;
        uint16_t num_curr_vecs;
        uint16_t index;
        bool     default_vport;
};

/* -----------------------------------------------------------------------
 * struct idpf_vector_lifo — vector-index stack for distribution algorithm.
 * [LOCAL:A21]
 * ----------------------------------------------------------------------- */
struct idpf_vector_lifo {
        uint16_t  top;
        uint16_t  base;
        uint16_t  size;
        uint16_t *vec_idx;
};

/* -----------------------------------------------------------------------
 * struct idpf_queue_id_reg_chunk — individual queue-ID / register chunk.
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_queue_id_reg_chunk {
        uint64_t qtail_reg_start;
        uint32_t qtail_reg_spacing;
        uint32_t type;
        uint32_t start_queue_id;
        uint32_t num_queues;
};

/* -----------------------------------------------------------------------
 * struct idpf_queue_id_reg_info — queue-ID / register chunk collection.
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_queue_id_reg_info {
        uint16_t                       num_chunks;
        struct idpf_queue_id_reg_chunk *queue_chunks;
};

/* -----------------------------------------------------------------------
 * struct idpf_vport_config — per-vport configuration and resource record.
 *
 * idpf_vec_affinity_config removed (Linux cpumask / irq_affinity_notify).
 * spinlock_t replaced by struct mtx.
 * DECLARE_BITMAP replaced by uint32_t.
 * [FBSD15:A32] [LOCAL:A18]
 * ----------------------------------------------------------------------- */
struct idpf_vport_config {
        struct idpf_vport_user_config_data  user_config;
        struct idpf_vport_max_q             max_q;
        struct idpf_queue_id_reg_info       qid_reg_info;
        struct mtx                          mac_filter_list_lock;
        struct mtx                          flow_steer_list_lock;
        uint32_t                            flags; /* idpf_vport_config_flags */
};

/* -----------------------------------------------------------------------
 * Iteration helper
 * ----------------------------------------------------------------------- */
#define idpf_for_each_vport(adapter, i) \
        for ((i) = 0; (i) < (adapter)->num_alloc_vports; (i)++)

/* -----------------------------------------------------------------------
 * struct idpf_adapter — top-level per-device context.
 *
 * struct pci_dev * replaced by device_t (FreeBSD newbus).
 * struct net_device ** replaced by if_ctx_t * array (iflib contexts).
 * struct msix_entry * replaced by struct resource ** (FreeBSD IRQ resources).
 * RDMA / RCA msix fields removed (CONDITIONAL / DELEGATED).
 * VDCM/MDEV surface removed; adi_info retained as a TAILQ registry because
 *   dev_ops.notify_adi_reset still resolves ADIs by identifier.
 * IDC cdev_info removed (DELEGATED).
 * devlink sf_* fields removed (devlink Linux-only).
 * struct mutex replaced by struct sx (sleepable, process context).
 * struct delayed_work replaced by struct callout (periodic) or
 *   struct task + struct taskqueue (deferred one-shot).
 * struct workqueue_struct * replaced by struct taskqueue *.
 * struct completion corer_done replaced by struct cv + struct mtx.
 * OEM caps field removed (CONFIG_OEM_CAPS / CONFIG_P2P Linux-only).
 * num_vfs removed (SR-IOV PF CONDITIONAL, not baseline VF-DPF).
 * irq_mb_handler: Linux irqreturn_t replaced by driver_filter_t *.
 * [FBSD15:A30-A34] [LOCAL:A20-A25] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
struct idpf_adapter {
        /*
         * dev: FreeBSD newbus device handle (replaces struct pci_dev *).
         * [FBSD15:A30]
         */
        device_t                         dev;

        const char                      *drv_name;
        const char                      *drv_ver;

        uint32_t                         virt_ver_maj;
        uint32_t                         virt_ver_min;
        uint32_t                         mb_wait_count;

        enum idpf_state                  state;

        /*
         * flags: adapter-level state bits (idpf_flags).
         * Plain uint32_t replaces Linux DECLARE_BITMAP.
         * Access: adapter->flags & (1u << IDPF_HR_RESET_IN_PROG)
         * [FBSD15:A32]
         */
        uint32_t                         flags;

        struct idpf_reset_reg            reset_reg;
        struct idpf_hw                   hw;

        /*
         * MSI-X resources.
         * struct msix_entry replaced by struct resource * array.
         * num_avail_msix: vectors available for datapath use.
         * num_msix_entries: total allocated (including mailbox vector).
         * msix_entries: array of num_msix_entries IRQ resource pointers.
         * [FBSD15:A34]
         */
        uint16_t                         num_avail_msix;
        uint16_t                         num_msix_entries;
        struct resource                **msix_entries;

        struct idpf_adi_info             adi_info;

        struct virtchnl2_alloc_vectors  *req_vec_chunks;
        struct idpf_q_vector             mb_vector;
        struct idpf_vector_lifo          vector_stack;

        /*
         * irq_mb_handler: mailbox hard-interrupt filter function.
         * Linux irqreturn_t (*)(int, void *) replaced by FreeBSD
         * driver_filter_t * (int (*)(void *)).  [FBSD15:A34]
         */
        driver_filter_t                 *irq_mb_handler;

        /*
         * mb_intr_tag: cookie from bus_setup_intr() for the mailbox vector,
         * required to tear the handler down again.  [FBSD15:A34]
         */
        void                            *mb_intr_tag;

        uint32_t                         tx_timeout_count;
        struct idpf_avail_queue_info     avail_queues;

        struct idpf_vport              **vports;

        /*
         * iflib_ctxs: per-vport iflib software contexts.
         * Replaces struct net_device ** netdevs.  [FBSD15:A30]
         */
        if_ctx_t                        *iflib_ctxs;

	/* Interface being attached, before any vport exists to own it. */
	if_ctx_t			attach_ctx;

        struct virtchnl2_create_vport  **vport_params_recvd;
        uint32_t                        *vport_ids;

        struct idpf_rx_ptype_decoded    *singleq_pt_lkup;
        struct idpf_rx_ptype_decoded    *splitq_pt_lkup;

        struct idpf_vport_config       **vport_config;
        uint16_t                         max_vports;
        uint16_t                         num_alloc_vports;
        uint16_t                         next_vport;

        /*
         * Deferred-work infrastructure.
         * Linux delayed_work / workqueue_struct replaced by:
         *   periodic tasks          → struct callout      + struct taskqueue *
         *   one-shot tasks          → struct task         + struct taskqueue *
         *   one-shot delayed tasks  → struct timeout_task + struct taskqueue *
         *
         * struct timeout_task is FreeBSD's direct analogue of Linux
         * delayed_work: it arms a callout that then enqueues the task on the
         * taskqueue, so the handler still runs in a context that may sleep.
         * Its handler signature is void (*)(void *ctx, int pending), which is
         * why idpf_init_task() and idpf_vc_event_task() are declared that way.
         * [FBSD15:A32-A33]
         */
        struct timeout_task              init_task;     /* init_wq equiv */
        struct taskqueue                *init_wq;
        struct callout                   serv_task;     /* serv_wq equiv */
        struct taskqueue                *serv_wq;
        struct task                      mbx_task;      /* mbx_wq equiv */
        struct taskqueue                *mbx_wq;
        struct timeout_task              vc_event_task; /* vc_event_wq equiv */
        struct taskqueue                *vc_event_wq;
        struct callout                   stats_task;    /* stats_wq equiv */
        struct taskqueue                *stats_wq;

        /*
         * The statistics and mailbox work sleeps on the mailbox, so its
         * callout only hands the work to a taskqueue thread.
         */
        struct task                      stats_deferred;
        struct callout                   mbx_poll_task;

        struct virtchnl2_get_capabilities caps;         /* [IDPF:A13-A14] */
        struct virtchnl2_vlan_get_caps    vlan_caps;    /* [IDPF:A13-A14] */
        struct idpf_vc_xn_manager        *vcxn_mngr;

        struct virtchnl2_edt_caps         edt_caps;    /* [IDPF:A13-A14] */

        struct idpf_dev_ops               dev_ops;

        bool                              req_tx_splitq;
        bool                              req_rx_splitq;
        bool                              crc_enable;

        /*
         * Control locks.
         * Linux struct mutex replaced by struct sx (sleepable, allows
         * msleep-equivalent operations inside the critical section).
         * [FBSD15:A32]
         */
        struct sx                         vport_ctrl_lock;
        struct sx                         vector_lock;
        struct sx                         queue_lock;

        /*
         * ptp: CONDITIONAL (feature 217 — PTP time synchronization).
         * NULL when PTP is not negotiated.  [ON_HOLD — see §23.4]
         */
        struct idpf_ptp                  *ptp;
        uint32_t                          tx_compl_tstamp_gran_s;

        /*
         * corer_done: CORER completion notification.
         * Linux struct completion replaced by condvar + mutex pair.
         * [FBSD15:A32-A33]
         */
        struct mtx                        corer_done_lock;
        struct cv                         corer_done_cv;
        /* Set from the mailbox interrupt filter, so accessed atomically. */
        volatile u_int                    corer_done_flag;
};

/* -----------------------------------------------------------------------
 * Inline helpers
 * ----------------------------------------------------------------------- */

/**
 * idpf_is_queue_model_split - check if queue model is split
 * @q_model: queue model single or split
 *
 * Returns non-zero if queue model is split, zero otherwise.
 * [IDPF:A13-A14]
 */
static inline int
idpf_is_queue_model_split(uint16_t q_model)
{
        return (q_model == VIRTCHNL2_QUEUE_MODEL_SPLIT);
}

/**
 * idpf_is_capability_ena - check whether a capability flag is set
 * @adapter: private data struct
 * @all: if true, all bits in @flag must be set; if false, any bit suffices
 * @field: which capability field to inspect (enum idpf_cap_field)
 * @flag: capability bitmask to test
 *
 * Declared here; defined in idpf_lib.c.  [IDPF:A13-A14]
 */
bool idpf_is_capability_ena(struct idpf_adapter *adapter, bool all,
                             enum idpf_cap_field field, uint64_t flag);

/**
 * idpf_get_reserved_vecs - get the number of vectors reserved by CP
 * @adapter: private data struct
 * [IDPF:A13-A14]
 */
static inline uint16_t
idpf_get_reserved_vecs(struct idpf_adapter *adapter)
{
        return le16toh(adapter->caps.num_allocated_vectors);
}

/**
 * idpf_get_default_vports - get the default number of vports
 * @adapter: private data struct
 * [IDPF:A13-A14]
 */
static inline uint16_t
idpf_get_default_vports(struct idpf_adapter *adapter)
{
	uint16_t nvports = le16toh(adapter->caps.default_num_vports);

	/*
	 * iflib creates one ifnet per PCI attach and only that vport owns an
	 * if_ctx_t, so the control plane's larger default cannot be honoured
	 * without separate device instances this port does not create.
	 */
	return (nvports > 1 ? 1 : nvports);
}

/**
 * idpf_get_max_vports - get the maximum number of vports
 * @adapter: private data struct
 * [IDPF:A13-A14]
 */
static inline uint16_t
idpf_get_max_vports(struct idpf_adapter *adapter)
{
        return le16toh(adapter->caps.max_vports);
}

/**
 * idpf_get_max_tx_bufs - get max scatter-gather buffers per TX packet
 * @adapter: private data struct
 * [IDPF:A13-A14]
 */
static inline unsigned int
idpf_get_max_tx_bufs(struct idpf_adapter *adapter)
{
        return adapter->caps.max_sg_bufs_per_tx_pkt;
}

/**
 * idpf_get_min_tx_pkt_len - get minimum TX packet length
 * @adapter: private data struct
 * [IDPF:A13-A14]
 */
static inline uint8_t
idpf_get_min_tx_pkt_len(struct idpf_adapter *adapter)
{
        uint8_t pkt_len = adapter->caps.min_sso_packet_len;

        return pkt_len ? pkt_len : IDPF_TX_MIN_PKT_LEN;
}

/**
 * idpf_reg_offset_in_region - check that a u32 access fits inside a BAR region
 * @region: mapped LAN BAR region descriptor
 * @reg_offset: register offset to test
 *
 * Returns true when the full u32 access falls inside @region.
 * [FBSD15:A31] [LOCAL:A22-A23]
 */
static inline bool
idpf_reg_offset_in_region(const struct idpf_mmio_reg *region,
                           bus_size_t reg_offset)
{
        if (reg_offset < region->addr_start ||
            region->addr_len < sizeof(uint32_t))
                return false;

        return reg_offset - region->addr_start <=
               region->addr_len - sizeof(uint32_t);
}

/**
 * idpf_reg_offset_is_mapped - check whether a BAR0 register offset is mapped
 * @adapter: private data struct
 * @reg_offset: register offset to test
 *
 * Returns true when the full u32 access falls inside a mapped LAN BAR region.
 * [FBSD15:A31] [LOCAL:A22-A23]
 */
static inline bool
idpf_reg_offset_is_mapped(struct idpf_adapter *adapter,
                           bus_size_t reg_offset)
{
        struct idpf_hw *hw = &adapter->hw;

        for (int i = 0; i < hw->num_lan_regs; i++) {
                if (idpf_reg_offset_in_region(&hw->lan_regs[i], reg_offset))
                        return true;
        }
        return false;
}

/**
 * idpf_get_mbx_reg_addr - get the mapped mailbox register address
 * @adapter: private data struct
 * @reg_offset: register offset within the mailbox region
 *
 * Returns the host-virtual address of the mailbox register.
 * Callers must use bus_space accessors, not direct pointer dereference.
 * [FBSD15:A31] [LOCAL:A22-A23]
 */
static inline void *
idpf_get_mbx_reg_addr(struct idpf_adapter *adapter, bus_size_t reg_offset)
{
        return (uint8_t *)adapter->hw.mbx.vaddr + reg_offset;
}

/**
 * idpf_get_rstat_reg_addr - get the mapped rstat register address
 * @adapter: private data struct
 * @reg_offset: absolute register offset (will be made region-relative)
 *
 * Returns the host-virtual address of the rstat register.
 * Callers must use bus_space accessors, not direct pointer dereference.
 * [FBSD15:A31] [LOCAL:A22-A23]
 */
static inline void *
idpf_get_rstat_reg_addr(struct idpf_adapter *adapter, bus_size_t reg_offset)
{
        reg_offset -= adapter->hw.rstat.addr_start;
        return (uint8_t *)adapter->hw.rstat.vaddr + reg_offset;
}

/**
 * idpf_get_reg_addr - get the mapped BAR0 register address for any region
 * @adapter: private data struct
 * @reg_offset: register offset value
 *
 * Walks the LAN region array and returns the host-virtual address.
 * Callers must use bus_space accessors, not direct pointer dereference.
 * Panics if the offset does not fall within any mapped region — this
 * mirrors the Linux BUG() behaviour and makes the fault site explicit.
 * [FBSD15:A31] [LOCAL:A22-A23]
 */
static inline void *
idpf_get_reg_addr(struct idpf_adapter *adapter, bus_size_t reg_offset)
{
        struct idpf_hw *hw = &adapter->hw;

        for (int i = 0; i < hw->num_lan_regs; i++) {
                struct idpf_mmio_reg *region = &hw->lan_regs[i];

                if (idpf_reg_offset_in_region(region, reg_offset)) {
                        reg_offset -= region->addr_start;
                        return (uint8_t *)region->vaddr + reg_offset;
                }
        }

        /*
         * No region matched.  This should never happen with offsets from CP.
         * panic() here makes the fault site explicit rather than allowing a
         * NULL-pointer dereference to produce a harder-to-diagnose trap.
         * [FBSD15:A31] — mirrors Linux BUG() intent.
         */
        panic("idpf_get_reg_addr: offset 0x%jx not in any mapped BAR region",
              (uintmax_t)reg_offset);
}

/**
 * idpf_is_reset_in_prog - check whether any reset is in progress
 * @adapter: private data struct
 *
 * Returns true if a hard reset, function reset, or driver-load reset
 * is currently active.  [LOCAL:A20-A21]
 */
static inline bool
idpf_is_reset_in_prog(struct idpf_adapter *adapter)
{
        return (adapter->flags & ((1u << IDPF_HR_RESET_IN_PROG) |
                                  (1u << IDPF_HR_FUNC_RESET)    |
                                  (1u << IDPF_HR_DRV_LOAD))) != 0;
}

/**
 * idpf_is_resource_rel_in_prog - check whether resource release is in progress
 * @adapter: private data struct
 *
 * Returns true if reset or driver removal is in progress.
 * [LOCAL:A20-A21]
 */
static inline bool
idpf_is_resource_rel_in_prog(struct idpf_adapter *adapter)
{
        return (adapter->flags & ((1u << IDPF_HR_RESET_IN_PROG) |
                                  (1u << IDPF_HR_FUNC_RESET)    |
                                  (1u << IDPF_REMOVE_IN_PROG))) != 0;
}

/**
 * idpf_netdev_to_vport - get the vport handle from an iflib context
 * @ctx: iflib software context
 *
 * Replaces the Linux netdev_priv() pattern.  [FBSD15:A30]
 */
static inline struct idpf_vport *
idpf_netdev_to_vport(if_ctx_t ctx)
{
        struct idpf_netdev_priv *np = iflib_get_softc(ctx);

        return np->vport;
}

/**
 * idpf_netdev_to_adapter - get the adapter handle from an iflib context
 * @ctx: iflib software context
 *
 * Replaces the Linux netdev_priv() pattern.  [FBSD15:A30]
 */
static inline struct idpf_adapter *
idpf_netdev_to_adapter(if_ctx_t ctx)
{
        struct idpf_netdev_priv *np = iflib_get_softc(ctx);

        return np->adapter;
}

/**
 * idpf_adapter_to_dev - get the FreeBSD device_t from an adapter
 * @adapter: private data struct
 * [FBSD15:A30]
 */
static inline device_t
idpf_adapter_to_dev(struct idpf_adapter *adapter)
{
        return adapter->dev;
}

/**
 * idpf_vport_ctrl_lock - acquire the vport control lock
 * @adapter: private data struct
 *
 * Protects non-datapath code against vport destruction.
 * Caller must be in process context (sx_xlock may sleep).
 * [FBSD15:A32]
 */
static inline void
idpf_vport_ctrl_lock(struct idpf_adapter *adapter)
{
        sx_xlock(&adapter->vport_ctrl_lock);
}

/**
 * idpf_vport_ctrl_unlock - release the vport control lock
 * @adapter: private data struct
 * [FBSD15:A32]
 */
static inline void
idpf_vport_ctrl_unlock(struct idpf_adapter *adapter)
{
        sx_xunlock(&adapter->vport_ctrl_lock);
}

/**
 * idpf_is_feature_ena - check whether a negotiated capability is enabled
 * @vport: vport to check
 * @cap: iflib/ifnet capability flag (e.g. IFCAP_TXCSUM)
 *
 * FreeBSD uses if_getcapenable() rather than netdev->features.
 * Returns true if the capability is currently enabled on the interface.
 * [FBSD15:A30]
 */
static inline bool
idpf_is_feature_ena(struct idpf_vport *vport, int cap)
{
        if (vport->ifp == NULL)
                return false;
        return (if_getcapenable(vport->ifp) & cap) != 0;
}

/**
 * idpf_get_vc_xn_min_timeout - get minimum VC transaction timeout in msec
 * @adapter: private data struct
 *
 * Emulation and simulation platforms answer far more slowly than silicon.
 */
static inline int
idpf_get_vc_xn_min_timeout(struct idpf_adapter *adapter)
{
        struct idpf_hw *hw = &adapter->hw;

        if (IS_EMR_DEVICE(hw->subsystem_device_id))
                return (120 * 1000);
        else if (IS_SIMICS_DEVICE(hw->subsystem_device_id))
                return (6 * 1000);

        return 2000;
}

/**
 * idpf_get_vc_xn_default_timeout - get default VC transaction timeout in msec
 * @adapter: private data struct
 */
static inline int
idpf_get_vc_xn_default_timeout(struct idpf_adapter *adapter)
{
        struct idpf_hw *hw = &adapter->hw;

        if (IS_EMR_DEVICE(hw->subsystem_device_id))
                return (120 * 1000);
        else
                return (60 * 1000);
}

/* -----------------------------------------------------------------------
 * Function declarations
 * Core method names are NOT renamed per architectural constraint.
 * [LOCAL:A20-A25] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */

/* Periodic / deferred work entry points */
void idpf_statistics_task(void *arg, int pending);
void idpf_statistics_task_cb(void *arg);
void idpf_init_task(void *arg, int pending);
void idpf_service_task(void *arg);
void idpf_mbx_task(void *arg, int pending);
void idpf_mbx_task_cb(void *arg);
void idpf_vc_event_task(void *arg, int pending);

/* Interface configuration */
int  idpf_vport_cfg_ifp(struct idpf_vport *vport);

/* iflib device-method table implemented in idpf_lib.c. */
extern driver_t idpf_if_driver;

/* Device lifecycle ifdi methods, implemented in idpf_main.c. */
int  idpf_if_attach_pre(if_ctx_t ctx);
int  idpf_if_attach_post(if_ctx_t ctx);
int  idpf_if_detach(if_ctx_t ctx);
int  idpf_if_shutdown(if_ctx_t ctx);
int  idpf_if_suspend(if_ctx_t ctx);
int  idpf_if_resume(if_ctx_t ctx);

/* Device-ops initializers */
void idpf_dev_ops_init(struct idpf_adapter *adapter);
void idpf_vf_dev_ops_init(struct idpf_adapter *adapter);

/* Queue and vector management */
void idpf_vport_adjust_qs(struct idpf_vport *vport,
                           struct idpf_q_vec_rsrc *rsrc);
int  idpf_intr_req(struct idpf_adapter *adapter);
void idpf_mb_intr_rel_irq(struct idpf_adapter *adapter);
void idpf_intr_rel(struct idpf_adapter *adapter);
int  idpf_req_rel_vector_indexes(struct idpf_adapter *adapter,
                                  uint16_t *q_vector_idxs,
                                  struct idpf_vector_info *vec_info);
int  idpf_vport_alloc_vec_indexes(struct idpf_vport *vport,
                                   struct idpf_q_vec_rsrc *rsrc);
void idpf_vport_dealloc_vec_indexes(struct idpf_vport *vport,
                                     struct idpf_q_vec_rsrc *rsrc);
void idpf_deinit_vector_stack(struct idpf_adapter *adapter);

/* Vport lifecycle */
int  idpf_vport_alloc_max_qs(struct idpf_adapter *adapter,
                               struct idpf_vport_max_q *max_q);
void idpf_vport_dealloc_max_qs(struct idpf_adapter *adapter,
                                 struct idpf_vport_max_q *max_q);
int  idpf_vport_init(struct idpf_vport *vport,
                      struct idpf_vport_max_q *max_q);
void idpf_vport_dealloc(struct idpf_vport *vport);

/* Soft reset */
int  idpf_initiate_soft_reset(struct idpf_vport *vport,
                               enum idpf_vport_reset_cause reset_cause);

/* Adapter lifecycle */
void idpf_deinit_task(struct idpf_adapter *adapter);
void idpf_detach_and_close(struct idpf_adapter *adapter);
void idpf_attach_and_open(struct idpf_adapter *adapter);

/* Reset and recovery */
int  idpf_check_reset_complete(struct idpf_adapter *adapter);
int  idpf_init_hard_reset(struct idpf_adapter *adapter);
int  idpf_get_vlan_caps(struct idpf_adapter *adapter);
int  idpf_reset_recover(struct idpf_adapter *adapter);
bool idpf_is_reset_detected(struct idpf_adapter *adapter);

/* Descriptor and interrupt helpers */
int  idpf_check_supported_desc_ids(struct idpf_vport *vport);
void idpf_vport_intr_write_itr(struct idpf_q_vector *q_vector,
                                 uint16_t itr, bool tx);

/* Promiscuous mode */
int  idpf_set_promiscuous(struct idpf_adapter *adapter,
                           struct idpf_vport_user_config_data *config_data,
                           uint32_t vport_id);

/* Header split (CONDITIONAL — feature 078a / hsplit).
 * Signature uses plain bool; ethtool-netlink variant removed.  [LOCAL:A18] */
void idpf_vport_set_hsplit(struct idpf_vport *vport, bool ena);

/* Statistics task control */
void idpf_stats_task_stop(struct idpf_adapter *adapter);
void idpf_stats_task_start(struct idpf_adapter *adapter);

#endif /* !_IDPF_H_ */