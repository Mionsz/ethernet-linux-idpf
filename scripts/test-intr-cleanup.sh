#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
awk '
    /^idpf_intr_req\(/ { print previous; copying = 1; found++ }
    copying { print }
    /^}/ { copying = 0 }
    { previous = $0 }
    END { if (found != 1) exit 1 }
' "$ROOT/idpf/src/idpf_lib.c" > "$BUILD/intr_under_test.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$BUILD" \
    "$ROOT/idpf/test/user/test_intr_cleanup.c" -o "$BUILD/test_intr_cleanup"
ulimit -c 0
result=0
for fault in 1 2 3 4 5 6 7; do
    "$BUILD/test_intr_cleanup" "$fault" || result=1
done
exit "$result"