/*
 * idpf_osdep.c
 *
 * Driver-side hooks declared by idpf_osdep.h.
 *
 * The bridge header maps the porting agent's provisional `os_*()` names onto
 * the OSAL API, but a handful of them need per-adapter state the OSAL does not
 * own: which MMIO mapping backs a given hw handle, which IRQ object backs a
 * given MSI-X vector, and the device handle itself. Those live here.
 *
 * Registration is explicit: the driver calls idpf_osdep_attach() once its BARs
 * and vectors exist, and idpf_osdep_detach() on teardown.
 */

#include "idpf_osdep.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

/*
 * Single-adapter context.
 *
 * The ported call sites reach registers through bare mapped addresses with no
 * adapter argument, so the address-only accessors need a default mapping to
 * resolve against. One context per KLD instance is sufficient for idpf, which
 * binds one PF per device_t.
 */
struct idpf_osdep_ctx {
	nic_osal_device_t	 dev;
	nic_osal_pci_device_t	 pdev;
	nic_osal_mmio_t		 mmio[IDPF_OSDEP_MAX_BARS];
	unsigned int		 nbars;
	nic_osal_irq_t		*irqs;
	int			 nvectors;
	struct resource		**irq_res;
	int			*irq_rid;
	bool			 attached;
};

static struct idpf_osdep_ctx idpf_ctx;

static MALLOC_DEFINE(M_IDPF_OSDEP, "idpf_osdep", "IDPF OS abstraction state");

MALLOC_DEFINE(M_IDPF, "idpf", "Intel IDPF driver allocations");

/* ------------------------------------------------------------------------
 * Attach / detach
 * ------------------------------------------------------------------------ */

int
idpf_osdep_attach(device_t dev, const unsigned int *bars, unsigned int nbars)
{
	unsigned int i;
	int error;

	if (dev == NULL || bars == NULL || nbars == 0 ||
	    nbars > IDPF_OSDEP_MAX_BARS)
		return (EINVAL);
	if (idpf_ctx.attached)
		return (EBUSY);

	bzero(&idpf_ctx, sizeof(idpf_ctx));
	idpf_ctx.dev.os_private = dev;
	idpf_ctx.pdev.os_private = dev;
	idpf_ctx.pdev.vendor_id = pci_get_vendor(dev);
	idpf_ctx.pdev.device_id = pci_get_device(dev);
	idpf_ctx.pdev.bus = pci_get_bus(dev);
	idpf_ctx.pdev.device = pci_get_slot(dev);
	idpf_ctx.pdev.function = pci_get_function(dev);

	for (i = 0; i < nbars; i++) {
		error = nic_os_mmio_map_bar(&idpf_ctx.pdev, bars[i],
		    &idpf_ctx.mmio[i]);
		if (error != 0) {
			while (i-- > 0)
				nic_os_mmio_unmap(&idpf_ctx.mmio[i]);
			return (error);
		}
	}
	idpf_ctx.nbars = nbars;
	idpf_ctx.attached = true;

	return (0);
}

void
idpf_osdep_detach(void)
{
	unsigned int i;

	if (!idpf_ctx.attached)
		return;

	idpf_pci_free_msix(&idpf_ctx.pdev);

	for (i = 0; i < idpf_ctx.nbars; i++)
		nic_os_mmio_unmap(&idpf_ctx.mmio[i]);

	bzero(&idpf_ctx, sizeof(idpf_ctx));
}

/* ------------------------------------------------------------------------
 * Device / MMIO resolution
 * ------------------------------------------------------------------------ */

nic_osal_device_t *
idpf_osal_dev(void)
{
	return (idpf_ctx.attached ? &idpf_ctx.dev : NULL);
}

nic_osal_pci_device_t *
idpf_osal_pci_dev(void)
{
	return (idpf_ctx.attached ? &idpf_ctx.pdev : NULL);
}

nic_osal_mmio_t *
idpf_default_mmio(void)
{
	return (idpf_ctx.attached ? &idpf_ctx.mmio[0] : NULL);
}

/*
 * Ported code passes an opaque hw handle. idpf keeps all register access on
 * BAR 0, so the handle only has to select the context, not a mapping.
 */
nic_osal_mmio_t *
idpf_hw_to_mmio(void *hw)
{
	(void)hw;
	return (idpf_default_mmio());
}

