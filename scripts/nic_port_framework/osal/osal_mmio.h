/*
 * osal_mmio.h
 *
 * FreeBSD-oriented OS abstraction for PCI BAR MMIO mapping and register access.
 *
 * Design notes:
 * - pdev->os_private must reference a FreeBSD device_t.
 * - MMIO mappings are backed by a FreeBSD SYS_RES_MEMORY resource.
 * - BAR index is expressed in PCI BAR numbering terms, usually 0..5.
 * - Register accessors operate on byte offsets within the mapped BAR.
 *
 * Return conventions:
 * - Functions returning int use FreeBSD-style positive errno values:
 *     0        success
 *     EINVAL   invalid argument/state
 *     EBUSY    already initialized/mapped
 *     ENODEV   invalid device
 *     ENXIO    BAR/resource unavailable
 *     ENOMEM   allocation failure
 *
 * Typical lifecycle:
 *
 *     nic_osal_mmio_t mmio = {0};
 *     uint32_t val;
 *
 *     nic_os_mmio_map_bar(&pdev, 0, &mmio);
 *     val = nic_os_mmio_read32(&mmio, REG_STATUS);
 *     nic_os_mmio_write32(&mmio, REG_CTRL, val | CTRL_ENABLE);
 *     nic_os_mmio_flush(&mmio, REG_CTRL);
 *     nic_os_mmio_unmap(&mmio);
 */

#ifndef NIC_OSAL_MMIO_H
#define NIC_OSAL_MMIO_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Map a PCI BAR as MMIO.
 *
 * Parameters:
 * - pdev: PCI device object
 * - bar:  BAR index, usually 0..5
 * - mmio: caller-owned MMIO mapping object
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid
 * - EBUSY if mmio is already mapped
 * - ENODEV if device handle is invalid
 * - ENXIO if BAR/resource cannot be mapped
 * - ENOMEM on allocation failure
 */
int nic_os_mmio_map_bar(nic_osal_pci_device_t *pdev,
                        unsigned int bar,
                        nic_osal_mmio_t *mmio);

/*
 * Unmap a previously mapped MMIO region.
 *
 * Safe to call on NULL/uninitialized objects; such calls are ignored.
 */
void nic_os_mmio_unmap(nic_osal_mmio_t *mmio);

/*
 * Query mapped MMIO base physical address and size.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid/uninitialized
 */
int nic_os_mmio_info(nic_osal_mmio_t *mmio,
                     nic_osal_phys_addr_t *base,
                     size_t *size);

/*
 * MMIO register reads.
 *
 * Notes:
 * - offset is a byte offset within the mapped BAR.
 * - caller is responsible for using valid/aligned offsets as appropriate.
 */
uint8_t  nic_os_mmio_read8(nic_osal_mmio_t *mmio, uint32_t offset);
uint16_t nic_os_mmio_read16(nic_osal_mmio_t *mmio, uint32_t offset);
uint32_t nic_os_mmio_read32(nic_osal_mmio_t *mmio, uint32_t offset);
uint64_t nic_os_mmio_read64(nic_osal_mmio_t *mmio, uint32_t offset);

/*
 * MMIO register writes.
 *
 * Notes:
 * - offset is a byte offset within the mapped BAR.
 * - caller is responsible for using valid/aligned offsets as appropriate.
 */
void nic_os_mmio_write8(nic_osal_mmio_t *mmio, uint32_t offset, uint8_t value);
void nic_os_mmio_write16(nic_osal_mmio_t *mmio, uint32_t offset, uint16_t value);
void nic_os_mmio_write32(nic_osal_mmio_t *mmio, uint32_t offset, uint32_t value);
void nic_os_mmio_write64(nic_osal_mmio_t *mmio, uint32_t offset, uint64_t value);

/*
 * Bulk MMIO copy helpers.
 *
 * Notes:
 * - These are intended for device memory windows/FIFOs where repeated
 *   bus-space accesses are appropriate.
 */
void nic_os_mmio_read_region_1(nic_osal_mmio_t *mmio,
                               uint32_t offset,
                               void *dst,
                               size_t count);
void nic_os_mmio_write_region_1(nic_osal_mmio_t *mmio,
                                uint32_t offset,
                                const void *src,
                                size_t count);

/*
 * Enforce MMIO access ordering for the mapped region.
 *
 * Notes:
 * - nic_os_mmio_barrier_read() ensures prior device reads complete.
 * - nic_os_mmio_barrier_write() ensures prior device writes are ordered.
 */
void nic_os_mmio_barrier_read(nic_osal_mmio_t *mmio);
void nic_os_mmio_barrier_write(nic_osal_mmio_t *mmio);

/*
 * Perform a posting read from the supplied register offset.
 *
 * Notes:
 * - Useful after writel-style register programming where a posted write flush
 *   is desired.
 * - Return value is ignored by many call sites; it is returned here for
 *   convenience.
 */
uint32_t nic_os_mmio_flush(nic_osal_mmio_t *mmio, uint32_t offset);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_MMIO_H */
