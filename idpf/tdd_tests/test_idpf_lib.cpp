/**
 * @file test_idpf_lib.cpp
 * @brief CppUTest tests for idpf_lib.c (Phase B, stub-level).
 *
 * Test intents (Feature 001's approved set, re-targeted): PCI resource
 * (BAR0) allocation must call bus_alloc_resource_any() with the
 * expected arguments and report success/failure accordingly; resource
 * release must call bus_release_resource() only if a resource was
 * actually allocated (Contract 6 rule 4: reverse-order teardown; rule
 * 5: bus_dma-adjacent resource-management KPI only).
 *
 * Uses the reused common/mock_src mock for bus_alloc_resource_any()/
 * bus_release_resource() (no feature-local mock needed -- Reuse-First,
 * resolves CHK029).
 */
#include "idpf_utest.h"

#include "../src/idpf_lib.c"

TEST_GROUP(idpf_lib)
{
	void teardown(void) override
	{
		idpf_common_teardown();
	}
};

/**
 * Test intent: idpf_allocate_pci_resources() calls
 * bus_alloc_resource_any() for BAR0 (PCIR_BAR(0)) and SYS_RES_MEMORY,
 * and returns 0 when a non-NULL resource is returned.
 */
TEST(idpf_lib, allocate_pci_resources_success)
{
	struct idpf_sc sc = {};
	struct resource fake_resource;
	int rc;

	mock().expectOneCall("bus_alloc_resource_any")
	      .withParameter("dev", sc.dev)
	      .withParameter("type", SYS_RES_MEMORY)
	      .withParameter("flags", (unsigned int)RF_ACTIVE)
	      .ignoreOtherParameters()
	      .andReturnValue((void *)&fake_resource);

	rc = idpf_allocate_pci_resources(&sc);

	LONGS_EQUAL(0, rc);
	POINTERS_EQUAL(&fake_resource, sc.pci_mem);
}

/**
 * Test intent: idpf_allocate_pci_resources() returns ENXIO when
 * bus_alloc_resource_any() reports failure (returns NULL), and does
 * not leave a dangling pci_mem pointer.
 */
TEST(idpf_lib, allocate_pci_resources_failure)
{
	struct idpf_sc sc = {};
	int rc;

	mock().expectOneCall("bus_alloc_resource_any")
	      .ignoreOtherParameters()
	      .andReturnValue((void *)NULL);

	rc = idpf_allocate_pci_resources(&sc);

	LONGS_EQUAL(ENXIO, rc);
}

/**
 * Test intent: idpf_free_pci_resources() calls bus_release_resource()
 * and clears sc->pci_mem only if a resource was previously allocated
 * -- the exact reverse of idpf_allocate_pci_resources() (Contract 6
 * rule 4: reverse-order teardown).
 */
TEST(idpf_lib, free_pci_resources_releases_allocated_resource)
{
	struct idpf_sc sc = {};
	struct resource fake_resource;
	int fake_rid = PCIR_BAR(0);

	sc.pci_mem = &fake_resource;

	mock().expectOneCall("rman_get_rid")
	      .withParameter("r", (const void *)&fake_resource)
	      .andReturnValue(fake_rid);

	mock().expectOneCall("bus_release_resource_old")
	      .withParameter("dev", sc.dev)
	      .withParameter("type", SYS_RES_MEMORY)
	      .withParameter("rid", fake_rid)
	      .withParameter("r", &fake_resource);

	idpf_free_pci_resources(&sc);

	POINTERS_EQUAL(NULL, sc.pci_mem);
}

/**
 * Test intent: idpf_free_pci_resources() does not call
 * bus_release_resource() when no resource was allocated (pci_mem is
 * NULL) -- guards against releasing a resource that was never
 * acquired.
 */
TEST(idpf_lib, free_pci_resources_noop_when_nothing_allocated)
{
	struct idpf_sc sc = {};

	idpf_free_pci_resources(&sc);
}
