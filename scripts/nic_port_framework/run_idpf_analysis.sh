#!/usr/bin/env bash
# Deprecated entrypoint. Kept so existing references keep working.
# The staged runner with logging and per-stage reporting is run_framework.sh.
set -euo pipefail
DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")")"
echo "run_idpf_analysis.sh is deprecated; running run_framework.sh instead." >&2
exec "${DIR}/run_framework.sh" "$@"
