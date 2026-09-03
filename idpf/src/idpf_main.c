/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Module and device entry points.
 *
 * FreeBSD port notes
 * ------------------
 * Driver model.  Linux registers a struct pci_driver whose probe() builds
 * everything.  FreeBSD registers a newbus driver whose device methods are
 * iflib's, and the real work moves into ifdi_attach_pre()/ifdi_attach_post():
 * iflib allocates the softc, creates the ifnet and only then calls back.  The
 * iflib softc is struct idpf_netdev_priv, the per-vport handle; the
 * function-wide struct idpf_adapter is allocated here and reached through
 * np->adapter.  [FBSD15:A30]
 *
 * MMIO.  Linux devm_ioremap()s the mailbox and reset windows separately.
 * FreeBSD will not hand out the same BAR twice, so BAR0 is mapped once into
 * dev_ops.static_reg_info[0] and every window is an offset into it, using the
 * geometry idpf_dev_ops_init() published in struct idpf_hw.  [FBSD15:A31]
 *
 * Deferred work.  The Linux workqueues become taskqueues, with the periodic
 * items on callouts and the delayed one-shots on timeout_tasks.  [FBSD15:A33]
 *
 * Removed.  PCI AER/error-handler callbacks, PTM, devlink, VFIO/mdev and
 * SR-IOV VF enablement have no counterpart reachable from here: FreeBSD
 * surfaces SR-IOV through pci_iov_attach() and a PCI_IOV_* method set, which
 * is a separate feature, and PCI error recovery is handled by the bus rather
 * than by per-driver callbacks.  The class-based PCI match Linux uses is also
 * gone; iflib matches on vendor/device, which the explicit table covers.
 * [LOCAL:A22]
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
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
#include <net/iflib.h>

#include "ifdi_if.h"

#include "idpf.h"
#include "idpf_lan_vf_regs.h"
#include "idpf_virtchnl.h"
#include "idpf_ptp.h"

#define DRV_SUMMARY	"Intel(R) Infrastructure Data Path Function Driver"

/* How long attach waits for the control plane to finish creating vports. */
#define IDPF_RESET_SETTLE_MS	15000

#define IDPF_INTEL_VENDOR_ID	0x8086

/* Written to VF_ARQBAL to tell a VF BAR apart from a PF one. */
#define IDPF_VF_TEST_VAL	0xfeed0000u

MALLOC_DEFINE(M_IDPF, "idpf", "Intel(R) IDPF");

static void *idpf_register(device_t dev);

static pci_vendor_info_t idpf_vendor_info_array[] = {
	PVID(IDPF_INTEL_VENDOR_ID, IDPF_DEV_ID_PF,
	    "Intel(R) Infrastructure Data Path Function PF"),
	PVID(IDPF_INTEL_VENDOR_ID, IDPF_DEV_ID_VF,
	    "Intel(R) Infrastructure Data Path Function VF"),
	PVID(IDPF_INTEL_VENDOR_ID, IDPF_DEV_ID_VF_SIOV,
	    "Intel(R) Infrastructure Data Path Function VF (S-IOV)"),
	PVID(IDPF_INTEL_VENDOR_ID, IDPF_DEV_ID_PF_SIMICS,
	    "Intel(R) Infrastructure Data Path Function PF (Simics)"),
	PVID(IDPF_INTEL_VENDOR_ID, IDPF_DEV_ID_VF_SIMICS,
	    "Intel(R) Infrastructure Data Path Function VF (Simics)"),
	PVID_END
};

static device_method_t idpf_methods[] = {
	DEVMETHOD(device_register,	idpf_register),
	DEVMETHOD(device_probe,		iflib_device_probe),
	DEVMETHOD(device_attach,	iflib_device_attach),
	DEVMETHOD(device_detach,	iflib_device_detach),
	DEVMETHOD(device_shutdown,	iflib_device_shutdown),
	DEVMETHOD(device_suspend,	iflib_device_suspend),
	DEVMETHOD(device_resume,	iflib_device_resume),
	DEVMETHOD_END
};

static driver_t idpf_driver = {
	"idpf", idpf_methods, sizeof(struct idpf_netdev_priv)
};

/* The test module compiles this file for its statics, not to claim devices. */
#ifndef IDPF_UNIT_TEST
DRIVER_MODULE(idpf, pci, idpf_driver, 0, 0);
MODULE_VERSION(idpf, 1);
MODULE_DEPEND(idpf, pci, 1, 1, 1);
MODULE_DEPEND(idpf, ether, 1, 1, 1);
MODULE_DEPEND(idpf, iflib, 1, 1, 1);
IFLIB_PNP_INFO(pci, idpf, idpf_vendor_info_array);
#endif

