/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDC_GENERIC_H_
#define _IDC_GENERIC_H_

/*
 * Inter-Driver Communication (IDC) contract between the LAN driver and a
 * dependent driver such as RDMA.
 *
 * Terminology
 * -----------
 * mfd       the multi-function driver that owns the hardware and shares it
 * mfd cell  the dependent driver that consumes the shared hardware data
 *
 * FreeBSD port notes
 * ------------------
 * Linux implements the split with the MFD subsystem: the LAN driver calls
 * mfd_add_devices() and the dependent driver registers a platform_driver
 * whose id_table selects it.  FreeBSD has no MFD subsystem; the equivalent is
 * a newbus child device created with device_add_child() and probed by its own
 * driver.  The mechanism therefore belongs to the attach path, not to this
 * header, and only the data contract survives the port:
 *
 *   struct pci_dev *pdev      -> device_t dev
 *   u8 __iomem *hw_addr       -> void *hw_addr (offset into the BAR0 mapping)
 *   struct msix_entry *        -> vector index base plus count
 *   struct net_device *netdev  -> if_t ifp
 *   struct __idc_mfd_data      -> removed (Linux platform-data wrapper)
 *
 * The IDC path is not wired up by this port: idpf_dev_ops_init() does not
 * install an idc_init hook and nothing includes this header yet.  It is kept
 * so the contract stays reviewable alongside the rest of the driver.
 * [FBSD15:A30-A31] [LOCAL:A22]
 */

#include <sys/param.h>
#include <sys/bus.h>

#include <net/if.h>
#include <net/if_var.h>

/* Unique names used to match and load mfd cells */
#define IDC_MFD_CELL_NAME_RDMA		"rdma"

/* Unique ids used to match and load mfd cells */
#define IDC_MFD_CELL_ID_RDMA_PF	0x1
#define IDC_MFD_CELL_ID_RDMA_VF	0x2
#define IDC_MFD_CELL_ID_MAX	0x3

/* Version info used to check for compatibility between mfd and mfd cell */
#define IDC_MAJOR_VER		1
#define IDC_MINOR_VER		1

#define IDC_QOS_MAX_USER_PRIORITY	8
#define IDC_QOS_MAX_TC	8

/* Forward declarations */
struct idc_mfd_data;

/* Reset types */
enum idc_reset_type {
	IDC_FUN_RESET = 0,
};

enum idc_close_reason {
	IDC_INTERFACE_DOWN,
	IDC_HW_RESET_PENDING,
};

enum idc_event {
	IDC_BEFORE_MTU_CHANGE,
	IDC_AFTER_MTU_CHANGE,
	IDC_BEFORE_TC_CHANGE,
	IDC_AFTER_TC_CHANGE,
	IDC_BEFORE_INTR_CHANGE,
	IDC_AFTER_INTR_CHANGE,
};

/* Version info used to check for compatibility between mfd and mfd cells */
struct idc_ver_info {
	uint16_t major;
	uint16_t minor;
};

/* QoS info */
struct idc_qos_params {
	uint8_t  rel_bw[IDC_QOS_MAX_TC];
	uint8_t  up2tc[IDC_QOS_MAX_USER_PRIORITY];
	uint32_t num_apps;
	uint8_t  num_tc;
	uint8_t  prio_type[IDC_QOS_MAX_TC];
	uint64_t tc_ctx[IDC_QOS_MAX_TC];
	uint8_t  vport_relative_bw;
	uint8_t  vport_priority_type;
};

/* RDMA queue vector map info */
struct idc_qv_info {
	uint32_t v_idx;
	uint16_t ceq_idx;
	uint16_t aeq_idx;
	uint8_t  itr_idx;
};

struct idc_qvlist_info {
	uint32_t num_vectors;
	struct idc_qv_info qv_info[];
};

/* Implemented by the mfd, invoked by the mfd cell */
struct idc_mfd_ops {
	/* Called by the mfd cell to indicate probe finished */
	int (*probe_finished)(struct idc_mfd_data *mfd_data);
	/* Called by the mfd cell to indicate remove started */
	void (*remove_started)(struct idc_mfd_data *mfd_data);
	/* Called by the mfd cell to indicate remove finished */
	void (*remove_finished)(struct idc_mfd_data *mfd_data);
	/* Used by the mfd cell to request a reset on the mfd */
	int (*request_reset)(struct idc_mfd_data *mfd_data,
			     enum idc_reset_type reset_type);
	/* Used by the mfd cell to send mailbox messages */
	int (*vc_send)(struct idc_mfd_data *mfd_data, uint32_t f_id,
		       uint8_t *msg, uint16_t len);
	/*
	 * Map or unmap queue vectors.  This uses a different virtchnl opcode
	 * from vc_send and therefore a separate callback.
	 */
	int (*vc_queue_vec_map_unmap)(struct idc_mfd_data *mfd_data,
				      struct idc_qvlist_info *qvl_info,
				      bool map);
};

/* Implemented by the mfd cell, invoked by the mfd */
struct idc_mfd_cell_ops {
	/*
	 * open is called from the mfd cell's attach path and again once a
	 * reset completes.  It is the symmetric counterpart of close.
	 */
	int (*open)(struct idc_mfd_data *mfd_data);

	/*
	 * close quiesces the mfd cell.  It is followed by either remove or
	 * open, and no IDC call from the cell may be accepted in between.
	 * @reason lets the cell adapt its teardown to the situation.
	 */
	int (*close)(struct idc_mfd_data *mfd_data,
		     enum idc_close_reason reason);
	/* Used by the mfd to hand received mailbox messages to the cell */
	int (*vc_receive)(struct idc_mfd_data *mfd_data, uint32_t f_id,
			  uint8_t *msg, uint16_t len);
	/* Used by the mfd to report software events */
	int (*event)(struct idc_mfd_data *mfd_data, enum idc_event event);
};

/*
 * Data shared between the mfd and its cells.  Bring-up order:
 *   1. the mfd fills in the fields it owns and creates the child device
 *   2. the child's driver probes and attaches
 *   3. the cell fills in mfd_cell_ver and mfd_cell_ops
 *   4. the cell calls probe_finished()
 *   5. the mfd calls open()
 *   6. the mfd calls close() when it goes down
 */
struct idc_mfd_data {
	/* Owned by the mfd, valid before the child device is created. */

	/* PCI device of the main function; used by the cell for DMA */
	device_t dev;
	/*
	 * Host-virtual address of the shared register window, computed as an
	 * offset into the LAN driver's single BAR0 mapping.  Access it with
	 * the MMIO seam, not by dereferencing it directly.
	 */
	void *hw_addr;

	/* First MSI-X vector reserved for the cell, and how many follow */
	uint16_t msix_base;
	uint16_t msix_count;
	/* Used by the cell for version checks */
	struct idc_ver_info mfd_ver;
	/* PF or VF */
	int func_type;
	/* Network interface owned by the mfd */
	if_t ifp;
	/* Traffic class configuration */
	struct idc_qos_params qos_info;
	/* Filled in by the mfd, called by the cell */
	struct idc_mfd_ops mfd_ops;

	/* Owned by the cell, valid before it calls probe_finished(). */

	/* Used by the mfd for version checks */
	struct idc_ver_info mfd_cell_ver;
	/* Filled in by the cell, called by the mfd */
	struct idc_mfd_cell_ops mfd_cell_ops;
};

#endif /* _IDC_GENERIC_H_ */
