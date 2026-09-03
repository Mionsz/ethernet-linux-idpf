#!/bin/sh
#
# idpf_ptp_test_fixed.sh - PTP validation for the FreeBSD IDPF driver.
#
# The original script gated on SIOC[SG]HWTSTAMP.  FreeBSD defines no such
# ioctl (sockio.h stops at 139 in that block), so _IOWR('i',140) falls
# through to ether_ioctl() and returns EINVAL without ever reaching the
# driver.  Timestamping state is reported through SIOCGDRVSPEC instead.
#
# A control plane that does not offer PTP is a PASS reporting capable=0,
# not a failure: the driver is expected to say so rather than error.

set -u
IFACE="${1:-idpf0}"
PASS=0
FAIL=0

gate()  { echo; echo "===== $* ====="; }
pass()  { echo "[PASS] $*"; PASS=$((PASS+1)); }
fail()  { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

gate "1. Interface state"
if ! ifconfig "$IFACE" >/dev/null 2>&1; then
	# A preceding test may have unloaded the module.
	kldload "${KMOD:-/tmp/idpfbuild/src/if_idpf.ko}" 2>/dev/null && sleep 8
fi
if ! ifconfig "$IFACE" >/dev/null 2>&1; then
	fail "interface $IFACE does not exist"
	echo "RESULT: $FAIL failure(s)"; exit 1
fi
ifconfig "$IFACE" up
sleep 1
STATUS=$(ifconfig "$IFACE" | awk '/status:/ {print $2}')
[ "$STATUS" = "active" ] && pass "link active" || fail "link not active ($STATUS)"

gate "2. Timestamp configuration query (SIOCGDRVSPEC)"
SRC=/tmp/ptp_drvspec_test.c
BIN=/tmp/ptp_drvspec_test
if [ ! -x "$BIN" ]; then
	if [ ! -f "$SRC" ]; then
		fail "missing $SRC"; echo "RESULT: $FAIL failure(s)"; exit 1
	fi
	cc -O2 -Wall -o "$BIN" "$SRC" || { fail "compile failed"; exit 1; }
fi

OUT=$("$BIN" "$IFACE" 2>&1)
RC=$?
echo "$OUT"
if [ $RC -ne 0 ]; then
	fail "SIOCGDRVSPEC timestamp query failed"
else
	pass "timestamp query returned successfully"
	echo "$OUT" | grep -q "short-length rejected: yes" \
		&& pass "driver rejects a wrong-length request" \
		|| fail "driver accepted a wrong-length request"
fi

gate "3. Capability agreement"
CAPABLE=$(echo "$OUT" | sed -n 's/.*capable=\([0-9]*\).*/\1/p')
OTHER=$(dmesg | grep -o 'other_caps 0x[0-9a-f]*' | tail -1 | awk '{print $2}')
if [ -n "$OTHER" ]; then
	# VIRTCHNL2_CAP_PTP is bit 13.
	BIT=$(python3 -c "print((int('$OTHER',16) >> 13) & 1)" 2>/dev/null)
	echo "other_caps=$OTHER  ptp_bit=$BIT  capable=$CAPABLE"
	if [ "$BIT" = "$CAPABLE" ]; then
		pass "reported capability matches negotiated caps bit 13"
	else
		fail "capability mismatch: bit13=$BIT but driver says $CAPABLE"
	fi
else
	echo "no other_caps line in dmesg; skipping cross-check"
fi

gate "4. Clock sysctl consistency"
NODE=$(sysctl -n dev.idpf.0.ptp_clock_ns 2>/dev/null)
if [ "${CAPABLE:-0}" = "0" ]; then
	[ -z "$NODE" ] && pass "no clock node published, matching capable=0" \
		|| fail "clock node present though PTP was declined"
else
	[ -n "$NODE" ] && pass "clock node reads $NODE" \
		|| fail "PTP negotiated but no clock node"
fi

echo
echo "===== Summary: $PASS passed, $FAIL failed ====="
[ $FAIL -eq 0 ] && echo "RESULT: no failures" || echo "RESULT: $FAIL failure(s)"
exit $FAIL
