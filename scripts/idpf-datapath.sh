#!/bin/sh
#
# idpf-datapath.sh - packet and data transfer tests for the FreeBSD IDPF
# driver.
#
# Hardware counters supplement, but never replace, verified peer delivery.
#
# shellcheck disable=SC2015
# Run on the FreeBSD target as root:
#     ./idpf-datapath.sh [interface] [module-path]

set -eu

IFACE="${1:-idpf0}"
KMOD="${2:-/tmp/idpfbuild/idpf/src/if_idpf.ko}"
TESTIP="${TESTIP:-192.168.211.1/24}"
PEER="${TESTPEER:-${PEER:-192.168.211.2}}"
PEER_IFACE=${PEER_IFACE:-ice0}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
export IDPF_IFACE="$IFACE" TESTIP TESTPEER="$PEER"
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
hw() {
	VALUE=$(sysctl -n "dev.$DRIVER.$UNIT.stats.$1") || return 1
	case "$VALUE" in ''|*[!0-9]*) echo "Invalid counter $1: $VALUE" >&2; return 1 ;; esac
	printf '%s\n' "$VALUE"
}
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

sh "$SCRIPT_DIR/link-partner.sh" confirm || exit 1

if [ "$(sysctl -n "dev.$DRIVER.$UNIT.stats.tx_bytes" 2>/dev/null || echo missing)" = "missing" ]; then
	echo "[FAIL] hardware counters not published; cannot verify the wire"
	echo "RESULT: 1 failure(s)"; exit 1
fi

OLDMTU=$(ifconfig "$IFACE" | sed -n 's/.*mtu \([0-9]*\).*/\1/p')
PEERMTU=$(sh "$SCRIPT_DIR/link-partner.sh" exec ifconfig "$PEER_IFACE" | sed -n 's/.*mtu \([0-9]*\).*/\1/p')
restore_mtu() {
	ifconfig "$IFACE" mtu "$OLDMTU" || return 1
	sh "$SCRIPT_DIR/link-partner.sh" exec ifconfig "$PEER_IFACE" mtu "$PEERMTU"
}
trap 'rc=$?; restore_mtu || rc=1; exit "$rc"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# ------------------------------------------------------- packet size sweep
section "1. Packet size sweep (hardware TX counters)"
# ping payload sizes; each ICMP frame is payload + 8 ICMP + 20 IP + 14 L2.
for SZ in 8 64 512 1024 1472; do
	B0=$(hw tx_bytes)
	P0=$(hw tx_unicast)
	Q0=$(hw tx_broadcast)
	ping -n -S "${TESTIP%/*}" -c 5 -i 0.2 -s "$SZ" -t 5 "$PEER" >/dev/null 2>&1 || fail "size $SZ: peer did not reply"
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
sh "$SCRIPT_DIR/link-partner.sh" confirm || exit 1
E0=$(errs)
B0=$(hw tx_bytes)
D0=$(hw tx_discards)
X0=$(hw tx_errors)
i=0
while [ $i -lt "$BURST" ]; do
	ping -n -S "${TESTIP%/*}" -c 1 -t 2 "$PEER" >/dev/null 2>&1 || fail "lost reply in burst packet $i"
	i=$((i+1))
done
sleep 3
NOW=$(hw tx_bytes); DB=$((NOW - B0))
NOW=$(hw tx_discards); DD=$((NOW - D0))
NOW=$(hw tx_errors); DX=$((NOW - X0))
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
sh "$SCRIPT_DIR/link-partner.sh" confirm || exit 1
Q0=$(hw tx_broadcast)
# The subnet broadcast address is unambiguously an L2 broadcast, unlike an
# ARP request whose emission depends on cache state.
ping -S "${TESTIP%/*}" -c 3 -i 0.2 -t 3 "$TEST_BROADCAST" >/dev/null 2>&1 || true
sleep 2
NOW=$(hw tx_broadcast); DQ=$((NOW - Q0))
[ "$DQ" -gt 0 ] && pass "broadcast counted separately ($DQ frames)" \
	|| fail "broadcast counter did not move for a broadcast destination"

M0=$(hw tx_multicast)
ping -I "${TESTIP%/*}" -c 3 -t 2 224.0.0.1 >/dev/null 2>&1 || true
sleep 2
NOW=$(hw tx_multicast); DM=$((NOW - M0))
if [ "$DM" -gt 0 ]; then
	pass "multicast counted separately ($DM frames)"
else
	skip "no multicast TX observed (routing may not select $IFACE)"
fi

