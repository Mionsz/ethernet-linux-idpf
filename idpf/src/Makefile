# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2019-2026 Intel Corporation
#
# FreeBSD kernel module build for the Intel(R) IDPF driver.
#
# Replaces the Linux kbuild Makefile: build with plain make(1) from this
# directory on a host that has /usr/src/sys and bsd.kmod.mk.  The ifdi_if
# methods resolve at load time out of the iflib module, so only the generated
# ifdi_if.h header is needed here.
#
# SIOV support is off by default because it drags in the ADI registry, which
# has no consumer until a device model is wired up:
#
#	make ENABLE_SIOV_SUPPORT=1

KMOD	= if_idpf

SRCS	= device_if.h bus_if.h pci_if.h ifdi_if.h
SRCS	+= opt_inet.h opt_inet6.h opt_rss.h

SRCS	+= idpf_main.c
SRCS	+= idpf_lib.c
SRCS	+= idpf_txrx.c
SRCS	+= idpf_singleq_txrx.c
SRCS	+= idpf_virtchnl.c
SRCS	+= idpf_controlq.c
SRCS	+= idpf_controlq_setup.c
SRCS	+= idpf_dev.c
SRCS	+= idpf_vf_dev.c

.if defined(ENABLE_SIOV_SUPPORT)
SRCS	+= idpf_adi.c
CFLAGS	+= -DENABLE_SIOV_SUPPORT
.endif

#
# Sources deliberately not built.  Each depends on a Linux subsystem with no
# FreeBSD counterpart, and every call site in the ported driver is gone:
#
#	idpf_ethtool.c		ethtool; iflib exposes its knobs via sysctl
#	idpf_idc.c		auxiliary bus / IIDC (RDMA)
#	idpf_xsk.c		AF_XDP
#	idpf_devlink.c		devlink
#	idpf_vdcm_main.c	VFIO/mdev
#	idpf_vdcm_dev.c		VFIO/mdev
#	idpf_ptp.c		PTP; idpf_ptp.h supplies no-op stubs instead
#	idpf_virtchnl_ptp.c	PTP
#	kcompat*.c		Linux kernel version compatibility
#

CWARNFLAGS	+= -Wno-unused-parameter

.include <bsd.kmod.mk>
