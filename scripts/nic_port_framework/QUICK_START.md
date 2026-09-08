# Quick Start

Full run of the NIC Port Framework (Linux IDPF -> FreeBSD baseline).
All paths below assume the framework lives in `/opt/ethernet-linux-idpf/scripts/nic_port_framework`.

---

## 1. Prerequisites

```bash
apt update
apt install -y build-essential python3 git \
  clang-22 clang-tools-22 libclang-22-dev libclang-cpp22 libclang-cpp22-dev llvm-22-dev
apt install -y linux-headers-$(uname -r)
```

Verify:

```bash
command -v clang++-22 python3 make
ls /usr/lib/llvm-22/include /usr/lib/llvm-22/lib
ls /usr/src/linux-headers-$(uname -r)/scripts/clang-tools/gen_compile_commands.py
```

---

## 2. Environment

```bash
export FRAMEWORK_DIR="/opt/ethernet-linux-idpf/scripts/nic_port_framework"
export FRAMEWORK_REPO_DIR="/opt/ethernet-linux-idpf"
export FRAMEWORK_REPO_SRC_DIR="${FRAMEWORK_REPO_DIR}/idpf/src"
export FRAMEWORK_OUT_DIR="${FRAMEWORK_REPO_DIR}/.out"
export KERNEL_BUILD_DIR="/usr/src/linux-headers-$(uname -r)"

export MANIFEST="${FRAMEWORK_DIR}/examples/idpf.full.manifest.json"
export SPEC_MANIFEST="${FRAMEWORK_DIR}/templates/orchestrator/records/spec_manifest.example.json"
export ANALYSIS_ROOT="${FRAMEWORK_OUT_DIR}/idpf-baseline"

cd "${FRAMEWORK_DIR}"
```

These variables are referenced by the manifests. They must be exported before any command.

---

## 3. One command

```bash
cd "${FRAMEWORK_DIR}"

# Preparation only (build + extract + orchestrate init + specs + status). No LLM calls.
./run_framework.sh

# Preparation + LLM analysis stages.
./run_framework.sh --model gpt-4.1 --device-flow --jobs 8
```

Per-stage logs: `.logs/<run-id>-<stage>.log`. Combined log: `.logs/run-<run-id>.log`.
Every run also writes `.logs/<run-id>-summary.md` (copied to `${ANALYSIS_ROOT}/reports/run_summary.md`)
with what ran, what broke, what never ran and the exact command to continue. It is written on an
aborted or interrupted run too.

Selected stages only:

```bash
./run_framework.sh --stages status
./run_framework.sh --stages extract,headers,scope
./run_framework.sh --stages all --model gpt-4.1
```

If another process holds the workspace lock the run says so, names the holder, and waits:

```text
Workspace is locked by another process: .../orchestration/.lock
  holder PID 394233: python3 nic_port_orchestrator.py ... init
Waiting 60 minutes for workspace lock release... [To override the lock, type 'yes' and press enter]
```

Defaults come from `config/policy/default_policy.json` -> `workspace_lock`
(`wait_timeout_seconds: 3600`, `on_timeout: override`). Override per run with
`--lock-timeout SECONDS` and `--lock-on-timeout exit|override`.

---

## 4. Manual step by step

Every command below is exactly what `run_framework.sh` runs.

### 4.1 Build the LibTooling extractor

```bash
mkdir -p "${FRAMEWORK_OUT_DIR}"
clang++-22 -std=c++17 -O2 "${FRAMEWORK_DIR}/nic_port_clang_extractor.cpp" \
  -I/usr/lib/llvm-22/include \
  -D_GNU_SOURCE -D_GLIBCXX_USE_CXX11_ABI=1 \
  -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
  -fno-exceptions \
  -L/usr/lib/llvm-22/lib -lLLVM-22 -lclang-cpp \
  -o "${FRAMEWORK_OUT_DIR}/nic-port-clang-extractor"
```

### 4.2 Build the driver and generate the compilation database

```bash
make -C "${FRAMEWORK_REPO_DIR}"
python3 "${KERNEL_BUILD_DIR}/scripts/clang-tools/gen_compile_commands.py" \
  -d "${FRAMEWORK_REPO_DIR}" \
  -o "${FRAMEWORK_OUT_DIR}/compile_commands.json"
```

`-d` must point at the built module tree (`${FRAMEWORK_REPO_DIR}`), not at the kernel headers.

### 4.3 Validate the manifest

```bash
python3 nic_port_pipeline.py --manifest "${MANIFEST}" validate-manifest
```

### 4.4 Extract source knowledge

```bash
python3 nic_port_pipeline.py --manifest "${MANIFEST}" build
python3 nic_port_pipeline.py --manifest "${MANIFEST}" diagnose-extraction
```

Required before continuing: `run_state = COMPLETE` and `extraction_quality = PASS`.

### 4.5 Scan headers and apply the OSAL policy

