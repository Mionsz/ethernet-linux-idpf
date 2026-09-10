/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * idpf_txrx.c - FreeBSD >= 15.0 IDPF VF-DPF TX/RX datapath.
 *
 * This is the production port of the Linux idpf_txrx.c / idpf_singleq_txrx.c
 * datapath onto FreeBSD 15 using iflib exclusively.  It implements the
 * contract declared in idpf_txrx.h and idpf.h.
 *
 * ---------------------------------------------------------------------------
 * OWNERSHIP MODEL (differs fundamentally from the Linux driver)
 * ---------------------------------------------------------------------------
 * Linux idpf owns every memory object on the datapath: descriptor rings via
 * dma_alloc_coherent(), RX payload pages via a private page-recycling
 * allocator, TX skb lifetime, NAPI polling, and the interrupt loop.
 *
 * Under iflib none of that belongs to the driver:
 *
 *   - Descriptor rings are allocated and bus_dma-synchronized by iflib and
 *     handed to the driver through ifdi_tx_queues_alloc()/ifdi_rx_queues_alloc()
 *     as (vaddr, paddr) pairs.  The driver records them in struct idpf_queue
 *     so that the control plane can publish dma_ring_addr in
 *     VIRTCHNL2_OP_CONFIG_{TX,RX}_QUEUES.  The driver must NOT allocate,
 *     free, or bus_dmamap_sync() those rings.
 *   - RX buffers are allocated, mapped and recycled by iflib.  The driver
 *     only writes iflib-supplied physical addresses into buffer descriptors
 *     (isc_rxd_refill) and reports which free-list entries a completion
 *     consumed (isc_rxd_pkt_get).  All page_info / page-recycling logic from
 *     the Linux driver is therefore deleted, not ported.
 *   - TX mbufs are owned by iflib.  isc_txd_encap() receives an already
 *     bus_dma-mapped scatter list in if_pkt_info.ipi_segs; the driver never
 *     touches struct mbuf on the TX path and never frees one.  Descriptor
 *     credits returned by isc_txd_credits_update() are what release mbufs.
 *   - There is no NAPI.  iflib owns the poll loop and the interrupt
 *     enable/disable transitions; the driver only programs ITR/DYN_CTL.
 *
 * ---------------------------------------------------------------------------
 * QUEUE GEOMETRY -> iflib RING GEOMETRY
 * ---------------------------------------------------------------------------
 * if_idpf.c must set isc_flags |= IFLIB_HAS_TXCQ | IFLIB_HAS_RXCQ for the
 * split queue model.  iflib then reserves ring index 0 of every queue set for
 * the completion queue (sys/net/iflib.c: first_txq = 1 when IFLIB_HAS_TXCQ,
 * ifr_fl_offset = 1 when IFLIB_HAS_RXCQ), which is exactly the IDPF shape:
 *
 *   split TX set i : ring 0 = TX completion queue, ring 1 = TX data ring
 *   split RX set i : ring 0 = RX completion queue, ring 1..n = buffer queues
 *   single TX set i: ring 0 = TX ring (no completion queue)
 *   single RX set i: ring 0 = RX ring (descriptor is both post and writeback)
 *
 * IDPF_MAX_BUFQS_PER_RXQ_GRP is 2 and iflib supports up to 2 free lists per
 * RX queue set, so a split RX group maps 1:1 onto an iflib RX queue set with
 * bufq j addressed as free-list id j.
 *
 * ---------------------------------------------------------------------------
 * SCHEDULING MODE
 * ---------------------------------------------------------------------------
 * IDPF split queue supports two TX scheduling modes.  Queue-based scheduling
 * retires descriptors strictly in order and reports a hardware head pointer,
 * which is exactly iflib's credit model.  Flow scheduling retires out of order
 * using completion tags and requires driver-owned buffer lifetime, an
 * out-of-order reclaim ring, rule-miss/reinjection handling and a software
 * retry timer - none of which can be expressed through
 * isc_txd_credits_update().  This port therefore always requests queue-based
 * scheduling and refuses to bring queues up if the control plane cannot offer
 * it.  See idpf_txq_group_alloc().
 *
 * Header split is likewise not enabled: IDPF ties the header buffer to the
 * same buffer-queue entry as the payload (hdr_addr in the same descriptor),
 * while iflib models a header buffer as an independent free list with its own
 * producer index.  The two cannot be reconciled without driver-owned buffers.
 *
 * Evidence classes per the Human Reference Guide Appendix A:
 *   [FBSD15:A30-A34] FreeBSD 15 kernel / iflib interfaces are authoritative.
 *   [IDPF:A13-A14]   Virtchnl2 / IDPF 1.0 protocol behaviour is authoritative.
 *   [LOCAL:A25]      Shape of the ported idpf_txrx.h is preserved.
 */

#include "idpf.h"
#include "idpf_virtchnl.h"
#include <idpf_lan_txrx.h>

#include <sys/smp.h>

/* ---------------------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------------------- */
static void	idpf_txq_group_rel(struct idpf_q_vec_rsrc *rsrc);
static void	idpf_rxq_group_rel(struct idpf_q_vec_rsrc *rsrc);
static void	idpf_vport_queue_grp_rel_all(struct idpf_q_vec_rsrc *rsrc);
static int	idpf_txq_group_alloc(struct idpf_vport *vport,
		    struct idpf_q_vec_rsrc *rsrc, u16 num_txq_per_grp);
static int	idpf_rxq_group_alloc(struct idpf_vport *vport,
		    struct idpf_q_vec_rsrc *rsrc, u16 num_rxq);
static int	idpf_fast_path_txq_init(struct idpf_vport *vport,
		    struct idpf_q_vec_rsrc *rsrc);
static void	idpf_init_cached_phc_time(struct idpf_vport *vport,
		    struct idpf_q_vec_rsrc *rsrc);

static int	idpf_isc_txd_encap(void *arg, if_pkt_info_t pi);
static void	idpf_isc_txd_flush(void *arg, u16 txqid, qidx_t pidx);
static int	idpf_isc_txd_credits_update(void *arg, u16 txqid,
		    bool clear);
static int	idpf_isc_rxd_available(void *arg, u16 rxqid, qidx_t idx,
		    qidx_t budget);
static int	idpf_isc_rxd_pkt_get(void *arg, if_rxd_info_t ri);
static void	idpf_isc_rxd_refill(void *arg, if_rxd_update_t iru);
static void	idpf_isc_rxd_flush(void *arg, u16 rxqid, u8 flid,
		    qidx_t pidx);

static void	idpf_vport_intr_map_vector_to_qs(struct idpf_q_vec_rsrc *rsrc);
static void	idpf_vport_intr_dis_irq_all(struct idpf_q_vec_rsrc *rsrc);

/* ---------------------------------------------------------------------------
 * Queue lookup helpers
 *
 * iflib addresses queues by a flat queue-set index.  In the split model each
 * group holds exactly one queue (IDPF_DFLT_SPLITQ_{TX,RX}Q_PER_GROUP) so the
 * set index is the group index; in the single model there is one group holding
 * every queue.  Both directions are therefore O(1).  [LOCAL:A25]
 * ------------------------------------------------------------------------- */
static inline struct idpf_queue *
idpf_txq(struct idpf_vport *vport, u16 qid)
{

	return (vport->txqs[qid]);
}

/**
 * idpf_softc_to_vport - resolve the vport from an iflib softc pointer
 * @softc: value returned by iflib_get_softc(), or the void *arg handed to an
 *         if_txrx callback
 *
 * idpf.h registers struct idpf_netdev_priv as the iflib softc, so every
 * callback entry has to go through it to reach the vport.  [FBSD15:A30]
 */
static inline struct idpf_vport *
idpf_softc_to_vport(void *softc)
{
	struct idpf_netdev_priv *np = softc;

	return (np->vport);
}

static inline struct idpf_queue *
idpf_rxq(struct idpf_q_vec_rsrc *rsrc, u16 qid)
{

	if (idpf_is_queue_model_split(rsrc->rxq_model))
		return (&rsrc->rxq_grps[qid].splitq.rxq_sets[0]->rxq);

	return (rsrc->rxq_grps[0].singleq.rxqs[qid]);
}

static inline struct idpf_queue *
idpf_bufq(struct idpf_q_vec_rsrc *rsrc, u16 qid, u8 flid)
{

	return (&rsrc->rxq_grps[qid].splitq.bufq_sets[flid].bufq);
}

/* ---------------------------------------------------------------------------
 * TX watchdog
 * ------------------------------------------------------------------------- */

/**
 * idpf_tx_timeout - handle an iflib TX watchdog expiry
 * @adapter: private data struct
 * @txqueue: index of the queue that stalled
 *
 * Called from ifdi_watchdog_reset().  Escalates to a function-level reset,
 * which the vc event task performs outside of this context.  [LOCAL:A25]
 */
void
idpf_tx_timeout(struct idpf_adapter *adapter, unsigned int txqueue)
{

	adapter->tx_timeout_count++;
	device_printf(adapter->dev,
	    "detected TX timeout: count %u, queue %u\n",
	    adapter->tx_timeout_count, txqueue);

	if (idpf_is_reset_in_prog(adapter))
		return;

	atomic_set_32(&adapter->flags, 1u << IDPF_HR_FUNC_RESET);
	taskqueue_enqueue_timeout(adapter->vc_event_wq, &adapter->vc_event_task,
	    0);
}

/* ---------------------------------------------------------------------------
 * Software queue-structure lifecycle
 *
 * Everything below allocates or frees driver bookkeeping only.  Descriptor
 * ring memory belongs to iflib and is bound in idpf_tx_queues_alloc() /
 * idpf_rx_queues_alloc().  [FBSD15:A30-A31]
 * ------------------------------------------------------------------------- */

/**
 * idpf_tx_buf_alloc_all - allocate the TX completion bookkeeping array
 * @txq: queue to allocate for
 *
 * With queue-based scheduling one entry per descriptor is sufficient: the
 * array is used as an in-order ring of "last descriptor of a packet" indices
 * consumed by idpf_tx_singleq_credits().  [LOCAL:A25]
 */
