/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Assignable Device Interface (ADI) support for Scalable IOV.
 *
 * See idpf_adi.h for what this port keeps and what it leaves behind.  In
 * short: the VFIO/mdev device model is gone, so the ops table, the sparse
 * mmap descriptions and the create/destroy paths that ride on the
 * NON_FLEX_CREATE_ADI virtchnl opcodes are not here.  What remains is the
 * ADI registry, the vector and queue bookkeeping, and the translation from a
 * guest-visible VF register offset to the owning PF register.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include "idpf.h"
#include "idpf_virtchnl.h"
#include "idpf_lan_pf_regs.h"
#include "idpf_lan_vf_regs.h"

/* 1 rx, 1 tx, 1 compl and 2 bufq per ADI queue slot */
#define IDPF_Q_ALLOC_CNT	(3 + 2)

/**
 * idpf_adi_hw_vector - hardware vector index behind a driver vector index
 * @adapter: driver private data
 * @vec_index: driver-relative vector index
 *
 * FreeBSD allocates MSI-X vectors as IRQ resources whose rid is the 1-based
 * vector number, so the hardware index the control plane expects is the rid
 * minus one.  Only the mailbox IRQ resource belongs to the driver -- iflib
 * owns the queue vectors -- so the index is derived rather than read back.
 *
 * Return: the hardware vector index, or -1 when the vector is out of range.
 */
static int
idpf_adi_hw_vector(struct idpf_adapter *adapter, uint16_t vec_index)
{

	if (vec_index >= adapter->num_msix_entries)
		return (-1);

	return (vec_index);
}

/**
 * idpf_adi_find_priv - look up an ADI by the identifier the CP assigned
 * @adapter: driver private data
 * @adi_id: ADI identifier
 *
 * Return: the ADI, or NULL when no ADI carries that identifier.
 */
static struct idpf_adi_priv *
idpf_adi_find_priv(struct idpf_adapter *adapter, uint16_t adi_id)
{
	struct idpf_adi_priv *priv;

	mtx_assert(&adapter->adi_info.priv_lock, MA_OWNED);

	TAILQ_FOREACH(priv, &adapter->adi_info.priv_list, list) {
		if (priv->adi_id == adi_id)
			return (priv);
	}

	return (NULL);
}

/**
 * idpf_notify_adi_reset - record a reset transition reported by the CP
 * @adapter: driver private data
 * @adi_id: ADI identifier
 * @reset: true when the reset has completed, false when it has begun
 *
 * The guest polls VFGEN_RSTAT, which idpf_adi_read_reg32() answers from the
 * state recorded here.
 */
void
idpf_notify_adi_reset(struct idpf_adapter *adapter, uint16_t adi_id,
    bool reset)
{
	struct idpf_adi_priv *priv;

	mtx_lock(&adapter->adi_info.priv_lock);

	priv = idpf_adi_find_priv(adapter, adi_id);
	if (priv != NULL)
		priv->reset_state = reset ? IDPF_ADI_RESET_COMPLETED :
		    IDPF_ADI_RESET_INPROGRESS;

	mtx_unlock(&adapter->adi_info.priv_lock);
}

/**
 * __idpf_adi_qid_reg_init - fill queue ids and tail registers of one type
 * @q: queue array to populate
 * @num_qids: capacity of @q
 * @q_type: queue type to collect
 * @chunks: queue chunks returned by the create-ADI reply
 *
 * Return: the number of entries written.
 */
static int
__idpf_adi_qid_reg_init(struct idpf_adi_q *q, int num_qids, uint16_t q_type,
    struct virtchnl2_non_flex_queue_reg_chunks *chunks)
{
	uint16_t num_chunks = le16toh(chunks->num_chunks);
	struct virtchnl2_queue_reg_chunk *chunk;
	uint32_t start_q_id, num_q, reg_spacing;
	uint32_t q_id_filled = 0, i = 0, c;
	uint64_t reg_val;

	for (c = 0; c < num_chunks; c++) {
		chunk = &chunks->chunks[c];
		if (le32toh(chunk->type) != q_type)
			continue;

		num_q = le32toh(chunk->num_queues);
		start_q_id = le32toh(chunk->start_queue_id);
		reg_val = le64toh(chunk->qtail_reg_start);
		reg_spacing = le32toh(chunk->qtail_reg_spacing);

		for (i = 0; i < num_q; i++) {
			if ((q_id_filled + i) >= (uint32_t)num_qids)
				break;
			q[q_id_filled + i].qid = start_q_id;
			q[q_id_filled + i].tail_reg = reg_val;
			reg_val += reg_spacing;
			start_q_id++;
		}

		q_id_filled += i;
	}