/*
 * The split queue model puts the completion queue at ring 0 of every queue
 * set, so a TX set is {completion, data} and an RX set is {completion, and
 * one free list per buffer queue}.  IFLIB_SKIP_MSIX is set because the
 * function's MSI-X vectors are pooled across vports in idpf_intr_req()
 * rather than allocated per interface.  [FBSD15:A30-A34]
 */
static struct if_shared_ctx idpf_sctx = {
	.isc_magic		= IFLIB_MAGIC,
	.isc_driver		= &idpf_if_driver,
	.isc_q_align		= PAGE_SIZE,
	.isc_admin_intrcnt	= 1,
	.isc_vendor_info	= idpf_vendor_info_array,
	.isc_driver_version	= IDPF_DRV_VER,

	.isc_nfl		= 1,
	.isc_ntxqs		= 1,
	.isc_nrxqs		= 1,

	.isc_ntxd_min		= { IDPF_MIN_TXQ_DESC, IDPF_MIN_TXQ_DESC },
	.isc_ntxd_max		= { IDPF_MAX_TXQ_DESC, IDPF_MAX_TXQ_DESC },
	.isc_ntxd_default	= { IDPF_DFLT_TX_Q_DESC_COUNT,
				    IDPF_DFLT_TX_Q_DESC_COUNT },
	.isc_nrxd_min		= { IDPF_MIN_RXQ_DESC, IDPF_MIN_RXQ_DESC,
				    IDPF_MIN_RXQ_DESC },
	.isc_nrxd_max		= { IDPF_MAX_RXQ_DESC, IDPF_MAX_RXQ_DESC,
				    IDPF_MAX_RXQ_DESC },
	.isc_nrxd_default	= { IDPF_DFLT_RX_Q_DESC_COUNT,
				    IDPF_DFLT_RX_Q_DESC_COUNT,
				    IDPF_DFLT_RX_Q_DESC_COUNT },

	.isc_tx_maxsize		= IDPF_TX_MAX_DESC_DATA,
	.isc_tx_maxsegsize	= IDPF_TX_MAX_DESC_DATA,
	.isc_tso_maxsize	= IDPF_TX_MAX_DESC_DATA,
	.isc_tso_maxsegsize	= IDPF_TX_MAX_READ_REQ_SIZE,
	.isc_rx_maxsize		= IDPF_RX_BUF_4096,
	.isc_rx_maxsegsize	= IDPF_RX_BUF_4096,
	.isc_rx_nsegments	= IDPF_MAX_BUFQS_PER_RXQ_GRP,

	.isc_flags		= IFLIB_SKIP_MSIX | IFLIB_ADMIN_ALWAYS_RUN,
};

/**
 * idpf_register - hand iflib the shared context
 * @dev: device being attached
 *
 * Return: the shared context.
 */
static void *
idpf_register(device_t dev __unused)
{

	return (&idpf_sctx);
}

/**
 * idpf_cfg_hw - point the register windows into the BAR0 mapping
 * @adapter: driver private data
 *
 * The window geometry was published by the device-ops initialiser; only the
 * virtual addresses are filled in here.
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_cfg_hw(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	struct resource *bar0 = adapter->dev_ops.static_reg_info[0];
	struct idpf_hw *hw = &adapter->hw;
	uint8_t *base;

	if (bar0 == NULL)
		return (ENXIO);

	base = (uint8_t *)rman_get_virtual(bar0);

	if (hw->mbx.addr_len == 0 || hw->rstat.addr_len == 0) {
		device_printf(dev,
		    "device ops did not publish the register windows\n");
		return (EINVAL);
	}

	hw->mbx.vaddr = base + hw->mbx.addr_start;
	hw->rstat.vaddr = base + hw->rstat.addr_start;

	hw->back = adapter;
	hw->vendor_id = pci_get_vendor(dev);
	hw->device_id = pci_get_device(dev);
	hw->subsystem_device_id = pci_get_subdevice(dev);
	hw->subsystem_vendor_id = pci_get_subvendor(dev);
	hw->revision_id = pci_get_revid(dev);

	return (0);
}

/**
 * idpf_get_device_type - tell a VF BAR apart from a PF one
 * @adapter: driver private data
 *
 * The VF mailbox base register is writable on a VF and reads back what was
 * written; on a PF the same offset does not behave that way.
 *
 * Return: IDPF_DEV_ID_VF or IDPF_DEV_ID_PF.
 */
