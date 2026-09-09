#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build and validate the FreeBSD IDPF port locally or through a FreeBSD host.
# Hardware-attaching validation is always opt-in: add --hardware and confirm
# console access with --console-confirmed (or IDPF_CONSOLE_CONFIRMED=1).

set -u

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
log_dir=${IDPF_LOG_DIR:-"$repo_root/.quick-start"}
host=${FBSD_HOST:-10.102.18.118}
remote_dir=${IDPF_REMOTE_DIR:-/tmp/idpfbuild}
interface=${IDPF_IFACE:-idpf0}
peer=${TESTPEER:-192.168.211.99}
install_dir=${IDPF_INSTALL_DIR:-/boot/modules}
console_confirmed=${IDPF_CONSOLE_CONFIRMED:-0}
want_build=0
want_install=0
want_test=0
want_run=0
want_hardware=0

usage() {
	cat <<EOF
Usage: ./quick_start.sh [options]

  --install              Build and copy if_idpf.ko to $install_dir; never load it.
  --test                 Build the inert kernel tests and safe userspace tests.
  --run                  Build and run safe userspace tests.
  --all                  Build, install, build tests, and run safe userspace tests.
  --hardware             Run hardware-facing checks after --console-confirmed.
  --console-confirmed    Acknowledge that a usable serial/IPMI console is available.
  --host HOST            FreeBSD SSH target when invoked outside FreeBSD.
  --interface IFACE      Driver interface for hardware checks (default: $interface).
  --peer ADDRESS         Test peer for hardware traffic checks (default: $peer).
  --help                 Show this help.

Environment: FBSD_HOST, IDPF_REMOTE_DIR, IDPF_IFACE, TESTPEER, TESTIP,
IDPF_INSTALL_DIR, IDPF_LOG_DIR, IDPF_CONSOLE_CONFIRMED, PCI_BUS, and PCI_MATCH.
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--install) want_build=1; want_install=1 ;;
	--test) want_test=1 ;;
	--run) want_test=1; want_run=1 ;;
	--all) want_build=1; want_install=1; want_test=1; want_run=1 ;;
	--hardware) want_hardware=1 ;;
	--console-confirmed) console_confirmed=1 ;;
	--host) shift; [[ $# -gt 0 ]] || { usage >&2; exit 1; }; host=$1 ;;
	--interface) shift; [[ $# -gt 0 ]] || { usage >&2; exit 1; }; interface=$1 ;;
	--peer) shift; [[ $# -gt 0 ]] || { usage >&2; exit 1; }; peer=$1 ;;
	-h|--help) usage; exit 0 ;;
	*) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 1 ;;
	esac
	shift
done

if (( ! want_build && ! want_test && ! want_hardware )); then
	want_build=1
fi

if (( want_hardware )) && [[ $console_confirmed != 1 && $console_confirmed != yes && $console_confirmed != true ]]; then
	if (( ! want_build && ! want_install && ! want_test && ! want_run )); then
		echo "[SKIP] hardware-suite: pass --console-confirmed after verifying serial/IPMI console access"
		exit 0
	fi
fi
if (( want_hardware )); then
	want_build=1
fi

mkdir -p "$log_dir"
stage_names=()
stage_states=()
stage_details=()
failures=0

record_stage() {
	stage_names+=("$1")
	stage_states+=("$2")
	stage_details+=("$3")
	printf '[%s] %s: %s\n' "$2" "$1" "$3"
}

run_stage() {
	local name=$1
	shift
	local logfile="$log_dir/${#stage_names[@]}-${name//[^A-Za-z0-9_.-]/_}.log"
	local rc

	printf '\n===== %s =====\n' "$name"
	"$@" > >(tee "$logfile") 2>&1
	rc=$?
	case "$rc" in
	0) record_stage "$name" PASS "log: $logfile" ;;
	2) record_stage "$name" SKIP "precondition not met; log: $logfile" ;;
	*) record_stage "$name" FAIL "exit $rc; log: $logfile"; failures=$((failures + 1)) ;;
	esac
}

on_freebsd=0
[[ $(uname -s) == FreeBSD ]] && on_freebsd=1