	return (q_id_filled);
}

/**
 * idpf_adi_qid_reg_init - populate queue ids and registers for every type
 * @priv: ADI to populate
 * @vc_cadi: create-ADI reply
 *
 * Return: 0 on success, EINVAL when any queue type came back empty.
 */
int
idpf_adi_qid_reg_init(struct idpf_adi_priv *priv,
    struct virtchnl2_non_flex_create_adi *vc_cadi)
{
	struct virtchnl2_non_flex_queue_reg_chunks *chunks = &vc_cadi->chunks;

	priv->qinfo.num_txqs = __idpf_adi_qid_reg_init(priv->qinfo.txq,
	    IDPF_MAX_ADI_Q_COUNT, VIRTCHNL2_QUEUE_TYPE_TX, chunks);
	if (priv->qinfo.num_txqs == 0)
		return (EINVAL);

	priv->qinfo.num_rxqs = __idpf_adi_qid_reg_init(priv->qinfo.rxq,
	    IDPF_MAX_ADI_Q_COUNT, VIRTCHNL2_QUEUE_TYPE_RX, chunks);
	if (priv->qinfo.num_rxqs == 0)
		return (EINVAL);

	priv->qinfo.num_complqs = __idpf_adi_qid_reg_init(priv->qinfo.complq,
	    IDPF_MAX_ADI_Q_COUNT, VIRTCHNL2_QUEUE_TYPE_TX_COMPLETION, chunks);
	if (priv->qinfo.num_complqs == 0)
		return (EINVAL);

	priv->qinfo.num_bufqs = __idpf_adi_qid_reg_init(priv->qinfo.bufq,
	    2 * IDPF_MAX_ADI_Q_COUNT, VIRTCHNL2_QUEUE_TYPE_RX_BUFFER, chunks);
	if (priv->qinfo.num_bufqs == 0)
		return (EINVAL);

	return (0);
}

/**
 * idpf_adi_alloc_vectors - claim this ADI's share of the interrupt vectors
 * @priv: ADI requesting vectors
 *
 * A partial grant is treated as failure: the guest driver expects exactly the
 * number of vectors advertised to it, so the partial set is handed straight
 * back.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_adi_alloc_vectors(struct idpf_adi_priv *priv)
{
	struct idpf_adapter *adapter = priv->adapter;
	struct idpf_vector_info vec_info;
	int alloc_cnt, hw_vec;

	priv->vec_info.vec_indexes = malloc(priv->vec_info.num_vectors *
	    sizeof(*priv->vec_info.vec_indexes), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (priv->vec_info.vec_indexes == NULL)
		return (ENOMEM);

	vec_info.num_req_vecs = priv->vec_info.num_vectors;
	vec_info.num_curr_vecs = 0;
	vec_info.index = 0;
	vec_info.default_vport = false;

	alloc_cnt = idpf_req_rel_vector_indexes(adapter,
	    priv->vec_info.vec_indexes, &vec_info);
	if (alloc_cnt != priv->vec_info.num_vectors) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "failed to allocate %d vectors (%d available)\n",
		    priv->vec_info.num_vectors, alloc_cnt);

		vec_info.num_req_vecs = 0;
		vec_info.num_curr_vecs = alloc_cnt;
		vec_info.index = 0;
		vec_info.default_vport = false;
		idpf_req_rel_vector_indexes(adapter,
		    priv->vec_info.vec_indexes, &vec_info);

		free(priv->vec_info.vec_indexes, M_DEVBUF);
		priv->vec_info.vec_indexes = NULL;

		return (ENOSPC);
	}

	hw_vec = idpf_adi_hw_vector(adapter, priv->vec_info.vec_indexes[0]);
	if (hw_vec < 0) {
		idpf_adi_dealloc_vectors(priv);
		return (EINVAL);
	}

	priv->vec_info.num_vectors = alloc_cnt;
	priv->vec_info.mbx_vec_id = hw_vec;

	/* The mailbox vector is first; the rest drive the data queues. */
	for (int i = IDPF_MBX_VECS_PER_ADI; i < alloc_cnt; i++) {
		hw_vec = idpf_adi_hw_vector(adapter,
		    priv->vec_info.vec_indexes[i]);
		if (hw_vec < 0) {
			idpf_adi_dealloc_vectors(priv);
			return (EINVAL);
		}
		priv->vec_info.data_q_vec_ids[i - IDPF_MBX_VECS_PER_ADI] =
		    hw_vec;
	}

	return (0);
}

/**
 * idpf_adi_dealloc_vectors - return this ADI's interrupt vectors
 * @priv: ADI releasing vectors
 */
