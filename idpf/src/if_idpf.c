/**
 * @file if_idpf.c
 * @brief iflib front-end for the idpf FreeBSD VF driver.
 *
 * Contains the main entry point for the iflib driver implementation.
 * Wires the 27 approved ifdi_* method stubs into the iflib method
 * table and registers the module/driver values needed to load an
 * iflib driver. No functional datapath, no PF content, and no
 * Virtchnl2 message-set implementation is present at this skeleton
 * stage.
 *
 * Modeled structurally on the out-of-tree iavf reference
 * (../../iavf/iavf/src/CORE/if_iavf_iflib.c), reduced to stub level per
 * this feature's own approved scope.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/errno.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net/iflib.h>

#include "ifdi_if.h"

#include "idpf_drv.h"
#include "idpf_lib.h"
#include "idpf_osdep.h"
#include "idpf_txrx_common.h"
#include "idpf_vc_common.h"

/**
 * @brief PCI vendor ID for this driver's device match table.
 *
 * Intel's PCI-SIG-assigned vendor ID -- a public, well-known constant,
 * not device-specific (already-validated; Constant Validation Ledger).
 */
#define IDPF_INTEL_VENDOR_ID	0x8086

/**
 * @brief PCI device ID for the IDPF Physical Function (PF).
 *
 * Recorded as parallel architectural context only -- this feature
 * targets the VF driver (IDPF_DEV_ID_VF below), not the PF. Not used
 * in this file's own device match table.
 *
 * Fresh-Co-Design-required (Constant Validation Ledger): confirmed via
 * the MGV (Morganville) project's "Device ID Strategy" wiki page (LAN and
 * RDMA data path PF for XHC PF, ACC, IMC; branding "Intel(R) Infrastructure Data
 * Path Function") and independently corroborated by a DPDK code
 * reference (drivers/common/idpf/base/idpf_devids.h: \#define
 * IDPF_DEV_ID_PF 0x1452).
 */
#define IDPF_DEV_ID_PF	0x1452

/**
 * @brief PCI device ID for this driver's device match table (VF).
 *
 * The operative production constant for this IDPF VF feature line.
 *
 * Fresh-Co-Design-required (Constant Validation Ledger): confirmed via
 * the MGV (Morganville) project's "Device ID Strategy" wiki page (LAN and
 * RDMA data path for XHC VF; branding "Intel(R) Infrastructure Data Path
 * Function"), which lists 0x145C distinctly from the PF's 0x1452 --
 * "The VF and PF have different device IDs but the same branding."
 * Supersedes the prior unresolved IDPF_DEVICE_ID_PLACEHOLDER.
 */
#define IDPF_DEV_ID_VF	0x145C

/*********************************************************************
 *  Function prototypes
 *********************************************************************/
static void	*idpf_register(device_t dev);

static int	idpf_if_attach_pre(if_ctx_t ctx);
static int	idpf_if_attach_post(if_ctx_t ctx);
static int	idpf_if_detach(if_ctx_t ctx);
static void	idpf_attach_unwind(struct idpf_sc *sc);
static int	idpf_if_shutdown(if_ctx_t ctx);
static int	idpf_if_suspend(if_ctx_t ctx);
static int	idpf_if_resume(if_ctx_t ctx);
static void	idpf_if_init(if_ctx_t ctx);
static void	idpf_if_stop(if_ctx_t ctx);
static int	idpf_if_msix_intr_assign(if_ctx_t ctx, int msix);
static void	idpf_if_intr_enable(if_ctx_t ctx);
static void	idpf_if_intr_disable(if_ctx_t ctx);
static int	idpf_if_rx_queue_intr_enable(if_ctx_t ctx, uint16_t rxqid);
static int	idpf_if_tx_queue_intr_enable(if_ctx_t ctx, uint16_t txqid);
static int	idpf_if_tx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs,
		    uint64_t *paddrs, int ntxqs, int ntxqsets);
static int	idpf_if_rx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs,
		    uint64_t *paddrs, int nrxqs, int nrxqsets);
