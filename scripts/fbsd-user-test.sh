#!/bin/sh
# Build and run the userspace idpf control-queue tests on a FreeBSD host.
#
# Safe by construction: no kernel module is built or loaded, so a failure is a
# failed process rather than a wedged machine. Requires no /usr/src.
#
# Usage: [FBSD_HOST=user@host] fbsd-user-test.sh [error-lines-to-show]
set -eu

HOST=${FBSD_HOST:-10.102.18.118}
LINES=${1:-40}
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
REMOTE_DIR=${IDPF_REMOTE_DIR:-/tmp/idpfuser}
RUN_TESTS=${RUN_TESTS:-1}

case "$REMOTE_DIR:$LINES:$RUN_TESTS" in *[!a-zA-Z0-9_./:-]*) echo 'Invalid build path/options' >&2; exit 1 ;; esac
ARCHIVE=$(mktemp /tmp/idpf-user.XXXXXX.tgz)
trap 'rm -f "$ARCHIVE"' EXIT
tar -C "$REPO_ROOT" --exclude='.git' --exclude='*.o' --exclude='test_ctlq' \
	--exclude='idpf/src/machine' --exclude='idpf/src/x86' --exclude='idpf/src/i386' \
	-czf "$ARCHIVE" idpf/src idpf/shared idpf/test/user scripts
scp -q -o BatchMode=yes -o ConnectTimeout=30 "$ARCHIVE" "$HOST:$ARCHIVE"

ssh -T -o BatchMode=yes -o ConnectTimeout=30 "$HOST" \
	"env RUN_TESTS='$RUN_TESTS' REMOTE_DIR='$REMOTE_DIR' ARCHIVE='$ARCHIVE' LINES='$LINES' sh -s" <<'REMOTE_EOF'
	set -eu
	BUILD=$(mktemp -d "$REMOTE_DIR.XXXXXX")
	tar -xzf "$ARCHIVE" -C "$BUILD"
	rm -f "$ARCHIVE"
	cd "$BUILD/idpf/test/user"
	if command -v gmake >/dev/null 2>&1; then MAKE=gmake; else MAKE=make; fi
	rc=0
	$MAKE > "$BUILD/build.log" 2>&1 || rc=$?
	echo "=== build exit $rc ==="
	if [ $rc -ne 0 ]; then
		grep -n 'error:' "$BUILD/build.log" | head -"$LINES"
		echo '--- distinct error kinds ---'
		grep -o 'error: .*' "$BUILD/build.log" |
		    sed 's/[0-9][0-9]*/N/g' | sort -u | head -25
		exit 1
	fi
	if [ "$RUN_TESTS" = 1 ]; then
		echo '=== run ==='
		./test_ctlq
		sh "$BUILD/scripts/test-port-userspace.sh"
	else
		echo '=== run skipped ==='
	fi
	echo "artifacts: $BUILD"
REMOTE_EOF
