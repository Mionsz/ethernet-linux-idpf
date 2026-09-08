#include "osal_dma.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <machine/bus.h>

#include <sys/malloc.h>
#include <sys/libkern.h>
#include <sys/errno.h>

struct nic_os_dma_mem_priv {
    bus_dma_tag_t  tag;
    bus_dmamap_t   map;
    bus_addr_t     dma_addr;
    int            map_loaded;
};

struct nic_os_dma_map_priv {
    bus_dma_tag_t      tag;
    bus_dmamap_t       map;
    bus_addr_t         dma_addr;
    bus_size_t         size;
    nic_osal_dma_dir_t dir;
    int                map_loaded;
};

static device_t
nic_osal_get_device(nic_osal_device_t *dev)
{
    if (dev == NULL || dev->os_private == NULL)
        return (device_t)NULL;

    return (device_t)dev->os_private;
}

static void
nic_os_dma_mem_load_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
    struct nic_os_dma_mem_priv *priv;

    priv = (struct nic_os_dma_mem_priv *)arg;
    if (priv == NULL || error != 0 || nseg < 1)
        return;

    priv->dma_addr = segs[0].ds_addr;
    priv->map_loaded = 1;
}

static void
nic_os_dma_map_load_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
    struct nic_os_dma_map_priv *priv;

    priv = (struct nic_os_dma_map_priv *)arg;
    if (priv == NULL || error != 0 || nseg < 1)
        return;

    /*
     * Simple OSAL implementation: one DMA segment only.
     * Redesign if scatter/gather is required.
     */
    priv->dma_addr = segs[0].ds_addr;
    priv->map_loaded = 1;
}

static int
nic_os_dma_dir_to_pre_flags(nic_osal_dma_dir_t dir)
{
    switch (dir) {
    case NIC_OSAL_DMA_TO_DEVICE:
        return BUS_DMASYNC_PREWRITE;
    case NIC_OSAL_DMA_FROM_DEVICE:
        return BUS_DMASYNC_PREREAD;
    case NIC_OSAL_DMA_BIDIRECTIONAL:
        return BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE;
    default:
        return 0;
    }
}

static int
nic_os_dma_dir_to_post_flags(nic_osal_dma_dir_t dir)
{
    switch (dir) {
    case NIC_OSAL_DMA_TO_DEVICE:
        return BUS_DMASYNC_POSTWRITE;
    case NIC_OSAL_DMA_FROM_DEVICE:
        return BUS_DMASYNC_POSTREAD;
    case NIC_OSAL_DMA_BIDIRECTIONAL:
        return BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE;
    default:
        return 0;
    }
}

int
nic_os_dma_alloc(nic_osal_device_t *dev,
                 size_t size,
                 nic_osal_dma_buffer_t *buf)
{
    device_t bsd_dev;
    struct nic_os_dma_mem_priv *priv;
    int error;

    if (dev == NULL || buf == NULL || size == 0)
        return EINVAL;

    memset(buf, 0, sizeof(*buf));

    bsd_dev = nic_osal_get_device(dev);
    if (bsd_dev == (device_t)NULL)
        return ENODEV;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    error = bus_dma_tag_create(
        bus_get_dma_tag(bsd_dev),
        1,
        0,
        BUS_SPACE_MAXADDR,
        BUS_SPACE_MAXADDR,
        NULL,
        NULL,
        size,
        1,
        size,
        0,
        NULL,
        NULL,
        &priv->tag);
    if (error != 0)
        goto fail_priv;

    error = bus_dmamem_alloc(priv->tag, &buf->vaddr,
        BUS_DMA_WAITOK | BUS_DMA_COHERENT | BUS_DMA_ZERO,
        &priv->map);
    if (error != 0)
        goto fail_tag;

    error = bus_dmamap_load(priv->tag, priv->map, buf->vaddr, size,
        nic_os_dma_mem_load_cb, priv, BUS_DMA_WAITOK);
    if (error != 0)
        goto fail_mem;

    if (!priv->map_loaded) {
        error = EFAULT;
        goto fail_unload;
    }

    buf->dma_addr = (nic_osal_dma_addr_t)priv->dma_addr;
    buf->size = size;
    buf->os_priv = priv;
    return 0;

fail_unload:
    if (priv->map_loaded)
        bus_dmamap_unload(priv->tag, priv->map);
fail_mem:
    bus_dmamem_free(priv->tag, buf->vaddr, priv->map);
fail_tag:
    bus_dma_tag_destroy(priv->tag);
fail_priv:
    free(priv, M_DEVBUF);
    memset(buf, 0, sizeof(*buf));
    return error;
}

