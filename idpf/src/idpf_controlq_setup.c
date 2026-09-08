/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Control queue ring and buffer allocation.
 *
 * FreeBSD port notes: kcalloc()/kfree() become malloc()/free() on M_DEVBUF,
 * and every function returns a positive errno.  The DMA seam
 * (idpf_alloc_dma_mem/idpf_free_dma_mem) is unchanged.  [FBSD15:A31]
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>

#include "idpf_controlq.h"

/**
 * idpf_ctlq_alloc_desc_ring - Allocate Control Queue (CQ) rings
 * @hw: pointer to hw struct
 * @cq: pointer to the specific Control queue
 */
static int
idpf_ctlq_alloc_desc_ring(struct idpf_hw *hw, struct idpf_ctlq_info *cq)
{
	size_t size = cq->ring_size * sizeof(struct idpf_ctlq_desc);

	cq->desc_ring.va = idpf_alloc_dma_mem(hw, &cq->desc_ring, size);
	if (cq->desc_ring.va == NULL)
		return (ENOMEM);

	return (0);
}

/**
 * idpf_ctlq_alloc_bufs - Allocate Control Queue (CQ) buffers
 * @hw: pointer to hw struct
 * @cq: pointer to the specific Control queue
 *
 * Allocate the buffer head for all control queues, and if it's a receive
 * queue, allocate DMA buffers
 */
static int
idpf_ctlq_alloc_bufs(struct idpf_hw *hw, struct idpf_ctlq_info *cq)
{
	int i;

	/* Do not allocate DMA buffers for transmit queues */
	if (cq->cq_type == IDPF_CTLQ_TYPE_MAILBOX_TX)
		return (0);

	/* We'll be allocating the buffer info memory first, then we can
	 * allocate the mapped buffers for the event processing
	 */
	cq->bi.rx_buff = malloc(cq->ring_size * sizeof(struct idpf_dma_mem *),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (cq->bi.rx_buff == NULL)
		return (ENOMEM);

	/* allocate the mapped buffers (except for the last one) */
	for (i = 0; i < cq->ring_size - 1; i++) {
		struct idpf_dma_mem *bi;

		cq->bi.rx_buff[i] = malloc(sizeof(struct idpf_dma_mem),
		    M_DEVBUF, M_NOWAIT | M_ZERO);
		if (cq->bi.rx_buff[i] == NULL)
			goto unwind_alloc_cq_bufs;

		bi = cq->bi.rx_buff[i];

		bi->va = idpf_alloc_dma_mem(hw, bi, cq->buf_size);
		if (bi->va == NULL) {
			/* unwind will not free the failed entry */
			free(cq->bi.rx_buff[i], M_DEVBUF);
			goto unwind_alloc_cq_bufs;
		}
	}

	return (0);

unwind_alloc_cq_bufs:
	/* don't try to free the one that failed... */
	i--;
	for (; i >= 0; i--) {
		idpf_free_dma_mem(hw, cq->bi.rx_buff[i]);
		free(cq->bi.rx_buff[i], M_DEVBUF);
	}
	free(cq->bi.rx_buff, M_DEVBUF);
	cq->bi.rx_buff = NULL;

	return (ENOMEM);
}

/**
 * idpf_ctlq_free_desc_ring - Free Control Queue (CQ) rings
 * @hw: pointer to hw struct
 * @cq: pointer to the specific Control queue
 *
 * This assumes the posted send buffers have already been cleaned
 * and de-allocated
 */
static void
idpf_ctlq_free_desc_ring(struct idpf_hw *hw, struct idpf_ctlq_info *cq)
{

	idpf_free_dma_mem(hw, &cq->desc_ring);
}

/**
 * idpf_ctlq_free_bufs - Free CQ buffer info elements
 * @hw: pointer to hw struct
 * @cq: pointer to the specific Control queue
 *
 * Free the DMA buffers for RX queues, and DMA buffer header for both RX and TX
 * queues.  The upper layers are expected to manage freeing of TX DMA buffers
 */
static void
idpf_ctlq_free_bufs(struct idpf_hw *hw, struct idpf_ctlq_info *cq)
{
	void *bi;

	if (cq->cq_type == IDPF_CTLQ_TYPE_MAILBOX_RX) {
		int i;

		/* free DMA buffers for rx queues */
		for (i = 0; i < cq->ring_size; i++) {
			if (cq->bi.rx_buff[i] != NULL) {
				idpf_free_dma_mem(hw, cq->bi.rx_buff[i]);
				free(cq->bi.rx_buff[i], M_DEVBUF);
			}
		}

		bi = cq->bi.rx_buff;
	} else {
		bi = cq->bi.tx_msg;
	}

	/* free the buffer header */
	free(bi, M_DEVBUF);
}

/**
 * idpf_ctlq_dealloc_ring_res - Free memory allocated for control queue
 * @hw: pointer to hw struct
 * @cq: pointer to the specific Control queue
 *
 * Free the memory used by the ring, buffers and other related structures
 */
void
idpf_ctlq_dealloc_ring_res(struct idpf_hw *hw, struct idpf_ctlq_info *cq)
{

	idpf_ctlq_free_bufs(hw, cq);
	idpf_ctlq_free_desc_ring(hw, cq);
}

/**
 * idpf_ctlq_alloc_ring_res - allocate memory for descriptor ring and bufs
 * @hw: pointer to hw struct
 * @cq: pointer to control queue struct
 *
 * Do *NOT* hold cq_lock when calling this: the allocators may sleep.
 */
int
idpf_ctlq_alloc_ring_res(struct idpf_hw *hw, struct idpf_ctlq_info *cq)
{
	int err;

	err = idpf_ctlq_alloc_desc_ring(hw, cq);
	if (err != 0)
		return (err);

	err = idpf_ctlq_alloc_bufs(hw, cq);
	if (err != 0)
		goto idpf_init_cq_free_ring;

	return (0);

idpf_init_cq_free_ring:
	idpf_free_dma_mem(hw, &cq->desc_ring);

	return (err);
}
