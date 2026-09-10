/**
 * @file idpf_txrx_common.h
 * @brief TX/RX common definitions for the idpf FreeBSD VF driver.
 *
 * Declares the OS-agnostic-shaped TX/RX placeholder surface shared by
 * the idpf iflib front-end and the TX/RX datapath file. No functional
 * datapath, no ring layout, and no hardware descriptor formats are
 * defined at this skeleton stage (spec.md S7 Phase A).
 */
#ifndef _IDPF_TXRX_COMMON_H_
#define _IDPF_TXRX_COMMON_H_

#include <sys/param.h>
#include <sys/types.h>

#include "idpf_drv.h"

int	    idpf_txrx_tx_queues_alloc(struct idpf_sc *sc, int ntxqs, int ntxqsets);
int	    idpf_txrx_rx_queues_alloc(struct idpf_sc *sc, int nrxqs, int nrxqsets);
void	idpf_txrx_queues_free(struct idpf_sc *sc);

#endif /* _IDPF_TXRX_COMMON_H_ */
