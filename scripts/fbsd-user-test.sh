#!/bin/sh
# Build and run the userspace idpf control-queue tests on a FreeBSD host.
#
# Safe by construction: no kernel module is built or loaded, so a failure is a
# failed process rather than a wedged machine. Requires no /usr/src.
#
# Usage: [FBSD_HOST=user@host] fbsd-user-test.sh [error-lines-to-show]
set -e

HOST=${FBSD_HOST:-10.102.18.118}
LINES=${1:-40}
LOCAL_SRC=/opt/ethernet-linux-idpf/idpf
REMOTE_DIR=${IDPF_REMOTE_DIR:-/tmp/idpfuser}
RUN_TESTS=${RUN_TESTS:-1}

cd "$LOCAL_SRC"
tar -czf /tmp/idpf-user.tgz src shared test/user
scp -q -o BatchMode=yes -o ConnectTimeout=30 /tmp/idpf-user.tgz "$HOST:/tmp/"

ssh -o BatchMode=yes -o ConnectTimeout=30 "$HOST" \
	"RUN_TESTS='$RUN_TESTS' REMOTE_DIR='$REMOTE_DIR' LINES='$LINES' sh -s" <<'REMOTE_EOF'
	rm -rf $REMOTE_DIR && mkdir -p $REMOTE_DIR &&
	tar -xzf /tmp/idpf-user.tgz -C $REMOTE_DIR &&
	cd $REMOTE_DIR/test/user &&
	gmake --version >/dev/null 2>&1 && MAKE=gmake || MAKE=make
	$MAKE > /tmp/idpf-user-build.log 2>&1
	rc=$?
	echo "=== build exit $rc ==="
	if [ $rc -ne 0 ]; then
		grep -n 'error:' /tmp/idpf-user-build.log | head -$LINES
		echo '--- distinct error kinds ---'
		grep -o 'error: .*' /tmp/idpf-user-build.log |
		    sed 's/[0-9][0-9]*/N/g' | sort -u | head -25
		exit 1
	fi
	if [ "$RUN_TESTS" = 1 ]; then
		echo '=== run ==='
		./test_ctlq
	else
		echo '=== run skipped ==='
	fi
REMOTE_EOF
