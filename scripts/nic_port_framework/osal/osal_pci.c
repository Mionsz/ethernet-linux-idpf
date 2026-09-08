/*
 * osal_pci.c
 *
 * FreeBSD-oriented OS abstraction for PCI configuration-space access
 * and light PCI device control.
 */

#include "osal_pci.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/rman.h>

#include <machine/resource.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <sys/errno.h>

static device_t
nic_osal_get_pci_device(nic_osal_pci_device_t *pdev)
{
    if (pdev == NULL || pdev->os_private == NULL)
        return (device_t)NULL;

    return (device_t)pdev->os_private;
}

static int
nic_os_pci_validate(nic_osal_pci_device_t *pdev, device_t *dev_out)
{
    device_t dev;

    if (pdev == NULL || dev_out == NULL)
        return EINVAL;

    dev = nic_osal_get_pci_device(pdev);
    if (dev == (device_t)NULL)
        return ENODEV;

    *dev_out = dev;
    return 0;
}

int
nic_os_pci_enable(nic_osal_pci_device_t *pdev)
{
    device_t dev;
    int error;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    /*
     * Minimal FreeBSD-oriented enable semantics:
     * - validate device handle
     * - refresh cached identity fields for caller convenience
     */
    pdev->vendor_id = pci_get_vendor(dev);
    pdev->device_id = pci_get_device(dev);
    pdev->bus = (uint8_t)pci_get_bus(dev);
    pdev->device = (uint8_t)pci_get_slot(dev);
    pdev->function = (uint8_t)pci_get_function(dev);

    return 0;
}

int
nic_os_pci_disable(nic_osal_pci_device_t *pdev)
{
    device_t dev;
    int error;
    uint16_t cmd;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    /*
     * Conservative disable semantics for this OSAL:
     * clear bus mastering. Resource teardown remains outside this layer.
     */
    cmd = (uint16_t)pci_read_config(dev, PCIR_COMMAND, 2);
    cmd &= (uint16_t)~PCIM_CMD_BUSMASTEREN;
    pci_write_config(dev, PCIR_COMMAND, cmd, 2);

    return 0;
}

int
nic_os_pci_set_master(nic_osal_pci_device_t *pdev)
{
    device_t dev;
    int error;
    uint16_t cmd;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    cmd = (uint16_t)pci_read_config(dev, PCIR_COMMAND, 2);
    cmd |= PCIM_CMD_BUSMASTEREN;
    pci_write_config(dev, PCIR_COMMAND, cmd, 2);

    return 0;
}

int
nic_os_pci_read8(nic_osal_pci_device_t *pdev, uint32_t offset, uint8_t *value)
{
    device_t dev;
    int error;

    if (value == NULL)
        return EINVAL;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    *value = (uint8_t)pci_read_config(dev, offset, 1);
    return 0;
}

int
nic_os_pci_read16(nic_osal_pci_device_t *pdev, uint32_t offset, uint16_t *value)
{
    device_t dev;
    int error;

    if (value == NULL)
        return EINVAL;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    *value = (uint16_t)pci_read_config(dev, offset, 2);
    return 0;
}

int
nic_os_pci_read32(nic_osal_pci_device_t *pdev, uint32_t offset, uint32_t *value)
{
    device_t dev;
    int error;

    if (value == NULL)
        return EINVAL;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    *value = (uint32_t)pci_read_config(dev, offset, 4);
    return 0;
}

int
nic_os_pci_write8(nic_osal_pci_device_t *pdev, uint32_t offset, uint8_t value)
{
    device_t dev;
    int error;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    pci_write_config(dev, offset, value, 1);
    return 0;
}

int
nic_os_pci_write16(nic_osal_pci_device_t *pdev, uint32_t offset, uint16_t value)
{
    device_t dev;
    int error;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    pci_write_config(dev, offset, value, 2);
    return 0;
}

int
nic_os_pci_write32(nic_osal_pci_device_t *pdev, uint32_t offset, uint32_t value)
{
    device_t dev;
    int error;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    pci_write_config(dev, offset, value, 4);
    return 0;
}

int
nic_os_pci_get_bar(nic_osal_pci_device_t *pdev,
                   unsigned int bar,
                   nic_osal_phys_addr_t *base,
                   size_t *size)
{
    device_t dev;
    int error;
    int rid;
    rman_res_t start;
    rman_res_t bar_size;

    if (base == NULL || size == NULL)
        return EINVAL;
    if (bar > 5)
        return EINVAL;

    error = nic_os_pci_validate(pdev, &dev);
    if (error != 0)
        return error;

    /* BAR metadata comes from the resource list the PCI bus already parsed. */
    rid = PCIR_BAR(bar);
    if (bus_get_resource(dev, SYS_RES_MEMORY, rid, &start, &bar_size) != 0)
        return ENXIO;

    if (bar_size == 0)
        return ENXIO;

    *base = (nic_osal_phys_addr_t)start;
    *size = (size_t)bar_size;
    return 0;
}