static void	idpf_if_queues_free(if_ctx_t ctx);
static void	idpf_if_update_admin_status(if_ctx_t ctx);
static void	idpf_if_multi_set(if_ctx_t ctx);
static int	idpf_if_mtu_set(if_ctx_t ctx, uint32_t mtu);
static void	idpf_if_media_status(if_ctx_t ctx, struct ifmediareq *ifmr);
static int	idpf_if_media_change(if_ctx_t ctx);
static int	idpf_if_promisc_set(if_ctx_t ctx, int flags);
static void	idpf_if_timer(if_ctx_t ctx, uint16_t qid);
static void	idpf_if_vlan_register(if_ctx_t ctx, uint16_t vtag);
static void	idpf_if_vlan_unregister(if_ctx_t ctx, uint16_t vtag);
static uint64_t	idpf_if_get_counter(if_ctx_t ctx, ift_counter cnt);
static bool	idpf_if_needs_restart(if_ctx_t ctx, enum iflib_restart_event event);

/**
 * @brief Pure VLAN-tag validity predicate (spec.md S5 items #24/#25
 * ifdi_vlan_register/ifdi_vlan_unregister; Phase C second increment).
 * Not yet wired into either call site -- see idpf_if_vlan_register()/
 * idpf_if_vlan_unregister() below, which remain unconditional no-ops.
 */
static bool	idpf_vlan_tag_is_valid(uint16_t vtag);

/*********************************************************************
 *  Device match table
 *********************************************************************/

/**
 * @var idpf_vendor_info_array
 * @brief PCI vendor/device match table for this driver (VF only).
 */
static pci_vendor_info_t idpf_vendor_info_array[] = {
	PVID(IDPF_INTEL_VENDOR_ID, IDPF_DEV_ID_VF,
	    "Intel(R) Infrastructure Data Path Function VF"),
	PVID_END
};

/*********************************************************************
 *  FreeBSD Device Interface Entry Points
 *********************************************************************/

/**
 * @var idpf_methods
 * @brief device methods for the idpf module (device_t layer)
 */
static device_method_t idpf_methods[] = {
	DEVMETHOD(device_register, idpf_register),
	DEVMETHOD(device_probe, iflib_device_probe),
	DEVMETHOD(device_attach, iflib_device_attach),
	DEVMETHOD(device_detach, iflib_device_detach),
	DEVMETHOD(device_shutdown, iflib_device_shutdown),
	DEVMETHOD_END
};

/**
 * @var idpf_driver
 * @brief device_t-layer driver descriptor for the idpf module.
 */
static driver_t idpf_driver = {
	"idpf", idpf_methods, sizeof(struct idpf_sc),
};

/**
 * @brief Registers the idpf device_t-layer driver module with the
 * FreeBSD kernel's newbus/module framework.
 */
DRIVER_MODULE(idpf, pci, idpf_driver, 0, 0);

/**
 * @brief Declares the module version for the idpf driver.
 */
MODULE_VERSION(idpf, 1);

/**
 * @brief Declares the idpf module's dependency on the pci bus driver.
 */
MODULE_DEPEND(idpf, pci, 1, 1, 1);

/**
 * @brief Declares the idpf module's dependency on the ether framework.
 */
MODULE_DEPEND(idpf, ether, 1, 1, 1);

/**
 * @brief Declares the idpf module's dependency on the iflib framework.
 */
MODULE_DEPEND(idpf, iflib, 1, 1, 1);

/**
 * @brief Registers this driver's PCI vendor/device match table with
 * iflib's plug-and-play info mechanism.
 */
IFLIB_PNP_INFO(pci, idpf, idpf_vendor_info_array);

/**
 * @var idpf_if_methods
 * @brief the 27 approved ifdi_* methods (spec.md S5), all stub-level
 */
