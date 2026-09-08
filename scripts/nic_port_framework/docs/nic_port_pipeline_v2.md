# NIC Driver Re-hosting Pipeline V2

## Purpose

This revision turns the earlier source-context and orchestration scripts into an additive, non-regressing engineering pipeline for re-hosting a NIC driver from one OS to another (initially Linux IDPF -> FreeBSD). The design keeps deterministic facts separate from reviewed semantics and keeps both separate from target implementation decisions.

The core rule is **monotonic engineering value**: a new iteration may add evidence, refine semantics, introduce a variant, or change source code, but it must not silently discard validated knowledge. Changed inputs invalidate only affected regions where the dependency graph can prove impact; unchanged regions are carried forward.

## Artifacts

- `nic_port_context_builder.py` — deterministic extraction/knowledge-base/prompt preparation.
- `nic_port_clang_extractor.cpp` — dedicated LibTooling extractor for large projects; optional backend, same KB contracts.
- `nic_port_orchestrator.py` — schema/provenance/evidence/registry/traceability/delta orchestration.

The original V1 files are intentionally not overwritten.

## Production invariants

1. Build truth comes from the compilation database, not `*.c` scanning.
2. Compiler-derived facts are never replaced by LLM inference.
3. Method/file/subsystem/final outputs are schema-validated and provenance-bound.
4. User/project architectural proposals are strong recommendations, not unquestionable conclusions. Agents may propose a better design, but must record rationale/evidence/scope impact.
5. Method agents cannot author provenance, stable request/risk IDs, or silently expand scope; the orchestrator owns those fields.
6. Every method-origin request/risk owned by a file must receive an explicit file-porter disposition.
7. A file can advance when its **own** methods and critical evidence are ready; unrelated files do not block it.
8. A file-scoped item can be promoted to project scope only by an explicit file-level `ESCALATE_PROJECT` decision; it then appears in final project resolution.
9. Feature IDs are the join key across specifications, methods, files, subsystems, tests, requests, risks and evidence.
10. A source revision uses semantic delta analysis; only modified/affected analysis regions are invalidated and unaffected validated results can be carried forward.

## Pipeline

```text
source + compile_commands + specs
            |
            v
 deterministic extraction
            |
            v
 architecture model
            |
            v
 method FARs  ---> method requests/risks
            |                  |
            +-------> registries
            |                  |
            v                  v
 evidence resolution     file porter triage
            |                  |
            +---------> file architecture
                              |
                              v
                       subsystem design
                              |
                              v
                  final target-OS design
                              |
                              v
                 implementation/test DAG
```

## 1. LibTooling backend

The builder retains `ast-json` as a fallback and adds:

```bash
--extractor-backend auto|ast-json|libtooling
--libtooling-extractor /path/to/nic-port-clang-extractor
```

`auto` uses the dedicated binary when available and falls back to the legacy AST JSON path. The C++ backend emits the same core KB records plus compiler-derived preprocessor, alias and callback data.

The extractor records:

- function/type definitions;
- direct and indirect calls;
- function references and function-pointer bindings;
- C99 designated initializer/ops-table callback bindings;
- alias relationships;
- alias-canonicalized structure member access and mutation mode;
- macro definition/undefinition/expansion;
- `defined`, `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`, and skipped ranges.

## 2. Preprocessor provenance

New KB outputs include:

```text
kb/preprocessor_events.jsonl
kb/function_pointer_bindings.jsonl
kb/alias_relations.jsonl
kb/function_semantic_fingerprints.jsonl
```

Each function record additionally contains active preprocessor condition ancestry, macro expansions and indirect-call candidates.

Semantic fingerprints deliberately omit source line numbers so comment/whitespace/line-only movement does not invalidate analysis.

## 3. Function-pointer / callback analysis

Indirect calls retain an uncertainty model:

```text
unresolved
candidate_set
single_candidate
```

