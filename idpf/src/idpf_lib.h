/**
 * @file idpf_lib.h
 * @brief Core setup/lifecycle declarations for the idpf FreeBSD VF driver.
 *
 * Declares the PCI resource allocation/release helpers and other
 * lifecycle-support functions used by the iflib front-end
 * (if_idpf.c). No hardware register access and no queue/ring setup is
 * implemented at this skeleton stage (spec.md S7 Phase A).
 */
#ifndef _IDPF_LIB_H_
#define _IDPF_LIB_H_

#include <sys/param.h>
#include <sys/types.h>

#include "idpf_drv.h"


int	    idpf_allocate_pci_resources(struct idpf_sc *sc);
void	idpf_free_pci_resources(struct idpf_sc *sc);
/**
 * ControlQ + idpf_xn transaction-manager bring-up/teardown (T019).
 * idpf_init_controlq() programs the two VF mailbox queues (ATQ/ARQ) and
 * initializes the shared transaction manager, storing the published
 * manager pointer in sc->xnm. idpf_deinit_controlq() is its reverse.
 */
int         idpf_init_controlq(struct idpf_sc *sc);
void        idpf_deinit_controlq(struct idpf_sc *sc);

/**
 * VF reset/FLR gates guarding mailbox bring-up.
 * idpf_wait_reset_complete() (FR-018) polls VFGEN_RSTAT for a Completed/
 * Active VF-reset state before any mailbox register access, failing with
 * ETIMEDOUT on exhaustion. idpf_check_flr() (FR-017) returns ENXIO if the
 * ATQ has been disabled by an in-flight Function Level Reset.
 */
int         idpf_wait_reset_complete(struct idpf_sc *sc);
int         idpf_check_flr(struct idpf_sc *sc);
#endif /* _IDPF_LIB_H_ */
