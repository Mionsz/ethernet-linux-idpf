/**
 * @file idpf_txrx.c
 * @brief TX/RX datapath placeholder for the idpf FreeBSD VF driver.
 *
 * Implements the stub-level TX/RX queue allocation/free surface
 * declared in idpf_txrx_common.h. No functional datapath, no ring
 * layout, and no bus_dma tag/map creation exists yet at this skeleton
 * stage (spec.md S7 Phase A) -- real bus_dma usage is introduced by a
 * later, traceable expansion (Contract 6 rule 5: bus_dma-only).
 */

#include "idpf_txrx_common.h"

/**
 * @brief Allocate TX queue resources for one device instance.
 *
 * Stub-level placeholder for Phase A: returns success without
 * allocating any real queue/ring resources or bus_dma tags.
 *
 * @param sc       Driver software context for the device instance.
 * @param ntxqs    Number of TX queues requested by iflib.
 * @param ntxqsets Number of TX queue sets requested by iflib.
 *
 * @return 0 on success.
 */
int
idpf_txrx_tx_queues_alloc(struct idpf_sc *sc __unused, int ntxqs __unused,
    int ntxqsets __unused)
{
	return (0);
}

/**
 * @brief Allocate RX queue resources for one device instance.
 *
 * Stub-level placeholder for Phase A: returns success without
 * allocating any real queue/ring resources or bus_dma tags.
 *
 * @param sc       Driver software context for the device instance.
 * @param nrxqs    Number of RX queues requested by iflib.
 * @param nrxqsets Number of RX queue sets requested by iflib.
 *
 * @return 0 on success.
 */
int
idpf_txrx_rx_queues_alloc(struct idpf_sc *sc __unused, int nrxqs __unused,
    int nrxqsets __unused)
{
	return (0);
}

/**
 * @brief Free TX/RX queue resources for one device instance.
 *
 * Stub-level placeholder for Phase A: no-op, since neither alloc
 * function above yet allocates any real resource to free. Declared for
 * structural symmetry (Contract 6 rule 4: reverse-order teardown).
 *
 * @param sc Driver software context for the device instance.
 */
void
idpf_txrx_queues_free(struct idpf_sc *sc __unused)
{
}
