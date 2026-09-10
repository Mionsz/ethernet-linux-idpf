/**
 * @file idpf_lib.c
 * @brief Core setup/lifecycle implementation for the idpf FreeBSD VF driver.
 *
 * Implements the PCI resource allocation/release pair used by the
 * iflib front-end's attach/detach path. Maps BAR0 via the native
 * bus_alloc_resource_any()/bus_release_resource() resource-management
 * KPI pair; introduces no hardware register access, no queue/ring
 * state, and no DMA memory allocation at this skeleton stage.
 */

#include "idpf_lib.h"
#include "idpf_osdep.h"
#include "idpf_lan_vf_regs.h"
#include "idpf_mmg_v_policy.h"

/**
 * @brief Allocate PCI resources for one device instance.
 *
 * Maps BAR0 as a memory resource using bus_alloc_resource_any(). This
 * is the only resource acquired at this skeleton stage; its release is
 * paired with idpf_free_pci_resources() in exact reverse order
 * (Contract 6 rule 4).
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 on success, ENXIO if BAR0 could not be mapped.
 */
int
idpf_allocate_pci_resources(struct idpf_sc *sc)
{
	int	pci_mem_rid;

	pci_mem_rid = PCIR_BAR(0);
	sc->pci_mem = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY,
	    &pci_mem_rid, RF_ACTIVE);

	if (sc->pci_mem == NULL)
		return (ENXIO);

	return (0);
}

/**
 * @brief Release PCI resources for one device instance.
 *
 * Releases BAR0 only if idpf_allocate_pci_resources() successfully
 * mapped it -- the exact reverse of that function's acquisition order
 * (Contract 6 rule 4: reverse-order teardown).
 *
 * @param sc Driver software context for the device instance.
 */
void
idpf_free_pci_resources(struct idpf_sc *sc)
{
	if (sc->pci_mem != NULL) {
		bus_release_resource(sc->dev, SYS_RES_MEMORY,
		    rman_get_rid(sc->pci_mem), sc->pci_mem);
		sc->pci_mem = NULL;
	}
}

/**
 * @brief Poll for VF reset completion before touching mailbox registers.
 *
 * FR-018: after BAR0 is mapped but before any mailbox register access, the
 * VF must wait for its reset to reach an operational state. VFGEN_RSTAT's
 * VFR_STATE field reports In-Progress (00b), Completed (01b), or VF-Active
 * (10b); attach may proceed on Completed or Active. Fails closed with
 * ETIMEDOUT if the bounded poll budget is exhausted.
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 once the VF reset is Completed/Active, ETIMEDOUT on exhaustion.
 */
int
idpf_wait_reset_complete(struct idpf_sc *sc)
{
	unsigned int i;

	for (i = 0; i < IDPF_MMG_V_RESET_POLL_MAX; i++) {
		uint32_t rstat = rd32(&sc->hw, VFGEN_RSTAT);
		uint32_t state = (rstat & VFGEN_RSTAT_VFR_STATE_M) >>
		    VFGEN_RSTAT_VFR_STATE_S;

		if (state == IDPF_MMG_V_VFR_STATE_COMPLETED ||
		    state == IDPF_MMG_V_VFR_STATE_ACTIVE)
			return (0);
		idpf_msec_delay(IDPF_MMG_V_RESET_POLL_MS);
	}
	return (ETIMEDOUT);
}

/**
 * @brief Detect an in-flight Function Level Reset before the first message.
 *
 * FR-017: after ControlQ init programs and enables the ATQ, but before the
 * first VERSION message, the driver re-reads VF_ATQLEN. Hardware clears the
 * ATQENABLE bit when an FLR is in progress, so a cleared bit means the queue
 * is no longer usable and attach must abort.
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 if the ATQ is enabled, ENXIO if an FLR has disabled it.
 */
int
idpf_check_flr(struct idpf_sc *sc)
{
	uint32_t atqlen = rd32(&sc->hw, VF_ATQLEN);

	if ((atqlen & VF_ATQLEN_ATQENABLE_M) == 0)
		return (ENXIO);
	return (0);
}

