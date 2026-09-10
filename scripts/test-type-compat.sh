#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
for mode in -D__KERNEL__ -U__KERNEL__; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsyntax-only -x c \
        "$mode" -I"$ROOT/idpf/src" - <<'EOF'
#include <linux/bits.h>
_Static_assert(sizeof(u64) == 8, "64-bit shared type");
_Static_assert(GENMASK(7, 4) == 0xf0UL, "32-bit mask");
_Static_assert(GENMASK_ULL(63, 0) == ~0ULL, "full mask");
_Static_assert(GENMASK_ULL(63, 60) == 0xf000000000000000ULL, "high bits");
_Static_assert(BIT(3) == 8, "bit helper");
EOF
done
echo 'Type compatibility: PASS in kernel and userspace modes'