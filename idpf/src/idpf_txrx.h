/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * idpf_txrx.h — FreeBSD >= 15.0 IDPF VF-DPF TX/RX datapath header.
 *
 * CONVERSION NOTES (evidence class per Human Reference Guide Appendix A):
 *
 *  [FBSD15:A30-A34]  FreeBSD 15 kernel interfaces are authoritative.
 *  [IDPF:A13-A14]    Virtchnl2 / IDPF 1.0 protocol structures retained.
 *  [LOCAL:A25]       Current idpf_txrx.c skeleton shape preserved.
 *  [PROPOSED:A38]    TARGET DESIGN items marked; not yet implemented.
 *
 * Removed entirely (Linux-only, no FreeBSD equivalent in scope):
 *   libeth_tx.h, kcompat_dim.h, linux/dim.h, linux/indirect_call_wrapper.h,
 *   net/tcp.h, net/netdev_queues.h, net/xdp.h,
 *   LIBETH_SQE_CHECK_PRIV, idpf_tx_buf_next (libeth),
 *   struct napi_struct, struct dim (DIM algorithm),
 *   cpumask_var_t, XDP/AF_XDP fields and functions,
 *   IDPF_XDP_* constants, IDPF_RX_DMA_ATTR (Linux DMA attrs),
 *   IDPF_SKB_HEAD_SIZE / IDPF_RX_HDR_SIZE (skb-specific),
 *   all HAVE_* / CONFIG_* / DEVLINK_ENABLED / IDPF_ADD_PROBES guards,
 *   enum libeth_sqe_type_ext (replaced by idpf_sqe_type),
 *   idpf_tx_extra_counters, idpf_rx_extra_counters (probe-only),
 *   idpf_rx_xdp, idpf_prepare_xdp_tx_*, idpf_xdp_rxq_init,
 *   idpf_xdpq_update_tail, idpf_finalize_xdp_rx, idpf_xmit_xdpq,
 *   INDIRECT_CALLABLE_DECLARE.
 *
 * Core method names are NOT renamed per architectural constraint.
 */

#ifndef _IDPF_TXRX_H_
#define _IDPF_TXRX_H_

/*
 * Types owned by idpf.h, which includes this header before defining them.
 */
struct idpf_adapter;
struct idpf_vport;
struct idpf_q_vec_rsrc;
struct idpf_rss_data;
struct idpf_vport_max_q;
struct idpf_vport_config;
struct idpf_queue_id_reg_info;

/* -----------------------------------------------------------------------
 * FreeBSD kernel headers  [FBSD15:A30-A34]
 * ----------------------------------------------------------------------- */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/callout.h>

#include <machine/bus.h>
#include <machine/param.h>      /* CACHE_LINE_SIZE */
#include <machine/resource.h>

#include <vm/vm.h>
#include <vm/vm_page.h>

#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/iflib.h>

/* -----------------------------------------------------------------------
 * Shared IDPF / Virtchnl2 protocol headers (carried source).
 * [IDPF:A13-A14] [LOCAL:A17]
 * ----------------------------------------------------------------------- */
#include "idpf_lan_txrx.h"
#include "virtchnl2_lan_desc.h"

/* -----------------------------------------------------------------------
 * Wire-field helpers
 *
 * The carried virtchnl2 and idpf_lan_txrx headers express descriptor fields
 * as GENMASK()/BIT() masks.  These replace the Linux FIELD_GET()/FIELD_PREP()
 * macros without pulling in <linux/bitfield.h>.  __builtin_ffsll() folds to a
 * constant for the constant masks used throughout the datapath.
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_FIELD_GET(mask, val) \
        (((val) & (mask)) >> (__builtin_ffsll(mask) - 1))
#define IDPF_FIELD_PREP(mask, val) \
        ((((uint64_t)(val)) << (__builtin_ffsll(mask) - 1)) & (mask))

/**
 * idpf_reg_wr32 - MMIO seam for the datapath doorbell and ITR registers
 * @addr: mapped register address from idpf_get_reg_addr() and friends
 * @value: value to publish
 *
 * idpf.h hands back host-virtual addresses inside the linear BAR0 mapping
 * created during attach, so a volatile 32-bit store is the
 * bus_space_write_4() equivalent for memory space on every architecture this
 * driver targets.  The release fence orders every prior descriptor store
 * ahead of the doorbell.  [FBSD15:A31]
 */
static inline void
idpf_reg_wr32(void *addr, uint32_t value)
{

        atomic_thread_fence_rel();
        *(volatile uint32_t *)addr = htole32(value);
}

/**
 * idpf_reg_rd32 - MMIO read seam
 * @addr: mapped register address from idpf_get_reg_addr() and friends
 *
 * The acquire fence keeps loads issued after the register read from being
 * hoisted above it.  [FBSD15:A31]
 */
static inline uint32_t
idpf_reg_rd32(void *addr)
{
        uint32_t value;

        value = le32toh(*(volatile uint32_t *)addr);
        atomic_thread_fence_acq();

        return (value);
}

/**
 * idpf_ring_next - advance a descriptor index with an explicit wrap
 * @idx: current index
 * @count: ring size
 *
 * IDPF only requires descriptor counts to be a multiple of 32, not a power of
 * two, so mask-based wrapping is not safe here.  [IDPF:A13-A14]
 */
static inline uint16_t
idpf_ring_next(uint16_t idx, uint16_t count)
{

        return (++idx == count ? 0 : idx);
}

/**
 * idpf_ring_delta - forward distance between two ring indices
 * @from: older index
 * @to: newer index
 * @count: ring size
 */
static inline uint16_t
idpf_ring_delta(uint16_t from, uint16_t to, uint16_t count)
{

        return (to >= from ? to - from : (uint16_t)(count - from + to));
}

/* -----------------------------------------------------------------------
 * Queue count limits  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_LARGE_MAX_Q                256
#define IDPF_MAX_TXQ                    IDPF_LARGE_MAX_Q
#define IDPF_MAX_RXQ                    64
#define IDPF_MIN_Q                      2
#define IDPF_DFLT_NUM_Q                 16
#define IDPF_MAX_MBXQ                   1

/* -----------------------------------------------------------------------
 * Descriptor count limits and alignment  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_MIN_TXQ_DESC               128
#define IDPF_MIN_RXQ_DESC               64
#define IDPF_MIN_TXQ_COMPLQ_DESC        256

/*
 * Number of descriptors in a queue must be a multiple of 32.
 * RX queue descriptors alone must be a multiple of IDPF_REQ_RXQ_DESC_MULTIPLE
 * to achieve BufQ descriptors aligned to 32.
 */
#define IDPF_REQ_DESC_MULTIPLE          32
#define IDPF_REQ_RXQ_DESC_MULTIPLE \
        (IDPF_MAX_BUFQS_PER_RXQ_GRP * IDPF_REQ_DESC_MULTIPLE)

/*
 * IDPF_MAX_TX_FRAGS: maximum scatter-gather fragments per TX packet.
 *
 * Linux uses MAX_SKB_FRAGS (typically 17-35 depending on kernel version).
 * FreeBSD has no direct equivalent; 35 is a conservative upper bound
 * matching the maximum mbuf chain depth used by iavf and similar drivers.
 * [PROPOSED:A38] — validate against negotiated max_sg_bufs_per_tx_pkt
 * from GET_CAPS before use in production.  [ON_HOLD — see §16.2]
 */