static int
idpf_tx_buf_alloc_all(struct idpf_queue *txq)
{

	txq->buf_pool_size = txq->desc_count;
	txq->tx.bufs = malloc(txq->buf_pool_size * sizeof(*txq->tx.bufs),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (txq->tx.bufs == NULL)
		return (ENOMEM);

	return (0);
}

static void
idpf_tx_buf_rel_all(struct idpf_queue *txq)
{

	free(txq->tx.bufs, M_DEVBUF);
	txq->tx.bufs = NULL;
	txq->buf_pool_size = 0;
}

/**
 * idpf_txq_group_alloc - allocate TX queue groups and their queues
 * @vport: vport that owns the groups
 * @rsrc: queue and vector resources
 * @num_txq_per_grp: queues per group
 *
 * Returns 0, ENOMEM, or ENOTSUP when the control plane cannot provide
 * queue-based scheduling.  [IDPF:A13-A14] [FBSD15:A30]
 */
static int
idpf_txq_group_alloc(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc,
    u16 num_txq_per_grp)
{
	struct idpf_adapter *adapter = vport->adapter;
	bool split;
	unsigned int i, j;
	int err;

	split = idpf_is_queue_model_split(rsrc->txq_model);

	/*
	 * VIRTCHNL2_CAP_SPLITQ_QSCHED advertises queue-based (in-order)
	 * scheduling.  Without it the device only offers flow scheduling,
	 * whose out-of-order completion-tag retirement cannot be expressed
	 * through iflib's in-order credit interface.
	 */
	if (split && !idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS,
	    VIRTCHNL2_CAP_SPLITQ_QSCHED)) {
		device_printf(adapter->dev,
		    "control plane offers only flow-scheduled split TX, which "
		    "is incompatible with the iflib credit model\n");
		return (ENOTSUP);
	}

	rsrc->txq_grps = malloc(rsrc->num_txq_grp * sizeof(*rsrc->txq_grps),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rsrc->txq_grps == NULL)
		return (ENOMEM);

	for (i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

		tx_qgrp->vport = vport;
		tx_qgrp->num_txq = num_txq_per_grp;
		tx_qgrp->txqs = malloc(tx_qgrp->num_txq * sizeof(*tx_qgrp->txqs),
		    M_DEVBUF, M_NOWAIT | M_ZERO);
		if (tx_qgrp->txqs == NULL) {
			err = ENOMEM;
			goto err_alloc;
		}

		for (j = 0; j < tx_qgrp->num_txq; j++) {
			struct idpf_queue *q;

			q = malloc(sizeof(*q), M_DEVBUF, M_NOWAIT | M_ZERO);
			if (q == NULL) {
				err = ENOMEM;
				goto err_alloc;
			}
			tx_qgrp->txqs[j] = q;

			q->dev = adapter->dev;
			q->ctx = vport->ctx;
			q->vport = vport;
			q->txq_grp = tx_qgrp;
			q->desc_count = rsrc->txq_desc_count;
			q->tx_max_bufs = idpf_get_max_tx_bufs(adapter);
			q->tx_min_pkt_len = idpf_get_min_tx_pkt_len(adapter);
			q->crc_enable = vport->crc_enable;
			q->q_type = VIRTCHNL2_QUEUE_TYPE_TX;
			if (split)
				q->tx.rel_qid = j;

			/*
			 * Flow scheduling stays off, so tx.refillq, the
			 * completion-tag pool and the rule-miss reinjection
			 * timers it drives are never armed.  The list and its
			 * lock are still initialised so the queue is coherent
			 * for the header's teardown contract.
			 */
			idpf_queue_clear(FLOW_SCH_EN, q);

			TAILQ_INIT(&q->reinject_timers);
			mtx_init(&q->reinject_lock, "idpf_reinject", NULL,
			    MTX_DEF);

			err = idpf_tx_buf_alloc_all(q);
			if (err != 0)
				goto err_alloc;
		}

		if (!split)
			continue;

		tx_qgrp->complq = malloc(sizeof(*tx_qgrp->complq), M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (tx_qgrp->complq == NULL) {
			err = ENOMEM;
			goto err_alloc;
		}

		tx_qgrp->complq->dev = adapter->dev;
		tx_qgrp->complq->ctx = vport->ctx;
		tx_qgrp->complq->vport = vport;
		tx_qgrp->complq->txq_grp = tx_qgrp;
		tx_qgrp->complq->desc_count = rsrc->complq_desc_count;
		tx_qgrp->complq->q_type = VIRTCHNL2_QUEUE_TYPE_TX_COMPLETION;
	}

	return (0);

err_alloc:
	idpf_txq_group_rel(rsrc);
	return (err);
}

static void
idpf_txq_group_rel(struct idpf_q_vec_rsrc *rsrc)
{
	unsigned int i, j;

	if (rsrc->txq_grps == NULL)
		return;

	for (i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

		if (tx_qgrp->txqs != NULL) {
			for (j = 0; j < tx_qgrp->num_txq; j++) {
				struct idpf_queue *q = tx_qgrp->txqs[j];

				if (q == NULL)
					continue;

				idpf_tx_buf_rel_all(q);
				mtx_destroy(&q->reinject_lock);
				free(q, M_DEVBUF);
				tx_qgrp->txqs[j] = NULL;
			}
			free(tx_qgrp->txqs, M_DEVBUF);
			tx_qgrp->txqs = NULL;
		}

		free(tx_qgrp->complq, M_DEVBUF);
		tx_qgrp->complq = NULL;
	}

	free(rsrc->txq_grps, M_DEVBUF);
	rsrc->txq_grps = NULL;
}

/**
 * idpf_rxq_set_descids - select the descriptor writeback format for a queue
 * @rsrc: queue and vector resources
 * @q: queue to configure
 *
 * The value is published to the control plane in virtchnl2_rxq_info.desc_ids.
 * [IDPF:A13-A14]
 */
static void
idpf_rxq_set_descids(struct idpf_q_vec_rsrc *rsrc, struct idpf_queue *q)
{

	if (idpf_is_queue_model_split(rsrc->rxq_model)) {
		q->rxdids = VIRTCHNL2_RXDID_2_FLEX_SPLITQ_M;
		return;
	}

	q->rxdids = rsrc->base_rxd ? VIRTCHNL2_RXDID_1_32B_BASE_M :
	    VIRTCHNL2_RXDID_2_FLEX_SQ_NIC_M;
}

static void
__idpf_rxq_init(struct idpf_vport *vport, struct idpf_queue *q)
{

	q->dev = vport->adapter->dev;
	q->vport = vport;
	q->ctx = vport->ctx;
	q->rx_buffer_low_watermark = IDPF_LOW_WATERMARK;

	/*
	 * Header split needs an independent producer index per header buffer,
	 * which iflib provides only as a separate free list.  IDPF instead
	 * couples hdr_addr to the payload entry of the same buffer descriptor,
	 * so the feature stays off under iflib.
	 */
	q->rx_hsplit_en = false;
	q->rx_hbuf_size = 0;

	idpf_queue_set(GEN_CHK, q);
}

/**
 * idpf_rxq_group_alloc - allocate RX queue groups, queues and buffer queues
 * @vport: vport that owns the groups
 * @rsrc: queue and vector resources
 * @num_rxq: RX queues per group
 */
static int
idpf_rxq_group_alloc(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc,
    u16 num_rxq)
{
	struct idpf_adapter *adapter = vport->adapter;
	bool split = idpf_is_queue_model_split(rsrc->rxq_model);
	unsigned int i, j;
	int err;

	rsrc->rxq_grps = malloc(rsrc->num_rxq_grp * sizeof(*rsrc->rxq_grps),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rsrc->rxq_grps == NULL)
		return (ENOMEM);

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];

		rx_qgrp->vport = vport;

		if (!split) {
			if (num_rxq > nitems(rx_qgrp->singleq.rxqs)) {
				err = EINVAL;
				goto err_alloc;
			}
			rx_qgrp->singleq.num_rxq = num_rxq;
			for (j = 0; j < num_rxq; j++) {
				rx_qgrp->singleq.rxqs[j] = malloc(
				    sizeof(*rx_qgrp->singleq.rxqs[j]),
				    M_DEVBUF, M_NOWAIT | M_ZERO);
				if (rx_qgrp->singleq.rxqs[j] == NULL) {
					err = ENOMEM;
					goto err_alloc;
				}
			}
			goto init_rxqs;
		}

		if (num_rxq > nitems(rx_qgrp->splitq.rxq_sets)) {
			err = EINVAL;
			goto err_alloc;
		}

		rx_qgrp->splitq.num_rxq_sets = num_rxq;
		for (j = 0; j < num_rxq; j++) {
			rx_qgrp->splitq.rxq_sets[j] = malloc(
			    sizeof(struct idpf_rxq_set), M_DEVBUF,
			    M_NOWAIT | M_ZERO);
			if (rx_qgrp->splitq.rxq_sets[j] == NULL) {
				err = ENOMEM;
				goto err_alloc;
			}
		}

		rx_qgrp->splitq.bufq_sets = malloc(rsrc->num_bufqs_per_qgrp *
		    sizeof(struct idpf_bufq_set), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (rx_qgrp->splitq.bufq_sets == NULL) {
			err = ENOMEM;
			goto err_alloc;
		}
		rx_qgrp->splitq.num_bufq_sets = rsrc->num_bufqs_per_qgrp;

		for (j = 0; j < rsrc->num_bufqs_per_qgrp; j++) {
			struct idpf_queue *bufq =
			    &rx_qgrp->splitq.bufq_sets[j].bufq;

			__idpf_rxq_init(vport, bufq);
			bufq->rxq_grp = rx_qgrp;
			bufq->idx = j;
			bufq->desc_count = rsrc->bufq_desc_count[j];
			bufq->rx_buf_size = rsrc->bufq_size[j];
			bufq->rx_buf_stride = IDPF_RX_BUF_STRIDE;
			bufq->rx.rxq_idx = i;
			bufq->q_type = VIRTCHNL2_QUEUE_TYPE_RX_BUFFER;

			/*
			 * Buffer lifetime belongs to iflib, so the Linux
			 * refill queues that mediated driver-owned buffer
			 * recycling are not allocated.
			 */
			rx_qgrp->splitq.bufq_sets[j].num_refillqs = 0;
			rx_qgrp->splitq.bufq_sets[j].refillqs = NULL;
		}

init_rxqs:
		for (j = 0; j < num_rxq; j++) {
			struct idpf_queue *q;

			if (split) {
				q = &rx_qgrp->splitq.rxq_sets[j]->rxq;
				q->rxq_grp = rx_qgrp;
				q->rx_ptype_lkup = adapter->splitq_pt_lkup;
				q->q_type = VIRTCHNL2_QUEUE_TYPE_RX;
			} else {
				q = rx_qgrp->singleq.rxqs[j];
				q->rx_ptype_lkup = adapter->singleq_pt_lkup;
				q->q_type = VIRTCHNL2_QUEUE_TYPE_RX;
			}

			__idpf_rxq_init(vport, q);
			q->idx = (i * num_rxq) + j;
			q->rx.rxq_idx = q->idx;
			q->desc_count = rsrc->rxq_desc_count;
			q->rx_buf_size = rsrc->bufq_size[0];
			/* iflib creates the ifnet after attach_pre returns. */
			q->rx_max_pkt_size = (vport->ifp != NULL ?
			    if_getmtu(vport->ifp) :
			    min(ETHERMTU, vport->max_mtu)) +
			    IDPF_PACKET_HDR_PAD;
			q->gen_rxcsum_status = 1;
			idpf_rxq_set_descids(rsrc, q);
		}
	}

	return (0);

err_alloc:
	idpf_rxq_group_rel(rsrc);
	return (err);
}

static void
idpf_rxq_group_rel(struct idpf_q_vec_rsrc *rsrc)
{
	unsigned int i, j;

	if (rsrc->rxq_grps == NULL)
		return;

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];

		if (idpf_is_queue_model_split(rsrc->rxq_model)) {
			for (j = 0; j < rx_qgrp->splitq.num_rxq_sets; j++) {
				free(rx_qgrp->splitq.rxq_sets[j], M_DEVBUF);
				rx_qgrp->splitq.rxq_sets[j] = NULL;
			}
			free(rx_qgrp->splitq.bufq_sets, M_DEVBUF);
			rx_qgrp->splitq.bufq_sets = NULL;
			continue;
		}

		for (j = 0; j < rx_qgrp->singleq.num_rxq; j++) {
			free(rx_qgrp->singleq.rxqs[j], M_DEVBUF);
			rx_qgrp->singleq.rxqs[j] = NULL;
		}
	}

	free(rsrc->rxq_grps, M_DEVBUF);
	rsrc->rxq_grps = NULL;
}

static void
idpf_vport_queue_grp_rel_all(struct idpf_q_vec_rsrc *rsrc)
{

	idpf_txq_group_rel(rsrc);
	idpf_rxq_group_rel(rsrc);
}

/**
 * idpf_fast_path_txq_init - build the flat TX queue lookup array
 * @vport: vport being brought up
 * @rsrc: queue and vector resources
 *
 * iflib addresses TX queues by a flat set index; this array turns that index
 * into a struct idpf_queue in one load on the hot path.  [LOCAL:A25]
 */
