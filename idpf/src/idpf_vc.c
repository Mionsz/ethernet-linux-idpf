/**
 * @file idpf_vc.c
 * @brief Control-plane iflib glue for the idpf FreeBSD VF driver.
 *
 * Bridges the iflib front-end's attach/detach lifecycle to the
 * control-plane message-processing task declared in idpf_vc_common.h.
 * No functional virtchnl2 message handling exists yet -- this file
 * only wires the init/deinit calls into the correct, reverse-order
 * lifecycle position (Contract 6 rule 4).
 */

#include "idpf_vc_common.h"

/**
 * @brief Attach-time entry point for the control-plane iflib glue.
 *
 * Calls idpf_vc_common_init() as the last step of attach, so that its
 * paired teardown (idpf_vc_glue_detach()) is the first step of detach --
 * the reverse-order teardown discipline (Contract 6 rule 4).
 *
 * @param sc Driver software context for the device instance.
 *
 * @return 0 on success, a idpf_vc_common_init()-compatible error code
 *         on failure.
 */
int
idpf_vc_glue_attach(struct idpf_sc *sc)
{
	return (idpf_vc_common_init(sc));
}

/**
 * @brief Detach-time entry point for the control-plane iflib glue.
 *
 * This function is intentionally a thin wrapper today. Its purpose is to
 * own the iflib-specific preconditions and sequencing around
 * idpf_vc_common_deinit(). As the driver matures, this is the correct
 * location for:
 *
 *   - asserting iflib resource preconditions (BAR mapped, MSI-X allocated)
 *   - performing iflib-specific VC state teardown before common deinit
 *   - handling iflib detach error paths that differ from the common teardown
 *
 * Do NOT collapse this into a direct call to idpf_vc_common_deinit() from
 * the iflib detach path. The boundary between iflib lifecycle and virtchnl2
 * protocol must remain explicit and independently testable.
 *
 * @param sc Driver software context for the device instance.
 */
void
idpf_vc_glue_detach(struct idpf_sc *sc)
{
	idpf_vc_common_deinit(sc);
}
