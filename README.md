idpf Linux* Base Driver Readme for Infrastructure Data-Plane Function
****************************************************************************

1. Userspace tests — safe, run these

Builds idpf_controlq.c + idpf_controlq_setup.c unmodified against the mock in mock and runs them as a normal process. 13 cases, 64 checks. No kernel, no src needed, and a failure is just a non-zero exit. Currently passes on both hosts.

Locally on the FreeBSD box you can also just:


2. Driver build

Compiles the kmod only. Build it on the host you intend to load on — a module built against 15.0 sources isn't guaranteed to load on a 15.1 kernel.

3. Module load/unload stress

62 kldload/kldunload cycles across nine delay tiers plus negative paths. Requires fbsd-build.sh to have run first. This only exercises module scope — with no IDPF device present the probe never matches, so it does not test attach/detach.

4. In-kernel test module — builds only, does not load

Stops at the .ko deliberately. Loading is a separate manual step, and only on freebsd (freebsd01 has no src):


Each case prints -> suite.case before it runs, so if the box wedges, the last console line names the culprit.

I have not run this since fixing the crash. It's the harness that panicked freebsd earlier — my fixture called idpf_vf_dev_ops_init(), which reaches pci_get_device() on a fabricated non-PCI device. That's fixed and it compiles clean, but it's unverified at runtime. Both hosts now have debug.debugger_on_panic=0 and a 10s reboot wait, so a panic reboots rather than parking at db> — but it's still a reboot. I'd want your go-ahead before running it, or I can run it if you'd rather I just proved it out.

Quick reference
Script	Risk	Needs src	Covers
fbsd-user-test.sh	none	no	control queue + simulated control plane
fbsd-build.sh	none	yes	compiles
fbsd-kld-test.sh	low	yes	module load/unload, not attach
fbsd-unit-build.sh	none to build, reboot risk to load	yes	taskqueues, PF device ops, ctlq in kernel context


# Rozpakowanie archiwum
mkdir -p /opt/idpf-ported
mv idpf.tgz /opt/idpf-ported/idpf.tgz
cd /opt/idpf-ported/
tar -xzf idpf.tgz && rm idpf.tgz


# Docelowy host freebsd dostepny z maszyny z linuxem:
export FBSD_HOST=root@10.102.18.118

./scripts/fbsd-build.sh
./scripts/fbsd-unit-build.sh
./scripts/fbsd-kld-test.sh
./scripts/fbsd-user-test.sh