static device_method_t idpf_if_methods[] = {
	DEVMETHOD(ifdi_attach_pre, idpf_if_attach_pre),
	DEVMETHOD(ifdi_attach_post, idpf_if_attach_post),
	DEVMETHOD(ifdi_detach, idpf_if_detach),
	DEVMETHOD(ifdi_shutdown, idpf_if_shutdown),
	DEVMETHOD(ifdi_suspend, idpf_if_suspend),
	DEVMETHOD(ifdi_resume, idpf_if_resume),
	DEVMETHOD(ifdi_init, idpf_if_init),
	DEVMETHOD(ifdi_stop, idpf_if_stop),
	DEVMETHOD(ifdi_msix_intr_assign, idpf_if_msix_intr_assign),
	DEVMETHOD(ifdi_intr_enable, idpf_if_intr_enable),
	DEVMETHOD(ifdi_intr_disable, idpf_if_intr_disable),
	DEVMETHOD(ifdi_rx_queue_intr_enable, idpf_if_rx_queue_intr_enable),
	DEVMETHOD(ifdi_tx_queue_intr_enable, idpf_if_tx_queue_intr_enable),
	DEVMETHOD(ifdi_tx_queues_alloc, idpf_if_tx_queues_alloc),
	DEVMETHOD(ifdi_rx_queues_alloc, idpf_if_rx_queues_alloc),
	DEVMETHOD(ifdi_queues_free, idpf_if_queues_free),
	DEVMETHOD(ifdi_update_admin_status, idpf_if_update_admin_status),
	DEVMETHOD(ifdi_multi_set, idpf_if_multi_set),
	DEVMETHOD(ifdi_mtu_set, idpf_if_mtu_set),
	DEVMETHOD(ifdi_media_status, idpf_if_media_status),
	DEVMETHOD(ifdi_media_change, idpf_if_media_change),
	DEVMETHOD(ifdi_promisc_set, idpf_if_promisc_set),
	DEVMETHOD(ifdi_timer, idpf_if_timer),
	DEVMETHOD(ifdi_vlan_register, idpf_if_vlan_register),
	DEVMETHOD(ifdi_vlan_unregister, idpf_if_vlan_unregister),
	DEVMETHOD(ifdi_get_counter, idpf_if_get_counter),
	DEVMETHOD(ifdi_needs_restart, idpf_if_needs_restart),
	DEVMETHOD_END
};

/**
 * @var idpf_if_driver
 * @brief iflib-layer driver descriptor for the idpf module.
 */
static driver_t idpf_if_driver = {
	"idpf_if", idpf_if_methods, sizeof(struct idpf_sc)
};

/**
 * @var idpf_sctx
 * @brief shared context for the idpf iflib driver
 *
 * Only structural/generic iflib scaffolding fields are populated here
 * (queue-set counts, alignment, driver/vendor-table pointers). No
 * hardware-magnitude constant (max frame size, ring min/max/default,
 * DMA segment limits) is populated at this skeleton stage -- those are
 * genuinely hardware-facing values requiring Co-Design citation before
 * they may be introduced (spec.md S6.8; Contract 3), deferred to a
 * later, traceable expansion.
 */
static struct if_shared_ctx idpf_sctx = {
	.isc_magic = IFLIB_MAGIC,
	.isc_driver = &idpf_if_driver,
	.isc_q_align = PAGE_SIZE,
	.isc_admin_intrcnt = 1,
	.isc_vendor_info = idpf_vendor_info_array,
	.isc_driver_version = __DECONST(char *, "0.0.1-skeleton"),
	.isc_nfl = 1,
	.isc_ntxqs = 1,
	.isc_nrxqs = 1,
	.isc_flags = IFLIB_IS_VF,
};

/*** Functions ***/

/**
 * @brief iflib callback to obtain the shared context pointer.
 *
 * Called by iflib when the driver is first attached, to obtain a
 * pointer to the shared context structure describing device features.
 *
 * @param dev The device being registered (unused at skeleton stage).
 *
 * @return A pointer to the idpf shared context structure.
 */
static void *
idpf_register(device_t dev __unused)
{
	return (&idpf_sctx);
}