#define IDPF_MAX_TX_FRAGS               35      /* [PROPOSED:A38] */

#define IDPF_MIN_TX_DESC_NEEDED         (IDPF_MAX_TX_FRAGS + 6)
#define IDPF_TX_WAKE_THRESH             ((uint16_t)(IDPF_MIN_TX_DESC_NEEDED * 2))

#define IDPF_MAX_DESCS                  8160
#define IDPF_MAX_TXQ_DESC \
        rounddown(IDPF_MAX_DESCS, IDPF_REQ_DESC_MULTIPLE)
#define IDPF_MAX_RXQ_DESC \
        rounddown(IDPF_MAX_DESCS, IDPF_REQ_RXQ_DESC_MULTIPLE)

#define MIN_SUPPORT_TXDID ( \
        VIRTCHNL2_TXDID_FLEX_FLOW_SCHED | \
        VIRTCHNL2_TXDID_FLEX_TSO_CTX)

/* -----------------------------------------------------------------------
 * Queue group defaults  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_DFLT_SINGLEQ_TX_Q_GROUPS  1
#define IDPF_DFLT_SINGLEQ_RX_Q_GROUPS  1
#define IDPF_COMPLQ_PER_GROUP           1
#define IDPF_SINGLE_BUFQ_PER_RXQ_GRP   1
#define IDPF_DFLT_SPLITQ_RXQ_PER_BUFQ  1
#define IDPF_MAX_BUFQS_PER_RXQ_GRP     2
#define IDPF_NUMQ_PER_CHUNK             1
#define IDPF_DFLT_SPLITQ_TXQ_PER_GROUP 1
#define IDPF_DFLT_SPLITQ_RXQ_PER_GROUP 1

/* -----------------------------------------------------------------------
 * Vector allocation minimums  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_MBX_Q_VEC                  1
#define IDPF_MIN_Q_VEC                  1
/*
 * IDPF_MIN_RDMA_VEC: CONDITIONAL (feature 209 — RDMA coordination).
 * Retained as a constant; not used until RDMA is negotiated.
 */
#define IDPF_MIN_RDMA_VEC               2       /* [CONDITIONAL — feature 209] */

/* -----------------------------------------------------------------------
 * Default descriptor counts  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_DFLT_TX_Q_DESC_COUNT       512
#define IDPF_DFLT_TX_COMPLQ_DESC_COUNT  512
#define IDPF_DFLT_RX_Q_DESC_COUNT       512

/* -----------------------------------------------------------------------
 * Buffer queue descriptor count helpers  [IDPF:A13-A14]
 *
 * IMPORTANT: Total buffers across all buffer queues must never exceed the
 * number of RX completion descriptors.  See Linux source comment for the
 * full rationale; the invariant is identical on FreeBSD.
 * ----------------------------------------------------------------------- */
#define IDPF_RX_BUFQ_DESC_COUNT(RXD, NUM_BUFQ)  ((RXD) / (NUM_BUFQ))
#define IDPF_RX_BUFQ_WORKING_SET(rxq)   ((rxq)->desc_count - 1)
#define IDPF_RX_BUFQ_NON_WORKING_SET(rxq) \
        ((rxq)->desc_count - IDPF_RX_BUFQ_WORKING_SET(rxq))

/* -----------------------------------------------------------------------
 * RX buffer sizes  [IDPF:A13-A14]
 *
 * IDPF_SKB_HEAD_SIZE and IDPF_RX_HDR_SIZE removed: they were defined in
 * terms of Linux skb internals (NET_SKB_PAD, NET_IP_ALIGN, skb_shared_info).
 * FreeBSD RX buffer sizing is derived from the validated vport MTU and
 * descriptor profile per §17.2.  [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
#define IDPF_MIN_RX_HDR_SIZE            192
#define IDPF_RX_BUF_2048                2048
#define IDPF_RX_BUF_4096                4096
#define IDPF_RX_BUF_STRIDE              32
#define IDPF_RX_BUF_POST_STRIDE         16
#define IDPF_LOW_WATERMARK              64
/* Header buffer size for header split  [IDPF:A13-A14] */
#define IDPF_HDR_BUF_SIZE               256

/*
 * IDPF_PACKET_HDR_PAD: minimum header overhead added to MTU for buffer sizing.
 * ETH_HLEN  → ETHER_HDR_LEN  (14)  [FBSD15:A30]
 * ETH_FCS_LEN → ETHER_CRC_LEN (4)  [FBSD15:A30]
 * VLAN_HLEN → ETHER_VLAN_ENCAP_LEN (4) [FBSD15:A30]
 */
#define IDPF_PACKET_HDR_PAD \
        (ETHER_HDR_LEN + ETHER_CRC_LEN + ETHER_VLAN_ENCAP_LEN * 2)

#define IDPF_TX_TSO_MIN_MSS             88

/* -----------------------------------------------------------------------
 * TX splitq RE-bit gap  [IDPF:A13-A14]
 * Spec: SW must keep a minimal gap of IDPF_TX_SPLITQ_RE_MIN_GAP descriptors
 * between two descriptors with the RE bit set.
 * ----------------------------------------------------------------------- */
#define IDPF_TX_SPLITQ_RE_MIN_GAP       64

