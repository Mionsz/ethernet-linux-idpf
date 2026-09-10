/**
 * @file test_idpf_vc.cpp
 * @brief CppUTest tests for idpf_vc.c (Phase B, stub-level).
 *
 * Test intent (Feature 001's approved set, re-targeted): the
 * control-plane iflib glue must correctly delegate attach/detach to
 * idpf_vc_common_init()/idpf_vc_common_deinit(), preserving the
 * reverse-order teardown relationship at the glue layer (Contract 6
 * rule 4).
 */
#include "idpf_utest.h"

#include "../src/idpf_vc.c"

TEST_GROUP(idpf_vc)
{
	void teardown(void) override
	{
		idpf_common_teardown();
	}
};

/**
 * Test intent: idpf_vc_glue_attach() delegates to
 * idpf_vc_common_init() and returns its result (0 at this skeleton
 * stage).
 */
TEST(idpf_vc, glue_attach_returns_success)
{
	struct idpf_sc sc;
	int rc;

	rc = idpf_vc_glue_attach(&sc);

	LONGS_EQUAL(0, rc);
}

/**
 * Test intent: idpf_vc_glue_detach() is callable after
 * idpf_vc_glue_attach() and does not crash, mirroring the underlying
 * idpf_vc_common_init()/idpf_vc_common_deinit() no-op pairing
 * (Contract 6 rule 4: reverse-order teardown).
 */
TEST(idpf_vc, glue_detach_is_noop_after_attach)
{
	struct idpf_sc sc;

	idpf_vc_glue_attach(&sc);
	idpf_vc_glue_detach(&sc);
}
