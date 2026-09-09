#!/bin/sh
#
# idpf_ptp_test_fixed.sh - PTP validation for the FreeBSD IDPF driver.
#
# The original script gated on SIOC[SG]HWTSTAMP.  FreeBSD defines no such
# ioctl (sockio.h stops at 139 in that block), so _IOWR('i',140) falls
# through to ether_ioctl() and returns EINVAL without ever reaching the
# driver.  Timestamping state is reported through SIOCGDRVSPEC instead.
#
# shellcheck disable=SC2015
# A control plane that does not offer PTP is a PASS reporting capable=0,
# not a failure: the driver is expected to say so rather than error.

set -u
IFACE="${1:-idpf0}"
KMOD="${KMOD:-/tmp/idpfbuild/src/if_idpf.ko}"
DRIVER=${IFACE%%[0-9]*}
UNIT=${IFACE#"$DRIVER"}
[ -n "$UNIT" ] || UNIT=0
PASS=0
FAIL=0

gate()  { echo; echo "===== $* ====="; }
pass()  { echo "[PASS] $*"; PASS=$((PASS+1)); }
fail()  { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }

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

gate "1. Interface state"
if ! ifconfig "$IFACE" >/dev/null 2>&1; then
	# A preceding test may have unloaded the module.
	kldload "$KMOD" 2>/dev/null && sleep 8
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
SRC=${TMPDIR:-/tmp}/ptp_drvspec_test.c
BIN=${TMPDIR:-/tmp}/ptp_drvspec_test
trap 'rm -f "$SRC" "$BIN"' EXIT HUP INT TERM
cat > "$SRC" <<'EOF'
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>

struct idpf_tstamp_config {
	uint32_t capable;
	int32_t tx_type;
	int32_t rx_filter;
	uint32_t flags;
};

int
main(int argc, char **argv)
{
	struct idpf_tstamp_config config;
	struct ifdrv request;
	int socket_fd;
	int rejected;

	if (argc != 2)
		return (2);
	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_fd < 0)
		return (1);
	memset(&config, 0, sizeof(config));
	memset(&request, 0, sizeof(request));
	strlcpy(request.ifd_name, argv[1], sizeof(request.ifd_name));
	request.ifd_cmd = 1;
	request.ifd_len = sizeof(config);
	request.ifd_data = &config;
	if (ioctl(socket_fd, SIOCGDRVSPEC, &request) != 0)
		return (1);
	printf("capable=%u tx_type=%d rx_filter=%d flags=%u\n", config.capable,
	    config.tx_type, config.rx_filter, config.flags);
	request.ifd_len = sizeof(config) - 1;
	errno = 0;
	rejected = ioctl(socket_fd, SIOCGDRVSPEC, &request) != 0 && errno == EINVAL;
	printf("short-length rejected: %s\n", rejected ? "yes" : "no");
	close(socket_fd);
	return (rejected ? 0 : 1);
}
EOF
cc -O2 -Wall -Wextra -o "$BIN" "$SRC" || {
	fail "compile failed"
	echo "RESULT: $FAIL failure(s)"
	exit 1
}

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
NODE=$(sysctl -n "dev.$DRIVER.$UNIT.ptp_clock_ns" 2>/dev/null)
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
