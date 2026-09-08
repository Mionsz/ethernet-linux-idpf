# V3.1 change summary

V3.1 is an additive hardening iteration over V3.

## Integrated from traditional source scanners / API registries

- Exact Linux API symbol catalogue, externally configurable and extensible.
- Regex API-family coverage retained as a secondary signal.
- Per-function API-hit inventory with detection provenance.
- Per-function weighted portability/risk triage and recommendation.
- Linux-specific functionality-family report.
- Include classification report.
- Project-wide Linux API usage report.
- Human portability summary rendered from an external template.
- API-catalogue gap report for iterative catalogue growth.

## Further externalization

Moved project/domain/policy data out of Python:

- Linux API exact catalogue and family patterns.
- Linux-specific feature patterns.
- FreeBSD target hints/subsystem patterns/risk weights/ignore rules.
- Portability scoring and thresholds.
- include classification.
- Kbuild/GCC -> Clang normalization lists.
- report layouts.
- context limits.
- stage/confidence/authority/risk model.
- result schemas.
- output-manifest catalogue.
- all operational prompt/record/report templates.

Transient algorithmic data structures and parser syntax implementation remain code by design.

## Single-entrypoint improvements

- `nic_port_pipeline.py --manifest ...` is the canonical operational entrypoint.
- optional manifest-defined bootstrap steps can build tools/source and create the compile DB using explicit argv execution with `shell=False`.
- `<output.root>/output_manifest.json` remains the canonical exitpoint and recursively indexes produced directories.

## Iterative/non-regression behavior

- semantic/catalog/scoring changes are fingerprinted separately from policy/templates/report/output navigation.
- exact catalogue conflicts fail by default rather than silently overriding.
- derived scores are triage signals only; FAR/file/subsystem/final decisions remain evidence-backed engineering decisions.
- catalogue gaps are reported but never auto-promoted.

# NIC Port Framework V3.2 — extraction resilience and transactional workspace validity

V3.2 is an additive hardening iteration driven by the first real IDPF V3.1 run. It preserves the V3.1 manifest/configuration model and orchestration contracts while changing the *runtime shape* of source extraction so a large kernel driver cannot silently leave a half-current workspace.

## Why this iteration exists

The IDPF run successfully parsed nine translation units, then seven later units produced zero semantic records and the automatic AST-JSON fallback did not complete. The same workspace contained a roughly 416 MB merged LibTooling JSON plus per-TU shards around 40–49 MB. This exposed memory amplification and insufficient extraction-state observability.

## Main changes

- LibTooling results are normalized one TU at a time. Raw JSON documents are released immediately.
- Repeated project-header function/struct/enum records are de-duplicated as TUs arrive, matching the previous resolver's first-occurrence semantics while reducing parent-process memory.
- Preprocessor events are filtered in the LibTooling extractor before serialization. Default scope is the configured project source root, with an external event-kind allow-list.
- Successful LibTooling raw shards are deleted by default. Failed shards are retained for diagnosis. The giant merged `raw_extractor/libtooling.json` is disabled by default.
- Per-TU diagnostics now record failure class, return code/signal, stderr/stdout tails, counts, PP filtering statistics, raw-shard provenance and best-effort memory telemetry.
- SIGKILL (`returncode == -9`) is classified as `out_of_memory_suspected`.
- `auto` fallback is failure-class aware. It does not answer a signal/OOM/timeout/frontend failure by immediately launching a much heavier full AST dump.
- AST fallback is sequential by default, limited in count, and spools JSON stdout to a temporary file rather than capturing it as one giant Python string.
- `manifest/run_state.json` and `manifest/extraction_quality.json` define transactional workspace validity. Orchestration requires current state `COMPLETE` and quality `PASS`.
- Failed extraction writes an exit manifest and diagnostics but deliberately does not present a canonical KB as current.
- Added `nic_port_pipeline.py ... diagnose-extraction` for current-run diagnosis without entering orchestration.
- Orchestration baseline fingerprinting now separates raw run provenance from stable semantic provenance, so timing/elapsed telemetry does not stale all accepted analysis on an otherwise identical rerun.
- Output-manifest know-how/significant-file entries include the new state, quality, diagnostic and raw-index artifacts.

## New configuration

`config/rules/extraction_runtime.json` controls:

- diagnostics/tails/memory telemetry;
- LibTooling timeout, PP scope/event kinds and raw retention;
- AST fallback eligibility/jobs/limits;
- extraction quality gates;
- workspace diagnostic artifact paths.

It is referenced by the default manifest profile and fingerprinted separately as semantic, quality and operational configuration.

## Compatibility

V3/V3.1 entrypoint files remain as wrappers. New deployments should use the stable entrypoint names:

```text
nic_port_pipeline.py
nic_port_context_builder.py
nic_port_orchestrator.py
nic_port_clang_extractor.cpp
```

The stable wrappers currently target V3.2.

# V3.3 changelog

- Added resumable per-TU semantic cache with gzip storage.
- Added cache identity from source hash, normalized compile command, dependency snapshot, extractor hash and semantic PP configuration.
- Added `resume` pipeline command that deliberately skips bootstrap.
- Added `manifest/extraction_checkpoint.json` with atomic per-TU progress updates.
- Added `manifest/preflight.json` and `manifest/artifact_budget.json`.
- Added configurable pre-TU memory/resource guard and raw artifact size budgets.
- Made global duplicate resolution deterministic under parallel LibTooling jobs.
- Made successful AST fallback recovery cacheable.
- Kept cache/checkpoint/runtime metrics outside the semantic baseline fingerprint.
- Extended `diagnose-extraction` with checkpoint/cache/preflight/artifact-budget information.
- Added `configuration.extraction_runtime` to the canonical manifest loader/default profile/schema.
- Curated extraction health/cache/checkpoint artifacts in the output-manifest catalogue.
- Updated stable entrypoints to V3.3 implementations.
- Updated full IDPF bootstrap example to compile the stable `nic_port_clang_extractor.cpp` entrypoint.
- Added local-source vs included-header function counts and a default local-function quality gate.
