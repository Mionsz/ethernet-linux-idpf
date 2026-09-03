/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Vport lifecycle, interrupt distribution, reset handling and the iflib
 * driver interface.
 *
 * FreeBSD port notes
 * ------------------
 * Interface model.  Linux allocates one struct net_device per vport with
 * alloc_etherdev_mqs() and drives it through struct net_device_ops.  FreeBSD
 * uses iflib: each vport owns an if_ctx_t recorded in adapter->iflib_ctxs[],
 * whose softc is the struct idpf_netdev_priv this file manipulates, and the
 * ndo_* callbacks become the ifdi_* methods at the end of this file.
 *
 * MSI-X ownership.  iflib normally allocates MSI-X itself, but IDPF pools its
 * vectors across every vport on the function and hands them out from a LIFO
 * stack, so the driver allocates the vectors (IFLIB_SKIP_MSIX) and tells
 * iflib which one to use per queue in ifdi_msix_intr_assign().  [FBSD15:A34]
 *
 * Locking.  vector_lock and vport_ctrl_lock are struct sx because both are
 * held across allocations that may sleep; the MAC filter list is a TAILQ
 * under an MTX_DEF mutex.  Linux delayed_work becomes struct timeout_task for
 * one-shot delayed work and struct callout for periodic work.  [FBSD15:A32]
 *
 * Removed features.  XDP/AF_XDP, ethtool, devlink, IDC/RDMA/RCA, SR-IOV VF
 * enablement, uplink port representor statistics and TC/ETF offload are not
 * part of this port; each is called out where its call sites used to be.
 * [LOCAL:A18] [LOCAL:A22]
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sx.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/if_var.h>
#include <net/ethernet.h>

#include <sys/sockio.h>

#include "idpf.h"
#include "idpf_virtchnl.h"
#include "idpf_ptp.h"

#include "ifdi_if.h"

/* Longest interrupt description this driver builds. */
#define IDPF_INT_NAME_STR_LEN	32

/**
 * idpf_is_valid_ether_addr - reject unusable unicast addresses
 * @addr: address to test
 */
static inline bool
idpf_is_valid_ether_addr(const uint8_t *addr)
{
	static const uint8_t zero[ETHER_ADDR_LEN];

	return (!ETHER_IS_MULTICAST(addr) &&
	    memcmp(addr, zero, ETHER_ADDR_LEN) != 0);
}

static void idpf_vport_stop(struct idpf_vport *vport);
static int  idpf_vport_open(struct idpf_vport *vport);
static int  idpf_apply_capabilities(struct idpf_vport *vport);

/* ---------------------------------------------------------------------
 * Interrupt vector pool
 * --------------------------------------------------------------------- */

/**
 * idpf_init_vector_stack - fill the MSI-X vector stack with vector indexes
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_init_vector_stack(struct idpf_adapter *adapter)
{
	struct idpf_vector_lifo *stack;
	uint16_t min_vec;
	uint32_t i;

	sx_xlock(&adapter->vector_lock);

	min_vec = adapter->num_msix_entries - adapter->num_avail_msix;
	stack = &adapter->vector_stack;
	stack->size = adapter->num_msix_entries;
	/*
	 * Base and top both start at the free pool so the reserved per-vport
	 * vectors below @min_vec are never handed out on demand.
	 */
	stack->base = min_vec;
	stack->top = min_vec;

	stack->vec_idx = malloc(stack->size * sizeof(*stack->vec_idx),
	    M_DEVBUF, M_NOWAIT | M_ZERO);
	if (stack->vec_idx == NULL) {
		sx_xunlock(&adapter->vector_lock);
		return (ENOMEM);
	}

	for (i = 0; i < stack->size; i++)
		stack->vec_idx[i] = i;

	sx_xunlock(&adapter->vector_lock);

	return (0);
}

/**
 * idpf_deinit_vector_stack - release the MSI-X vector stack
 * @adapter: driver private data
 */
void
idpf_deinit_vector_stack(struct idpf_adapter *adapter)
{
	struct idpf_vector_lifo *stack;

	sx_xlock(&adapter->vector_lock);
	stack = &adapter->vector_stack;
	free(stack->vec_idx, M_DEVBUF);
	stack->vec_idx = NULL;
	sx_xunlock(&adapter->vector_lock);
}

/**
 * idpf_vector_lifo_push - push an MSI-X vector index onto the stack
 * @adapter: driver private data
 * @vec_idx: vector index to store
 *
 * Return: 0 on success, EINVAL when the stack is already full.
 */
static int
idpf_vector_lifo_push(struct idpf_adapter *adapter, uint16_t vec_idx)
{
	struct idpf_vector_lifo *stack = &adapter->vector_stack;

	sx_assert(&adapter->vector_lock, SA_XLOCKED);

	if (stack->top == stack->base) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "exceeded the vector stack limit: %d\n", stack->top);
		return (EINVAL);
	}

	stack->vec_idx[--stack->top] = vec_idx;

	return (0);
}

/**
 * idpf_vector_lifo_pop - pop an MSI-X vector index from the stack
 * @adapter: driver private data
 *
 * Return: the vector index, or -1 when the stack is empty.
 */
static int
idpf_vector_lifo_pop(struct idpf_adapter *adapter)
{
	struct idpf_vector_lifo *stack = &adapter->vector_stack;

	sx_assert(&adapter->vector_lock, SA_XLOCKED);

	if (stack->top == stack->size) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "no interrupt vectors are available to distribute\n");
		return (-1);
	}

	return (stack->vec_idx[stack->top++]);
}

/**
 * idpf_vector_stash - return previously allocated vector indexes to the stack
 * @adapter: driver private data
 * @q_vector_idxs: vector index array
 * @vec_info: how many vectors the caller currently holds
 */
static void
idpf_vector_stash(struct idpf_adapter *adapter, uint16_t *q_vector_idxs,
    struct idpf_vector_info *vec_info)
{
	int i, base = 0;
	uint16_t vec_idx;

	sx_assert(&adapter->vector_lock, SA_XLOCKED);

	if (vec_info->num_curr_vecs == 0)
		return;

	/*
	 * Default vports keep their reserved vectors; only what they drew from
	 * the free pool goes back on the stack.
	 */
	if (vec_info->default_vport)
		base = IDPF_MIN_Q_VEC;

	for (i = vec_info->num_curr_vecs - 1; i >= base; i--) {
		vec_idx = q_vector_idxs[i];
		idpf_vector_lifo_push(adapter, vec_idx);
		adapter->num_avail_msix++;
	}
}

/**
 * idpf_req_rel_vector_indexes - request or release MSI-X vector indexes
 * @adapter: driver private data
 * @q_vector_idxs: vector index array
 * @vec_info: number of vectors required and currently held
 *
 * Stashes whatever the caller already holds, then satisfies the new request
 * from what is left.  Requesting zero vectors is the release path.
 *
 * Return: the number of vectors allocated; 0 means the request could not be
 * satisfied at all, which is a failure for the caller.
 */
int
idpf_req_rel_vector_indexes(struct idpf_adapter *adapter,
    uint16_t *q_vector_idxs, struct idpf_vector_info *vec_info)
{
	uint16_t num_req_vecs, num_alloc_vecs = 0, max_vecs;
	struct idpf_vector_lifo *stack;
	int i, j, vecid;

	sx_xlock(&adapter->vector_lock);

	stack = &adapter->vector_stack;
	num_req_vecs = vec_info->num_req_vecs;

	idpf_vector_stash(adapter, q_vector_idxs, vec_info);

	if (num_req_vecs == 0)
		goto rel_lock;

	if (vec_info->default_vport) {
		/*
		 * IDPF_MIN_Q_VEC per default vport sits below the free pool;
		 * hand those out directly.
		 */
		j = vec_info->index * IDPF_MIN_Q_VEC + IDPF_MBX_Q_VEC;
		for (i = 0; i < IDPF_MIN_Q_VEC; i++) {
			q_vector_idxs[num_alloc_vecs++] = stack->vec_idx[j++];
			num_req_vecs--;
		}
	}

	max_vecs = min(adapter->num_avail_msix, num_req_vecs);

	for (j = 0; j < max_vecs; j++) {
		vecid = idpf_vector_lifo_pop(adapter);
		if (vecid < 0)
			break;
		q_vector_idxs[num_alloc_vecs++] = vecid;
	}
	adapter->num_avail_msix -= j;

rel_lock:
	sx_xunlock(&adapter->vector_lock);

	return (num_alloc_vecs);
}

/* ---------------------------------------------------------------------
 * Mailbox interrupt
 * --------------------------------------------------------------------- */

/**
 * idpf_mb_intr_rel_irq - detach the mailbox interrupt handler
 * @adapter: driver private data
 *
 * Also leaves interrupt mode, so the mailbox task is queued to keep polling
 * the mailbox from here on.
 */
void
idpf_mb_intr_rel_irq(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);

	if ((adapter->flags & (1u << IDPF_MB_INTR_MODE)) == 0)
		return;
	adapter->flags &= ~(1u << IDPF_MB_INTR_MODE);

	if (adapter->mb_intr_tag != NULL) {
		bus_teardown_intr(dev, adapter->msix_entries[0],
		    adapter->mb_intr_tag);
		adapter->mb_intr_tag = NULL;
	}

	free(adapter->mb_vector.name, M_DEVBUF);
	adapter->mb_vector.name = NULL;

	taskqueue_enqueue(adapter->mbx_wq, &adapter->mbx_task);
}

/**
 * idpf_intr_rel - release interrupt capabilities and free memory
 * @adapter: driver private data
 */
void
idpf_intr_rel(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	int i;

	if (adapter->msix_entries == NULL)
		return;

	idpf_mb_intr_rel_irq(adapter);

	for (i = 0; i < adapter->num_msix_entries; i++) {
		if (adapter->msix_entries[i] == NULL)
			continue;
		bus_release_resource(dev, SYS_RES_IRQ, i + 1,
		    adapter->msix_entries[i]);
		adapter->msix_entries[i] = NULL;
	}
	pci_release_msi(dev);

	idpf_send_dealloc_vectors_msg(adapter);
	idpf_deinit_vector_stack(adapter);

	free(adapter->msix_entries, M_DEVBUF);
	adapter->msix_entries = NULL;
}

static void idpf_mb_irq_enable(struct idpf_adapter *adapter);

/**
 * idpf_mb_intr_clean - mailbox interrupt filter
 * @data: adapter
 *
 * Runs in filter context, so it may only record state and schedule work; the
 * CORER waiter is woken from idpf_mbx_task() instead.
 *
 * Return: FILTER_HANDLED.
 */
static int
idpf_mb_intr_clean(void *data)
{
	struct idpf_adapter *adapter = data;

	/* A mailbox interrupt during CORER signals that the reset finished. */
	if ((adapter->flags & (1u << IDPF_CORER_IN_PROG)) != 0) {
		adapter->flags &= ~(1u << IDPF_CORER_IN_PROG);
		atomic_store_rel_int(&adapter->corer_done_flag, 1);
		taskqueue_enqueue(adapter->mbx_wq, &adapter->mbx_task);
		idpf_mb_irq_enable(adapter);

		return (FILTER_HANDLED);
	}

	/* The ASQ may not be set up yet. */
	if (adapter->hw.asq != NULL) {
		uint32_t len;

		len = idpf_reg_rd32(idpf_get_mbx_reg_addr(adapter,
		    adapter->hw.asq->reg.len));
		if ((len & adapter->hw.asq->reg.len_ena_mask) == 0) {
			adapter->flags |= (1u << IDPF_CORER_IN_PROG);
			atomic_store_rel_int(&adapter->corer_done_flag, 0);
		}
	}

	taskqueue_enqueue(adapter->mbx_wq, &adapter->mbx_task);
	callout_reset(&adapter->serv_task, 1, idpf_service_task, adapter);

	/* Clear the cause and unmask, or the vector re-fires immediately. */
	idpf_mb_irq_enable(adapter);

	return (FILTER_HANDLED);
}