ensure_package() {
	local package=$1
	pkg info -e "$package" >/dev/null 2>&1 && return 0
	[[ $(id -u) -eq 0 ]] || {
		echo "missing package $package and package installation requires root" >&2
		return 1
	}
	timeout "${IDPF_PKG_TIMEOUT:-120}" pkg install -y "$package"
}

local_dependencies() {
	local command
	[[ $(uname -s) == FreeBSD ]] || return 2
	for command in make cc pkg kldxref; do
		command -v "$command" >/dev/null 2>&1 || {
			echo "missing required FreeBSD command: $command" >&2
			return 1
		}
	done
	ensure_package python3
	ensure_package cpputest
	ensure_package llvm19
}

remote_dependencies() {
	ssh -o BatchMode=yes -o ConnectTimeout=30 "$host" 'sh -s' <<'REMOTE_EOF'
set -eu
for command in make cc pkg kldxref; do
	command -v "$command" >/dev/null 2>&1 || {
		echo "missing required FreeBSD command: $command" >&2
		exit 1
	}
done
for package in python3 cpputest llvm19; do
	pkg info -e "$package" >/dev/null 2>&1 || timeout "${IDPF_PKG_TIMEOUT:-120}" pkg install -y "$package"
done
REMOTE_EOF
}

local_build() {
	make -C "$repo_root/idpf/src" -j4
}

local_install() {
	local module="$repo_root/idpf/src/if_idpf.ko"
	[[ -f $module ]] || { echo "module not built: $module" >&2; return 1; }
	install -d "$install_dir"
	install -m 0644 "$module" "$install_dir/if_idpf.ko"
	kldxref "$install_dir"
}

remote_install() {
	ssh -o BatchMode=yes -o ConnectTimeout=30 "$host" \
		"install -d '$install_dir' && install -m 0644 '$remote_dir/idpf/src/if_idpf.ko' '$install_dir/if_idpf.ko' && kldxref '$install_dir'"
}

local_test_build() {
	make -C "$repo_root/idpf/test" -j4
	make -C "$repo_root/idpf/test/user"
}

local_test_run() {
	make -C "$repo_root/idpf/test/user" run
}

prepare_submodules() {
	cd "$repo_root" || return 1
	if ! git submodule update --init --recursive; then
		echo "WARNING: submodule update was incomplete; preserving existing submodule worktrees" >&2
		git submodule status --recursive >&2 || true
	fi
}

stage_remote_hardware_scripts() {
	local archive=/tmp/idpf-quick-scripts.tgz
	tar -C "$repo_root" -czf "$archive" scripts
	scp -q -o BatchMode=yes -o ConnectTimeout=30 "$archive" "$host:/tmp/"
	ssh -o BatchMode=yes -o ConnectTimeout=30 "$host" \
		"rm -rf '$remote_dir/scripts' && tar -xzf /tmp/idpf-quick-scripts.tgz -C '$remote_dir' && chmod +x '$remote_dir/scripts/'*.sh '$remote_dir/scripts/set_irq_affinity'"
	rm -f "$archive"
}

run_local_hardware() {
	local module="$repo_root/idpf/src/if_idpf.ko"
	local script
	for script in hw-cleanup.sh hw-attach-test.sh idpf-validate.sh idpf-datapath.sh idpf-harden.sh idpf-ptp-validate.sh set_irq_affinity; do
		case "$script" in
		hw-cleanup.sh) run_stage "$script" "$repo_root/scripts/$script" "$interface" ;;
		hw-attach-test.sh) run_stage "$script" env KMOD="$module" IDPF_IFACE="$interface" IDPF_ALLOW_HARDWARE=1 IDPF_CONSOLE_CONFIRMED=1 "$repo_root/scripts/$script" ;;
		idpf-validate.sh|idpf-datapath.sh|idpf-harden.sh|idpf-ptp-validate.sh)
			run_stage "$script" env KMOD="$module" IDPF_ALLOW_HARDWARE=1 TESTPEER="$peer" PEER="$peer" "$repo_root/scripts/$script" "$interface" "$module"
			;;
		set_irq_affinity) run_stage "$script" "$repo_root/scripts/$script" -s "$interface" ;;
		esac
	done
}

