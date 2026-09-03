/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Attach-path tests.
 *
 * idpf_main.c is included rather than linked so that its file-static helpers
 * are reachable; it is deliberately left out of SRCS in the test Makefile so
 * nothing is defined twice.
 *
 * ifdi_attach_pre() itself cannot be called without a real if_ctx_t and a real
 * PCI device, so what is covered here is everything underneath it that does
 * not need hardware: the deferred-work construction, the register-window
 * publication and the reachable error paths.
 */

#include "idpf_main.c"

#include "idpf_test.h"
#include "idpf_test_env.h"

static void
test_taskqueues_lifecycle(void)
{
	struct idpf_test_env env;
	struct idpf_adapter *adapter;

	if (idpf_test_env_setup(&env) != 0) {
		IDPF_EXPECT(false, "env setup failed");
		return;
	}
	adapter = env.adapter;

	IDPF_EXPECT_OK(idpf_alloc_taskqueues(adapter));

	IDPF_EXPECT_NOT_NULL(adapter->init_wq);
	IDPF_EXPECT_NOT_NULL(adapter->serv_wq);
	IDPF_EXPECT_NOT_NULL(adapter->mbx_wq);
	IDPF_EXPECT_NOT_NULL(adapter->vc_event_wq);

	/* subsystem_device_id 0 is neither Simics nor EMR, so silicon. */
	IDPF_EXPECT(IS_SILICON_DEVICE(adapter->hw.subsystem_device_id),
	    "fixture should classify as silicon");
	IDPF_EXPECT_NOT_NULL(adapter->stats_wq);

	idpf_free_taskqueues(adapter);

	IDPF_EXPECT_NULL(adapter->init_wq);
	IDPF_EXPECT_NULL(adapter->serv_wq);
	IDPF_EXPECT_NULL(adapter->mbx_wq);
	IDPF_EXPECT_NULL(adapter->stats_wq);
	IDPF_EXPECT_NULL(adapter->vc_event_wq);

	idpf_test_env_teardown(&env);
}

static void
test_taskqueues_no_stats_wq_on_simics(void)
{
	struct idpf_test_env env;
	struct idpf_adapter *adapter;

	if (idpf_test_env_setup(&env) != 0) {
		IDPF_EXPECT(false, "env setup failed");
		return;
	}
	adapter = env.adapter;
	adapter->hw.subsystem_device_id = IDPF_SUBDEV_ID_SIMICS;

	IDPF_EXPECT_OK(idpf_alloc_taskqueues(adapter));
	IDPF_EXPECT_NULL(adapter->stats_wq);
	IDPF_EXPECT_NOT_NULL(adapter->mbx_wq);

	idpf_free_taskqueues(adapter);
	idpf_test_env_teardown(&env);
}

static void
test_taskqueues_repeated_cycles(void)
{
	struct idpf_test_env env;
	int i, err = 0;

	if (idpf_test_env_setup(&env) != 0) {
		IDPF_EXPECT(false, "env setup failed");
		return;
	}

	for (i = 0; i < 5; i++) {
		err = idpf_alloc_taskqueues(env.adapter);
		if (err != 0)
			break;
		idpf_free_taskqueues(env.adapter);
	}
	IDPF_EXPECT_EQ(err, 0);
	IDPF_EXPECT_EQ(i, 5);

	idpf_test_env_teardown(&env);
}

static void
test_free_taskqueues_is_idempotent(void)
{
	struct idpf_test_env env;

	if (idpf_test_env_setup(&env) != 0) {
		IDPF_EXPECT(false, "env setup failed");
		return;
	}

	/* Detach can run before the queues exist; that must be harmless. */
	idpf_free_taskqueues(env.adapter);

	IDPF_EXPECT_OK(idpf_alloc_taskqueues(env.adapter));
	idpf_free_taskqueues(env.adapter);
	idpf_free_taskqueues(env.adapter);

	IDPF_EXPECT_NULL(env.adapter->init_wq);

	idpf_test_env_teardown(&env);
}

static void
test_cfg_hw_rejects_missing_bar(void)
{
	struct idpf_test_env env;

	if (idpf_test_env_setup(&env) != 0) {
		IDPF_EXPECT(false, "env setup failed");
		return;
	}

	env.adapter->dev_ops.static_reg_info[0] = NULL;
	IDPF_EXPECT_ERR(idpf_cfg_hw(env.adapter), ENXIO);

	idpf_test_env_teardown(&env);
}

