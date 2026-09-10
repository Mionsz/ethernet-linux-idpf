#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
awk '
    /^idpf_alloc_taskqueues\(/ || /^idpf_free_taskqueues\(/ {
        print previous; copying = 1; found++
    }
    copying { print }
    /^}/ { copying = 0 }
    { previous = $0 }
    END { if (found != 2) exit 1 }
' "$ROOT/idpf/src/idpf_main.c" > "$BUILD/tasks_under_test.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$BUILD" \
    "$ROOT/idpf/test/user/test_taskqueue_init.c" -o "$BUILD/test_taskqueue_init"
"$BUILD/test_taskqueue_init"