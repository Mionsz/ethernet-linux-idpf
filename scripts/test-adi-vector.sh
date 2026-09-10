#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
awk '
    /^idpf_adi_hw_vector\(/ { print previous; copying = 1; found++ }
    copying { print }
    /^}/ { copying = 0 }
    { previous = $0 }
    END { if (found != 1) exit 1 }
' "$ROOT/idpf/src/idpf_adi.c" > "$BUILD/adi_under_test.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$BUILD" -x c -o "$BUILD/test_adi" - <<'EOF'
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef uint16_t u16;
struct idpf_adapter { u16 num_msix_entries; u16 *vector_ids; };
#include "adi_under_test.inc"
int main(void)
{
    u16 ids[] = {0, 282, 511};
    struct idpf_adapter adapter = {3, ids};
    int failures = 0;
    failures += idpf_adi_hw_vector(&adapter, 1) != 282;
    failures += idpf_adi_hw_vector(&adapter, 2) != 511;
    failures += idpf_adi_hw_vector(&adapter, 3) != -1;
    adapter.vector_ids = NULL;
    failures += idpf_adi_hw_vector(&adapter, 0) != -1;
    printf("ADI vector mapping: %d failures\n", failures);
    return failures != 0;
}
EOF
"$BUILD/test_adi"