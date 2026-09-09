#!/bin/sh
# Staged attach test for the idpf driver on a host with real hardware.
#
# Every milestone goes to syslog as well as stdout: if attach wedges, stdout is
# lost but /var/log/messages keeps whatever syslogd managed to flush.
#
# No dead-man reboot is armed: debug.debugger_on_panic=0 in /etc/sysctl.conf
# already makes a panic self-reboot, and an extra timer only causes reboots the
# operator did not ask for. Recovery from a FAILED (not wedged) attach is
# hw-cleanup.sh, which needs no reboot.
#
# Usage: hw-attach-test.sh [seconds-to-observe]
OBSERVE=${1:-30}
KO=${KMOD:-/tmp/idpfbuild/idpf/src/if_idpf.ko}
DEV=${IDPF_IFACE:-idpf0}
PCI_MATCH=${PCI_MATCH:-idpf}

log() { echo ">>> $*"; logger -t idpftest "$*" 2>/dev/null; }

[ "$(uname -s)" = "FreeBSD" ] || {
	echo "RESULT: SKIP - FreeBSD only"
	exit 0
}
[ "$(id -u)" -eq 0 ] || { log "must run as root"; exit 1; }
case "${IDPF_ALLOW_HARDWARE:-}" in
1|yes|true) ;;
*) log "set IDPF_ALLOW_HARDWARE=1 to authorize attach"; exit 2 ;;
esac
case "${IDPF_CONSOLE_CONFIRMED:-}" in
1|yes|true) ;;
*) log "set IDPF_CONSOLE_CONFIRMED=1 after verifying console access"; exit 2 ;;
esac
[ -f "$KO" ] || { log "module not found: $KO"; exit 1; }
case "$OBSERVE" in
*[!0-9]*|'') log "observation interval must be a non-negative integer"; exit 1 ;;
esac

# Panic must reboot rather than park at db>; sysctls do not survive a reboot.
sysctl debug.debugger_on_panic=0 >/dev/null 2>&1
sysctl kern.panic_reboot_wait_time=10 >/dev/null 2>&1

if command -v hw-cleanup.sh >/dev/null 2>&1; then
	hw-cleanup.sh "$DEV" >/dev/null 2>&1
else
	devctl detach -f "$DEV" >/dev/null 2>&1
	devctl clear driver -f "$DEV" >/dev/null 2>&1
	kldunload -f if_idpf >/dev/null 2>&1
fi

devctl freeze || { log "freeze failed"; exit 1; }
if ! kldload "$KO"; then
	log "load failed"
	devctl thaw
	exit 1
fi
log "module loaded, device still unprobed"

dmesg -c >/dev/null 2>&1
log "thawing - attach starts now"
devctl thaw >/tmp/thaw.out 2>&1 &
THAW_PID=$!

i=0
while [ $i -lt "$OBSERVE" ]; do
	sleep 5
	i=$((i + 5))
	logger -t idpftest "t+${i}s last=[$(dmesg | tail -1)]" 2>/dev/null
done

log "=== dmesg ==="
dmesg | tail -25
log "=== device ==="
pciconf -l | grep "$PCI_MATCH" || true
log "=== interfaces ==="
ifconfig -l
if ! wait "$THAW_PID" 2>/dev/null; then
	log "thaw command returned an error"
	exit 1
fi
log "RESULT: attach observation complete"

