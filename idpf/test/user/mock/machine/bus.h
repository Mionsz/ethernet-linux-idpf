/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/* Userspace stand-in for machine/bus.h: bus_dma types only, no real DMA. */

#ifndef _IDPF_MOCK_MACHINE_BUS_H_
#define _IDPF_MOCK_MACHINE_BUS_H_

#include <sys/systm.h>

typedef uint64_t	bus_addr_t;
typedef uint64_t	bus_size_t;

struct idpf_mock_dma_tag;
typedef struct idpf_mock_dma_tag	*bus_dma_tag_t;
typedef struct idpf_mock_dma_map	*bus_dmamap_t;

#define BUS_DMASYNC_PREREAD	0x01
#define BUS_DMASYNC_POSTREAD	0x02
#define BUS_DMASYNC_PREWRITE	0x04
#define BUS_DMASYNC_POSTWRITE	0x08

#define BUS_SPACE_MAXADDR	0xFFFFFFFFFFFFFFFFULL
#define BUS_SPACE_MAXSIZE	0xFFFFFFFFFFFFFFFFULL

/*
 * Counted so a test can assert the driver syncs a descriptor ring before
 * reading it; there is no coherency to enforce in userspace.
 */
void idpf_test_dmamap_sync(bus_dma_tag_t tag, bus_dmamap_t map, int op);
long idpf_test_sync_count(void);

#define bus_dmamap_sync(t, m, op)	idpf_test_dmamap_sync((t), (m), (op))

#endif /* _IDPF_MOCK_MACHINE_BUS_H_ */