static int
idpf_get_device_type(struct idpf_adapter *adapter)
{
	struct resource *bar0 = adapter->dev_ops.static_reg_info[0];
	uint32_t val;

	bus_write_4(bar0, VF_ARQBAL, IDPF_VF_TEST_VAL);
	val = bus_read_4(bar0, VF_ARQBAL);

	return (val == IDPF_VF_TEST_VAL ? IDPF_DEV_ID_VF : IDPF_DEV_ID_PF);
}

/**
 * idpf_dev_init - install the PF or VF device operations
 * @adapter: driver private data
 *
 * Return: 0 on success, ENODEV for an unrecognised device.
 */
static int
idpf_dev_init(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);

	switch (pci_get_device(dev)) {
	case IDPF_DEV_ID_PF:
	case IDPF_DEV_ID_PF_SIMICS:
		idpf_dev_ops_init(adapter);
		return (0);
	case IDPF_DEV_ID_VF:
	case IDPF_DEV_ID_VF_SIMICS:
		idpf_vf_dev_ops_init(adapter);
		adapter->crc_enable = true;
		return (0);
	case IDPF_DEV_ID_VF_SIOV:
		idpf_vf_dev_ops_init(adapter);
		return (0);
	default:
		break;
	}

	/*
	 * Parts that only advertise the Ethernet class are identified by
	 * probing the VF mailbox window.
	 */
	switch (idpf_get_device_type(adapter)) {
	case IDPF_DEV_ID_VF:
		idpf_vf_dev_ops_init(adapter);
		adapter->crc_enable = true;
		return (0);
	case IDPF_DEV_ID_PF:
		idpf_dev_ops_init(adapter);
		return (0);
	default:
		return (ENODEV);
	}
}

/**
 * idpf_free_taskqueues - tear down the deferred-work infrastructure
 * @adapter: driver private data
 */
static void
idpf_free_taskqueues(struct idpf_adapter *adapter)
{

	if (adapter->init_wq != NULL) {
		taskqueue_drain_timeout(adapter->init_wq, &adapter->init_task);
		taskqueue_free(adapter->init_wq);
		adapter->init_wq = NULL;
	}
	if (adapter->serv_wq != NULL) {
		taskqueue_free(adapter->serv_wq);
		adapter->serv_wq = NULL;
	}
	if (adapter->mbx_wq != NULL) {
		taskqueue_drain(adapter->mbx_wq, &adapter->mbx_task);
		taskqueue_free(adapter->mbx_wq);
		adapter->mbx_wq = NULL;
	}
	if (adapter->stats_wq != NULL) {
		taskqueue_drain(adapter->stats_wq, &adapter->stats_deferred);
		taskqueue_free(adapter->stats_wq);
		adapter->stats_wq = NULL;
	}
	if (adapter->vc_event_wq != NULL) {
		taskqueue_drain_timeout(adapter->vc_event_wq,
		    &adapter->vc_event_task);
		taskqueue_free(adapter->vc_event_wq);
		adapter->vc_event_wq = NULL;
	}
}

/**
 * idpf_alloc_taskqueue - create one taskqueue and start its thread
 * @adapter: driver private data
 * @tqp: where to store the taskqueue
 * @suffix: name suffix for the thread
 *
 * Return: 0 on success, ENOMEM on failure.
 */
static int
idpf_alloc_taskqueue(struct idpf_adapter *adapter, struct taskqueue **tqp,
    const char *suffix)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	struct taskqueue *tq;

	tq = taskqueue_create_fast(suffix, M_NOWAIT, taskqueue_thread_enqueue,
	    tqp);
	if (tq == NULL)
		return (ENOMEM);

	*tqp = tq;
	if (taskqueue_start_threads(tqp, 1, PI_NET, "%s %s",
	    device_get_nameunit(dev), suffix) != 0) {
		taskqueue_free(tq);
		*tqp = NULL;
		return (ENOMEM);
	}

	return (0);
}