static int
idpf_fast_path_txq_init(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_ptp_vport_tx_tstamp_caps *caps = vport->tx_tstamp_caps;
	unsigned int i, j, k = 0;

	vport->txqs = malloc(rsrc->num_txq * sizeof(*vport->txqs), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (vport->txqs == NULL)
		return (ENOMEM);

	vport->num_txq = rsrc->num_txq;

	for (i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *tx_grp = &rsrc->txq_grps[i];

		for (j = 0; j < tx_grp->num_txq; j++, k++) {
			if (k >= rsrc->num_txq) {
				free(vport->txqs, M_DEVBUF);
				vport->txqs = NULL;
				vport->num_txq = 0;
				return (EINVAL);
			}
			vport->txqs[k] = tx_grp->txqs[j];
			vport->txqs[k]->idx = k;
			vport->txqs[k]->cached_tstamp_caps = caps;
		}
	}

	if (k != rsrc->num_txq) {
		free(vport->txqs, M_DEVBUF);
		vport->txqs = NULL;
		vport->num_txq = 0;
		return (EINVAL);
	}

	return (0);
}

/**
 * idpf_init_cached_phc_time - publish the cached PHC time pointer to queues
 * @vport: vport being brought up
 * @rsrc: queue and vector resources
 *
 * CONDITIONAL (feature 217 - PTP).  A NULL adapter->ptp leaves every queue
 * with a NULL pointer, which the datapath treats as "no timestamping".
 */
static void
idpf_init_cached_phc_time(struct idpf_vport *vport,
    struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_adapter *adapter = vport->adapter;
	bool split = idpf_is_queue_model_split(rsrc->rxq_model);
	unsigned int i, j;

	if (adapter->ptp == NULL)
		return;

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
		u16 num_rxq;

		num_rxq = split ? rx_qgrp->splitq.num_rxq_sets :
		    rx_qgrp->singleq.num_rxq;

		for (j = 0; j < num_rxq; j++) {
			struct idpf_queue *q;

			q = split ? &rx_qgrp->splitq.rxq_sets[j]->rxq :
			    rx_qgrp->singleq.rxqs[j];
			q->cached_phc_time = NULL;
		}
	}

	for (i = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

		for (j = 0; j < tx_qgrp->num_txq; j++)
			tx_qgrp->txqs[j]->cached_phc_time = NULL;
	}
}

/**
 * idpf_vport_queue_alloc_all - allocate all software queue structures
 * @vport: vport to allocate for
 * @rsrc: queue and vector resources
 *
 * Runs before iflib creates its queue sets, so no descriptor ring memory is
 * touched here.  idpf_tx_queues_alloc()/idpf_rx_queues_alloc() attach the
 * rings afterwards.  Returns 0 or an errno.  [FBSD15:A30-A31]
 */
int
idpf_vport_queue_alloc_all(struct idpf_vport *vport,
    struct idpf_q_vec_rsrc *rsrc)
{
	u16 num_txq, num_rxq;
	int err;

	/*
	 * iflib asks for the rings during attach, while the Linux flow only
	 * built these at open, so this now runs from both paths.
	 */
	if (rsrc->txq_grps != NULL)
		return (0);

	if (idpf_is_queue_model_split(rsrc->txq_model))
		num_txq = IDPF_DFLT_SPLITQ_TXQ_PER_GROUP;
	else
		num_txq = rsrc->num_txq;

	if (idpf_is_queue_model_split(rsrc->rxq_model))
		num_rxq = IDPF_DFLT_SPLITQ_RXQ_PER_GROUP;
	else
		num_rxq = rsrc->num_rxq;

	err = idpf_txq_group_alloc(vport, rsrc, num_txq);
	if (err != 0)
		goto err_out;

	err = idpf_rxq_group_alloc(vport, rsrc, num_rxq);
	if (err != 0)
		goto err_out;

	err = idpf_fast_path_txq_init(vport, rsrc);
	if (err != 0)
		goto err_out;

	idpf_init_cached_phc_time(vport, rsrc);

	return (0);

err_out:
	idpf_vport_queues_rel(vport, rsrc);
	return (err);
}

/**
 * idpf_vport_queues_rel - release all software queue structures
 * @vport: vport being torn down
 * @rsrc: queue and vector resources
 */
void
idpf_vport_queues_rel(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{

	idpf_vport_queue_grp_rel_all(rsrc);
	free(vport->txqs, M_DEVBUF);
	vport->txqs = NULL;
	vport->num_txq = 0;
}

/* ---------------------------------------------------------------------------
 * Queue count and geometry calculation
 * ------------------------------------------------------------------------- */

/**
 * idpf_vport_init_num_qs - record the queue counts the control plane granted
 * @vport: vport being initialised
 * @vport_msg: CREATE_VPORT response
 * @rsrc: queue and vector resources
 * [IDPF:A13-A14]
 */
void
idpf_vport_init_num_qs(struct idpf_vport *vport,
    struct virtchnl2_create_vport *vport_msg, struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_vport_user_config_data *config_data;

	config_data = &vport->adapter->vport_config[vport->idx]->user_config;

	rsrc->num_txq = le16toh(vport_msg->num_tx_q);
	rsrc->num_rxq = le16toh(vport_msg->num_rx_q);

	if (config_data->num_req_tx_qs == 0 && config_data->num_req_rx_qs == 0) {
		config_data->num_req_tx_qs = rsrc->num_txq;
		config_data->num_req_rx_qs = rsrc->num_rxq;
	}

	if (idpf_is_queue_model_split(rsrc->txq_model))
		rsrc->num_complq = le16toh(vport_msg->num_tx_complq);
	else
		rsrc->num_complq = 0;

	if (!idpf_is_queue_model_split(rsrc->rxq_model)) {
		rsrc->num_bufqs_per_qgrp = 0;
		rsrc->num_bufq = 0;
		rsrc->bufq_size[0] = IDPF_RX_BUF_2048;
		return;
	}

	rsrc->num_bufq = le16toh(vport_msg->num_rx_bufq);
	rsrc->num_bufqs_per_qgrp = IDPF_MAX_BUFQS_PER_RXQ_GRP;
	rsrc->bufq_size[0] = IDPF_RX_BUF_4096;
	rsrc->bufq_size[1] = IDPF_RX_BUF_2048;
}

/**
 * idpf_vport_calc_num_q_desc - derive descriptor counts for every queue type
 * @vport: vport being initialised
 * @rsrc: queue and vector resources
 * [IDPF:A13-A14]
 */
void
idpf_vport_calc_num_q_desc(struct idpf_vport *vport,
    struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_vport_user_config_data *config_data;
	u8 num_bufqs = rsrc->num_bufqs_per_qgrp;
	u32 num_req_txq_desc, num_req_rxq_desc;
	unsigned int i;

	config_data = &vport->adapter->vport_config[vport->idx]->user_config;
	num_req_txq_desc = config_data->num_req_txq_desc;
	num_req_rxq_desc = config_data->num_req_rxq_desc;

	rsrc->complq_desc_count = 0;

	if (num_req_txq_desc != 0) {
		rsrc->txq_desc_count = num_req_txq_desc;
		if (idpf_is_queue_model_split(rsrc->txq_model)) {
			rsrc->complq_desc_count = num_req_txq_desc;
			if (rsrc->complq_desc_count < IDPF_MIN_TXQ_COMPLQ_DESC)
				rsrc->complq_desc_count =
				    IDPF_MIN_TXQ_COMPLQ_DESC;
		}
	} else {
		rsrc->txq_desc_count = IDPF_DFLT_TX_Q_DESC_COUNT;
		if (idpf_is_queue_model_split(rsrc->txq_model))
			rsrc->complq_desc_count =
			    IDPF_DFLT_TX_COMPLQ_DESC_COUNT;
	}

	rsrc->rxq_desc_count = num_req_rxq_desc != 0 ? num_req_rxq_desc :
	    IDPF_DFLT_RX_Q_DESC_COUNT;

	for (i = 0; i < num_bufqs; i++) {
		if (rsrc->bufq_desc_count[i] == 0)
			rsrc->bufq_desc_count[i] =
			    IDPF_RX_BUFQ_DESC_COUNT(rsrc->rxq_desc_count,
			    num_bufqs);
	}
}

/**
 * idpf_vport_calc_total_qs - fill in the queue counts requested at vport create
 * @adapter: private data struct
 * @vport_idx: index of the vport being created
 * @vport_msg: CREATE_VPORT request being built
 * @max_q: per-vport queue limits
 * [IDPF:A13-A14]
 */
void
idpf_vport_calc_total_qs(struct idpf_adapter *adapter, u16 vport_idx,
    struct virtchnl2_create_vport *vport_msg, struct idpf_vport_max_q *max_q)
{
	struct idpf_vport_config *vport_config;
	u16 num_txq, num_rxq, num_complq = 0, num_bufq = 0;

	vport_config = adapter->vport_config[vport_idx];

	if (vport_config != NULL &&
	    vport_config->user_config.num_req_tx_qs != 0 &&
	    vport_config->user_config.num_req_rx_qs != 0) {
		num_txq = vport_config->user_config.num_req_tx_qs;
		num_rxq = vport_config->user_config.num_req_rx_qs;
	} else {
		u16 dflt_tx = IDPF_DFLT_NUM_Q;
		u16 dflt_rx = IDPF_DFLT_NUM_Q;

		if (max_q->max_txq < dflt_tx)
			dflt_tx = max_q->max_txq;
		if (max_q->max_rxq < dflt_rx)
			dflt_rx = max_q->max_rxq;

		num_txq = min(dflt_tx, (u16)mp_ncpus);
		num_rxq = min(dflt_rx, (u16)mp_ncpus);
	}

	if (num_txq == 0)
		num_txq = IDPF_MIN_Q;
	if (num_rxq == 0)
		num_rxq = IDPF_MIN_Q;

	if (idpf_is_queue_model_split(le16toh(vport_msg->txq_model)))
		num_complq = num_txq * IDPF_COMPLQ_PER_GROUP;
	if (idpf_is_queue_model_split(le16toh(vport_msg->rxq_model)))
		num_bufq = num_rxq * IDPF_MAX_BUFQS_PER_RXQ_GRP;

	vport_msg->num_tx_q = htole16(num_txq);
	vport_msg->num_tx_complq = htole16(num_complq);
	vport_msg->num_rx_q = htole16(num_rxq);
	vport_msg->num_rx_bufq = htole16(num_bufq);
}

/**
 * idpf_vport_calc_num_q_groups - derive the number of TX and RX queue groups
 * @rsrc: queue and vector resources
 * [IDPF:A13-A14]
 */
void
idpf_vport_calc_num_q_groups(struct idpf_q_vec_rsrc *rsrc)
{

	rsrc->num_txq_grp = idpf_is_queue_model_split(rsrc->txq_model) ?
	    rsrc->num_complq : IDPF_DFLT_SINGLEQ_TX_Q_GROUPS;
	rsrc->num_rxq_grp = idpf_is_queue_model_split(rsrc->rxq_model) ?
	    rsrc->num_rxq : IDPF_DFLT_SINGLEQ_RX_Q_GROUPS;
}

/* ---------------------------------------------------------------------------
 * iflib ring binding
 * ------------------------------------------------------------------------- */

/**
 * idpf_txrx_ring_reset - reset the software view of a descriptor ring
 * @q: queue whose indices are reset
 */
static void
idpf_txrx_ring_reset(struct idpf_queue *q)
{

	q->next_to_use = 0;
	q->next_to_clean = 0;
	q->next_to_alloc = 0;
	idpf_queue_set(GEN_CHK, q);
}

/**
 * idpf_tx_queues_alloc - bind iflib TX rings to IDPF queues
 * @ctx: iflib context
 * @vaddrs: per-ring kernel virtual addresses, indexed [set * ntxqs + ring]
 * @paddrs: matching bus addresses
 * @ntxqs: rings per TX queue set
 * @ntxqsets: number of TX queue sets
 *
 * ifdi_tx_queues_alloc() implementation.  With IFLIB_HAS_TXCQ ring 0 of each
 * set is the completion queue and ring 1 is the data ring, matching
 * sys/net/iflib.c (first_txq = 1).  Returns 0 or an errno.
 * [FBSD15:A30-A31]
 */
int
idpf_tx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs, u64 *paddrs,
    int ntxqs, int ntxqsets)
{
	struct idpf_vport *vport = idpf_softc_to_vport(iflib_get_softc(ctx));
	struct idpf_q_vec_rsrc *rsrc;

	/* iflib can reach here before the vport exists; fail, do not fault. */
	if (vport == NULL) {
		device_printf(iflib_get_dev(ctx),
		    "queue setup before a vport exists\n");
		return (ENXIO);
	}
	rsrc = &vport->dflt_qv_rsrc;
	if (rsrc->txq_grps == NULL) {
		device_printf(iflib_get_dev(ctx),
		    "TX queue structures not allocated\n");
		return (ENXIO);
	}
	bool split = idpf_is_queue_model_split(rsrc->txq_model);
	int data_ring = split ? 1 : 0;
	int i;

	if (ntxqsets != rsrc->num_txq || ntxqs != (split ? 2 : 1)) {
		device_printf(vport->adapter->dev,
		    "iflib TX geometry mismatch: %d sets x %d rings, "
		    "expected %u x %d\n", ntxqsets, ntxqs, rsrc->num_txq,
		    split ? 2 : 1);
		return (EINVAL);
	}

	for (i = 0; i < ntxqsets; i++) {
		struct idpf_queue *txq = idpf_txq(vport, i);
		struct idpf_queue *complq;

		txq->desc_ring = vaddrs[i * ntxqs + data_ring];
		txq->dma = paddrs[i * ntxqs + data_ring];
		txq->size = txq->desc_count * (split ?
		    sizeof(union idpf_tx_flex_desc) :
		    sizeof(struct idpf_base_tx_desc));
		idpf_txrx_ring_reset(txq);
		txq->tx.num_completions = 0;
		txq->tx.last_re = 0;

		if (!split)
			continue;

		complq = txq->txq_grp->complq;
		complq->desc_ring = vaddrs[i * ntxqs + 0];
		complq->dma = paddrs[i * ntxqs + 0];
		complq->size = complq->desc_count *
		    sizeof(struct idpf_splitq_tx_compl_desc);
		idpf_txrx_ring_reset(complq);
		complq->tx.num_completions = 0;
	}

	return (0);
}

