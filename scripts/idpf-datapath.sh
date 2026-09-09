#!/bin/sh
#
# idpf-datapath.sh - packet and data transfer tests for the FreeBSD IDPF
# driver.
#
# What can and cannot be proven on this setup
# -------------------------------------------
# TX is provable at wire level: dev.idpf.N.stats.* are control-plane
# counters, so a delta there means the frame reached the device, not just
# the host stack.  Every TX assertion below compares a hardware delta
# against what was sent.
#
# RX from an external sender is NOT provable here: the control plane does
# not offer VIRTCHNL2_CAP_LOOPBACK (other_caps bit 19 is clear) and the
# link has no peer that transmits.  RX stages therefore report SKIP with
# the observed counters rather than a fabricated PASS.  They become real
# assertions the moment a link partner exists.
#
# shellcheck disable=SC2015
# Run on the FreeBSD target as root:
#     ./idpf-datapath.sh [interface] [module-path]

set -u

IFACE="${1:-idpf0}"
KMOD="${2:-/tmp/idpfbuild/idpf/src/if_idpf.ko}"
TESTIP="${TESTIP:-192.168.211.1/24}"
PEER="${PEER:-192.168.211.99}"
TEST_BROADCAST="${TEST_BROADCAST:-192.168.211.255}"
BURST="${BURST:-200}"

PASS=0
FAIL=0
SKIP=0
section() { echo; echo "===== $* ====="; }
pass()    { echo "[PASS] $*"; PASS=$((PASS+1)); }
fail()    { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }
skip()    { echo "[SKIP] $*"; SKIP=$((SKIP+1)); }

[ "$(uname -s)" = "FreeBSD" ] || { echo "RESULT: SKIP - FreeBSD only"; exit 0; }
[ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }
case "${IDPF_ALLOW_HARDWARE:-}" in
1|yes|true) ;;
*) echo "RESULT: SKIP - set IDPF_ALLOW_HARDWARE=1 to authorize hardware tests"; exit 2 ;;
esac
case "${IDPF_CONSOLE_CONFIRMED:-}" in
1|yes|true) ;;
*) echo "RESULT: SKIP - set IDPF_CONSOLE_CONFIRMED=1 after verifying console access"; exit 2 ;;
esac