/**
 * idpf_alloc_taskqueues - create the deferred-work infrastructure
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
static int
idpf_alloc_taskqueues(struct idpf_adapter *adapter)
{
	int err;

	callout_init(&adapter->serv_task, 1);
	callout_init(&adapter->stats_task, 1);
	callout_init(&adapter->mbx_poll_task, 1);

	err = idpf_alloc_taskqueue(adapter, &adapter->init_wq, "init");
	if (err != 0)
		goto fail;
	err = idpf_alloc_taskqueue(adapter, &adapter->serv_wq, "service");
	if (err != 0)
		goto fail;
	err = idpf_alloc_taskqueue(adapter, &adapter->mbx_wq, "mbx");
	if (err != 0)
		goto fail;
	if (IS_SILICON_DEVICE(adapter->hw.subsystem_device_id)) {
		err = idpf_alloc_taskqueue(adapter, &adapter->stats_wq,
		    "stats");
		if (err != 0)
			goto fail;
	}
	err = idpf_alloc_taskqueue(adapter, &adapter->vc_event_wq, "vc_event");
	if (err != 0)
		goto fail;

	TIMEOUT_TASK_INIT(adapter->init_wq, &adapter->init_task, 0,
	    idpf_init_task, adapter);
	TIMEOUT_TASK_INIT(adapter->vc_event_wq, &adapter->vc_event_task, 0,
	    idpf_vc_event_task, adapter);
	TASK_INIT(&adapter->mbx_task, 0, idpf_mbx_task, adapter);
	TASK_INIT(&adapter->stats_deferred, 0, idpf_statistics_task, adapter);

	return (0);

fail:
	device_printf(idpf_adapter_to_dev(adapter),
	    "failed to allocate taskqueues: %d\n", err);
	idpf_free_taskqueues(adapter);

	return (err);
}

/**
 * idpf_set_softc_ctx - publish the negotiated queue geometry to iflib
 * @ctx: iflib context
 * @vport: vport whose default resources describe the geometry
 *
 * iflib reads all of this immediately after ifdi_attach_pre() returns, so it
 * has to be filled in before then.  That is the reason the control-plane
 * handshake runs synchronously during attach: the queue counts and descriptor
 * sizes are only known once the vport exists.
 *
 * Return: 0 on success, EINVAL when the negotiated geometry does not match the
 * ring counts declared in the shared context.
 */
static int
idpf_set_softc_ctx(if_ctx_t ctx, struct idpf_vport *vport)
{
	if_softc_ctx_t scctx = iflib_get_softc_ctx(ctx);
	struct idpf_q_vec_rsrc *rsrc = &vport->dflt_qv_rsrc;
	struct idpf_adapter *adapter = vport->adapter;
	device_t dev = idpf_adapter_to_dev(adapter);
	bool tx_split = idpf_is_queue_model_split(rsrc->txq_model);
	bool rx_split = idpf_is_queue_model_split(rsrc->rxq_model);
	int expect_rxqs, expect_txqs;
	int caps = 0;
	int i;

	/*
	 * The shared context declares the per-set ring counts statically, so a
	 * device that negotiates a different shape has to be rejected rather
	 * than left to overrun the descriptor arrays.
	 */
	expect_txqs = tx_split ? 2 : 1;
	expect_rxqs = rx_split ? 1 + rsrc->num_bufqs_per_qgrp : 1;
	if (expect_txqs != idpf_sctx.isc_ntxqs ||
	    expect_rxqs != idpf_sctx.isc_nrxqs) {
		device_printf(dev,
		    "negotiated queue shape %dx%d unsupported by this driver "
		    "build (%dx%d)\n", expect_txqs, expect_rxqs,
		    idpf_sctx.isc_ntxqs, idpf_sctx.isc_nrxqs);
		return (EINVAL);
	}

	scctx->isc_txrx = &idpf_txrx_ops;

	scctx->isc_ntxqsets = rsrc->num_txq;
	scctx->isc_nrxqsets = rsrc->num_rxq;
	scctx->isc_ntxqsets_max = rsrc->num_txq;
	scctx->isc_nrxqsets_max = rsrc->num_rxq;
	scctx->isc_vectors = rsrc->num_q_vectors;

	/* Ring 0 of a split TX set is the completion queue. */
	if (tx_split) {
		scctx->isc_txd_size[0] =
		    sizeof(struct idpf_splitq_tx_compl_desc);
		scctx->isc_txd_size[1] = sizeof(union idpf_tx_flex_desc);
	} else {
		scctx->isc_txd_size[0] = sizeof(struct idpf_base_tx_desc);
	}
	for (i = 0; i < expect_txqs; i++)
		scctx->isc_txqsizes[i] = roundup2(scctx->isc_ntxd[i] *
		    scctx->isc_txd_size[i], PAGE_SIZE);

	/* Ring 0 of an RX set is the completion queue, the rest are free lists. */
	scctx->isc_rxd_size[0] = sizeof(union virtchnl2_rx_desc);
	if (rx_split) {
		for (i = 1; i < expect_rxqs; i++)
			scctx->isc_rxd_size[i] =
			    sizeof(struct virtchnl2_splitq_rx_buf_desc);
	}
	for (i = 0; i < expect_rxqs; i++)
		scctx->isc_rxqsizes[i] = roundup2(scctx->isc_nrxd[i] *
		    scctx->isc_rxd_size[i], PAGE_SIZE);

	scctx->isc_tx_nsegments = idpf_get_max_tx_bufs(adapter);
	scctx->isc_tx_tso_segments_max = idpf_get_max_tx_bufs(adapter);
	scctx->isc_tx_tso_size_max = IDPF_TX_MAX_DESC_DATA;
	scctx->isc_tx_tso_segsize_max = IDPF_TX_MAX_READ_REQ_SIZE;

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
	/* LINKSTATE: the control plane pushes link events to iflib. */
	caps |= IFCAP_JUMBO_MTU | IFCAP_HWSTATS | IFCAP_LINKSTATE;

	scctx->isc_capabilities = caps;
	scctx->isc_capenable = caps;
	scctx->isc_tx_csum_flags = IDPF_CSUM_OFFLOAD;

	scctx->isc_max_frame_size = vport->max_mtu + ETHER_HDR_LEN +
	    ETHER_CRC_LEN;
	scctx->isc_min_frame_size = ETHER_MIN_LEN;

	/* Vectors are pooled across vports in idpf_intr_req(), not by iflib. */
	scctx->isc_msix_bar = -1;
	scctx->isc_intr = IFLIB_INTR_MSIX;

	scctx->isc_rss_table_size =
	    adapter->vport_config[vport->idx]->user_config.rss_data.rss_lut_size;

	return (0);
}

