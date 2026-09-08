# NIC Port Framework V3.3 — Resumable Extraction and Deterministic Knowledge Promotion

## Purpose

V3.3 is a non-destructive hardening iteration over V3.2. The chosen direction is extraction reliability: a production driver analysis must be measurable, resumable, deterministic, and safe to continue after a partial failure before additional orchestration features are added.

The framework retains the V3/V3.1/V3.2 architecture: one input manifest, external configuration/templates, compiler-derived source knowledge, schema-gated PE analysis, evidence, feature/test traceability, request/risk registries, file/subsystem/project integration, multi-variant merge, and semantic delta invalidation.

## New invariants

1. A successful translation unit is reusable only when its semantic cache identity still matches.
2. Cache/checkpoint/runtime telemetry never changes the semantic baseline by itself.
3. Global duplicate resolution follows authoritative compile-unit order, never worker completion order.
4. A failed builder run remains FAILED even when older orchestration artifacts are physically present.
5. `prepare` may bootstrap/build; `resume` never bootstraps and is the preferred recovery path after extraction failure.
6. Fallback recovery is cacheable and retains backend/recovery provenance.
7. Raw extractor artifacts are diagnostic artifacts, not the canonical KB.

## TU semantic cache identity

Each LibTooling TU cache key is derived from:

- source file SHA-256;
- normalized tooling compile arguments and working directory;
- compile dependency snapshot from the Kbuild/GCC `.d` file when available;
- extractor binary SHA-256;
- semantic extraction runtime (`pp_scope`, selected PP event kinds).

The dependency snapshot mode is configurable (`stat` or `content`). `stat` is the default because it is cheaper and is appropriate for interrupted-run resume. `content` can be selected where timestamp-preserving file replacement is a concern.

Successful entries are stored as compressed normalized TU records under `cache/translation_units/`. Failed entries are not reused by default.

## Checkpoint

`manifest/extraction_checkpoint.json` is updated after each completed TU. It records:

- TU path;
- status/failure class;
- backend;
- cache hit/miss;
- cache key;
- function count;
- last update time.

The checkpoint is operational provenance and intentionally excluded from the semantic baseline.

## Deterministic parallelism

V3.2 performed early duplicate elimination as worker results returned. With `libtooling_jobs > 1`, that could make “first occurrence wins” depend on thread completion order.

V3.3 removes that source of nondeterminism. Workers produce TU-local normalized records. Global function/struct/enum duplicate resolution runs only after results are reordered according to the authoritative compile-unit sequence.

## Resource and artifact guards

`config/rules/extraction_runtime.json` now defines:

- minimum MemAvailable before launching a TU;
- per-TU timeout;
- maximum retained raw shard size;
- total raw-artifact budget;
- cache directory and dependency fingerprint policy;
- fallback eligibility;
- quality-gate policy.

A resource guard produces an explicit failure class rather than silently starting an analysis process in an unsafe condition.

## Preflight and diagnostics

New/strengthened files:

- `manifest/preflight.json`
- `manifest/run_state.json`
- `manifest/extraction_quality.json`
- `manifest/translation_unit_diagnostics.jsonl`
- `manifest/extraction_checkpoint.json`
- `manifest/artifact_budget.json`
- `raw_extractor/index.json`

`preflight.json` records tool paths/version, extractor hash, compile database identity, available memory/disk, TU count and cache policy before extraction begins.

## Recovery workflow

Initial run:

```bash
python3 nic_port_pipeline.py --manifest PROJECT.manifest.json prepare
```

Always inspect extraction state when needed:

```bash
python3 nic_port_pipeline.py --manifest PROJECT.manifest.json diagnose-extraction
```

After correcting a TU-specific or environmental problem:

```bash
python3 nic_port_pipeline.py --manifest PROJECT.manifest.json resume
```

`resume` does not rerun bootstrap. Matching successful TU cache entries are reused; only missing/invalid TUs are freshly extracted.

## Fallback cache

If LibTooling produces a fallback-eligible failure and AST-JSON successfully recovers the TU, the recovered normalized TU is cached under the same semantic TU identity. Later resumes can reuse that result instead of repeating the known failing LibTooling attempt.

## Semantic baseline stability

The orchestration semantic baseline excludes operational fields such as:

- cache hit/fresh extraction counts;
- timestamps;
- elapsed time;
- memory telemetry;
- checkpoint progression.