/**
 * @brief Reverse-order teardown of whatever attach milestones are set.
 *
 * Shared by the attach-failure unwind path (idpf_if_attach_pre) and the full
 * detach path (idpf_if_detach), per contracts Rule 2. Inspects the state
 * bitfield and, in exact reverse order, releases only the resources actually
 * acquired: DESTROY_VPORT iff a vport exists, RESET_VF + ControlQ deinit iff
 * the mailbox came up, BAR0 release iff mapped. Mailbox teardown messages are
 * best-effort (FR-003); every step is state-gated and idempotent, so it is
 * safe to call from any partial-attach state with no double-free.
 *
 * @param sc Driver software context for the device instance.
 */
static void
idpf_attach_unwind(struct idpf_sc *sc)
{
	if (sc->state & IDPF_STATE_VPORT_CREATED) {
		(void)idpf_destroy_vport(sc);
		atomic_clear_32(&sc->state, IDPF_STATE_VPORT_CREATED);
	}
	if (sc->state & IDPF_STATE_CQ_INITIALIZED) {
		(void)idpf_reset_vf(sc);
		idpf_deinit_controlq(sc);
		atomic_clear_32(&sc->state, IDPF_STATE_CQ_INITIALIZED |
		    IDPF_STATE_VERSION_DONE | IDPF_STATE_CAPS_DONE);
	}
	if (sc->state & IDPF_STATE_BAR0_MAPPED) {
		idpf_free_pci_resources(sc);
		atomic_clear_32(&sc->state, IDPF_STATE_BAR0_MAPPED);
	}
}

/**
 * @brief Pre-initialization attach hook.
 *
 * Acquires the device bring-up milestones in the exact forward order
 * (contracts Rule 1), so idpf_if_detach() can release them in reverse
 * (Contract 6 rule 4): BAR0 map -> ControlQ/idpf_xn init -> VERSION ->
 * GET_CAPS -> CREATE_VPORT. Two VF-reset gates guard the mailbox: the
 * VFGEN_RSTAT reset-complete poll (FR-018) runs before any mailbox
 * register access, and the VF_ATQLEN.ATQENABLE FLR check (FR-017) runs
 * after ControlQ init but before the first VIRTCHNL2 message. No
 * OP_ALLOC_VECTORS is issued (FR-002, FR-007). Each of VERSION/GET_CAPS/
 * CREATE_VPORT sets its own state flag on success. Any failure runs
 * idpf_attach_unwind() to release the partial state before returning (US5).
 *
 * @param ctx iflib software context.
 *
 * @return 0 on success; a propagated errno from the first failing step.
 */
static int
idpf_if_attach_pre(if_ctx_t ctx)
{
	struct idpf_sc *sc = (struct idpf_sc *)iflib_get_softc(ctx);
	int rc;

	sc->ctx = ctx;
	sc->dev = iflib_get_dev(ctx);
	sc->sctx = iflib_get_sctx(ctx);
	sc->scctx = iflib_get_softc_ctx(ctx);

	/* 1. Map BAR0. */
	rc = idpf_allocate_pci_resources(sc);
	if (rc != 0) {
		device_printf(sc->dev, "idpf: BAR0 map failed: %d\n", rc);
		idpf_attach_unwind(sc);
		return (rc);
	}
	device_printf(sc->dev, "idpf: BAR0 mapped\n");
	atomic_set_32(&sc->state, IDPF_STATE_BAR0_MAPPED);

	/* FR-018: gate on VF reset completion before any mailbox reg access. */
	rc = idpf_wait_reset_complete(sc);
	if (rc != 0) {
		device_printf(sc->dev,
		    "idpf: VF reset did not complete: %d\n", rc);
		idpf_attach_unwind(sc);
		return (rc);
	}

	/* 2. Initialize the ControlQ and idpf_xn transaction manager. */
	rc = idpf_init_controlq(sc);
	if (rc != 0) {
		idpf_attach_unwind(sc);
		return (rc);
	}
	atomic_set_32(&sc->state, IDPF_STATE_CQ_INITIALIZED);

	/* FR-017: detect an in-flight FLR before the first mailbox message. */
	rc = idpf_check_flr(sc);
	if (rc != 0) {
		device_printf(sc->dev,
		    "idpf: FLR in progress (ATQ disabled)\n");
		idpf_attach_unwind(sc);
		return (rc);
	}

	/* 3. Negotiate the VIRTCHNL2 version (sets VERSION_DONE). */
	rc = idpf_send_version(sc);
	if (rc != 0) {
		idpf_attach_unwind(sc);
		return (rc);
	}

	/* 4. Fetch device capabilities (sets CAPS_DONE). */
	rc = idpf_get_caps(sc);
	if (rc != 0) {
		idpf_attach_unwind(sc);
		return (rc);
	}

	/* 5. Create the single default vport (sets VPORT_CREATED). */
	rc = idpf_create_vport(sc);
	if (rc != 0) {
		idpf_attach_unwind(sc);
		return (rc);
	}

	return (0);
}

