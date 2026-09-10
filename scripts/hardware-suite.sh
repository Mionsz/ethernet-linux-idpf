#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
export IDPF_IFACE=${IDPF_IFACE:-idpf0}
export TESTPEER=${TESTPEER:-192.168.211.2}
export PEER="$TESTPEER"
export TESTIP=${TESTIP:-192.168.211.1/24}
KMOD=${KMOD:?KMOD must identify the built driver}
case "${IDPF_ALLOW_HARDWARE:-0}:${IDPF_CONSOLE_CONFIRMED:-0}" in
    1:1) ;; *) echo '[FAIL] hardware and console authorization required'; exit 1 ;;
esac
if test "${1:-}" != --inside; then
    exec sh "$ROOT/link-partner.sh" with sh "$0" --inside
fi
CHILD=
cleanup() {
    if test -n "$CHILD"; then kill "$CHILD" 2>/dev/null || true; wait "$CHILD" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
for script in idpf-validate.sh idpf-datapath.sh idpf-harden.sh idpf-ptp-validate.sh; do
    echo "===== $script ====="
    sh "$ROOT/$script" "$IDPF_IFACE" "$KMOD" & CHILD=$!
    RESULT=0
    wait "$CHILD" || RESULT=$?
    CHILD=
    if test "$RESULT" -ne 0; then
        echo "[FAIL] $script exited $RESULT; stopping hardware suite"
        exit 1
    fi
    echo "[PASS] $script"
done
"$ROOT/set_irq_affinity" -s "$IDPF_IFACE"