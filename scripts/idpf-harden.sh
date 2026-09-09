#!/bin/sh
#
# idpf-harden.sh - feature and robustness tests for the FreeBSD IDPF driver.
#
# Complements idpf-validate.sh, which covers attach, link, datapath, MTU,
# cycling and detach.  This script drives the iflib callbacks that suite
# never reaches: vlan_register/unregister, promisc_set, multi_set, the
# SIOCSIFCAP capability path and the MAC filter path, then hammers them.
#
# Negative cases are assertions too: rejecting a bad MTU or a malformed
# private ioctl is required behaviour, not an error.
# shellcheck disable=SC2015

set -u

IFACE="${1:-idpf0}"
KMOD="${2:-/tmp/idpfbuild/idpf/src/if_idpf.ko}"
TESTIP="${TESTIP:-192.168.211.1/24}"
TESTPEER="${TESTPEER:-192.168.211.99}"
VLANPEER="${VLANPEER:-192.168.212.99}"
VLANID="${VLANID:-101}"
STRESS="${STRESS:-20}"
DRIVER=${IFACE%%[0-9]*}
UNIT=${IFACE#"$DRIVER"}
[ -n "$UNIT" ] || UNIT=0

PASS=0
FAIL=0
section() { echo; echo "===== $* ====="; }
pass()    { echo "[PASS] $*"; PASS=$((PASS+1)); }
fail()    { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }
skip()    { echo "[SKIP] $*"; }

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

if ! ifconfig "$IFACE" >/dev/null 2>&1; then
	kldload "$KMOD" 2>/dev/null && sleep 8
fi
if ! ifconfig "$IFACE" >/dev/null 2>&1; then
	echo "[FAIL] interface $IFACE does not exist"
	echo "RESULT: 1 failure(s)"; exit 1
fi

ifconfig "$IFACE" inet "$TESTIP" alias 2>/dev/null
ifconfig "$IFACE" up
sleep 2

# Interface must still be usable after every subsection.
alive() {
	ifconfig "$IFACE" >/dev/null 2>&1 || return 1
	ifconfig "$IFACE" | grep -q "UP" || return 1
	return 0
}

# ---------------------------------------------------------------- VLAN
section "1. VLAN (ifdi_vlan_register / ifdi_vlan_unregister)"
VIF="${IFACE}.${VLANID}"
ifconfig "$VIF" destroy 2>/dev/null
if ifconfig "$VIF" create vlan "$VLANID" vlandev "$IFACE" 2>/dev/null; then
	sleep 1
	if ifconfig "$VIF" >/dev/null 2>&1; then
		pass "vlan interface $VIF created"
	else
		fail "vlan interface $VIF missing after create"
	fi
	ifconfig "$VIF" inet 192.168.212.1/24 up 2>/dev/null
	sleep 1
	if ifconfig "$VIF" | grep -q "UP"; then
		pass "vlan interface came up"
	else
		fail "vlan interface did not come up"
	fi

	ping -c 2 -t 2 "$VLANPEER" >/dev/null 2>&1
	VERR=$(netstat -I "$IFACE" -b | awk 'NR==2 {print $6+$10}')
	if [ "${VERR:-0}" -eq 0 ]; then
		pass "no interface errors after vlan traffic"
	else
		fail "interface reports $VERR errors after vlan traffic"
	fi

	if ifconfig "$VIF" destroy 2>/dev/null; then
		pass "vlan interface destroyed"
	else
		fail "vlan interface destroy failed"
	fi
	sleep 1
	if alive; then
		pass "parent survived vlan teardown"
	else
		fail "parent broken after vlan teardown"
	fi
else
	skip "cannot create vlan interface"
fi

# ------------------------------------------------------------ promiscuous
section "2. Promiscuous mode (ifdi_promisc_set)"
ifconfig "$IFACE" promisc 2>/dev/null
sleep 1
if ifconfig "$IFACE" | grep -q "PROMISC"; then
	pass "promiscuous mode enabled"
else
	fail "PROMISC flag not set"
fi
ifconfig "$IFACE" -promisc 2>/dev/null
sleep 1
ifconfig "$IFACE" | grep -q "PROMISC" \
	&& fail "PROMISC still set after disable" \
	|| pass "promiscuous mode disabled"

# allmulti travels the same virtchnl path
ifconfig "$IFACE" allmulti 2>/dev/null
sleep 1
ifconfig "$IFACE" -allmulti 2>/dev/null
sleep 1
alive && pass "interface healthy after allmulti toggle" \
	|| fail "interface broken after allmulti toggle"

# ------------------------------------------------------------- multicast
section "3. Multicast filters (ifdi_multi_set)"
# Enabling IPv6 makes the stack join the solicited-node groups.
ifconfig "$IFACE" inet6 -ifdisabled 2>/dev/null
sleep 2
MCAST_GROUPS=$(ifmcstat -i "$IFACE" 2>/dev/null | grep -c "group")
if [ "${MCAST_GROUPS:-0}" -gt 0 ]; then
	pass "interface joined $MCAST_GROUPS multicast group(s)"
else
	skip "no multicast groups reported by ifmcstat"
fi
alive && pass "interface healthy after multicast join" \
	|| fail "interface broken after multicast join"

# --------------------------------------------------------- capabilities
section "4. Capability toggling (SIOCSIFCAP)"
ORIG=$(ifconfig "$IFACE" | awk '/options=/{print $1}')
# ifconfig's parameter name differs from the token printed in options=,
# so each entry carries both.
for PAIR in rxcsum:RXCSUM txcsum:TXCSUM tso4:TSO4 tso6:TSO6 lro:LRO \
    vlanhwtag:VLAN_HWTAGGING vlanhwfilter:VLAN_HWFILTER; do
	CAP=${PAIR%%:*}
	TOK=${PAIR##*:}
	if ! ifconfig "$IFACE" | grep -qE "options=.*[<,]${TOK}[,>]"; then
		skip "$CAP ($TOK) not advertised"
		continue
	fi
	if ifconfig "$IFACE" -"$CAP" 2>/dev/null; then
		sleep 1
		if ifconfig "$IFACE" | grep -qE "options=.*[<,]${TOK}[,>]"; then
			fail "$CAP still enabled after disable"
		else
			pass "$CAP disabled"
		fi
		ifconfig "$IFACE" "$CAP" 2>/dev/null
		sleep 1
		ifconfig "$IFACE" | grep -qE "options=.*[<,]${TOK}[,>]" \
			&& pass "$CAP re-enabled" \
			|| fail "$CAP did not come back"
	else
		fail "could not disable $CAP"
	fi
done
NOW=$(ifconfig "$IFACE" | awk '/options=/{print $1}')
[ "$NOW" = "$ORIG" ] && pass "capabilities restored to $ORIG" \
	|| fail "capabilities are $NOW, expected $ORIG"
ping -c 3 -t 2 "$TESTPEER" >/dev/null 2>&1
CERR=$(netstat -I "$IFACE" -b | awk 'NR==2 {print $6+$10}')
[ "${CERR:-0}" -eq 0 ] && pass "no errors after capability toggling" \
	|| fail "$CERR errors after capability toggling"
alive && pass "interface healthy after capability toggling" \
	|| fail "interface broken after capability toggling"

# ------------------------------------------------------------ MAC filter
section "5. MAC address change (MAC filter path)"
OLDMAC=$(ifconfig "$IFACE" | awk '/ether/{print $2}')
NEWMAC="02:00:00:aa:bb:cc"
if ifconfig "$IFACE" ether "$NEWMAC" 2>/dev/null; then
	sleep 2
	CUR=$(ifconfig "$IFACE" | awk '/ether/{print $2}')
	[ "$CUR" = "$NEWMAC" ] && pass "MAC changed to $NEWMAC" \
		|| fail "MAC is $CUR, expected $NEWMAC"
	ifconfig "$IFACE" ether "$OLDMAC" 2>/dev/null
	sleep 2
	CUR=$(ifconfig "$IFACE" | awk '/ether/{print $2}')
	[ "$CUR" = "$OLDMAC" ] && pass "MAC restored to $OLDMAC" \
		|| fail "MAC not restored (now $CUR)"
else
	skip "driver rejected MAC change"
fi
alive && pass "interface healthy after MAC change" \
	|| fail "interface broken after MAC change"

# ------------------------------------------------------- MTU boundaries
section "6. MTU boundary handling"
ifconfig "$IFACE" mtu 1500 2>/dev/null
sleep 1
# Oversized MTU must be refused, not accepted and then mishandled.
if ifconfig "$IFACE" mtu 99999 2>/dev/null; then
	fail "accepted MTU 99999"
	ifconfig "$IFACE" mtu 1500 2>/dev/null
else
	pass "rejected MTU 99999"
fi
if ifconfig "$IFACE" mtu 1 2>/dev/null; then
	fail "accepted MTU 1"
	ifconfig "$IFACE" mtu 1500 2>/dev/null
else
	pass "rejected MTU 1"
fi
CUR=$(ifconfig "$IFACE" | sed -n 's/.*mtu \([0-9]*\).*/\1/p')
[ "$CUR" = "1500" ] && pass "MTU still 1500 after rejected changes" \
	|| fail "MTU is $CUR after rejected changes"

# --------------------------------------------------- private ioctl abuse
section "7. Private ioctl negative cases"
cat > /tmp/drvspec_neg.c <<'EOF'
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <net/if.h>
int main(int argc, char **argv)
{
	struct ifdrv ifd;
	char buf[256];
	int s, bad = 0;

	if (argc < 2) return 1;
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return 1;
	memset(&ifd, 0, sizeof(ifd));
	strlcpy(ifd.ifd_name, argv[1], sizeof(ifd.ifd_name));
	ifd.ifd_data = buf;

	ifd.ifd_cmd = 9999; ifd.ifd_len = sizeof(buf);
	if (ioctl(s, SIOCGDRVSPEC, &ifd) == 0) { printf("unknown cmd accepted\n"); bad++; }
	ifd.ifd_cmd = 0; ifd.ifd_len = 1;
	if (ioctl(s, SIOCGDRVSPEC, &ifd) == 0) { printf("short len accepted\n"); bad++; }
	ifd.ifd_cmd = 0; ifd.ifd_len = sizeof(buf);
	if (ioctl(s, SIOCSDRVSPEC, &ifd) == 0) { printf("SIOCSDRVSPEC accepted\n"); bad++; }
	close(s);
	printf("%s\n", bad ? "REJECT-FAIL" : "REJECT-OK");
	return bad;
}
EOF
if cc -O2 -o /tmp/drvspec_neg /tmp/drvspec_neg.c 2>/dev/null; then
	OUT=$(/tmp/drvspec_neg "$IFACE" 2>&1)
	echo "$OUT" | grep -q "REJECT-OK" \
		&& pass "malformed private ioctls rejected" \
		|| fail "private ioctl accepted bad input: $OUT"
else
	skip "could not build negative ioctl test"
fi

# ------------------------------------------------------------ statistics
section "8. Hardware statistics"
NODES=$(sysctl "dev.$DRIVER.$UNIT.stats" 2>/dev/null | wc -l | tr -d ' ')
if [ "${NODES:-0}" -ge 12 ]; then
	pass "$NODES statistics nodes published"
else
	fail "expected >=12 statistics nodes, found ${NODES:-0}"
fi
TX0=$(sysctl -n "dev.$DRIVER.$UNIT.stats.tx_bytes" 2>/dev/null || echo 0)
ping -c 5 -i 0.2 -t 2 "$TESTPEER" >/dev/null 2>&1
sleep 2
TX1=$(sysctl -n "dev.$DRIVER.$UNIT.stats.tx_bytes" 2>/dev/null || echo 0)
if [ "${TX1:-0}" -gt "${TX0:-0}" ]; then
	pass "tx_bytes advanced $TX0 -> $TX1 on the wire"
else
	fail "tx_bytes did not advance ($TX0 -> $TX1)"
fi

# ---------------------------------------------------------------- stress
section "9. Stress (${STRESS} iterations)"
i=0
BROKE=0
while [ $i -lt "$STRESS" ]; do
	ifconfig "$IFACE" down 2>/dev/null
	ifconfig "$IFACE" up 2>/dev/null
	i=$((i+1))
done
sleep 3
alive && pass "$STRESS rapid down/up cycles survived" \
	|| { fail "interface broken after rapid cycling"; BROKE=1; }

i=0
while [ $i -lt "$STRESS" ]; do
	ifconfig "$IFACE" mtu 9000 2>/dev/null
	ifconfig "$IFACE" mtu 1500 2>/dev/null
	i=$((i+1))
done
sleep 2
alive && pass "$STRESS MTU flaps survived" || fail "interface broken after MTU flapping"

i=0
while [ $i -lt "$STRESS" ]; do
	ifconfig "$IFACE" promisc 2>/dev/null
	ifconfig "$IFACE" -promisc 2>/dev/null
	i=$((i+1))
done
sleep 2
alive && pass "$STRESS promisc flaps survived" || fail "interface broken after promisc flapping"

if [ "$BROKE" -eq 0 ]; then
	ifconfig "$IFACE" up 2>/dev/null
	sleep 2
	ping -c 3 -t 2 "$TESTPEER" >/dev/null 2>&1
	ERRS=$(netstat -I "$IFACE" -b | awk 'NR==2 {print $6+$10}')
	[ "${ERRS:-0}" -eq 0 ] && pass "no errors after stress" \
		|| fail "$ERRS errors after stress"
fi

section "10. Teardown under load"
# Detach while the interface is up and a sender is active: iflib must
# quiesce the queues before the driver frees them.
ifconfig "$IFACE" up 2>/dev/null
sleep 1
( i=0; while [ $i -lt 2000 ]; do
	ping -c 1 -t 1 "$TESTPEER" >/dev/null 2>&1
	i=$((i+1))
  done ) &
LOADPID=$!
sleep 2

if kldunload if_idpf 2>/dev/null; then
	pass "kldunload succeeded with the interface up and busy"
else
	fail "kldunload failed under load"
fi
kill "$LOADPID" 2>/dev/null
wait "$LOADPID" 2>/dev/null

sleep 2
ifconfig "$IFACE" >/dev/null 2>&1 \
	&& fail "$IFACE still present after unload" \
	|| pass "$IFACE removed on unload"

if kldload "$KMOD" 2>/dev/null; then
	sleep 8
	if ifconfig "$IFACE" >/dev/null 2>&1; then
		pass "driver reloaded after teardown under load"
		ifconfig "$IFACE" inet "$TESTIP" alias 2>/dev/null
		ifconfig "$IFACE" up
		sleep 3
		ping -c 3 -t 2 "$TESTPEER" >/dev/null 2>&1
		RERR=$(netstat -I "$IFACE" -b | awk 'NR==2 {print $6+$10}')
		[ "${RERR:-0}" -eq 0 ] && pass "datapath clean after reload" \
			|| fail "$RERR errors after reload"
	else
		fail "interface missing after reload"
	fi
else
	fail "could not reload driver"
fi

section "Summary"
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && echo "RESULT: no failures" || echo "RESULT: $FAIL failure(s)"
exit "$FAIL"