/* -----------------------------------------------------------------------
 * Refill queue generation / buffer-ID bit fields  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_RFL_BI_GEN_M               (1u << 16)
#define IDPF_RFL_BI_BUFID_M             0x0000FFFFu

/* -----------------------------------------------------------------------
 * RX descriptor EOF markers  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_RXD_EOF_SPLITQ    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_EOF_M
#define IDPF_RXD_EOF_SINGLEQ   VIRTCHNL2_RX_BASE_DESC_STATUS_EOF_M

/* -----------------------------------------------------------------------
 * Descriptor ring accessor macros  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_SINGLEQ_RX_BUF_DESC(rxq, i) \
        (&(((struct virtchnl2_singleq_rx_buf_desc *)((rxq)->desc_ring))[i]))
#define IDPF_SPLITQ_RX_BUF_DESC(rxq, i) \
        (&(((struct virtchnl2_splitq_rx_buf_desc *)((rxq)->desc_ring))[i]))
#define IDPF_SPLITQ_RX_BI_DESC(rxq, i)  ((((rxq)->ring))[i])

#define IDPF_BASE_TX_DESC(txq, i) \
        (&(((struct idpf_base_tx_desc *)((txq)->desc_ring))[i]))
#define IDPF_BASE_TX_CTX_DESC(txq, i) \
        (&(((struct idpf_base_tx_ctx_desc *)((txq)->desc_ring))[i]))
#define IDPF_SPLITQ_TX_COMPLQ_DESC(txcq, i) \
        (&(((struct idpf_splitq_tx_compl_desc *)((txcq)->desc_ring))[i]))
#define IDPF_FLEX_TX_DESC(txq, i) \
        (&(((union idpf_tx_flex_desc *)((txq)->desc_ring))[i]))
#define IDPF_FLEX_TX_CTX_DESC(txq, i) \
        (&(((union idpf_flex_tx_ctx_desc *)((txq)->desc_ring))[i]))

/* -----------------------------------------------------------------------
 * Descriptor availability and completion queue helpers  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_DESC_UNUSED(txq) \
        ((((txq)->next_to_clean > (txq)->next_to_use) ? \
          0 : (txq)->desc_count) + \
         (txq)->next_to_clean - (txq)->next_to_use - 1)

#define IDPF_TX_COMPLQ_OVERFLOW_THRESH(txcq)    ((txcq)->desc_count >> 1)
#define IDPF_TX_COMPLQ_PENDING(txq) \
        ((txq)->num_completions_pending - (txq)->complq->tx.num_completions)

#define IDPF_TX_SPLITQ_MISS_COMPL_TAG   (1u << 15)
#define IDPF_TXBUF_NULL                 UINT32_MAX

#define IDPF_TXD_LAST_DESC_CMD  (IDPF_TX_DESC_CMD_EOP | IDPF_TX_DESC_CMD_RS)

/* -----------------------------------------------------------------------
 * TX offload flags  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_TX_FLAGS_TSO       (1u << 0)
#define IDPF_TX_FLAGS_IPV4      (1u << 1)
#define IDPF_TX_FLAGS_IPV6      (1u << 2)
#define IDPF_TX_FLAGS_TUNNEL    (1u << 3)
#define IDPF_TX_FLAGS_TSYN      (1u << 4)

/* -----------------------------------------------------------------------
 * TX descriptor size limits  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_TX_COMPLQ_CLEAN_BUDGET     256
#define IDPF_TX_MIN_PKT_LEN             17
#define IDPF_TX_DESCS_FOR_SKB_DATA_PTR  1
#define IDPF_TX_DESCS_PER_CACHE_LINE \
        (CACHE_LINE_SIZE / sizeof(struct idpf_flex_tx_desc))
#define IDPF_TX_DESCS_FOR_CTX           1
/* TX descriptors needed, worst case: */
#define IDPF_TX_DESC_NEEDED \
        (IDPF_MAX_TX_FRAGS + IDPF_TX_DESCS_FOR_CTX + \
         IDPF_TX_DESCS_PER_CACHE_LINE + IDPF_TX_DESCS_FOR_SKB_DATA_PTR)

/*
 * TX buffer size limits.
 * SZ_4K / SZ_16K replaced by numeric literals.  [FBSD15:A30]
 */
/*
 * Every checksum offload iflib can request on TX.  FreeBSD has no single
 * CSUM_OFFLOAD mask covering both address families.
 */
#define IDPF_CSUM_OFFLOAD						\
	(CSUM_IP | CSUM_IP_UDP | CSUM_IP_TCP | CSUM_IP_SCTP |		\
	 CSUM_IP6_UDP | CSUM_IP6_TCP | CSUM_IP6_SCTP)

#define IDPF_TX_MAX_READ_REQ_SIZE       4096
#define IDPF_TX_MAX_DESC_DATA           (16384 - 1)
#define IDPF_TX_MAX_DESC_DATA_ALIGNED \
        rounddown(IDPF_TX_MAX_DESC_DATA, IDPF_TX_MAX_READ_REQ_SIZE)

/* -----------------------------------------------------------------------
 * RX descriptor accessor  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_RX_DESC(rxq, i) \
        (&(((union virtchnl2_rx_desc *)((rxq)->desc_ring))[i]))

/* -----------------------------------------------------------------------
 * PTYPE constants  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_RX_MAX_PTYPE_PROTO_IDS     32
#define IDPF_RX_MAX_PTYPE_SZ \
        (sizeof(struct virtchnl2_ptype) + \
         (sizeof(uint16_t) * (IDPF_RX_MAX_PTYPE_PROTO_IDS - 1)))
#define IDPF_RX_PTYPE_HDR_SZ    (sizeof(struct virtchnl2_get_ptype_info))
#define IDPF_RX_MAX_PTYPES_PER_BUF \
        ((IDPF_CTLQ_MAX_BUF_LEN - IDPF_RX_PTYPE_HDR_SZ) / IDPF_RX_MAX_PTYPE_SZ)

#define IDPF_GET_PTYPE_SIZE(p) \
        (sizeof(*(p)) + sizeof(uint16_t) * ((p)->proto_id_count - 1))

#define IDPF_TUN_IP_GRE ( \
        IDPF_PTYPE_TUNNEL_IP | \
        IDPF_PTYPE_TUNNEL_IP_GRENAT)

#define IDPF_TUN_IP_GRE_MAC ( \
        IDPF_TUN_IP_GRE | \
        IDPF_PTYPE_TUNNEL_IP_GRENAT_MAC)

#define IDPF_RX_MAX_PTYPE       1024
#define IDPF_RX_MAX_BASE_PTYPE  256
#define IDPF_INVALID_PTYPE_ID   0xFFFF

/* -----------------------------------------------------------------------
 * ITR constants  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_ITR_DYNAMIC        1
#define IDPF_ITR_MAX            0x1FE0
#define IDPF_ITR_20K            0x0032
#define IDPF_ITR_GRAN_S         1       /* ITR granularity 2 µs */
#define IDPF_ITR_MASK           0x1FFE
#define ITR_REG_ALIGN(setting)  ((setting) & IDPF_ITR_MASK)
#define IDPF_ITR_IS_DYNAMIC(itr_mode)   (itr_mode)
#define IDPF_ITR_TX_DEF         IDPF_ITR_20K
#define IDPF_ITR_RX_DEF         IDPF_ITR_20K
#define IDPF_SW_ITR_UPDATE_IDX  2
#define IDPF_NO_ITR_UPDATE_IDX  3
#define IDPF_ITR_IDX_SPACING(spacing, dflt) ((spacing) ? (spacing) : (dflt))
#define IDPF_DIM_DEFAULT_PROFILE_IX     1

/* Maximum number of segments supported by RSC and TSO  [IDPF:A13-A14] */
#define IDPF_MAX_SEGS           16

/* -----------------------------------------------------------------------
 * TX timestamp invalid index  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
#define IDPF_TX_TSTAMP_INVALID_IDX      0xFF

/* -----------------------------------------------------------------------
 * enum idpf_sqe_type — SQE (send-queue entry) type codes.
 *
 * Replaces Linux enum libeth_sqe_type_ext.  The libeth dependency is
 * removed; these values are driver-internal and do not cross any ABI.
 * [LOCAL:A25] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
enum idpf_sqe_type {
        IDPF_SQE_EMPTY          = 0,
        IDPF_SQE_MBUF,                  /* normal mbuf TX */
        IDPF_SQE_MBUF_TSTAMP,           /* PTP mbuf (CONDITIONAL — feature 253) */
        IDPF_SQE_MISS,                  /* exception path: unmap DMA only */
        IDPF_SQE_REINJECT,              /* exception path: consume mbuf, update stats */
};

/* -----------------------------------------------------------------------
 * struct idpf_tx_buf — per-descriptor TX buffer tracking.
 *
 * Replaces Linux libeth_sqe.  mbuf replaces sk_buff.
 * bus_addr_t replaces dma_addr_t.  [FBSD15:A31]
 * ----------------------------------------------------------------------- */
struct idpf_tx_buf {
        struct mbuf     *mbuf;          /* owned mbuf; NULL when empty */
        bus_addr_t       dma;           /* DMA address of first segment */
        bus_dmamap_t     map;           /* busdma map for this buffer */
        uint32_t         bytecount;     /* bytes in this buffer */
        uint16_t         gso_segs;      /* GSO segment count */
        enum idpf_sqe_type type;        /* SQE type */
        uint32_t         priv;          /* next-buffer linkage (splitq) */
};

