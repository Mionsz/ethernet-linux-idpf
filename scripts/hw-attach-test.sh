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
KO=/tmp/idpfbuild/src/if_idpf.ko

log() { echo ">>> $*"; logger -t idpftest "$*" 2>/dev/null; }

# Panic must reboot rather than park at db>; sysctls do not survive a reboot.
sysctl debug.debugger_on_panic=0 >/dev/null 2>&1
sysctl kern.panic_reboot_wait_time=10 >/dev/null 2>&1

sh /tmp/hw-cleanup.sh >/dev/null 2>&1

devctl freeze || { log "freeze failed"; exit 1; }
if ! kldload "$KO"; then
	log "load failed"
	devctl thaw
	exit 1
fi
log "module loaded, device still unprobed"

dmesg -c >/dev/null 2>&1
log "thawing - attach starts now"
nohup devctl thaw >/tmp/thaw.out 2>&1 &

i=0
while [ $i -lt "$OBSERVE" ]; do
	sleep 5
	i=$((i + 5))
	logger -t idpftest "t+${i}s last=[$(dmesg | tail -1)]" 2>/dev/null
done

log "=== dmesg ==="
dmesg | tail -25
log "=== device ==="
pciconf -l | grep "pci0:131:0:0"
log "=== interfaces ==="
ifconfig -l