The system records candidates rather than inventing a unique target. Ops-table and designated-initializer bindings are represented as field-level bindings where the LibTooling backend can derive them.

## 4. Alias-aware mutation

State access records contain:

```text
object_expr
canonical_object
object_path
through_alias
alias_chain
mode = read|write|readwrite|address_taken
```

This allows cross-function delta analysis to propagate from a changed writer through users of the same canonical state path.

## 5. Specification ingestion

Use:

```bash
python3 nic_port_orchestrator.py ingest-spec \
  --root ANALYSIS_ROOT \
  --manifest spec_manifest.json
```

Example manifest:

```json
{
  "documents": [
    {
      "document_id": "VIRTCHNL2",
      "title": "Virtchnl2 specification",
      "version": "<version>",
      "authority_class": "A5",
      "path": "virtchnl2.md"
    }
  ]
}
```

Stable clause IDs are generated as:

```text
SPEC::<document_id>::<clause-id>
```

Explicit `SPEC-ID:` markers are supported and are preferred when specifications can be annotated.

## 6. Evidence verification and authority

Evidence sources use authority classes:

```text
A5 formal/hardware/protocol specification
A4 official OS docs/upstream/vendor source/manual
A3 upstream commit/reviewed mailing list/official issue
A2 reference/community evidence
A1 secondary material
A0 unknown
```

`verify-evidence` checks known specification IDs, local file/hash references, citation locator presence, and URL/reference structure; `--online` optionally checks URL reachability. This is **citation/provenance verification**, not automatic proof that arbitrary web prose semantically entails an agent claim.

Critical semantics default to requiring A4-or-better evidence.

## 7. Multi-variant semantic merge

Run one extraction/analysis root per meaningful build configuration, then:

```bash
python3 nic_port_orchestrator.py merge-variants \
  --roots analysis/base analysis/ptp analysis/siov \
  --out-dir analysis/merged
```

Functions are matched by stable function ID and classified as:

```text
common_identical
conditional_identical
analysis_divergence
variant_source_semantics
mixed_divergence
```

Only semantic divergences are placed into the merge-review queue. Duplicate human variant labels are disambiguated rather than collapsed.

## 8. Feature/test/request/risk control plane

### Feature/test database

`traceability` emits:

```text
orchestration/traceability/features.jsonl
orchestration/traceability/tests.jsonl
orchestration/traceability/links.jsonl
orchestration/traceability/coverage.json
```

Relations include:

```text
feature -> specified_by -> spec clause
feature -> implemented_by_source_function -> method
feature -> integrated_by_file -> file
feature -> owned_by_subsystem -> subsystem
feature -> verified_by -> test
feature -> has_port_request -> request
feature -> has_risk -> risk
```

Explicit duplicate `TEST-*` IDs with conflicting semantics are reported as a high-severity audit finding rather than silently overwritten. Identical references are merged with multiple origins.

Default required test priorities are P0/P1. Missing tests are visible and can be configured as a final-blocking gate.

### Script-owned request lists and risk registers

Method agents may propose items, but the orchestrator assigns scope/provenance/stable IDs.

Outputs:

```text
orchestration/registries/file_scope_requests.jsonl
orchestration/registries/project_scope_requests.jsonl
orchestration/registries/file_feature_risks.jsonl
orchestration/registries/project_scope_risks.jsonl
orchestration/registries/resolution.json
```

#### Method request proposal

```json
{
  "local_id": "REQ1",
  "scope_hint": "file|project",
  "category": "api|architecture|shared_abstraction|dependency|build|test|documentation|other",
  "title": "",
  "problem": "",
  "requested_change": "",
  "rationale": "",
  "cross_file_impact": "required for project scope",
  "feature_ids": [],
  "blocking": false,
  "acceptance_criteria": [],
  "dependencies": [],
  "evidence_refs": []
}
```

#### Method risk proposal

