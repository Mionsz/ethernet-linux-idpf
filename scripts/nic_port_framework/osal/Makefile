# FreeBSD KLD build for the IDPF OS abstraction layer.
#
# Builds only the OSAL and the idpf_osdep bridge, so the abstraction can be
# compiled and verified independently of the ported driver sources.

KMOD=	idpf_osal

SRCS=	osal_atomic.c \
	osal_dma.c \
	osal_irq.c \
	osal_lock.c \
	osal_log.c \
	osal_mem.c \
	osal_mmio.c \
	osal_netdev.c \
	osal_pci.c \
	osal_time.c \
	osal_work.c \
	idpf_osdep.c

# Generated interface headers pulled in by sys/bus.h and the PCI layer.
SRCS+=	device_if.h bus_if.h pci_if.h

CFLAGS+=	-I${.CURDIR}

.include <bsd.kmod.mk>
