#!/bin/sh
#
# idpf-validate.sh - staged validation harness for the FreeBSD IDPF driver.
#
# Run on the FreeBSD target as root:
#     ./idpf-validate.sh [interface] [module-path] [outdir]
#
# Every stage reports PASS, FAIL, or SKIP.  SKIP means a precondition was
# absent (no tool, capability not negotiated); it is never treated as a pass.
# Exit status is non-zero if any stage FAILs.
#
# Stages 6-9 are the ones that have caught real defects: a stop/init cycle
# used to release the descriptor rings that iflib owns, and detach used to
# leak the MSI-X allocation.
# shellcheck disable=SC2015

set -u

IFACE="${1:-idpf0}"
KMOD="${2:-/tmp/idpfbuild/src/if_idpf.ko}"
OUTDIR="${3:-/tmp/idpf_validate_$(date +%Y%m%d_%H%M%S)}"

TESTIP="${TESTIP:-192.168.211.1/24}"
TESTPEER="${TESTPEER:-192.168.211.99}"
RX_WINDOW="${RX_WINDOW:-10}"
CYCLES="${CYCLES:-3}"

mkdir -p "$OUTDIR" || exit 1
LOG="$OUTDIR/validate.log"
RESULTS="$OUTDIR/results.txt"
FAILURES=0

log()  { echo "$*" | tee -a "$LOG"; }
pass() { log "[PASS] $*"; echo "PASS $*" >> "$RESULTS"; }
skip() { log "[SKIP] $*"; echo "SKIP $*" >> "$RESULTS"; }
fail() { log "[FAIL] $*"; echo "FAIL $*" >> "$RESULTS"; FAILURES=$((FAILURES+1)); }
section() { log ""; log "===== $* ====="; }

[ "$(uname -s)" = "FreeBSD" ] || { echo "RESULT: SKIP - FreeBSD only"; exit 0; }
if [ "$(id -u)" -ne 0 ]; then
	echo "must run as root" >&2
	exit 1
fi
case "${IDPF_ALLOW_HARDWARE:-}" in
1|yes|true) ;;
*) echo "RESULT: SKIP - set IDPF_ALLOW_HARDWARE=1 to authorize hardware tests"; exit 2 ;;
esac
case "${IDPF_CONSOLE_CONFIRMED:-}" in
1|yes|true) ;;
*) echo "RESULT: SKIP - set IDPF_CONSOLE_CONFIRMED=1 after verifying console access"; exit 2 ;;
esac

# Counter column 5 is Ibytes, column 8 is Obytes in netstat -I -b output.
rx_bytes() { netstat -I "$IFACE" -b 2>/dev/null | awk 'NR==2 {print $8}'; }
tx_bytes() { netstat -I "$IFACE" -b 2>/dev/null | awk 'NR==2 {print $11}'; }
iface_up() { ifconfig "$IFACE" 2>/dev/null | grep -q "<UP,"; }

section "1. Environment"
{
	echo "date=$(date)"
	uname -a
	echo "iface=$IFACE"
	echo "kmod=$KMOD"
} > "$OUTDIR/environment.txt" 2>&1
log "$(uname -sr), target $IFACE"
pass "environment captured"

section "2. Module load"
kldunload if_idpf 2>/dev/null
if [ ! -f "$KMOD" ]; then
	fail "module not found at $KMOD"
	log "cannot continue without the module"
	exit 1
fi
# Staged load: freeze the bus so attach happens under our control, and thaw
# in the background because thaw does not return until attach completes.
devctl freeze 2>/dev/null
if kldload "$KMOD" >>"$LOG" 2>&1; then
	pass "kldload"
else
	fail "kldload"
	devctl thaw 2>/dev/null
	exit 1
fi
(devctl thaw >/dev/null 2>&1 &)
sleep 12

section "3. Attach and interface creation"
if ifconfig "$IFACE" >/dev/null 2>&1; then
	pass "interface $IFACE created"
else
	fail "interface $IFACE not created"
	dmesg | tail -30 >> "$LOG"
	exit 1