```json
{
  "local_id": "RSK1",
  "scope_hint": "file_feature|project",
  "title": "",
  "condition": "",
  "consequence": "",
  "likelihood": "L1|L2|L3|L4|L5",
  "impact": "I1|I2|I3|I4|I5",
  "mitigation": "",
  "detection": "",
  "trigger": "",
  "feature_ids": [],
  "blocking": false,
  "evidence_refs": []
}
```

The script adds:

```text
stable request_id/risk_id
origin_function_key
origin_stable_id
origin_file
origin_variant
origin method-result hash
effective scope
duplicate group
risk score
```

### File porter decision point

When all methods and critical evidence for one file are ready, that file becomes independently actionable.

The file porter receives:

```text
decision_required_requests
decision_required_risks
cross_file_project_requests_advisory
cross_file_project_risks_advisory
relevant feature/test traceability
```

Every owned item must receive exactly one decision. Ignoring an item is invalid.

Request decisions:

```text
IMPLEMENT_IN_FILE
ADOPT_DESIGN
ESCALATE_PROJECT
DEFER
REJECT
DUPLICATE
NEEDS_EVIDENCE
NOT_APPLICABLE
```

Risk decisions:

```text
MITIGATE_IN_FILE
ACCEPT
ESCALATE_PROJECT
DEFER
REJECT_INVALID
DUPLICATE
NEEDS_EVIDENCE
TRANSFER
```

An accepted item should link implementation/design and test IDs. `ESCALATE_PROJECT` promotes even an originally file-scoped item into the project-resolution list while preserving provenance.

The generated process/template files are under:

```text
orchestration/templates/registry_and_test_templates.json
orchestration/templates/spec_manifest.example.json
orchestration/templates/PROCESS.md
```

## 9. Architecture consistency checks

`consistency` performs cross-FAR checks including:

- may-sleep behavior reported in non-sleepable execution contexts;
- one logical lock assigned incompatible sleepable/non-sleepable classes;
- lock-order cycles;
- state/resource guard inconsistencies.

HIGH findings can block final synthesis.

## 10. Implementation delta and carry-forward

For a later source revision:

```bash
python3 nic_port_orchestrator.py delta \
  --old analysis/old \
  --new analysis/new \
  --out delta.json \
  --carry-forward
```

The delta uses stable IDs, semantic fingerprints, reverse direct-call closure and canonical shared-state coupling. It reports:

```text
added functions
removed functions
modified functions
impacted functions
unaffected functions
changed state paths
impacted files
impacted subsystems
architecture-surface change
```

Validated unaffected architecture/FAR/evidence/file/subsystem artifacts are carried forward where their dependencies remain valid. A line-only source movement does not count as semantic modification.

## Non-regression audit

Run after every meaningful iteration:

```bash
python3 nic_port_orchestrator.py audit --root ANALYSIS_ROOT
```

The audit does not require project completion. It detects degradation such as:

- extraction health failure;
- INVALID accepted results;
- STALE results (reported visibly, medium severity);
- duplicate function/spec stable IDs;
- conflicting TEST IDs;
- HIGH/MEDIUM architecture consistency findings;
- critical evidence declared verified without sufficient verified authority.

A missing not-yet-created result is not itself a regression.

## Typical flow