# --------------------------------------------------------------- jumbo MTU
section "4. Jumbo frames"
sh "$SCRIPT_DIR/link-partner.sh" confirm || exit 1
if ifconfig "$IFACE" mtu 9000 2>/dev/null; then
	sh "$SCRIPT_DIR/link-partner.sh" exec ifconfig "$PEER_IFACE" mtu 9000 || exit 1
	sleep 2
	B0=$(hw tx_bytes)
	X0=$(hw tx_errors)
	ping -n -D -S "${TESTIP%/*}" -c 5 -i 0.2 -s 8000 -t 5 "$PEER" >/dev/null 2>&1 || fail "jumbo peer delivery failed"
	sleep 2
	NOW=$(hw tx_bytes); DB=$((NOW - B0))
	NOW=$(hw tx_errors); DX=$((NOW - X0))
	[ "$DB" -gt 8000 ] && pass "jumbo frames reached the device ($DB bytes)" \
		|| fail "jumbo TX delta too small ($DB bytes)"
	[ "$DX" -eq 0 ] && pass "no TX errors with jumbo frames" \
		|| fail "$DX TX errors with jumbo frames"
	restore_mtu || exit 1
	sleep 2
else
	skip "device refused MTU 9000"
fi

# ------------------------------------------------------------- UDP payload
section "5. UDP data transfer"
B0=$(hw tx_bytes)
X0=$(hw tx_errors)
if "${PYTHON:-python3}" "$SCRIPT_DIR/peer-udp.py"; then
	pass "UDP payloads received and echoed intact by peer"
else
	fail "UDP peer delivery failed"
fi
sleep 3
NOW=$(hw tx_bytes); DB=$((NOW - B0))
NOW=$(hw tx_errors); DX=$((NOW - X0))
if [ "$DB" -gt 100000 ]; then
	pass "UDP stream moved $DB bytes on the wire"
elif [ "$DB" -gt 0 ]; then
	fail "UDP hardware delta too small ($DB bytes)"
else
	fail "no UDP TX delta"
fi
[ "$DX" -eq 0 ] && pass "no TX errors during UDP stream" \
	|| fail "$DX TX errors during UDP stream"

# ------------------------------------------------- counter self-consistency
section "6. Counter consistency"
sh "$SCRIPT_DIR/link-partner.sh" confirm || exit 1
O0=$(ifc 11)
B0=$(hw tx_bytes)
ping -n -S "${TESTIP%/*}" -c 20 -i 0.05 -s 512 -t 5 "$PEER" >/dev/null 2>&1 || fail "counter test peer delivery failed"
sleep 3
DO=$(( $(ifc 11) - O0 ))
NOW=$(hw tx_bytes); DB=$((NOW - B0))
if [ "$DO" -gt 0 ] && [ "$DB" -gt 0 ]; then
	pass "counters advanced during verified traffic (ifnet $DO, hardware $DB; may share a source)"
else
	fail "no traffic recorded (ifnet $DO, hardware $DB)"
fi

# ------------------------------------------------------------------ receive
section "7. Receive path"
RB0=$(hw rx_bytes)
RE0=$(hw rx_errors)
RD0=$(hw rx_discards)
I0=$(ifc 5)
if sh "$SCRIPT_DIR/link-partner.sh" confirm; then
	pass "receive path delivered verified peer traffic"
else
	fail "receive path failed with transmitting peer"
fi
sleep 1
NOW=$(hw rx_bytes); DRB=$((NOW - RB0))
DI=$(( $(ifc 5) - I0 ))
NOW=$(hw rx_errors); DRE=$((NOW - RE0))
NOW=$(hw rx_discards); DRD=$((NOW - RD0))
echo "hardware rx_bytes +$DRB, ifnet Ipkts +$DI"
[ "$DRB" -gt 0 ] && [ "$DI" -ge 10 ] || fail "RX counters did not reflect confirmed traffic"
[ "$DRE" -eq 0 ] && pass "no RX errors" || fail "$DRE RX errors"
[ "$DRD" -eq 0 ] && pass "no RX discards" || fail "$DRD RX discards"

# ---------------------------------------------------- post-traffic health
section "8. Health after traffic"
ifconfig "$IFACE" | grep -q "UP" && pass "interface still up" \
	|| fail "interface down after traffic"
[ "$(ifconfig "$IFACE" | awk '/status:/{print $2}')" = "active" ] \
	&& pass "link still active" || fail "link not active after traffic"
sh "$SCRIPT_DIR/link-partner.sh" confirm || fail "post-traffic peer confirmation failed"
FE=$(errs)
[ "${FE:-0}" -eq 0 ] && pass "interface error counters still zero" \
	|| fail "$FE interface errors after the run"

section "Summary"
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ] && echo "RESULT: no failures" || echo "RESULT: $FAIL failure(s)"
[ "$FAIL" -eq 0 ]