/**
 * idpf_if_attach_pre - ifdi_attach_pre() implementation
 * @ctx: iflib context
 *
 * Claims BAR0, identifies the device, builds the deferred-work
 * infrastructure and runs the load-time reset through to a created vport, so
 * that the negotiated geometry can be handed to iflib before it builds the
 * queues.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_if_attach_pre(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	device_t dev = iflib_get_dev(ctx);
	struct idpf_adapter *adapter;
	int rid, msix_cap, err;

	adapter = malloc(sizeof(*adapter), M_IDPF, M_NOWAIT | M_ZERO);
	if (adapter == NULL)
		return (ENOMEM);

	np->adapter = adapter;
	adapter->dev = dev;
	adapter->drv_name = IDPF_DRV_NAME;
	adapter->drv_ver = IDPF_DRV_VER;

	/*
	 * Single queue model: this control plane does not advertise
	 * VIRTCHNL2_CAP_SPLITQ_QSCHED, and flow-scheduled split TX retires
	 * completion tags out of order, which iflib's in-order credit
	 * interface cannot express.
	 */
	adapter->req_tx_splitq = false;
	adapter->req_rx_splitq = false;

	mtx_init(&np->stats_lock, "idpf_stats", NULL, MTX_DEF);

	sx_init(&adapter->vport_ctrl_lock, "idpf_vport_ctrl");
	sx_init(&adapter->vector_lock, "idpf_vector");
	sx_init(&adapter->queue_lock, "idpf_queue");

	mtx_init(&adapter->corer_done_lock, "idpf_corer", NULL, MTX_DEF);
	cv_init(&adapter->corer_done_cv, "idpf_corer");

	mtx_init(&adapter->adi_info.priv_lock, "idpf_adi", NULL, MTX_DEF);
	TAILQ_INIT(&adapter->adi_info.priv_list);

	pci_enable_busmaster(dev);

	rid = PCIR_BAR(0);
	adapter->dev_ops.static_reg_info[0] = bus_alloc_resource_any(dev,
	    SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (adapter->dev_ops.static_reg_info[0] == NULL) {
		device_printf(dev, "failed to map BAR0\n");
		err = ENXIO;
		goto err_locks;
	}

	/*
	 * The MSI-X table lives in its own BAR, which pci_alloc_msix() requires
	 * to be mapped before it will hand out vectors.  The BAR index comes
	 * from the capability rather than being assumed.
	 */
	if (pci_find_cap(dev, PCIY_MSIX, &msix_cap) == 0) {
		uint32_t table;

		table = pci_read_config(dev, msix_cap + PCIR_MSIX_TABLE, 4);
		rid = PCIR_BAR(table & PCIM_MSIX_BIR_MASK);
		if (rid != PCIR_BAR(0)) {
			adapter->dev_ops.static_reg_info[1] =
			    bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
			    RF_ACTIVE);
			if (adapter->dev_ops.static_reg_info[1] == NULL) {
				device_printf(dev,
				    "failed to map the MSI-X table BAR\n");
				err = ENXIO;
				goto err_bar;
			}
		}
	}

	adapter->vcxn_mngr = malloc(sizeof(*adapter->vcxn_mngr), M_IDPF,
	    M_NOWAIT | M_ZERO);
	if (adapter->vcxn_mngr == NULL) {
		err = ENOMEM;
		goto err_bar;
	}
	idpf_init_vc_xn_completion(adapter->vcxn_mngr);
	idpf_vc_xn_init(adapter->vcxn_mngr);

	err = idpf_dev_init(adapter);
	if (err != 0) {
		device_printf(dev, "unexpected device 0x%x\n",
		    pci_get_device(dev));
		goto err_vcxn;
	}

	err = idpf_cfg_hw(adapter);
	if (err != 0) {
		device_printf(dev, "failed to configure HW structure: %d\n",
		    err);
		goto err_vcxn;
	}

	err = idpf_alloc_taskqueues(adapter);
	if (err != 0)
		goto err_vcxn;

	adapter->dev_ops.reg_ops.reset_reg_init(adapter);

	/*
	 * The vport has to exist before iflib builds the queues, so the load
	 * reset is driven here rather than from the event task.  This blocks
	 * until the control plane has answered and idpf_init_task() has created
	 * the default vport.
	 */
	adapter->attach_ctx = ctx;
	adapter->flags |= (1u << IDPF_HR_DRV_LOAD);
	/*
	 * idpf_reset_recover() only waits for the vport if this is set, which
	 * is what makes the call below synchronous.
	 */
	adapter->flags |= (1u << IDPF_HR_RESET_IN_PROG);
	idpf_init_hard_reset(adapter);

	if (np->vport == NULL) {
		device_printf(dev, "no vport after load reset\n");
		err = EIO;
		goto err_bringup;
	}

	err = idpf_set_softc_ctx(ctx, np->vport);
	if (err != 0)
		goto err_bringup;

	/*
	 * iflib allocates the descriptor rings as soon as this returns and
	 * hands them straight to ifdi_tx_queues_alloc(), so the software
	 * queue structures have to exist now.  The Linux flow only built
	 * them at open.
	 */
	err = idpf_vport_intr_alloc(np->vport, &np->vport->dflt_qv_rsrc);
	if (err != 0) {
		device_printf(dev, "failed to allocate interrupt vectors: %d\n",
		    err);
		goto err_bringup;
	}

	err = idpf_vport_queue_alloc_all(np->vport, &np->vport->dflt_qv_rsrc);
	if (err != 0) {
		device_printf(dev, "failed to allocate queue structures: %d\n",
		    err);
		goto err_bringup;
	}

	/* iflib passes this to ether_ifattach() before attach_post runs. */
	iflib_set_mac(ctx, np->vport->default_mac_addr);

	/*
	 * The control plane reports a speed but not a medium, so only
	 * autoselect is offered; idpf_if_media_status() fills in the detail.
	 */
	ifmedia_add(iflib_get_media(ctx), IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(iflib_get_media(ctx), IFM_ETHER | IFM_AUTO);

	return (0);

err_bringup:
	idpf_vc_core_deinit(adapter);
	idpf_deinit_dflt_mbx(adapter);
	callout_drain(&adapter->serv_task);
	callout_drain(&adapter->stats_task);
	callout_drain(&adapter->mbx_poll_task);
	idpf_free_taskqueues(adapter);
err_vcxn:
	idpf_vc_xn_shutdown(adapter->vcxn_mngr);
	idpf_deinit_vc_xn_completion(adapter->vcxn_mngr);
	free(adapter->vcxn_mngr, M_IDPF);
	adapter->vcxn_mngr = NULL;
err_bar:
	if (adapter->dev_ops.static_reg_info[1] != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY,
		    rman_get_rid(adapter->dev_ops.static_reg_info[1]),
		    adapter->dev_ops.static_reg_info[1]);
		adapter->dev_ops.static_reg_info[1] = NULL;
	}
	bus_release_resource(dev, SYS_RES_MEMORY, PCIR_BAR(0),
	    adapter->dev_ops.static_reg_info[0]);
	adapter->dev_ops.static_reg_info[0] = NULL;