/**
 * @brief Initialize the VF ControlQ and idpf_xn transaction manager (T019).
 *
 * Programs the two default VF mailbox queues -- ATQ (mailbox TX) and ARQ
 * (mailbox RX) -- and hands them to the shared idpf_ctlq_xn_init(), which
 * internally calls idpf_ctlq_init() (allocating the descriptor rings) and
 * allocates the transaction manager, publishing it through its OUT param.
 * The published manager pointer is stored in sc->xnm for the VIRTCHNL2
 * bring-up wrappers. The PCI identity fields on sc->hw (data-model.md S3) are
 * populated from the bus so shared code and diagnostics can read them.
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 on success, EIO if the shared ControlQ/xn init fails.
 */
int
idpf_init_controlq(struct idpf_sc *sc)
{
	struct idpf_ctlq_create_info	cq_info[2];
	struct idpf_ctlq_xn_init_params	xn_params;
	int				err;

	/* Record PCI identity for shared code / diagnostics (data-model.md S3). */
	sc->hw.device_id = pci_get_device(sc->dev);
	sc->hw.vendor_id = pci_get_vendor(sc->dev);
	sc->hw.subsystem_device_id = pci_get_subdevice(sc->dev);
	sc->hw.subsystem_vendor_id = pci_get_subvendor(sc->dev);
	sc->hw.revision_id = pci_get_revid(sc->dev);

	memset(cq_info, 0, sizeof(cq_info));

	/* ATQ -- mailbox TX (default mailbox: id == -1). */
	cq_info[0].type = IDPF_CTLQ_TYPE_MAILBOX_TX;
	cq_info[0].id = -1;
	cq_info[0].len = IDPF_MMG_V_MBX_Q_LEN;
	cq_info[0].buf_size = IDPF_DFLT_MBX_BUF_SIZE;
	cq_info[0].reg.head = VF_ATQH;
	cq_info[0].reg.tail = VF_ATQT;
	cq_info[0].reg.len = VF_ATQLEN;
	cq_info[0].reg.bah = VF_ATQBAH;
	cq_info[0].reg.bal = VF_ATQBAL;
	cq_info[0].reg.len_mask = VF_ATQLEN_ATQLEN_M;
	cq_info[0].reg.len_ena_mask = VF_ATQLEN_ATQENABLE_M;
	cq_info[0].reg.head_mask = VF_ATQH_ATQH_M;

	/* ARQ -- mailbox RX (default mailbox: id == -1). */
	cq_info[1].type = IDPF_CTLQ_TYPE_MAILBOX_RX;
	cq_info[1].id = -1;
	cq_info[1].len = IDPF_MMG_V_MBX_Q_LEN;
	cq_info[1].buf_size = IDPF_DFLT_MBX_BUF_SIZE;
	cq_info[1].reg.head = VF_ARQH;
	cq_info[1].reg.tail = VF_ARQT;
	cq_info[1].reg.len = VF_ARQLEN;
	cq_info[1].reg.bah = VF_ARQBAH;
	cq_info[1].reg.bal = VF_ARQBAL;
	cq_info[1].reg.len_mask = VF_ARQLEN_ARQLEN_M;
	cq_info[1].reg.len_ena_mask = VF_ARQLEN_ARQENABLE_M;
	cq_info[1].reg.head_mask = VF_ARQH_ARQH_M;

	memset(&xn_params, 0, sizeof(xn_params));
	xn_params.num_qs = 2;
	xn_params.cctlq_info = cq_info;
	xn_params.hw = &sc->hw;
	xn_params.ctx = sc;

	err = idpf_ctlq_xn_init(&xn_params);
	if (err != 0) {
		device_printf(sc->dev,
		    "idpf: ControlQ/xn init failed: %d\n", err);
		return (EIO);
	}
	sc->xnm = xn_params.xnm;
	return (0);
}

/**
 * @brief Tear down the ControlQ/idpf_xn transaction manager (T019).
 *
 * Reverse of idpf_init_controlq(): releases the shared transaction manager
 * and its ControlQ rings, then clears sc->xnm. Safe to call when ControlQ
 * init never ran (sc->xnm == NULL).
 *
 * @param sc Driver software context for the device instance.
 */
void
idpf_deinit_controlq(struct idpf_sc *sc)
{
	struct idpf_ctlq_xn_init_params	xn_params;

	if (sc->xnm == NULL)
		return;

	memset(&xn_params, 0, sizeof(xn_params));
	xn_params.hw = &sc->hw;
	xn_params.xnm = sc->xnm;
	idpf_ctlq_xn_deinit(&xn_params);
	sc->xnm = NULL;
}