void
idpf_adi_dealloc_vectors(struct idpf_adi_priv *priv)
{
	struct idpf_vector_info vec_info;

	vec_info.num_req_vecs = 0;
	vec_info.num_curr_vecs = priv->vec_info.num_vectors;
	vec_info.index = 0;
	vec_info.default_vport = false;

	idpf_req_rel_vector_indexes(priv->adapter, priv->vec_info.vec_indexes,
	    &vec_info);

	free(priv->vec_info.vec_indexes, M_DEVBUF);
	priv->vec_info.vec_indexes = NULL;
}

/**
 * idpf_adi_read_reg32 - translate a guest register read to a PF register
 * @priv: ADI the access belongs to
 * @offs: guest-visible register offset
 *
 * Return: the register value, or 0xdeadbeef for an offset the ADI does not
 * expose.
 */
uint32_t
idpf_adi_read_reg32(struct idpf_adi_priv *priv, size_t offs)
{
	struct idpf_adapter *adapter = priv->adapter;

	if (offs == VFGEN_RSTAT)
		return (priv->reset_state);

	switch (offs) {
	case VF_ATQBAL:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQBAL(priv->mbx_id))));
	case VF_ATQBAH:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQBAH(priv->mbx_id))));
	case VF_ATQLEN:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQLEN(priv->mbx_id))));
	case VF_ATQH:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQH(priv->mbx_id))));
	case VF_ATQT:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQT(priv->mbx_id))));
	case VF_ARQBAL:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQBAL(priv->mbx_id))));
	case VF_ARQBAH:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQBAH(priv->mbx_id))));
	case VF_ARQLEN:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQLEN(priv->mbx_id))));
	case VF_ARQH:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQH(priv->mbx_id))));
	case VF_ARQT:
		return (idpf_reg_rd32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQT(priv->mbx_id))));
	default:
		return (0xdeadbeef);
	}
}

/**
 * idpf_adi_write_reg32 - translate a guest register write to a PF register
 * @priv: ADI the access belongs to
 * @offs: guest-visible register offset
 * @data: value to write
 *
 * Queue and vector offsets are bounds checked against what the ADI actually
 * owns; anything outside is dropped rather than aimed at another function's
 * queue.
 */
void
idpf_adi_write_reg32(struct idpf_adi_priv *priv, size_t offs, uint32_t data)
{
	struct idpf_adapter *adapter = priv->adapter;
	int index;

	switch (offs) {
	case VF_ATQBAL:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQBAL(priv->mbx_id)), data);
		break;
	case VF_ATQBAH:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQBAH(priv->mbx_id)), data);
		break;
	case VF_ATQLEN:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQLEN(priv->mbx_id)), data);
		break;
	case VF_ATQH:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQH(priv->mbx_id)), data);
		break;
	case VF_ATQT:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ATQT(priv->mbx_id)), data);
		break;
	case VF_ARQBAL:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQBAL(priv->mbx_id)), data);
		break;
	case VF_ARQBAH:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQBAH(priv->mbx_id)), data);
		break;
	case VF_ARQLEN:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQLEN(priv->mbx_id)), data);
		break;
	case VF_ARQH:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQH(priv->mbx_id)), data);
		break;
	case VF_ARQT:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_MBX_ARQT(priv->mbx_id)), data);
		break;
	case VF_INT_DYN_CTL0:
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_GLINT_DYN_CTL(priv->vec_info.mbx_vec_id)), data);
		break;
	case VF_QRX_TAIL_EXT(0) ... VF_QRX_TAIL_EXT(255):
		index = (offs - VF_QRX_TAIL_EXT(0)) / 4;
		if (index >= priv->qinfo.num_rxqs)
			goto err;
		index = priv->qinfo.rxq[index].qid;
		idpf_reg_wr32(idpf_get_reg_addr(adapter, PF_QRX_TAIL(index)),
		    data);
		break;
	case VF_QRXB_TAIL(0) ... VF_QRXB_TAIL(255):
		index = (offs - VF_QRXB_TAIL(0)) / 4;
		if (index >= priv->qinfo.num_bufqs)
			goto err;
		index = priv->qinfo.bufq[index].qid;
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_QRX_BUFFQ_TAIL(index)), data);
		break;
	case VF_QTX_TAIL_EXT(0) ... VF_QTX_TAIL_EXT(255):
		index = (offs - VF_QTX_TAIL_EXT(0)) / 4;
		if (index >= priv->qinfo.num_txqs)
			goto err;
		index = priv->qinfo.txq[index].qid;
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_QTX_COMM_DBELL(index)), data);
		break;
	case VF_INT_DYN_CTLN(0) ... VF_INT_DYN_CTLN(255):
		index = (offs - VF_INT_DYN_CTLN(0)) / 4;
		if (index + IDPF_MBX_VECS_PER_ADI >=
		    priv->vec_info.num_vectors)
			goto err;
		index = priv->vec_info.data_q_vec_ids[index];
		idpf_reg_wr32(idpf_get_reg_addr(adapter,
		    PF_GLINT_DYN_CTL(index)), data);
		break;
	default:
		break;
	}

	return;

