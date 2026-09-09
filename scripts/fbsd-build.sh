#!/bin/sh
# Sync the IDPF source and shared dependency tree to a FreeBSD host and build
# the kmod there.
# Usage: [FBSD_HOST=user@host] fbsd-build.sh [number-of-error-lines-to-show]
#
# Build on the host you intend to load on: a module built against 15.0 sources
# is not guaranteed to load on a 15.1 kernel.
set -e

HOST=${FBSD_HOST:-10.102.18.118}
LINES=${1:-60}
LOCAL_SRC=/opt/ethernet-linux-idpf/idpf
REMOTE_DIR=${IDPF_REMOTE_DIR:-/tmp/idpfbuild}

cd "$LOCAL_SRC"
tar -czf /tmp/idpf-src.tgz src shared
scp -q -o BatchMode=yes /tmp/idpf-src.tgz "$HOST:/tmp/"

ssh -o BatchMode=yes "$HOST" "
	rm -rf $REMOTE_DIR && mkdir -p $REMOTE_DIR &&
	tar -xzf /tmp/idpf-src.tgz -C $REMOTE_DIR &&
	cd $REMOTE_DIR/src &&
	make -j4 > /tmp/idpf-build.log 2>&1
	rc=\$?
	echo \"=== exit \$rc ===\"
	if [ \$rc -eq 0 ]; then
		ls -l if_idpf.ko
	else
		grep -n 'error:\|warning:' /tmp/idpf-build.log | head -$LINES
		echo '--- distinct error kinds ---'
		grep -o 'error: .*' /tmp/idpf-build.log | sed 's/[0-9]\+/N/g' |
		    sort -u | head -25
	fi
	exit \$rc
"
