/*
 * osal_pci.h
 *
 * FreeBSD-oriented OS abstraction for PCI configuration-space access
 * and light PCI device control.
 *
 * Design notes:
 * - pdev->os_private must reference a FreeBSD device_t.
 * - This layer provides config-space reads/writes, bus-master control,
 *   and BAR metadata queries.
 * - BAR resource allocation/mapping is intentionally out of scope here.
 *
 * Return conventions:
 * - Functions returning int use FreeBSD-style positive errno values:
 *     0        success
 *     EINVAL   invalid argument/state
 *     ENODEV   invalid device
 *     ENXIO    requested BAR/resource not present
 *
 * Typical usage:
 *
 *     nic_osal_pci_device_t pdev = { .os_private = dev };
 *     uint16_t cmd;
 *     nic_osal_phys_addr_t base;
 *     size_t size;
 *
 *     nic_os_pci_enable(&pdev);
 *     nic_os_pci_set_master(&pdev);
 *     nic_os_pci_read16(&pdev, PCIR_COMMAND, &cmd);
 *     nic_os_pci_get_bar(&pdev, 0, &base, &size);
 */

#ifndef NIC_OSAL_PCI_H
#define NIC_OSAL_PCI_H

#include "osal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prepare a PCI device for driver use.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if pdev is invalid
 * - ENODEV if underlying device handle is invalid
 *
 * Notes:
 * - On FreeBSD this performs minimal validation and does not duplicate Linux
 *   pci_enable_device() semantics exactly.
 */
int nic_os_pci_enable(nic_osal_pci_device_t *pdev);

/*
 * Disable device access policy managed by this OSAL.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if pdev is invalid
 * - ENODEV if underlying device handle is invalid
 *
 * Notes:
 * - On FreeBSD this clears bus mastering as a conservative disable step.
 * - It is not a full inverse of Linux pci_disable_device().
 */
int nic_os_pci_disable(nic_osal_pci_device_t *pdev);

/*
 * Enable PCI bus mastering.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if pdev is invalid
 * - ENODEV if underlying device handle is invalid
 */
int nic_os_pci_set_master(nic_osal_pci_device_t *pdev);

/*
 * Read PCI config-space values.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid
 * - ENODEV if underlying device handle is invalid
 */
int nic_os_pci_read8(nic_osal_pci_device_t *pdev, uint32_t offset, uint8_t *value);
int nic_os_pci_read16(nic_osal_pci_device_t *pdev, uint32_t offset, uint16_t *value);
int nic_os_pci_read32(nic_osal_pci_device_t *pdev, uint32_t offset, uint32_t *value);

/*
 * Write PCI config-space values.
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid
 * - ENODEV if underlying device handle is invalid
 */
int nic_os_pci_write8(nic_osal_pci_device_t *pdev, uint32_t offset, uint8_t value);
int nic_os_pci_write16(nic_osal_pci_device_t *pdev, uint32_t offset, uint16_t value);
int nic_os_pci_write32(nic_osal_pci_device_t *pdev, uint32_t offset, uint32_t value);

/*
 * Query BAR base address and size metadata.
 *
 * Parameters:
 * - bar: BAR index, usually 0..5
 *
 * Returns:
 * - 0 on success
 * - EINVAL if arguments are invalid
 * - ENODEV if underlying device handle is invalid
 * - ENXIO if BAR is not present or has zero size
 *
 * Notes:
 * - This reports BAR metadata only.
 * - Mapping/allocating BAR resources remains the responsibility of higher
 *   driver layers.
 */
int nic_os_pci_get_bar(nic_osal_pci_device_t *pdev,
                       unsigned int bar,
                       nic_osal_phys_addr_t *base,
                       size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* NIC_OSAL_PCI_H */