/**
 * idpf_mb_irq_enable - unmask the mailbox MSI-X vector
 * @adapter: driver private data
 */
static void
idpf_mb_irq_enable(struct idpf_adapter *adapter)
{
	struct idpf_intr_reg *intr = &adapter->mb_vector.intr_reg;
	uint32_t val;

	val = intr->dyn_ctl_intena_m | intr->dyn_ctl_itridx_m;
	idpf_reg_wr32(intr->dyn_ctl, val);
	idpf_reg_wr32(intr->icr_ena, intr->icr_ena_ctlq_m);
}

/**
 * idpf_mb_intr_req_irq - attach the mailbox interrupt handler
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_mb_intr_req_irq(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	const int mb_vidx = IDPF_MBX_VEC_IDX;
	char *name;
	int err;

	name = malloc(IDPF_INT_NAME_STR_LEN, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (name == NULL)
		return (ENOMEM);
	snprintf(name, IDPF_INT_NAME_STR_LEN, "%s-Mailbox-%d",
	    device_get_nameunit(dev), mb_vidx);

	err = bus_setup_intr(dev, adapter->msix_entries[mb_vidx],
	    INTR_TYPE_NET | INTR_MPSAFE, adapter->irq_mb_handler, NULL,
	    adapter, &adapter->mb_intr_tag);
	if (err != 0) {
		free(name, M_DEVBUF);
		device_printf(dev,
		    "IRQ request for mailbox failed, error: %d\n", err);
		return (err);
	}
	bus_describe_intr(dev, adapter->msix_entries[mb_vidx],
	    adapter->mb_intr_tag, "%s", name);

	adapter->mb_vector.name = name;
	adapter->flags |= (1u << IDPF_MB_INTR_MODE);

	return (0);
}

/**
 * idpf_mb_intr_init - initialise the mailbox interrupt
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_mb_intr_init(struct idpf_adapter *adapter)
{

	adapter->dev_ops.reg_ops.mb_intr_reg_init(adapter);
	adapter->irq_mb_handler = idpf_mb_intr_clean;

	return (idpf_mb_intr_req_irq(adapter));
}

/**
 * idpf_intr_req - acquire the function's MSI-X vectors
 * @adapter: driver private data
 *
 * The control plane is asked for the data queue vectors first, then the OS is
 * asked for the matching MSI-X allocation.  Every vector is claimed as an IRQ
 * resource here rather than by iflib, because the pool is shared by all the
 * vports on this function.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_intr_req(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	uint16_t default_vports = idpf_get_default_vports(adapter);
	uint16_t num_lan_vecs, min_lan_vecs;
	int num_q_vecs, total_vecs, num_vec_ids;
	int actual_vecs, err;
	uint16_t *vecids = NULL;
	int i, rid;

	total_vecs = idpf_get_reserved_vecs(adapter);
	num_q_vecs = total_vecs - IDPF_MBX_Q_VEC;

	err = idpf_send_alloc_vectors_msg(adapter, num_q_vecs);
	if (err != 0) {
		device_printf(dev, "failed to allocate %d vectors: %d\n",
		    num_q_vecs, err);
		return (EAGAIN);
	}

	min_lan_vecs = IDPF_MBX_Q_VEC + IDPF_MIN_Q_VEC * default_vports;

	actual_vecs = total_vecs;
	err = pci_alloc_msix(dev, &actual_vecs);
	if (err != 0 || actual_vecs < min_lan_vecs) {
		device_printf(dev,
		    "failed to allocate the minimum %u MSI-X vectors "
		    "(got %d): %d\n", min_lan_vecs, actual_vecs, err);
		if (err == 0) {
			pci_release_msi(dev);
			err = ENOSPC;
		}
		goto send_dealloc_vecs;
	}
	num_lan_vecs = actual_vecs;

	adapter->msix_entries = malloc(num_lan_vecs *
	    sizeof(*adapter->msix_entries), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter->msix_entries == NULL) {
		err = ENOMEM;
		goto free_irq;
	}

	/*
	 * Only the mailbox vector is claimed here.  iflib allocates the
	 * per-queue IRQ resources in ifdi_msix_intr_assign(), and claiming
	 * them first makes that allocation fail with a busy rid.
	 */
	rid = IDPF_MBX_VEC_IDX + 1;
	adapter->msix_entries[IDPF_MBX_VEC_IDX] = bus_alloc_resource_any(dev,
	    SYS_RES_IRQ, &rid, RF_ACTIVE | RF_SHAREABLE);
	if (adapter->msix_entries[IDPF_MBX_VEC_IDX] == NULL) {
		device_printf(dev,
		    "failed to allocate the mailbox IRQ resource\n");
		err = ENXIO;
		goto free_msix;
	}

	adapter->mb_vector.v_idx = le16toh(adapter->caps.mailbox_vector_id);

	vecids = malloc(actual_vecs * sizeof(*vecids), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (vecids == NULL) {
		err = ENOMEM;
		goto free_msix;
	}

	num_vec_ids = idpf_get_vec_ids(adapter, vecids, actual_vecs,
	    &adapter->req_vec_chunks->vchunks);
	if (num_vec_ids < actual_vecs) {
		err = EINVAL;
		goto free_vecids;
	}

	/*
	 * num_avail_msix is what is left to distribute to the vports once each
	 * default vport's reserved minimum has been set aside.
	 */
	adapter->num_avail_msix = num_lan_vecs - min_lan_vecs;
	adapter->num_msix_entries = num_lan_vecs;

	err = idpf_init_vector_stack(adapter);
	if (err != 0)
		goto free_vecids;

	err = idpf_mb_intr_init(adapter);
	if (err != 0)
		goto deinit_vec_stack;

	idpf_mb_irq_enable(adapter);
	free(vecids, M_DEVBUF);

	return (0);

deinit_vec_stack:
	idpf_deinit_vector_stack(adapter);
free_vecids:
	free(vecids, M_DEVBUF);
free_msix:
	for (i = 0; i < num_lan_vecs; i++) {
		if (adapter->msix_entries[i] == NULL)
			continue;
		bus_release_resource(dev, SYS_RES_IRQ, i + 1,
		    adapter->msix_entries[i]);
	}
	free(adapter->msix_entries, M_DEVBUF);
	adapter->msix_entries = NULL;
free_irq:
	pci_release_msi(dev);
send_dealloc_vecs:
	idpf_send_dealloc_vectors_msg(adapter);

	return (err);
}

/* ---------------------------------------------------------------------
 * Capabilities and DMA
 * --------------------------------------------------------------------- */

/**
 * idpf_is_capability_ena - test a negotiated capability flag
 * @adapter: driver private data
 * @all: true when every bit in @flag must be set
 * @field: which capability word to inspect
 * @flag: bits to test
 *
 * Return: whether the capability is present.
 */
bool
idpf_is_capability_ena(struct idpf_adapter *adapter, bool all,
    enum idpf_cap_field field, uint64_t flag)
{
	uint8_t *caps = (uint8_t *)&adapter->caps;
	uint64_t *cap_field;

	if (field == IDPF_BASE_CAPS)
		return (false);

	cap_field = (uint64_t *)(caps + field);

	if (all)
		return ((*cap_field & flag) == flag);

	return ((*cap_field & flag) != 0);
}

/**
 * idpf_dma_map_cb - bus_dma callback recording the mapped address
 * @arg: where to store the bus address
 * @segs: mapped segments
 * @nseg: segment count
 * @error: mapping error
 */
static void
idpf_dma_map_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{

	if (error != 0 || nseg != 1)
		return;

	*(bus_addr_t *)arg = segs[0].ds_addr;
}

/**
 * idpf_alloc_dma_mem - allocate coherent DMA memory
 * @hw: hardware struct
 * @mem: descriptor to fill
 * @size: bytes required
 *
 * A single physically contiguous segment is requested because the control
 * queue releases this memory with its queue lock held.
 *
 * Return: the mapped virtual address, or NULL.
 */
void *
idpf_alloc_dma_mem(struct idpf_hw *hw, struct idpf_dma_mem *mem, uint64_t size)
{
	struct idpf_adapter *adapter = hw->back;
	device_t dev = idpf_adapter_to_dev(adapter);
	bus_size_t sz = roundup2(size, 4096);
	int err;

	err = bus_dma_tag_create(bus_get_dma_tag(dev), 4096, 0,
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL, sz, 1, sz,
	    0, NULL, NULL, &mem->tag);
	if (err != 0)
		return (NULL);

	err = bus_dmamem_alloc(mem->tag, &mem->va,
	    BUS_DMA_NOWAIT | BUS_DMA_ZERO | BUS_DMA_COHERENT, &mem->map);
	if (err != 0)
		goto free_tag;

	mem->pa = 0;
	err = bus_dmamap_load(mem->tag, mem->map, mem->va, sz,
	    idpf_dma_map_cb, &mem->pa, BUS_DMA_NOWAIT);
	if (err != 0 || mem->pa == 0)
		goto free_mem;

	mem->size = sz;

	return (mem->va);

free_mem:
	bus_dmamem_free(mem->tag, mem->va, mem->map);
	mem->va = NULL;
free_tag:
	bus_dma_tag_destroy(mem->tag);
	mem->tag = NULL;

	return (NULL);
}

/**
 * idpf_free_dma_mem - release coherent DMA memory
 * @hw: hardware struct
 * @mem: descriptor to release
 */
void
idpf_free_dma_mem(struct idpf_hw *hw, struct idpf_dma_mem *mem)
{

	if (mem->va == NULL)
		return;

	bus_dmamap_unload(mem->tag, mem->map);
	bus_dmamem_free(mem->tag, mem->va, mem->map);
	bus_dma_tag_destroy(mem->tag);

	mem->tag = NULL;
	mem->map = NULL;
	mem->size = 0;
	mem->va = NULL;
	mem->pa = 0;
}

/* ---------------------------------------------------------------------
 * MAC filters
 * --------------------------------------------------------------------- */

/**
 * idpf_find_mac_filter - search the filter list for a MAC address
 * @vconfig: vport configuration holding the list
 * @macaddr: address to look for
 *
 * Caller must hold mac_filter_list_lock.
 *
 * Return: the filter, or NULL.
 */
static struct idpf_mac_filter *
idpf_find_mac_filter(struct idpf_vport_config *vconfig, const uint8_t *macaddr)
{
	struct idpf_mac_filter *f;

	if (macaddr == NULL)
		return (NULL);

	TAILQ_FOREACH(f, &vconfig->user_config.mac_filter_list, list) {
		if (memcmp(macaddr, f->macaddr, ETHER_ADDR_LEN) == 0)
			return (f);
	}

	return (NULL);
}

/**
 * __idpf_del_mac_filter - drop a MAC filter from the software list
 * @vport_config: vport configuration holding the list
 * @macaddr: address to remove
 *
 * Return: 0.
 */
