#!/bin/sh
# Cleanup for the idpf driver on a host with real hardware.
#
# A failed ifdi_attach_pre() leaves the device claimed and the module pinned:
# devctl detach reports "Device not configured" (it never reached DS_ATTACHED)
# while kldunload reports "Device busy" (the driver is still associated).
# Breaking the association with "devctl clear driver" releases the reference
# without a reboot.
#
# Usage: hw-cleanup.sh [device]
DEV=${1:-idpf0}
MOD=if_idpf

log() { echo ">>> $*"; logger -t idpfcleanup "$*" 2>/dev/null; }

if ! kldstat -q -n "$MOD"; then
	log "module not loaded, nothing to do"
	exit 0
fi

# Best effort in order of increasing force; each may legitimately fail.
devctl detach -f "$DEV" >/dev/null 2>&1 && log "detached $DEV"
devctl clear driver -f "$DEV" >/dev/null 2>&1 && log "cleared driver on $DEV"

if kldunload "$MOD" >/dev/null 2>&1; then
	log "unloaded $MOD"
elif kldunload -f "$MOD" >/dev/null 2>&1; then
	log "force-unloaded $MOD"
else
	log "FAILED to unload $MOD; a thread is probably still stuck in attach"
	kldstat | grep "$MOD"
	exit 1
fi

# Leave the device probeable again for the next run.
devctl rescan pci16 >/dev/null 2>&1
exit 0