/**
 * @brief Post-initialization attach hook.
 *
 * Verifies that all five idpf_if_attach_pre() milestones completed, then
 * initializes the OS-shim layer and the control-plane task as the final
 * steps of attach and marks the device fully attached (IDPF_STATE_ATTACHED).
 * No OP_ALLOC_VECTORS dependency exists (FR-002, FR-007). Teardown occurs in
 * exact reverse order (Contract 6 rule 4).
 *
 * @param ctx iflib software context.
 *
 * @return 0 on success, ENXIO if a prior milestone is missing, or a
 *         propagated idpf_vc_glue_attach() error code.
 */
static int
idpf_if_attach_post(if_ctx_t ctx)
{
	struct idpf_sc *sc = (struct idpf_sc *)iflib_get_softc(ctx);
	const uint32_t need = IDPF_STATE_BAR0_MAPPED |
	    IDPF_STATE_CQ_INITIALIZED | IDPF_STATE_VERSION_DONE |
	    IDPF_STATE_CAPS_DONE | IDPF_STATE_VPORT_CREATED;
	int rc;

	if ((sc->state & need) != need) {
		device_printf(sc->dev,
		    "idpf: attach_post precondition not met (state=0x%x)\n",
		    sc->state);
		return (ENXIO);
	}

	idpf_osdep_init(sc);

	rc = idpf_vc_glue_attach(sc);
	if (rc != 0)
		return (rc);

	atomic_set_32(&sc->state, IDPF_STATE_ATTACHED);
	return (0);
}

/**
 * @brief Detach hook.
 *
 * Tears down in the exact reverse order of attach (contracts Rule 2):
 * clear IDPF_STATE_ATTACHED, DESTROY_VPORT (iff a vport was created),
 * RESET_VF unconditionally (iff the mailbox came up), stop the VC glue and
 * deinitialize the ControlQ/idpf_xn, then release BAR0 last. The two mailbox
 * teardown messages are best-effort: a failure is logged and detach still
 * completes local teardown (FR-003).
 *
 * @param ctx iflib software context.
 *
 * @return 0 always.
 */
static int
idpf_if_detach(if_ctx_t ctx)
{
	struct idpf_sc *sc = (struct idpf_sc *)iflib_get_softc(ctx);

	/* 1. Mark detaching. */
	atomic_clear_32(&sc->state, IDPF_STATE_ATTACHED);

	/* 2. Tear down the attach_post resources (VC glue, OS shim) first. */
	idpf_vc_glue_detach(sc);
	idpf_osdep_deinit(sc);

	/* 3. Reverse-order teardown of the attach_pre milestones (shared US5). */
	idpf_attach_unwind(sc);

	return (0);
}

/**
 * @brief Shutdown hook. Stub-level: performs the same interrupt
 * quiescing idpf_if_stop() would, without full detach.
 *
 * @param ctx iflib software context.
 *
 * @return 0 always, at this skeleton stage.
 */