static int
__idpf_del_mac_filter(struct idpf_vport_config *vport_config,
    const uint8_t *macaddr)
{
	struct idpf_mac_filter *f;

	mtx_lock(&vport_config->mac_filter_list_lock);
	f = idpf_find_mac_filter(vport_config, macaddr);
	if (f != NULL) {
		TAILQ_REMOVE(&vport_config->user_config.mac_filter_list, f,
		    list);
		free(f, M_DEVBUF);
	}
	mtx_unlock(&vport_config->mac_filter_list_lock);

	return (0);
}

/**
 * idpf_del_mac_filter - remove a MAC filter from the list and the device
 * @vport: vport owning the filter
 * @np: per-vport private data
 * @macaddr: address to remove
 * @async: true to send without waiting for the reply
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_del_mac_filter(struct idpf_vport *vport, struct idpf_netdev_priv *np,
    const uint8_t *macaddr, bool async)
{
	struct idpf_vport_config *vport_config;
	struct idpf_mac_filter *f;

	vport_config = np->adapter->vport_config[np->vport_idx];

	mtx_lock(&vport_config->mac_filter_list_lock);
	f = idpf_find_mac_filter(vport_config, macaddr);
	if (f == NULL) {
		mtx_unlock(&vport_config->mac_filter_list_lock);
		return (EINVAL);
	}
	f->remove = true;
	mtx_unlock(&vport_config->mac_filter_list_lock);

	if ((np->state & (1u << IDPF_VPORT_UP)) != 0) {
		int err;

		err = idpf_add_del_mac_filters(np->adapter, vport_config,
		    vport->default_mac_addr, np->vport_id, false, async);
		if (err != 0)
			return (err);
	}

	return (__idpf_del_mac_filter(vport_config, macaddr));
}

/**
 * __idpf_add_mac_filter - add a MAC filter to the software list
 * @vport_config: vport configuration holding the list
 * @macaddr: address to add
 *
 * Return: 0 on success, ENOMEM on allocation failure.
 */
static int
__idpf_add_mac_filter(struct idpf_vport_config *vport_config,
    const uint8_t *macaddr)
{
	struct idpf_mac_filter *f;

	mtx_lock(&vport_config->mac_filter_list_lock);

	f = idpf_find_mac_filter(vport_config, macaddr);
	if (f != NULL) {
		f->remove = false;
		mtx_unlock(&vport_config->mac_filter_list_lock);
		return (0);
	}

	f = malloc(sizeof(*f), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (f == NULL) {
		mtx_unlock(&vport_config->mac_filter_list_lock);
		return (ENOMEM);
	}

	memcpy(f->macaddr, macaddr, ETHER_ADDR_LEN);
	f->add = true;
	TAILQ_INSERT_TAIL(&vport_config->user_config.mac_filter_list, f, list);

	mtx_unlock(&vport_config->mac_filter_list_lock);

	return (0);
}

/**
 * idpf_add_mac_filter - add a MAC filter to the list and the device
 * @vport: vport owning the filter
 * @np: per-vport private data
 * @macaddr: address to add
 * @async: true to send without waiting for the reply
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_add_mac_filter(struct idpf_vport *vport, struct idpf_netdev_priv *np,
    const uint8_t *macaddr, bool async)
{
	struct idpf_vport_config *vport_config;
	int err;

	if (np->adapter == NULL || np->adapter->vport_config == NULL ||
	    np->vport_idx >= np->adapter->max_vports ||
	    np->adapter->vport_config[np->vport_idx] == NULL) {
		printf("idpf: mac filter with no vport config "
		    "(adapter=%p cfgs=%p idx=%u max=%u)\n",
		    (void *)np->adapter,
		    np->adapter == NULL ? NULL :
		    (void *)np->adapter->vport_config,
		    np->vport_idx,
		    np->adapter == NULL ? 0 : np->adapter->max_vports);
		return (ENXIO);
	}

	vport_config = np->adapter->vport_config[np->vport_idx];
	err = __idpf_add_mac_filter(vport_config, macaddr);
	if (err != 0)
		return (err);

	if ((np->state & (1u << IDPF_VPORT_UP)) != 0)
		err = idpf_add_del_mac_filters(np->adapter, vport_config,
		    vport->default_mac_addr, np->vport_id, true, async);

	return (err);
}

/**
 * idpf_del_all_mac_filters - drop every MAC filter from the list
 * @vport: vport owning the filters
 */
static void
idpf_del_all_mac_filters(struct idpf_vport *vport)
{
	struct idpf_vport_config *vport_config;
	struct idpf_mac_filter *f, *ftmp;

	vport_config = vport->adapter->vport_config[vport->idx];

	mtx_lock(&vport_config->mac_filter_list_lock);
	TAILQ_FOREACH_SAFE(f, &vport_config->user_config.mac_filter_list, list,
	    ftmp) {
		TAILQ_REMOVE(&vport_config->user_config.mac_filter_list, f,
		    list);
		free(f, M_DEVBUF);
	}
	mtx_unlock(&vport_config->mac_filter_list_lock);
}

/**
 * idpf_restore_mac_filters - re-apply every MAC filter to the device
 * @vport: vport owning the filters
 */
static void
idpf_restore_mac_filters(struct idpf_vport *vport)
{
	struct idpf_vport_config *vport_config;
	struct idpf_mac_filter *f;

	vport_config = vport->adapter->vport_config[vport->idx];

	mtx_lock(&vport_config->mac_filter_list_lock);
	TAILQ_FOREACH(f, &vport_config->user_config.mac_filter_list, list)
		f->add = true;
	mtx_unlock(&vport_config->mac_filter_list_lock);

	idpf_add_del_mac_filters(vport->adapter, vport_config,
	    vport->default_mac_addr, vport->vport_id, true, false);
}

/**
 * idpf_remove_mac_filters - withdraw every MAC filter from the device
 * @vport: vport owning the filters
 */
static void
idpf_remove_mac_filters(struct idpf_vport *vport)
{
	struct idpf_vport_config *vport_config;
	struct idpf_mac_filter *f;

	vport_config = vport->adapter->vport_config[vport->idx];

	mtx_lock(&vport_config->mac_filter_list_lock);
	TAILQ_FOREACH(f, &vport_config->user_config.mac_filter_list, list)
		f->remove = true;
	mtx_unlock(&vport_config->mac_filter_list_lock);

	idpf_add_del_mac_filters(vport->adapter, vport_config,
	    vport->default_mac_addr, vport->vport_id, false, false);
}

/**
 * idpf_deinit_mac_addr - drop the vport's primary address filter
 * @vport: vport being torn down
 */
static void
idpf_deinit_mac_addr(struct idpf_vport *vport)
{
	struct idpf_vport_config *vport_config;
	struct idpf_mac_filter *f;

	vport_config = vport->adapter->vport_config[vport->idx];

	mtx_lock(&vport_config->mac_filter_list_lock);
	f = idpf_find_mac_filter(vport_config, vport->default_mac_addr);
	if (f != NULL) {
		TAILQ_REMOVE(&vport_config->user_config.mac_filter_list, f,
		    list);
		free(f, M_DEVBUF);
	}
	mtx_unlock(&vport_config->mac_filter_list_lock);
}

/**
 * idpf_init_mac_addr - install the vport's primary address
 * @vport: vport being brought up
 * @np: per-vport private data
 *
 * A random address is generated when the control plane did not supply one,
 * which requires the MAC filter capability.
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_init_mac_addr(struct idpf_vport *vport, struct idpf_netdev_priv *np)
{
	struct idpf_adapter *adapter = vport->adapter;
	int err;

	idpf_dbg(idpf_adapter_to_dev(adapter),
	    "mac: %02x:%02x:%02x:%02x:%02x:%02x valid=%d cfg=%p\n",
	    vport->default_mac_addr[0], vport->default_mac_addr[1],
	    vport->default_mac_addr[2], vport->default_mac_addr[3],
	    vport->default_mac_addr[4], vport->default_mac_addr[5],
	    idpf_is_valid_ether_addr(vport->default_mac_addr),
	    (void *)(np->adapter == NULL ? NULL :
	    np->adapter->vport_config[np->vport_idx]));

	if (idpf_is_valid_ether_addr(vport->default_mac_addr))
		return (idpf_add_mac_filter(vport, np,
		    vport->default_mac_addr, false));

	idpf_dbg(idpf_adapter_to_dev(adapter), "mac: generating\n");

	if (!idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS,
	    VIRTCHNL2_CAP_MACFILTER)) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "MAC address is not provided and capability is not set\n");
		return (EINVAL);
	}

	ether_gen_addr_byname(device_get_nameunit(idpf_adapter_to_dev(adapter)),
	    (struct ether_addr *)vport->default_mac_addr);

	err = idpf_add_mac_filter(vport, np, vport->default_mac_addr, false);
	if (err != 0)
		return (err);

	device_printf(idpf_adapter_to_dev(adapter),
	    "no MAC address provided, using generated %02x:%02x:%02x:%02x:%02x:%02x\n",
	    vport->default_mac_addr[0], vport->default_mac_addr[1],
	    vport->default_mac_addr[2], vport->default_mac_addr[3],
	    vport->default_mac_addr[4], vport->default_mac_addr[5]);

	return (0);
}

/* ---------------------------------------------------------------------
 * Vport lifecycle
 * --------------------------------------------------------------------- */

/**
 * idpf_get_free_slot - find the next free vport slot
 * @adapter: driver private data
 *
 * Return: the slot index, or IDPF_NO_FREE_SLOT.
 */
static int
idpf_get_free_slot(struct idpf_adapter *adapter)
{
	unsigned int i;

	for (i = 0; i < adapter->max_vports; i++) {
		if (adapter->vports[i] == NULL)
			return (i);
	}

	return (IDPF_NO_FREE_SLOT);
}

/**
 * idpf_remove_features - turn off the features a vport negotiated
 * @vport: vport being taken down
 */
static void
idpf_remove_features(struct idpf_vport *vport)
{
	struct idpf_adapter *adapter = vport->adapter;

	if (idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_MACFILTER))
		idpf_remove_mac_filters(vport);
}

/**
 * idpf_restore_features - re-apply the features a vport negotiated
 * @vport: vport being brought up
 */
static void
idpf_restore_features(struct idpf_vport *vport)
{
	struct idpf_adapter *adapter = vport->adapter;

	if (idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_MACFILTER))
		idpf_restore_mac_filters(vport);

	/*
	 * RX timestamp restore needs the hwtstamp filter the vport was last
	 * configured with, which this port does not carry: PTP is not built
	 * and idpf_ptp_is_vport_rx_tstamp_ena() is a stub returning false.
	 */
}

/**
 * idpf_vport_set_hsplit - enable or disable header split on a vport
 * @vport: vport to configure
 * @ena: true to enable
 *
 * Header split needs both the capability and the split queue model.
 */
void
idpf_vport_set_hsplit(struct idpf_vport *vport, bool ena)
{
	struct idpf_vport_user_config_data *config_data;

	config_data = &vport->adapter->vport_config[vport->idx]->user_config;

	if (!ena) {
		config_data->user_flags &= ~(1ULL << __IDPF_PRIV_FLAGS_HDR_SPLIT);
		return;
	}

	if (idpf_is_cap_ena_all(vport->adapter, IDPF_HSPLIT_CAPS,
	    IDPF_CAP_HSPLIT) &&
	    idpf_is_queue_model_split(vport->dflt_qv_rsrc.rxq_model))
		config_data->user_flags |= (1ULL << __IDPF_PRIV_FLAGS_HDR_SPLIT);
}