/* -----------------------------------------------------------------------
 * union idpf_tx_flex_desc  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
union idpf_tx_flex_desc {
        struct idpf_flex_tx_desc        q;      /* queue-based scheduling */
        struct idpf_flex_tx_sched_desc  flow;   /* flow-based scheduling */
};

/* -----------------------------------------------------------------------
 * struct idpf_tx_offload_params  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_tx_offload_params {
        uint32_t tx_flags;
        uint32_t hdr_offsets;
        uint32_t cd_tunneling;
        uint32_t tso_len;
        uint16_t mss;
        uint16_t tso_segs;
        uint16_t tso_hdr_len;
        uint16_t td_cmd;
        uint8_t  desc_ts[3];    /* flow scheduling timestamp */
};

/* -----------------------------------------------------------------------
 * struct idpf_tx_splitq_params  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_tx_splitq_params {
        enum idpf_tx_desc_dtype_value dtype;
        uint16_t eop_cmd;
        union {
                uint32_t compl_tag;
                uint16_t td_tag;
        };
        struct idpf_tx_offload_params offload;
        uint16_t prev_ntu;
        uint16_t prev_refill_ntc;
        bool     prev_refill_gen;
};

/* -----------------------------------------------------------------------
 * struct idpf_reinject_timer — exception-path packet reinjection timer.
 *
 * Linux struct timer_list replaced by struct callout.  [FBSD15:A33]
 * struct sk_buff replaced by struct mbuf *.  [FBSD15:A30]
 * ----------------------------------------------------------------------- */
struct idpf_reinject_timer {
        struct callout          timer;          /* [FBSD15:A33] */
        struct idpf_queue      *txq;
        struct mbuf            *mbuf;           /* [FBSD15:A30] */
        uint32_t                bytes;
        uint16_t                gso_segs;
};

/* TAILQ head for reinject timer list (replaces struct xarray reinject_timers) */
TAILQ_HEAD(idpf_reinject_timer_head, idpf_reinject_timer_entry);

struct idpf_reinject_timer_entry {
        TAILQ_ENTRY(idpf_reinject_timer_entry) link;
        struct idpf_reinject_timer              timer;
};

/* -----------------------------------------------------------------------
 * enum idpf_tx_ctx_desc_eipt_offload  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
enum idpf_tx_ctx_desc_eipt_offload {
        IDPF_TX_CTX_EXT_IP_NONE         = 0x0,
        IDPF_TX_CTX_EXT_IP_IPV6         = 0x1,
        IDPF_TX_CTX_EXT_IP_IPV4_NO_CSUM = 0x2,
        IDPF_TX_CTX_EXT_IP_IPV4         = 0x3,
};

/* -----------------------------------------------------------------------
 * struct idpf_rx_csum_decoded — checksum offload bits from RX descriptor.
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_rx_csum_decoded {
        uint32_t l3l4p       : 1;
        uint32_t ipe         : 1;
        uint32_t eipe        : 1;
        uint32_t eudpe       : 1;
        uint32_t ipv6exadd   : 1;
        uint32_t l4e         : 1;
        uint32_t pprs        : 1;
        uint32_t nat         : 1;
        uint32_t raw_csum_inv: 1;
        uint32_t raw_csum    : 16;
};

/* -----------------------------------------------------------------------
 * struct idpf_rx_extracted — fields extracted from an RX descriptor.
 * [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_rx_extracted {
        unsigned int size;
        uint16_t     rx_ptype;
};

/* -----------------------------------------------------------------------
 * PTYPE enumeration types  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
enum idpf_rx_ptype_l2 {
        IDPF_RX_PTYPE_L2_RESERVED       = 0,
        IDPF_RX_PTYPE_L2_MAC_PAY2       = 1,
        IDPF_RX_PTYPE_L2_TIMESYNC_PAY2  = 2,
        IDPF_RX_PTYPE_L2_FIP_PAY2       = 3,
        IDPF_RX_PTYPE_L2_OUI_PAY2       = 4,
        IDPF_RX_PTYPE_L2_MACCNTRL_PAY2  = 5,
        IDPF_RX_PTYPE_L2_LLDP_PAY2      = 6,
        IDPF_RX_PTYPE_L2_ECP_PAY2       = 7,
        IDPF_RX_PTYPE_L2_EVB_PAY2       = 8,
        IDPF_RX_PTYPE_L2_QCN_PAY2       = 9,
        IDPF_RX_PTYPE_L2_EAPOL_PAY2     = 10,
        IDPF_RX_PTYPE_L2_ARP            = 11,
};

enum idpf_rx_ptype_outer_ip {
        IDPF_RX_PTYPE_OUTER_L2  = 0,
        IDPF_RX_PTYPE_OUTER_IP  = 1,
};

#define IDPF_RX_PTYPE_TO_IPV(ptype, ipv) \
        (((ptype)->outer_ip == IDPF_RX_PTYPE_OUTER_IP) && \
         ((ptype)->outer_ip_ver == (ipv)))

enum idpf_rx_ptype_outer_ip_ver {
        IDPF_RX_PTYPE_OUTER_NONE        = 0,
        IDPF_RX_PTYPE_OUTER_IPV4        = 1,
        IDPF_RX_PTYPE_OUTER_IPV6        = 2,
};

enum idpf_rx_ptype_outer_fragmented {
        IDPF_RX_PTYPE_NOT_FRAG  = 0,
        IDPF_RX_PTYPE_FRAG      = 1,
};

enum idpf_rx_ptype_tunnel_type {
        IDPF_RX_PTYPE_TUNNEL_NONE               = 0,
        IDPF_RX_PTYPE_TUNNEL_IP_IP              = 1,
        IDPF_RX_PTYPE_TUNNEL_IP_GRENAT          = 2,
        IDPF_RX_PTYPE_TUNNEL_IP_GRENAT_MAC      = 3,
        IDPF_RX_PTYPE_TUNNEL_IP_GRENAT_MAC_VLAN = 4,
};

enum idpf_rx_ptype_tunnel_end_prot {
        IDPF_RX_PTYPE_TUNNEL_END_NONE   = 0,
        IDPF_RX_PTYPE_TUNNEL_END_IPV4   = 1,
        IDPF_RX_PTYPE_TUNNEL_END_IPV6   = 2,
};

enum idpf_rx_ptype_inner_prot {
        IDPF_RX_PTYPE_INNER_PROT_NONE      = 0,
        IDPF_RX_PTYPE_INNER_PROT_UDP       = 1,
        IDPF_RX_PTYPE_INNER_PROT_TCP       = 2,
        IDPF_RX_PTYPE_INNER_PROT_SCTP      = 3,
        IDPF_RX_PTYPE_INNER_PROT_ICMP      = 4,
        IDPF_RX_PTYPE_INNER_PROT_TIMESYNC  = 5,
};

enum idpf_rx_ptype_payload_layer {
        IDPF_RX_PTYPE_PAYLOAD_LAYER_NONE = 0,
        IDPF_RX_PTYPE_PAYLOAD_LAYER_PAY2 = 1,
        IDPF_RX_PTYPE_PAYLOAD_LAYER_PAY3 = 2,
        IDPF_RX_PTYPE_PAYLOAD_LAYER_PAY4 = 3,
};

enum idpf_tunnel_state {
        IDPF_PTYPE_TUNNEL_IP            = (1u << 0),
        IDPF_PTYPE_TUNNEL_IP_GRENAT     = (1u << 1),
        IDPF_PTYPE_TUNNEL_IP_GRENAT_MAC = (1u << 2),
};

struct idpf_ptype_state {
        bool outer_ip;
        bool outer_frag;
        uint8_t tunnel_state;
};

struct idpf_rx_ptype_decoded {
        uint32_t ptype          : 10;
        uint32_t known          : 1;
        uint32_t outer_ip       : 1;
        uint32_t outer_ip_ver   : 2;
        uint32_t outer_frag     : 1;
        uint32_t tunnel_type    : 3;
        uint32_t tunnel_end_prot: 2;
        uint32_t tunnel_end_frag: 1;
        uint32_t inner_prot     : 4;
        uint32_t payload_layer  : 3;
};

/* -----------------------------------------------------------------------
 * enum idpf_queue_flags_t — per-queue state flags.
 *
 * DECLARE_BITMAP replaced by uint32_t in struct idpf_queue.
 * XDP flag removed.  ETF flag retained (CONDITIONAL — feature 250).
 * [LOCAL:A25] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
enum idpf_queue_flags_t {
        __IDPF_Q_GEN_CHK,
        __IDPF_Q_RFL_GEN_CHK,
        __IDPF_Q_FLOW_SCH_EN,
        __IDPF_Q_ETF_EN,        /* CONDITIONAL — feature 250 (EDT pacing) */
        __IDPF_Q_SW_MARKER,
        __IDPF_Q_POLL_MODE,
        __IDPF_Q_MISS_TAG_EN,
        __IDPF_Q_RSC_EN,
        __IDPF_Q_FLAGS_NBITS,
};