static int
idpf_if_shutdown(if_ctx_t ctx __unused)
{
	return (0);
}

/**
 * @brief Suspend hook. Stub-level, no-op.
 *
 * @param ctx iflib software context.
 *
 * @return 0 always, at this skeleton stage.
 */
static int
idpf_if_suspend(if_ctx_t ctx __unused)
{
	return (0);
}

/**
 * @brief Resume hook. Stub-level, no-op.
 *
 * @param ctx iflib software context.
 *
 * @return 0 always, at this skeleton stage.
 */
static int
idpf_if_resume(if_ctx_t ctx __unused)
{
	return (0);
}

/**
 * @brief Init hook (interface up). Stub-level, no-op.
 *
 * @param ctx iflib software context.
 */
static void
idpf_if_init(if_ctx_t ctx __unused)
{
}

/**
 * @brief Stop hook (interface down). Stub-level, no-op.
 *
 * @param ctx iflib software context.
 */
static void
idpf_if_stop(if_ctx_t ctx __unused)
{
}

/**
 * @brief MSI-X interrupt assignment hook. Stub-level, no-op.
 *
 * @param ctx  iflib software context.
 * @param msix Number of MSI-X vectors assigned by iflib.
 *
 * @return 0 always, at this skeleton stage.
 */
static int
idpf_if_msix_intr_assign(if_ctx_t ctx __unused, int msix __unused)
{
	return (0);
}

/**
 * @brief Enable all interrupts. Stub-level, no-op.
 *
 * @param ctx iflib software context.
 */
static void
idpf_if_intr_enable(if_ctx_t ctx __unused)
{
}

/**
 * @brief Disable all interrupts. Stub-level, no-op.
 *
 * Paired with idpf_if_intr_enable() -- must run before
 * idpf_if_queues_free() once real interrupt handlers exist
 * (Contract 6 rule 7: interrupt-teardown ordering).
 *
 * @param ctx iflib software context.
 */
static void
idpf_if_intr_disable(if_ctx_t ctx __unused)
{
}

/**
 * @brief Enable one RX queue's interrupt.
 *
 * Phase C, fourth increment (spec.md S5 item #12; execution-review-
 * package.md S2a). Returns ENOTSUP rather than falsely claiming
 * success, since idpf does not implement any real per-queue interrupt
 * enable logic yet (no queues/interrupts exist at this skeleton
 * stage). Directly modeled on FreeBSD's own canonical generic default
 * for this exact method, defined in the real kernel's own
 * sys/net/ifdi_if.m ("null_queue_intr_enable"), which returns ENOTSUP
 * unconditionally for any driver that does not override it. This is a
 * pure return-code correction, not a queue-index bounds check -- no
 * real hardware queue-count fact is involved or required.
 *
 * @param ctx   iflib software context.
 * @param rxqid RX queue index.
 *
 * @return ENOTSUP always, at this skeleton stage.
 */
static int
idpf_if_rx_queue_intr_enable(if_ctx_t ctx __unused, uint16_t rxqid __unused)
{
	return (ENOTSUP);
}

/**
 * @brief Enable one TX queue's interrupt.
 *
 * Phase C, fifth increment (spec.md S5 item #13; execution-review-
 * package.md S2a). Symmetric to idpf_if_rx_queue_intr_enable():
 * returns ENOTSUP rather than falsely claiming success, since idpf
 * does not implement any real per-queue interrupt enable logic yet.
 * The real kernel's own sys/net/ifdi_if.m declares the identical
 * DEFAULT null_queue_intr_enable clause for both tx_queue_intr_enable
 * and rx_queue_intr_enable, so the same proven framework precedent
 * applies here for the same reason.
 *
 * @param ctx   iflib software context.
 * @param txqid TX queue index.
 *
 * @return ENOTSUP always, at this skeleton stage.
 */
static int
idpf_if_tx_queue_intr_enable(if_ctx_t ctx __unused, uint16_t txqid __unused)
{
	return (ENOTSUP);
}