/**
 * idpf_rx_init_buf_tail - publish the initial buffer ring tail values
 * @rsrc: queue and vector resources
 */
static void
idpf_rx_init_buf_tail(struct idpf_q_vec_rsrc *rsrc)
{
	unsigned int i, j;

	for (i = 0; i < rsrc->num_rxq_grp; i++) {
		struct idpf_rxq_group *grp = &rsrc->rxq_grps[i];

		if (idpf_is_queue_model_split(rsrc->rxq_model)) {
			for (j = 0; j < rsrc->num_bufqs_per_qgrp; j++) {
				struct idpf_queue *q =
				    &grp->splitq.bufq_sets[j].bufq;

				idpf_reg_wr32(q->tail, q->next_to_alloc);
			}
		} else {
			for (j = 0; j < grp->singleq.num_rxq; j++) {
				struct idpf_queue *q = grp->singleq.rxqs[j];

				idpf_reg_wr32(q->tail, q->next_to_alloc);
			}
		}
	}
}

/**
 * idpf_up_complete - finish bringing an interface up
 * @vport: vport being brought up
 *
 * Return: 0.
 */
static int
idpf_up_complete(struct idpf_vport *vport)
{
	struct idpf_netdev_priv *np = iflib_get_softc(vport->ctx);

	if (vport->link_up)
		iflib_link_state_change(vport->ctx, LINK_STATE_UP,
		    IF_Mbps(np->link_speed_mbps));

	np->state |= (1u << IDPF_VPORT_UP);

	return (0);
}

/**
 * idpf_vport_stop - disable a vport
 * @vport: vport to disable
 */
static void
idpf_vport_stop(struct idpf_vport *vport)
{
	struct idpf_netdev_priv *np = iflib_get_softc(vport->ctx);
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	struct idpf_queue_id_reg_info *chunks;
	uint32_t vport_id = vport->vport_id;

	if ((np->state & (1u << IDPF_VPORT_UP)) == 0)
		return;
	np->state &= ~(1u << IDPF_VPORT_UP);

	iflib_link_state_change(vport->ctx, LINK_STATE_DOWN, 0);

	chunks = &adapter->vport_config[vport->idx]->qid_reg_info;

	if ((adapter->flags & (1u << IDPF_CORER_IN_PROG)) == 0) {
		idpf_send_disable_vport_msg(adapter, vport_id);
		idpf_send_disable_queues_msg(adapter, vport, rsrc, chunks);
	}
	idpf_send_map_unmap_queue_vector_msg(adapter, rsrc, vport_id, false);

	/*
	 * Queues are normally requested once, in create_vport; they are only
	 * deleted here when the requested count changed underneath us.
	 */
	if ((vport->flags & (1u << IDPF_VPORT_DEL_QUEUES)) != 0) {
		vport->flags &= ~(1u << IDPF_VPORT_DEL_QUEUES);
		idpf_send_delete_queues_msg(adapter, chunks, vport_id);
	}

	idpf_remove_features(vport);

	/*
	 * Only the hardware side is torn down here.  iflib hands the descriptor
	 * rings to the driver once, through ifdi_rx_queues_alloc(), and assigns
	 * the queue interrupts once, through ifdi_msix_intr_assign(); releasing
	 * either on stop would leave the next init refilling a freed ring.
	 * They are released in ifdi_queues_free() and idpf_vport_rel().
	 */
	idpf_vport_intr_deinit(vport, rsrc);
}

/**
 * idpf_vport_open - bring a vport up
 * @vport: vport to bring up
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_vport_open(struct idpf_vport *vport)
{
	struct idpf_netdev_priv *np = iflib_get_softc(vport->ctx);
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	device_t dev = idpf_adapter_to_dev(adapter);
	struct idpf_vport_config *vport_config;
	struct idpf_queue_id_reg_info *chunks;
	struct idpf_rss_data *rss_data;
	uint32_t vport_id = vport->vport_id;
	int err;

	if ((np->state & (1u << IDPF_VPORT_UP)) != 0)
		return (EBUSY);

	iflib_link_state_change(vport->ctx, LINK_STATE_DOWN, 0);

	err = idpf_vport_intr_alloc(vport, rsrc);
	if (err != 0) {
		device_printf(dev,
		    "failed to allocate interrupts for vport %u: %d\n",
		    vport_id, err);
		return (err);
	}

	err = idpf_vport_queue_alloc_all(vport, rsrc);
	if (err != 0)
		goto intr_rel;

	vport_config = adapter->vport_config[vport->idx];
	chunks = &vport_config->qid_reg_info;

	err = idpf_vport_queue_ids_init(rsrc, chunks);
	if (err != 0) {
		device_printf(dev,
		    "failed to initialize queue ids for vport %u: %d\n",
		    vport_id, err);
		goto queues_rel;
	}

	err = idpf_vport_intr_init(vport, rsrc);
	if (err != 0) {
		device_printf(dev,
		    "failed to initialize interrupts for vport %u: %d\n",
		    vport_id, err);
		goto queues_rel;
	}

	err = idpf_queue_reg_init(vport, rsrc, chunks);
	if (err != 0) {
		device_printf(dev,
		    "failed to initialize queue registers for vport %u: %d\n",
		    vport_id, err);
		goto intr_deinit;
	}

	/*
	 * iflib has already allocated and mapped every RX buffer and written
	 * the free-list tails, so only the ring tail registers are published
	 * here.  The Linux idpf_rx_bufs_init_all() step has no counterpart.
	 */
	idpf_rx_init_buf_tail(rsrc);

	idpf_vport_intr_ena(vport, rsrc);

	err = idpf_send_config_queues_msg(adapter, rsrc, vport_id);
	if (err != 0) {
		device_printf(dev,
		    "failed to configure queues for vport %u: %d\n",
		    vport_id, err);
		goto intr_deinit;
	}

	err = idpf_send_map_unmap_queue_vector_msg(adapter, rsrc, vport_id,
	    true);
	if (err != 0) {
		device_printf(dev,
		    "failed to map queue vectors for vport %u: %d\n",
		    vport_id, err);
		goto intr_deinit;
	}

	err = idpf_send_enable_queues_msg(adapter, vport_id, chunks);
	if (err != 0) {
		device_printf(dev,
		    "failed to enable queues for vport %u: %d\n",
		    vport_id, err);
		goto unmap_queue_vectors;
	}

	err = idpf_send_enable_vport_msg(adapter, vport_id);
	if (err != 0) {
		device_printf(dev, "failed to enable vport %u: %d\n",
		    vport_id, err);
		err = EAGAIN;
		goto disable_queues;
	}

	idpf_restore_features(vport);

	rss_data = &vport_config->user_config.rss_data;
	if (rss_data->rss_lut != NULL)
		err = idpf_config_rss(vport, rss_data);
	else
		err = idpf_init_rss(vport, rss_data, rsrc);
	if (err != 0) {
		device_printf(dev,
		    "failed to initialize RSS for vport %u: %d\n",
		    vport_id, err);
		goto disable_vport;
	}

	err = idpf_up_complete(vport);
	if (err != 0) {
		device_printf(dev,
		    "failed to complete interface up for vport %u: %d\n",
		    vport_id, err);
		goto deinit_rss;
	}

	return (0);

deinit_rss:
	idpf_deinit_rss(rss_data);
disable_vport:
	idpf_send_disable_vport_msg(adapter, vport_id);
disable_queues:
	idpf_send_disable_queues_msg(adapter, vport, rsrc, chunks);
unmap_queue_vectors:
	idpf_send_map_unmap_queue_vector_msg(adapter, rsrc, vport_id, false);
intr_deinit:
	idpf_vport_intr_deinit(vport, rsrc);
queues_rel:
	idpf_vport_queues_rel(vport, rsrc);
intr_rel:
	idpf_vport_intr_rel(rsrc);

	return (err);
}

/**
 * idpf_vport_rel - destroy a vport and free its resources
 * @vport: vport being removed
 */
static void
idpf_vport_rel(struct idpf_vport *vport)
{
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	struct idpf_vport_config *vport_config;
	struct idpf_rss_data *rss_data;
	struct idpf_vport_max_q max_q;
	uint16_t idx = vport->idx;

	vport_config = adapter->vport_config[idx];
	rss_data = &vport_config->user_config.rss_data;
	idpf_deinit_rss(rss_data);
	free(rss_data->rss_key, M_DEVBUF);
	rss_data->rss_key = NULL;

	idpf_send_destroy_vport_msg(adapter, vport->vport_id);

	idpf_ptp_release_vport_tstamps_caps(vport);

	/* Return the queue budget to the adapter's pool. */
	max_q.max_rxq = vport_config->max_q.max_rxq;
	max_q.max_txq = vport_config->max_q.max_txq;
	max_q.max_bufq = vport_config->max_q.max_bufq;
	max_q.max_complq = vport_config->max_q.max_complq;
	idpf_vport_dealloc_max_qs(adapter, &max_q);

	idpf_vport_dealloc_vec_indexes(vport, rsrc);

	/* Vectors are allocated in attach_pre, so release them even if the
	 * interface was never brought up and vport_stop() never ran. */
	idpf_vport_intr_rel(rsrc);

	idpf_vport_deinit_queue_reg_chunks(vport_config);

	free(adapter->vport_params_recvd[idx], M_DEVBUF);
	adapter->vport_params_recvd[idx] = NULL;

	free(vport, M_DEVBUF);
	adapter->num_alloc_vports--;
}

/**
 * idpf_del_user_cfg_data - drop the user configuration a vport accumulated
 * @vport: vport being removed
 */
static void
idpf_del_user_cfg_data(struct idpf_vport *vport)
{

	idpf_del_all_mac_filters(vport);
}

/**
 * idpf_vport_dealloc - tear a vport down and release it
 * @vport: vport to release
 */
void
idpf_vport_dealloc(struct idpf_vport *vport)
{
	struct idpf_adapter *adapter = vport->adapter;
	unsigned int i = vport->idx;

	adapter->vports[i] = NULL;

	idpf_deinit_mac_addr(vport);

	if ((adapter->flags & (1u << IDPF_HR_RESET_IN_PROG)) == 0)
		idpf_vport_stop(vport);

	if ((adapter->flags & (1u << IDPF_REMOVE_IN_PROG)) != 0)
		idpf_del_user_cfg_data(vport);

	if (adapter->iflib_ctxs[i] != NULL) {
		struct idpf_netdev_priv *np;

		np = iflib_get_softc(adapter->iflib_ctxs[i]);
		np->vport = NULL;
	}

	idpf_vport_rel(vport);

	adapter->next_vport = idpf_get_free_slot(adapter);
}

/**
 * idpf_vport_alloc - allocate the next free vport
 * @adapter: driver private data
 * @max_q: queue budget for the new vport
 *
 * Return: the new vport, or NULL.
 */
