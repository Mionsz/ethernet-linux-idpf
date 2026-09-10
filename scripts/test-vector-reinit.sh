#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
awk '
    /^idpf_vport_intr_map_vector_to_qs\(/ { print previous; copying = 1; found++ }
    copying { print }
    /^}/ { copying = 0 }
    { previous = $0 }
    END { if (found != 1) exit 1 }
' "$ROOT/idpf/src/idpf_txrx.c" > "$BUILD/vectors_under_test.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$BUILD" \
    "$ROOT/idpf/test/user/test_vector_reinit.c" -o "$BUILD/test_vector_reinit"
"$BUILD/test_vector_reinit"