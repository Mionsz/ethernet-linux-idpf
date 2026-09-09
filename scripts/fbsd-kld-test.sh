#!/bin/sh
# Module lifecycle stress test for if_idpf on the FreeBSD host.
#
# This exercises module load/unload only.  With no IDPF device present the
# probe never matches, so ifdi_attach_pre() and friends are NOT covered here;
# see scripts/README-testing.md for the layers that do cover them.
#
# What it does catch: unresolved symbols, SYSINIT/SYSUNINIT ordering,
# MALLOC_DEFINE and DRIVER_MODULE re-registration across reload, taskqueue or
# sysctl state left behind at module scope, and races between an unload and a
# load that has not fully settled.
#
# Usage: fbsd-kld-test.sh [path-to-remote-build-dir]
set -e

HOST=${FBSD_HOST:-10.102.18.118}
REMOTE_DIR=${1:-${IDPF_REMOTE_DIR:-/tmp/idpfbuild}}
REMOTE_SCRIPT=/tmp/idpf-kld-test.sh

case "${IDPF_ALLOW_HARDWARE:-}" in
1|yes|true) ;;
*)
	echo "RESULT: SKIP - set IDPF_ALLOW_HARDWARE=1 before loading a module"
	exit 2
	;;
esac
case "${IDPF_CONSOLE_CONFIRMED:-}" in
1|yes|true) ;;
*)
	echo "RESULT: SKIP - set IDPF_CONSOLE_CONFIRMED=1 after verifying console access"
	exit 2
	;;
esac

cat > /tmp/idpf-kld-test.sh.local <<'REMOTE_EOF'
#!/bin/sh
# Runs on the FreeBSD host.  Deliberately not `set -e`: a failed iteration must
# not abort the matrix, or one stuck load hides every later tier.
KO=./if_idpf.ko
MOD=if_idpf

is_loaded() {
	kldstat -v 2>/dev/null | grep -qw "$MOD" && return 0
	kldstat 2>/dev/null | awk '{print $NF}' | grep -qx "${MOD}.ko"
}

force_cleanup() {
	is_loaded && kldunload -f "$MOD" >/dev/null 2>&1
	return 0
}

delay() {
	[ "$1" = "0" ] && return 0
	sleep "$1"
}

total=0; total_fail=0
printf '%-8s %6s %6s %6s %6s %6s\n' TIER ITERS PASS LOADF VERF UNLDF
printf -- '------------------------------------------------\n'

run_tier() {
	tier=$1; iters=$2
	pass=0; loadf=0; verf=0; unldf=0

	i=0
	while [ $i -lt $iters ]; do
		i=$((i + 1))
		total=$((total + 1))

		if ! kldload "$KO" >/dev/null 2>&1; then
			loadf=$((loadf + 1)); force_cleanup; delay "$tier"; continue
		fi
		delay "$tier"

		if ! is_loaded; then
			verf=$((verf + 1)); force_cleanup; delay "$tier"; continue
		fi
		delay "$tier"

		if ! kldunload "$MOD" >/dev/null 2>&1; then
			unldf=$((unldf + 1)); force_cleanup; delay "$tier"; continue
		fi

		pass=$((pass + 1))
		force_cleanup
		delay "$tier"
	done

	fails=$((loadf + verf + unldf))
	total_fail=$((total_fail + fails))
	printf '%-8s %6d %6d %6d %6d %6d\n' "$tier" "$iters" "$pass" \
	    "$loadf" "$verf" "$unldf"
}

force_cleanup
for t in 0 0.001 0.02 0.05 0.1 0.2 0.5; do run_tier "$t" 8; done
for t in 1 3; do run_tier "$t" 3; done

printf -- '------------------------------------------------\n'
printf 'cycles=%d failures=%d\n\n' "$total" "$total_fail"

# --- negative paths -------------------------------------------------------
neg_fail=0
echo "=== negative paths ==="

kldunload "$MOD" >/dev/null 2>&1 &&
    { echo "FAIL: unload of a not-loaded module succeeded"; neg_fail=1; } ||
    echo "ok: unload of not-loaded module rejected"

if kldload "$KO" >/dev/null 2>&1; then
	kldload "$KO" >/dev/null 2>&1 &&
	    { echo "FAIL: double load succeeded"; neg_fail=1; } ||
	    echo "ok: double load rejected"
	kldunload "$MOD" >/dev/null 2>&1
else
	echo "FAIL: could not load for double-load check"; neg_fail=1
fi

# M_IDPF is created by MALLOC_DEFINE at module load; nothing should survive
# unload.  Weak while probe never matches, but free to check.
force_cleanup
if vmstat -m 2>/dev/null | awk '{print $1}' | grep -qx idpf; then
	echo "FAIL: M_IDPF malloc type still present after unload"
	vmstat -m | grep -w idpf
	neg_fail=1
else
	echo "ok: no M_IDPF malloc type after unload"
fi

echo
if [ "$total_fail" -eq 0 ] && [ "$neg_fail" -eq 0 ]; then
	echo "KLD LIFECYCLE: PASS ($total cycles)"
	exit 0
fi
echo "KLD LIFECYCLE: FAIL (cycle failures=$total_fail negative=$neg_fail)"
exit 1
REMOTE_EOF

if [ "$HOST" = local ]; then
	cp /tmp/idpf-kld-test.sh.local "$REMOTE_SCRIPT"
	chmod +x "$REMOTE_SCRIPT"
	cd "$REMOTE_DIR/src"
	exec "$REMOTE_SCRIPT"
fi

scp -q -o BatchMode=yes /tmp/idpf-kld-test.sh.local "$HOST:$REMOTE_SCRIPT"
ssh -o BatchMode=yes "$HOST" "chmod +x $REMOTE_SCRIPT && cd $REMOTE_DIR/src && $REMOTE_SCRIPT"
