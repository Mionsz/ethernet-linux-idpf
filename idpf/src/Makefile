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
SRCS	+= idpf_osdep.c
SRCS	+= idpf_txrx.c
SRCS	+= idpf_singleq_txrx.c
SRCS	+= idpf_virtchnl.c
SRCS	+= idpf_controlq.c
SRCS	+= idpf_controlq_setup.c
SRCS	+= idpf_dev.c
SRCS	+= idpf_vf_dev.c
SRCS	+= idpf_ptp.c

.if defined(ENABLE_SIOV_SUPPORT)
SRCS	+= idpf_adi.c
CFLAGS	+= -DENABLE_SIOV_SUPPORT
.endif

#
# The Linux-only sources have been removed from the tree rather than left
# unbuilt: ethtool, IIDC/auxiliary bus (RDMA), AF_XDP, devlink, VFIO/mdev and
# the kcompat layer all depend on subsystems with no FreeBSD counterpart, and
# no call site in the ported driver remains.  They are recoverable from git
# history if a port is attempted later.
#
# idpf_ptp.c covers PTP capability negotiation and the device clock only; TX
# timestamp latch harvest is not ported (see idpf_ptp.h).
#
# Include paths: driver sources, shared code, and virtchnl headers
CFLAGS+=	-I${.CURDIR}/../shared
CFLAGS+=	-I${.CURDIR}/../shared/virtchnl
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-I/usr/src/sys/compat/linuxkpi/common/include

# Define FREEBSD_SUPPORT for shared code OS detection
CFLAGS+=	-DFREEBSD_SUPPORT
CFLAGS+=	-D__KERNEL__
CFLAGS+=	-DLINUX_SUPPORT
CFLAGS+=	-DCONFIG_EDT
CFLAGS+=	-include ${.CURDIR}/idpf_type_compat.h
CFLAGS+=	-include ${.CURDIR}/idpf_osdep.h
CFLAGS+=	-include idpf_type.h
CFLAGS+=	-include idpf_lan_pf_regs.h
CFLAGS+=	-include idpf_lan_txrx.h
CFLAGS+=	-include idpf_lan_vf_regs.h
CFLAGS+=	-include virtchnl/virtchnl2.h

# Path to shared code .c files for bsd.kmod.mk
.PATH:		${.CURDIR}/../shared

# Per-file CFLAGS: Force-include idpf_type_compat.h ONLY for shared code files
# This minimal header provides type definitions without kernel includes,
# avoiding preprocessing issues. The full idpf_osdep.h is included normally
# by shared code headers that need it.
# When adding new shared code files, add a corresponding line here.
#
# Use CFLAGS.<source> (bsd.sys.mk) rather than a "target: CFLAGS +=" line: a
# target-local assignment starts from an empty value in bmake and would drop
# the -I/-D flags set above.
CWARNFLAGS	+= -Wno-unused-parameter

.include <bsd.kmod.mk>
