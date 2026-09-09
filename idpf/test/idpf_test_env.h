/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

/*
 * Shared fixture for the idpf test suites.
 *
 * The register file is ordinary memory: struct idpf_hw reaches MMIO only
 * through mbx.vaddr/rstat.vaddr, so pointing those at a malloc'd buffer makes
 * every wr32()/rd32() in the driver land somewhere the simulated control plane
 * can read.  DMA stays real - bus_dma against the host bridge - so descriptor
 * rings have genuine physical addresses and genuine alignment constraints.
 */

#ifndef _IDPF_TEST_ENV_H_
#define _IDPF_TEST_ENV_H_

#include "idpf.h"

/* Large enough for every mailbox/reset offset the register maps use. */
#define IDPF_TEST_REGFILE_SIZE	(64 * 1024)

struct idpf_test_env {
	struct idpf_adapter	*adapter;
	uint8_t			*regfile;
	bool			 taskqueues;
};

int idpf_test_env_setup(struct idpf_test_env *env);
void idpf_test_env_teardown(struct idpf_test_env *env);

/* Raw access to the simulated register file, bypassing the driver's helpers. */
uint32_t idpf_test_reg_get(struct idpf_test_env *env, uint32_t off);
void idpf_test_reg_set(struct idpf_test_env *env, uint32_t off, uint32_t val);

#endif /* _IDPF_TEST_ENV_H_ */