```bash
# 1. Extract one concrete build variant.
python3 nic_port_context_builder.py \
  --source /opt/porting/ethernet-linux-idpf/idpf/src \
  --compile-commands /work/build/compile_commands.json \
  --project-name idpf \
  --variant baseline \
  --source-os Linux \
  --target-os FreeBSD \
  --extractor-backend auto \
  --libtooling-extractor /opt/porting/nic-port-clang-extractor \
  --out /opt/porting/idpf-baseline \
  --strict

# 2. Initialize orchestration.
python3 nic_port_orchestrator.py init \
  --root /work/analysis/idpf-baseline

# 3. Ingest specs.
python3 nic_port_orchestrator.py ingest-spec \
  --root /work/analysis/idpf-baseline \
  --manifest /work/spec_manifest.json

# 4. Run architecture, then method FARs.
python3 nic_port_orchestrator.py render --root /work/analysis/idpf-baseline --stage architecture
python3 nic_port_orchestrator.py render --root /work/analysis/idpf-baseline --stage methods

# 5. Resolve evidence as affected methods become available.
python3 nic_port_orchestrator.py render --root /work/analysis/idpf-baseline --stage evidence
python3 nic_port_orchestrator.py verify-evidence --root /work/analysis/idpf-baseline

# 6. File prompts are now item-local: ready files can advance independently.
python3 nic_port_orchestrator.py render --root /work/analysis/idpf-baseline --stage files --item idpf_controlq.c

# 7. Observe control-plane state.
python3 nic_port_orchestrator.py registries --root /work/analysis/idpf-baseline --json
python3 nic_port_orchestrator.py traceability --root /work/analysis/idpf-baseline --json
python3 nic_port_orchestrator.py consistency --root /work/analysis/idpf-baseline --json
python3 nic_port_orchestrator.py audit --root /work/analysis/idpf-baseline

# 8. Continue with ready subsystems and final design.
python3 nic_port_orchestrator.py render --root /work/analysis/idpf-baseline --stage subsystems
python3 nic_port_orchestrator.py render --root /work/analysis/idpf-baseline --stage final
```

or

```bash
echo "Stage 0. Start: Pre-execution"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set -x && \
export FRAMEWORK_DIR="/opt/ethernet-linux-idpf/scripts/nic_port_framework" && \
export FRAMEWORK_OUT_DIR="${FRAMEWORK_DIR}/../../.out" && \
export FRAMEWORK_REPO_DIR="/opt/ethernet-linux-idpf" && \
export FRAMEWORK_REPO_SRC_DIR="${FRAMEWORK_REPO_DIR}/idpf/src" && \
export FRAMEWORK_MANIFEST="${1:-${FRAMEWORK_DIR}/examples/idpf.full.manifest.json}" && \
export FRAMEWORK_PIPELINE="${FRAMEWORK_DIR}/nic_port_pipeline.py" && \
export KERNEL_BUILD_DIR="/usr/src/linux-headers-$(uname -r)" && \
export KERNEL_GEN_COMPILE_COMMANDS="${GEN_COMPILE_COMMANDS:-${KERNEL_BUILD_DIR}/scripts/clang-tools/gen_compile_commands.py}"
FRAMEWORK_STAGE_0_RESULT="$?"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set +x
echo "Stage 0. End: $FRAMEWORK_STAGE_0_RESULT"

# 1. Extract one concrete build variant.
python3 "${FRAMEWORK_DIR}/nic_port_context_builder.py" \
  --source "${FRAMEWORK_REPO_SRC_DIR}" \
  --compile-commands "${FRAMEWORK_OUT_DIR}/compile_commands.json" \
  --project-name idpf \
  --variant baseline \
  --source-os Linux \
  --target-os FreeBSD \
  --extractor-backend auto \
  --libtooling-extractor "${FRAMEWORK_OUT_DIR}/nic-port-clang-extractor" \
  --out "${FRAMEWORK_OUT_DIR}/idpf-baseline" \
  --strict

# 2. Initialize orchestration.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" init --root "${FRAMEWORK_OUT_DIR}/idpf-baseline"

# 3. Ingest specs.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" ingest-spec --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --spec-manifest "${FRAMEWORK_SPEC_MANIFEST}"

# 4. Run architecture, then method FARs.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage architecture
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage methods

# 5. Resolve evidence as affected methods become available.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage evidence
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" verify-evidence --root "${FRAMEWORK_OUT_DIR}/idpf-baseline"

# 6. File prompts are now item-local: ready files can advance independently.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage files --item idpf_controlq.c

# 7. Observe control-plane state.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" registries --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --json
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" traceability --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --json
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" consistency --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --json
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" audit --root "${FRAMEWORK_OUT_DIR}/idpf-baseline"

# 8. Continue with ready subsystems and final design.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage subsystems
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage final
```