err_locks:
	mtx_destroy(&adapter->adi_info.priv_lock);
	cv_destroy(&adapter->corer_done_cv);
	mtx_destroy(&adapter->corer_done_lock);
	sx_destroy(&adapter->queue_lock);
	sx_destroy(&adapter->vector_lock);
	sx_destroy(&adapter->vport_ctrl_lock);
	mtx_destroy(&np->stats_lock);
	free(adapter, M_IDPF);
	np->adapter = NULL;

	return (err);
}

/**
 * idpf_if_attach_post - ifdi_attach_post() implementation
 * @ctx: iflib context
 *
 * The vports are created by the init task once the control plane answers, so
 * this only records the context the first one will bind to.
 *
 * Return: 0.
 */
int
idpf_if_attach_post(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;
	if_softc_ctx_t scctx = iflib_get_softc_ctx(ctx);
	if_t ifp = iflib_get_ifp(ctx);

	/*
	 * The vport was created before this ifnet existed, so the properties
	 * idpf_vport_cfg_ifp() had to skip are published here.
	 */
	if (ifp != NULL && np->vport != NULL) {
		np->vport->ifp = ifp;
		if_setcapabilities(ifp, scctx->isc_capabilities);
		if_setcapenable(ifp, scctx->isc_capenable);
		if_setmtu(ifp, min(if_getmtu(ifp), np->vport->max_mtu));
	}

	device_printf(adapter->dev, "%s, version %s\n", DRV_SUMMARY,
	    IDPF_DRV_VER);

	idpf_ptp_sysctl_init(adapter);

	return (0);
}