/**
 * idpf_rx_queues_alloc - bind iflib RX rings to IDPF queues
 * @ctx: iflib context
 * @vaddrs: per-ring kernel virtual addresses, indexed [set * nrxqs + ring]
 * @paddrs: matching bus addresses
 * @nrxqs: rings per RX queue set
 * @nrxqsets: number of RX queue sets
 *
 * ifdi_rx_queues_alloc() implementation.  With IFLIB_HAS_RXCQ ring 0 of each
 * set is the RX completion queue and rings 1..n are the free lists, which map
 * onto the IDPF buffer queues of the same group.  Returns 0 or an errno.
 * [FBSD15:A30-A31]
 */
int
idpf_rx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs, u64 *paddrs,
    int nrxqs, int nrxqsets)
{
	struct idpf_vport *vport = idpf_softc_to_vport(iflib_get_softc(ctx));
	struct idpf_q_vec_rsrc *rsrc;

	/* iflib can reach here before the vport exists; fail, do not fault. */
	if (vport == NULL) {
		device_printf(iflib_get_dev(ctx),
		    "queue setup before a vport exists\n");
		return (ENXIO);
	}
	rsrc = &vport->dflt_qv_rsrc;
	bool split = idpf_is_queue_model_split(rsrc->rxq_model);
	int expect = split ? 1 + rsrc->num_bufqs_per_qgrp : 1;
	int i, j;

	if (nrxqsets != rsrc->num_rxq || nrxqs != expect) {
		device_printf(vport->adapter->dev,
		    "iflib RX geometry mismatch: %d sets x %d rings, "
		    "expected %u x %d\n", nrxqsets, nrxqs, rsrc->num_rxq,
		    expect);
		return (EINVAL);
	}

	for (i = 0; i < nrxqsets; i++) {
		struct idpf_queue *rxq = idpf_rxq(rsrc, i);

		rxq->desc_ring = vaddrs[i * nrxqs + 0];
		rxq->dma = paddrs[i * nrxqs + 0];
		rxq->size = rxq->desc_count * sizeof(union virtchnl2_rx_desc);
		idpf_txrx_ring_reset(rxq);
		rxq->rx.mbuf = NULL;

		if (!split)
			continue;

		for (j = 0; j < rsrc->num_bufqs_per_qgrp; j++) {
			struct idpf_queue *bufq = idpf_bufq(rsrc, i, j);

			bufq->desc_ring = vaddrs[i * nrxqs + 1 + j];
			bufq->dma = paddrs[i * nrxqs + 1 + j];
			bufq->size = bufq->desc_count *
			    sizeof(struct virtchnl2_splitq_rx_buf_desc);
			idpf_txrx_ring_reset(bufq);
		}
	}

	return (0);
}

/**
 * idpf_vport_set_rx_frame_size - publish a new MTU into the RX queues
 * @rsrc: queue and vector resources
 * @mtu: new interface MTU
 *
 * The queues survive a stop/init cycle, so an MTU change has to be written
 * into them before the queues are reconfigured.
 */
void
idpf_vport_set_rx_frame_size(struct idpf_q_vec_rsrc *rsrc, u32 mtu)
{
	u16 i;

	if (rsrc->rxq_grps == NULL)
		return;

	for (i = 0; i < rsrc->num_rxq; i++) {
		struct idpf_queue *rxq = idpf_rxq(rsrc, i);

		if (rxq != NULL)
			rxq->rx_max_pkt_size = mtu + IDPF_PACKET_HDR_PAD;
	}
}

/**
 * idpf_queues_free - ifdi_queues_free() implementation
 * @ctx: iflib context
 *
 * iflib frees the descriptor rings themselves; only the driver bookkeeping is
 * released here.
 */
void
idpf_queues_free(if_ctx_t ctx)
{
	struct idpf_vport *vport = idpf_softc_to_vport(iflib_get_softc(ctx));

	/* iflib also unwinds through here when attach fails before the vport. */
	if (vport == NULL)
		return;

	idpf_vport_queues_rel(vport, &vport->dflt_qv_rsrc);
}

/* ---------------------------------------------------------------------------
 * Hardware tail (doorbell) publication
 * ------------------------------------------------------------------------- */

/**
 * idpf_tx_buf_hw_update - publish a TX producer index to hardware
 * @tx_q: queue to ring
 * @val: new producer index
 * @xmit_more: true when more packets are already queued behind this one
 *
 * @tail is NULL until the control plane has mapped the queue's doorbell, so
 * the software index is always updated first and the store skipped when the
 * mapping is absent.  [FBSD15:A31] [IDPF:A13-A14]
 */
void
idpf_tx_buf_hw_update(struct idpf_queue *tx_q, u32 val, bool xmit_more)
{

	tx_q->next_to_use = val;

	if (xmit_more || tx_q->tail == NULL)
		return;

	idpf_reg_wr32(tx_q->tail, val);
}

/**
 * idpf_rx_buf_hw_update - publish an RX buffer producer index to hardware
 * @rxq: buffer queue (split model) or RX queue (single model)
 * @val: new producer index
 * [FBSD15:A31] [IDPF:A13-A14]
 */
void
idpf_rx_buf_hw_update(struct idpf_queue *rxq, u32 val)
{

	rxq->next_to_use = val;

	if (rxq->tail == NULL)
		return;

	idpf_reg_wr32(rxq->tail, val);
}

/* ---------------------------------------------------------------------------
 * TX descriptor construction
 * ------------------------------------------------------------------------- */

/**
 * idpf_tx_splitq_build_ctb - build a queue-scheduled flex data descriptor
 * @desc: descriptor to populate
 * @params: TX parameters
 * @td_cmd: command bits
 * @size: buffer size in bytes
 * [IDPF:A13-A14]
 */
void
idpf_tx_splitq_build_ctb(union idpf_tx_flex_desc *desc,
    struct idpf_tx_splitq_params *params, u16 td_cmd, u16 size)
{

	desc->q.qw1.cmd_dtype =
	    htole16(IDPF_FIELD_PREP(IDPF_FLEX_TXD_QW1_DTYPE_M, params->dtype) |
	    IDPF_FIELD_PREP(IDPF_FLEX_TXD_QW1_CMD_M, td_cmd));
	desc->q.qw1.buf_size = htole16(size);
	desc->q.qw1.l2tags.l2tag1 = htole16(params->td_tag);
}

/**
 * idpf_tx_splitq_build_flow_desc - build a flow-scheduled data descriptor
 * @desc: descriptor to populate
 * @params: TX parameters
 * @td_cmd: command bits
 * @size: buffer size in bytes
 *
 * Retained for protocol completeness.  This port always negotiates
 * queue-based scheduling, so the flow-scheduled path is not exercised on the
 * iflib datapath.  [IDPF:A13-A14]
 */
void
idpf_tx_splitq_build_flow_desc(union idpf_tx_flex_desc *desc,
    struct idpf_tx_splitq_params *params, u16 td_cmd, u16 size)
{

	desc->flow.qw1.cmd_dtype = (u8)(params->dtype | td_cmd);
	desc->flow.qw1.rxr_bufsize = htole16(size & IDPF_TXD_FLEX_FLOW_BUFSIZE_M);
	desc->flow.qw1.compl_tag = htole16(params->compl_tag);
	desc->flow.qw1.ts[0] = params->offload.desc_ts[0];
	desc->flow.qw1.ts[1] = params->offload.desc_ts[1];
	desc->flow.qw1.ts[2] = params->offload.desc_ts[2];
}

/**
 * idpf_tx_splitq_tso_setup - write a flex TSO context descriptor
 * @txq: queue being filled
 * @pi: iflib packet description
 * @idx: descriptor index to write
 *
 * Returns the next descriptor index.  [IDPF:A13-A14]
 */
static u16
idpf_tx_splitq_tso_setup(struct idpf_queue *txq, if_pkt_info_t pi,
    u16 idx)
{
	union idpf_flex_tx_ctx_desc *ctx;
	u32 hdr_len, tso_len;

	ctx = IDPF_FLEX_TX_CTX_DESC(txq, idx);

	hdr_len = pi->ipi_ehdrlen + pi->ipi_ip_hlen + pi->ipi_tcp_hlen;
	tso_len = pi->ipi_len - hdr_len;

	ctx->tso.qw1.cmd_dtype = htole16(IDPF_TX_DESC_DTYPE_FLEX_TSO_CTX |
	    IDPF_TX_FLEX_CTX_DESC_CMD_TSO);
	ctx->tso.qw0.flex_tlen = htole32(tso_len & IDPF_TXD_FLEX_CTX_TLEN_M);
	ctx->tso.qw0.mss_rt = htole16(pi->ipi_tso_segsz &
	    IDPF_TXD_FLEX_CTX_MSS_RT_M);
	ctx->tso.qw0.hdr_len = (u8)hdr_len;
	ctx->tso.qw0.flex = 0;
	memset(ctx->tso.qw1.flex, 0, sizeof(ctx->tso.qw1.flex));

	return (idpf_ring_next(idx, txq->desc_count));
}

/**
 * idpf_tx_splitq_encap - translate an iflib packet into flex TX descriptors
 * @txq: queue being filled
 * @pi: packet description carrying an already mapped scatter list
 *
 * Length and segment count have already been validated by the caller.
 * Returns 0.  [FBSD15:A30-A31] [IDPF:A13-A14]
 */