/**
 * @brief Allocate TX queue resources. Delegates to idpf_txrx.c.
 *
 * @param ctx      iflib software context.
 * @param vaddrs   Unused at this skeleton stage.
 * @param paddrs   Unused at this skeleton stage.
 * @param ntxqs    Number of TX queues requested by iflib.
 * @param ntxqsets Number of TX queue sets requested by iflib.
 *
 * @return 0 on success.
 */
static int
idpf_if_tx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs __unused,
    uint64_t *paddrs __unused, int ntxqs, int ntxqsets)
{
	struct idpf_sc *sc = (struct idpf_sc *)iflib_get_softc(ctx);

	return (idpf_txrx_tx_queues_alloc(sc, ntxqs, ntxqsets));
}

/**
 * @brief Allocate RX queue resources. Delegates to idpf_txrx.c.
 *
 * @param ctx      iflib software context.
 * @param vaddrs   Unused at this skeleton stage.
 * @param paddrs   Unused at this skeleton stage.
 * @param nrxqs    Number of RX queues requested by iflib.
 * @param nrxqsets Number of RX queue sets requested by iflib.
 *
 * @return 0 on success.
 */
static int
idpf_if_rx_queues_alloc(if_ctx_t ctx, caddr_t *vaddrs __unused,
    uint64_t *paddrs __unused, int nrxqs, int nrxqsets)
{
	struct idpf_sc *sc = (struct idpf_sc *)iflib_get_softc(ctx);

	return (idpf_txrx_rx_queues_alloc(sc, nrxqs, nrxqsets));
}

/**
 * @brief Free TX/RX queue resources. Delegates to idpf_txrx.c.
 *
 * Must run after idpf_if_intr_disable() once real interrupts exist
 * (Contract 6 rule 7: interrupt-teardown ordering).
 *
 * @param ctx iflib software context.
 */
static void
idpf_if_queues_free(if_ctx_t ctx)
{
	struct idpf_sc *sc = (struct idpf_sc *)iflib_get_softc(ctx);

	idpf_txrx_queues_free(sc);
}

/**
 * @brief Poll/update admin-queue status. Stub-level, no-op.
 *
 * @param ctx iflib software context.
 */
static void
idpf_if_update_admin_status(if_ctx_t ctx __unused)
{
}

/**
 * @brief Update multicast filter list. Stub-level, no-op.
 *
 * @param ctx iflib software context.
 */
static void
idpf_if_multi_set(if_ctx_t ctx __unused)
{
}

/**
 * @brief Set interface MTU.
 *
 * Phase C, first increment (spec.md S5 item #19; execution-review-
 * package.md S2a). Rejects MTU values below ETHERMIN -- a generic,
 * universal FreeBSD/IEEE-802.3 constant from net/ethernet.h, not a
 * hardware/vendor-specific value (already-validated, no Co-Design
 * citation required). Any value at or above ETHERMIN is still
 * accepted unconditionally, matching the original stub behavior for
 * all realistic MTU requests -- no hardware-facing upper bound (e.g.
 * a device-specific max frame size) is introduced here.
 *
 * @param ctx iflib software context.
 * @param mtu Requested MTU value.
 *
 * @return 0 on success, EINVAL if mtu is below ETHERMIN.
 */
static int
idpf_if_mtu_set(if_ctx_t ctx __unused, uint32_t mtu)
{
	if (mtu < ETHERMIN)
		return (EINVAL);

	return (0);
}

/**
 * @brief Report media status. Stub-level: reports link down.
 *
 * @param ctx  iflib software context.
 * @param ifmr Media status/request structure to populate.
 */
static void
idpf_if_media_status(if_ctx_t ctx __unused, struct ifmediareq *ifmr)
{
	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;
}

/**
 * @brief Handle media change request. Stub-level, no-op.
 *
 * @param ctx iflib software context.
 *
 * @return 0 always, at this skeleton stage.
 */
static int
idpf_if_media_change(if_ctx_t ctx __unused)
{
	return (0);
}

