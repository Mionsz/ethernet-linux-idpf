/**
 * @file test_idpf_txrx.cpp
 * @brief CppUTest tests for idpf_txrx.c (Phase B, stub-level).
 *
 * Test intents (Feature 001's approved set, re-targeted): TX/RX queue
 * allocation must report success without performing real allocation;
 * queue free must be callable and side-effect-free, mirroring the
 * alloc functions (Contract 6 rule 4: reverse-order teardown).
 */
#include "idpf_utest.h"

#include "../src/idpf_txrx.c"

TEST_GROUP(idpf_txrx)
{
	void teardown(void) override
	{
		idpf_common_teardown();
	}
};

/**
 * Test intent: idpf_txrx_tx_queues_alloc() returns success (0) for a
 * representative queue-count request, without allocating any real
 * bus_dma-backed resource at this skeleton stage.
 */
TEST(idpf_txrx, tx_queues_alloc_returns_success)
{
	struct idpf_sc sc;
	int rc;

	rc = idpf_txrx_tx_queues_alloc(&sc, /*ntxqs=*/1, /*ntxqsets=*/1);

	LONGS_EQUAL(0, rc);
}

/**
 * Test intent: idpf_txrx_rx_queues_alloc() returns success (0) for a
 * representative queue-count request, without allocating any real
 * bus_dma-backed resource at this skeleton stage.
 */
TEST(idpf_txrx, rx_queues_alloc_returns_success)
{
	struct idpf_sc sc;
	int rc;

	rc = idpf_txrx_rx_queues_alloc(&sc, /*nrxqs=*/1, /*nrxqsets=*/1);

	LONGS_EQUAL(0, rc);
}

/**
 * Test intent: idpf_txrx_queues_free() is callable and does not crash
 * after either alloc function above, mirroring their no-op behavior
 * (structural reverse-order-teardown symmetry, Contract 6 rule 4).
 */
TEST(idpf_txrx, queues_free_is_noop)
{
	struct idpf_sc sc;

	idpf_txrx_queues_free(&sc);
}