/**
 * idpf_if_detach - ifdi_detach() implementation
 * @ctx: iflib context
 *
 * Unwinds attach in reverse: stop taking work, quiesce the control plane,
 * leave the device reset, then release the locks and BAR.
 *
 * Return: 0.
 */
int
idpf_if_detach(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;
	device_t dev;
	int i;

	if (adapter == NULL)
		return (0);

	dev = idpf_adapter_to_dev(adapter);
	adapter->flags |= (1u << IDPF_REMOVE_IN_PROG);

	/*
	 * Wait for the event task before releasing anything: a hard reset in
	 * flight would otherwise keep walking structures being freed.
	 */
	taskqueue_drain_timeout(adapter->vc_event_wq, &adapter->vc_event_task);

	idpf_vc_core_deinit(adapter);

	/* Leave the device clean for whoever attaches next. */
	adapter->dev_ops.reg_ops.trigger_reset(adapter, IDPF_HR_FUNC_RESET);
	idpf_deinit_dflt_mbx(adapter);

	callout_drain(&adapter->serv_task);
	callout_drain(&adapter->stats_task);
	callout_drain(&adapter->mbx_poll_task);
	idpf_free_taskqueues(adapter);

	if (adapter->vport_config != NULL) {
		for (i = 0; i < adapter->max_vports; i++) {
			if (adapter->vport_config[i] == NULL)
				continue;
			mtx_destroy(&adapter->vport_config[i]->
			    flow_steer_list_lock);
			mtx_destroy(&adapter->vport_config[i]->
			    mac_filter_list_lock);
			free(adapter->vport_config[i]->user_config.q_coalesce,
			    M_DEVBUF);
			free(adapter->vport_config[i], M_DEVBUF);
			adapter->vport_config[i] = NULL;
		}
		free(adapter->vport_config, M_DEVBUF);
		adapter->vport_config = NULL;
	}

	free(adapter->iflib_ctxs, M_DEVBUF);
	adapter->iflib_ctxs = NULL;

	if (adapter->vcxn_mngr != NULL) {
		idpf_vc_xn_shutdown(adapter->vcxn_mngr);
		idpf_deinit_vc_xn_completion(adapter->vcxn_mngr);
		free(adapter->vcxn_mngr, M_IDPF);
		adapter->vcxn_mngr = NULL;
	}

	if (adapter->dev_ops.static_reg_info[1] != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY,
		    rman_get_rid(adapter->dev_ops.static_reg_info[1]),
		    adapter->dev_ops.static_reg_info[1]);
		adapter->dev_ops.static_reg_info[1] = NULL;
	}
	if (adapter->dev_ops.static_reg_info[0] != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, PCIR_BAR(0),
		    adapter->dev_ops.static_reg_info[0]);
		adapter->dev_ops.static_reg_info[0] = NULL;
	}
	pci_disable_busmaster(dev);

	mtx_destroy(&adapter->adi_info.priv_lock);
	cv_destroy(&adapter->corer_done_cv);
	mtx_destroy(&adapter->corer_done_lock);
	sx_destroy(&adapter->queue_lock);
	sx_destroy(&adapter->vector_lock);
	sx_destroy(&adapter->vport_ctrl_lock);
	mtx_destroy(&np->stats_lock);

	free(adapter, M_IDPF);
	np->adapter = NULL;

	return (0);
}

/**
 * idpf_if_shutdown - ifdi_shutdown() implementation
 * @ctx: iflib context
 *
 * Return: 0.
 */
int
idpf_if_shutdown(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;

	if (adapter == NULL)
		return (0);

	adapter->flags |= (1u << IDPF_REMOVE_IN_PROG);

	callout_drain(&adapter->serv_task);
	taskqueue_drain_timeout(adapter->vc_event_wq, &adapter->vc_event_task);

	if (adapter->vcxn_mngr != NULL)
		idpf_vc_xn_shutdown(adapter->vcxn_mngr);

	idpf_vc_core_deinit(adapter);
	idpf_deinit_dflt_mbx(adapter);

	return (0);
}