static int
idpf_tx_splitq_encap(struct idpf_queue *txq, if_pkt_info_t pi)
{
	struct idpf_tx_splitq_params params = {
		.dtype = IDPF_TX_DESC_DTYPE_FLEX_L2TAG1_L2TAG2,
		.td_tag = pi->ipi_vtag,
	};
	bus_dma_segment_t *segs = pi->ipi_segs;
	u16 i = pi->ipi_pidx;
	u16 td_cmd = 0;
	int j, nsegs = pi->ipi_nsegs;

	if ((pi->ipi_csum_flags & CSUM_TSO) != 0) {
		i = idpf_tx_splitq_tso_setup(txq, pi, i);
		txq->q_stats.tx.lso_pkts++;
		txq->q_stats.tx.lso_bytes += pi->ipi_len;
		txq->q_stats.tx.lso_segs_tot += howmany(pi->ipi_len,
		    pi->ipi_tso_segsz);
	}

	if ((pi->ipi_csum_flags & IDPF_CSUM_OFFLOAD) != 0)
		td_cmd |= IDPF_TX_FLEX_DESC_CMD_CS_EN;
	if ((pi->ipi_mflags & M_VLANTAG) != 0)
		td_cmd |= IDPF_TX_FLEX_DESC_CMD_IL2TAG1;

	for (j = 0; j < nsegs; j++) {
		union idpf_tx_flex_desc *desc;
		bus_size_t len = segs[j].ds_len;
		bus_addr_t addr = segs[j].ds_addr;

		/*
		 * A segment longer than the descriptor length field is split
		 * on a 4 KiB-aligned boundary, which is the device's maximum
		 * read request granularity.
		 */
		while (len > IDPF_TX_MAX_DESC_DATA) {
			desc = IDPF_FLEX_TX_DESC(txq, i);
			desc->q.buf_addr = htole64(addr);
			idpf_tx_splitq_build_ctb(desc, &params, td_cmd,
			    IDPF_TX_MAX_DESC_DATA_ALIGNED);
			i = idpf_ring_next(i, txq->desc_count);
			addr += IDPF_TX_MAX_DESC_DATA_ALIGNED;
			len -= IDPF_TX_MAX_DESC_DATA_ALIGNED;
		}

		desc = IDPF_FLEX_TX_DESC(txq, i);
		desc->q.buf_addr = htole64(addr);
		if (j == nsegs - 1)
			td_cmd |= IDPF_TX_FLEX_DESC_CMD_EOP |
			    IDPF_TX_FLEX_DESC_CMD_RS;
		idpf_tx_splitq_build_ctb(desc, &params, td_cmd,
		    (u16)len);
		i = idpf_ring_next(i, txq->desc_count);
	}

	pi->ipi_new_pidx = i;

	return (0);
}

/**
 * idpf_isc_txd_encap - translate an iflib packet into TX descriptors
 * @arg: vport (the iflib softc)
 * @pi: packet description carrying an already mapped scatter list
 *
 * Returns 0 on success or an errno; on error no descriptor is published and
 * the mbuf stays owned by iflib.  [FBSD15:A30-A31] [IDPF:A13-A14]
 */
static int
idpf_isc_txd_encap(void *arg, if_pkt_info_t pi)
{
	struct idpf_vport *vport = idpf_softc_to_vport(arg);
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_queue *txq = idpf_txq(vport, pi->ipi_qsidx);
	int err;

	if (__predict_false(pi->ipi_len < txq->tx_min_pkt_len)) {
		txq->q_stats.tx.skb_drops++;
		return (EINVAL);
	}

	/*
	 * iflib sizes its scatter list from isc_tx_nsegments, but the device
	 * limit comes from the negotiated max_sg_bufs_per_tx_pkt.  Reject
	 * rather than silently overrun the ring accounting; iflib will
	 * linearize and retry.
	 */
	if (__predict_false(pi->ipi_nsegs > txq->tx_max_bufs)) {
		txq->q_stats.tx.linearize++;
		return (EFBIG);
	}

	if (idpf_is_queue_model_split(rsrc->txq_model))
		err = idpf_tx_splitq_encap(txq, pi);
	else
		err = idpf_tx_singleq_encap(txq, pi);

	if (__predict_false(err != 0))
		return (err);

	txq->q_stats.tx.packets++;
	txq->q_stats.tx.bytes += pi->ipi_len;

	return (0);
}

/**
 * idpf_isc_txd_flush - ring the TX doorbell
 * @arg: vport (the iflib softc)
 * @txqid: TX queue set index
 * @pidx: producer index to publish
 */
static void
idpf_isc_txd_flush(void *arg, u16 txqid, qidx_t pidx)
{
	struct idpf_vport *vport = idpf_softc_to_vport(arg);
	struct idpf_queue *txq = idpf_txq(vport, txqid);

	MPASS(pidx < txq->desc_count);
	idpf_tx_buf_hw_update(txq, pidx, false);
}

/**
 * idpf_tx_handle_sw_marker - note that a queue drained past a software marker
 * @tx_q: queue whose marker completed
 *
 * The vport-wide wait is released only once every queue has been marked.
 * [FBSD15:A32-A33] [IDPF:A13-A14]
 */
static void
idpf_tx_handle_sw_marker(struct idpf_queue *tx_q)
{
	struct idpf_vport *vport = tx_q->vport;
	u16 i;

	idpf_queue_clear(SW_MARKER, tx_q);

	for (i = 0; i < vport->num_txq; i++) {
		if (idpf_queue_has(SW_MARKER, vport->txqs[i]))
			return;
	}

	mtx_lock(&vport->sw_marker_lock);
	vport->flags |= (1u << IDPF_VPORT_SW_MARKER);
	cv_broadcast(&vport->sw_marker_cv);
	mtx_unlock(&vport->sw_marker_lock);
}

/**
 * idpf_tx_splitq_credits - drain a TX completion queue into a credit count
 * @txq: data queue being reclaimed
 * @clear: commit the scan when true, report only when false
 *
 * Queue-based scheduling reports an in-order hardware head in every RE and RS
 * completion, so the credit count is simply the distance the head advanced.
 * Every field taken out of a completion descriptor is device-authored and is
 * range checked before use.  [IDPF:A13-A14] [FBSD15:A30]
 */
static int
idpf_tx_splitq_credits(struct idpf_queue *txq, bool clear)
{
	struct idpf_queue *complq = txq->txq_grp->complq;
	struct idpf_splitq_tx_compl_desc *desc;
	u16 ntc = complq->next_to_clean;
	u16 head = txq->next_to_clean;
	bool gen = idpf_queue_has(GEN_CHK, complq);
	unsigned int budget = IDPF_TX_COMPLQ_CLEAN_BUDGET;
	u32 completions = 0;
	int credits;

	while (budget-- != 0) {
		u16 qid_comptype_gen, rel_qid;
		u8 ctype;

		desc = IDPF_SPLITQ_TX_COMPLQ_DESC(complq, ntc);
		qid_comptype_gen = le16toh(desc->qid_comptype_gen);

		if (!!IDPF_FIELD_GET(IDPF_TXD_COMPLQ_GEN_M,
		    qid_comptype_gen) != gen)
			break;

		rel_qid = IDPF_FIELD_GET(IDPF_TXD_COMPLQ_QID_M,
		    qid_comptype_gen);
		ctype = IDPF_FIELD_GET(IDPF_TXD_COMPLQ_COMPL_TYPE_M,
		    qid_comptype_gen);

		if (__predict_false(rel_qid >= complq->txq_grp->num_txq)) {
			txq->q_stats.tx.q_busy++;
			goto next;
		}

		switch (ctype) {
		case IDPF_TXD_COMPLT_RE:
		case IDPF_TXD_COMPLT_RS: {
			u16 hw_head =
			    le16toh(desc->q_head_compl_tag.q_head);

			if (__predict_false(hw_head >= txq->desc_count))
				break;
			head = hw_head;
			break;
		}
		case IDPF_TXD_COMPLT_SW_MARKER:
			idpf_tx_handle_sw_marker(
			    complq->txq_grp->txqs[rel_qid]);
			break;
		default:
			/*
			 * RULE_MISS and REINJECTED only occur under flow
			 * scheduling, which this port never negotiates.
			 */
			break;
		}

		completions++;
next:
		ntc = idpf_ring_next(ntc, complq->desc_count);
		if (ntc == 0)
			gen = !gen;
	}

	credits = idpf_ring_delta(txq->next_to_clean, head, txq->desc_count);

	if (clear) {
		complq->next_to_clean = ntc;
		idpf_queue_assign(GEN_CHK, complq, gen);
		complq->tx.num_completions += completions;
		txq->next_to_clean = head;
	}

	return (credits);
}

/**
 * idpf_isc_txd_credits_update - report reclaimable TX descriptors to iflib
 * @arg: vport (the iflib softc)
 * @txqid: TX queue set index
 * @clear: commit the reclaim when true
 */
static int
idpf_isc_txd_credits_update(void *arg, u16 txqid, bool clear)
{
	struct idpf_vport *vport = idpf_softc_to_vport(arg);
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_queue *txq = idpf_txq(vport, txqid);

	if (idpf_is_queue_model_split(rsrc->txq_model))
		return (idpf_tx_splitq_credits(txq, clear));

	return (idpf_tx_singleq_credits(txq, clear));
}

/* ---------------------------------------------------------------------------
 * RX metadata decode
 * ------------------------------------------------------------------------- */

/**
 * idpf_ptype_to_htype - translate a decoded IDPF packet type to an RSS type
 * @decoded: decoded packet type
 *
 * Returns an M_HASHTYPE_* value; iflib stamps it onto the mbuf together with
 * iri_flowid.  [FBSD15:A30] [IDPF:A13-A14]
 */
u32
idpf_ptype_to_htype(const struct idpf_rx_ptype_decoded *decoded)
{

	if (!decoded->known)
		return (M_HASHTYPE_OPAQUE);

	if (decoded->outer_ip != IDPF_RX_PTYPE_OUTER_IP)
		return (M_HASHTYPE_OPAQUE);

	switch (decoded->outer_ip_ver) {
	case IDPF_RX_PTYPE_OUTER_IPV4:
		switch (decoded->inner_prot) {
		case IDPF_RX_PTYPE_INNER_PROT_TCP:
			return (M_HASHTYPE_RSS_TCP_IPV4);
		case IDPF_RX_PTYPE_INNER_PROT_UDP:
			return (M_HASHTYPE_RSS_UDP_IPV4);
		default:
			return (M_HASHTYPE_RSS_IPV4);
		}
	case IDPF_RX_PTYPE_OUTER_IPV6:
		switch (decoded->inner_prot) {
		case IDPF_RX_PTYPE_INNER_PROT_TCP:
			return (M_HASHTYPE_RSS_TCP_IPV6);
		case IDPF_RX_PTYPE_INNER_PROT_UDP:
			return (M_HASHTYPE_RSS_UDP_IPV6);
		default:
			return (M_HASHTYPE_RSS_IPV6);
		}
	default:
		return (M_HASHTYPE_OPAQUE);
	}
}

/**
 * idpf_rx_csum - translate hardware checksum status into iflib csum flags
 * @rxq: queue the packet arrived on
 * @ri: iflib receive descriptor info being filled
 * @csum_bits: decoded status and error bits
 * @decoded: decoded packet type
 *
 * FreeBSD has no equivalent of Linux CHECKSUM_COMPLETE, so a packet the
 * device validated is reported as fully verified and anything else is left
 * for the stack to check.  [FBSD15:A30] [IDPF:A13-A14]
 */
void
idpf_rx_csum(struct idpf_queue *rxq, if_rxd_info_t ri,
    const struct idpf_rx_csum_decoded *csum_bits,
    const struct idpf_rx_ptype_decoded *decoded)
{
	bool ipv4, ipv6;

	if (!idpf_is_feature_ena(rxq->vport, IFCAP_RXCSUM))
		return;

	/* The device only reports a verdict when it parsed L3/L4. */
	if (!csum_bits->l3l4p)
		return;

	ipv4 = IDPF_RX_PTYPE_TO_IPV(decoded, IDPF_RX_PTYPE_OUTER_IPV4);
	ipv6 = IDPF_RX_PTYPE_TO_IPV(decoded, IDPF_RX_PTYPE_OUTER_IPV6);

	if (ipv4 && (csum_bits->ipe || csum_bits->eipe)) {
		rxq->q_stats.rx.hw_csum_err++;
		return;
	}

	/*
	 * IPv6 extension headers are not parsed far enough for the L4 verdict
	 * to be trustworthy.
	 */
	if (ipv6 && csum_bits->ipv6exadd)
		return;

	if (csum_bits->l4e) {
		rxq->q_stats.rx.hw_csum_err++;
		return;
	}

	if (ipv4)
		ri->iri_csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;

	switch (decoded->inner_prot) {
	case IDPF_RX_PTYPE_INNER_PROT_ICMP:
	case IDPF_RX_PTYPE_INNER_PROT_TCP:
	case IDPF_RX_PTYPE_INNER_PROT_UDP:
	case IDPF_RX_PTYPE_INNER_PROT_SCTP:
		ri->iri_csum_flags |= CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
		ri->iri_csum_data = htons(0xffff);
		break;
	default:
		break;
	}
}