/**
 * @brief Set promiscuous mode. Stub-level, no-op.
 *
 * @param ctx   iflib software context.
 * @param flags Requested promiscuous-mode flags.
 *
 * @return 0 always, at this skeleton stage.
 */
static int
idpf_if_promisc_set(if_ctx_t ctx __unused, int flags __unused)
{
	return (0);
}

/**
 * @brief Per-queue periodic timer callback. Stub-level, no-op.
 *
 * @param ctx iflib software context.
 * @param qid Queue index.
 */
static void
idpf_if_timer(if_ctx_t ctx __unused, uint16_t qid __unused)
{
}

/**
 * @brief Register a VLAN tag. Stub-level, no-op.
 *
 * @param ctx  iflib software context.
 * @param vtag VLAN tag to register.
 */
static void
idpf_if_vlan_register(if_ctx_t ctx __unused, uint16_t vtag __unused)
{
}

/**
 * @brief Unregister a VLAN tag. Stub-level, no-op.
 *
 * @param ctx  iflib software context.
 * @param vtag VLAN tag to unregister.
 */
static void
idpf_if_vlan_unregister(if_ctx_t ctx __unused, uint16_t vtag __unused)
{
}

/**
 * @brief Report a driver statistics counter.
 *
 * Phase C, third increment (spec.md S5 item #26; execution-review-
 * package.md S2a). Delegates every counter request to the generic,
 * non-hardware-specific iflib/OS-level if_get_counter_default()
 * fallback, since idpf does not yet track any real per-VSI hardware
 * statistics (no queue/hardware state exists at this skeleton stage).
 * Directly modeled on the proven iavf/iavf/src/CORE/if_iavf_iflib.c
 * reference's own "default: return (if_get_counter_default(ifp,
 * cnt));" fallback case, generalized to every counter identifier
 * since idpf has no per-counter special cases of its own yet.
 *
 * @param ctx iflib software context.
 * @param cnt Counter identifier requested by iflib.
 *
 * @return The generic OS-level default accounting value for cnt.
 */
static uint64_t
idpf_if_get_counter(if_ctx_t ctx, ift_counter cnt)
{
	return (if_get_counter_default(iflib_get_ifp(ctx), cnt));
}

/**
 * @brief Report whether a restart is needed for a given event.
 * Stub-level: never requests a restart.
 *
 * @param ctx   iflib software context.
 * @param event The restart-triggering event type.
 *
 * @return false always, at this skeleton stage.
 */
static bool
idpf_if_needs_restart(if_ctx_t ctx __unused,
    enum iflib_restart_event event __unused)
{
	return (false);
}

/**
 * @brief Pure VLAN-tag validity predicate.
 *
 * Phase C, second increment (spec.md S5 items #24/#25; execution-
 * review-package.md S2a). Rejects VLAN tag 0 (reserved for untagged/
 * priority-tagged traffic per IEEE 802.1Q) and any value above
 * EVL_VLID_MASK (0x0FFF/4095, the 12-bit VLAN ID field width) -- both
 * generic, universal FreeBSD/IEEE-802.1Q constants from
 * net/ethernet.h, not hardware/vendor-specific (already-validated, no
 * Co-Design citation required). Directly modeled on the proven
 * iavf/iavf/src/CORE/if_iavf_iflib.c reference's own
 * "if ((vtag == 0) || (vtag > 4095)) return;" boundary check. Not yet
 * wired into idpf_if_vlan_register()/idpf_if_vlan_unregister(), which
 * remain unconditional no-ops -- that wiring is a separate, deferred
 * increment.
 *
 * @param vtag The 802.1Q VLAN tag to validate.
 *
 * @return true if vtag is a valid, non-reserved 802.1Q VLAN ID
 *         (1-4095 inclusive), false otherwise.
 */
static bool
idpf_vlan_tag_is_valid(uint16_t vtag)
{
	return (vtag != 0 && vtag <= EVL_VLID_MASK);
}
