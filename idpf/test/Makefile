# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2019-2026 Intel Corporation
#
# In-kernel test module for the idpf FreeBSD port.
#
# Builds the driver objects a second time with -DIDPF_UNIT_TEST.  idpf_main.c
# is NOT listed here: idpf_test_attach.c includes it so that its file-static
# helpers are reachable, and listing it as well would define everything twice.

KMOD	= if_idpf_test

SRCDIR	= ${.CURDIR}/../src
.PATH:	${SRCDIR}

SRCS	= device_if.h bus_if.h pci_if.h ifdi_if.h
SRCS	+= opt_inet.h opt_inet6.h opt_rss.h

SRCS	+= idpf_test_main.c
SRCS	+= idpf_test_env.c
SRCS	+= idpf_test_attach.c
SRCS	+= idpf_test_ctlq.c

SRCS	+= idpf_lib.c
SRCS	+= idpf_txrx.c
SRCS	+= idpf_singleq_txrx.c
SRCS	+= idpf_virtchnl.c
SRCS	+= idpf_controlq.c
SRCS	+= idpf_controlq_setup.c
SRCS	+= idpf_dev.c
SRCS	+= idpf_vf_dev.c

CFLAGS	+= -DIDPF_UNIT_TEST
CFLAGS	+= -I${SRCDIR} -I${.CURDIR}

CWARNFLAGS += -Wno-unused-parameter

.include <bsd.kmod.mk>