## Validation performed in this revision

- Both Python files pass `py_compile`.
- Legacy `ast-json` extraction was run on a synthetic driver.
- The new `libtooling` integration path was run end-to-end using a contract-compatible mock extractor, including compiler-style `#if` and macro expansion records.
- Semantic-delta testing verified that unrelated line insertion does not mark functions modified.
- A changed helper was the only modified function; its caller was classified as impacted through the call dependency graph.
- Architecture-surface comparison remained stable across relocated source roots.
- Variant merge correctly distinguished common and variant-semantic functions and disambiguated duplicate human variant labels.
- Full orchestration was tested through architecture -> methods -> file -> subsystem -> final.
- A file-scoped request was explicitly promoted with `ESCALATE_PROJECT` and appeared in final project resolution.
- Traceability produced feature/spec/method/file/subsystem/test/request/risk links with no required P0 test gap in the synthetic example.
- `audit` completed green after final re-synthesis.
- Delta carry-forward retained validated unchanged architecture and unaffected method analysis while leaving modified/impacted methods for re-analysis.

## Known environment limitation

The provided `nic_port_clang_extractor.cpp` could not be compiled in the current execution environment because Clang LibTooling development headers and `llvm-config` are not installed. The source targets current LibTooling/PPCallbacks APIs and the Python integration contract is exercised, but the C++ binary should be built and CI-tested in an LLVM/Clang development environment before production deployment.

```bash
root@linux-test:/opt/porting# apt list clang* | grep installed
clangd-22/oldstable-security,now 1:22.1.8-1~deb12u1 amd64 [installed]
clang-tools-22/oldstable-security,now 1:22.1.8-1~deb12u1 amd64 [installed]
clang-format-22/oldstable-security,now 1:22.1.8-1~deb12u1 amd64 [installed]
clang-tidy-22/oldstable-security,now 1:22.1.8-1~deb12u1 amd64 [installed]
clangd-22/oldstable-security,now 1:22.1.8-1~deb12u1 amd64 [installed]
```

```bash
apt update
apt install libclang-cpp22 libclang-cpp22-dev libclang-22 libclang-22-dev clang-22 clang clangd-22 clang-tools-22 clang-format-22 clang-tidy-22 python3 python3.11-full python3-pip build-essential vim curl wget git
```

```bash
clang++ -std=c++17 -O2 nic_port_clang_extractor.cpp $(llvm-config --cxxflags --ldflags --system-libs --libs) -lclang-cpp -o nic-port-clang-extractor -v
```

or unfolded:

