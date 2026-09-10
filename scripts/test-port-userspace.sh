#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
for test in test-mmio test-intr-cleanup test-vector-reinit test-taskqueue-init test-type-compat test-adi-vector; do
    sh "$ROOT/$test.sh"
done