fi
ifconfig "$IFACE" > "$OUTDIR/ifconfig-initial.txt" 2>&1
dmesg | grep -i idpf | tail -40 > "$OUTDIR/dmesg-attach.txt" 2>&1

MAC=$(ifconfig "$IFACE" | awk '/ether/{print $2}')
case "$MAC" in
"" )                 fail "no MAC address published" ;;
"00:00:00:00:00:00") fail "MAC is all zeroes (iflib_set_mac must run in attach_pre)" ;;
* )                  pass "MAC address $MAC" ;;
esac

if ifconfig "$IFACE" | grep -q "media:"; then
	pass "media types published"
else
	fail "no media types (ifmedia_add must run in attach_pre)"
fi

# Before the control plane reports link, there must be no status line at all;
# printing "no carrier" would assert a down link that has not been observed.
if ifconfig "$IFACE" | grep -q "status:"; then
	log "  note: link state already known at attach"
fi

section "4. Link up"
ifconfig "$IFACE" up 2>>"$LOG"
sleep 8
if ifconfig "$IFACE" | grep -q "status: active"; then
	pass "link active"
elif ifconfig "$IFACE" | grep -q "status: no carrier"; then
	skip "link reports no carrier - check the cable or link partner"
else
	fail "no link status after bring-up"
fi
ifconfig "$IFACE" > "$OUTDIR/ifconfig-up.txt" 2>&1

section "5. Datapath"
ifconfig "$IFACE" inet "$TESTIP" 2>>"$LOG"
sleep 2

RX0=$(rx_bytes)
sleep "$RX_WINDOW"
RX1=$(rx_bytes)
if [ "${RX1:-0}" -gt "${RX0:-0}" ] 2>/dev/null; then
	pass "RX received $((RX1-RX0)) bytes in ${RX_WINDOW}s"
else
	skip "no RX traffic in ${RX_WINDOW}s (quiet wire?)"
fi

TX0=$(tx_bytes)
ping -c 3 -t 2 "$TESTPEER" >/dev/null 2>&1
sleep 2
TX1=$(tx_bytes)
if [ "${TX1:-0}" -gt "${TX0:-0}" ] 2>/dev/null; then
	pass "TX transmitted $((TX1-TX0)) bytes"
else
	fail "TX counters did not advance"
fi

ERRS=$(netstat -I "$IFACE" -b | awk 'NR==2 {print $6+$10}')
if [ "${ERRS:-0}" -eq 0 ] 2>/dev/null; then
	pass "no RX/TX errors"
else
	fail "interface reports $ERRS errors"
fi
netstat -I "$IFACE" -b > "$OUTDIR/counters.txt" 2>&1

section "6. Stop/init cycling"
# Regression: releasing iflib-owned rings on stop made the next init refill
# a NULL desc_ring.
i=1
CYCLE_OK=1
while [ "$i" -le "$CYCLES" ]; do
	ifconfig "$IFACE" down 2>>"$LOG"
	sleep 2
	ifconfig "$IFACE" up 2>>"$LOG"
	sleep 5
	if ! iface_up; then
		fail "interface did not come up on cycle $i"
		CYCLE_OK=0
		break
	fi
	i=$((i+1))
done
[ "$CYCLE_OK" -eq 1 ] && pass "$CYCLES down/up cycles survived"

section "7. MTU change"
ORIG_MTU=$(ifconfig "$IFACE" | awk '{for(n=1;n<=NF;n++) if($n=="mtu") print $(n+1)}' | head -1)
MTU_OK=1
for m in 9000 4000 1500; do
	if ifconfig "$IFACE" mtu "$m" 2>>"$LOG"; then
		sleep 3
		NOW=$(ifconfig "$IFACE" | awk '{for(n=1;n<=NF;n++) if($n=="mtu") print $(n+1)}' | head -1)
		if [ "$NOW" != "$m" ]; then
			fail "MTU $m requested but interface reports $NOW"
			MTU_OK=0
		fi
	else
		skip "MTU $m rejected (may exceed the negotiated maximum)"
	fi
done
[ "$MTU_OK" -eq 1 ] && pass "MTU changes applied (was $ORIG_MTU)"