/**
 * idpf_rx_splitq_extract_csum_bits - decode split-model checksum status
 * @rx_desc: completion descriptor
 * @csum: decoded bits, filled in
 * [IDPF:A13-A14]
 */
static void
idpf_rx_splitq_extract_csum_bits(
    const struct virtchnl2_rx_flex_desc_adv_nic_3 *rx_desc,
    struct idpf_rx_csum_decoded *csum)
{
	u8 qword0 = rx_desc->status_err0_qw0;
	u8 qword1 = rx_desc->status_err0_qw1;

	csum->ipe = !!(qword1 &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_XSUM_IPE_M);
	csum->eipe = !!(qword1 &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_XSUM_EIPE_M);
	csum->eudpe = !!(qword1 &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_XSUM_EUDPE_M);
	csum->l4e = !!(qword1 &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_XSUM_L4E_M);
	csum->l3l4p = !!(qword1 &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_L3L4P_M);
	csum->ipv6exadd = !!(qword0 &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_IPV6EXADD_M);
	csum->raw_csum_inv = !!(le16toh(rx_desc->ptype_err_fflags0) &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_RAW_CSUM_INV_M);
	csum->raw_csum = le16toh(rx_desc->misc.raw_cs);
}

/**
 * idpf_rx_decode_ptype - bounds-checked packet type lookup
 * @rxq: queue the packet arrived on
 * @ptype: device-authored packet type identifier
 *
 * Returns a pointer to the decoded entry or NULL when the identifier is out
 * of range or the lookup table has not been received yet.  [IDPF:A13-A17]
 */
const struct idpf_rx_ptype_decoded *
idpf_rx_decode_ptype(struct idpf_queue *rxq, u16 ptype)
{
	u16 limit;

	if (__predict_false(rxq->rx_ptype_lkup == NULL))
		return (NULL);

	limit = idpf_is_queue_model_split(rxq->vport->dflt_qv_rsrc.rxq_model) ?
	    IDPF_RX_MAX_PTYPE : IDPF_RX_MAX_BASE_PTYPE;

	if (__predict_false(ptype >= limit))
		return (NULL);

	return (&rxq->rx_ptype_lkup[ptype]);
}

/* ---------------------------------------------------------------------------
 * RX datapath - split queue model
 * ------------------------------------------------------------------------- */

/**
 * idpf_rx_splitq_available - count complete packets waiting on a completion queue
 * @rxq: RX completion queue
 * @budget: maximum number of packets to report
 *
 * The scan is non-destructive: the generation view is taken from the queue's
 * own next_to_clean, which idpf_rx_splitq_pkt_get() advances in lockstep with
 * iflib's consumer index.  [IDPF:A13-A14]
 */
static int
idpf_rx_splitq_available(struct idpf_queue *rxq, qidx_t budget)
{
	u16 ntc = rxq->next_to_clean;
	bool gen = idpf_queue_has(GEN_CHK, rxq);
	int pkts = 0, descs = 0;

	while (pkts < budget && descs < rxq->desc_count) {
		const struct virtchnl2_rx_flex_desc_adv_nic_3 *rx_desc;
		u16 pktlen_gen_bufq_id;

		rx_desc = &IDPF_RX_DESC(rxq, ntc)->flex_adv_nic_3_wb;
		atomic_thread_fence_acq();

		pktlen_gen_bufq_id = le16toh(rx_desc->pktlen_gen_bufq_id);
		if (!!IDPF_FIELD_GET(VIRTCHNL2_RX_FLEX_DESC_ADV_GEN_M,
		    pktlen_gen_bufq_id) != gen)
			break;

		if ((rx_desc->status_err0_qw1 & IDPF_RXD_EOF_SPLITQ) != 0)
			pkts++;

		descs++;
		ntc = idpf_ring_next(ntc, rxq->desc_count);
		if (ntc == 0)
			gen = !gen;
	}

	return (pkts);
}

/**
 * idpf_rx_splitq_pkt_get - decode one split-model packet for iflib
 * @rxq: RX completion queue
 * @ri: receive descriptor info to fill
 *
 * Returns 0 on success or EBADMSG when the device produced a descriptor the
 * driver cannot honour; in that case the buffers are still released back to
 * iflib so the free list cannot leak.  [IDPF:A13-A17] [FBSD15:A30]
 */
static int
idpf_rx_splitq_pkt_get(struct idpf_queue *rxq, if_rxd_info_t ri)
{
	const struct idpf_rx_ptype_decoded *decoded = NULL;
	struct idpf_rx_csum_decoded csum_bits = { 0 };
	const struct virtchnl2_rx_flex_desc_adv_nic_3 *rx_desc = NULL;
	u16 ntc = rxq->next_to_clean;
	u32 total_len = 0;
	u16 ptype = IDPF_INVALID_PTYPE_ID;
	int nfrags = 0, err = 0;
	bool gen = idpf_queue_has(GEN_CHK, rxq);
	bool rsc = false;

	for (;;) {
		u16 pktlen_gen_bufq_id, pkt_len, buf_id, hdrlen_flags;
		u8 bufq_id, rxdid;
		bool eop;

		rx_desc = &IDPF_RX_DESC(rxq, ntc)->flex_adv_nic_3_wb;
		atomic_thread_fence_acq();

		pktlen_gen_bufq_id = le16toh(rx_desc->pktlen_gen_bufq_id);
		if (__predict_false(
		    !!IDPF_FIELD_GET(VIRTCHNL2_RX_FLEX_DESC_ADV_GEN_M,
		    pktlen_gen_bufq_id) != gen)) {
			/*
			 * isc_rxd_available() promised this descriptor; a
			 * generation mismatch here means the ring state and
			 * the device disagree.
			 */
			rxq->q_stats.rx.bad_descs++;
			return (EBADMSG);
		}

		rxdid = IDPF_FIELD_GET(VIRTCHNL2_RX_FLEX_DESC_ADV_RXDID_M,
		    rx_desc->rxdid_ucast);
		if (__predict_false(rxdid != VIRTCHNL2_RXDID_2_FLEX_SPLITQ)) {
			rxq->q_stats.rx.bad_descs++;
			err = EBADMSG;
		}

		pkt_len = IDPF_FIELD_GET(
		    VIRTCHNL2_RX_FLEX_DESC_ADV_LEN_PBUF_M, pktlen_gen_bufq_id);
		bufq_id = IDPF_FIELD_GET(
		    VIRTCHNL2_RX_FLEX_DESC_ADV_BUFQ_ID_M, pktlen_gen_bufq_id);
		buf_id = le16toh(rx_desc->buf_id);
		hdrlen_flags = le16toh(rx_desc->hdrlen_flags);
		eop = (rx_desc->status_err0_qw1 & IDPF_RXD_EOF_SPLITQ) != 0;

		if ((hdrlen_flags & VIRTCHNL2_RX_FLEX_DESC_ADV_RSC_M) != 0)
			rsc = true;

		/*
		 * Everything below is device authored and indexes either the
		 * iflib free list or the ptype table, so it is validated
		 * before it is used.
		 */
		if (__predict_false(bufq_id >=
		    rxq->vport->dflt_qv_rsrc.num_bufqs_per_qgrp) ||
		    __predict_false(buf_id >=
		    idpf_bufq(&rxq->vport->dflt_qv_rsrc, rxq->rx.rxq_idx,
		    bufq_id)->desc_count) ||
		    __predict_false(pkt_len > rxq->rx_max_pkt_size) ||
		    __predict_false(nfrags >= IFLIB_MAX_RX_SEGS)) {
			rxq->q_stats.rx.bad_descs++;
			err = EBADMSG;
			goto advance;
		}

		if (nfrags == 0)
			ptype = IDPF_FIELD_GET(
			    VIRTCHNL2_RX_FLEX_DESC_ADV_PTYPE_M,
			    le16toh(rx_desc->ptype_err_fflags0));

		ri->iri_frags[nfrags].irf_flid = bufq_id;
		ri->iri_frags[nfrags].irf_idx = buf_id;
		ri->iri_frags[nfrags].irf_len = pkt_len;
		nfrags++;
		total_len += pkt_len;

advance:
		ntc = idpf_ring_next(ntc, rxq->desc_count);
		if (ntc == 0)
			gen = !gen;

		if (eop)
			break;

		if (__predict_false(nfrags >= IFLIB_MAX_RX_SEGS)) {
			rxq->q_stats.rx.bad_descs++;
			err = EBADMSG;
			break;
		}
	}

	rxq->next_to_clean = ntc;
	idpf_queue_assign(GEN_CHK, rxq, gen);
	ri->iri_cidx = ntc;
	ri->iri_nfrags = nfrags;
	ri->iri_len = (u16)total_len;

	if (err != 0 || nfrags == 0)
		return (err != 0 ? err : EBADMSG);

	decoded = idpf_rx_decode_ptype(rxq, ptype);
	if (decoded != NULL && decoded->known) {
		ri->iri_flowid = le16toh(rx_desc->hash1) |
		    ((u32)rx_desc->ff2_mirrid_hash2.hash2 << 16) |
		    ((u32)rx_desc->hash3 << 24);
		ri->iri_rsstype = idpf_ptype_to_htype(decoded);

		idpf_rx_splitq_extract_csum_bits(rx_desc, &csum_bits);
		idpf_rx_csum(rxq, ri, &csum_bits, decoded);
	} else {
		ri->iri_rsstype = M_HASHTYPE_OPAQUE;
	}

	if ((rx_desc->status_err0_qw0 &
	    VIRTCHNL2_RX_FLEX_DESC_ADV_STATUS0_L2TAG1P_M) != 0) {
		ri->iri_vtag = le16toh(rx_desc->l2tag1);
		ri->iri_flags |= M_VLANTAG;
	}

	if (rsc) {
		rxq->q_stats.rx.rsc_pkts++;
		rxq->q_stats.rx.rsc_bytes += total_len;
	}

	rxq->q_stats.rx.packets++;
	rxq->q_stats.rx.bytes += total_len;

	return (0);
}

/**
 * idpf_rx_splitq_refill - post iflib buffers to a buffer queue
 * @bufq: buffer queue receiving the addresses
 * @iru: iflib refill request
 * [FBSD15:A30-A31] [IDPF:A13-A14]
 */
static void
idpf_rx_splitq_refill(struct idpf_queue *bufq, if_rxd_update_t iru)
{
	struct virtchnl2_splitq_rx_buf_desc *desc;
	u32 pidx = iru->iru_pidx;
	u16 i;

	for (i = 0; i < iru->iru_count; i++) {
		MPASS(pidx < bufq->desc_count);

		desc = IDPF_SPLITQ_RX_BUF_DESC(bufq, pidx);
		desc->pkt_addr = htole64(iru->iru_paddrs[i]);
		desc->hdr_addr = htole64(0);
		desc->qword0.buf_id = htole16(pidx);
		desc->qword0.rsvd0 = 0;
		desc->qword0.rsvd1 = 0;
		desc->rsvd2 = 0;

		pidx = idpf_ring_next(pidx, bufq->desc_count);
	}

	bufq->next_to_alloc = pidx;
}

/* ---------------------------------------------------------------------------
 * iflib RX callbacks
 * ------------------------------------------------------------------------- */

static int
idpf_isc_rxd_available(void *arg, u16 rxqid, qidx_t idx, qidx_t budget)
{
	struct idpf_vport *vport = idpf_softc_to_vport(arg);
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_queue *rxq = idpf_rxq(rsrc, rxqid);

	if (idpf_is_queue_model_split(rsrc->rxq_model))
		return (idpf_rx_splitq_available(rxq, budget));

	return (idpf_rx_singleq_available(rxq, idx, budget));
}

