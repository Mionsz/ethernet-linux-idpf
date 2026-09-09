# Intel IDPF FreeBSD Port

This repository contains the FreeBSD kernel-module port of Intel's
Infrastructure Data-Plane Function (IDPF) driver. Use `quick_start.sh` for
normal builds and tests; it writes a per-stage `PASS`, `FAIL`, or `SKIP`
summary and stores logs in `.quick-start/`.

## Quick Start

Run these commands from the repository root.

### Linux or another non-FreeBSD host

Set the FreeBSD build target, then run the desired stage. The runner transfers
the required source and runs the FreeBSD build remotely over SSH. On Windows,
use WSL or Git Bash.

```sh
export FBSD_HOST=root@10.102.18.118

./quick_start.sh                 # Build if_idpf.ko remotely
./quick_start.sh --test          # Build kernel and userspace test binaries
./quick_start.sh --run           # Build and run safe userspace tests
./quick_start.sh --all           # Build, install, build tests, and run tests
```

### FreeBSD host

Run the same commands directly on the FreeBSD system. The runner verifies the
build environment and installs missing `python3`, `cpputest`, and `llvm19`
packages when run as root.

```sh
cd /path/to/ethernet-linux-idpf

./quick_start.sh                 # Build only
./quick_start.sh --test          # Build tests only
./quick_start.sh --run           # Build and run safe userspace tests
./quick_start.sh --all           # Build, install, build tests, and run tests
```

`--install` copies `if_idpf.ko` to `/boot/modules/if_idpf.ko` and runs
`kldxref`; it never loads the module. Set `IDPF_INSTALL_DIR` to install
somewhere else.

## Test Levels

| Command | What it does | Hardware risk |
| --- | --- | --- |
| `./quick_start.sh` | Builds the FreeBSD kernel module | None |
| `./quick_start.sh --test` | Builds the inert kernel test module and userspace tests | None |
| `./quick_start.sh --run` | Runs control-queue tests using a simulated control plane | None |
| `./quick_start.sh --all` | Builds, installs, builds tests, and runs safe userspace tests | None |

The userspace suite currently runs 20 cases and 136 checks. It does not load
a kernel module or touch network hardware.

## Hardware Validation

Hardware validation can load the module, attach a physical device, change
interface settings, and transmit traffic. It is never included in `--all`.

Before running it, confirm that a usable serial or IPMI console is open and
that the target is safe to recover. Then run:

```sh
./quick_start.sh --hardware --console-confirmed \
  --interface idpf0 --peer 192.168.211.99
```

This runs module lifecycle, attach, validation, datapath, hardening, PTP, and
Linux IRQ-affinity checks. `set_irq_affinity` is Linux-only and is reported as
`SKIP` on FreeBSD.

## Configuration

| Variable or option | Purpose | Default |
| --- | --- | --- |
| `FBSD_HOST` or `--host` | Remote FreeBSD SSH target | `10.102.18.118` |
| `IDPF_REMOTE_DIR` | Remote build directory | `/tmp/idpfbuild` |
| `IDPF_IFACE` or `--interface` | IDPF interface for hardware checks | `idpf0` |
| `TESTPEER` or `--peer` | Peer address for traffic tests | `192.168.211.99` |
| `TESTIP` | Address assigned to the IDPF interface | `192.168.211.1/24` |
| `IDPF_LOG_DIR` | Directory for `quick_start.sh` logs | `.quick-start/` |
| `IDPF_PKG_TIMEOUT` | Package-install timeout in seconds | `120` |

Run `./quick_start.sh --help` for the full option list.
