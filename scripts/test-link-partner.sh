#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
PARTNER="$ROOT/scripts/link-partner.sh"
test -f "$PARTNER" || { echo 'FAIL: link-partner.sh missing'; exit 1; }
test "$(uname -s)" = FreeBSD || { echo 'FreeBSD required'; exit 2; }
test "$(id -u)" = 0 || { echo 'root required'; exit 1; }
PAIR=$(ifconfig epair create)
export IDPF_IFACE="$PAIR" PEER_IFACE="${PAIR%a}b" PEER_MODULE=none
export IDPF_PEER_JAIL="idpf_test_$$" IDPF_ALLOW_HARDWARE=1
export TESTIP=192.168.211.1/24 TESTPEER=192.168.211.2
cleanup() {
    sh "$PARTNER" down || true
    ifconfig "$PAIR" destroy
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
sh "$PARTNER" up
sh "$PARTNER" status
sh "$PARTNER" confirm
"${PYTHON:-python3}" "$ROOT/scripts/peer-udp.py"
sh "$PARTNER" exec ifconfig "$PEER_IFACE" down
if sh "$PARTNER" confirm; then
    echo 'FAIL: down peer reported success'; exit 1
fi
sh "$PARTNER" exec ifconfig "$PEER_IFACE" up
sh "$PARTNER" down
ifconfig "$PEER_IFACE" >/dev/null
if jls -j "$IDPF_PEER_JAIL" >/dev/null 2>&1; then
    echo 'FAIL: jail left behind'; exit 1
fi
sh "$PARTNER" down
if sh "$PARTNER" with false; then
    echo 'FAIL: failed child reported success'; exit 1
fi
ifconfig "$PEER_IFACE" >/dev/null
test ! -d "/var/run/idpf-link-$IDPF_IFACE"
if sh "$PARTNER" with sh -c 'kill -TERM "$PPID"; exit 0'; then
    echo 'FAIL: interrupted session reported success'; exit 1
fi
ifconfig "$PEER_IFACE" >/dev/null
test ! -d "/var/run/idpf-link-$IDPF_IFACE"
sh "$PARTNER" with sh "$PARTNER" confirm
ifconfig "$PEER_IFACE" >/dev/null
test ! -d "/var/run/idpf-link-$IDPF_IFACE"
echo 'PASS: VNET traffic, UDP, link failure, and normal/failure/signal cleanup'