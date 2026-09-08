/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * idpf_singleq_txrx.c - FreeBSD >= 15.0 IDPF VF-DPF single queue datapath.
 *
 * The single queue model uses one ring per direction.  A TX ring carries base
 * (DTYPE 0) data descriptors and is written back in place with DTYPE
 * DESC_DONE on every descriptor that carried the RS bit.  An RX ring is both
 * the buffer-post ring and the writeback ring: the driver posts a buffer
 * address into a descriptor and hardware overwrites that descriptor with the
 * completion.  There is no completion queue and no generation bit in either
 * direction, so both are strictly in order - which is exactly iflib's model.
 *
 * Two RX writeback formats are supported, selected by the descriptor profile
 * negotiated in virtchnl2_rxq_info.desc_ids and recorded in idpf_queue.rxdids:
 *
 *   VIRTCHNL2_RXDID_1_32B_BASE_M    legacy 32-byte base writeback
 *   VIRTCHNL2_RXDID_2_FLEX_SQ_NIC_M flexible NIC writeback
 *
 * The Linux driver's page-recycling allocator, skb assembly and NAPI loop are
 * not ported: iflib owns RX buffers, mbuf assembly and the poll loop.  This
 * file only decodes descriptors and reports indices back to iflib.
 *
 * Entry points are declared in idpf_txrx.h and are called from the iflib
 * callbacks in idpf_txrx.c once the queue model has been resolved.
 *
 * Evidence classes per the Human Reference Guide Appendix A:
 *   [FBSD15:A30-A34] FreeBSD 15 kernel / iflib interfaces are authoritative.
 *   [IDPF:A13-A17]   Virtchnl2 / IDPF 1.0 protocol behaviour is authoritative.
 *   [LOCAL:A25]      Shape of the ported idpf_txrx.h is preserved.
 */

#include "idpf.h"
#include "idpf_lan_txrx.h"

#include <netinet/in.h>
#include <netinet/sctp.h>
#include <netinet/udp.h>

/*
 * FLTSTAT value that marks the RSS hash in a base writeback descriptor as
 * valid.  The field is two bits wide; every other value selects a different
 * filter status.  [IDPF:A13-A14]
 */
#define	IDPF_RX_BASE_FLTSTAT_RSS_VALID	3

/**
 * struct idpf_rx_singleq_fields - descriptor fields shared by both formats
 * @csum: decoded checksum status and error bits
 * @hash: RSS hash value, valid only when @hash_valid
 * @len: payload length in this descriptor
 * @ptype: device-authored packet type identifier
 * @vtag: stripped VLAN tag, valid only when @vlan_valid
 * @dd: descriptor done - hardware has written this descriptor back
 * @eop: end of packet
 * @rxe: receive error reported by hardware
 * @hash_valid: @hash carries a usable RSS hash
 * @vlan_valid: @vtag carries a stripped VLAN tag
 */
struct idpf_rx_singleq_fields {
	struct idpf_rx_csum_decoded	csum;
	u32			hash;
	u16			len;
	u16			ptype;
	u16			vtag;
	bool				dd;
	bool				eop;
	bool				rxe;
	bool				hash_valid;
	bool				vlan_valid;
};

/* ---------------------------------------------------------------------------
 * TX datapath
 * ------------------------------------------------------------------------- */

/**
 * idpf_tx_singleq_offload - derive the command and offset descriptor words
 * @pi: iflib packet description
 * @cmd: command bits, updated
 * @off: header offset bits, updated
 *
 * Base descriptors carry explicit header lengths rather than relying on the
 * device to reparse the packet, and iflib has already extracted all of them
 * from the mbuf.  [IDPF:A13-A14] [FBSD15:A30]
 */