run_remote_hardware() {
	local script
	stage_remote_hardware_scripts || return 1
	for script in hw-cleanup.sh hw-attach-test.sh idpf-validate.sh idpf-datapath.sh idpf-harden.sh idpf-ptp-validate.sh set_irq_affinity; do
		if [[ $script == set_irq_affinity ]]; then
			run_stage "$script" ssh -o BatchMode=yes -o ConnectTimeout=30 "$host" \
				"'$remote_dir/scripts/$script' -s '$interface'"
			continue
		fi
		if [[ $script == hw-attach-test.sh ]]; then
			run_stage "$script" ssh -o BatchMode=yes -o ConnectTimeout=30 "$host" \
				"KMOD='$remote_dir/idpf/src/if_idpf.ko' IDPF_IFACE='$interface' IDPF_ALLOW_HARDWARE=1 IDPF_CONSOLE_CONFIRMED=1 '$remote_dir/scripts/$script'"
			continue
		fi
		run_stage "$script" ssh -o BatchMode=yes -o ConnectTimeout=30 "$host" \
			"PATH='$remote_dir/scripts':\$PATH KMOD='$remote_dir/idpf/src/if_idpf.ko' IDPF_IFACE='$interface' IDPF_ALLOW_HARDWARE=1 IDPF_CONSOLE_CONFIRMED=1 TESTPEER='$peer' PEER='$peer' '$remote_dir/scripts/$script' '$interface' '$remote_dir/idpf/src/if_idpf.ko'"
	done
}

if (( on_freebsd )); then
	run_stage dependencies local_dependencies
	if (( want_build )); then run_stage driver-build local_build; fi
	if (( want_install )); then run_stage driver-install local_install; fi
	if (( want_test )); then run_stage test-build local_test_build; fi
	if (( want_run )); then run_stage userspace-tests local_test_run; fi
	if (( want_hardware )); then
		if [[ $console_confirmed == 1 || $console_confirmed == yes || $console_confirmed == true ]]; then
			run_local_hardware
		else
			record_stage hardware-suite SKIP "pass --console-confirmed after verifying serial/IPMI console access"
		fi
	fi
else
	if (( want_build || want_test || want_run || want_hardware )); then
		run_stage submodule-prepare prepare_submodules
	fi
	run_stage dependencies remote_dependencies
	if (( want_build )); then run_stage driver-build env FBSD_HOST="$host" IDPF_REMOTE_DIR="$remote_dir" "$repo_root/scripts/fbsd-build.sh"; fi
	if (( want_install )); then run_stage driver-install remote_install; fi
	if (( want_test )); then
		run_stage kernel-test-build env FBSD_HOST="$host" IDPF_REMOTE_DIR="${IDPF_TEST_REMOTE_DIR:-/tmp/idpftest}" "$repo_root/scripts/fbsd-unit-build.sh"
		run_stage userspace-test-build env FBSD_HOST="$host" IDPF_REMOTE_DIR="${IDPF_USER_REMOTE_DIR:-/tmp/idpfuser}" RUN_TESTS=0 "$repo_root/scripts/fbsd-user-test.sh"
	fi
	if (( want_run )); then run_stage userspace-tests env FBSD_HOST="$host" IDPF_REMOTE_DIR="${IDPF_USER_REMOTE_DIR:-/tmp/idpfuser}" RUN_TESTS=1 "$repo_root/scripts/fbsd-user-test.sh"; fi
	if (( want_hardware )); then
		if [[ $console_confirmed == 1 || $console_confirmed == yes || $console_confirmed == true ]]; then
			run_remote_hardware
		else
			record_stage hardware-suite SKIP "pass --console-confirmed after verifying serial/IPMI console access"
		fi
	fi
fi

printf '\n===== IDPF QUICK START SUMMARY =====\n'
printf '%-24s %-6s %s\n' STAGE STATUS DETAIL
for index in "${!stage_names[@]}"; do
	printf '%-24s %-6s %s\n' "${stage_names[index]}" "${stage_states[index]}" "${stage_details[index]}"
done
printf 'logs: %s\n' "$log_dir"
if (( failures == 0 )); then
	echo "RESULT: PASS"
	exit 0
fi
echo "RESULT: FAIL ($failures stage(s) failed)"
exit 1