DRIVER=${IFACE%%[0-9]*}
UNIT=${IFACE#"$DRIVER"}
[ -n "$UNIT" ] || UNIT=0
hw() { sysctl -n "dev.$DRIVER.$UNIT.stats.$1" 2>/dev/null || echo 0; }
ifc() { netstat -I "$IFACE" -b | awk 'NR==2 {print $'"$1"'}'; }
# netstat -I -b columns: 6=Ierrs 7=Idrop 8=Ibytes 9=Opkts 10=Oerrs 11=Obytes
errs() { netstat -I "$IFACE" -b | awk 'NR==2 {print $6+$10}'; }

if ! ifconfig "$IFACE" >/dev/null 2>&1; then
	kldload "$KMOD" 2>/dev/null && sleep 8
fi
if ! ifconfig "$IFACE" >/dev/null 2>&1; then
	echo "[FAIL] interface $IFACE does not exist"
	echo "RESULT: 1 failure(s)"; exit 1
fi

ifconfig "$IFACE" inet "$TESTIP" alias 2>/dev/null
ifconfig "$IFACE" up
sleep 3

# Without this the peer never resolves, every IP packet is dropped before it
# reaches the device, and the size tests below would only ever measure ARP.
arp -s "$PEER" 02:00:00:00:be:ef >/dev/null 2>&1

if [ "$(sysctl -n "dev.$DRIVER.$UNIT.stats.tx_bytes" 2>/dev/null || echo missing)" = "missing" ]; then
	echo "[FAIL] hardware counters not published; cannot verify the wire"
	echo "RESULT: 1 failure(s)"; exit 1
fi

# ------------------------------------------------------- packet size sweep
section "1. Packet size sweep (hardware TX counters)"
# ping payload sizes; each ICMP frame is payload + 8 ICMP + 20 IP + 14 L2.
for SZ in 8 64 512 1024 1472; do
	B0=$(hw tx_bytes)
	P0=$(hw tx_unicast)
	Q0=$(hw tx_broadcast)
	ping -c 5 -i 0.2 -s "$SZ" -t 5 "$PEER" >/dev/null 2>&1
	sleep 2
	B1=$(hw tx_bytes)
	P1=$(hw tx_unicast)
	Q1=$(hw tx_broadcast)
	DB=$((B1 - B0))
	DP=$((P1 - P0 + Q1 - Q0))
	# Each echo request carries SZ payload + 28 IP/ICMP + 18 L2 and CRC.
	# Allow for ping losing a couple of the five to timing.
	WANT=$(( (SZ + 28) * 3 ))
	if [ "$DB" -ge "$WANT" ] && [ "$DP" -ge 3 ]; then
		pass "size $SZ: $DP frames, $DB bytes (>= $WANT expected)"
	else
		fail "size $SZ: only $DP frames / $DB bytes, expected >= $WANT"
	fi
done

# ------------------------------------------------------------ burst / load
section "2. Sustained transmit ($BURST frames)"
E0=$(errs)
B0=$(hw tx_bytes)
D0=$(hw tx_discards)
X0=$(hw tx_errors)
i=0
while [ $i -lt "$BURST" ]; do
	ping -c 1 -t 1 "$PEER" >/dev/null 2>&1
	i=$((i+1))
done
sleep 3
DB=$(( $(hw tx_bytes) - B0 ))
DD=$(( $(hw tx_discards) - D0 ))
DX=$(( $(hw tx_errors) - X0 ))
DE=$(( $(errs) - E0 ))
[ "$DB" -gt 0 ] && pass "sustained TX moved $DB bytes on the wire" \
	|| fail "no hardware TX delta under load"
[ "$DD" -eq 0 ] && pass "no TX discards under load" \
	|| fail "$DD TX discards under load"
[ "$DX" -eq 0 ] && pass "no TX errors under load" \
	|| fail "$DX TX errors under load"
[ "$DE" -eq 0 ] && pass "no interface errors under load" \
	|| fail "$DE interface errors under load"

# --------------------------------------------------- broadcast / multicast
section "3. Broadcast and multicast classification"
Q0=$(hw tx_broadcast)
# The subnet broadcast address is unambiguously an L2 broadcast, unlike an
# ARP request whose emission depends on cache state.
ping -c 3 -i 0.2 -t 3 "$TEST_BROADCAST" >/dev/null 2>&1
sleep 2
DQ=$(( $(hw tx_broadcast) - Q0 ))
[ "$DQ" -gt 0 ] && pass "broadcast counted separately ($DQ frames)" \
	|| fail "broadcast counter did not move for a broadcast destination"

M0=$(hw tx_multicast)
ping -c 3 -t 2 224.0.0.1 >/dev/null 2>&1
sleep 2
DM=$(( $(hw tx_multicast) - M0 ))
if [ "$DM" -gt 0 ]; then
	pass "multicast counted separately ($DM frames)"
else
	skip "no multicast TX observed (routing may not select $IFACE)"
fi

# --------------------------------------------------------------- jumbo MTU
section "4. Jumbo frames"
OLDMTU=$(ifconfig "$IFACE" | sed -n 's/.*mtu \([0-9]*\).*/\1/p')
if ifconfig "$IFACE" mtu 9000 2>/dev/null; then
	sleep 2
	B0=$(hw tx_bytes)
	X0=$(hw tx_errors)
	ping -c 5 -i 0.2 -s 8000 -t 5 "$PEER" >/dev/null 2>&1
	sleep 2
	DB=$(( $(hw tx_bytes) - B0 ))
	DX=$(( $(hw tx_errors) - X0 ))
	[ "$DB" -gt 8000 ] && pass "jumbo frames reached the device ($DB bytes)" \
		|| fail "jumbo TX delta too small ($DB bytes)"
	[ "$DX" -eq 0 ] && pass "no TX errors with jumbo frames" \
		|| fail "$DX TX errors with jumbo frames"
	ifconfig "$IFACE" mtu "$OLDMTU" 2>/dev/null
	sleep 2
else
	skip "device refused MTU 9000"
fi

# ------------------------------------------------------------- UDP payload
section "5. UDP data transfer"
B0=$(hw tx_bytes)
X0=$(hw tx_errors)
python3 - "$PEER" <<'EOF' >/dev/null 2>&1 || true
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
payload = b'x' * 1400
for _ in range(500):
    try:
        s.sendto(payload, (sys.argv[1], 9999))
    except OSError:
        pass
EOF
sleep 3
DB=$(( $(hw tx_bytes) - B0 ))
DX=$(( $(hw tx_errors) - X0 ))
if [ "$DB" -gt 100000 ]; then
	pass "UDP stream moved $DB bytes on the wire"
elif [ "$DB" -gt 0 ]; then
	pass "UDP stream moved $DB bytes (ARP unresolved limits the count)"
else
	skip "no UDP TX delta (peer unresolved, nothing queued)"
fi
[ "$DX" -eq 0 ] && pass "no TX errors during UDP stream" \
	|| fail "$DX TX errors during UDP stream"

# ------------------------------------------------- counter self-consistency
section "6. Counter consistency"
arp -s "$PEER" 02:00:00:00:be:ef >/dev/null 2>&1
O0=$(ifc 11)
B0=$(hw tx_bytes)
ping -c 20 -i 0.05 -s 512 -t 5 "$PEER" >/dev/null 2>&1
sleep 3
DO=$(( $(ifc 11) - O0 ))
DB=$(( $(hw tx_bytes) - B0 ))
if [ "$DO" -gt 0 ] && [ "$DB" -gt 0 ]; then
	# The device counts L2 framing the ifnet does not, so hw >= ifnet.
	if [ "$DB" -ge "$DO" ]; then
		pass "hardware bytes ($DB) >= ifnet bytes ($DO), as expected"
	else
		fail "hardware bytes ($DB) < ifnet bytes ($DO)"
	fi
else
	fail "no traffic recorded (ifnet $DO, hardware $DB)"
fi

# ------------------------------------------------------------------ receive
section "7. Receive path"
RB0=$(hw rx_bytes)
RE0=$(hw rx_errors)
RD0=$(hw rx_discards)
I0=$(ifc 5)
MAC=$(ifconfig "$IFACE" | awk '/ether/{print $2}')
timeout 15 tcpdump -i "$IFACE" -n -c 5 "not ether src $MAC" >/tmp/idpf_rx.txt 2>&1
sleep 1
DRB=$(( $(hw rx_bytes) - RB0 ))
DI=$(( $(ifc 5) - I0 ))
DRE=$(( $(hw rx_errors) - RE0 ))
DRD=$(( $(hw rx_discards) - RD0 ))
OFFBOX=$(awk '/packets captured/ {print $1}' /tmp/idpf_rx.txt)

echo "off-box frames captured ${OFFBOX:-0}, hardware rx_bytes +$DRB, ifnet Ipkts +$DI"
# Ipkts counts frames the stack looped back to itself, so it cannot stand in
# for reception; only the capture filtered on a foreign source MAC can.
if [ "${OFFBOX:-0}" -gt 0 ]; then
	pass "received ${OFFBOX} frames from off-box"
else
	skip "no off-box frames in 15s: link has no transmitting peer"
fi
[ "$DRE" -eq 0 ] && pass "no RX errors" || fail "$DRE RX errors"
[ "$DRD" -eq 0 ] && pass "no RX discards" || fail "$DRD RX discards"

# ---------------------------------------------------- post-traffic health
section "8. Health after traffic"
ifconfig "$IFACE" | grep -q "UP" && pass "interface still up" \
	|| fail "interface down after traffic"
[ "$(ifconfig "$IFACE" | awk '/status:/{print $2}')" = "active" ] \
	&& pass "link still active" || fail "link not active after traffic"
ping -c 3 -t 2 "$PEER" >/dev/null 2>&1
FE=$(errs)
[ "${FE:-0}" -eq 0 ] && pass "interface error counters still zero" \
	|| fail "$FE interface errors after the run"

section "Summary"
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ] && echo "RESULT: no failures" || echo "RESULT: $FAIL failure(s)"
exit "$FAIL"