static struct idpf_vport *
idpf_vport_alloc(struct idpf_adapter *adapter, struct idpf_vport_max_q *max_q)
{
	struct idpf_rss_data *rss_data;
	struct idpf_q_vec_rsrc *rsrc;
	uint16_t idx = adapter->next_vport;
	struct idpf_vport *vport;
	uint16_t num_max_q;
	int i, err;

	if (idx == IDPF_NO_FREE_SLOT) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "vport alloc: no free slot (alloc %u of max %u)\n",
		    adapter->num_alloc_vports, adapter->max_vports);
		return (NULL);
	}

	vport = malloc(sizeof(*vport), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (vport == NULL)
		return (NULL);

	num_max_q = max(max_q->max_txq, max_q->max_rxq);
	if (adapter->vport_config[idx] == NULL) {
		struct idpf_vport_config *vport_config;
		struct idpf_q_coalesce *q_coal;

		vport_config = malloc(sizeof(*vport_config), M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (vport_config == NULL) {
			free(vport, M_DEVBUF);
			return (NULL);
		}

		q_coal = malloc(num_max_q * sizeof(*q_coal), M_DEVBUF,
		    M_NOWAIT | M_ZERO);
		if (q_coal == NULL) {
			free(vport_config, M_DEVBUF);
			free(vport, M_DEVBUF);
			return (NULL);
		}
		for (i = 0; i < num_max_q; i++) {
			q_coal[i].tx_intr_mode = IDPF_ITR_DYNAMIC;
			q_coal[i].tx_coalesce_usecs = IDPF_ITR_TX_DEF;
			q_coal[i].rx_intr_mode = IDPF_ITR_DYNAMIC;
			q_coal[i].rx_coalesce_usecs = IDPF_ITR_RX_DEF;
		}
		vport_config->user_config.q_coalesce = q_coal;

		mtx_init(&vport_config->mac_filter_list_lock, "idpf_macflt",
		    NULL, MTX_DEF);
		mtx_init(&vport_config->flow_steer_list_lock, "idpf_fsteer",
		    NULL, MTX_DEF);
		TAILQ_INIT(&vport_config->user_config.mac_filter_list);

		adapter->vport_config[idx] = vport_config;
	}

	vport->idx = idx;
	vport->adapter = adapter;
	/* Every vport is driven through an iflib interface; without this the
	 * ifp helpers dereference NULL. */
	vport->ctx = adapter->iflib_ctxs != NULL ?
	    adapter->iflib_ctxs[idx] : NULL;
	vport->compln_clean_budget = IDPF_TX_COMPLQ_CLEAN_BUDGET;
	vport->default_vport = adapter->num_alloc_vports <
	    idpf_get_default_vports(adapter);

	mtx_init(&vport->sw_marker_lock, "idpf_swmark", NULL, MTX_DEF);
	cv_init(&vport->sw_marker_cv, "idpf_swmark");

	rsrc = &vport->dflt_qv_rsrc;
	rsrc->dev = idpf_adapter_to_dev(adapter);
	rsrc->q_vector_idxs = malloc(num_max_q * sizeof(uint16_t), M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (rsrc->q_vector_idxs == NULL)
		goto free_vport;

	err = idpf_vport_init(vport, max_q);
	if (err != 0) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "vport alloc: idpf_vport_init failed: %d\n", err);
		goto free_vector_idxs;
	}

	/*
	 * The key is allocated separately from the LUT: a queue-count change
	 * needs a new LUT but the key can live as long as the vport does.
	 */
	rss_data = &adapter->vport_config[idx]->user_config.rss_data;
	rss_data->rss_key = malloc(rss_data->rss_key_size, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (rss_data->rss_key == NULL)
		goto free_qreg_chunks;

	arc4random_buf(rss_data->rss_key, rss_data->rss_key_size);

	adapter->vports[idx] = vport;
	adapter->vport_ids[idx] = vport->vport_id;

	adapter->num_alloc_vports++;
	adapter->next_vport = idpf_get_free_slot(adapter);

	return (vport);

free_qreg_chunks:
	idpf_vport_deinit_queue_reg_chunks(adapter->vport_config[idx]);
free_vector_idxs:
	free(rsrc->q_vector_idxs, M_DEVBUF);
	rsrc->q_vector_idxs = NULL;
free_vport:
	cv_destroy(&vport->sw_marker_cv);
	mtx_destroy(&vport->sw_marker_lock);
	free(vport, M_DEVBUF);

	return (NULL);
}

/* ---------------------------------------------------------------------
 * Interface attach and detach across a reset
 * --------------------------------------------------------------------- */

/**
 * idpf_detach_and_close - stop every running interface before a reset
 * @adapter: driver private data
 *
 * IDPF_VPORT_UP_REQUESTED records which interfaces were running so that
 * idpf_attach_and_open() can bring exactly those back.
 */
void
idpf_detach_and_close(struct idpf_adapter *adapter)
{
	int max_vports = adapter->max_vports;

	for (int i = 0; i < max_vports; i++) {
		struct idpf_vport *vport = adapter->vports[i];
		struct idpf_netdev_priv *np;

		if (vport == NULL || vport->ctx == NULL)
			continue;

		np = iflib_get_softc(vport->ctx);
		if ((np->state & (1u << IDPF_VPORT_UP)) == 0)
			continue;

		adapter->vport_config[i]->flags |=
		    (1u << IDPF_VPORT_UP_REQUESTED);
		iflib_request_reset(vport->ctx);
		idpf_vport_stop(vport);
	}
}

/**
 * idpf_attach_and_open - restore the interfaces a reset took down
 * @adapter: driver private data
 */
void
idpf_attach_and_open(struct idpf_adapter *adapter)
{
	int max_vports = adapter->max_vports;

	for (int i = 0; i < max_vports; i++) {
		struct idpf_vport *vport = adapter->vports[i];
		struct idpf_vport_config *vport_config;

		/*
		 * A critical error in the init task frees the vport; only
		 * restore the ones that survived.
		 */
		if (vport == NULL)
			continue;

		vport_config = adapter->vport_config[vport->idx];
		if ((vport_config->flags &
		    (1u << IDPF_VPORT_UP_REQUESTED)) == 0)
			continue;

		vport_config->flags &= ~(1u << IDPF_VPORT_UP_REQUESTED);
		idpf_vport_open(vport);
	}
}

/* ---------------------------------------------------------------------
 * Deferred tasks
 * --------------------------------------------------------------------- */

/**
 * idpf_statistics_task - periodically refresh the per-vport counters
 * @arg: adapter
 * @pending: taskqueue pending count
 */
void
idpf_statistics_task(void *arg, int pending __unused)
{
	struct idpf_adapter *adapter = arg;
	int i;

	for (i = 0; i < adapter->max_vports; i++) {
		struct idpf_vport *vport = adapter->vports[i];

		if (vport == NULL || vport->ctx == NULL)
			continue;

		idpf_send_get_stats_msg(iflib_get_softc(vport->ctx),
		    &vport->port_stats);
	}

	/* Do not re-arm on the teardown path. */
	if (idpf_is_resource_rel_in_prog(adapter))
		return;

	callout_reset(&adapter->stats_task, idpf_msecs_to_ticks(1000),
	    idpf_statistics_task_cb, adapter);
}

/**
 * idpf_statistics_task_cb - callout trampoline for the statistics task
 * @arg: adapter
 *
 * The statistics task sleeps on the mailbox, so the callout only hands the
 * work to a taskqueue thread.
 */
void
idpf_statistics_task_cb(void *arg)
{
	struct idpf_adapter *adapter = arg;

	taskqueue_enqueue(adapter->stats_wq, &adapter->stats_deferred);
}

/**
 * idpf_stats_task_stop - cancel the statistics task
 * @adapter: driver private data
 */
void
idpf_stats_task_stop(struct idpf_adapter *adapter)
{

	if (!IS_SILICON_DEVICE(adapter->hw.subsystem_device_id))
		return;

	callout_drain(&adapter->stats_task);
	taskqueue_drain(adapter->stats_wq, &adapter->stats_deferred);
}

/**
 * idpf_stats_task_start - start the statistics task
 * @adapter: driver private data
 */
void
idpf_stats_task_start(struct idpf_adapter *adapter)
{

	if (!IS_SILICON_DEVICE(adapter->hw.subsystem_device_id))
		return;

	if (idpf_is_resource_rel_in_prog(adapter))
		return;

	callout_reset(&adapter->stats_task, 1, idpf_statistics_task_cb,
	    adapter);
}

/**
 * idpf_mbx_task - drain the mailbox receive queue
 * @arg: adapter
 * @pending: taskqueue pending count
 */
void
idpf_mbx_task(void *arg, int pending __unused)
{
	struct idpf_adapter *adapter = arg;
	struct idpf_ctlq_info *arq;

	/* Bail if the mailbox is down. */
	arq = adapter->hw.arq;
	if (arq == NULL)
		return;

	/* Wake anyone waiting on a CORER that the filter observed finishing. */
	if (atomic_load_acq_int(&adapter->corer_done_flag) != 0) {
		mtx_lock(&adapter->corer_done_lock);
		cv_broadcast(&adapter->corer_done_cv);
		mtx_unlock(&adapter->corer_done_lock);
	}

	if ((adapter->flags & (1u << IDPF_MB_INTR_MODE)) != 0)
		idpf_mb_irq_enable(adapter);
	else
		callout_reset(&adapter->mbx_poll_task,
		    idpf_msecs_to_ticks(300), idpf_mbx_task_cb, adapter);

	idpf_recv_mb_msg(adapter, arq);
}

/**
 * idpf_mbx_task_cb - callout trampoline for the mailbox poll
 * @arg: adapter
 */
void
idpf_mbx_task_cb(void *arg)
{
	struct idpf_adapter *adapter = arg;

	taskqueue_enqueue(adapter->mbx_wq, &adapter->mbx_task);
}

/**
 * idpf_service_task - watch for a reset asserted by the device
 * @arg: adapter
 */
void
idpf_service_task(void *arg)
{
	struct idpf_adapter *adapter = arg;

	if (idpf_is_reset_detected(adapter) &&
	    !idpf_is_reset_in_prog(adapter) &&
	    (adapter->flags & (1u << IDPF_REMOVE_IN_PROG)) == 0) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "%s reset detected\n",
		    (adapter->flags & (1u << IDPF_CORER_IN_PROG)) != 0 ?
		    "CORER" : "HW");

		adapter->flags |= (1u << IDPF_HR_FUNC_RESET);
		taskqueue_enqueue_timeout(adapter->vc_event_wq,
		    &adapter->vc_event_task, idpf_msecs_to_ticks(10));

		return;
	}

	callout_reset(&adapter->serv_task, idpf_msecs_to_ticks(300),
	    idpf_service_task, adapter);
}

/**
 * idpf_init_task - finish the bring-up that probe deferred
 * @arg: adapter
 * @pending: taskqueue pending count
 *
 * The control plane can take milliseconds to answer, so vport creation runs
 * here rather than blocking attach.
 */