```bash
# Declarations that no translation unit compiled (structs, includes, macros).
python3 nic_port_headers.py --manifest "${MANIFEST}" scan

# OSAL surface. Default mode is 'optional': context only, sources untouched.
python3 nic_port_osal.py --manifest "${MANIFEST}" index
python3 nic_port_osal.py --manifest "${MANIFEST}" plan

# Mandatory mode rewrites a staged COPY of the sources onto the OSAL API and
# retargets the compile database at it. The repository is never modified.
python3 nic_port_osal.py --manifest "${MANIFEST}" --mode mandatory stage
```

Mode and symbol map live in `config/rules/osal_rules.json`. A mapping is applied only when the
OSAL macro takes the same number of arguments as the Linux original; the rest are reported for
manual work in `reports/osal_summary.md`.

### 4.6 Scope the port to the destination OS

```bash
python3 nic_port_target.py --manifest "${MANIFEST}" --root "${ANALYSIS_ROOT}" scope
```

This runs before any per-method analysis and does two things:

1. Removes source-OS-only methods from the porting loop (kcompat shims, devlink, auxiliary-bus
   IDC, XDP/AF_XDP, VFIO mdev, ethtool). Nothing is deleted: each exclusion records the rule,
   the rationale and a disposition of `drop`, `replace` or `defer`.
2. Plans the work items the destination OS mandates and that have no source counterpart
   (module glue, `ifdi_*` methods, `isc_txd_*` / `isc_rxd_*` datapath, MSI-X assignment,
   ifmedia, busdma, sysctl tree, build and validation).

The active profile is `config/targets/freebsd_iflib.json` (`configuration.target_profile`).
Result for the IDPF baseline: 625 methods extracted, 480 retained, 145 excluded, 32 target work
items across 11 workflows. Review `reports/target_scope.md` before spending model budget.

### 4.7 Initialize orchestration

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" init --root "${ANALYSIS_ROOT}"
```

### 4.8 Ingest specifications, authorized MCP tools and skills

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
  ingest-spec --root "${ANALYSIS_ROOT}" --spec-manifest "${SPEC_MANIFEST}"

python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
  capabilities --root "${ANALYSIS_ROOT}"
```

Ingest specifications before running any analysis stage. Ingesting or changing them later
changes the specification fingerprint and marks already accepted results `STALE`, which
means they have to be executed again.

### 4.9 Status

```bash
python3 nic_port_pipeline.py --manifest "${MANIFEST}" status --verbose
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" next
```

---

## 5. LLM provider

```bash
# One-time interactive login; the token is cached in ~/.cache/nic_porting/llm_tokens.json
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" llm-auth --login --llm-device-flow

# Pick a model id
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" list-models

# Check cached credential state
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" llm-auth
```

Alternative to device flow: `export GITHUB_TOKEN=<pat-with-copilot-scope>`.

---

## 6. Analysis stages

Run in this order. Each stage is `render` (build prompts) then `run` (execute them).
The `target` stage comes before `methods`: the destination-OS obligations are designed first,
so the per-method loop can refer to them instead of inventing a target design per function.

```bash
for STAGE in architecture target methods evidence files subsystems final; do
  python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
    render --root "${ANALYSIS_ROOT}" --stage "${STAGE}"

  python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
    run --root "${ANALYSIS_ROOT}" --stage "${STAGE}" \
    --llm-model gpt-4.1 --jobs 8
done
```

Single item:

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
  run --root "${ANALYSIS_ROOT}" --stage files --item idpf_controlq.c --llm-model gpt-4.1
```

External runner instead of the built-in provider:

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
  run --root "${ANALYSIS_ROOT}" --stage architecture \
  --runner 'my-llm-cli --input {prompt}'
```

Ingest a result produced elsewhere:

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
  ingest --root "${ANALYSIS_ROOT}" --stage architecture --item architecture \
  --result ./architecture-result.json
```

### Stage gates

| Stage | Unlocked when |
|---|---|
| architecture | extraction COMPLETE + quality PASS |
| target | architecture VALID at confidence >= C3 + target work items planned (`scope`) |
| methods | architecture VALID at >= C3 + 100% target work items VALID at >= C2 |
| evidence | 100% methods VALID at >= C2 |
| files | 100% methods at >= C3 + 100% critical evidence resolved |
| subsystems | 100% files VALID at >= C3 |
| final | architecture >= C4 + 100% subsystems >= C3 + 100% critical evidence |

Gate reasons:

```bash
python3 -c "import json;print(json.load(open('${ANALYSIS_ROOT}/orchestration/reports/status.json'))['gates'])"
```

Bypass a gate (result is still validated and can still go STALE):

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" \
  render --root "${ANALYSIS_ROOT}" --stage methods --force
```

---

