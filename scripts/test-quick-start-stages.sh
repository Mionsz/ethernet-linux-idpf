#!/usr/bin/env bash
set -eu
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
awk '
    /^record_stage\(\)/ || /^run_stage\(\)/ { copying=1; found++ }
    copying { print }
    /^}/ { copying=0 }
    END { if (found != 2) exit 1 }
' "$ROOT/quick_start.sh" > "$BUILD/stages.sh"
source "$BUILD/stages.sh"
log_dir=$BUILD
stage_names=(); stage_states=(); stage_details=(); failures=0
for code in 1 2; do
    result=0
    run_stage "failure-$code" sh -c "exit $code" || result=$?
    [[ $result == "$code" ]] || exit 1
    [[ ${stage_states[-1]} == FAIL ]] || { echo "FAIL: exit $code not reported as failure"; exit 1; }
done
run_stage success true
[[ $failures == 2 && ${stage_states[-1]} == PASS ]]
echo 'PASS: failed commands gate hardware regardless of exit code'