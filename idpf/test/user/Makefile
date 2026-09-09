# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2019-2026 Intel Corporation
#
# Userspace tests for the control queue layer.
#
# No kernel involvement: the driver sources are compiled unmodified against a
# small mock of the kernel-only headers, so a failure is a failed process, not
# a dead machine. Needs no /usr/src.
#
#   make && make run

PROG	= test_ctlq

SRCDIR		= ../../src
SHAREDDIR	= ../../shared
MOCKDIR		= mock

CC	?= cc

# The mock directory must precede the system headers so the kernel-only ones
# resolve to the stand-ins. sys/param.h, sys/queue.h, sys/endian.h and
# machine/atomic.h are deliberately NOT mocked and come from the base system.
CFLAGS	= -I. -I$(MOCKDIR) -I$(SHAREDDIR) -I$(SRCDIR)
CFLAGS	+= -g -O1 -std=gnu99
CFLAGS	+= -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
CFLAGS	+= -fno-strict-aliasing
CFLAGS	+= -D__KERNEL__ -DLINUX_SUPPORT -DCOMBINED_BUILD

OBJS	= test_ctlq.o idpf_osdep_user.o idpf_controlq.o idpf_controlq_setup.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

idpf_controlq.o: $(SHAREDDIR)/idpf_controlq.c
	$(CC) $(CFLAGS) -c -o $@ $<

idpf_controlq_setup.o: $(SHAREDDIR)/idpf_controlq_setup.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(PROG)
	./$(PROG)

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: all run clean