/*
 * Ported code computes register addresses as `bar_base + offset` via
 * idpf_get_reg_addr(). Recover the offset by subtracting the mapped base.
 *
 * Returns 0 for an address outside every mapped BAR, which reads back as
 * register 0 rather than faulting; callers must only pass addresses obtained
 * from idpf_get_reg_addr().
 */
uint32_t
idpf_mmio_offset_of(const volatile void *addr)
{
	nic_osal_phys_addr_t base;
	uintptr_t a;
	size_t size;
	unsigned int i;

	a = (uintptr_t)(const volatile char *)addr;

	for (i = 0; i < idpf_ctx.nbars; i++) {
		if (nic_os_mmio_info(&idpf_ctx.mmio[i], &base, &size) != 0)
			continue;
		if (a >= (uintptr_t)base && a < (uintptr_t)base + size)
			return ((uint32_t)(a - (uintptr_t)base));
	}

	return (0);
}

/* ------------------------------------------------------------------------
 * MSI-X vectors
 * ------------------------------------------------------------------------ */

nic_osal_irq_t *
idpf_vector_to_irq(int vector)
{
	if (!idpf_ctx.attached || idpf_ctx.irqs == NULL)
		return (NULL);
	if (vector < 0 || vector >= idpf_ctx.nvectors)
		return (NULL);

	return (&idpf_ctx.irqs[vector]);
}

int
idpf_pci_alloc_msix(nic_osal_pci_device_t *pdev, int min_vectors, int want)
{
	device_t dev;
	int count, i;

	if (pdev == NULL || pdev->os_private == NULL)
		return (-EINVAL);
	if (want < min_vectors || min_vectors < 1)
		return (-EINVAL);
	if (idpf_ctx.irqs != NULL)
		return (-EBUSY);

	dev = (device_t)pdev->os_private;

	count = want;
	if (pci_alloc_msix(dev, &count) != 0)
		return (-ENXIO);
	if (count < min_vectors) {
		pci_release_msi(dev);
		return (-ENOSPC);
	}

	idpf_ctx.irqs = malloc(sizeof(*idpf_ctx.irqs) * count, M_IDPF_OSDEP,
	    M_WAITOK | M_ZERO);
	idpf_ctx.irq_res = malloc(sizeof(*idpf_ctx.irq_res) * count,
	    M_IDPF_OSDEP, M_WAITOK | M_ZERO);
	idpf_ctx.irq_rid = malloc(sizeof(*idpf_ctx.irq_rid) * count,
	    M_IDPF_OSDEP, M_WAITOK | M_ZERO);

	for (i = 0; i < count; i++) {
		/* MSI-X RIDs are 1-based on FreeBSD. */
		idpf_ctx.irq_rid[i] = i + 1;
		idpf_ctx.irq_res[i] = bus_alloc_resource_any(dev, SYS_RES_IRQ,
		    &idpf_ctx.irq_rid[i], RF_ACTIVE);
		if (idpf_ctx.irq_res[i] == NULL)
			break;
		if (nic_os_irq_prealloc(&idpf_ctx.irqs[i],
		    idpf_ctx.irq_res[i], idpf_ctx.irq_rid[i], i) != 0)
			break;
	}

	if (i < count) {
		idpf_ctx.nvectors = i;
		idpf_pci_free_msix(pdev);
		return (-ENXIO);
	}

	idpf_ctx.nvectors = count;

	return (count);
}

void
idpf_pci_free_msix(nic_osal_pci_device_t *pdev)
{
	device_t dev;
	int i;

	if (pdev == NULL || pdev->os_private == NULL)
		return;

	dev = (device_t)pdev->os_private;

	for (i = 0; i < idpf_ctx.nvectors; i++) {
		if (idpf_ctx.irqs != NULL)
			nic_os_irq_release(&idpf_ctx.irqs[i]);
		if (idpf_ctx.irq_res != NULL && idpf_ctx.irq_res[i] != NULL)
			bus_release_resource(dev, SYS_RES_IRQ,
			    idpf_ctx.irq_rid[i], idpf_ctx.irq_res[i]);
	}

	free(idpf_ctx.irqs, M_IDPF_OSDEP);
	free(idpf_ctx.irq_res, M_IDPF_OSDEP);
	free(idpf_ctx.irq_rid, M_IDPF_OSDEP);
	idpf_ctx.irqs = NULL;
	idpf_ctx.irq_res = NULL;
	idpf_ctx.irq_rid = NULL;

	if (idpf_ctx.nvectors > 0) {
		pci_release_msi(dev);
		idpf_ctx.nvectors = 0;
	}
}