err:
	device_printf(idpf_adapter_to_dev(adapter),
	    "invalid resource access by ADI at 0x%zx\n", offs);
}

/**
 * idpf_adi_priv_free - release an ADI and its bookkeeping
 * @priv: ADI to release
 */
void
idpf_adi_priv_free(struct idpf_adi_priv *priv)
{
	struct idpf_adapter *adapter = priv->adapter;

	if (adapter != NULL) {
		mtx_lock(&adapter->adi_info.priv_lock);
		TAILQ_REMOVE(&adapter->adi_info.priv_list, priv, list);
		adapter->adi_info.curr_adi_cnt--;
		mtx_unlock(&adapter->adi_info.priv_lock);
	}

	free(priv->vec_info.data_q_vec_ids, M_DEVBUF);
	free(priv->qinfo.txq, M_DEVBUF);
	free(priv, M_DEVBUF);
}

/**
 * idpf_adi_priv_alloc - allocate an ADI and register it
 * @adapter: driver private data
 *
 * The four queue arrays are carved out of one allocation, so only
 * qinfo.txq is freed.
 *
 * Return: the new ADI, or NULL on failure.
 */
struct idpf_adi_priv *
idpf_adi_priv_alloc(struct idpf_adapter *adapter)
{
	struct idpf_adi_priv *priv;

	mtx_lock(&adapter->adi_info.priv_lock);
	if (adapter->adi_info.curr_adi_cnt >= adapter->adi_info.max_adi_cnt) {
		mtx_unlock(&adapter->adi_info.priv_lock);
		device_printf(idpf_adapter_to_dev(adapter),
		    "maximum number of ADIs (%u) has been reached\n",
		    adapter->adi_info.max_adi_cnt);
		return (NULL);
	}
	mtx_unlock(&adapter->adi_info.priv_lock);

	priv = malloc(sizeof(*priv), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (priv == NULL)
		return (NULL);

	priv->vec_info.data_q_vec_ids = malloc(IDPF_MAX_ADI_Q_COUNT *
	    sizeof(*priv->vec_info.data_q_vec_ids), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (priv->vec_info.data_q_vec_ids == NULL)
		goto err_free;

	priv->qinfo.txq = malloc(IDPF_MAX_ADI_Q_COUNT * IDPF_Q_ALLOC_CNT *
	    sizeof(struct idpf_adi_q), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (priv->qinfo.txq == NULL)
		goto err_free;

	priv->qinfo.rxq = priv->qinfo.txq + IDPF_MAX_ADI_Q_COUNT;
	priv->qinfo.complq = priv->qinfo.rxq + IDPF_MAX_ADI_Q_COUNT;
	priv->qinfo.bufq = priv->qinfo.complq + IDPF_MAX_ADI_Q_COUNT;
	priv->adapter = adapter;
	priv->adi_index = 0;
	priv->reset_state = IDPF_ADI_RESET_INPROGRESS;
	priv->vec_info.num_vectors = IDPF_DEFAULT_ADI_VEC +
	    IDPF_MBX_VECS_PER_ADI;

	mtx_lock(&adapter->adi_info.priv_lock);
	TAILQ_INSERT_TAIL(&adapter->adi_info.priv_list, priv, list);
	adapter->adi_info.curr_adi_cnt++;
	mtx_unlock(&adapter->adi_info.priv_lock);

	return (priv);

err_free:
	free(priv->vec_info.data_q_vec_ids, M_DEVBUF);
	free(priv, M_DEVBUF);

	return (NULL);
}

/**
 * idpf_adi_core_init - record the ADI budget granted by the control plane
 * @adapter: driver private data
 *
 * Return: 0.
 */
int
idpf_adi_core_init(struct idpf_adapter *adapter)
{
	uint16_t max_adi_cnt;

	max_adi_cnt = le16toh(adapter->caps.max_adis);
	if (!adapter->adi_info.siov_ena || max_adi_cnt == 0)
		return (0);

	adapter->adi_info.max_adi_cnt = min(max_adi_cnt,
	    (uint16_t)IDPF_MAX_ADI_NUM);
	device_printf(idpf_adapter_to_dev(adapter),
	    "up to %u ADIs are permitted\n", adapter->adi_info.max_adi_cnt);

	return (0);
}
