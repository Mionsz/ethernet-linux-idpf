#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
awk '
    /^hw\(\)/ { copying=1; found++ }
    copying { print }
    /^}/ || /; }$/ { copying=0 }
    END { if (found != 1) exit 1 }
' "$ROOT/scripts/idpf-datapath.sh" > "$BUILD/counter.inc"
. "$BUILD/counter.inc"
DRIVER=idpf UNIT=0
sysctl() { return 1; }
if hw rx_errors; then echo 'FAIL: missing counter accepted'; exit 1; fi
sysctl() { echo unavailable; }
if hw rx_errors; then echo 'FAIL: nonnumeric counter accepted'; exit 1; fi
sysctl() { echo 123; }
test "$(hw rx_errors)" = 123
echo 'PASS: unavailable counters cannot masquerade as zero'