static void
test_dev_ops_publish_windows(void)
{
	struct idpf_test_env env;
	struct idpf_adapter *adapter;

	if (idpf_test_env_setup(&env) != 0) {
		IDPF_EXPECT(false, "env setup failed");
		return;
	}
	adapter = env.adapter;

	idpf_dev_ops_init(adapter);
	IDPF_EXPECT(adapter->hw.mbx.addr_len != 0, "PF mbx window not published");
	IDPF_EXPECT(adapter->hw.rstat.addr_len != 0,
	    "PF rstat window not published");
	IDPF_EXPECT_NOT_NULL(adapter->dev_ops.reg_ops.ctlq_reg_init);
	IDPF_EXPECT_NOT_NULL(adapter->dev_ops.reg_ops.reset_reg_init);
	IDPF_EXPECT_NOT_NULL(adapter->dev_ops.reg_ops.trigger_reset);

	/*
	 * The VF path is deliberately not exercised here: idpf_vf_dev_ops_init()
	 * reaches idpf_vf_is_siov(), which calls pci_get_device().  The fixture's
	 * device is not a PCI device, so that reads absent ivars and faults.
	 * Covering the VF ops needs either a real PCI device or the userspace
	 * harness, where an unmocked accessor fails to link instead.
	 */

	idpf_test_env_teardown(&env);
}

static void
test_ctlq_reg_init_offsets_are_in_window(void)
{
	struct idpf_ctlq_create_info info[2];
	struct idpf_test_env env;
	struct idpf_adapter *adapter;

	if (idpf_test_env_setup(&env) != 0) {
		IDPF_EXPECT(false, "env setup failed");
		return;
	}
	adapter = env.adapter;
	bzero(info, sizeof(info));

	idpf_dev_ops_init(adapter);
	adapter->dev_ops.reg_ops.ctlq_reg_init(adapter, info);

	/*
	 * Every offset the mailbox will touch must land inside the window the
	 * device ops published, or attach would write outside BAR0.
	 */
	IDPF_EXPECT(idpf_reg_offset_in_region(&adapter->hw.mbx, info[0].reg.head),
	    "ASQ head 0x%x outside mbx window", info[0].reg.head);
	IDPF_EXPECT(idpf_reg_offset_in_region(&adapter->hw.mbx, info[0].reg.tail),
	    "ASQ tail 0x%x outside mbx window", info[0].reg.tail);
	IDPF_EXPECT(idpf_reg_offset_in_region(&adapter->hw.mbx, info[0].reg.len),
	    "ASQ len 0x%x outside mbx window", info[0].reg.len);
	IDPF_EXPECT(idpf_reg_offset_in_region(&adapter->hw.mbx, info[1].reg.head),
	    "ARQ head 0x%x outside mbx window", info[1].reg.head);
	IDPF_EXPECT(idpf_reg_offset_in_region(&adapter->hw.mbx, info[1].reg.tail),
	    "ARQ tail 0x%x outside mbx window", info[1].reg.tail);

	IDPF_EXPECT(info[0].reg.len_ena_mask != 0, "ASQ len enable mask unset");
	IDPF_EXPECT(info[1].reg.len_ena_mask != 0, "ARQ len enable mask unset");

	idpf_test_env_teardown(&env);
}

static const struct idpf_test_case attach_cases[] = {
	{ "taskqueues_lifecycle", test_taskqueues_lifecycle },
	{ "taskqueues_no_stats_wq_on_simics",
	  test_taskqueues_no_stats_wq_on_simics },
	{ "taskqueues_repeated_cycles", test_taskqueues_repeated_cycles },
	{ "free_taskqueues_is_idempotent", test_free_taskqueues_is_idempotent },
	{ "cfg_hw_rejects_missing_bar", test_cfg_hw_rejects_missing_bar },
	{ "dev_ops_publish_windows", test_dev_ops_publish_windows },
	{ "ctlq_reg_init_offsets_are_in_window",
	  test_ctlq_reg_init_offsets_are_in_window },
};

const struct idpf_test_suite idpf_test_suite_attach =
    IDPF_TEST_SUITE("attach", attach_cases);
