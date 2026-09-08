/*
 * osal_mmio.c
 *
 * FreeBSD-oriented OS abstraction for PCI BAR MMIO mapping and register access.
 */

#include "osal_mmio.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <sys/errno.h>

struct nic_os_mmio_priv {
    device_t            dev;
    struct resource    *res;
    int                 rid;
    unsigned int        bar;
    bus_space_tag_t     bst;
    bus_space_handle_t  bsh;
    bus_addr_t          base;
    bus_size_t          size;
};

static device_t
nic_osal_get_pci_device(nic_osal_pci_device_t *pdev)
{
    if (pdev == NULL || pdev->os_private == NULL)
        return (device_t)NULL;

    return (device_t)pdev->os_private;
}

static struct nic_os_mmio_priv *
nic_os_mmio_get_priv(nic_osal_mmio_t *mmio)
{
    if (mmio == NULL || mmio->os_private == NULL)
        return NULL;

    return (struct nic_os_mmio_priv *)mmio->os_private;
}

int
nic_os_mmio_map_bar(nic_osal_pci_device_t *pdev,
                    unsigned int bar,
                    nic_osal_mmio_t *mmio)
{
    device_t dev;
    struct nic_os_mmio_priv *priv;
    int rid;

    if (pdev == NULL || mmio == NULL)
        return EINVAL;
    if (bar > 5)
        return EINVAL;
    if (mmio->os_private != NULL)
        return EBUSY;

    dev = nic_osal_get_pci_device(pdev);
    if (dev == (device_t)NULL)
        return ENODEV;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    rid = PCIR_BAR(bar);
    priv->dev = dev;
    priv->rid = rid;
    priv->bar = bar;

    priv->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &priv->rid,
                                       RF_ACTIVE);
    if (priv->res == NULL) {
        device_printf(dev,
            "osal_mmio: failed to map BAR%u (rid=%d)\n", bar, rid);
        free(priv, M_DEVBUF);
        return ENXIO;
    }

    priv->bst = rman_get_bustag(priv->res);
    priv->bsh = rman_get_bushandle(priv->res);
    priv->base = rman_get_start(priv->res);
    priv->size = rman_get_size(priv->res);

    if (priv->size == 0) {
        device_printf(dev,
            "osal_mmio: BAR%u mapped with zero size\n", bar);
        bus_release_resource(dev, SYS_RES_MEMORY, priv->rid, priv->res);
        free(priv, M_DEVBUF);
        return ENXIO;
    }

    mmio->os_private = priv;

    device_printf(dev,
        "osal_mmio: mapped BAR%u base=%#jx size=%#jx\n",
        bar, (uintmax_t)priv->base, (uintmax_t)priv->size);

    return 0;
}

void
nic_os_mmio_unmap(nic_osal_mmio_t *mmio)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return;

    if (priv->res != NULL && priv->dev != (device_t)NULL)
        bus_release_resource(priv->dev, SYS_RES_MEMORY, priv->rid, priv->res);

    if (priv->dev != (device_t)NULL) {
        device_printf(priv->dev,
            "osal_mmio: unmapped BAR%u\n", priv->bar);
    }

    free(priv, M_DEVBUF);
    mmio->os_private = NULL;
}

int
nic_os_mmio_info(nic_osal_mmio_t *mmio,
                 nic_osal_phys_addr_t *base,
                 size_t *size)
{
    struct nic_os_mmio_priv *priv;

    if (mmio == NULL || base == NULL || size == NULL)
        return EINVAL;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return EINVAL;

    *base = (nic_osal_phys_addr_t)priv->base;
    *size = (size_t)priv->size;
    return 0;
}

uint8_t
nic_os_mmio_read8(nic_osal_mmio_t *mmio, uint32_t offset)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return 0;

    return bus_space_read_1(priv->bst, priv->bsh, offset);
}

uint16_t
nic_os_mmio_read16(nic_osal_mmio_t *mmio, uint32_t offset)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return 0;

    return bus_space_read_2(priv->bst, priv->bsh, offset);
}

uint32_t
nic_os_mmio_read32(nic_osal_mmio_t *mmio, uint32_t offset)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return 0;

    return bus_space_read_4(priv->bst, priv->bsh, offset);
}

uint64_t
nic_os_mmio_read64(nic_osal_mmio_t *mmio, uint32_t offset)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return 0;

#ifdef bus_space_read_8
    return bus_space_read_8(priv->bst, priv->bsh, offset);
#else
    return ((uint64_t)bus_space_read_4(priv->bst, priv->bsh, offset + 4) << 32) |
            (uint64_t)bus_space_read_4(priv->bst, priv->bsh, offset);
#endif
}

void
nic_os_mmio_write8(nic_osal_mmio_t *mmio, uint32_t offset, uint8_t value)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return;

    bus_space_write_1(priv->bst, priv->bsh, offset, value);
}

void
nic_os_mmio_write16(nic_osal_mmio_t *mmio, uint32_t offset, uint16_t value)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return;

    bus_space_write_2(priv->bst, priv->bsh, offset, value);
}

void
nic_os_mmio_write32(nic_osal_mmio_t *mmio, uint32_t offset, uint32_t value)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return;

    bus_space_write_4(priv->bst, priv->bsh, offset, value);
}

void
nic_os_mmio_write64(nic_osal_mmio_t *mmio, uint32_t offset, uint64_t value)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return;

#ifdef bus_space_write_8
    bus_space_write_8(priv->bst, priv->bsh, offset, value);
#else
    bus_space_write_4(priv->bst, priv->bsh, offset, (uint32_t)(value & 0xffffffffU));
    bus_space_write_4(priv->bst, priv->bsh, offset + 4, (uint32_t)(value >> 32));
#endif
}

void
nic_os_mmio_read_region_1(nic_osal_mmio_t *mmio,
                          uint32_t offset,
                          void *dst,
                          size_t count)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL || dst == NULL || count == 0)
        return;

    bus_space_read_region_1(priv->bst, priv->bsh, offset, dst, count);
}

void
nic_os_mmio_write_region_1(nic_osal_mmio_t *mmio,
                           uint32_t offset,
                           const void *src,
                           size_t count)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL || src == NULL || count == 0)
        return;

    bus_space_write_region_1(priv->bst, priv->bsh, offset, src, count);
}

void
nic_os_mmio_barrier_read(nic_osal_mmio_t *mmio)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return;

    bus_barrier(priv->res, 0, priv->size, BUS_SPACE_BARRIER_READ);
}

void
nic_os_mmio_barrier_write(nic_osal_mmio_t *mmio)
{
    struct nic_os_mmio_priv *priv;

    priv = nic_os_mmio_get_priv(mmio);
    if (priv == NULL)
        return;

    bus_barrier(priv->res, 0, priv->size, BUS_SPACE_BARRIER_WRITE);
}

uint32_t
nic_os_mmio_flush(nic_osal_mmio_t *mmio, uint32_t offset)
{
    /*
     * Posting-read helper. A read after writes is the common pattern for
     * flushing posted MMIO writes on PCI-like buses.
     */
    return nic_os_mmio_read32(mmio, offset);
}