Therefore an identical rerun served entirely from cache has the same semantic baseline fingerprint as a fresh run.

## Recommended IDPF sequence

1. `validate-manifest`
2. `prepare`
3. `diagnose-extraction`
4. If failed: resolve diagnostics, then `resume`
5. Require `run_state=COMPLETE` and `extraction_quality=PASS`
6. `status --verbose`
7. Perform extraction-quality audit of IDPF callback/state/API/PP coverage
8. Start the controlq + virtchnl vertical architecture/FAR/evidence/file/subsystem pilot
9. Use the existing request/risk/feature/test traceability control plane for implementation decisions
10. Use semantic delta/carry-forward for later source revisions

## Iterative principle

V3.3 intentionally adds operational reliability without weakening earlier semantics. Existing extracted facts, FAR schemas, registries, traceability, evidence, templates and orchestration stages remain valid. New cache/checkpoint files are additive and are not allowed to silently redefine source truth.

## Local-source function quality check

A TU is not considered semantically healthy merely because included project headers contributed inline function definitions. Diagnostics record `local_function_count` and `header_function_count`; the default quality profile requires a non-zero local source-file function count for normal semantic driver TUs. This prevents an include graph from masking failure to extract the actual `.c` implementation.

## NIC Port Framework v3.2

### Production invariant introduced in v3.2

A workspace is consumable only when all three statements are true:

1. the current builder run is `COMPLETE`;
2. the extraction quality gate is `PASS`;
3. orchestration provenance refers to that qualified extraction baseline.

Older downstream files may physically remain after a failed rerun; they are intentionally retained as engineering history. They are **not current** while `manifest/run_state.json` or `manifest/extraction_quality.json` says otherwise.

### v3.2 Runtime data flow

```text
authoritative Kbuild compile DB
        |
        v
analysis-only Clang tooling DB
        |
        v
one LibTooling process / TU
        |
        +--> compact TU facts --> parent KB accumulator
        |
        +--> per-TU diagnostic
        |
        +--> raw shard: delete on success / retain on failure (default)
        |
        v
extraction quality gate
        |
   +----+----+
   |         |
 PASS       FAIL
   |         |
   v         v
canonical   diagnostics + failed exit manifest
KB          orchestration blocked
   |
   v
orchestration init
```

### v3.2 Failure classes

The builder records a machine-readable failure class for each TU. The default classes include:

- `frontend_error`
- `process_signal`
- `out_of_memory_suspected`
- `timeout`
- `extractor_contract_error`
- `empty_semantic_output`
- `extractor_reported_error`

Fallback policy is external. By default only contract/empty-output cases are eligible for AST-JSON fallback. Frontend errors, signals, timeout and suspected OOM are *not* retried with the heavier AST backend.

### v3.2 Primary diagnostic artifacts

```text
manifest/run_state.json
manifest/extraction_quality.json
manifest/translation_unit_diagnostics.jsonl
manifest/extraction_errors.json
raw_extractor/index.json
raw_extractor/diagnostics/*.json
raw_extractor/libtooling_shards/*.json   # failed shards by default
```

Use:

```bash
python3 nic_port_pipeline.py \
  --manifest examples/idpf.full.manifest.json \
  diagnose-extraction
```

This command is valid even when orchestration is blocked.

### v3.2 Memory behavior

V3.1 could materialize a large per-TU JSON document, retain its normalized records, then retain all later documents in the parent before also constructing a merged raw JSON. Large kernel-header preprocessor streams multiplied the problem.

V3.2 reduces amplification in four places:

1. preprocessing events are scope/event filtered in C++ before serialization;
2. each raw shard is parsed and normalized immediately;
3. repeated header function/record definitions are compacted as TUs arrive;
4. successful raw shards and merged raw output are disabled by default.

The per-TU diagnostic records best-effort Linux memory telemetry so a later IDPF run can distinguish an extractor semantic failure from resource pressure.

### v3.2 Iterative/non-degrading behavior

V3.2 keeps exact-run hashes for audit but derives the orchestration semantic baseline from stable content. Timestamps, elapsed time, and other execution-only telemetry are excluded from the semantic fingerprint. Re-running the same source/build/config therefore does not create artificial semantic churn.

A real source/configuration change still invalidates dependent results through the existing semantic fingerprint and delta machinery.

### v3.2 Recommended IDPF rerun