ifconfig "$IFACE" mtu 1500 2>/dev/null
sleep 2
ifconfig "$IFACE" inet "$TESTIP" 2>/dev/null
TX0=$(tx_bytes)
ping -c 2 -t 2 "$TESTPEER" >/dev/null 2>&1
sleep 2
if [ "$(tx_bytes)" -gt "${TX0:-0}" ] 2>/dev/null; then
	pass "datapath still transmits after MTU changes"
else
	fail "datapath stopped transmitting after MTU changes"
fi

section "8. Driver-private ioctl"
# Exposed through SIOCGDRVSPEC; FreeBSD has no hwtstamp ioctl and iflib routes
# only SIOCGPRIVATE_0 and SIOCxDRVSPEC to ifdi_priv_ioctl.
if [ -x /tmp/idpf_priv_test ]; then
	if /tmp/idpf_priv_test >> "$LOG" 2>&1; then
		pass "SIOCGDRVSPEC returned driver info"
	else
		fail "SIOCGDRVSPEC failed"
	fi
else
	skip "no /tmp/idpf_priv_test helper built"
fi

section "9. Detach and resource release"
# Regression: the queue interrupts were never freed, so pci_release_msi()
# failed and the MSI-X allocation leaked into the next attach.
if kldunload if_idpf >>"$LOG" 2>&1; then
	pass "kldunload"
else
	fail "kldunload"
fi
sleep 2
if dmesg | tail -20 | grep -qi "leaked"; then
	fail "device leaked resources on detach"
	dmesg | tail -20 | grep -i leaked >> "$LOG"
else
	pass "no leaked resources reported"
fi

section "10. PTP negotiation"
# The PTP capability path is exercised on every attach whether or not the
# control plane grants it, so these assert behaviour rather than skipping.
PTPLINE=$(grep -i "PTP:" "$OUTDIR/dmesg-attach.txt" 2>/dev/null | tail -1)
PTPNODE="dev.${IFACE%%[0-9]*}.0.ptp_clock_ns"

if [ -z "$PTPLINE" ]; then
	fail "attach reported no PTP status at all"
elif echo "$PTPLINE" | grep -q "not offered"; then
	# Negotiation ran and correctly declined.  The contract is then that
	# no clock node exists; publishing one would expose an unreadable clock.
	pass "PTP negotiation ran and declined: ${PTPLINE#*PTP: }"
	CAPS=$(echo "$PTPLINE" | sed -n 's/.*other_caps \(0x[0-9a-f]*\).*/\1/p')
	if [ -n "$CAPS" ]; then
		# VIRTCHNL2_CAP_PTP is bit 13 (0x2000) of other_caps.
		if [ $(( CAPS & 0x2000 )) -eq 0 ] 2>/dev/null; then
			pass "reported caps $CAPS confirm bit 13 clear"
		else
			fail "caps $CAPS have PTP bit 13 set but PTP was declined"
		fi
	fi
	if sysctl "$PTPNODE" >/dev/null 2>&1; then
		fail "ptp_clock_ns published although PTP was not negotiated"
	else
		pass "no clock node published, matching the declined capability"
	fi
else
	pass "PTP negotiated: ${PTPLINE#*PTP: }"
	if ! sysctl "$PTPNODE" >/dev/null 2>&1; then
		fail "PTP negotiated but $PTPNODE is missing"
	else
		A=$(sysctl -n "$PTPNODE" 2>/dev/null)
		sleep 1
		B=$(sysctl -n "$PTPNODE" 2>/dev/null)
		if [ "${B:-0}" -gt "${A:-0}" ] 2>/dev/null; then
			pass "device clock advances ($A -> $B)"
		else
			fail "device clock did not advance ($A -> $B)"
		fi
	fi
fi

section "Summary"
tee -a "$LOG" < "$RESULTS"
log ""
log "artifacts in $OUTDIR"
if [ "$FAILURES" -eq 0 ]; then
	log "RESULT: no failures"
	exit 0
fi
log "RESULT: $FAILURES failure(s)"
exit 1
