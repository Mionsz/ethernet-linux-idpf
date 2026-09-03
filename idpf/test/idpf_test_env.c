/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sx.h>

#include <machine/bus.h>

#include "idpf.h"
#include "idpf_test.h"
#include "idpf_test_env.h"

/**
 * idpf_test_borrow_dev - find a device to hang DMA tags off
 *
 * bus_get_dma_tag() dispatches on the device's PARENT, so root_bus is not
 * usable here - it has none.  A PCI bus device is used instead; config space
 * is never touched, only the DMA tag lineage.
 *
 * Return: a device_t, or NULL if no suitable bus exists.
 */
static device_t
idpf_test_borrow_dev(void)
{
	static const char *const busnames[] = { "pci", "nexus" };
	unsigned int i;
	devclass_t dc;
	device_t dev;

	for (i = 0; i < nitems(busnames); i++) {
		dc = devclass_find(busnames[i]);
		if (dc == NULL)
			continue;
		dev = devclass_get_device(dc, 0);
		if (dev != NULL && device_get_parent(dev) != NULL)
			return (dev);
	}

	return (NULL);
}

/**
 * idpf_test_env_setup - build a fake adapter
 * @env: fixture to populate
 *
 * Mirrors the parts of idpf_if_attach_pre() that do not need a PCI device:
 * locks, the register windows and the transaction manager.
 *
 * Return: 0 on success, otherwise an errno.
 */
int
idpf_test_env_setup(struct idpf_test_env *env)
{
	struct idpf_adapter *adapter;

	bzero(env, sizeof(*env));

	env->regfile = malloc(IDPF_TEST_REGFILE_SIZE, M_DEVBUF,
	    M_NOWAIT | M_ZERO);
	if (env->regfile == NULL)
		return (ENOMEM);

	adapter = malloc(sizeof(*adapter), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (adapter == NULL) {
		free(env->regfile, M_DEVBUF);
		env->regfile = NULL;
		return (ENOMEM);
	}

	adapter->dev = idpf_test_borrow_dev();
	if (adapter->dev == NULL) {
		free(adapter, M_DEVBUF);
		free(env->regfile, M_DEVBUF);
		env->regfile = NULL;
		return (ENXIO);
	}
	adapter->drv_name = IDPF_DRV_NAME;
	adapter->drv_ver = IDPF_DRV_VER;

	sx_init(&adapter->vport_ctrl_lock, "idpf_t_vport");
	sx_init(&adapter->vector_lock, "idpf_t_vector");
	sx_init(&adapter->queue_lock, "idpf_t_queue");
	mtx_init(&adapter->corer_done_lock, "idpf_t_corer", NULL, MTX_DEF);
	cv_init(&adapter->corer_done_cv, "idpf_t_corer");
	mtx_init(&adapter->adi_info.priv_lock, "idpf_t_adi", NULL, MTX_DEF);
	TAILQ_INIT(&adapter->adi_info.priv_list);

	/* Both windows alias the same buffer; offsets keep them apart. */
	adapter->hw.back = adapter;
	adapter->hw.mbx.vaddr = env->regfile;
	adapter->hw.mbx.addr_start = 0;
	adapter->hw.mbx.addr_len = IDPF_TEST_REGFILE_SIZE;
	adapter->hw.rstat.vaddr = env->regfile;
	adapter->hw.rstat.addr_start = 0;
	adapter->hw.rstat.addr_len = IDPF_TEST_REGFILE_SIZE;

	env->adapter = adapter;

	return (0);
}

/**
 * idpf_test_env_teardown - release everything idpf_test_env_setup() built
 * @env: fixture to tear down
 */
void
idpf_test_env_teardown(struct idpf_test_env *env)
{
	struct idpf_adapter *adapter = env->adapter;

	if (adapter == NULL)
		goto free_regs;

	mtx_destroy(&adapter->adi_info.priv_lock);
	cv_destroy(&adapter->corer_done_cv);
	mtx_destroy(&adapter->corer_done_lock);
	sx_destroy(&adapter->queue_lock);
	sx_destroy(&adapter->vector_lock);
	sx_destroy(&adapter->vport_ctrl_lock);

	free(adapter, M_DEVBUF);
	env->adapter = NULL;

free_regs:
	free(env->regfile, M_DEVBUF);
	env->regfile = NULL;
}

uint32_t
idpf_test_reg_get(struct idpf_test_env *env, uint32_t off)
{

	KASSERT(off + sizeof(uint32_t) <= IDPF_TEST_REGFILE_SIZE,
	    ("idpf_test: register offset 0x%x out of range", off));

	return (le32toh(*(volatile uint32_t *)(env->regfile + off)));
}

void
idpf_test_reg_set(struct idpf_test_env *env, uint32_t off, uint32_t val)
{

	KASSERT(off + sizeof(uint32_t) <= IDPF_TEST_REGFILE_SIZE,
	    ("idpf_test: register offset 0x%x out of range", off));

	*(volatile uint32_t *)(env->regfile + off) = htole32(val);
}
