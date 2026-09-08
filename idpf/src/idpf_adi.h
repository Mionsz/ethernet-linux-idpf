/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDPF_ADI_H_
#define _IDPF_ADI_H_

/*
 * Assignable Device Interface (ADI) state for Scalable IOV.
 *
 * FreeBSD port notes
 * ------------------
 * The Linux driver splits this feature in two: idpf_adi.c owns the hardware
 * state and idpf_vdcm_*.c bind it to VFIO/mdev, with the shared types living
 * in idpf_vdcm.h.  FreeBSD has no VFIO/mdev, so idpf_vdcm.h is not part of
 * this port and the device-model-agnostic types it declared now live here.
 * What is deliberately absent is the VFIO surface: the struct idpf_adi ops
 * table, sparse mmap descriptions, and the mdev alloc/free entry points.
 *
 *   struct xarray priv_info -> TAILQ of struct idpf_adi_priv
 *   struct msix_entry *     -> struct resource ** (see struct idpf_adapter)
 *
 * ADI create and destroy are not implemented: they ride on
 * VIRTCHNL2_OP_NON_FLEX_CREATE_ADI / _DESTROY_ADI, which this port does not
 * carry.  What remains is the state the rest of the driver touches - the
 * reset notification path reached through dev_ops.notify_adi_reset - plus the
 * register-window translation, which is device-model independent.
 * [LOCAL:A22] [FBSD15:A30-A34]
 */

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include "siov_regs.h"

struct idpf_adapter;
struct virtchnl2_non_flex_create_adi;

#define VDEV_MBX_ARQBAL			(VDEV_MBX_START + 0x0000)
#define VDEV_MBX_ARQBAH			(VDEV_MBX_START + 0x0004)
#define VDEV_MBX_ARQLEN			(VDEV_MBX_START + 0x0008)
#define VDEV_MBX_ARQH			(VDEV_MBX_START + 0x000C)
#define VDEV_MBX_ARQT			(VDEV_MBX_START + 0x0010)
#define VDEV_MBX_ATQBAL			(VDEV_MBX_START + 0x0014)
#define VDEV_MBX_ATQBAH			(VDEV_MBX_START + 0x0018)
#define VDEV_MBX_ATQLEN			(VDEV_MBX_START + 0x001C)
#define VDEV_MBX_ATQH			(VDEV_MBX_START + 0x0020)
#define VDEV_MBX_ATQT			(VDEV_MBX_START + 0x0024)

/* ADI vectors */
#define IDPF_MAX_ADI_Q_COUNT		64
#define IDPF_MBX_VECS_PER_ADI		1
#define IDPF_PAGES_FOR_MBX_REGS		1
#define IDPF_DEFAULT_ADI_VEC		8
/* Max number of ADIs supported */
#define IDPF_MAX_ADI_NUM		30

/**
 * struct idpf_adi_vec_info - ADI vector information
 * @num_vectors: vectors assigned to this ADI, data queue and mailbox alike
 * @vec_indexes: driver-relative vector indexes, mailbox vector first
 * @mbx_vec_id: hardware vector index of the mailbox vector
 * @data_q_vec_ids: hardware vector indexes of the data queue vectors
 */
struct idpf_adi_vec_info {
	u16	 num_vectors;
	u16	*vec_indexes;
	int		 mbx_vec_id;
	int		*data_q_vec_ids;
};

struct idpf_adi_q {
	int		qid;
	u64	tail_reg;
};

struct idpf_adi_queue_info {
	int	num_txqs;
	int	num_complqs;
	int	num_rxqs;
	int	num_bufqs;
	struct idpf_adi_q *txq;
	struct idpf_adi_q *complq;
	struct idpf_adi_q *rxq;
	struct idpf_adi_q *bufq;
};

enum idpf_adi_reset_state {
	IDPF_ADI_RESET_INPROGRESS,
	IDPF_ADI_RESET_COMPLETED
};

struct idpf_adi_priv {
	TAILQ_ENTRY(idpf_adi_priv) list;
	struct idpf_adapter	*adapter;
	struct idpf_adi_vec_info vec_info;
	struct idpf_adi_queue_info qinfo;
	int			 mbx_id;
	/* ADI id assigned by the control plane */
	int			 adi_id;
	u16		 adi_index;
	enum idpf_adi_reset_state reset_state;
};

/**
 * struct idpf_adi_info - registry of the ADIs owned by this function
 * @priv_list: every live struct idpf_adi_priv
 * @priv_lock: protects @priv_list and @curr_adi_cnt
 * @max_adi_cnt: ADI budget granted by the control plane
 * @curr_adi_cnt: ADIs currently allocated
 * @siov_ena: true once Scalable IOV has been enabled on this function
 */
struct idpf_adi_info {
	TAILQ_HEAD(idpf_adi_priv_head, idpf_adi_priv) priv_list;
	struct mtx	priv_lock;
	u16	max_adi_cnt;
	u16	curr_adi_cnt;
	bool		siov_ena;
};

int  idpf_adi_core_init(struct idpf_adapter *adapter);
void idpf_notify_adi_reset(struct idpf_adapter *adapter, u16 adi_id,
    bool reset);
struct idpf_adi_priv *idpf_adi_priv_alloc(struct idpf_adapter *adapter);
void idpf_adi_priv_free(struct idpf_adi_priv *priv);
int  idpf_adi_qid_reg_init(struct idpf_adi_priv *priv,
    struct virtchnl2_non_flex_create_adi *vc_cadi);
int  idpf_adi_alloc_vectors(struct idpf_adi_priv *priv);
void idpf_adi_dealloc_vectors(struct idpf_adi_priv *priv);
u32 idpf_adi_read_reg32(struct idpf_adi_priv *priv, size_t offs);
void idpf_adi_write_reg32(struct idpf_adi_priv *priv, size_t offs,
    u32 data);

#endif /* !_IDPF_ADI_H_ */