void
idpf_init_task(void *arg, int pending __unused)
{
	struct idpf_adapter *adapter = arg;
	device_t dev = idpf_adapter_to_dev(adapter);
	struct idpf_vport *vport = NULL;
	struct idpf_vport_max_q max_q;
	uint16_t num_default_vports;
	bool default_vport;
	int index, err;

	idpf_dbg(dev, "init_task: entered (alloc %u)\n",
	    adapter->num_alloc_vports);

	num_default_vports = idpf_get_default_vports(adapter);
	default_vport = adapter->num_alloc_vports < num_default_vports;

	err = idpf_vport_alloc_max_qs(adapter, &max_q);
	if (err != 0)
		goto unwind_vports;

	idpf_dbg(dev, "init_task: creating vport\n");
	err = idpf_send_create_vport_msg(adapter, &max_q);
	if (err != 0) {
		idpf_vport_dealloc_max_qs(adapter, &max_q);
		goto unwind_vports;
	}

	vport = idpf_vport_alloc(adapter, &max_q);
	if (vport == NULL) {
		err = EFAULT;
		device_printf(dev, "failed to allocate vport: %d\n", err);
		idpf_vport_dealloc_max_qs(adapter, &max_q);
		goto unwind_vports;
	}

	err = idpf_check_supported_desc_ids(vport);
	if (err != 0) {
		device_printf(dev, "failed to get required descriptor ids\n");
		goto unwind_vports;
	}

	idpf_dbg(dev, "init_task: vport %u created, configuring\n",
	    vport->vport_id);
	err = idpf_vport_cfg_ifp(vport);
	if (err != 0)
		goto unwind_vports;

	/* Keep re-arming until every default vport exists. */
	if (adapter->num_alloc_vports < num_default_vports) {
		taskqueue_enqueue_timeout(adapter->init_wq,
		    &adapter->init_task,
		    idpf_msecs_to_ticks(5 * (pci_get_function(dev) & 0x07)));

		return;
	}

	/* All vports are created; the reset and load are done. */
	adapter->flags &= ~(1u << IDPF_HR_RESET_IN_PROG);
	adapter->flags &= ~(1u << IDPF_HR_DRV_LOAD);

	idpf_stats_task_start(adapter);

	return;

unwind_vports:
	if (default_vport) {
		for (index = 0; index < adapter->max_vports; index++) {
			if (adapter->vports[index] != NULL)
				idpf_vport_dealloc(adapter->vports[index]);
		}
	} else if (vport != NULL && adapter->vports[vport->idx] == vport) {
		idpf_vport_dealloc(vport);
	}

	/*
	 * idpf_vc_core_init() has no way of knowing that the init task failed
	 * on driver load, so clean up after it here.
	 */
	if ((adapter->flags & (1u << IDPF_HR_DRV_LOAD)) != 0) {
		adapter->flags &= ~(1u << IDPF_HR_DRV_LOAD);
		callout_drain(&adapter->serv_task);
		taskqueue_drain(adapter->mbx_wq, &adapter->mbx_task);
		idpf_ptp_release(adapter);
	} else if (default_vport) {
		idpf_ptp_release(adapter);
	}

	adapter->flags &= ~(1u << IDPF_HR_RESET_IN_PROG);
}

/**
 * idpf_deinit_task - release every vport
 * @adapter: driver private data
 *
 * Shared by detach and hard reset.
 */
void
idpf_deinit_task(struct idpf_adapter *adapter)
{
	unsigned int i;

	/*
	 * Wait for the init task first, otherwise it can race this thread and
	 * rebuild what is being torn down.
	 */
	taskqueue_drain_timeout(adapter->init_wq, &adapter->init_task);

	idpf_stats_task_stop(adapter);

	if (adapter->vports == NULL)
		return;

	for (i = 0; i < adapter->max_vports; i++) {
		if (adapter->vports[i] != NULL)
			idpf_vport_dealloc(adapter->vports[i]);
	}
}

/* ---------------------------------------------------------------------
 * Reset
 * --------------------------------------------------------------------- */

/**
 * idpf_check_reset_complete - wait for the device to leave reset
 * @adapter: driver private data
 *
 * Return: 0 when the device is usable, EBUSY otherwise.
 */
int
idpf_check_reset_complete(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	int i;

	/* A CORER must complete before anything else is attempted. */
	if ((adapter->flags & (1u << IDPF_CORER_IN_PROG)) != 0) {
		int rc = 0;

		mtx_lock(&adapter->corer_done_lock);
		while (atomic_load_acq_int(&adapter->corer_done_flag) == 0 &&
		    rc == 0)
			rc = cv_timedwait_sig(&adapter->corer_done_cv,
			    &adapter->corer_done_lock,
			    idpf_msecs_to_ticks(IDPF_CORER_TIMEOUT_MSEC));
		mtx_unlock(&adapter->corer_done_lock);

		if (rc != 0) {
			adapter->flags &= ~(1u << IDPF_CORER_IN_PROG);
			device_printf(dev, "waiting for CORER timed out\n");

			return (EBUSY);
		}
	}

	for (i = 0; i < IDPF_RESET_POLL_COUNT; i++) {
		uint32_t reg_val = idpf_reg_rd32(adapter->reset_reg.rstat);

		/* Do not keep the removal path waiting. */
		if ((adapter->flags & (1u << IDPF_REMOVE_IN_PROG)) != 0)
			return (EBUSY);

		/*
		 * 0xFFFFFFFF is read while the other side has not written the
		 * register yet, and is not a valid value for it.
		 */
		if (reg_val != 0xFFFFFFFF &&
		    (reg_val & adapter->reset_reg.rstat_m) != 0)
			return (0);

		if (IS_EMR_DEVICE(adapter->hw.subsystem_device_id))
			pause("idpfrst", idpf_msecs_to_ticks(4000));
		else
			DELAY(5000);
	}

	device_printf(dev, "device reset timeout\n");

	/* The reset is no longer in progress from the driver's point of view. */
	adapter->flags &= ~(1u << IDPF_HR_RESET_IN_PROG);

	return (EBUSY);
}

/**
 * idpf_wait_on_reset_detection - wait until the reset becomes visible
 * @adapter: driver private data
 *
 * Return: 0 once the reset is detected, EBUSY on timeout.
 */
static int
idpf_wait_on_reset_detection(struct idpf_adapter *adapter)
{
	uint16_t i;

	for (i = 0; i < IDPF_RESET_POLL_COUNT; i++) {
		if (idpf_is_reset_detected(adapter))
			return (0);

		if (IS_EMR_DEVICE(adapter->hw.subsystem_device_id))
			pause("idpfrst", idpf_msecs_to_ticks(4000));
		else
			DELAY(5000);
	}

	return (EBUSY);
}

/**
 * idpf_init_hard_reset - drive a hardware reset and rebuild everything
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_init_hard_reset(struct idpf_adapter *adapter)
{
	struct idpf_reg_ops *reg_ops = &adapter->dev_ops.reg_ops;
	device_t dev = idpf_adapter_to_dev(adapter);
	int err;

	idpf_detach_and_close(adapter);
	idpf_vport_ctrl_lock(adapter);

	device_printf(dev, "device HW reset initiated\n");

	/* A PCI-level reset already happened; go straight to recovery. */
	if ((adapter->flags & (1u << IDPF_PCI_CB_RESET)) != 0) {
		adapter->flags &= ~(1u << IDPF_PCI_CB_RESET);
		goto check_rst_complete;
	}

	if ((adapter->flags & (1u << IDPF_HR_DRV_LOAD)) != 0) {
		reg_ops->trigger_reset(adapter, IDPF_HR_DRV_LOAD);
	} else if ((adapter->flags & (1u << IDPF_HR_FUNC_RESET)) != 0) {
		if (!idpf_is_reset_detected(adapter)) {
			reg_ops->trigger_reset(adapter, IDPF_HR_FUNC_RESET);
			err = idpf_wait_on_reset_detection(adapter);
			if (err != 0) {
				device_printf(dev, "device failed to reset\n");
				goto unlock;
			}
		}
	} else {
		device_printf(dev, "unhandled hard reset cause\n");
		err = EINVAL;
		goto unlock;
	}

check_rst_complete:
	err = idpf_check_reset_complete(adapter);
	if (err != 0) {
		device_printf(dev,
		    "unable to contact the device firmware; check that it is "
		    "running. Driver state = 0x%x\n", adapter->state);
		goto unlock;
	}

	if ((adapter->flags & (1u << IDPF_HR_FUNC_RESET)) != 0) {
		/*
		 * Releasing the IRQs touches device registers, so it has to
		 * wait until the reset has actually completed.
		 */
		idpf_vc_core_deinit(adapter);
		idpf_deinit_dflt_mbx(adapter);
	}

	adapter->flags &= ~(1u << IDPF_HR_FUNC_RESET);

	err = idpf_reset_recover(adapter);

unlock:
	idpf_vport_ctrl_unlock(adapter);

	if (err == 0)
		idpf_attach_and_open(adapter);

	return (err);
}

/**
 * idpf_vc_event_task - handle a virtchnl event
 * @arg: adapter
 * @pending: taskqueue pending count
 */
void
idpf_vc_event_task(void *arg, int pending __unused)
{
	struct idpf_adapter *adapter = arg;

	if ((adapter->flags & (1u << IDPF_REMOVE_IN_PROG)) != 0)
		return;

	if ((adapter->flags & (1u << IDPF_HR_FUNC_RESET)) != 0)
		goto func_reset;

	if ((adapter->flags & (1u << IDPF_HR_DRV_LOAD)) != 0 ||
	    (adapter->flags & (1u << IDPF_PCI_CB_RESET)) != 0)
		goto drv_load;

	return;

func_reset:
	idpf_vc_xn_shutdown(adapter->vcxn_mngr);
drv_load:
	adapter->flags |= (1u << IDPF_HR_RESET_IN_PROG);
	idpf_init_hard_reset(adapter);
}

/**
 * idpf_initiate_soft_reset - reallocate a vport's queue resources
 * @vport: vport to reconfigure
 * @reset_cause: what triggered the reconfiguration
 *
 * The new resources are described in a clone of the vport so that a failure
 * leaves the running configuration untouched.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_initiate_soft_reset(struct idpf_vport *vport,
    enum idpf_vport_reset_cause reset_cause)
{
	struct idpf_netdev_priv *np = iflib_get_softc(vport->ctx);
	bool vport_is_up = (np->state & (1u << IDPF_VPORT_UP)) != 0;
	struct idpf_adapter *adapter = vport->adapter;
	struct idpf_vport_config *vport_config;
	struct idpf_q_vec_rsrc *new_rsrc;
	struct idpf_rss_data *rss_data;
	struct idpf_vport *new_vport;
	uint32_t vport_id = vport->vport_id;
	int err, tmp_err = 0;

	/*
	 * Allocating the new resources before releasing the old ones keeps a
	 * memory shortage from leaving the vport with neither.
	 */
	new_vport = malloc(sizeof(*new_vport), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (new_vport == NULL)
		return (ENOMEM);

	/*
	 * Copy only up to the synchronisation members: the clone must never
	 * own the condition variable or the mutex.
	 */
	memcpy(new_vport, vport, offsetof(struct idpf_vport, sw_marker_lock));

	new_rsrc = &new_vport->dflt_qv_rsrc;

	switch (reset_cause) {
	case IDPF_SR_Q_CHANGE:
		idpf_vport_adjust_qs(new_vport, new_rsrc);
		break;
	case IDPF_SR_Q_DESC_CHANGE:
		idpf_vport_calc_num_q_desc(new_vport, new_rsrc);
		break;
	case IDPF_SR_Q_SCH_CHANGE:
	case IDPF_SR_MTU_CHANGE:
	case IDPF_SR_RSC_CHANGE:
	case IDPF_SR_HSPLIT_CHANGE:
		break;
	default:
		device_printf(idpf_adapter_to_dev(adapter),
		    "unhandled soft reset cause\n");
		err = EINVAL;
		goto free_vport;
	}

	vport_config = adapter->vport_config[vport->idx];

	if (!vport_is_up) {
		idpf_send_delete_queues_msg(adapter,
		    &vport_config->qid_reg_info, vport_id);
	} else {
		vport->flags |= (1u << IDPF_VPORT_DEL_QUEUES);
		idpf_vport_stop(vport);
	}

	if (reset_cause == IDPF_SR_Q_CHANGE) {
		rss_data = &vport_config->user_config.rss_data;
		idpf_deinit_rss(rss_data);
	}

	/*
	 * vport is passed here rather than new_vport because the message needs
	 * the real synchronisation members; nothing below may change the vport
	 * configuration inside vport itself, as it is overwritten just after.
	 */
	err = idpf_send_add_queues_msg(adapter, vport_config, new_rsrc,
	    vport_id);
	if (err != 0)
		goto err_reset;

	memcpy(vport, new_vport, offsetof(struct idpf_vport, sw_marker_lock));

	if (reset_cause == IDPF_SR_Q_CHANGE)
		idpf_vport_alloc_vec_indexes(vport, &vport->dflt_qv_rsrc);

	if (vport_is_up)
		err = idpf_vport_open(vport);

	goto free_vport;

err_reset:
	tmp_err = idpf_send_add_queues_msg(adapter, vport_config,
	    &vport->dflt_qv_rsrc, vport_id);
	if (tmp_err == 0 && vport_is_up)
		idpf_vport_open(vport);

free_vport:
	free(new_vport, M_DEVBUF);

	return (err);
}