static int
idpf_isc_rxd_pkt_get(void *arg, if_rxd_info_t ri)
{
	struct idpf_vport *vport = idpf_softc_to_vport(arg);
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_queue *rxq = idpf_rxq(rsrc, ri->iri_qsidx);

	ri->iri_csum_flags = 0;
	ri->iri_csum_data = 0;
	ri->iri_vtag = 0;
	ri->iri_flowid = 0;
	ri->iri_rsstype = M_HASHTYPE_NONE;

	if (idpf_is_queue_model_split(rsrc->rxq_model))
		return (idpf_rx_splitq_pkt_get(rxq, ri));

	return (idpf_rx_singleq_pkt_get(rxq, ri));
}

static void
idpf_isc_rxd_refill(void *arg, if_rxd_update_t iru)
{
	struct idpf_vport *vport = idpf_softc_to_vport(arg);
	struct idpf_q_vec_rsrc *rsrc;
	struct idpf_queue *q;

	/*
	 * ifdi_init() returns void, so iflib refills even when the vport
	 * failed to open.  Refuse rather than write through a ring that was
	 * never handed to the driver.
	 */
	if (vport == NULL)
		return;

	rsrc = &vport->dflt_qv_rsrc;
	if (rsrc->rxq_grps == NULL)
		return;

	if (!idpf_is_queue_model_split(rsrc->rxq_model)) {
		q = idpf_rxq(rsrc, iru->iru_qsidx);
		if (q == NULL || q->desc_ring == NULL)
			return;
		idpf_rx_singleq_refill(q, iru);
		return;
	}

	MPASS(iru->iru_flidx < rsrc->num_bufqs_per_qgrp);
	q = idpf_bufq(rsrc, iru->iru_qsidx, iru->iru_flidx);
	if (q == NULL || q->desc_ring == NULL)
		return;

	idpf_rx_splitq_refill(q, iru);
}

/**
 * idpf_isc_rxd_flush - publish refilled buffers to hardware
 * @arg: vport (the iflib softc)
 * @rxqid: RX queue set index
 * @flid: free list (buffer queue) index
 * @pidx: producer index to publish
 *
 * The split model requires the buffer-queue tail to advance in units of
 * IDPF_RX_BUF_POST_STRIDE descriptors, so the doorbell is rounded down and
 * skipped when rounding produces no progress.  [IDPF:A13-A14]
 */
static void
idpf_isc_rxd_flush(void *arg, u16 rxqid, u8 flid, qidx_t pidx)
{
	struct idpf_vport *vport = idpf_softc_to_vport(arg);
	struct idpf_q_vec_rsrc *rsrc;
	struct idpf_queue *q;
	u32 tail;

	/* Reached from the same unconditional iflib path as the refill. */
	if (vport == NULL)
		return;

	rsrc = &vport->dflt_qv_rsrc;
	if (rsrc->rxq_grps == NULL)
		return;

	if (!idpf_is_queue_model_split(rsrc->rxq_model)) {
		q = idpf_rxq(rsrc, rxqid);
		if (q == NULL || q->tail == NULL)
			return;
		idpf_rx_buf_hw_update(q, pidx);
		return;
	}

	q = idpf_bufq(rsrc, rxqid, flid);
	if (q == NULL || q->tail == NULL)
		return;
	tail = rounddown(pidx, IDPF_RX_BUF_POST_STRIDE);
	if (tail == q->next_to_use)
		return;

	idpf_rx_buf_hw_update(q, tail);
}

/**
 * @var idpf_txrx_ops
 * @brief iflib datapath operations for the IDPF VF-DPF driver
 *
 * Referenced by if_idpf.c through if_shared_ctx.isc_txrx.  [FBSD15:A30]
 */
struct if_txrx idpf_txrx_ops = {
	.ift_txd_encap		= idpf_isc_txd_encap,
	.ift_txd_flush		= idpf_isc_txd_flush,
	.ift_txd_credits_update	= idpf_isc_txd_credits_update,
	.ift_rxd_available	= idpf_isc_rxd_available,
	.ift_rxd_pkt_get	= idpf_isc_rxd_pkt_get,
	.ift_rxd_refill		= idpf_isc_rxd_refill,
	.ift_rxd_flush		= idpf_isc_rxd_flush,
	.ift_legacy_intr	= NULL,
};

/* ---------------------------------------------------------------------------
 * Interrupt vectors and ITR
 * ------------------------------------------------------------------------- */

/**
 * idpf_vport_intr_rel - release the per-vector queue arrays
 * @rsrc: queue and vector resources
 */
void
idpf_vport_intr_rel(struct idpf_q_vec_rsrc *rsrc)
{
	u16 v_idx;

	if (rsrc->q_vectors == NULL)
		return;

	for (v_idx = 0; v_idx < rsrc->num_q_vectors; v_idx++) {
		struct idpf_q_vector *q_vector = &rsrc->q_vectors[v_idx];

		/*
		 * iflib only frees the legacy interrupt itself; leaving these
		 * held makes pci_release_msi() fail and leaks the vectors.
		 */
		if (q_vector->vport != NULL && q_vector->vport->ctx != NULL)
			iflib_irq_free(q_vector->vport->ctx,
			    &q_vector->que_irq);

		free(q_vector->bufq, M_DEVBUF);
		q_vector->bufq = NULL;
		free(q_vector->tx, M_DEVBUF);
		q_vector->tx = NULL;
		free(q_vector->rx, M_DEVBUF);
		q_vector->rx = NULL;
	}

	free(rsrc->q_vectors, M_DEVBUF);
	rsrc->q_vectors = NULL;
}

/**
 * idpf_vport_intr_alloc - allocate per-vector queue arrays
 * @vport: vport being brought up
 * @rsrc: queue and vector resources
 *
 * Returns 0 or ENOMEM.  [IDPF:A13-A14] [FBSD15:A34]
 */