```bash
export FRAMEWORK_DIR=/opt/nic_port_orchestrator/nic_port_framework
export FRAMEWORK_OUT_DIR=/opt/nic_port_orchestrator/out
export FRAMEWORK_REPO_DIR=/opt/porting/ethernet-linux-idpf
export FRAMEWORK_REPO_SRC_DIR=${FRAMEWORK_REPO_DIR}/idpf/src
export KERNEL_BUILD_DIR=/usr/src/linux-headers-6.1.0-52-amd64

cd "$FRAMEWORK_DIR"
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json validate-manifest
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json prepare
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json diagnose-extraction
python3 nic_port_pipeline.py --manifest examples/idpf.full.manifest.json status --verbose
```

If `prepare` fails, skip `status` and inspect `diagnose-extraction` plus the referenced per-TU diagnostic file. `run_idpf_analysis.sh` automates this branching.

### v3.2 Acceptance gate before architecture analysis

For the IDPF baseline, the recommended default remains strict:

```text
translation units accounted       100%
translation units semantic success 100%
current run state                  COMPLETE
extraction quality                 PASS
compile database                   authoritative
```

Recovered TUs are permitted but explicitly reported; a production baseline should review every recovery and decide whether the fallback is acceptable before freezing the baseline.


## NIC Port Framework V3.1 — Externalized Knowledge Catalogues and Derived Portability Indexes

### 1. Iteration objective

V3.1 is an additive iteration over V3. It does **not** replace compiler-derived knowledge, FAR review, evidence verification, registries, traceability, variants or semantic delta. It adds a configurable secondary analysis plane inspired by traditional source scanners and exact Linux API registries.

The design invariant is:

```text
                    INPUT MANIFEST
                          |
       +------------------+-------------------+
       |                  |                   |
       v                  v                   v
 compiler/build      semantic/catalog       templates/policy
 truth               configuration          configuration
       |                  |                   |
       +------------------+-------------------+
                          v
                 COMPILER-DERIVED KB
                          |
              +-----------+-----------+
              |                       |
              v                       v
       authoritative facts      derived signals
       calls/callbacks/AST       exact API catalogue
       PP/aliases/state          regex families
                                include families
                                Linux-specific families
                                risk/triage
              |                       |
              +-----------+-----------+
                          v
                 FAR / EVIDENCE / FILE
                    ENGINEERING REVIEW
                          |
                          v
                   OUTPUT MANIFEST
```

Compiler/LibTooling facts remain authoritative for actual syntax/AST relationships. Exact catalogue and regex detections are review/completeness signals. A weighted risk score is a triage metric, never an engineering decision.

### 2. Single input and output entrypoints

The default configuration entrypoint is:

```text
config/default.manifest.json
```

A project normally supplies one manifest that includes the default profile. Every referenced path is normalized relative to the file that introduced it, so defaults and extensions remain relocatable.

The generated exitpoint is:

```text
<output.root>/output_manifest.json
```

The exit manifest lists generated directory paths, significant artifacts, hashes/status where applicable, concise know-how and next actions. It is an index/navigation contract and does not concatenate referenced files.

### 3. Configuration externalization policy

V3.1 applies a strict distinction:

#### Must be external configuration/data

- Linux API patterns and exact-symbol catalogues.
- FreeBSD target hints.
- subsystem classification patterns.
- risk/category weights and thresholds.
- ignore directories and generated-TU patterns.
- Linux-specific source feature families.
- include/header classification.
- GCC/Kbuild -> Clang tooling normalization lists.
- prompt/context budgets and review limits.
- workflow/stage/confidence/authority/risk scales.
- result schemas.
- orchestration policy/gates.
- output directory/significant-file catalogue and know-how.
- prompt, record, repair, process and report templates.
- report columns/layout.

#### May remain implementation code

- transient dictionaries/lists/sets created while executing an algorithm.
- parser grammar/syntax expressions needed to interpret JSON/template/environment syntax.
- generic Python/JSON type dispatch tables that implement schema checking.
- local AST traversal mechanics.

Externalizing those implementation mechanics would turn code into data without making the framework more configurable or auditable. The important invariant is that **changing project/domain/policy/template behavior does not require editing Python**.

### 4. Default configuration graph

`config/default.manifest.json` includes `config/profiles/defaults.json`, which references:

