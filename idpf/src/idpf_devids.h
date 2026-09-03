/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2019-2026 Intel Corporation */

#ifndef _IDPF_DEVIDS_H_
#define _IDPF_DEVIDS_H_

/*
 * PCI device and subsystem identifiers.  Nothing here is OS specific, so the
 * FreeBSD port carries this header unchanged apart from parenthesising the
 * macro arguments below.
 */

/* Device IDs */
#define IDPF_DEV_ID_PF			0x1452
#define IDPF_DEV_ID_VF			0x145C
#define IDPF_DEV_ID_VF_SIOV		0x0DD5

#define IDPF_DEV_ID_PF_SIMICS		0xF002
#define IDPF_DEV_ID_VF_SIMICS		0xF00C
#define IDPF_SUBDEV_ID_SIMICS		0x12D1
#define IS_SIMICS_DEVICE(subdev)	((subdev) == IDPF_SUBDEV_ID_SIMICS)
#define IDPF_SUBDEV_ID_EMR		0xF0D1
#define IS_EMR_DEVICE(subdev)		((subdev) == IDPF_SUBDEV_ID_EMR)

/* Real silicon: neither the simulator nor the emulation platform. */
#define IS_SILICON_DEVICE(subdev) \
	(!IS_SIMICS_DEVICE(subdev) && !IS_EMR_DEVICE(subdev))

#endif /* _IDPF_DEVIDS_H_ */
