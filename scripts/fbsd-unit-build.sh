#!/bin/sh
# Build the in-kernel idpf test module. Does NOT load it.
#
# Loading is deliberately a separate, manual step: the module runs driver code
# that has never executed before, and a hang in kernel context takes the whole
# machine with it. Load it only on a box you can reach the console of:
#
#   kldload ./if_idpf_test.ko          # inert, runs nothing
#   sysctl hw.idpf_test.run=0          # run one suite at a time
#   sysctl hw.idpf_test.run=1
#   sysctl hw.idpf_test.run=-1         # or everything
#
# Each case prints "-> suite.case" before it runs, so if the box wedges the
# last line on the console names the case that did it.
#
# Usage: [FBSD_HOST=user@host] fbsd-unit-build.sh [error-lines-to-show]
set -e

HOST=${FBSD_HOST:-freebsd}
LINES=${1:-40}
LOCAL_SRC=/opt/ethernet-linux-idpf/idpf
REMOTE_DIR=/tmp/idpftest

cd "$LOCAL_SRC"
tar -czf /tmp/idpf-test.tgz src test
scp -q -o BatchMode=yes /tmp/idpf-test.tgz "$HOST:/tmp/"

ssh -o BatchMode=yes "$HOST" "
	rm -rf $REMOTE_DIR && mkdir -p $REMOTE_DIR &&
	tar -xzf /tmp/idpf-test.tgz -C $REMOTE_DIR &&
	cd $REMOTE_DIR/test &&
	make -j4 > /tmp/idpf-test-build.log 2>&1
	rc=\$?
	echo \"=== build exit \$rc ===\"
	if [ \$rc -eq 0 ]; then
		ls -l if_idpf_test.ko
		echo 'NOT loaded. To run:  cd $REMOTE_DIR/test && kldload ./if_idpf_test.ko'
	else
		grep -n 'error:' /tmp/idpf-test-build.log | head -$LINES
		echo '--- distinct error kinds ---'
		grep -o 'error: .*' /tmp/idpf-test-build.log |
		    sed 's/[0-9][0-9]*/N/g' | sort -u | head -25
		exit 1
	fi
"