```text
config/catalogs/linux_api_registry.json
config/rules/default_semantic_rules.json
config/rules/linux_specific_features.json
config/rules/include_classification.json
config/rules/portability_scoring.json
config/rules/tooling_normalization.json
config/rules/context_layout.json
config/policy/default_policy.json
config/framework/default_framework_model.json
config/output/default_output_catalog.json
config/reports/default_report_layout.json
config/templates/indexes/default_templates.json
```

Result schemas live under:

```text
config/schemas/results/
```

and all prompt/record/report wording lives under:

```text
templates/
```

### 5. Exact Linux API catalogue

The default exact catalogue is:

```text
config/catalogs/linux_api_registry.json
```

Each symbol has structured metadata such as:

```json
{
  "dma_map_single": {
    "semantic_category": "dma",
    "source_group": "dma",
    "symbol_kind": "function_or_symbol",
    "portability_relevance": "linux_kernel_api"
  }
}
```

The catalogue is useful for known functions, macros and types that are not always visible as external calls. It includes networking, DMA, PCI, synchronization, MMIO, work/timer, module, ethtool, devlink, PTP, auxiliary-bus, XDP/XSK and VFIO/mdev families.

Multiple catalogues may be supplied:

```json
{
  "configuration": {
    "api_catalogs": {
      "$append": ["idpf_api_catalog.extension.json"]
    }
  }
}
```

Exact-symbol redefinition is deliberately controlled. The default framework model uses:

```text
api_catalog_conflict_policy = error
```

so an extension cannot silently classify the same exact symbol differently. The resolved catalogue retains conflict diagnostics.

### 6. Regex API families remain complementary

`config/rules/default_semantic_rules.json` continues to provide regex families such as DMA, PCI, netstack, synchronization, async work, interrupts and related categories.

The two mechanisms have different roles:

```text
exact registry
    high-confidence known-symbol classification

regex families
    broader/future symbol-family coverage

compiler/AST
    authoritative call/location/structure facts
```

A symbol may therefore be detected through several sources. Detections retain provenance and risk scoring deduplicates equivalent semantic hits rather than summing every duplicate signal.

### 7. Linux-specific feature families

`config/rules/linux_specific_features.json` externalizes source-OS feature patterns such as:

```text
module_lifecycle
pci_driver_binding
network_device_registration
napi_polling
skb_packet_path
dma_buffer_management
interrupt_handling
deferred_execution
timers_and_polling
sysfs_procfs_debugfs
power_management
linux_logging_diagnostics
```

These are not treated as proof that an entire function is non-portable. They are coverage indicators for PE/FAR/file review.

### 8. Include classification

`config/rules/include_classification.json` defines standard-C headers and Linux/kernel prefix families.

The builder emits:

```text
indexes/include_report.csv
```

This makes file/header-level OS coupling visible separately from function calls.

### 9. Configurable portability scoring

`config/rules/portability_scoring.json` defines:

```text
category weights
per-category hit cap
detection-source weights
P0..P5 thresholds
risk bands
subsystem adjustments
recommendation rules
omit-candidate groups
documentation-queue source types
```

A function score is calculated from deduplicated category/symbol detections with detection provenance. The output can classify a function as e.g.:

```text
P0/P1: mostly reusable/common
P2: adaptation expected
P3: integration rewrite
P4: target-native redesign
P5: explicit design/defer decision
```

These names are configurable. The score is a prioritization signal only. FAR/file/subsystem agents must still reason from source semantics, evidence, target OS architecture, ownership and concurrency.

### 10. Derived portability indexes

V3.1 adds:

```text
indexes/function_inventory.csv
indexes/function_api_hits.csv
indexes/include_report.csv
indexes/linux_specific_functionality.csv
indexes/linux_api_usage.csv
indexes/api_catalog_gaps.csv
indexes/function_portability.jsonl
reports/portability_summary.md
```

#### `function_inventory.csv`

One row per function. Intended for prioritization and review planning. It includes stable/function identity, file/line information, subsystem, risk score/band, portability class, recommendation and API density according to the configured report layout.

#### `function_api_hits.csv`

One row per function/API detection. It records symbol, semantic category, source group, symbol kind, detection source, optional annotation and risk metadata.

Detection sources include forms such as:

```text
ast_external_call
macro_expansion
lexical_exact
regex_family
```

#### `linux_api_usage.csv`

Project-wide aggregation of detected Linux API usage for identifying repeated portability boundaries and shared abstractions.