int
idpf_vport_intr_alloc(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_vport_user_config_data *user_config;
	u16 txqs_per_vector, rxqs_per_vector, bufqs_per_vector;
	u16 num_txq_vec_need;
	u16 v_idx;

	if (rsrc->num_q_vectors == 0)
		return (EINVAL);

	/* Runs from both attach and open; the first caller wins. */
	if (rsrc->q_vectors != NULL)
		return (0);

	user_config = &vport->adapter->vport_config[vport->idx]->user_config;
	if (user_config->q_coalesce == NULL)
		return (EINVAL);

	rsrc->q_vectors = malloc(rsrc->num_q_vectors * sizeof(*rsrc->q_vectors),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (rsrc->q_vectors == NULL)
		return (ENOMEM);

	num_txq_vec_need = idpf_is_queue_model_split(rsrc->txq_model) ?
	    rsrc->num_complq : rsrc->num_txq;

	txqs_per_vector = howmany(num_txq_vec_need, rsrc->num_q_vectors);
	rxqs_per_vector = howmany(rsrc->num_rxq, rsrc->num_q_vectors);
	bufqs_per_vector = rsrc->num_bufqs_per_qgrp * rxqs_per_vector;

	for (v_idx = 0; v_idx < rsrc->num_q_vectors; v_idx++) {
		struct idpf_q_vector *q_vector = &rsrc->q_vectors[v_idx];
		struct idpf_q_coalesce *q_coal =
		    &user_config->q_coalesce[v_idx];

		q_vector->vport = vport;
		q_vector->v_idx = v_idx;
		q_vector->tx_itr_value = q_coal->tx_coalesce_usecs;
		q_vector->tx_intr_mode = q_coal->tx_intr_mode;
		q_vector->tx_itr_idx = VIRTCHNL2_ITR_IDX_1;
		q_vector->rx_itr_value = q_coal->rx_coalesce_usecs;
		q_vector->rx_intr_mode = q_coal->rx_intr_mode;
		q_vector->rx_itr_idx = VIRTCHNL2_ITR_IDX_0;

		q_vector->tx = malloc(txqs_per_vector * sizeof(*q_vector->tx),
		    M_DEVBUF, M_NOWAIT | M_ZERO);
		if (q_vector->tx == NULL)
			goto err_alloc;

		q_vector->rx = malloc(rxqs_per_vector * sizeof(*q_vector->rx),
		    M_DEVBUF, M_NOWAIT | M_ZERO);
		if (q_vector->rx == NULL)
			goto err_alloc;

		if (!idpf_is_queue_model_split(rsrc->rxq_model))
			continue;

		q_vector->bufq = malloc(bufqs_per_vector *
		    sizeof(*q_vector->bufq), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (q_vector->bufq == NULL)
			goto err_alloc;
	}

	return (0);

err_alloc:
	idpf_vport_intr_rel(rsrc);
	return (ENOMEM);
}

/**
 * idpf_vport_intr_map_vector_to_qs - distribute queues across vectors
 * @rsrc: queue and vector resources
 * [IDPF:A13-A14]
 */
static void
idpf_vport_intr_map_vector_to_qs(struct idpf_q_vec_rsrc *rsrc)
{
	bool rx_split = idpf_is_queue_model_split(rsrc->rxq_model);
	bool tx_split = idpf_is_queue_model_split(rsrc->txq_model);
	unsigned int i, j;
	u16 qv_idx = 0, bufq_vidx = 0;

	for (i = 0; i < rsrc->num_q_vectors; i++) {
		rsrc->q_vectors[i].num_rxq = 0;
		rsrc->q_vectors[i].num_txq = 0;
		rsrc->q_vectors[i].num_bufq = 0;
	}

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *rx_qgrp = &rsrc->rxq_grps[i];
		u16 num_rxq;

		num_rxq = rx_split ? rx_qgrp->splitq.num_rxq_sets :
		    rx_qgrp->singleq.num_rxq;

		for (j = 0; j < num_rxq; j++) {
			struct idpf_queue *q;

			if (qv_idx >= rsrc->num_q_vectors)
				qv_idx = 0;

			q = rx_split ? &rx_qgrp->splitq.rxq_sets[j]->rxq :
			    rx_qgrp->singleq.rxqs[j];
			q->q_vector = &rsrc->q_vectors[qv_idx];
			q->q_vector->rx[q->q_vector->num_rxq++] = q;
			qv_idx++;
		}

		if (!rx_split)
			continue;

		for (j = 0; j < rsrc->num_bufqs_per_qgrp; j++) {
			struct idpf_queue *bufq = &rx_qgrp->splitq.bufq_sets[j].bufq;

			bufq->q_vector = &rsrc->q_vectors[bufq_vidx];
			bufq->q_vector->bufq[bufq->q_vector->num_bufq++] = bufq;
		}

		if (++bufq_vidx >= rsrc->num_q_vectors)
			bufq_vidx = 0;
	}

	for (i = 0, qv_idx = 0; i < rsrc->num_txq_grp; i++) {
		struct idpf_txq_group *tx_qgrp = &rsrc->txq_grps[i];

		if (tx_split) {
			struct idpf_queue *q = tx_qgrp->complq;

			if (qv_idx >= rsrc->num_q_vectors)
				qv_idx = 0;

			q->q_vector = &rsrc->q_vectors[qv_idx];
			q->q_vector->tx[q->q_vector->num_txq++] = q;
			qv_idx++;
			continue;
		}

		for (j = 0; j < tx_qgrp->num_txq; j++) {
			struct idpf_queue *q = tx_qgrp->txqs[j];

			if (qv_idx >= rsrc->num_q_vectors)
				qv_idx = 0;

			q->q_vector = &rsrc->q_vectors[qv_idx];
			q->q_vector->tx[q->q_vector->num_txq++] = q;
			qv_idx++;
		}
	}
}

/**
 * idpf_vport_intr_buildreg_itr - build the DYN_CTL value that re-arms a vector
 * @q_vector: vector being re-armed
 *
 * Leaving PBA untouched avoids dropping interrupts that arrived while the
 * queue was being polled.  [IDPF:A13-A14]
 */
static u32
idpf_vport_intr_buildreg_itr(struct idpf_q_vector *q_vector)
{
	u32 itr_val = q_vector->intr_reg.dyn_ctl_intena_m;
	u32 type = IDPF_NO_ITR_UPDATE_IDX;
	u16 itr = 0;

	if (q_vector->wb_on_itr) {
		/* Trigger a software interrupt when leaving write-back-on-ITR. */
		itr_val |= q_vector->intr_reg.dyn_ctl_swint_trig_m |
		    q_vector->intr_reg.dyn_ctl_sw_itridx_ena_m;
		type = IDPF_SW_ITR_UPDATE_IDX;
		itr = IDPF_ITR_20K;
	}

	itr &= IDPF_ITR_MASK;
	itr_val |= (type << q_vector->intr_reg.dyn_ctl_itridx_s) |
	    ((u32)itr << (q_vector->intr_reg.dyn_ctl_intrvl_s - 1));

	return (itr_val);
}

/**
 * idpf_vport_intr_update_itr_ena_irq - re-arm a vector's interrupt
 * @q_vector: vector to re-arm
 */
void
idpf_vport_intr_update_itr_ena_irq(struct idpf_q_vector *q_vector)
{
	u32 intval;

	intval = idpf_vport_intr_buildreg_itr(q_vector);
	q_vector->wb_on_itr = false;

	if (q_vector->intr_reg.dyn_ctl != NULL)
		idpf_reg_wr32(q_vector->intr_reg.dyn_ctl, intval);
}

/**
 * idpf_vport_intr_set_wb_on_itr - keep completions flowing without interrupts
 * @q_vector: vector to place in write-back-on-ITR mode
 */
void
idpf_vport_intr_set_wb_on_itr(struct idpf_q_vector *q_vector)
{

	if (q_vector->wb_on_itr)
		return;

	q_vector->wb_on_itr = true;

	if (q_vector->intr_reg.dyn_ctl == NULL)
		return;

	idpf_reg_wr32(q_vector->intr_reg.dyn_ctl,
	    q_vector->intr_reg.dyn_ctl_wb_on_itr_m |
	    (IDPF_NO_ITR_UPDATE_IDX << q_vector->intr_reg.dyn_ctl_itridx_s) |
	    q_vector->intr_reg.dyn_ctl_intena_msk_m);
}

/**
 * idpf_vport_intr_write_itr - program an ITR interval for a vector
 * @q_vector: vector to program
 * @itr: interval in microseconds
 * @tx: true for the TX ITR register, false for RX
 *
 * Declared in idpf.h because ethtool-equivalent sysctl handlers use it.
 * [IDPF:A13-A14]
 */
void
idpf_vport_intr_write_itr(struct idpf_q_vector *q_vector, u16 itr,
    bool tx)
{
	struct idpf_intr_reg *intr_reg;
	void *reg;

	if (tx && q_vector->tx == NULL)
		return;
	if (!tx && q_vector->rx == NULL)
		return;

	intr_reg = &q_vector->intr_reg;
	reg = tx ? intr_reg->tx_itr : intr_reg->rx_itr;
	if (reg == NULL)
		return;

	idpf_reg_wr32(reg, ITR_REG_ALIGN(itr) >> IDPF_ITR_GRAN_S);
}

static void
idpf_vport_intr_dis_irq_all(struct idpf_q_vec_rsrc *rsrc)
{
	u16 q_idx;

	if (rsrc->q_vectors == NULL)
		return;

	for (q_idx = 0; q_idx < rsrc->num_q_vectors; q_idx++) {
		struct idpf_q_vector *qv = &rsrc->q_vectors[q_idx];

		if (qv->intr_reg.dyn_ctl != NULL)
			idpf_reg_wr32(qv->intr_reg.dyn_ctl, 0);
	}
}

/**
 * idpf_vport_intr_ena - program ITR values and arm every vector
 * @vport: vport being brought up
 * @rsrc: queue and vector resources
 */
void
idpf_vport_intr_ena(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{
	u16 q_idx;

	if (rsrc->q_vectors == NULL)
		return;

	for (q_idx = 0; q_idx < rsrc->num_q_vectors; q_idx++) {
		struct idpf_q_vector *qv = &rsrc->q_vectors[q_idx];

		if (qv->num_txq != 0)
			idpf_vport_intr_write_itr(qv, qv->tx_itr_value, true);
		if (qv->num_rxq != 0)
			idpf_vport_intr_write_itr(qv, qv->rx_itr_value, false);
		if (qv->num_txq != 0 || qv->num_rxq != 0)
			idpf_vport_intr_update_itr_ena_irq(qv);
	}
}

/**
 * idpf_vport_intr_init - map queues to vectors and initialise their registers
 * @vport: vport being brought up
 * @rsrc: queue and vector resources
 *
 * Returns 0 or an errno.  [IDPF:A13-A14] [FBSD15:A34]
 */
int
idpf_vport_intr_init(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{
	struct idpf_adapter *adapter = vport->adapter;
	u16 i;
	int err;

	if (rsrc->q_vectors == NULL || rsrc->q_vector_idxs == NULL)
		return (EINVAL);

	for (i = 0; i < rsrc->num_q_vectors; i++) {
		if (rsrc->q_vector_idxs[i] >= adapter->num_msix_entries)
			return (EINVAL);
		/* The control plane addresses vectors by absolute id. */
		rsrc->q_vectors[i].v_idx =
		    adapter->vector_ids[rsrc->q_vector_idxs[i]];
	}

	idpf_vport_intr_map_vector_to_qs(rsrc);

	if (adapter->dev_ops.reg_ops.intr_reg_init == NULL)
		return (EINVAL);

	err = adapter->dev_ops.reg_ops.intr_reg_init(vport, rsrc);
	if (err != 0)
		return (err);

	return (0);
}

/**
 * idpf_vport_intr_deinit - quiesce every vector
 * @vport: vport being torn down
 * @rsrc: queue and vector resources
 */
void
idpf_vport_intr_deinit(struct idpf_vport *vport, struct idpf_q_vec_rsrc *rsrc)
{

	idpf_vport_intr_dis_irq_all(rsrc);
}

/**
 * idpf_tx_queue_intr_enable - ifdi_tx_queue_intr_enable() implementation
 * @ctx: iflib context
 * @txqid: TX queue set index
 *
 * Returns 0.  [FBSD15:A34]
 */
int
idpf_tx_queue_intr_enable(if_ctx_t ctx, u16 txqid)
{
	struct idpf_vport *vport = idpf_softc_to_vport(iflib_get_softc(ctx));
	struct idpf_queue *txq;

	/* iflib keeps driving its init sequence after the driver refuses it. */
	if (vport == NULL || vport->dflt_qv_rsrc.txq_grps == NULL)
		return (ENXIO);

	txq = idpf_txq(vport, txqid);
	if (txq != NULL && txq->q_vector != NULL)
		idpf_vport_intr_update_itr_ena_irq(txq->q_vector);

	return (0);
}

/**
 * idpf_rx_queue_intr_enable - ifdi_rx_queue_intr_enable() implementation
 * @ctx: iflib context
 * @rxqid: RX queue set index
 *
 * Returns 0.  [FBSD15:A34]
 */
int
idpf_rx_queue_intr_enable(if_ctx_t ctx, u16 rxqid)
{
	struct idpf_vport *vport = idpf_softc_to_vport(iflib_get_softc(ctx));
	struct idpf_queue *rxq;

	if (vport == NULL || vport->dflt_qv_rsrc.rxq_grps == NULL)
		return (ENXIO);

	rxq = idpf_rxq(&vport->dflt_qv_rsrc, rxqid);
	if (rxq != NULL && rxq->q_vector != NULL)
		idpf_vport_intr_update_itr_ena_irq(rxq->q_vector);

	return (0);
}

/**
 * idpf_intr_enable - ifdi_intr_enable() implementation
 * @ctx: iflib context
 */
void
idpf_intr_enable(if_ctx_t ctx)
{
	struct idpf_vport *vport = idpf_softc_to_vport(iflib_get_softc(ctx));

	if (vport == NULL)
		return;

	idpf_vport_intr_ena(vport, &vport->dflt_qv_rsrc);
}

/**
 * idpf_intr_disable - ifdi_intr_disable() implementation
 * @ctx: iflib context
 */
void
idpf_intr_disable(if_ctx_t ctx)
{
	struct idpf_vport *vport = idpf_softc_to_vport(iflib_get_softc(ctx));

	if (vport == NULL)
		return;

	idpf_vport_intr_dis_irq_all(&vport->dflt_qv_rsrc);
}

/* ---------------------------------------------------------------------------
 * RSS
 * ------------------------------------------------------------------------- */

/**
 * idpf_config_rss - push the current RSS key and LUT to the control plane
 * @vport: vport to configure
 * @rss_data: RSS key and LUT
 *
 * Returns 0 or an errno.  [IDPF:A13-A14]
 */
int
idpf_config_rss(struct idpf_vport *vport, struct idpf_rss_data *rss_data)
{
	struct idpf_adapter *adapter = vport->adapter;
	int err;

	err = idpf_send_get_set_rss_key_msg(adapter, rss_data, vport->vport_id,
	    false);
	if (err != 0)
		return (err);

	return (idpf_send_get_set_rss_lut_msg(adapter, rss_data,
	    vport->vport_id, false));
}

/**
 * idpf_fill_dflt_rss_lut - populate a round-robin redirection table
 * @rss_data: RSS key and LUT
 * @rsrc: queue and vector resources
 */
static void
idpf_fill_dflt_rss_lut(struct idpf_rss_data *rss_data,
    struct idpf_q_vec_rsrc *rsrc)
{
	u16 num_active_rxq = rsrc->num_rxq;
	u16 i;

	for (i = 0; i < rss_data->rss_lut_size; i++) {
		rss_data->rss_lut[i] = i % num_active_rxq;
		rss_data->cached_lut[i] = rss_data->rss_lut[i];
	}
}

/**
 * idpf_init_rss - allocate and program the default RSS configuration
 * @vport: vport to configure
 * @rss_data: RSS key and LUT
 * @rsrc: queue and vector resources
 *
 * Returns 0 or an errno.  [IDPF:A13-A14]
 */
int
idpf_init_rss(struct idpf_vport *vport, struct idpf_rss_data *rss_data,
    struct idpf_q_vec_rsrc *rsrc)
{
	u32 lut_size;
	int err;

	if (rss_data->rss_lut_size == 0 || rsrc->num_rxq == 0) {
		device_printf(idpf_adapter_to_dev(vport->adapter),
		    "RSS unusable: lut_size %u, num_rxq %u\n",
		    rss_data->rss_lut_size, rsrc->num_rxq);
		return (EINVAL);
	}

	lut_size = rss_data->rss_lut_size * sizeof(*rss_data->rss_lut);

	/* Runs again on every open, but the tables are allocated only once. */
	if (rss_data->rss_lut == NULL) {
		rss_data->rss_lut = malloc(lut_size, M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (rss_data->rss_lut == NULL)
			return (ENOMEM);
	}

	if (rss_data->cached_lut == NULL) {
		rss_data->cached_lut = malloc(lut_size, M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (rss_data->cached_lut == NULL) {
			free(rss_data->rss_lut, M_DEVBUF);
			rss_data->rss_lut = NULL;
			return (ENOMEM);
		}
	}

	idpf_fill_dflt_rss_lut(rss_data, rsrc);

	err = idpf_config_rss(vport, rss_data);
	if (err != 0)
		idpf_deinit_rss(rss_data);

	return (err);
}

/**
 * idpf_deinit_rss - release the RSS redirection tables
 * @rss_data: RSS key and LUT
 */
void
idpf_deinit_rss(struct idpf_rss_data *rss_data)
{

	free(rss_data->cached_lut, M_DEVBUF);
	rss_data->cached_lut = NULL;
	free(rss_data->rss_lut, M_DEVBUF);
	rss_data->rss_lut = NULL;
}