static void
idpf_tx_singleq_offload(if_pkt_info_t pi, u32 *cmd, u32 *off)
{

	switch (pi->ipi_etype) {
	case ETHERTYPE_IP:
		/*
		 * The device computes the IPv4 header checksum only when the
		 * IIPT field asks for it; TSO always needs it because each
		 * segment gets a fresh header.
		 */
		*cmd |= (pi->ipi_csum_flags & (CSUM_IP | CSUM_IP_TSO)) != 0 ?
		    IDPF_TX_DESC_CMD_IIPT_IPV4_CSUM :
		    IDPF_TX_DESC_CMD_IIPT_IPV4;
		break;
	case ETHERTYPE_IPV6:
		*cmd |= IDPF_TX_DESC_CMD_IIPT_IPV6;
		break;
	default:
		return;
	}

	*off |= ((u32)pi->ipi_ehdrlen >> 1) << IDPF_TX_DESC_LEN_MACLEN_S;
	*off |= ((u32)pi->ipi_ip_hlen >> 2) << IDPF_TX_DESC_LEN_IPLEN_S;

	switch (pi->ipi_ipproto) {
	case IPPROTO_TCP:
		if ((pi->ipi_csum_flags & (CSUM_TCP | CSUM_IP6_TCP |
		    CSUM_IP_TSO | CSUM_IP6_TSO)) == 0)
			break;
		*cmd |= IDPF_TX_DESC_CMD_L4T_EOFT_TCP;
		*off |= ((u32)pi->ipi_tcp_hlen >> 2) <<
		    IDPF_TX_DESC_LEN_L4_LEN_S;
		break;
	case IPPROTO_UDP:
		if ((pi->ipi_csum_flags & (CSUM_UDP | CSUM_IP6_UDP)) == 0)
			break;
		*cmd |= IDPF_TX_DESC_CMD_L4T_EOFT_UDP;
		*off |= ((u32)(sizeof(struct udphdr) >> 2)) <<
		    IDPF_TX_DESC_LEN_L4_LEN_S;
		break;
	case IPPROTO_SCTP:
		if ((pi->ipi_csum_flags & (CSUM_SCTP | CSUM_IP6_SCTP)) == 0)
			break;
		*cmd |= IDPF_TX_DESC_CMD_L4T_EOFT_SCTP;
		*off |= ((u32)(sizeof(struct sctphdr) >> 2)) <<
		    IDPF_TX_DESC_LEN_L4_LEN_S;
		break;
	default:
		break;
	}
}

/**
 * idpf_tx_singleq_tso_setup - write a base TSO context descriptor
 * @txq: queue being filled
 * @pi: iflib packet description
 * @idx: descriptor index to write
 *
 * Returns the next descriptor index.  [IDPF:A13-A14]
 */
static u16
idpf_tx_singleq_tso_setup(struct idpf_queue *txq, if_pkt_info_t pi,
    u16 idx)
{
	struct idpf_base_tx_ctx_desc *ctx;
	u32 hdr_len, tso_len;
	u16 mss;
	u64 qw1;

	ctx = IDPF_BASE_TX_CTX_DESC(txq, idx);

	hdr_len = pi->ipi_ehdrlen + pi->ipi_ip_hlen + pi->ipi_tcp_hlen;
	tso_len = pi->ipi_len - hdr_len;

	mss = pi->ipi_tso_segsz;
	if (mss < IDPF_TX_TSO_MIN_MSS)
		mss = IDPF_TX_TSO_MIN_MSS;

	qw1 = IDPF_FIELD_PREP(IDPF_TXD_CTX_QW1_DTYPE_M,
	    IDPF_TX_DESC_DTYPE_CTX) |
	    IDPF_FIELD_PREP(IDPF_TXD_CTX_QW1_CMD_M, IDPF_TX_CTX_DESC_TSO) |
	    IDPF_FIELD_PREP(IDPF_TXD_CTX_QW1_TSO_LEN_M, tso_len) |
	    IDPF_FIELD_PREP(IDPF_TXD_CTX_QW1_MSS_M, mss);

	ctx->qw0.tunneling_params = htole32(0);
	ctx->qw0.l2tag2 = htole16(0);
	ctx->qw0.rsvd1 = htole16(0);
	ctx->qw1 = htole64(qw1);

	return (idpf_ring_next(idx, txq->desc_count));
}

/**
 * idpf_tx_singleq_rs_push - record the last descriptor of a packet
 * @txq: queue being filled
 * @pidx_last: index of the descriptor carrying the RS bit
 *
 * With no completion queue, reclaim has to know where each packet ended.
 * tx.bufs[] is sized to desc_count and its priv field - unused under iflib,
 * which owns mbuf lifetime - holds that index; next_to_alloc and
 * next_to_clean are the producer and consumer of this in-order ring.
 * iflib never allows more packets in flight than there are descriptors, so
 * the ring cannot overrun.  [LOCAL:A25]
 */