_Static_assert(__IDPF_Q_FLAGS_NBITS <= 32,
    "idpf_queue_flags_t exceeds uint32_t width");

/*
 * Queue flag accessors operating on a uint32_t flags field.
 * Linux __set_bit / __clear_bit / test_bit / test_and_clear_bit /
 * __change_bit / __assign_bit replaced by plain bit operations.
 * [FBSD15:A32]
 */
#define idpf_queue_set(f, q) \
        ((q)->flags |= (1u << __IDPF_Q_##f))
#define idpf_queue_clear(f, q) \
        ((q)->flags &= ~(1u << __IDPF_Q_##f))
#define idpf_queue_change(f, q) \
        ((q)->flags ^= (1u << __IDPF_Q_##f))
#define idpf_queue_has(f, q) \
        (((q)->flags & (1u << __IDPF_Q_##f)) != 0)
#define idpf_queue_has_clear(f, q) \
        ({ bool _v = idpf_queue_has(f, q); idpf_queue_clear(f, q); _v; })
#define idpf_queue_assign(f, q, v) \
        do { if (v) idpf_queue_set(f, q); else idpf_queue_clear(f, q); } while (0)

/* -----------------------------------------------------------------------
 * struct idpf_vec_regs — vector register offsets  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_vec_regs {
        uint32_t dyn_ctl_reg;
        uint32_t itrn_reg;
        uint32_t itrn_index_spacing;
};

/* -----------------------------------------------------------------------
 * struct idpf_intr_reg — per-vector interrupt register set.
 *
 * void __iomem * replaced by void * (bus_space accessor model; raw pointer
 * is valid only while the BAR resource is mapped).  [FBSD15:A31]
 * ----------------------------------------------------------------------- */
struct idpf_intr_reg {
        void    *dyn_ctl;               /* dynamic control register */
        void    *rx_itr;                /* RX ITR register */
        void    *tx_itr;                /* TX ITR register */
        void    *icr_ena;               /* interrupt cause enable register */
        uint32_t icr_ena_ctlq_m;
        uint32_t dyn_ctl_intena_msk_m;
        uint32_t dyn_ctl_wb_on_itr_m;
        uint32_t dyn_ctl_sw_itridx_ena_m;
        uint32_t dyn_ctl_swint_trig_m;
        uint8_t  dyn_ctl_itridx_m   : 5;
        uint8_t  dyn_ctl_intrvl_s   : 3;
        uint8_t  dyn_ctl_itridx_s   : 2;
        uint8_t  dyn_ctl_intena_m   : 1;
};

/* -----------------------------------------------------------------------
 * struct idpf_q_vector — per-vector queue service context.
 *
 * struct napi_struct removed: FreeBSD uses iflib deferred processing.
 * struct dim (DIM algorithm) removed: Linux-only.
 * cpumask_var_t removed: Linux-only.
 * CONFIG_TX_TIMEOUT_VERBOSE stats fields removed.
 * [FBSD15:A30] [LOCAL:A25]
 * ----------------------------------------------------------------------- */
struct idpf_q_vector {
        struct idpf_vport       *vport;
        uint16_t                 v_idx;
        struct idpf_intr_reg     intr_reg;
        bool                     wb_on_itr;

        /* iflib IRQ handle bound in ifdi_msix_intr_assign(). [FBSD15:A34] */
	struct if_irq            que_irq;
        uint16_t                 num_txq;
        struct idpf_queue      **tx;
        uint32_t                 tx_itr_value;
        bool                     tx_intr_mode;
        uint32_t                 tx_itr_idx;

        uint16_t                 num_rxq;
        struct idpf_queue      **rx;
        uint32_t                 rx_itr_value;
        bool                     rx_intr_mode;
        uint32_t                 rx_itr_idx;

        uint16_t                 num_bufq;
        struct idpf_queue      **bufq;

        uint16_t                 total_events;
        char                    *name;
};

/* -----------------------------------------------------------------------
 * Queue statistics  [IDPF:A13-A14]
 *
 * u64_stats_sync removed; counters are plain uint64_t protected by the
 * per-port stats_lock (struct mtx) in idpf_port_stats.  [FBSD15:A32]
 * CONFIG_TX_TIMEOUT_VERBOSE fields removed.
 * ----------------------------------------------------------------------- */
struct idpf_rx_queue_stats {
        uint64_t packets;
        uint64_t bytes;
        uint64_t rsc_pkts;
        uint64_t hw_csum_err;
        uint64_t hsplit_pkts;
        uint64_t hsplit_buf_ovf;
        uint64_t bad_descs;
        uint64_t page_recycles;
        uint64_t page_reallocs;
        uint64_t rsc_bytes;
        uint64_t rsc_segs_tot;
        uint64_t segs[IDPF_MAX_SEGS];
};

struct idpf_tx_queue_stats {
        uint64_t packets;
        uint64_t bytes;
        uint64_t lso_pkts;
        uint64_t linearize;
        uint64_t q_busy;
        uint64_t skb_drops;
        uint64_t dma_map_errs;
        uint64_t tstamp_skipped;
        uint64_t lso_bytes;
        uint64_t lso_segs_tot;
        uint64_t segs[IDPF_MAX_SEGS];
};

union idpf_queue_stats {
        struct idpf_rx_queue_stats rx;
        struct idpf_tx_queue_stats tx;
};

/* -----------------------------------------------------------------------
 * struct idpf_sw_queue — software-only refill queue (splitq).
 *
 * DECLARE_BITMAP replaced by uint32_t flags.
 * ____cacheline_internodealigned_in_smp replaced by __aligned(CACHE_LINE_SIZE).
 * [FBSD15:A32] [LOCAL:A25]
 * ----------------------------------------------------------------------- */
struct idpf_sw_queue {
        uint32_t *ring;
        uint32_t  flags;        /* idpf_queue_flags_t bits */
        uint32_t  desc_count;
        uint32_t  next_to_use;
        uint32_t  next_to_clean;
} __aligned(CACHE_LINE_SIZE);

/* -----------------------------------------------------------------------
 * struct idpf_page_info — per-page DMA tracking for RX buffers.
 *
 * dma_addr_t replaced by bus_addr_t.  [FBSD15:A31]
 * struct page * replaced by vm_page_t.  [FBSD15:A31]
 * ----------------------------------------------------------------------- */
struct idpf_page_info {
        bus_addr_t      dma;
        vm_page_t       page;           /* [FBSD15:A31] */
        unsigned int    page_offset;
        unsigned int    default_offset;
        uint16_t        pagecnt_bias;
        uint8_t         reuse_bias;
};

/* -----------------------------------------------------------------------
 * struct idpf_rx_buf — per-descriptor RX buffer.
 *
 * dma_addr_t replaced by bus_addr_t.
 * struct sk_buff * replaced by struct mbuf *.
 * XDP / AF_XDP union variant removed.
 * [FBSD15:A30-A31]
 * ----------------------------------------------------------------------- */
struct idpf_rx_buf {
#define IDPF_RX_BUF_MAX_PAGES   2
        struct idpf_page_info   page_info[IDPF_RX_BUF_MAX_PAGES];
        uint8_t                 page_indx;
        uint16_t                buf_size;
        struct mbuf            *mbuf;   /* [FBSD15:A30] */
        bus_dmamap_t            map;    /* busdma map for this buffer [FBSD15:A31] */
};

/* -----------------------------------------------------------------------
 * struct idpf_queue — unified queue descriptor.
 *
 * struct device * replaced by device_t.
 * struct net_device * replaced by if_ctx_t.
 * struct sk_buff * replaced by struct mbuf *.
 * dma_addr_t replaced by bus_addr_t.
 * void __iomem *tail replaced by void * (bus_space accessor model).
 * DECLARE_BITMAP replaced by uint32_t flags.
 * u64_stats_sync removed; stats protected by port stats_lock.
 * struct work_struct *tstamp_task replaced by struct task *.
 * XDP / AF_XDP fields removed.
 * struct xarray reinject_timers replaced by TAILQ head + struct mtx.
 * __be16 vlan_proto replaced by uint16_t (big-endian stored explicitly).
 * ____cacheline_internodealigned_in_smp replaced by __aligned(CACHE_LINE_SIZE).
 * [FBSD15:A30-A34] [LOCAL:A25] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */
struct idpf_queue {
        device_t                 dev;           /* [FBSD15:A30] */
        struct idpf_vport       *vport;
        if_ctx_t                 ctx;           /* replaces net_device * [FBSD15:A30] */

        union {
                struct idpf_txq_group *txq_grp;
                struct idpf_rxq_group *rxq_grp;
        };

        union {
                struct {
                        struct idpf_tx_buf      *bufs;
                        struct idpf_sw_queue    *refillq;
                        uint32_t                 num_completions;
                        uint32_t                 rel_qid;
                        uint16_t                 num_txq;
                        uint16_t                 last_re;
                        uint8_t                  cmpl_tstamp_ns_s;
                } tx;
                struct {
                        union {
                                struct {
                                        struct idpf_rx_buf *bufs;
                                        bus_addr_t          hdr_buf_pa; /* [FBSD15:A31] */
                                        void               *hdr_buf_va;
                                };
                                struct {
                                        struct idpf_rx_buf **bufq_bufs;
                                        uint64_t           **bufq_hdr_bufs;
                                        struct mbuf         *mbuf; /* [FBSD15:A30] */
                                };
                        };
                        struct idpf_sw_queue    *refillqs;
                        int                      num_refillq;
                        uint16_t                 rxq_idx;
                        uint16_t                 vlan_proto; /* big-endian stored */
                } rx;
        };

        /*
         * tstamp_task: CONDITIONAL (feature 253 — TX completion timestamps).
         * NULL when not negotiated.  [ON_HOLD — see §21.8]
         */
        struct task                             *tstamp_task;
        struct idpf_ptp_vport_tx_tstamp_caps    *cached_tstamp_caps;
        uint64_t                                *cached_phc_time;
        bool                                     tstmp_en;

        uint16_t                 idx;
        uint8_t                  gen_rxcsum_status;

        /*
         * tail: mapped tail doorbell register address.
         * Callers must use bus_space_write_4() via idpf_tx_buf_hw_update()
         * and idpf_rx_buf_hw_update(); do not dereference directly.
         * [FBSD15:A31]
         */
        void                    *tail;

        uint16_t                 q_type;
        uint32_t                 q_id;
        uint16_t                 desc_count;
        uint16_t                 next_to_use;
        uint16_t                 next_to_clean;
        uint16_t                 next_to_alloc;

        uint32_t                 flags;         /* idpf_queue_flags_t bits */

        union idpf_queue_stats   q_stats;

        uint32_t                 cleaned_bytes;
        uint16_t                 cleaned_pkts;
        bool                     rx_hsplit_en;
        uint16_t                 rx_hbuf_size;
        uint16_t                 rx_buf_size;
        uint16_t                 rx_max_pkt_size;
        uint16_t                 rx_buf_stride;
        uint8_t                  rx_buffer_low_watermark;
        uint64_t                 rxdids;

        struct idpf_q_vector    *q_vector;
        unsigned int             size;          /* descriptor ring size in bytes */
        bus_addr_t               dma;           /* [FBSD15:A31] */
        void                    *desc_ring;
        uint32_t                 buf_pool_size;

        uint16_t                 tx_max_bufs;
        bool                     crc_enable;
        uint8_t                  tx_min_pkt_len;

        struct idpf_rx_ptype_decoded *rx_ptype_lkup;

        /*
         * reinject_timers: exception-path reinjection timer list.
         * Linux struct xarray replaced by TAILQ + struct mtx.  [FBSD15:A32-A33]
         */
        struct idpf_reinject_timer_head  reinject_timers;
        struct mtx                       reinject_lock;

} __aligned(CACHE_LINE_SIZE);

/* -----------------------------------------------------------------------
 * struct idpf_rxq_set — RX queue + refill queue association (splitq).
 * [IDPF:A13-A14] [LOCAL:A25]
 * ----------------------------------------------------------------------- */
struct idpf_rxq_set {
        struct idpf_queue        rxq;
        struct idpf_sw_queue    *refillq[IDPF_MAX_BUFQS_PER_RXQ_GRP];
};

/* -----------------------------------------------------------------------
 * struct idpf_bufq_set — buffer queue + refill queue association (splitq).
 * [IDPF:A13-A14] [LOCAL:A25]
 * ----------------------------------------------------------------------- */
struct idpf_bufq_set {
        struct idpf_queue        bufq;
        int                      num_refillqs;
        struct idpf_sw_queue    *refillqs;
};

/* -----------------------------------------------------------------------
 * struct idpf_rxq_group — RX queue group (singleq or splitq).
 * [IDPF:A13-A14] [LOCAL:A25]
 * ----------------------------------------------------------------------- */
struct idpf_rxq_group {
        struct idpf_vport *vport;

        union {
                struct {
                        uint16_t           num_rxq;
                        struct idpf_queue *rxqs[IDPF_LARGE_MAX_Q];
                } singleq;
                struct {
                        uint16_t              num_rxq_sets;
                        uint16_t              num_bufq_sets;
                        struct idpf_rxq_set  *rxq_sets[IDPF_LARGE_MAX_Q];
                        struct idpf_bufq_set *bufq_sets;
                } splitq;
        };
};

/* -----------------------------------------------------------------------
 * struct idpf_txq_group — TX queue group (singleq or splitq).
 * num_completions_pending aligned to cache line.  [IDPF:A13-A14]
 * ----------------------------------------------------------------------- */
struct idpf_txq_group {
        struct idpf_vport  *vport;
        uint16_t            num_txq;
        struct idpf_queue **txqs;
        struct idpf_queue  *complq;     /* splitq only */
        uint32_t            num_completions_pending __aligned(CACHE_LINE_SIZE);
};

/* -----------------------------------------------------------------------
 * Inline helpers
 * ----------------------------------------------------------------------- */

/**
 * idpf_rx_bump_ntc - bump and wrap next_to_clean, flipping gen bit on wrap.
 * @rxq: queue to bump
 * @ntc: current next_to_clean
 *
 * Returns updated next_to_clean.  [IDPF:A13-A14]
 */
static inline uint16_t
idpf_rx_bump_ntc(struct idpf_queue *rxq, uint16_t ntc)
{
        if (__predict_false(++ntc == rxq->desc_count)) {
                ntc = 0;
                idpf_queue_change(GEN_CHK, rxq);
        }
        return ntc;
}

/**
 * idpf_singleq_bump_desc_idx - bump and wrap a descriptor index.
 * @q: queue to bump
 * @idx: current index
 *
 * Returns updated index.  [IDPF:A13-A14]
 */
static inline uint16_t
idpf_singleq_bump_desc_idx(struct idpf_queue *q, uint16_t idx)
{
        if (__predict_false(++idx == q->desc_count))
                idx = 0;
        return idx;
}

/**
 * idpf_rx_singleq_test_staterr - test status/error bits in an RX descriptor.
 * @rx_desc: pointer to receive descriptor
 * @stat_err_bits: bitmask to test
 *
 * Returns true if any of the masked bits are set.  [IDPF:A13-A14]
 */
static inline bool
idpf_rx_singleq_test_staterr(const union virtchnl2_rx_desc *rx_desc,
                               const uint64_t stat_err_bits)
{
        return !!(rx_desc->base_wb.qword1.status_error_ptype_len &
                  htole64(stat_err_bits));
}

/**
 * idpf_size_to_txd_count - get descriptor count needed for a large TX fragment.
 * @size: transmit request size in bytes
 *
 * Large fragments (>= 16 KiB) must be split due to 4 KiB alignment.
 * [IDPF:A13-A14]
 */
static inline uint32_t
idpf_size_to_txd_count(unsigned int size)
{
        return howmany(size, IDPF_TX_MAX_DESC_DATA_ALIGNED);
}

/**
 * idpf_tx_singleq_build_ctob - populate command/tag/offset/size descriptor word.
 * @td_cmd: command bits
 * @td_offset: header offset bits
 * @size: buffer size in bytes
 * @td_tag: L2 tag value
 *
 * Returns the 64-bit little-endian descriptor word.  [IDPF:A13-A14]
 */
static inline uint64_t
idpf_tx_singleq_build_ctob(uint64_t td_cmd, uint64_t td_offset,
                             unsigned int size, uint64_t td_tag)
{
        return htole64(IDPF_TX_DESC_DTYPE_DATA |
                       (td_cmd    << IDPF_TXD_QW1_CMD_S)       |
                       (td_offset << IDPF_TXD_QW1_OFFSET_S)    |
                       ((uint64_t)size << IDPF_TXD_QW1_TX_BUF_SZ_S) |
                       (td_tag    << IDPF_TXD_QW1_L2TAG1_S));
}

/**
 * idpf_tx_splitq_need_re - check whether the RE bit must be set.
 * @tx_q: TX queue
 *
 * Returns true if the gap since the last RE descriptor meets the minimum.
 * [IDPF:A13-A14]
 */
static inline bool
idpf_tx_splitq_need_re(struct idpf_queue *tx_q)
{
        int gap = tx_q->next_to_use - tx_q->tx.last_re;

        gap += (gap < 0) ? tx_q->desc_count : 0;
        return gap >= IDPF_TX_SPLITQ_RE_MIN_GAP;
}

/**
 * idpf_tx_get_free_buf_id - get a free buffer ID from a refill queue.
 * @refillq: refill queue
 * @buf_id: output buffer ID
 *
 * Returns true if a buffer ID was available, false otherwise.
 * [IDPF:A13-A14]
 *
 * FIELD_GET(IDPF_RFL_BI_BUFID_M, refill_desc) replaced by explicit mask.
 */
static inline bool
idpf_tx_get_free_buf_id(struct idpf_sw_queue *refillq, uint32_t *buf_id)
{
        uint32_t ntc = refillq->next_to_clean;
        uint32_t refill_desc;

        refill_desc = refillq->ring[ntc];

        if (__predict_false(idpf_queue_has(RFL_GEN_CHK, refillq) !=
                            !!(refill_desc & IDPF_RFL_BI_GEN_M)))
                return false;

        *buf_id = refill_desc & IDPF_RFL_BI_BUFID_M;

        if (__predict_false(++ntc == refillq->desc_count)) {
                idpf_queue_change(RFL_GEN_CHK, refillq);
                ntc = 0;
        }
        refillq->next_to_clean = ntc;
        return true;
}

/**
 * idpf_tx_splitq_get_free_bufs - get number of free buffer IDs in a refill queue.
 * @refillq: refill queue
 * [IDPF:A13-A14]
 */
static inline uint32_t
idpf_tx_splitq_get_free_bufs(struct idpf_sw_queue *refillq)
{
        return (refillq->next_to_use > refillq->next_to_clean ?
                0 : refillq->desc_count) +
               refillq->next_to_use - refillq->next_to_clean - 1;
}

/* -----------------------------------------------------------------------
 * Function declarations
 * Core method names are NOT renamed per architectural constraint.
 * [LOCAL:A25] [PROPOSED:A38]
 * ----------------------------------------------------------------------- */

/* Descriptor build helpers */
void idpf_tx_splitq_build_ctb(union idpf_tx_flex_desc *desc,
                               struct idpf_tx_splitq_params *params,
                               uint16_t td_cmd, uint16_t size);
void idpf_tx_splitq_build_flow_desc(union idpf_tx_flex_desc *desc,
                                     struct idpf_tx_splitq_params *params,
                                     uint16_t td_cmd, uint16_t size);

/**
 * idpf_tx_splitq_build_desc - select and build the appropriate TX descriptor.
 * @desc: descriptor to populate
 * @params: TX parameters
 * @td_cmd: command field
 * @size: buffer size
 * [IDPF:A13-A14]
 */
static inline void
idpf_tx_splitq_build_desc(union idpf_tx_flex_desc *desc,
                           struct idpf_tx_splitq_params *params,
                           uint16_t td_cmd, uint16_t size)
{
        if (params->dtype == IDPF_TX_DESC_DTYPE_FLEX_L2TAG1_L2TAG2)
                idpf_tx_splitq_build_ctb(desc, params, td_cmd, size);
        else
                idpf_tx_splitq_build_flow_desc(desc, params, td_cmd, size);
}

/* Queue lifecycle */
void idpf_vport_init_num_qs(struct idpf_vport *vport,
                              struct virtchnl2_create_vport *vport_msg,
                              struct idpf_q_vec_rsrc *rsrc);
void idpf_vport_calc_num_q_desc(struct idpf_vport *vport,
                                  struct idpf_q_vec_rsrc *rsrc);
void idpf_vport_calc_total_qs(struct idpf_adapter *adapter,
                                uint16_t vport_index,
                                struct virtchnl2_create_vport *vport_msg,
                                struct idpf_vport_max_q *max_q);
void idpf_vport_calc_num_q_groups(struct idpf_q_vec_rsrc *rsrc);
int  idpf_vport_queue_alloc_all(struct idpf_vport *vport,
                                  struct idpf_q_vec_rsrc *rsrc);
void idpf_vport_queues_rel(struct idpf_vport *vport,
                             struct idpf_q_vec_rsrc *rsrc);

/*
 * RX buffer management is owned by iflib.  The Linux refill-queue helpers
 * (idpf_post_buf_refill / idpf_rx_bufs_init_all) and the page-recycling
 * allocator they served have no counterpart here: iflib allocates, maps,
 * recycles and frees every RX buffer, and the driver only writes the
 * addresses it is handed into buffer descriptors.  [FBSD15:A30-A31]
 */

/* Interrupt management */
void idpf_vport_intr_rel(struct idpf_q_vec_rsrc *rsrc);
int  idpf_vport_intr_alloc(struct idpf_vport *vport,
                             struct idpf_q_vec_rsrc *rsrc);
void idpf_vport_intr_update_itr_ena_irq(struct idpf_q_vector *q_vector);
void idpf_vport_intr_deinit(struct idpf_vport *vport,
                              struct idpf_q_vec_rsrc *rsrc);
int  idpf_vport_intr_init(struct idpf_vport *vport,
                           struct idpf_q_vec_rsrc *rsrc);
void idpf_vport_intr_ena(struct idpf_vport *vport,
                          struct idpf_q_vec_rsrc *rsrc);
void idpf_vport_intr_set_wb_on_itr(struct idpf_q_vector *q_vector);

/*
 * idpf_ptype_to_htype - translate IDPF PTYPE to FreeBSD hash type.
 *
 * Linux pkt_hash_types replaced by uint32_t (M_HASHTYPE_* values from
 * FreeBSD <sys/mbuf.h>).  [FBSD15:A30]
 */
uint32_t idpf_ptype_to_htype(const struct idpf_rx_ptype_decoded *decoded);

/* RSS */
int  idpf_config_rss(struct idpf_vport *vport,
                      struct idpf_rss_data *rss_data);
int  idpf_init_rss(struct idpf_vport *vport,
                    struct idpf_rss_data *rss_data,
                    struct idpf_q_vec_rsrc *rsrc);
void idpf_deinit_rss(struct idpf_rss_data *rss_data);

/* Hardware tail update helpers */
void idpf_rx_buf_hw_update(struct idpf_queue *rxq, uint32_t val);
void idpf_tx_buf_hw_update(struct idpf_queue *tx_q, uint32_t val,
                             bool xmit_more);

/*
 * idpf_tx_timeout: TX watchdog timeout callback.
 * net_device / txqueue arguments replaced by adapter + queue index.
 * [FBSD15:A30] [LOCAL:A25]
 */
void idpf_tx_timeout(struct idpf_adapter *adapter, unsigned int txqueue);

/* -----------------------------------------------------------------------
 * iflib datapath contract
 *
 * The Linux packet-lifecycle entry points (idpf_tx_splitq_start,
 * idpf_tx_singleq_start, idpf_vport_singleq_napi_poll, idpf_tx_drop_skb,
 * idpf_tso, idpf_rx_*_process_skb_fields, idpf_rx_singleq_buf_hw_alloc_all)
 * are replaced by the callbacks below.  Under iflib the driver never owns an
 * mbuf, never allocates an RX buffer, and never runs a poll loop: it receives
 * an already bus_dma-mapped scatter list on TX, hands back free-list indices
 * on RX, and reports credits.  [FBSD15:A30-A31] [PROPOSED:A38]
 *
 * if_idpf.c must set, in its if_shared_ctx:
 *   isc_txrx  = &idpf_txrx_ops
 *   isc_flags |= IFLIB_HAS_TXCQ | IFLIB_HAS_RXCQ  (split queue model only)
 * so that ring index 0 of every queue set is the completion queue.
 * ----------------------------------------------------------------------- */
extern struct if_txrx idpf_txrx_ops;

/* ifdi_{tx,rx}_queues_alloc / ifdi_queues_free */
int  idpf_tx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs, uint64_t *paddrs,
                            int ntxqs, int ntxqsets);
int  idpf_rx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs, uint64_t *paddrs,
                            int nrxqs, int nrxqsets);
void idpf_queues_free(if_ctx_t ctx);

/* ifdi_intr_enable / ifdi_intr_disable / ifdi_{tx,rx}_queue_intr_enable */
void idpf_intr_enable(if_ctx_t ctx);
void idpf_intr_disable(if_ctx_t ctx);
int  idpf_tx_queue_intr_enable(if_ctx_t ctx, uint16_t txqid);
int  idpf_rx_queue_intr_enable(if_ctx_t ctx, uint16_t rxqid);

/* -----------------------------------------------------------------------
 * Shared RX decode helpers (idpf_txrx.c), used by both queue models.
 * ----------------------------------------------------------------------- */
const struct idpf_rx_ptype_decoded *idpf_rx_decode_ptype(struct idpf_queue *rxq,
                                                          uint16_t ptype);
void idpf_rx_csum(struct idpf_queue *rxq, if_rxd_info_t ri,
                   const struct idpf_rx_csum_decoded *csum_bits,
                   const struct idpf_rx_ptype_decoded *decoded);

/* -----------------------------------------------------------------------
 * Single queue model datapath (idpf_singleq_txrx.c).
 *
 * Called from the iflib callbacks in idpf_txrx.c once the queue model has
 * been resolved, so the model test stays on the cold path.
 * ----------------------------------------------------------------------- */
int  idpf_tx_singleq_encap(struct idpf_queue *txq, if_pkt_info_t pi);
int  idpf_tx_singleq_credits(struct idpf_queue *txq, bool clear);
int  idpf_rx_singleq_available(struct idpf_queue *rxq, qidx_t idx,
                                qidx_t budget);
int  idpf_rx_singleq_pkt_get(struct idpf_queue *rxq, if_rxd_info_t ri);
void idpf_rx_singleq_refill(struct idpf_queue *rxq, if_rxd_update_t iru);

#endif /* !_IDPF_TXRX_H_ */