#### `api_catalog_gaps.csv`

Symbols observed through compiler-derived calls or regex-family classification but absent from the exact catalogue. This is an explicit, reviewable feedback loop: a gap may be added to a project/catalogue extension in a later iteration, rejected as generic/non-portable noise, or left intentionally pattern-only. The framework never mutates the catalogue automatically.

#### `linux_specific_functionality.csv`

Function-to-source-OS-feature-family detections.

#### `include_report.csv`

Include/header classification and portability family.

#### `function_portability.jsonl`

Machine-readable derived portability records for downstream automation.

#### `reports/portability_summary.md`

Human summary rendered from `templates/reports/portability_summary.md` using `config/reports/default_report_layout.json`.

### 11. Documentation/evidence queue integration

Compiler external calls remain evidence candidates. V3.1 additionally permits configured high-confidence detection sources to produce documentation questions.

By default:

```text
ast_external_call
macro_expansion
lexical_exact
```

may enter the evidence queue, while `regex_family` is not automatically promoted because a family regex is a weaker signal.

This behavior is configured through `documentation_detection_sources` in the portability-scoring file.

### 12. Context integration

Method context packages now carry:

```text
catalog_api_hits
linux_specific_features
porting_risk
```

and the method-review template explicitly describes them as supplemental triage information. A method agent must not infer hardware semantics solely from a regex/API score.

The file porter still receives all accepted method FARs, evidence and request/risk registries and makes the file-scope implementation decisions.

### 13. External report/context/layout configuration

Report columns and presentation are in:

```text
config/reports/default_report_layout.json
```

Prompt/context size and excerpt limits are in:

```text
config/rules/context_layout.json
```

This removes another class of embedded thresholds/column lists from code.

### 14. Tooling normalization is also data

The Kbuild GCC -> Clang analysis-only transformation remains from V2.1, but its flag lists are now externalized:

```text
config/rules/tooling_normalization.json
```

The authoritative build `compile_commands.json` remains unchanged. The builder derives an auditable tooling database and records transformations.

### 15. Framework model and result schemas

Stage names, confidence levels, portability classes, authority levels, risk scales, stage identity rules and result-schema locations are in:

```text
config/framework/default_framework_model.json
```

Schemas live in:

```text
config/schemas/results/*.schema.json
```

This prevents stage validation contracts from being buried inside the orchestrator.

### 16. Output catalogue is data

The definition of meaningful output directory roles, significant files, generic know-how and next-step templates is in:

```text
config/output/default_output_catalog.json
```

The generated `output_manifest.json` additionally auto-indexes all produced directories recursively. This means a future stage can add a new artifact subtree without making the exit manifest blind to it.

### 17. Templates remain one-file-per-role

The default template index is:

```text
config/templates/indexes/default_templates.json
```

It maps stable roles to files under:

```text
templates/builder/
templates/orchestrator/contracts/
templates/orchestrator/misc/
templates/orchestrator/process/
templates/orchestrator/records/
templates/reports/
```

Templates use a deliberately small `{{VARIABLE}}` substitution syntax. JSON examples can therefore remain ordinary JSON without brace escaping.

### 18. Manifest extension model

A project can extend semantic rules or the API catalogue without copying the defaults.

Examples:

```text
examples/idpf_with_extension.manifest.json
examples/idpf_with_catalog_extension.manifest.json
examples/idpf_semantic_rules.extension.json
examples/idpf_api_catalog.extension.json
```

List merge directives are explicit:

```text
$append
$prepend
$replace
```

Path values remain relative to the manifest/config file that introduced them.

### 19. Optional manifest-defined bootstrap

V3.1 extends the single-entrypoint model to preparation commands.

A manifest may define:

```json
{
  "bootstrap": {
    "run_on_prepare": true,
    "steps": [
      {
        "id": "build-extractor",
        "enabled": true,
        "argv": ["clang++-22", "..."],
        "cwd": "...",
        "allow_failure": false
      }
    ]
  }
}
```

The pipeline executes `argv` directly with `shell=False`. There is no shell expansion/evaluation of a single command string. Bootstrap is disabled by default.

`examples/idpf.full.manifest.json` demonstrates:

```text
build LibTooling extractor
build IDPF external module
generate compile_commands.json
run extraction
initialize orchestration
```

from the same manifest entrypoint.

### 20. Configuration fingerprints and non-regression

The configuration lock now fingerprints categories by role, including:

