# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2019-2026 Intel Corporation
#
# In-kernel test module for the idpf FreeBSD port.
#
# Builds the driver objects a second time with -DIDPF_UNIT_TEST.  idpf_main.c
# is NOT listed here: idpf_test_attach.c includes it so that its file-static
# helpers are reachable, and listing it as well would define everything twice.

KMOD	= if_idpf_test

SRCDIR		= ${.CURDIR}/../src
SHAREDDIR	= ${.CURDIR}/../shared
.PATH:		${SRCDIR} ${SHAREDDIR}

SRCS	= device_if.h bus_if.h pci_if.h ifdi_if.h
SRCS	+= opt_inet.h opt_inet6.h opt_rss.h

SRCS	+= idpf_test_main.c
SRCS	+= idpf_test_env.c
SRCS	+= idpf_test_attach.c
SRCS	+= idpf_test_ctlq.c

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

CFLAGS	+= -DIDPF_UNIT_TEST
CFLAGS	+= -I${SHAREDDIR}
CFLAGS	+= -I${SHAREDDIR}/virtchnl
CFLAGS	+= -I${SRCDIR}
CFLAGS	+= -I${.CURDIR}
CFLAGS	+= -I/usr/src/sys/compat/linuxkpi/common/include
CFLAGS	+= -DFREEBSD_SUPPORT -D__KERNEL__ -DLINUX_SUPPORT -DCONFIG_EDT
CFLAGS	+= -include ${SRCDIR}/idpf_type_compat.h
CFLAGS	+= -include ${SRCDIR}/idpf_osdep.h
CFLAGS	+= -include linux/bits.h
CFLAGS	+= -include idpf_type.h
CFLAGS	+= -include idpf_lan_pf_regs.h
CFLAGS	+= -include idpf_lan_txrx.h
CFLAGS	+= -include idpf_lan_vf_regs.h
CFLAGS	+= -include virtchnl/virtchnl2.h

CWARNFLAGS += -Wno-unused-parameter

.include <bsd.kmod.mk>