```bash
export FRAMEWORK_DEBUG=true
export FRAMEWORK_SCRIPT_DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")")"

echo "Stage 0. Start: Pre-execution"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set -x && \
export FRAMEWORK_DIR="/opt/ethernet-linux-idpf/scripts/nic_port_framework" && \
export FRAMEWORK_OUT_DIR="${FRAMEWORK_DIR}/../../.out" && \
export FRAMEWORK_REPO_DIR="/opt/ethernet-linux-idpf" && \
export FRAMEWORK_REPO_SRC_DIR="${FRAMEWORK_REPO_DIR}/idpf/src" && \
export FRAMEWORK_MANIFEST="${1:-${FRAMEWORK_DIR}/examples/idpf.full.manifest.json}" && \
export FRAMEWORK_SPEC_MANIFEST="${2:-${FRAMEWORK_DIR}/templates/orchestrator/records/spec_manifest.example.json}" && \
export FRAMEWORK_PIPELINE="${FRAMEWORK_DIR}/nic_port_pipeline.py" && \
export KERNEL_BUILD_DIR="/usr/src/linux-headers-$(uname -r)" && \
export KERNEL_GEN_COMPILE_COMMANDS="${GEN_COMPILE_COMMANDS:-${KERNEL_BUILD_DIR}/scripts/clang-tools/gen_compile_commands.py}"
FRAMEWORK_STAGE_0_RESULT="$?"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set +x
echo "Stage 0. End: $FRAMEWORK_STAGE_0_RESULT"

echo "Stage 1. Preparation"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set -x && \
mkdir -p "${FRAMEWORK_OUT_DIR}" && \
cd "${FRAMEWORK_DIR}" && \
clang++ -v -std=c++17 -O2 "${FRAMEWORK_DIR}/nic_port_clang_extractor.cpp" \
  -I/usr/lib/llvm-22/include -D_GNU_SOURCE -D_GLIBCXX_USE_CXX11_ABI=1 -D__STDC_CONSTANT_MACROS \
  -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -fno-exceptions \
  -L/usr/lib/llvm-22/lib -lLLVM-22 -lclang-cpp \
  -o "${FRAMEWORK_OUT_DIR}/nic-port-clang-extractor"
FRAMEWORK_STAGE_1_RESULT="$?"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set +x
echo "Stage 1. End: $FRAMEWORK_STAGE_1_RESULT"

echo "Stage 2. Project source build and gen_compile_commands"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set -x && \
make -C "${FRAMEWORK_REPO_DIR}" && \
python3 "${KERNEL_GEN_COMPILE_COMMANDS}" -d "${FRAMEWORK_REPO_DIR}" -o "${FRAMEWORK_OUT_DIR}/compile_commands.json" && \
python3 "${FRAMEWORK_PIPELINE}" --manifest "${FRAMEWORK_MANIFEST}" validate-manifest && \
python3 "${FRAMEWORK_PIPELINE}" --manifest "${FRAMEWORK_MANIFEST}" prepare
FRAMEWORK_STAGE_2_RESULT="$?"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set +x
echo "Stage 2. End: $FRAMEWORK_STAGE_2_RESULT"

echo "Stage 3. Starting nic_port_context_builder"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set -x && \
python3 "${FRAMEWORK_PIPELINE}" --manifest "${FRAMEWORK_MANIFEST}" validate-manifest && \
python3 nic_port_context_builder.py \
  --source "${FRAMEWORK_REPO_SRC_DIR}" \
  --compile-commands "${FRAMEWORK_OUT_DIR}/compile_commands.json" \
  --project-name idpf \
  --variant baseline \
  --source-os Linux \
  --target-os FreeBSD \
  --extractor-backend auto \
  --libtooling-extractor "${FRAMEWORK_OUT_DIR}/nic-port-clang-extractor" \
  --out "${FRAMEWORK_OUT_DIR}/idpf-baseline" \
  --strict
FRAMEWORK_STAGE_3_RESULT="$?"
echo "Stage 3. End: $FRAMEWORK_STAGE_3_RESULT" > "${FRAMEWORK_DIR}/.logs/run.log"
[ "$FRAMEWORK_DEBUG" == 'true' ] && set +x

# 2. Initialize orchestration.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" init --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" 2>&1 | tee -a "${FRAMEWORK_DIR}/.logs/run.log"

# 3. Ingest specs.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" ingest-spec --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --spec-manifest "${FRAMEWORK_SPEC_MANIFEST}" 2>&1 | tee -a "${FRAMEWORK_DIR}/.logs/run.log"

# 4. Run architecture, then method FARs.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" \
    --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" render \
    --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" \
    --stage architecture 2>&1 | \
  tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" \
    --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" run \
    --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" \
    --stage architecture  --llm-model claude-opus-5 --llm-device-flow 2>&1 | \
  tee -a "${FRAMEWORK_DIR}/.logs/run.log"

python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" \
    --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" render \
    --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" \
    --stage methods 2>&1 | \
  tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" \
    --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" run \
    --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" \
    --stage methods  --llm-model claude-opus-5 --llm-device-flow 2>&1 | \
  tee -a "${FRAMEWORK_DIR}/.logs/run.log"

# 5. Resolve evidence as affected methods become available.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage evidence 2>&1 | tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" verify-evidence --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" 2>&1 | tee -a "${FRAMEWORK_DIR}/.logs/run.log"

python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" llm-auth --llm-model claude-opus-5 --llm-device-flow 2>&1 | tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" --manifest "${FRAMEWORK_DIR}/examples/idpf.full.manifest.json" capabilities 2>&1 | tee -a "${FRAMEWORK_DIR}/.logs/run.log"

# 6. File prompts are now item-local: ready files can advance independently.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage files --item idpf_controlq.c | tee -a "${FRAMEWORK_DIR}/.logs/run.log"

# 7. Observe control-plane state.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" registries --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --json | tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" traceability --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --json | tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" consistency --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --json | tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" audit --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" | tee -a "${FRAMEWORK_DIR}/.logs/run.log"

# 8. Continue with ready subsystems and final design.
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage subsystems | tee -a "${FRAMEWORK_DIR}/.logs/run.log"
python3 "${FRAMEWORK_DIR}/nic_port_orchestrator.py" render --root "${FRAMEWORK_OUT_DIR}/idpf-baseline" --stage final | tee -a "${FRAMEWORK_DIR}/.logs/run.log"

```
validate-manifest,bootstrap,build,prepare,resume,init-orchestration,status,next,audit,diagnose-extraction
# Then run the script, pointing it at the module directory