/* ---------------------------------------------------------------------
 * Interface configuration
 * --------------------------------------------------------------------- */

/**
 * idpf_get_vlan_caps - VLAN capabilities the device offers
 * @adapter: driver private data
 *
 * FreeBSD exposes hardware VLAN tagging as one capability covering both
 * directions, so it is only offered when the device can do both.
 *
 * Return: the IFCAP_* bits to advertise.
 */
int
idpf_get_vlan_caps(struct idpf_adapter *adapter)
{
	struct virtchnl2_vlan_supported_caps *insert, *strip;

	if (!idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_VLAN))
		return (0);

	strip = &adapter->vlan_caps.strip;
	insert = &adapter->vlan_caps.insert;

	if ((le32toh(strip->outer) & VIRTCHNL2_VLAN_ETHERTYPE_8100) == 0 ||
	    (le32toh(insert->outer) & VIRTCHNL2_VLAN_ETHERTYPE_8100) == 0)
		return (0);

	/* VLAN_MTU: a tagged frame is 4 bytes over the configured MTU. */
	return (IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_HWFILTER | IFCAP_VLAN_MTU);
}

/**
 * idpf_vport_cfg_ifp - publish a vport's capabilities on its interface
 * @vport: vport to configure
 *
 * iflib created the ifnet during attach; this fills in the offloads the
 * control plane granted and installs the primary MAC address.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_vport_cfg_ifp(struct idpf_vport *vport)
{
	struct idpf_adapter *adapter = vport->adapter;
	struct idpf_netdev_priv *np;
	if_t ifp = vport->ifp;
	int caps = 0;
	int err;

	if (vport->ctx == NULL) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "vport %u has no iflib context\n", vport->idx);
		return (ENXIO);
	}

	idpf_dbg(idpf_adapter_to_dev(adapter), "cfg_ifp: softc\n");
	np = iflib_get_softc(vport->ctx);
	np->vport = vport;
	np->vport_idx = vport->idx;
	np->vport_id = vport->vport_id;
	np->tx_max_bufs = idpf_get_max_tx_bufs(adapter);

	idpf_dbg(idpf_adapter_to_dev(adapter), "cfg_ifp: mac\n");
	err = idpf_init_mac_addr(vport, np);
	if (err != 0) {
		device_printf(idpf_adapter_to_dev(adapter),
		    "cfg_ifp: mac failed %d\n", err);
		return (err);
	}
	idpf_dbg(idpf_adapter_to_dev(adapter), "cfg_ifp: caps\n");

	/*
	 * RSS has no IFCAP bit on FreeBSD: it is not user-toggleable, so the
	 * negotiated capability is consulted directly where it matters.
	 */
	if (idpf_is_cap_ena_all(adapter, IDPF_CSUM_CAPS, IDPF_CAP_TX_CSUM_L4V4))
		caps |= IFCAP_TXCSUM;
	if (idpf_is_cap_ena_all(adapter, IDPF_CSUM_CAPS, IDPF_CAP_TX_CSUM_L4V6))
		caps |= IFCAP_TXCSUM_IPV6;
	if (idpf_is_cap_ena(adapter, IDPF_CSUM_CAPS, IDPF_CAP_RX_CSUM))
		caps |= IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6;
	if (idpf_is_cap_ena(adapter, IDPF_SEG_CAPS, VIRTCHNL2_CAP_SEG_IPV4_TCP))
		caps |= IFCAP_TSO4;
	if (idpf_is_cap_ena(adapter, IDPF_SEG_CAPS, VIRTCHNL2_CAP_SEG_IPV6_TCP))
		caps |= IFCAP_TSO6;
	if (idpf_is_cap_ena_all(adapter, IDPF_RSC_CAPS, IDPF_CAP_RSC))
		caps |= IFCAP_LRO;

	caps |= idpf_get_vlan_caps(adapter);
	caps |= IFCAP_JUMBO_MTU | IFCAP_HWSTATS;

	/*
	 * During attach the vport is created before iflib builds the ifnet, so
	 * the capabilities are published from ifdi_attach_post() instead; the
	 * same values are recomputed there.
	 */
	idpf_dbg(idpf_adapter_to_dev(adapter),
	    "cfg_ifp: ifp=%p caps=0x%x\n", (void *)ifp, caps);
	if (ifp == NULL)
		return (0);

	if_setcapabilities(ifp, caps);
	if_setcapenable(ifp, caps);
	if_setbaudrate(ifp, IF_Gbps(25));
	if_setmtu(ifp, min(if_getmtu(ifp), vport->max_mtu));

	/*
	 * SCTP CRC and loopback have no IFCAP counterpart and are left to the
	 * control plane's default.  [FBSD15:A30]
	 */

	return (0);
}

/**
 * idpf_vport_manage_rss_lut - zero or restore the redirection table
 * @vport: vport being changed
 *
 * Disabling RSS on FreeBSD means steering everything to queue 0, which is
 * done by zeroing the table; the configured table is cached so that
 * re-enabling restores it.
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_vport_manage_rss_lut(struct idpf_vport *vport)
{
	bool ena = idpf_is_cap_ena_all(vport->adapter, IDPF_RSS_CAPS,
	    IDPF_CAP_RSS);
	struct idpf_rss_data *rss_data;
	uint16_t idx = vport->idx;
	int lut_size;

	if (!vport->link_up)
		return (0);

	rss_data = &vport->adapter->vport_config[idx]->user_config.rss_data;
	lut_size = rss_data->rss_lut_size * sizeof(uint32_t);

	if (ena) {
		memcpy(rss_data->rss_lut, rss_data->cached_lut, lut_size);
	} else {
		memcpy(rss_data->cached_lut, rss_data->rss_lut, lut_size);
		memset(rss_data->rss_lut, 0, lut_size);
	}

	return (idpf_config_rss(vport, rss_data));
}

/* ---------------------------------------------------------------------
 * iflib device interface
 * --------------------------------------------------------------------- */

/**
 * idpf_if_init - ifdi_init() implementation
 * @ctx: iflib context
 */
static void
idpf_if_init(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;
	struct idpf_vport *vport = np->vport;

	if (vport == NULL)
		return;

	if ((adapter->flags & (1u << IDPF_REMOVE_IN_PROG)) != 0)
		return;

	idpf_vport_ctrl_lock(adapter);
	idpf_vport_open(vport);
	idpf_apply_capabilities(vport);
	idpf_vport_ctrl_unlock(adapter);
}

/**
 * idpf_if_stop - ifdi_stop() implementation
 * @ctx: iflib context
 */
static void
idpf_if_stop(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;
	struct idpf_vport *vport = np->vport;

	if (vport == NULL)
		return;

	idpf_vport_ctrl_lock(adapter);
	idpf_vport_stop(vport);
	idpf_vport_ctrl_unlock(adapter);
}

/**
 * idpf_if_msix_intr_assign - ifdi_msix_intr_assign() implementation
 * @ctx: iflib context
 * @msix: number of vectors iflib expects to use
 *
 * The vectors themselves were allocated in idpf_intr_req(); this only binds
 * the queue filters to the ones this vport was granted.
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_if_msix_intr_assign(if_ctx_t ctx, int msix __unused)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;
	struct idpf_vport *vport = np->vport;
	struct idpf_q_vec_rsrc *rsrc;
	char irq_name[IDPF_INT_NAME_STR_LEN];
	int i, err, rid;

	if (vport == NULL)
		return (ENXIO);

	rsrc = &vport->dflt_qv_rsrc;

	if (rsrc->q_vectors == NULL || rsrc->q_vector_idxs == NULL)
		return (ENXIO);

	for (i = 0; i < rsrc->num_q_vectors; i++) {
		rid = rsrc->q_vector_idxs[i] + 1;

		snprintf(irq_name, sizeof(irq_name), "rxq%d", i);
		err = iflib_irq_alloc_generic(ctx, &rsrc->q_vectors[i].que_irq,
		    rid, IFLIB_INTR_RXTX, NULL, &rsrc->q_vectors[i], i,
		    irq_name);
		if (err != 0) {
			device_printf(idpf_adapter_to_dev(adapter),
			    "failed to allocate interrupt for queue %d: %d\n",
			    i, err);
			return (err);
		}
	}

	for (i = 0; i < rsrc->num_txq; i++)
		iflib_softirq_alloc_generic(ctx, NULL, IFLIB_INTR_TX, NULL, i,
		    "tx");

	return (0);
}

/**
 * idpf_if_update_admin_status - ifdi_update_admin_status() implementation
 * @ctx: iflib context
 *
 * Link state is pushed by the control plane through idpf_handle_event_link(),
 * so this only has to keep the service task armed.
 */
static void
idpf_if_update_admin_status(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;

	if (idpf_is_resource_rel_in_prog(adapter))
		return;

	callout_reset(&adapter->serv_task, idpf_msecs_to_ticks(300),
	    idpf_service_task, adapter);
}

/**
 * idpf_if_mtu_set - ifdi_mtu_set() implementation
 * @ctx: iflib context
 * @mtu: requested MTU
 *
 * Return: 0 on success, EINVAL when out of range.
 */
static int
idpf_if_mtu_set(if_ctx_t ctx, uint32_t mtu)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_vport *vport = np->vport;
	if_softc_ctx_t scctx = iflib_get_softc_ctx(ctx);

	if (vport == NULL)
		return (ENXIO);

	if (mtu < ETHERMIN || mtu > vport->max_mtu)
		return (EINVAL);

	if_setmtu(iflib_get_ifp(ctx), mtu);
	scctx->isc_max_frame_size = mtu + ETHER_HDR_LEN + ETHER_CRC_LEN;

	/*
	 * The queues are not reallocated across a stop/init cycle, so the new
	 * frame size has to be written into them before the reconfiguration
	 * that iflib performs around this call sends it to the device.
	 */
	idpf_vport_set_rx_frame_size(&vport->dflt_qv_rsrc, mtu);

	return (0);
}