static void
idpf_tx_singleq_rs_push(struct idpf_queue *txq, u16 pidx_last)
{

	txq->tx.bufs[txq->next_to_alloc].priv = pidx_last;
	txq->next_to_alloc = idpf_ring_next(txq->next_to_alloc,
	    txq->desc_count);
}

/**
 * idpf_tx_singleq_encap - translate an iflib packet into base TX descriptors
 * @txq: queue being filled
 * @pi: packet description carrying an already mapped scatter list
 *
 * Length and segment count have already been validated by the caller.
 * Returns 0.  [FBSD15:A30-A31] [IDPF:A13-A14]
 */
int
idpf_tx_singleq_encap(struct idpf_queue *txq, if_pkt_info_t pi)
{
	bus_dma_segment_t *segs = pi->ipi_segs;
	u64 td_tag = pi->ipi_vtag;
	u32 cmd = 0, off = 0;
	u16 i = pi->ipi_pidx;
	u16 pidx_last = i;
	int j, nsegs = pi->ipi_nsegs;

	if ((pi->ipi_csum_flags & CSUM_TSO) != 0) {
		i = idpf_tx_singleq_tso_setup(txq, pi, i);
		txq->q_stats.tx.lso_pkts++;
		txq->q_stats.tx.lso_bytes += pi->ipi_len;
		txq->q_stats.tx.lso_segs_tot += howmany(pi->ipi_len,
		    pi->ipi_tso_segsz);
	}

	if ((pi->ipi_csum_flags & IDPF_CSUM_OFFLOAD) != 0)
		idpf_tx_singleq_offload(pi, &cmd, &off);
	if ((pi->ipi_mflags & M_VLANTAG) != 0)
		cmd |= IDPF_TX_DESC_CMD_IL2TAG1;
	if (txq->crc_enable)
		cmd |= IDPF_TX_DESC_CMD_ICRC;

	for (j = 0; j < nsegs; j++) {
		struct idpf_base_tx_desc *desc;
		bus_size_t len = segs[j].ds_len;
		bus_addr_t addr = segs[j].ds_addr;

		/*
		 * A segment longer than the descriptor length field is split
		 * on a 4 KiB-aligned boundary, which is the device's maximum
		 * read request granularity.
		 */
		while (len > IDPF_TX_MAX_DESC_DATA) {
			desc = IDPF_BASE_TX_DESC(txq, i);
			desc->buf_addr = htole64(addr);
			desc->qw1 = idpf_tx_singleq_build_ctob(cmd, off,
			    IDPF_TX_MAX_DESC_DATA_ALIGNED, td_tag);
			pidx_last = i;
			i = idpf_ring_next(i, txq->desc_count);
			addr += IDPF_TX_MAX_DESC_DATA_ALIGNED;
			len -= IDPF_TX_MAX_DESC_DATA_ALIGNED;
		}

		desc = IDPF_BASE_TX_DESC(txq, i);
		desc->buf_addr = htole64(addr);
		if (j == nsegs - 1)
			cmd |= IDPF_TXD_LAST_DESC_CMD;
		desc->qw1 = idpf_tx_singleq_build_ctob(cmd, off,
		    (unsigned int)len, td_tag);
		pidx_last = i;
		i = idpf_ring_next(i, txq->desc_count);
	}

	idpf_tx_singleq_rs_push(txq, pidx_last);
	pi->ipi_new_pidx = i;

	return (0);
}

/**
 * idpf_tx_singleq_credits - reclaim completed base descriptors
 * @txq: queue being reclaimed
 * @clear: commit the scan when true, report only when false
 *
 * Hardware writes DTYPE = DESC_DONE back into every descriptor that carried
 * the RS bit, so reclaim walks the RS ring built by idpf_tx_singleq_rs_push()
 * and credits iflib with the distance covered by each completed packet.
 * Returns the number of reclaimable descriptors.  [IDPF:A13-A14]
 */