I would not add more framework features immediately. The next highest-value step is to **prove V2.1 against the real IDPF tree end-to-end**, then use the first real extraction results to drive the next iteration. Otherwise we risk hardening abstractions against synthetic cases rather than actual driver complexity.

My proposed sequence is:

1. **Run a clean V2.1 baseline extraction on IDPF.** Use the corrected kernel objtree, one-TU LibTooling isolation, generated-TU exclusion, and `--strict`. The immediate success criteria are not “all prompts generated”; they are: every intended TU parsed, no unexplained fallback, no missing kernel-generated headers, no fatal Clang incompatibility, and acceptable peak memory per TU.

2. **Audit the extracted KB before any LLM analysis.** I would inspect function counts, source-file coverage, call edges, unresolved indirect calls, callback/ops-table bindings, preprocessing events, alias/state accesses, external OS APIs, and extraction diagnostics. This is where we determine whether the extractor is actually giving production-quality facts. In particular, I would compare several complex IDPF functions manually against the generated records.

3. **Create an extraction-quality gate.** Based on the real IDPF output, add measurable acceptance thresholds such as “100% semantic TUs parsed,” “all ops tables captured,” “unresolved indirect-call percentage below X or explicitly classified,” “no missing active preprocessor provenance for compiled functions,” and “all extraction fallback events recorded.” This should become Phase 0.5 before orchestration can start.

4. **Ingest the authoritative specifications and create the initial feature ontology.** Before method analysis, populate stable IDs for virtchnl2/control-queue/descriptors/reset/queue models and FreeBSD target APIs. Then create the first real `FEATURE-*`, `SPEC::*`, and test namespaces. This prevents method agents from inventing incompatible feature names later.

5. **Pilot one foundational subsystem end-to-end.** My choice would be `control queue + virtchnl initialization`, not Tx/Rx yet. It is large enough to exercise protocol, DMA, MMIO, state, async behavior, evidence, requests/risks, file synthesis, and subsystem synthesis, but is more bounded than the datapath. Run:
   `architecture → FARs → evidence → file porter → subsystem porter → traceability → consistency → audit`.
   Do not analyze the entire driver first.

