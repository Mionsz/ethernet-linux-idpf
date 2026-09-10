# idpf TDD kobject Makefile
#
# Generates device_if.h/bus_if.h/pci_if.h (standard) and ifdi_if.h
# (iflib) from their .m interface definitions in the reused
# common/mock_inc tree, via common/kobj.mk's boilerplate. Modeled on
# the proven iavf/iavf/src/TDD/kobjs/Makefile.

MAKEOBJOPS := ../../../common/mock_inc/tools/makeobjops.awk

interface_categories := standard iflib

standard_files := \
	../../../common/mock_inc/dev/pci/pci_if.m \
	../../../common/mock_inc/kern/bus_if.m \
	../../../common/mock_inc/kern/device_if.m

iflib_files := \
	../../../common/mock_inc/net/ifdi_if.m

.include "../../../common/kobj.mk"
