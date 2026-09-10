/**
 * @file idpf_drv.h
 * @brief Main driver context structure for the idpf FreeBSD VF driver.
 *
 * Declares the software context ("softc") structure used to track
 * per-device state for the idpf iflib driver. This is a minimal,
 * stub-level skeleton: only structural fields required for iflib
 * attach/detach scaffolding and the control-plane task are present.
 * No hardware register definitions, no queue/ring state, and no
 * virtchnl2 message structures are populated here.
 */
#ifndef _IDPF_DRV_H_
#define _IDPF_DRV_H_

#include <sys/param.h>
#include <sys/types.h>
#include <sys/bus.h>
#include <machine/bus.h>
#include <sys/rman.h>
#include <sys/taskqueue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <machine/resource.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/iflib.h>

/*
 * struct idpf_hw is owned by the shared code (idpf/shared submodule).
 * Pulling it in here keeps a single definition shared by driver and shared
 * code.
 */
#include "idpf_controlq_api.h"
#include "idpf_xn.h"
#include "virtchnl2.h"

/**
 * @name Driver lifecycle state flags (data-model.md §2)
 * @brief Serialized acquisition milestones on `struct idpf_sc.state`; each
 * gates its matching acquisition and reverse-order teardown step (FR-008).
 * Accessed only via atomic helpers.
 * @{
 */
#define IDPF_STATE_BAR0_MAPPED		(1U << 0) /**< BAR0 mapped. */
#define IDPF_STATE_CQ_INITIALIZED	(1U << 1) /**< ControlQ + idpf_xn ready. */
#define IDPF_STATE_VERSION_DONE		(1U << 2) /**< OP_VERSION completed. */
#define IDPF_STATE_CAPS_DONE		(1U << 3) /**< OP_GET_CAPS completed. */
#define IDPF_STATE_VPORT_CREATED	(1U << 4) /**< OP_CREATE_VPORT completed. */
#define IDPF_STATE_ATTACHED		(1U << 5) /**< Attach fully complete. */
/** @} */

/**
 * @struct idpf_sc
 * @brief Main context structure ("softc") for the idpf VF driver.
 *
 * Minimal skeleton-level software context. Only the fields required to
 * scaffold PCI resource attach/detach and the control-plane task are
 * present; queue/ring state and virtchnl2 message structures are
 * intentionally deferred to later, traceable expansion.
 */
struct idpf_sc {
	/**
	 * OS-shim hardware-access primitive; MUST remain the first
	 * member of this structure (FR-010, US1-AS-3, SC-003).
	 */
	struct idpf_hw		hw;
	device_t			dev;		/**< Bus device_t for this instance. */
	if_ctx_t			ctx;		/**< iflib context handle. */
	if_shared_ctx_t		sctx;		/**< iflib shared context pointer. */
	if_softc_ctx_t		scctx;		/**< iflib per-instance softc context. */

	struct resource		*pci_mem;	/**< Mapped PCI BAR resource. */

	/** Driver state flags; access only via atomic functions. */
	uint32_t			state;

	/**
	 * ControlQ transaction manager (data-model.md §4). A pointer, not an
	 * embedded value: idpf_ctlq_xn_init() allocates the manager itself and
	 * publishes it through its OUT param, which idpf_init_controlq() stores
	 * here. NULL until ControlQ init succeeds; cleared on teardown.
	 */
	struct idpf_ctlq_xn_manager	*xnm;

	/** Cached OP_GET_CAPS response (data-model.md §5; idpf_get_caps). */
	struct virtchnl2_get_capabilities	caps;

	/** Cached OP_CREATE_VPORT response (FR-020; idpf_create_vport). */
	struct virtchnl2_create_vport	vport;
	/** CP-assigned primary vport id, from the CREATE_VPORT response. */
	uint32_t			vport_id;

	/** Control-plane (virtchnl2) message-processing task. */
	struct task			vc_task;
	/** Taskqueue backing vc_task. */
	struct taskqueue	*vc_tq;
	/** Mutex protecting the control-plane task's shared state. */
	struct mtx			vc_mtx;
	/** Name string for vc_mtx (see mtx_init(9)). */
	char				vc_mtx_name[16];
};

#endif /* _IDPF_DRV_H_ */