6. **Evaluate the request/risk mechanism using real findings.** This is especially important given your latest requirements. We should see whether method agents naturally produce useful items such as “shared DMA abstraction required,” “reset lifetime unclear,” or “firmware command semantics need project-wide policy.” Then verify that the file porter can correctly `IMPLEMENT`, `REJECT`, `DEFER`, or `ESCALATE_PROJECT` them without registry noise exploding.

7. **Build the evidence resolver next.** This is the missing major component. It should consume `doc_search_queue.jsonl` and registry evidence requests, search authoritative sources, normalize citations, assign authority scores, detect contradictions/staleness, and return structured evidence records. I would keep it provider-independent just like the other tools. This will remove the largest remaining manual break in the pipeline.

8. **Add real implementation planning only after the pilot passes.** At that point, generate the first FreeBSD target design records: OS-service mappings, target file responsibilities, implementation dependencies, acceptance tests, and milestone DAG. Then start with module/PCI/MMIO/CQ enablement rather than generating a broad port.

9. **Use the first source change to validate delta mode.** Make or consume a genuine IDPF upstream revision change and verify that semantic invalidation affects only changed functions, callers, owning files/features, associated evidence, and downstream designs. This is the strongest test of the “each iteration adds value without degrading prior validated work” requirement.

10. **Only then scale horizontally across the whole driver.** Once one subsystem is green, parallelize by subsystem: device lifecycle/reset, interrupts, Tx, Rx, RSS/offloads, management, PTP, virtualization, XDP/XSK. The framework should have earned the right to scale by surviving one real vertical slice.

**Opinion:** the most valuable immediate engineering work is steps 1–6, not another architectural rewrite. We now have enough machinery. What we lack is evidence that the machinery accurately models *real IDPF*. The first production milestone should therefore be:

```text
IDPF baseline extraction
        ↓
extraction-quality audit = PASS
        ↓
controlq/virtchnl pilot
        ↓
all FARs + evidence + file decisions
        ↓
request/risk registries resolved
        ↓
subsystem design = VALID
        ↓
traceability + consistency + audit = GREEN
```

After that, I would implement the **third component, `nic_port_evidence_resolver.py`**, because it is the largest remaining gap between “structured analysis” and a genuinely self-driving, evidence-backed porting workflow.

```json
{
  "documents": [
    {
      "document_id": "VIRTCHNL2",
      "title": "VIRTCHNL2 Protocol Specification",
      "version": "1.0.0",
      "authority_class": "A5",
      "path": "/root/drivers.ethernet.freebsd.common/scripts/mcp/runtime/uploads/virtchnl.md"
    },
    {
      "document_id": "idpf_specification",
      "title": "IDPF Protocol Specification",
      "version": "1.0.0",
      "authority_class": "A5",
      "path": "/root/drivers.ethernet.freebsd.common/.github/mgv_freebsd_idpf_speckit_v2/idpf_specification.md"
    },
    {
      "document_id": "freebsd_developers_handbook",
      "title": "FreeBSD Developers Handbook",
      "version": "1.0.0",
      "authority_class": "A5",
      "path": "/root/drivers.ethernet.freebsd.common/scripts/mcp/runtime/uploads/freebsd_developers-handbook_en.pdf.md"
    },
    {
      "document_id": "freebsd-architecture-handbook",
      "title": "FreeBSD Architecture Handbook",
      "version": "1.0.0",
      "authority_class": "A5",
      "path": "/root/drivers.ethernet.freebsd.common/scripts/mcp/runtime/uploads/FreeBSD-Architecture-Handbook-arch-handbook_en.pdf.md"
    },
    {
      "document_id": "freebsd-handbook",
      "title": "FreeBSD Handbook",
      "version": "1.0.0",
      "authority_class": "A5",
      "path": "/root/drivers.ethernet.freebsd.common/scripts/mcp/runtime/uploads/freebsd-handbook.pdf.md"
    }
  ]
}
```