```text
semantic/catalog/scoring configuration
tooling normalization
context layout
framework model
output catalogue
builder templates
orchestrator templates
record templates
report configuration/templates
orchestration policy
```

This supports selective invalidation:

```text
source/build or semantic catalogue change
    -> re-extract/reclassify

tooling normalization change
    -> regenerate compiler analysis

builder/context template change
    -> regenerate base contexts/prompts

orchestrator/record template change
    -> prompt hash/staleness handling

report layout/template change
    -> regenerate derived reports

policy change
    -> recompute gates without claiming source semantics changed

output-catalog change
    -> refresh navigation/output manifest
```

### 21. Request/risk/test/feature control plane

V3.1 preserves the controlled promotion model:

```text
METHOD AGENT
  may propose tests/requests/risks/features
        |
        v
orchestrator assigns stable provenance/scope metadata
        |
        v
FILE PORTER
  must disposition owned file-scoped items
  may implement/reject/defer/request evidence/escalate
        |
        v
SUBSYSTEM
  reconciles cross-file architecture
        |
        v
FINAL/PROJECT
  resolves true project-wide items
```

The derived portability risk is not automatically inserted as a project risk. It helps prioritize where a method/file review should look; accepted risks still require the structured risk record and explicit porter decision.

### 22. Canonical operational commands

Validate configuration:

```bash
python3 nic_port_pipeline.py \
  --manifest examples/idpf.manifest.json \
  validate-manifest
```

Run optional bootstrap explicitly:

```bash
python3 nic_port_pipeline.py \
  --manifest examples/idpf.full.manifest.json \
  bootstrap
```

Build extraction + initialize orchestration:

```bash
python3 nic_port_pipeline.py \
  --manifest examples/idpf.manifest.json \
  prepare
```

Inspect state:

```bash
python3 nic_port_pipeline.py \
  --manifest examples/idpf.manifest.json \
  status --verbose
```

Inspect next stage:

```bash
python3 nic_port_pipeline.py \
  --manifest examples/idpf.manifest.json \
  next
```

Run orchestration audit:

```bash
python3 nic_port_pipeline.py \
  --manifest examples/idpf.manifest.json \
  audit
```

### 23. IDPF workflow recommendation

For the real IDPF project, do not use the new risk report as a substitute for the compiler/PE pilot. Use it to prioritize and cross-check the first vertical slice.

Recommended sequence:

```text
manifest/bootstrap validates
        |
        v
Kbuild-normalized LibTooling baseline extraction
        |
        v
inspect function/API/include/risk indexes
        |
        v
compiler-KB extraction quality audit
        |
        v
architecture review
        |
        v
controlq + virtchnl FAR/evidence/file/subsystem pilot
        |
        v
compare derived risk indicators with actual porter decisions
        |
        v
adjust external catalogues/weights only when evidence justifies it
```

This feedback loop prevents the scoring model from becoming self-fulfilling.

### 24. Validation performed in V3.1

The implementation has been exercised on manifest-driven synthetic C projects.

Validated behaviors include:

```text
recursive manifest/profile loading
external API catalogue loading and conflict checking
external Linux-specific family loading
external include classification
external portability scoring
external tooling-normalization rules
external report/context/framework/output models
external prompt/record/report templates
compiler-derived function extraction
exact API classification
regex-family coverage
Linux-specific feature detection
risk/portability/recommendation generation
CSV/JSONL derived indexes
external report rendering
evidence-queue promotion for configured detection sources
configuration fingerprints/drift MATCH
orchestrator initialization and gates
single output manifest navigation
```

The catalog smoke case exercised DMA, locking, `READ_ONCE` and NAPI-like source and produced the expected derived API/evidence/risk indexes.

### 25. Iterative design conclusion

V3.1 deliberately integrates the useful ideas of older regex/function-inventory tools without regressing the architecture into text scanning:

```text
traditional scanner idea
    function inventory/API hits/risk/include report
        +
exact API registry idea
    human-auditable known-symbol catalogue
        +
V2/V3 compiler pipeline
    AST/PP/callback/alias/build truth
        =
V3.1 layered analysis model
```

The next framework change should be driven by real IDPF extraction/pilot findings rather than by adding more speculative categories. In particular, catalogue and risk weights should evolve as external data under review, preserving the rule that each iteration adds evidence and coverage without silently changing validated engineering decisions.