int
idpf_tx_singleq_credits(struct idpf_queue *txq, bool clear)
{
	u16 rs_cidx = txq->next_to_clean;
	u16 rs_pidx = txq->next_to_alloc;
	u16 prev = (u16)txq->tx.num_completions;
	int credits = 0;

	while (rs_cidx != rs_pidx) {
		const struct idpf_base_tx_desc *desc;
		u16 cur = (u16)txq->tx.bufs[rs_cidx].priv;

		if (__predict_false(cur >= txq->desc_count))
			break;

		desc = IDPF_BASE_TX_DESC(txq, cur);
		atomic_thread_fence_acq();

		if (IDPF_FIELD_GET(IDPF_TXD_QW1_DTYPE_M, le64toh(desc->qw1)) !=
		    IDPF_TX_DESC_DTYPE_DESC_DONE)
			break;

		credits += idpf_ring_delta(prev, cur, txq->desc_count);
		prev = cur;
		rs_cidx = idpf_ring_next(rs_cidx, txq->desc_count);
	}

	if (clear && credits != 0) {
		txq->next_to_clean = rs_cidx;
		txq->tx.num_completions = prev;
	}

	return (credits);
}

/* ---------------------------------------------------------------------------
 * RX descriptor decode
 * ------------------------------------------------------------------------- */

/**
 * idpf_rx_singleq_extract_base - decode a base (32 byte) writeback descriptor
 * @rx_desc: descriptor to decode
 * @fields: storage for the extracted values
 *
 * Operates on the VIRTCHNL2_RXDID_1_32B_BASE_M writeback format.
 * [IDPF:A13-A14]
 */
static void
idpf_rx_singleq_extract_base(const union virtchnl2_rx_desc *rx_desc,
    struct idpf_rx_singleq_fields *fields)
{
	u32 status, error;
	u64 qword1;

	qword1 = le64toh(rx_desc->base_wb.qword1.status_error_ptype_len);
	status = IDPF_FIELD_GET(VIRTCHNL2_RX_BASE_DESC_QW1_STATUS_M, qword1);
	error = IDPF_FIELD_GET(VIRTCHNL2_RX_BASE_DESC_QW1_ERROR_M, qword1);

	fields->dd = (status & VIRTCHNL2_RX_BASE_DESC_STATUS_DD_M) != 0;
	fields->eop = (status & VIRTCHNL2_RX_BASE_DESC_STATUS_EOF_M) != 0;
	fields->rxe = (error & VIRTCHNL2_RX_BASE_DESC_ERROR_RXE_M) != 0;

	fields->len = IDPF_FIELD_GET(VIRTCHNL2_RX_BASE_DESC_QW1_LEN_PBUF_M,
	    qword1);
	fields->ptype = IDPF_FIELD_GET(VIRTCHNL2_RX_BASE_DESC_QW1_PTYPE_M,
	    qword1);

	fields->vlan_valid =
	    (status & VIRTCHNL2_RX_BASE_DESC_STATUS_L2TAG1P_M) != 0;
	fields->vtag = le16toh(rx_desc->base_wb.qword0.lo_dword.l2tag1);

	fields->hash_valid =
	    IDPF_FIELD_GET(VIRTCHNL2_RX_BASE_DESC_STATUS_FLTSTAT_M, status) ==
	    IDPF_RX_BASE_FLTSTAT_RSS_VALID;
	fields->hash = le32toh(rx_desc->base_wb.qword0.hi_dword.rss);

	fields->csum.l3l4p =
	    (status & VIRTCHNL2_RX_BASE_DESC_STATUS_L3L4P_M) != 0;
	fields->csum.ipv6exadd =
	    (status & VIRTCHNL2_RX_BASE_DESC_STATUS_IPV6EXADD_M) != 0;
	fields->csum.ipe = (error & VIRTCHNL2_RX_BASE_DESC_ERROR_IPE_M) != 0;
	fields->csum.eipe = (error & VIRTCHNL2_RX_BASE_DESC_ERROR_EIPE_M) != 0;
	fields->csum.l4e = (error & VIRTCHNL2_RX_BASE_DESC_ERROR_L4E_M) != 0;
	fields->csum.pprs = (error & VIRTCHNL2_RX_BASE_DESC_ERROR_PPRS_M) != 0;
	fields->csum.nat = 0;
	fields->csum.eudpe = 0;
}