## 7. Control plane and audit

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" verify-evidence --root "${ANALYSIS_ROOT}"
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" registries    --root "${ANALYSIS_ROOT}" --json
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" traceability  --root "${ANALYSIS_ROOT}" --json
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" consistency   --root "${ANALYSIS_ROOT}" --json
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" audit         --root "${ANALYSIS_ROOT}"
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" validate      --root "${ANALYSIS_ROOT}"
```

---

## 8. Recovery

Extraction failed:

```bash
python3 nic_port_pipeline.py --manifest "${MANIFEST}" diagnose-extraction
# fix the reported translation unit / environment problem, then:
python3 nic_port_pipeline.py --manifest "${MANIFEST}" resume
```

`resume` skips bootstrap and reuses cached successful translation units.

Source changed since the last analysis:

```bash
python3 nic_port_orchestrator.py --manifest "${MANIFEST}" delta \
  --old "${FRAMEWORK_OUT_DIR}/idpf-baseline" \
  --new "${FRAMEWORK_OUT_DIR}/idpf-next" \
  --out "${FRAMEWORK_OUT_DIR}/delta.json" --carry-forward
```

Start over from a clean output tree:

```bash
rm -rf "${ANALYSIS_ROOT}"
./run_framework.sh
```

---

## 9. Outputs

```text
${FRAMEWORK_OUT_DIR}/compile_commands.json          authoritative build database
${FRAMEWORK_OUT_DIR}/nic-port-clang-extractor       LibTooling extractor binary
${FRAMEWORK_OUT_DIR}/osal_staged_source/            OSAL-rewritten sources (mandatory mode only)
${ANALYSIS_ROOT}/output_manifest.json               start here: index of everything produced
${ANALYSIS_ROOT}/manifest/run_state.json            extraction transaction state
${ANALYSIS_ROOT}/manifest/extraction_quality.json   extraction quality gate
${ANALYSIS_ROOT}/manifest/header_coverage.json      headers not covered by any translation unit
${ANALYSIS_ROOT}/manifest/osal_plan.json            OSAL call sites and manual-review list
${ANALYSIS_ROOT}/kb/functions.jsonl                 compiler-derived source knowledge
${ANALYSIS_ROOT}/kb/headers.jsonl                   header declarations and include obligations
${ANALYSIS_ROOT}/kb/osal_index.json                 OSAL API surface and mapping status
${ANALYSIS_ROOT}/indexes/*.csv                      portability/API/include/header indexes
${ANALYSIS_ROOT}/reports/portability_summary.md     human summary
${ANALYSIS_ROOT}/reports/header_summary.md          header inventory
${ANALYSIS_ROOT}/reports/osal_summary.md            OSAL policy and substitution plan
${ANALYSIS_ROOT}/reports/target_scope.md            excluded source methods + target work items
${ANALYSIS_ROOT}/reports/run_summary.md             last run: what ran, what broke, how to continue
${ANALYSIS_ROOT}/orchestration/target/exclusions.json   methods removed from the porting loop
${ANALYSIS_ROOT}/orchestration/target/items/*.json      target-mandated work items
${ANALYSIS_ROOT}/orchestration/prompts/<stage>/     enriched prompts
${ANALYSIS_ROOT}/orchestration/results/<stage>/     validated results
${ANALYSIS_ROOT}/orchestration/reports/status.json  coverage + gates
${ANALYSIS_ROOT}/orchestration/registries/          requests and risks
${ANALYSIS_ROOT}/orchestration/traceability/        features, tests, links
${FRAMEWORK_DIR}/.logs/                             run logs and run summaries
```

---

## 10. Script stage reference

| Stage | Command it runs |
|---|---|
| preflight | tool and path checks |
| extractor | `clang++-22 ... nic_port_clang_extractor.cpp` |
| compiledb | `make -C` + `gen_compile_commands.py` |
| validate | `nic_port_pipeline.py validate-manifest` |
| osal | `nic_port_osal.py index` + `plan` (+ `stage` in mandatory mode) |
| extract | `nic_port_pipeline.py build` (or `resume` with `--resume`) |
| headers | `nic_port_headers.py scan` |
| diagnose | `nic_port_pipeline.py diagnose-extraction` |
| init | `nic_port_orchestrator.py init` |
| scope | `nic_port_target.py scope` (Linux-only triage + target work item plan) |
| specs | `ingest-spec` + `capabilities` |
| status | `nic_port_pipeline.py status --verbose` |
| architecture, target, methods, evidence, files, subsystems, final | `render` + `run` |
| audit | `verify-evidence`, `traceability`, `consistency`, `audit` |

`preflight`, `extractor`, `compiledb`, `validate`, `osal`, `extract`, `init` and `scope` are hard
prerequisites: the script aborts if one fails. Later stages are reported and the run continues.

```bash
/opt/ethernet-linux-idpf/scripts/nic_port_framework/run_framework.sh --manifest /opt/ethernet-linux-idpf/scripts/nic_port_framework/examples/idpf.full.manifest.json --spec-manifest /opt/ethernet-linux-idpf/scripts/nic_port_framework/templates/orchestrator/records/spec_manifest.example.json --out-dir /opt/ethernet-linux-idpf/.out --repo-dir /opt/ethernet-linux-idpf/ --src-dir /opt/ethernet-linux-idpf/idpf/src --kernel-dir /usr/src/linux-headers-6.8.0-117-generic --model claude-opus-5 --jobs 20 --device-flow --force --stages all
```