void
nic_os_dma_free(nic_osal_device_t *dev,
                nic_osal_dma_buffer_t *buf)
{
    struct nic_os_dma_mem_priv *priv;

    (void)dev;

    if (buf == NULL || buf->os_priv == NULL)
        return;

    priv = (struct nic_os_dma_mem_priv *)buf->os_priv;

    if (priv->map_loaded)
        bus_dmamap_unload(priv->tag, priv->map);

    if (buf->vaddr != NULL)
        bus_dmamem_free(priv->tag, buf->vaddr, priv->map);

    if (priv->tag != NULL)
        bus_dma_tag_destroy(priv->tag);

    free(priv, M_DEVBUF);
    memset(buf, 0, sizeof(*buf));
}

int
nic_os_dma_map(nic_osal_device_t *dev,
               void *ptr,
               size_t size,
               nic_osal_dma_dir_t dir,
               nic_osal_dma_map_t *map)
{
    device_t bsd_dev;
    struct nic_os_dma_map_priv *priv;
    int error;
    int sync_flags;

    if (dev == NULL || ptr == NULL || map == NULL || size == 0)
        return EINVAL;

    memset(map, 0, sizeof(*map));

    bsd_dev = nic_osal_get_device(dev);
    if (bsd_dev == (device_t)NULL)
        return ENODEV;

    priv = malloc(sizeof(*priv), M_DEVBUF, M_WAITOK | M_ZERO);
    if (priv == NULL)
        return ENOMEM;

    priv->size = size;
    priv->dir = dir;

    error = bus_dma_tag_create(
        bus_get_dma_tag(bsd_dev),
        1,
        0,
        BUS_SPACE_MAXADDR,
        BUS_SPACE_MAXADDR,
        NULL,
        NULL,
        size,
        1,
        size,
        0,
        NULL,
        NULL,
        &priv->tag);
    if (error != 0)
        goto fail_priv;

    error = bus_dmamap_create(priv->tag, BUS_DMA_WAITOK, &priv->map);
    if (error != 0)
        goto fail_tag;

    error = bus_dmamap_load(priv->tag, priv->map, ptr, size,
        nic_os_dma_map_load_cb, priv, BUS_DMA_WAITOK);
    if (error != 0)
        goto fail_map;

    if (!priv->map_loaded) {
        error = EFAULT;
        goto fail_unload;
    }

    sync_flags = nic_os_dma_dir_to_pre_flags(dir);
    if (sync_flags != 0)
        bus_dmamap_sync(priv->tag, priv->map, sync_flags);

    map->dma_addr = (nic_osal_dma_addr_t)priv->dma_addr;
    map->size = size;
    map->os_priv = priv;
    return 0;

fail_unload:
    if (priv->map_loaded)
        bus_dmamap_unload(priv->tag, priv->map);
fail_map:
    bus_dmamap_destroy(priv->tag, priv->map);
fail_tag:
    bus_dma_tag_destroy(priv->tag);
fail_priv:
    free(priv, M_DEVBUF);
    memset(map, 0, sizeof(*map));
    return error;
}

void
nic_os_dma_unmap(nic_osal_device_t *dev,
                 nic_osal_dma_map_t *map,
                 nic_osal_dma_dir_t dir)
{
    struct nic_os_dma_map_priv *priv;
    int sync_flags;

    (void)dev;

    if (map == NULL || map->os_priv == NULL)
        return;

    priv = (struct nic_os_dma_map_priv *)map->os_priv;

    sync_flags = nic_os_dma_dir_to_post_flags(dir);
    if (sync_flags != 0 && priv->map_loaded)
        bus_dmamap_sync(priv->tag, priv->map, sync_flags);

    if (priv->map_loaded)
        bus_dmamap_unload(priv->tag, priv->map);

    if (priv->map != NULL)
        bus_dmamap_destroy(priv->tag, priv->map);

    if (priv->tag != NULL)
        bus_dma_tag_destroy(priv->tag);

    free(priv, M_DEVBUF);
    memset(map, 0, sizeof(*map));
}

void
nic_os_dma_sync_for_cpu(nic_osal_device_t *dev,
                        nic_osal_dma_map_t *map,
                        nic_osal_dma_dir_t dir)
{
    struct nic_os_dma_map_priv *priv;
    int sync_flags;

    (void)dev;

    if (map == NULL || map->os_priv == NULL)
        return;

    priv = (struct nic_os_dma_map_priv *)map->os_priv;
    if (!priv->map_loaded)
        return;

    sync_flags = nic_os_dma_dir_to_post_flags(dir);
    if (sync_flags != 0)
        bus_dmamap_sync(priv->tag, priv->map, sync_flags);
}

void
nic_os_dma_sync_for_device(nic_osal_device_t *dev,
                           nic_osal_dma_map_t *map,
                           nic_osal_dma_dir_t dir)
{
    struct nic_os_dma_map_priv *priv;
    int sync_flags;

    (void)dev;

    if (map == NULL || map->os_priv == NULL)
        return;

    priv = (struct nic_os_dma_map_priv *)map->os_priv;
    if (!priv->map_loaded)
        return;

    sync_flags = nic_os_dma_dir_to_pre_flags(dir);
    if (sync_flags != 0)
        bus_dmamap_sync(priv->tag, priv->map, sync_flags);
}
