/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDPF_CONTROLQ_H_
#define _IDPF_CONTROLQ_H_

/*
 * Control queue descriptor layout and internal helpers.
 *
 * FreeBSD port notes: __le16/__le32 become plain u16/u32 - the
 * descriptor fields are little-endian on the wire and every access already
 * goes through htole*()/le*toh() explicitly.  BIT() is spelled out because
 * FreeBSD has no such macro.  The DMA allocation prototypes moved to
 * idpf_mem.h alongside struct idpf_dma_mem.  [FBSD15:A31]
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <machine/bus.h>

#include "idpf_controlq_api.h"
#include "idpf_devids.h"

#define IDPF_CTLQ_DESC(R, i) \
	(&(((struct idpf_ctlq_desc *)((R)->desc_ring.va))[i]))

#define IDPF_CTLQ_DESC_UNUSED(R) \
	((u16)((((R)->next_to_clean > (R)->next_to_use) ? 0 : \
	      (R)->ring_size) + (R)->next_to_clean - (R)->next_to_use - 1))

/* Control Queue default settings */
#define IDPF_CTRL_SQ_CMD_TIMEOUT	250  /* msecs */

#define IDPF_CTLQ_DESC_VF_ID_S	0
#define IDPF_CTLQ_DESC_VF_ID_M	(0x7FF << IDPF_CTLQ_DESC_VF_ID_S)
#define IDPF_CTLQ_DESC_PF_ID_S	11
#define IDPF_CTLQ_DESC_PF_ID_M	(0x1F << IDPF_CTLQ_DESC_PF_ID_S)

struct idpf_ctlq_desc {
	u16	flags;
	u16	opcode;
	u16	datalen;	/* 0 for direct commands */
	union {
		u16 ret_val;
		u16 pfid_vfid;
	};
	u32 cookie_high;
	u32 cookie_low;
	union {
		struct {
			u32 param0;
			u32 param1;
			u32 param2;
			u32 param3;
		} direct;
		struct {
			u32 param0;
			u32 param1;
			u32 addr_high;
			u32 addr_low;
		} indirect;
		u8 raw[16];
	} params;
};

/* Flags sub-structure
 * |0  |1  |2  |3  |4  |5  |6  |7  |8  |9  |10 |11 |12 |13 |14 |15 |
 * |DD |CMP|ERR|  * RSV *  |FTYPE  | *RSV* |RD |VFC|BUF|  HOST_ID  |
 */
/* command flags and offsets */
#define IDPF_CTLQ_FLAG_DD_S		0
#define IDPF_CTLQ_FLAG_CMP_S		1
#define IDPF_CTLQ_FLAG_ERR_S		2
#define IDPF_CTLQ_FLAG_FTYPE_S		6
#define IDPF_CTLQ_FLAG_RD_S		10
#define IDPF_CTLQ_FLAG_VFC_S		11
#define IDPF_CTLQ_FLAG_BUF_S		12
#define IDPF_CTLQ_FLAG_HOST_ID_S	13

#define IDPF_CTLQ_FLAG_DD	(1U << IDPF_CTLQ_FLAG_DD_S)	  /* 0x1    */
#define IDPF_CTLQ_FLAG_CMP	(1U << IDPF_CTLQ_FLAG_CMP_S)	  /* 0x2    */
#define IDPF_CTLQ_FLAG_ERR	(1U << IDPF_CTLQ_FLAG_ERR_S)	  /* 0x4    */
#define IDPF_CTLQ_FLAG_FTYPE_VM	(1U << IDPF_CTLQ_FLAG_FTYPE_S)	  /* 0x40   */
#define IDPF_CTLQ_FLAG_FTYPE_PF	(1U << (IDPF_CTLQ_FLAG_FTYPE_S + 1)) /* 0x80 */
#define IDPF_CTLQ_FLAG_RD	(1U << IDPF_CTLQ_FLAG_RD_S)	  /* 0x400  */
#define IDPF_CTLQ_FLAG_VFC	(1U << IDPF_CTLQ_FLAG_VFC_S)	  /* 0x800  */
#define IDPF_CTLQ_FLAG_BUF	(1U << IDPF_CTLQ_FLAG_BUF_S)	  /* 0x1000 */

struct idpf_mbxq_desc {
	u8  pad[8];	/* CTLQ flags/opcode/len/retval fields */
	u32 chnl_opcode;	/* avoid confusion with desc->opcode */
	u32 chnl_retval;	/* ditto for desc->retval */
	u32 pf_vf_id;	/* used by CP when sending to PF */
};

/**
 * idpf_ctlq_dma_sync - synchronise a control queue DMA mapping
 * @mem: DMA memory to synchronise, may be NULL
 * @op: BUS_DMASYNC_* operation
 *
 * Tolerates an absent or not-yet-created mapping so that callers do not have
 * to guard every call site.
 */
static inline void
idpf_ctlq_dma_sync(struct idpf_dma_mem *mem, int op)
{

	if (mem != NULL && mem->tag != NULL && mem->map != NULL)
		bus_dmamap_sync(mem->tag, mem->map, op);
}

int idpf_ctlq_alloc_ring_res(struct idpf_hw *hw,
			     struct idpf_ctlq_info *cq);

void idpf_ctlq_dealloc_ring_res(struct idpf_hw *hw, struct idpf_ctlq_info *cq);

#endif /* _IDPF_CONTROLQ_H_ */