/**
 * idpf_rx_singleq_extract_flex - decode a flexible NIC writeback descriptor
 * @rx_desc: descriptor to decode
 * @fields: storage for the extracted values
 *
 * Operates on the VIRTCHNL2_RXDID_2_FLEX_SQ_NIC writeback format.
 * [IDPF:A13-A14]
 */
static void
idpf_rx_singleq_extract_flex(const union virtchnl2_rx_desc *rx_desc,
    struct idpf_rx_singleq_fields *fields)
{
	u16 status0, status1;

	status0 = le16toh(rx_desc->flex_nic_wb.status_error0);
	status1 = le16toh(rx_desc->flex_nic_wb.status_error1);

	fields->dd = (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_DD_M) != 0;
	fields->eop = (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_EOF_M) != 0;
	fields->rxe = (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_RXE_M) != 0;

	fields->len = IDPF_FIELD_GET(VIRTCHNL2_RX_FLEX_DESC_PKT_LEN_M,
	    le16toh(rx_desc->flex_nic_wb.pkt_len));
	fields->ptype = IDPF_FIELD_GET(VIRTCHNL2_RX_FLEX_DESC_PTYPE_M,
	    le16toh(rx_desc->flex_nic_wb.ptype_flex_flags0));

	fields->vlan_valid =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_L2TAG1P_M) != 0;
	fields->vtag = le16toh(rx_desc->flex_nic_wb.l2tag1);

	fields->hash_valid =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_RSS_VALID_M) != 0;
	fields->hash = le32toh(rx_desc->flex_nic_wb.rss_hash);

	fields->csum.l3l4p =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_L3L4P_M) != 0;
	fields->csum.ipv6exadd =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_IPV6EXADD_M) != 0;
	fields->csum.ipe =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_XSUM_IPE_M) != 0;
	fields->csum.eipe =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_XSUM_EIPE_M) != 0;
	fields->csum.l4e =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_XSUM_L4E_M) != 0;
	fields->csum.eudpe =
	    (status0 & VIRTCHNL2_RX_FLEX_DESC_STATUS0_XSUM_EUDPE_M) != 0;
	fields->csum.nat = (status1 & VIRTCHNL2_RX_FLEX_DESC_STATUS1_NAT_M) != 0;
	fields->csum.pprs = 0;
}

/**
 * idpf_rx_singleq_extract - decode one descriptor in the negotiated format
 * @rxq: RX queue the descriptor belongs to
 * @rx_desc: descriptor to decode
 * @fields: storage for the extracted values
 */
static void
idpf_rx_singleq_extract(const struct idpf_queue *rxq,
    const union virtchnl2_rx_desc *rx_desc,
    struct idpf_rx_singleq_fields *fields)
{

	memset(fields, 0, sizeof(*fields));

	/*
	 * The raw checksum is only carried by the advanced split-queue
	 * writeback, so neither single-queue format can supply one.
	 */
	fields->csum.raw_csum_inv = 1;

	if (rxq->rxdids == VIRTCHNL2_RXDID_1_32B_BASE_M)
		idpf_rx_singleq_extract_base(rx_desc, fields);
	else
		idpf_rx_singleq_extract_flex(rx_desc, fields);
}

/* ---------------------------------------------------------------------------
 * iflib RX entry points
 * ------------------------------------------------------------------------- */

/**
 * idpf_rx_singleq_available - count complete packets waiting on the ring
 * @rxq: RX queue
 * @idx: iflib consumer index to start from
 * @budget: maximum number of packets to report
 *
 * The scan is non-destructive.  Returns the number of packets whose final
 * descriptor has been written back.  [IDPF:A13-A14]
 */
int
idpf_rx_singleq_available(struct idpf_queue *rxq, qidx_t idx, qidx_t budget)
{
	u16 ntc = idx;
	int pkts = 0, descs = 0;

	while (pkts < budget && descs < rxq->desc_count) {
		struct idpf_rx_singleq_fields fields;

		atomic_thread_fence_acq();
		idpf_rx_singleq_extract(rxq, IDPF_RX_DESC(rxq, ntc), &fields);

		if (!fields.dd)
			break;

		if (fields.eop)
			pkts++;

		descs++;
		ntc = idpf_ring_next(ntc, rxq->desc_count);
	}

	return (pkts);
}