/**
 * idpf_if_promisc_set - ifdi_promisc_set() implementation
 * @ctx: iflib context
 * @flags: interface flags
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_if_promisc_set(if_ctx_t ctx, int flags)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;
	struct idpf_vport_user_config_data *config_data;

	if (!idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_PROMISC))
		return (0);

	config_data = &adapter->vport_config[np->vport_idx]->user_config;

	/*
	 * IFF_PROMISC covers unicast and multicast; IFF_ALLMULTI covers only
	 * multicast.
	 */
	if ((flags & IFF_PROMISC) != 0)
		config_data->user_flags |= (1ULL << __IDPF_PROMISC_UC) |
		    (1ULL << __IDPF_PROMISC_MC);
	else if ((flags & IFF_ALLMULTI) != 0)
		config_data->user_flags = (config_data->user_flags &
		    ~(1ULL << __IDPF_PROMISC_UC)) | (1ULL << __IDPF_PROMISC_MC);
	else
		config_data->user_flags &= ~((1ULL << __IDPF_PROMISC_UC) |
		    (1ULL << __IDPF_PROMISC_MC));

	return (idpf_set_promiscuous(adapter, config_data, np->vport_id));
}

/**
 * idpf_multi_set_cb - per-address callback for the multicast walk
 * @arg: vport
 * @sdl: link-layer address
 * @count: iteration count
 *
 * Return: 1 so that the walk keeps going.
 */
static u_int
idpf_multi_set_cb(void *arg, struct sockaddr_dl *sdl, u_int count __unused)
{
	struct idpf_vport *vport = arg;
	struct idpf_netdev_priv *np = iflib_get_softc(vport->ctx);

	idpf_add_mac_filter(vport, np, (uint8_t *)LLADDR(sdl), true);

	return (1);
}

/**
 * idpf_if_multi_set - ifdi_multi_set() implementation
 * @ctx: iflib context
 */
static void
idpf_if_multi_set(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;
	struct idpf_vport *vport = np->vport;

	if (vport == NULL)
		return;

	if (!idpf_is_cap_ena(adapter, IDPF_OTHER_CAPS, VIRTCHNL2_CAP_MACFILTER))
		return;

	if_foreach_llmaddr(iflib_get_ifp(ctx), idpf_multi_set_cb, vport);
}

/**
 * idpf_if_timer - ifdi_timer() implementation
 * @ctx: iflib context
 * @qid: queue being polled
 *
 * iflib calls this once per second per queue; only queue 0 needs to nudge the
 * admin path.
 */
static void
idpf_if_timer(if_ctx_t ctx, uint16_t qid)
{

	if (qid != 0)
		return;

	iflib_admin_intr_deferred(ctx);
}

/**
 * idpf_if_get_counter - ifdi_get_counter() implementation
 * @ctx: iflib context
 * @cnt: counter being read
 *
 * Return: the counter value.
 */
static uint64_t
idpf_if_get_counter(if_ctx_t ctx, ift_counter cnt)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	if_t ifp = iflib_get_ifp(ctx);
	uint64_t val;

	/* The emulation platform does not implement the statistics message. */
	if (IS_EMR_DEVICE(np->adapter->hw.subsystem_device_id))
		return (if_get_counter_default(ifp, cnt));

	mtx_lock(&np->stats_lock);
	switch (cnt) {
	case IFCOUNTER_IPACKETS:
		val = np->netstats.ifi_ipackets;
		break;
	case IFCOUNTER_IBYTES:
		val = np->netstats.ifi_ibytes;
		break;
	case IFCOUNTER_IERRORS:
		val = np->netstats.ifi_ierrors;
		break;
	case IFCOUNTER_IQDROPS:
		val = np->netstats.ifi_iqdrops;
		break;
	case IFCOUNTER_OPACKETS:
		val = np->netstats.ifi_opackets;
		break;
	case IFCOUNTER_OBYTES:
		val = np->netstats.ifi_obytes;
		break;
	case IFCOUNTER_OERRORS:
		val = np->netstats.ifi_oerrors;
		break;
	case IFCOUNTER_OQDROPS:
		val = np->netstats.ifi_oqdrops;
		break;
	default:
		mtx_unlock(&np->stats_lock);
		return (if_get_counter_default(ifp, cnt));
	}
	mtx_unlock(&np->stats_lock);

	return (val);
}

/**
 * idpf_if_priv_ioctl - ifdi_priv_ioctl() implementation
 * @ctx: iflib context
 * @command: socket ioctl command
 * @data: struct ifdrv from userspace
 *
 * iflib dispatches only SIOCGPRIVATE_0 and SIOCxDRVSPEC here, under a
 * sleepable context lock, so copyout() is safe.
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_if_priv_ioctl(if_ctx_t ctx, u_long command, caddr_t data)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct ifdrv *ifd = (struct ifdrv *)data;
	struct idpf_vport *vport = np->vport;
	struct idpf_q_vec_rsrc *rsrc;
	struct idpf_drv_info info;

	if (command != SIOCGDRVSPEC)
		return (ENOTTY);
	if (vport == NULL)
		return (ENXIO);
	if (ifd->ifd_cmd != IDPF_DRVCMD_GET_INFO)
		return (EINVAL);
	if (ifd->ifd_len != sizeof(info))
		return (EINVAL);

	rsrc = &vport->dflt_qv_rsrc;
	memset(&info, 0, sizeof(info));
	info.vport_id = vport->vport_id;
	info.link_speed_mbps = np->link_speed_mbps;
	info.num_txq = rsrc->num_txq;
	info.num_rxq = rsrc->num_rxq;
	info.num_q_vectors = rsrc->num_q_vectors;
	info.link_up = vport->link_up;
	info.link_known = vport->link_known;
	memcpy(info.mac, vport->default_mac_addr, ETHER_ADDR_LEN);

	return (copyout(&info, ifd->ifd_data, sizeof(info)));
}

/**
 * idpf_if_media_status - ifdi_media_status() implementation
 * @ctx: iflib context
 * @ifmr: media request to fill
 */
static void
idpf_if_media_status(if_ctx_t ctx, struct ifmediareq *ifmr)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_vport *vport = np->vport;

	ifmr->ifm_active = IFM_ETHER;

	/*
	 * Leave IFM_AVALID clear until the control plane has reported link;
	 * setting it would claim "no carrier" when the state is simply unknown.
	 */
	ifmr->ifm_status = 0;
	if (vport == NULL || !vport->link_known)
		return;

	ifmr->ifm_status = IFM_AVALID;
	if (!vport->link_up)
		return;

	ifmr->ifm_status |= IFM_ACTIVE;
	/*
	 * The control plane reports a speed but not a medium, so the link is
	 * described as auto-negotiated full duplex.  [FBSD15:A30]
	 */
	ifmr->ifm_active |= IFM_AUTO | IFM_FDX;
}

/**
 * idpf_if_media_change - ifdi_media_change() implementation
 * @ctx: iflib context
 *
 * Return: ENODEV; the medium is owned by the control plane.
 */
static int
idpf_if_media_change(if_ctx_t ctx __unused)
{

	return (ENODEV);
}

/**
 * idpf_if_vlan_register - ifdi_vlan_register() implementation
 * @ctx: iflib context
 * @vtag: VLAN being added
 */
static void
idpf_if_vlan_register(if_ctx_t ctx, uint16_t vtag __unused)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);

	if (np->vport != NULL)
		idpf_set_vlan_features(np->vport, IFCAP_VLAN_HWTAGGING);
}

/**
 * idpf_if_vlan_unregister - ifdi_vlan_unregister() implementation
 * @ctx: iflib context
 * @vtag: VLAN being removed
 */
static void
idpf_if_vlan_unregister(if_ctx_t ctx, uint16_t vtag __unused)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);

	if (np->vport != NULL)
		idpf_set_vlan_features(np->vport, IFCAP_VLAN_HWTAGGING);
}

/**
 * idpf_if_needs_restart - ifdi_needs_restart() implementation
 * @ctx: iflib context
 * @event: event iflib is asking about
 *
 * Return: whether the interface has to be restarted for @event.
 */
static bool
idpf_if_needs_restart(if_ctx_t ctx __unused, enum iflib_restart_event event)
{

	switch (event) {
	case IFLIB_RESTART_VLAN_CONFIG:
		/* VLAN offloads are reprogrammed without a restart. */
		return (false);
	default:
		return (true);
	}
}

/**
 * idpf_if_watchdog_reset - ifdi_watchdog_reset() implementation
 * @ctx: iflib context
 */
static void
idpf_if_watchdog_reset(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);

	idpf_tx_timeout(np->adapter, 0);
}

/**
 * idpf_apply_capabilities - push the interface's capability state to the device
 * @vport: vport to reconfigure
 *
 * iflib re-runs ifdi_init() after SIOCSIFCAP, so the current if_capenable is
 * applied here rather than from a mask-based callback: iflib has no per-driver
 * capability hook.  [FBSD15:A30]
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_apply_capabilities(struct idpf_vport *vport)
{
	int err;

	err = idpf_vport_manage_rss_lut(vport);
	if (err != 0)
		return (err);

	return (idpf_set_vlan_features(vport, IFCAP_VLAN_HWTAGGING));
}

static device_method_t idpf_if_methods[] = {
	DEVMETHOD(ifdi_attach_pre,		idpf_if_attach_pre),
	DEVMETHOD(ifdi_attach_post,		idpf_if_attach_post),
	DEVMETHOD(ifdi_detach,			idpf_if_detach),
	DEVMETHOD(ifdi_shutdown,		idpf_if_shutdown),
	DEVMETHOD(ifdi_suspend,			idpf_if_suspend),
	DEVMETHOD(ifdi_resume,			idpf_if_resume),
	DEVMETHOD(ifdi_init,			idpf_if_init),
	DEVMETHOD(ifdi_stop,			idpf_if_stop),
	DEVMETHOD(ifdi_msix_intr_assign,	idpf_if_msix_intr_assign),
	DEVMETHOD(ifdi_intr_enable,		idpf_intr_enable),
	DEVMETHOD(ifdi_intr_disable,		idpf_intr_disable),
	DEVMETHOD(ifdi_tx_queue_intr_enable,	idpf_tx_queue_intr_enable),
	DEVMETHOD(ifdi_rx_queue_intr_enable,	idpf_rx_queue_intr_enable),
	DEVMETHOD(ifdi_tx_queues_alloc,		idpf_tx_queues_alloc),
	DEVMETHOD(ifdi_rx_queues_alloc,		idpf_rx_queues_alloc),
	DEVMETHOD(ifdi_queues_free,		idpf_queues_free),
	DEVMETHOD(ifdi_update_admin_status,	idpf_if_update_admin_status),
	DEVMETHOD(ifdi_multi_set,		idpf_if_multi_set),
	DEVMETHOD(ifdi_mtu_set,			idpf_if_mtu_set),
	DEVMETHOD(ifdi_media_status,		idpf_if_media_status),
	DEVMETHOD(ifdi_media_change,		idpf_if_media_change),
	DEVMETHOD(ifdi_priv_ioctl,		idpf_if_priv_ioctl),
	DEVMETHOD(ifdi_promisc_set,		idpf_if_promisc_set),
	DEVMETHOD(ifdi_timer,			idpf_if_timer),
	DEVMETHOD(ifdi_watchdog_reset,		idpf_if_watchdog_reset),
	DEVMETHOD(ifdi_get_counter,		idpf_if_get_counter),
	DEVMETHOD(ifdi_vlan_register,		idpf_if_vlan_register),
	DEVMETHOD(ifdi_vlan_unregister,		idpf_if_vlan_unregister),
	DEVMETHOD(ifdi_needs_restart,		idpf_if_needs_restart),
	DEVMETHOD_END
};

driver_t idpf_if_driver = {
	"idpf_if", idpf_if_methods, sizeof(struct idpf_netdev_priv)
};