/**
 * idpf_reset_prepare - quiesce the driver ahead of a bus-level reset
 * @adapter: driver private data
 */
static void
idpf_reset_prepare(struct idpf_adapter *adapter)
{

	device_printf(idpf_adapter_to_dev(adapter), "resetting\n");

	callout_drain(&adapter->serv_task);
	taskqueue_drain_timeout(adapter->vc_event_wq, &adapter->vc_event_task);
	taskqueue_drain_timeout(adapter->init_wq, &adapter->init_task);

	adapter->flags |= (1u << IDPF_HR_RESET_IN_PROG);
	idpf_detach_and_close(adapter);

	idpf_vport_ctrl_lock(adapter);
	idpf_vc_core_deinit(adapter);
	idpf_deinit_dflt_mbx(adapter);
	idpf_vport_ctrl_unlock(adapter);
}

/**
 * idpf_if_suspend - ifdi_suspend() implementation
 * @ctx: iflib context
 *
 * Return: 0.
 */
int
idpf_if_suspend(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);

	if (np->adapter != NULL)
		idpf_reset_prepare(np->adapter);

	return (0);
}

/**
 * idpf_if_resume - ifdi_resume() implementation
 * @ctx: iflib context
 *
 * Recovery runs through the normal reset path so that suspend/resume and a
 * device-asserted reset converge on the same code.
 *
 * Return: 0.
 */
int
idpf_if_resume(if_ctx_t ctx)
{
	struct idpf_netdev_priv *np = iflib_get_softc(ctx);
	struct idpf_adapter *adapter = np->adapter;

	if (adapter == NULL)
		return (0);

	adapter->flags |= (1u << IDPF_PCI_CB_RESET);
	taskqueue_enqueue_timeout(adapter->vc_event_wq, &adapter->vc_event_task,
	    idpf_msecs_to_ticks(300));

	return (0);
}

/**
 * idpf_reset_recover - rebuild the driver after a reset
 * @adapter: driver private data
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_reset_recover(struct idpf_adapter *adapter)
{
	device_t dev = idpf_adapter_to_dev(adapter);
	int waited;
	int err;

	err = idpf_init_dflt_mbx(adapter);
	if (err != 0) {
		device_printf(dev,
		    "failed to initialize default mailbox: %d\n", err);
		return (err);
	}

	if (!adapter->vcxn_mngr->active)
		idpf_vc_xn_init(adapter->vcxn_mngr);

	callout_reset(&adapter->serv_task,
	    idpf_msecs_to_ticks(5 * (pci_get_function(dev) & 0x07)),
	    idpf_service_task, adapter);

	err = idpf_vc_core_init(adapter);
	if (err != 0)
		goto init_err;

	/*
	 * Hold the reset until every vport exists, otherwise an ioctl can
	 * reach a half-built one.  Bounded: this runs in the attach thread, so
	 * waiting forever would take the machine with it.
	 */
	for (waited = 0; waited < IDPF_RESET_SETTLE_MS; waited += 100) {
		if ((adapter->flags & (1u << IDPF_HR_RESET_IN_PROG)) == 0)
			break;
		pause("idpfrec", idpf_msecs_to_ticks(100));
	}
	if ((adapter->flags & (1u << IDPF_HR_RESET_IN_PROG)) != 0) {
		device_printf(dev,
		    "vports did not settle within %d ms (flags 0x%x state %d)\n",
		    IDPF_RESET_SETTLE_MS, adapter->flags, adapter->state);
		adapter->flags &= ~(1u << IDPF_HR_RESET_IN_PROG);
		err = ETIMEDOUT;
		goto init_err;
	}

	return (0);

init_err:
	callout_drain(&adapter->serv_task);
	idpf_deinit_dflt_mbx(adapter);

	return (err);
}

/**
 * idpf_is_reset_detected - report whether the device is or was in reset
 * @adapter: driver private data
 *
 * Return: true when a reset is in progress or has happened.
 */
bool
idpf_is_reset_detected(struct idpf_adapter *adapter)
{
	struct idpf_ctlq_reg *reg;
	uint32_t arqlen;

	/* No need to check the reset state during a CORER. */
	if ((adapter->flags & (1u << IDPF_CORER_IN_PROG)) != 0)
		return (true);

	if (adapter->hw.arq == NULL)
		return (true);

	reg = &adapter->hw.arq->reg;
	arqlen = idpf_reg_rd32(idpf_get_mbx_reg_addr(adapter, reg->len));

	/* In reset when either the length or the enable bits are cleared. */
	return ((arqlen & reg->len_mask) == 0 ||
	    (arqlen & reg->len_ena_mask) == 0);
}