/**
 * idpf_rx_singleq_pkt_get - decode one packet for iflib
 * @rxq: RX queue
 * @ri: receive descriptor info to fill
 *
 * Walks the descriptors of one packet, records which free-list entries it
 * consumed, and decodes the metadata carried by its final descriptor.
 * Returns 0, or EBADMSG when the device produced a descriptor the driver
 * cannot honour - which iflib escalates to an interface reset.
 * [IDPF:A13-A17] [FBSD15:A30]
 */
int
idpf_rx_singleq_pkt_get(struct idpf_queue *rxq, if_rxd_info_t ri)
{
	const struct idpf_rx_ptype_decoded *decoded;
	struct idpf_rx_singleq_fields fields;
	u16 ntc = ri->iri_cidx;
	u32 total_len = 0;
	int nfrags = 0;

	for (;;) {
		atomic_thread_fence_acq();
		idpf_rx_singleq_extract(rxq, IDPF_RX_DESC(rxq, ntc), &fields);

		/*
		 * isc_rxd_available() promised this descriptor, so a missing
		 * DD bit means the ring state and the device disagree.
		 */
		if (__predict_false(!fields.dd)) {
			rxq->q_stats.rx.bad_descs++;
			return (EBADMSG);
		}

		if (__predict_false(fields.len > rxq->rx_max_pkt_size) ||
		    __predict_false(nfrags >= IFLIB_MAX_RX_SEGS)) {
			rxq->q_stats.rx.bad_descs++;
			return (EBADMSG);
		}

		ri->iri_frags[nfrags].irf_flid = 0;
		ri->iri_frags[nfrags].irf_idx = ntc;
		ri->iri_frags[nfrags].irf_len = fields.len;
		nfrags++;
		total_len += fields.len;

		ntc = idpf_ring_next(ntc, rxq->desc_count);

		if (fields.eop)
			break;
	}

	rxq->next_to_clean = ntc;
	ri->iri_nfrags = nfrags;
	ri->iri_len = (u16)total_len;

	if (__predict_false(fields.rxe)) {
		rxq->q_stats.rx.bad_descs++;
		return (EBADMSG);
	}

	decoded = idpf_rx_decode_ptype(rxq, fields.ptype);
	if (decoded != NULL && decoded->known) {
		if (fields.hash_valid) {
			ri->iri_flowid = fields.hash;
			ri->iri_rsstype = idpf_ptype_to_htype(decoded);
		} else {
			ri->iri_rsstype = M_HASHTYPE_OPAQUE;
		}
		idpf_rx_csum(rxq, ri, &fields.csum, decoded);
	} else {
		ri->iri_rsstype = M_HASHTYPE_OPAQUE;
	}

	if (fields.vlan_valid) {
		ri->iri_vtag = fields.vtag;
		ri->iri_flags |= M_VLANTAG;
	}

	rxq->q_stats.rx.packets++;
	rxq->q_stats.rx.bytes += total_len;

	return (0);
}

/**
 * idpf_rx_singleq_refill - post iflib buffers to the RX ring
 * @rxq: RX queue receiving the addresses
 * @iru: iflib refill request
 *
 * The same ring carries the posted buffer descriptor and its writeback, so
 * refilling overwrites the completion in place.  [FBSD15:A30-A31]
 * [IDPF:A13-A14]
 */
void
idpf_rx_singleq_refill(struct idpf_queue *rxq, if_rxd_update_t iru)
{
	struct virtchnl2_singleq_rx_buf_desc *desc;
	u32 pidx = iru->iru_pidx;
	u16 i;

	for (i = 0; i < iru->iru_count; i++) {
		MPASS(pidx < rxq->desc_count);

		desc = IDPF_SINGLEQ_RX_BUF_DESC(rxq, pidx);
		desc->pkt_addr = htole64(iru->iru_paddrs[i]);
		desc->hdr_addr = htole64(0);
		desc->rsvd1 = 0;
		desc->rsvd2 = 0;

		pidx = idpf_ring_next(pidx, rxq->desc_count);
	}

	rxq->next_to_alloc = pidx;
}
