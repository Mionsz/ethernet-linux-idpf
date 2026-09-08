#!/usr/bin/env python3
"""
nic_port_orchestrator.py

Production orchestration layer for nic_port_context_builder.py.

The context builder is deliberately deterministic: it extracts compiler/build
facts, source relations, OS-facing calls, state accesses, and base prompts.
This file provides the second layer: staged engineering review orchestration,
schema validation, provenance, evidence tracking, prompt enrichment, gates,
staleness detection, and optional execution through an external command-line
runner or a registered LLM provider (see nic_port_llm.py).

Design invariants
-----------------
1. Extraction facts are immutable inputs.  The orchestrator never edits the
   builder's manifest/, kb/, contexts/, graphs/, or prompts/ trees.
2. Every accepted analysis result is bound to:
      * source/build baseline fingerprint
      * exact enriched prompt SHA-256
      * result schema version
3. Downstream prompts are regenerated from validated upstream results.  If an
   upstream result changes, dependent prompt hashes change and old downstream
   results become STALE automatically.
4. Stage gates prevent "prompt batching" from bypassing architectural,
   evidence, confidence, and coverage requirements.
5. Documentation/evidence is first-class data.  It is aggregated and deduped
   rather than buried in prose.
6. No vendor SDK is embedded here.  `run` either shells out to an external argv
   template or calls a provider registered in `nic_port_llm` (copilot by
   default), both of which yield the same raw-result ingestion path.
7. Method agents may propose requests/risks, but provenance, effective scope,
   stable IDs and registry placement are assigned by this orchestrator.
8. File integration is a decision gate: every request/risk raised by that
   file's methods must receive an explicit disposition before the file passes.
9. JSON is the canonical interchange format.  Markdown/narrative may coexist
   in raw model output, but ingestion extracts and validates one JSON object.
10. Authorized MCP servers/tools and skills are declared in the specification
   manifest, ingested as capability records and injected into every prompt.

Expected builder output
-----------------------
<analysis-root>/
  manifest/
  kb/
  contexts/
  prompts/
  analysis/           # builder-created drop zones; left untouched

Orchestrator output
-------------------
<analysis-root>/orchestration/
  policy.json
  baseline.json
  inventory.json
  schemas/
  prompts/
    architecture/
    methods/
    evidence/
    files/
    subsystems/
    final/
  raw_results/
  results/
  result_meta/
  validation/
  reports/
  evidence/
  .lock

Recommended workflow
--------------------
  # 1. Initialize/refresh inventory and enriched architecture prompt.
  python3 nic_port_orchestrator.py init --root ./analysis/idpf-baseline

  # 2. Inspect stage status/gates.
  python3 nic_port_orchestrator.py status --root ./analysis/idpf-baseline

  # 3. Execute one stage with any external CLI that prints its answer to stdout.
  #    {prompt}, {stage}, {item_id}, {root} placeholders are supported.
  python3 nic_port_orchestrator.py run \
      --root ./analysis/idpf-baseline \
      --stage architecture \
      --runner 'my-llm-cli --input {prompt}'

  #    Or execute through a registered LLM provider (default: copilot).  The
  #    credential is cached locally, so device-flow login is needed only once.
  python3 nic_port_orchestrator.py list-models --llm-device-flow
  python3 nic_port_orchestrator.py run \
      --root ./analysis/idpf-baseline \
      --stage architecture --llm-model gpt-4.1

  # Or ingest a result produced elsewhere.
  python3 nic_port_orchestrator.py ingest \
      --root ./analysis/idpf-baseline \
      --stage architecture --item architecture \
      --result ./architecture-result.md

  # 4. Render/run methods after architecture passes.
  python3 nic_port_orchestrator.py render --root ... --stage methods
  python3 nic_port_orchestrator.py run    --root ... --stage methods \
      --runner 'my-llm-cli --input {prompt}' --jobs 8

  # 5. Evidence -> files -> subsystems -> final.
  python3 nic_port_orchestrator.py render --root ... --stage evidence
  python3 nic_port_orchestrator.py status --root ... --verbose

Important: the `run` command is a synchronous local process executor.  It does
not imply background/asynchronous work.  The external runner is responsible
for authentication/network access if it needs them.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import copy
import dataclasses
import datetime as dt
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence

from nic_port_manifest import (
    ManifestError, get_path, load_manifest, load_policy_from_manifest, load_template_index, load_semantic_rules,
    load_api_catalog, load_linux_specific_rules, load_include_classification, load_portability_scoring,
    load_tooling_normalization, load_report_layout, load_context_layout, load_output_catalog, load_framework_model, load_extraction_runtime,
    manifest_ref, render_template, template_path, validate_manifest_basics, write_output_manifest, sha256_file,
)
from nic_port_llm import (
    DEFAULT_PROVIDER, LLMProvider, ProviderError, TokenCache, create_provider, default_cache_path,
    print_models, provider_names, summarize_models,
)


TOOL_VERSION = "3.4.0"
SCHEMA_VERSION = 3
STAGES: tuple[str, ...] = ()
CONFIDENCE_ORDER: dict[str, int] = {}
PORTABILITY_CLASSES: set[str] = set()
RESULT_SCHEMAS: dict[str, dict[str, Any]] = {}
FRAMEWORK_MODEL: dict[str, Any] = {}
OUTPUT_CATALOG: dict[str, Any] = {}
EXTRACTION_RUNTIME: dict[str, Any] = {}

# Runtime policy/templates/framework model are loaded from the same input manifest
# used by the extractor. Domain contracts and enumerations are data, not code.
DEFAULT_POLICY: dict[str, Any] = {}
TEMPLATES: dict[str, Path] = {}
INPUT_MANIFEST: dict[str, Any] = {}
INPUT_MANIFEST_PATH: Path | None = None


@dataclass
class WorkItem:
    stage: str
    item_id: str
    source_prompt: str | None = None
    source_context: str | None = None
    identity: str | None = None
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class ValidationResult:
    valid: bool
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Generic filesystem/JSON helpers
# ---------------------------------------------------------------------------

def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()


def canonical_json(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_text(text: str) -> str:
    return sha256_bytes(text.encode("utf-8"))


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def load_json(path: Path, default: Any = None) -> Any:
    if not path.exists():
        return copy.deepcopy(default)
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def iter_jsonl(path: Path) -> Iterator[dict[str, Any]]:
    if not path.exists():
        return
    with path.open("r", encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"Invalid JSONL {path}:{n}: {exc}") from exc
            if isinstance(obj, dict):
                yield obj


def atomic_write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(tmp)


def atomic_write_json(path: Path, obj: Any) -> None:
    atomic_write_text(path, json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=False) + "\n")


def safe_id(value: str, max_len: int = 180) -> str:
    base = re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("._") or "item"
    if len(base) <= max_len:
        return base
    digest = hashlib.sha1(value.encode("utf-8")).hexdigest()[:12]
    return base[: max_len - 13] + "_" + digest


def deep_merge(base: dict[str, Any], overlay: Mapping[str, Any]) -> dict[str, Any]:
    out = copy.deepcopy(base)
    for key, value in overlay.items():
        if isinstance(value, Mapping) and isinstance(out.get(key), dict):
            out[key] = deep_merge(out[key], value)
        else:
            out[key] = copy.deepcopy(value)
    return out


@contextlib.contextmanager
def workspace_lock(root: Path) -> Iterator[None]:
    """Advisory process lock.  On non-POSIX systems it degrades gracefully."""
    p = root / "orchestration" / ".lock"
    p.parent.mkdir(parents=True, exist_ok=True)
    fh = p.open("a+", encoding="utf-8")
    try:
        try:
            import fcntl  # POSIX only
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
        except (ImportError, OSError):
            pass
        yield
    finally:
        try:
            import fcntl
            fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
        except (ImportError, OSError):
            pass
        fh.close()


# ---------------------------------------------------------------------------
# Workspace discovery and baseline provenance
# ---------------------------------------------------------------------------

def require_builder_root(root: Path) -> None:
    required = [
        root / "manifest" / "project.json",
        root / "manifest" / "summary.json",
        root / "manifest" / "run_state.json",
        root / "manifest" / "extraction_quality.json",
        root / "manifest" / "translation_unit_diagnostics.jsonl",
        root / "kb" / "functions.jsonl",
        root / "contexts" / "methods",
        root / "prompts" / "00_architecture_review.md",
        root / "prompts" / "99_final_port_design.md",
    ]
    missing = [str(x) for x in required if not x.exists()]
    if missing:
        raise SystemExit(
            "Not a complete nic_port_context_builder output root. Missing:\n  "
            + "\n  ".join(missing)
        )


def current_configuration_fingerprints() -> dict[str, str]:
    def payload(obj: dict[str, Any]) -> dict[str, Any]:
        return {k:v for k,v in obj.items() if not str(k).startswith("_")}
    rules = load_semantic_rules(INPUT_MANIFEST)
    semantic_payload = {
        "semantic_rules": payload(rules),
        "api_catalog": payload(load_api_catalog(INPUT_MANIFEST)),
        "linux_specific_rules": payload(load_linux_specific_rules(INPUT_MANIFEST)),
        "include_classification": payload(load_include_classification(INPUT_MANIFEST)),
        "portability_scoring": payload(load_portability_scoring(INPUT_MANIFEST)),
    }
    semantic = sha256_bytes(canonical_json(semantic_payload))
    tooling = sha256_bytes(canonical_json(payload(load_tooling_normalization(INPUT_MANIFEST))))
    report_config = sha256_bytes(canonical_json(payload(load_report_layout(INPUT_MANIFEST))))
    context_layout = sha256_bytes(canonical_json(payload(load_context_layout(INPUT_MANIFEST))))
    framework_model = sha256_bytes(canonical_json(payload(load_framework_model(INPUT_MANIFEST))))
    output_catalog = sha256_bytes(canonical_json(payload(load_output_catalog(INPUT_MANIFEST))))
    extraction_runtime_obj = payload(load_extraction_runtime(INPUT_MANIFEST))
    _lt = dict(extraction_runtime_obj.get("libtooling") or {})
    extraction_semantic = sha256_bytes(canonical_json({"pp_scope": _lt.get("pp_scope"), "pp_event_kinds": _lt.get("pp_event_kinds")}))
    extraction_quality = sha256_bytes(canonical_json(dict(extraction_runtime_obj.get("quality_gate") or {})))
    extraction_operational_obj = {k:v for k,v in extraction_runtime_obj.items() if k != "quality_gate"}
    if isinstance(extraction_operational_obj.get("libtooling"), dict):
        extraction_operational_obj["libtooling"] = {k:v for k,v in extraction_operational_obj["libtooling"].items() if k not in {"pp_scope", "pp_event_kinds"}}
    extraction_operational = sha256_bytes(canonical_json(extraction_operational_obj))
    template_hashes = {k: sha256_file(v) for k,v in TEMPLATES.items() if k != "__index__" and v.is_file()}
    def group(prefix: str) -> str:
        return sha256_bytes(canonical_json({k:v for k,v in template_hashes.items() if k.startswith(prefix)}))
    policy_refs = get_path(INPUT_MANIFEST, "configuration.orchestration_policy", []) or []
    if isinstance(policy_refs, str): policy_refs = [policy_refs]
    policy_payload = {}
    for ref in policy_refs:
        p = Path(str(ref)).resolve()
        policy_payload[str(p)] = sha256_file(p) if p.is_file() else None
    return {
        "semantic_fingerprint": semantic,
        "tooling_fingerprint": tooling,
        "report_config_fingerprint": report_config,
        "context_layout_fingerprint": context_layout,
        "framework_model_fingerprint": framework_model,
        "output_catalog_fingerprint": output_catalog,
        "extraction_semantic_fingerprint": extraction_semantic,
        "extraction_quality_fingerprint": extraction_quality,
        "extraction_operational_fingerprint": extraction_operational,
        "builder_template_fingerprint": group("builder."),
        "orchestrator_template_fingerprint": group("orchestrator."),
        "record_template_fingerprint": group("record."),
        "report_template_fingerprint": group("report."),
        "policy_fingerprint": sha256_bytes(canonical_json(policy_payload)),
    }


def validate_configuration_drift(root: Path) -> dict[str, Any]:
    lock = load_json(root / "manifest" / "configuration_lock.json", {}) or {}
    if not lock:
        return {"status": "UNKNOWN", "message": "builder configuration_lock.json is absent"}
    cur = current_configuration_fingerprints()
    hard = []
    if lock.get("semantic_fingerprint") != cur["semantic_fingerprint"]:
        hard.append("semantic/API/scoring classification changed since extraction; rebuild source knowledge before orchestration")
    if lock.get("tooling_fingerprint") != cur["tooling_fingerprint"]:
        hard.append("compiler-tooling normalization changed since extraction; rebuild source knowledge before orchestration")
    if lock.get("extraction_semantic_fingerprint") != cur["extraction_semantic_fingerprint"]:
        hard.append("extraction semantic scope/preprocessor-event policy changed; rebuild source knowledge before orchestration")
    if lock.get("extraction_quality_fingerprint") != cur["extraction_quality_fingerprint"]:
        hard.append("extraction quality-gate policy changed; rerun builder so the current baseline is requalified")
    if lock.get("framework_model_fingerprint") != cur["framework_model_fingerprint"]:
        hard.append("framework stages/result contracts changed since extraction; rebuild/init with the new framework model")
    if lock.get("context_layout_fingerprint") != cur["context_layout_fingerprint"]:
        hard.append("builder context-layout limits changed since extraction; regenerate contexts/base prompts before orchestration")
    if lock.get("builder_template_fingerprint") != cur["builder_template_fingerprint"]:
        hard.append("builder/base prompt templates changed since extraction; regenerate builder artifacts before orchestration")
    soft = []
    if lock.get("orchestrator_template_fingerprint") != cur["orchestrator_template_fingerprint"]:
        soft.append("orchestrator templates changed; enriched prompt hashes will invalidate affected results")
    if lock.get("record_template_fingerprint") != cur["record_template_fingerprint"]:
        soft.append("record templates changed; enriched prompt hashes will invalidate affected results")
    if lock.get("policy_fingerprint") != cur["policy_fingerprint"]:
        soft.append("orchestration policy changed; stage gates will be recomputed")
    if lock.get("report_config_fingerprint") != cur["report_config_fingerprint"] or lock.get("report_template_fingerprint") != cur["report_template_fingerprint"]:
        soft.append("derived report configuration/template changed; regenerate derived indexes/reports when desired")
    if lock.get("output_catalog_fingerprint") != cur["output_catalog_fingerprint"]:
        soft.append("output catalog changed; output_manifest navigation will be refreshed")
    if lock.get("extraction_operational_fingerprint") != cur["extraction_operational_fingerprint"]:
        soft.append("extraction diagnostics/raw-retention/fallback execution policy changed; existing semantic KB remains usable, next extraction will use the new runtime policy")
    report = {"status": "BLOCKED" if hard else ("CHANGED" if soft else "MATCH"), "blocking": hard, "changes": soft, "current": cur, "builder": {k: lock.get(k) for k in cur}}
    atomic_write_json(root / "orchestration" / "reports" / "configuration_drift.json", report)
    if hard:
        raise SystemExit("Configuration drift requires builder regeneration:\n  " + "\n  ".join(hard))
    return report


def _baseline_component_content(path: Path) -> Any:
    if path.suffix == ".jsonl":
        return list(iter_jsonl(path))
    return load_json(path)


def _stable_baseline_component(name: str, content: Any) -> Any:
    """Remove execution-only telemetry from the semantic baseline fingerprint.

    Raw component hashes remain in baseline.json for exact-run provenance, but a
    rerun of identical source/build/config must not stale all accepted analysis
    merely because timestamps or extractor elapsed times changed.
    """
    obj = copy.deepcopy(content)
    if isinstance(obj, dict):
        obj.pop("generated_utc", None)
        obj.pop("updated_utc", None)
        obj.pop("started_utc", None)
    if name == "manifest/run_state.json":
        # Health is enforced separately; this is not semantic source evidence.
        return {"status": obj.get("status"), "phase": obj.get("phase")} if isinstance(obj, dict) else obj
    if name == "manifest/extraction_quality.json" and isinstance(obj, dict):
        obj.pop("generated_utc", None)
        obj.pop("translation_units_cache_hits", None)
        obj.pop("translation_units_fresh_extractions", None)
    if name == "manifest/translation_unit_diagnostics.jsonl" and isinstance(obj, list):
        stable_rows = []
        for row in obj:
            if not isinstance(row, dict):
                continue
            stable_rows.append({
                k: row.get(k) for k in (
                    "translation_unit", "final_backend", "status", "recovered",
                    "failure_class", "function_count", "local_function_count", "header_function_count", "struct_count", "enum_count",
                    "pp_event_count", "errors"
                )
            })
        return stable_rows
    return obj


def baseline_components(root: Path) -> dict[str, Any]:
    names = [
        "manifest/project.json",
        "manifest/compile_units.json",
        "manifest/source_files.json",
        "manifest/summary.json",
        "manifest/extraction_errors.json",
        "manifest/extraction_quality.json",
        "manifest/run_state.json",
        "manifest/translation_unit_diagnostics.jsonl",
        "manifest/semantic_rules.resolved.json",
    ]
    out: dict[str, Any] = {}
    for name in names:
        p = root / name
        if p.exists():
            content = _baseline_component_content(p)
            stable = _stable_baseline_component(name, content)
            out[name] = {
                "sha256": sha256_file(p),
                "semantic_sha256": sha256_bytes(canonical_json(stable)),
                "content": content,
            }
    return out


def compute_baseline(root: Path) -> dict[str, Any]:
    components = baseline_components(root)
    semantic_components = {k: v["semantic_sha256"] for k, v in components.items()}
    fingerprint = sha256_bytes(canonical_json(semantic_components))
    run_fingerprint = sha256_bytes(canonical_json({k: v["sha256"] for k, v in components.items()}))
    project = load_json(root / "manifest" / "project.json", {})
    summary = load_json(root / "manifest" / "summary.json", {})
    errors = load_json(root / "manifest" / "extraction_errors.json", []) or []
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_utc": now_iso(),
        "fingerprint": fingerprint,
        "run_fingerprint": run_fingerprint,
        "project": project,
        "summary": summary,
        "extraction_error_count": len(errors),
        "components": {k: v["sha256"] for k, v in components.items()},
        "semantic_components": semantic_components,
    }


def extraction_health(root: Path) -> ValidationResult:
    errors = load_json(root / "manifest" / "extraction_errors.json", []) or []
    project = load_json(root / "manifest" / "project.json", {}) or {}
    summary = load_json(root / "manifest" / "summary.json", {}) or {}
    run_state = load_json(root / "manifest" / "run_state.json", {}) or {}
    quality = load_json(root / "manifest" / "extraction_quality.json", {}) or {}
    errs: list[str] = []
    warns: list[str] = []
    if run_state.get("status") != "COMPLETE":
        errs.append(f"Current builder run is not COMPLETE (status={run_state.get('status') or 'missing'}; phase={run_state.get('phase') or 'unknown'}).")
    if quality.get("status") != "PASS":
        errs.append("Current extraction quality gate is not PASS.")
        for msg in quality.get("blockers") or []:
            errs.append("Extraction quality: " + str(msg))
    if not project.get("build_graph_authoritative"):
        errs.append("Builder run is not compile-database authoritative.")
    if not summary.get("functions"):
        errs.append("No functions were extracted.")
    if errors:
        warns.append(f"{len(errors)} translation unit(s) reported extraction diagnostics; inspect manifest/translation_unit_diagnostics.jsonl.")
    if quality.get("translation_units_recovered"):
        warns.append(f"{quality.get('translation_units_recovered')} translation unit(s) required fallback recovery.")
    dirty = project.get("git_status_porcelain")
    if dirty:
        warns.append("Source tree was dirty when extracted; baseline is reproducible only with the recorded diff/worktree state.")
    return ValidationResult(not errs, errs, warns)


# ---------------------------------------------------------------------------
# Inventory
# ---------------------------------------------------------------------------

def load_functions(root: Path) -> list[dict[str, Any]]:
    return list(iter_jsonl(root / "kb" / "functions.jsonl"))


def prompt_by_stem(directory: Path) -> dict[str, Path]:
    return {p.stem: p for p in sorted(directory.glob("*.md")) if p.is_file()}


def target_scope(root: Path) -> dict[str, Any]:
    return load_json(root / "orchestration" / "target" / "exclusions.json", {}) or {}


def excluded_method_ids(root: Path) -> set[str]:
    """Methods the target triage removed from the porting loop."""
    return set((target_scope(root).get("excluded") or {}).keys())


def target_items(root: Path) -> list[WorkItem]:
    items: list[WorkItem] = []
    for ctx in sorted((root / "orchestration" / "target" / "items").glob("*.json")):
        obj = load_json(ctx, {}) or {}
        identity = str(obj.get("target_item_id") or ctx.stem)
        workflow = obj.get("workflow") or {}
        items.append(WorkItem(
            stage="target", item_id=ctx.stem,
            source_context=str(ctx), identity=identity,
            metadata={
                "target_item_id": identity,
                "title": obj.get("title"),
                "mandatory": bool(obj.get("mandatory", True)),
                "workflow_id": workflow.get("workflow_id"),
                "workflow_order": workflow.get("order"),
                "target_os": obj.get("target_os"),
                "driver_framework": obj.get("driver_framework"),
                "obligation_count": len(obj.get("obligations") or []),
                "superseded_source_method_count": obj.get("superseded_source_method_count") or 0,
            },
        ))
    return sorted(items, key=lambda x: (x.metadata.get("workflow_order") or 0, x.item_id))


def method_items(root: Path) -> list[WorkItem]:
    prompts = prompt_by_stem(root / "prompts" / "methods")
    excluded = excluded_method_ids(root)
    items: list[WorkItem] = []
    for ctx in sorted((root / "contexts" / "methods").glob("*.json")):
        obj = load_json(ctx, {}) or {}
        stem = ctx.stem
        if stem in excluded:
            continue
        key = str(obj.get("key") or obj.get("function_key") or stem)
        items.append(WorkItem(
            stage="methods",
            item_id=stem,
            source_prompt=str(prompts.get(stem)) if stem in prompts else None,
            source_context=str(ctx),
            identity=key,
            metadata={
                "function_key": key,
                "stable_id": obj.get("stable_id") or f"FN::{obj.get('file')}::{obj.get('name')}",
                "semantic_fingerprint": obj.get("semantic_fingerprint"),
                "source_sha256": obj.get("source_sha256"),
                "preprocessor_conditions": obj.get("preprocessor_conditions") or [],
                "file": obj.get("file"),
                "name": obj.get("name"),
                "portability_score": obj.get("portability_score"),
                "porting_risk": obj.get("porting_risk") or {},
                "catalog_api_hits": obj.get("catalog_api_hits") or [],
                "linux_specific_features": obj.get("linux_specific_features") or [],
                "subsystems": obj.get("subsystems") or [],
            },
        ))
    return items


def file_items(root: Path) -> list[WorkItem]:
    prompts = prompt_by_stem(root / "prompts" / "files")
    items: list[WorkItem] = []
    for ctx in sorted((root / "contexts" / "files").glob("*.json")):
        obj = load_json(ctx, {}) or {}
        stem = ctx.stem
        identity = str(obj.get("file") or stem)
        items.append(WorkItem(
            stage="files", item_id=stem,
            source_prompt=str(prompts.get(stem)) if stem in prompts else None,
            source_context=str(ctx), identity=identity,
            metadata={
                "file": identity,
                "function_keys": [x.get("key") for x in obj.get("functions", []) if x.get("key")],
                "function_stable_ids": [x.get("stable_id") for x in obj.get("functions", []) if x.get("stable_id")],
            },
        ))
    return items


def subsystem_items(root: Path) -> list[WorkItem]:
    prompts = prompt_by_stem(root / "prompts" / "subsystems")
    items: list[WorkItem] = []
    for ctx in sorted((root / "contexts" / "subsystems").glob("*.json")):
        obj = load_json(ctx, {}) or {}
        stem = ctx.stem
        identity = str(obj.get("subsystem") or stem)
        fkeys = [x.get("key") for x in obj.get("functions", []) if x.get("key")]
        files = sorted({x.get("file") for x in obj.get("functions", []) if x.get("file")})
        items.append(WorkItem(
            stage="subsystems", item_id=stem,
            source_prompt=str(prompts.get(stem)) if stem in prompts else None,
            source_context=str(ctx), identity=identity,
            metadata={"subsystem": identity, "function_keys": fkeys, "files": files,
                      "function_stable_ids": [x.get("stable_id") for x in obj.get("functions", []) if x.get("stable_id")]},
        ))
    return items


def architecture_item(root: Path) -> WorkItem:
    return WorkItem(
        stage="architecture", item_id="architecture",
        source_prompt=str(root / "prompts" / "00_architecture_review.md"),
        identity="architecture",
    )


def final_item(root: Path) -> WorkItem:
    return WorkItem(
        stage="final", item_id="final",
        source_prompt=str(root / "prompts" / "99_final_port_design.md"),
        identity="final",
    )


def all_static_items(root: Path) -> dict[str, list[WorkItem]]:
    return {
        "architecture": [architecture_item(root)],
        "target": target_items(root),
        "methods": method_items(root),
        "files": file_items(root),
        "subsystems": subsystem_items(root),
        "final": [final_item(root)],
    }


def build_inventory(root: Path, baseline: dict[str, Any]) -> dict[str, Any]:
    static = all_static_items(root)
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_utc": now_iso(),
        "baseline_fingerprint": baseline["fingerprint"],
        "stages": {
            stage: [dataclasses.asdict(x) for x in items]
            for stage, items in static.items()
        },
    }


# ---------------------------------------------------------------------------
# Schemas/prompts contracts
# ---------------------------------------------------------------------------

def json_schema_for_stage(stage: str) -> dict[str, Any]:
    schema = RESULT_SCHEMAS.get(stage)
    if not schema:
        raise ManifestError(f"No result schema configured for stage {stage!r}")
    return copy.deepcopy(schema)


def write_schemas(root: Path) -> None:
    d = root / "orchestration" / "schemas"
    for stage in STAGES:
        atomic_write_json(d / f"{stage}.schema.json", json_schema_for_stage(stage))


def write_process_templates(root: Path) -> None:
    """Materialize manifest-selected process/record templates into the workspace."""
    d = root / "orchestration" / "templates"
    d.mkdir(parents=True, exist_ok=True)
    names = [str(x) for x in (FRAMEWORK_MODEL.get("materialized_template_roles") or [])]
    copied: dict[str, str] = {}
    for name in names:
        src = template_path(TEMPLATES, name)
        dst_name = name.replace("record.", "").replace("orchestrator.", "")
        suffix = src.suffix or ".txt"
        dst = d / (dst_name if dst_name.endswith(suffix) else dst_name + suffix)
        shutil.copyfile(src, dst)
        copied[name] = str(dst)
    atomic_write_json(d / "template_sources.json", {
        "template_index": str(TEMPLATES.get("__index__")) if TEMPLATES.get("__index__") else None,
        "materialized": copied,
    })


def contract_text(stage: str, identity: str, baseline_fingerprint: str, project_name: str) -> str:
    name = f"orchestrator.contract.{stage}"
    return render_template(template_path(TEMPLATES, name), {
        "IDENTITY": identity,
        "BASELINE_FINGERPRINT": baseline_fingerprint,
        "PROJECT_NAME": project_name,
    })


# ---------------------------------------------------------------------------
# Result storage, extraction and validation
# ---------------------------------------------------------------------------

def result_path(root: Path, stage: str, item_id: str) -> Path:
    return root / "orchestration" / "results" / stage / f"{safe_id(item_id)}.json"


def result_meta_path(root: Path, stage: str, item_id: str) -> Path:
    return root / "orchestration" / "result_meta" / stage / f"{safe_id(item_id)}.json"


def validation_path(root: Path, stage: str, item_id: str) -> Path:
    return root / "orchestration" / "validation" / stage / f"{safe_id(item_id)}.json"


def enriched_prompt_path(root: Path, stage: str, item_id: str) -> Path:
    return root / "orchestration" / "prompts" / stage / f"{safe_id(item_id)}.md"


def raw_result_path(root: Path, stage: str, item_id: str) -> Path:
    return root / "orchestration" / "raw_results" / stage / f"{safe_id(item_id)}.txt"


def extract_json_object(text: str) -> dict[str, Any]:
    """Extract the last plausible JSON object from raw narrative/model output."""
    stripped = text.strip()
    if not stripped:
        raise ValueError("empty result")
    try:
        obj = json.loads(stripped)
        if isinstance(obj, dict):
            return obj
    except json.JSONDecodeError:
        pass

    fenced = re.findall(r"```(?:json)?\s*(\{.*?\})\s*```", text, flags=re.IGNORECASE | re.DOTALL)
    for candidate in reversed(fenced):
        try:
            obj = json.loads(candidate)
            if isinstance(obj, dict):
                return obj
        except json.JSONDecodeError:
            continue

    decoder = json.JSONDecoder()
    candidates: list[dict[str, Any]] = []
    for m in re.finditer(r"\{", text):
        try:
            obj, _ = decoder.raw_decode(text[m.start():])
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            candidates.append(obj)
    if candidates:
        return candidates[-1]
    raise ValueError("no valid JSON object found in result")


def validate_result(stage: str, obj: dict[str, Any], identity: str, baseline_fingerprint: str) -> ValidationResult:
    schema = RESULT_SCHEMAS.get(stage) or {}
    errors: list[str] = []
    warnings: list[str] = []
    props = schema.get("properties") or {}
    required = schema.get("required") or []
    type_map = {"string": str, "array": list, "object": dict, "boolean": bool, "integer": int, "number": (int, float)}
    for key in required:
        if key not in obj:
            errors.append(f"missing required key: {key}")
            continue
        spec = props.get(key) or {}
        expected_name = spec.get("type")
        expected = type_map.get(expected_name)
        if expected is not None and not isinstance(obj[key], expected):
            errors.append(f"key {key!r}: expected {expected_name}, got {type(obj[key]).__name__}")
        enum = spec.get("enum")
        if enum is not None and obj[key] not in enum:
            errors.append(f"key {key!r}: invalid value {obj[key]!r}; expected one of {enum}")

    validation = (FRAMEWORK_MODEL.get("stage_validation") or {}).get(stage) or {}
    identity_field = validation.get("identity_field")
    if identity_field and obj.get(identity_field) != identity:
        errors.append(f"{identity_field} mismatch: expected {identity!r}, got {obj.get(identity_field)!r}")
    for key, value in (validation.get("fixed_fields") or {}).items():
        if obj.get(key) != value:
            errors.append(f"{stage} {key} must be {value!r}")
    baseline_field = validation.get("baseline_field")
    if baseline_field and obj.get(baseline_field) != baseline_fingerprint:
        errors.append(f"{stage} {baseline_field} mismatch")
    status_field = validation.get("status_field")
    if status_field:
        allowed = list(validation.get("status_values") or [])
        if allowed and obj.get(status_field) not in allowed:
            errors.append(f"{status_field} must be one of {allowed}")
        if obj.get(status_field) == validation.get("verified_value"):
            for field_name in validation.get("verified_requires_nonempty") or []:
                if not obj.get(field_name):
                    errors.append(f"{validation.get('verified_value')} result requires non-empty {field_name}")

    conf = obj.get("confidence")
    high_levels = set(FRAMEWORK_MODEL.get("high_confidence_levels") or [])
    high_stages = set(FRAMEWORK_MODEL.get("high_confidence_evidence_stages") or [])
    if conf in high_levels and stage in high_stages:
        evidenceish = obj.get("sources") or obj.get("evidence_gaps") or obj.get("open_questions")
        if stage == "evidence" and not obj.get("sources"):
            errors.append("high-confidence evidence result requires sources")
        elif stage != "evidence" and evidenceish is None:
            warnings.append("High confidence result has no explicit evidence/open-question field content.")
    return ValidationResult(not errors, errors, warnings)


def _result_sha(root: Path, stage: str, item_id: str) -> str | None:
    p = result_path(root, stage, item_id)
    return sha256_file(p) if p.exists() else None



def _hash_jsonl_projection(path: Path, keys: Sequence[str] | None = None) -> str:
    rows = []
    for r in iter_jsonl(path):
        if keys:
            rows.append({k: r.get(k) for k in keys})
        else:
            rows.append(r)
    return sha256_bytes(canonical_json(rows))


def architecture_surface_fingerprint(root: Path) -> str:
    """Fingerprint the architecture surface while ignoring path/line-only churn.

    This deliberately excludes commit IDs, absolute build paths and preprocessor
    line numbers.  It changes when the build/configuration/architectural shape
    changes, not when a source tree is moved or comments are inserted.
    """
    project = load_json(root / "manifest" / "project.json", {}) or {}
    source_root_raw = str(project.get("source_root") or "")
    source_root = Path(source_root_raw).resolve() if source_root_raw else None

    def rel_file(value: Any) -> str:
        raw = str(value or "")
        if not raw:
            return raw
        p = Path(raw)
        if source_root is not None:
            try:
                return str(p.resolve().relative_to(source_root)).replace("\\", "/")
            except Exception:
                pass
        # Builder records are normally source-relative already.  If not, avoid
        # embedding workspace-specific prefixes in the architecture fingerprint.
        return raw.replace("\\", "/") if not p.is_absolute() else p.name

    def semantic_conditions(rows: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
        keep = ("directive", "condition", "name", "value", "active")
        return [{k: x.get(k) for k in keep if k in x} for x in rows if isinstance(x, Mapping)]

    funcs = []
    for f in load_functions(root):
        funcs.append({
            "stable_id": f.get("stable_id") or f"FN::{rel_file(f.get('file'))}::{f.get('name')}",
            "file": rel_file(f.get("file")), "name": f.get("name"),
            "subsystems": sorted(f.get("subsystems") or []),
            "external_categories": sorted({c for x in (f.get("calls_external") or []) for c in (x.get("categories") or [])}),
            "conditions": semantic_conditions(f.get("preprocessor_conditions") or []),
        })
    structs = [
        {"name": x.get("name"), "file": rel_file(x.get("file")), "fields": x.get("fields")}
        for x in iter_jsonl(root / "kb" / "structs.jsonl")
    ]
    cus = load_json(root / "manifest" / "compile_units.json", []) or []
    compile_surface = [
        {"file": rel_file(x.get("file")), "defines": sorted(x.get("defines") or [])}
        for x in cus
    ]
    payload = {
        "project_name": project.get("project_name"), "build_variant": project.get("build_variant"),
        "source_os": project.get("source_os"), "target_os": project.get("target_os"),
        "compile_surface": compile_surface, "functions": funcs, "structs": structs,
    }
    return sha256_bytes(canonical_json(payload))


def spec_fingerprint(root: Path) -> str:
    d = root / "orchestration" / "specifications"
    clauses = d / "clauses.jsonl"
    payload = {
        "clauses": _hash_jsonl_projection(clauses, ["spec_clause_id", "text_sha256", "source_version", "authority_class"]) if clauses.exists() else "no-specs",
        "mcp_and_tools": _hash_jsonl_projection(d / "mcp_and_tools.jsonl", ["capability_id", "path", "version", "authority_class", "sha256"]),
        "skills": _hash_jsonl_projection(d / "skills.jsonl", ["capability_id", "path", "version", "authority_class", "sha256"]),
    }
    return sha256_bytes(canonical_json(payload))


def registry_fingerprint(root: Path) -> str:
    d = root / "orchestration" / "registries"
    payload = {}
    for name in ("file_scope_requests", "project_scope_requests", "file_feature_risks", "project_scope_risks"):
        p = d / f"{name}.jsonl"
        payload[name] = list(iter_jsonl(p)) if p.exists() else []
    return sha256_bytes(canonical_json(payload))

def registry_fingerprint_for_file(root: Path, file_name: str, baseline: dict[str, Any], static: dict[str, list[WorkItem]]) -> str:
    """Hash only registry state that can affect one file porter.

    This is deliberately narrower than the project registry hash so that a new
    request in an unrelated file does not stale otherwise valid file analysis.
    """
    regs = load_registries(root)
    features = file_method_feature_ids(root, file_name, baseline, static)
    subset = registry_subset_for_file(file_name, features, regs)
    return sha256_bytes(canonical_json(subset))


def registry_fingerprint_for_subsystem(root: Path, item: WorkItem, baseline: dict[str, Any], static: dict[str, list[WorkItem]]) -> str:
    """Hash registry entries relevant to a subsystem's files or feature set."""
    regs = load_registries(root)
    files = set(str(x) for x in (item.metadata.get("files") or []))
    features: set[str] = set()
    for f in files:
        features.update(file_method_feature_ids(root, f, baseline, static))
    payload: dict[str, list[dict[str, Any]]] = {}
    for name, rows in regs.items():
        selected = []
        for r in rows:
            origin = str(r.get("origin_file") or "")
            rfeatures = set(str(x) for x in (r.get("feature_ids") or []))
            if origin in files or (features and features.intersection(rfeatures)):
                selected.append(r)
        payload[name] = selected
    return sha256_bytes(canonical_json(payload))


def _analysis_artifact_fingerprint(
    root: Path,
    stage: str,
    item: WorkItem,
    baseline: dict[str, Any],
    static: dict[str, list[WorkItem]],
    evidence_rows: Sequence[dict[str, Any]],
) -> str | None:
    rsha = _result_sha(root, stage, item.item_id)
    if rsha is None:
        return None
    dep = dependency_fingerprint(root, stage, item, baseline, static, evidence_rows)
    return sha256_bytes(canonical_json({"result_sha256": rsha, "dependency_fingerprint": dep}))


def dependency_fingerprint(
    root: Path,
    stage: str,
    item: WorkItem,
    baseline: dict[str, Any],
    static: dict[str, list[WorkItem]] | None = None,
    evidence_rows: Sequence[dict[str, Any]] | None = None,
) -> str:
    """Hash the exact validated-analysis dependencies of a work item.

    This fingerprint is independent from the rendered prompt file.  Therefore
    upstream result changes are detectable immediately, even before downstream
    prompts are regenerated.
    """
    if static is None:
        static = all_static_items(root)
    if evidence_rows is None:
        evidence_rows = aggregate_evidence(root, static, load_policy(root))

    deps: dict[str, Any] = {"stage": stage, "identity": item.identity, "spec_fingerprint": spec_fingerprint(root)}
    if stage == "architecture":
        deps["architecture_surface"] = architecture_surface_fingerprint(root)
    elif stage == "methods":
        deps["method_source_surface"] = {
            "stable_id": item.metadata.get("stable_id"),
            "semantic_fingerprint": item.metadata.get("semantic_fingerprint"),
            "source_sha256": item.metadata.get("source_sha256"),
            "preprocessor_conditions": item.metadata.get("preprocessor_conditions") or [],
        }
    else:
        deps["architecture_surface"] = architecture_surface_fingerprint(root)
    method_by_identity = {x.identity: x for x in static.get("methods", [])}
    file_by_identity = {x.identity: x for x in static.get("files", [])}

    if stage == "architecture":
        pass
    elif stage == "methods":
        deps["architecture"] = _analysis_artifact_fingerprint(root, "architecture", static["architecture"][0], baseline, static, evidence_rows)
    elif stage == "evidence":
        deps["ledger"] = item.metadata
        deps["methods"] = {
            fk: _analysis_artifact_fingerprint(root, "methods", method_by_identity[fk], baseline, static, evidence_rows)
            for fk in sorted(set(item.metadata.get("affected_functions") or []))
            if fk in method_by_identity
        }
    elif stage == "files":
        fkeys = sorted(set(item.metadata.get("function_keys") or []))
        deps["registry_fingerprint"] = registry_fingerprint_for_file(root, str(item.identity), baseline, static)
        deps["architecture"] = _analysis_artifact_fingerprint(root, "architecture", static["architecture"][0], baseline, static, evidence_rows)
        deps["methods"] = {
            fk: _analysis_artifact_fingerprint(root, "methods", method_by_identity[fk], baseline, static, evidence_rows)
            for fk in fkeys if fk in method_by_identity
        }
        deps["evidence"] = {
            row["evidence_id"]: _analysis_artifact_fingerprint(root, "evidence", WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row), baseline, static, evidence_rows)
            for row in evidence_rows
            if set(row.get("affected_functions") or []).intersection(fkeys)
        }
    elif stage == "subsystems":
        fkeys = sorted(set(item.metadata.get("function_keys") or []))
        deps["registry_fingerprint"] = registry_fingerprint_for_subsystem(root, item, baseline, static)
        files = sorted(set(item.metadata.get("files") or []))
        deps["architecture"] = _analysis_artifact_fingerprint(root, "architecture", static["architecture"][0], baseline, static, evidence_rows)
        deps["methods"] = {
            fk: _analysis_artifact_fingerprint(root, "methods", method_by_identity[fk], baseline, static, evidence_rows)
            for fk in fkeys if fk in method_by_identity
        }
        deps["files"] = {
            f: _analysis_artifact_fingerprint(root, "files", file_by_identity[f], baseline, static, evidence_rows)
            for f in files if f in file_by_identity
        }
        deps["evidence"] = {
            row["evidence_id"]: _analysis_artifact_fingerprint(root, "evidence", WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row), baseline, static, evidence_rows)
            for row in evidence_rows
            if set(row.get("affected_functions") or []).intersection(fkeys)
        }
    elif stage == "final":
        deps["registry_fingerprint"] = registry_fingerprint(root)
        trace_cov = load_json(root / "orchestration" / "traceability" / "coverage.json", {}) or {}
        consistency = load_json(root / "orchestration" / "reports" / "consistency.json", {}) or {}
        deps["traceability"] = sha256_bytes(canonical_json(trace_cov))
        deps["consistency"] = sha256_bytes(canonical_json({"findings": consistency.get("findings") or [], "counts": consistency.get("counts") or {}}))
        deps["architecture"] = _analysis_artifact_fingerprint(root, "architecture", static["architecture"][0], baseline, static, evidence_rows)
        deps["files"] = {x.item_id: _analysis_artifact_fingerprint(root, "files", x, baseline, static, evidence_rows) for x in static.get("files", [])}
        deps["subsystems"] = {x.item_id: _analysis_artifact_fingerprint(root, "subsystems", x, baseline, static, evidence_rows) for x in static.get("subsystems", [])}
        deps["evidence_inventory"] = evidence_rows
        deps["evidence"] = {row["evidence_id"]: _analysis_artifact_fingerprint(root, "evidence", WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row), baseline, static, evidence_rows) for row in evidence_rows}
    else:
        raise ValueError(stage)
    return sha256_bytes(canonical_json(deps))


def result_status(
    root: Path,
    stage: str,
    item: WorkItem,
    baseline: dict[str, Any],
    static: dict[str, list[WorkItem]] | None = None,
    evidence_rows: Sequence[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    rp = result_path(root, stage, item.item_id)
    mp = result_meta_path(root, stage, item.item_id)
    pp = enriched_prompt_path(root, stage, item.item_id)
    if not rp.exists():
        return {"status": "MISSING", "valid": False, "confidence": None}
    obj = load_json(rp, {}) or {}
    obj, migration_warnings = normalize_result_v2(stage, obj)
    meta = load_json(mp, {}) or {}
    vr = validate_result(stage, obj, item.identity or item.item_id, baseline["fingerprint"])
    vr.warnings.extend(migration_warnings)
    if stage == "files":
        static_now = static or all_static_items(root)
        policy_now = load_policy(root)
        regs = load_registries(root)
        if not any(regs.values()):
            regs = build_registries(root, baseline, static_now, policy_now)
        subset = registry_subset_for_file(str(item.identity), file_method_feature_ids(root, str(item.identity), baseline, static_now), regs)
        extra = validate_file_registry_decisions(obj, str(item.identity), subset, policy_now)
        if extra:
            vr.errors.extend(extra); vr.valid = False
    elif stage == "final":
        static_now = static or all_static_items(root)
        policy_now = load_policy(root)
        regs = load_registries(root)
        if not any(regs.values()):
            regs = build_registries(root, baseline, static_now, policy_now)
        extra = validate_final_registry_decisions(obj, project_registry_resolution(root, baseline, static_now, regs), policy_now)
        if extra:
            vr.errors.extend(extra); vr.valid = False
    stale_reasons: list[str] = []
    if int(meta.get("schema_version") or 1) < 2 and meta.get("baseline_fingerprint") != baseline["fingerprint"]:
        stale_reasons.append("baseline fingerprint changed (legacy provenance)")
    current_dep = dependency_fingerprint(root, stage, item, baseline, static, evidence_rows)
    if meta.get("dependency_fingerprint") != current_dep:
        stale_reasons.append("upstream analysis/evidence dependency changed")
    if pp.exists():
        current_hash = sha256_file(pp)
        if meta.get("prompt_sha256") != current_hash:
            stale_reasons.append("enriched prompt changed")
    else:
        stale_reasons.append("current enriched prompt is absent")
    if stale_reasons:
        return {
            "status": "STALE", "valid": False, "confidence": obj.get("confidence"),
            "errors": vr.errors, "warnings": vr.warnings, "stale_reasons": stale_reasons,
        }
    return {
        "status": "VALID" if vr.valid else "INVALID",
        "valid": vr.valid,
        "confidence": obj.get("confidence"),
        "errors": vr.errors,
        "warnings": vr.warnings,
    }


def ingest_result(root: Path, stage: str, item: WorkItem, raw_path: Path, baseline: dict[str, Any]) -> ValidationResult:
    text = raw_path.read_text(encoding="utf-8", errors="replace")
    try:
        obj = extract_json_object(text)
    except Exception as exc:
        vr = ValidationResult(False, [f"JSON extraction failed: {exc}"])
        atomic_write_json(validation_path(root, stage, item.item_id), dataclasses.asdict(vr))
        return vr
    obj, migration_warnings = normalize_result_v2(stage, obj)
    vr = validate_result(stage, obj, item.identity or item.item_id, baseline["fingerprint"])
    vr.warnings.extend(migration_warnings)
    if stage == "files":
        static_now = all_static_items(root)
        policy_now = load_policy(root)
        regs = build_registries(root, baseline, static_now, policy_now)
        subset = registry_subset_for_file(str(item.identity), file_method_feature_ids(root, str(item.identity), baseline, static_now), regs)
        vr.errors.extend(validate_file_registry_decisions(obj, str(item.identity), subset, policy_now))
        vr.valid = not vr.errors
    elif stage == "final":
        static_now = all_static_items(root)
        policy_now = load_policy(root)
        regs = build_registries(root, baseline, static_now, policy_now)
        vr.errors.extend(validate_final_registry_decisions(obj, project_registry_resolution(root, baseline, static_now, regs), policy_now))
        vr.valid = not vr.errors
    atomic_write_json(validation_path(root, stage, item.item_id), dataclasses.asdict(vr))
    if not vr.valid:
        return vr

    pp = enriched_prompt_path(root, stage, item.item_id)
    if not pp.exists():
        return ValidationResult(False, [f"Enriched prompt does not exist: {pp}. Run render first."])
    atomic_write_json(result_path(root, stage, item.item_id), obj)
    prompt_meta = load_json(pp.with_suffix(".meta.json"), {}) or {}
    atomic_write_json(result_meta_path(root, stage, item.item_id), {
        "schema_version": SCHEMA_VERSION,
        "orchestrator_version": TOOL_VERSION,
        "stage": stage,
        "item_id": item.item_id,
        "identity": item.identity,
        "accepted_utc": now_iso(),
        "baseline_fingerprint": baseline["fingerprint"],
        "dependency_fingerprint": prompt_meta.get("dependency_fingerprint"),
        "prompt_sha256": sha256_file(pp),
        "raw_result_sha256": sha256_file(raw_path),
        "raw_result_path": str(raw_path),
        "validation_warnings": vr.warnings,
    })
    return vr


# ---------------------------------------------------------------------------
# Evidence inventory
# ---------------------------------------------------------------------------

def evidence_id_for(*parts: str) -> str:
    raw = "\x1f".join(parts)
    return "EV-" + hashlib.sha256(raw.encode("utf-8")).hexdigest()[:16]


def aggregate_evidence(root: Path, static_items: dict[str, list[WorkItem]], policy: dict[str, Any]) -> list[dict[str, Any]]:
    by_key: dict[tuple[str, str], dict[str, Any]] = {}
    qpath = root / "kb" / "doc_search_queue.jsonl"
    for row in iter_jsonl(qpath):
        symbol = str(row.get("symbol") or "<unknown>")
        category = str(row.get("semantic_category") or "external_unclassified")
        dedupe_key = (symbol, category)
        rec = by_key.setdefault(dedupe_key, {
            "evidence_id": evidence_id_for("api", symbol, category),
            "kind": "api_semantics",
            "symbol": symbol,
            "semantic_category": category,
            "question": f"Resolve authoritative source and target semantics for {symbol} as service '{category}'.",
            "source_questions": [],
            "target_questions": [],
            "research_hints": [],
            "affected_functions": [],
            "critical": category in set(policy["evidence"]["critical_categories"]),
        })
        if row.get("source_question"):
            rec["source_questions"].append(row["source_question"])
        if row.get("target_question"):
            rec["target_questions"].append(row["target_question"])
        if row.get("target_research_hint"):
            rec["research_hints"].append(row["target_research_hint"])
        if row.get("function_key"):
            rec["affected_functions"].append(row["function_key"])

    # Method-specific evidence questions become stable evidence items only after
    # methods are analyzed.  They complement (not replace) the API ledger.
    if policy.get("evidence", {}).get("include_method_questions", True):
        for item in static_items["methods"]:
            rp = result_path(root, "methods", item.item_id)
            if not rp.exists():
                continue
            obj = load_json(rp, {}) or {}
            for idx, q in enumerate(obj.get("documentation_search") or []):
                if isinstance(q, str):
                    question = q.strip()
                    preferred = ""
                elif isinstance(q, dict):
                    question = str(q.get("question") or "").strip()
                    preferred = str(q.get("preferred_source") or "").strip()
                else:
                    continue
                if not question:
                    continue
                eid = evidence_id_for("method", str(item.identity), question)
                key = (eid, "method_question")
                by_key[key] = {
                    "evidence_id": eid,
                    "kind": "method_question",
                    "symbol": "",
                    "semantic_category": "method_semantics",
                    "question": question,
                    "source_questions": [question],
                    "target_questions": [],
                    "research_hints": [preferred] if preferred else [],
                    "affected_functions": [item.identity],
                    # Method questions are critical when the analyzed method is
                    # high portability risk P3-P5; policy can still be edited.
                    "critical": str(obj.get("portability_class")) in {"P3", "P4", "P5"},
                }

    rows = []
    for rec in by_key.values():
        for key in ("source_questions", "target_questions", "research_hints", "affected_functions"):
            rec[key] = sorted(set(x for x in rec[key] if x))
        rows.append(rec)
    rows.sort(key=lambda x: (not x["critical"], x["semantic_category"], x.get("symbol") or "", x["evidence_id"]))
    return rows


def evidence_items(root: Path, evidence_rows: Sequence[dict[str, Any]]) -> list[WorkItem]:
    return [WorkItem(
        stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=dict(row)
    ) for row in evidence_rows]



# ---------------------------------------------------------------------------
# V2 registries, specifications, traceability, evidence verification and
# consistency support.  These are derived artifacts: they never overwrite
# builder facts or accepted analysis results.
# ---------------------------------------------------------------------------

AUTHORITY_ORDER: dict[str, int] = {}
LIKELIHOOD_ORDER: dict[str, int] = {}
IMPACT_ORDER: dict[str, int] = {}


def atomic_write_jsonl(path: Path, rows: Iterable[Mapping[str, Any]]) -> None:
    text = "".join(json.dumps(dict(r), sort_keys=True, ensure_ascii=False) + "\n" for r in rows)
    atomic_write_text(path, text)


def normalize_result_v2(stage: str, obj: dict[str, Any]) -> tuple[dict[str, Any], list[str]]:
    """Add non-destructive V2 defaults so V1 results remain ingestible.

    New process fields are mandatory in newly rendered prompts, but absence in
    old results is migrated to empty collections and reported as a warning.
    File/final registry coverage validation later rejects an empty collection
    when actual registry items require decisions.
    """
    obj = copy.deepcopy(obj)
    warnings: list[str] = []
    defaults: dict[str, Any] = copy.deepcopy((FRAMEWORK_MODEL.get("stage_migration_defaults") or {}).get(stage) or {})
    for k, v in defaults.items():
        if k not in obj:
            obj[k] = copy.deepcopy(v)
            warnings.append(f"V2 migration inserted missing field {k!r}")
    return obj, warnings


def _function_meta(static: dict[str, list[WorkItem]]) -> dict[str, dict[str, Any]]:
    return {str(x.identity): dict(x.metadata) for x in static.get("methods", [])}


def _variant_label(root: Path) -> str:
    p = load_json(root / "manifest" / "project.json", {}) or {}
    return str(p.get("build_variant") or "")


def _request_signature(row: Mapping[str, Any]) -> str:
    return sha256_bytes(canonical_json({
        "scope": row.get("scope"), "category": row.get("category"),
        "title": str(row.get("title") or "").strip().lower(),
        "requested_change": str(row.get("requested_change") or "").strip().lower(),
        "feature_ids": sorted(str(x) for x in (row.get("feature_ids") or [])),
    }))[:16]


def _risk_signature(row: Mapping[str, Any]) -> str:
    return sha256_bytes(canonical_json({
        "scope": row.get("scope"),
        "title": str(row.get("title") or "").strip().lower(),
        "condition": str(row.get("condition") or "").strip().lower(),
        "consequence": str(row.get("consequence") or "").strip().lower(),
        "feature_ids": sorted(str(x) for x in (row.get("feature_ids") or [])),
    }))[:16]


def _method_feature_ids(obj: Mapping[str, Any]) -> list[str]:
    return sorted(set(str(x) for x in (obj.get("feature_ids") or []) if str(x).strip()))


def _normalize_request_proposal(
    proposal: Any,
    *,
    method_obj: Mapping[str, Any],
    item: WorkItem,
    variant: str,
    method_result_sha: str,
    policy: Mapping[str, Any],
    index: int,
) -> dict[str, Any] | None:
    if isinstance(proposal, str):
        proposal = {"title": proposal, "problem": proposal, "requested_change": proposal, "scope_hint": "file"}
    if not isinstance(proposal, Mapping):
        return None
    title = str(proposal.get("title") or proposal.get("request") or "").strip()
    change = str(proposal.get("requested_change") or proposal.get("change") or "").strip()
    problem = str(proposal.get("problem") or proposal.get("rationale") or title).strip()
    if not title and not change:
        return None
    hint = str(proposal.get("scope_hint") or proposal.get("scope") or "file").lower()
    scope = "project" if hint == "project" else "file"
    cross = str(proposal.get("cross_file_impact") or "").strip()
    adjustment = None
    if scope == "project" and policy.get("registries", {}).get("project_scope_requires_cross_file_impact", True) and not cross:
        scope = "file"
        adjustment = "project scope hint downgraded: cross_file_impact was not supplied"
    feature_ids = sorted(set(_method_feature_ids(method_obj) + [str(x) for x in (proposal.get("feature_ids") or []) if str(x).strip()]))
    origin_stable = str(item.metadata.get("stable_id") or item.identity)
    local_id = str(proposal.get("local_id") or f"R{index+1}")
    rid = "REQ-" + sha256_text("\x1f".join([origin_stable, local_id, title, change]))[:16]
    row = {
        "request_id": rid,
        "local_id": local_id,
        "scope": scope,
        "scope_hint": hint,
        "scope_adjustment_reason": adjustment,
        "category": str(proposal.get("category") or "other"),
        "title": title or change,
        "problem": problem,
        "requested_change": change or title,
        "rationale": str(proposal.get("rationale") or ""),
        "cross_file_impact": cross,
        "blocking": bool(proposal.get("blocking", False)),
        "urgency": str(proposal.get("urgency") or ("blocking" if proposal.get("blocking") else "normal")),
        "acceptance_criteria": list(proposal.get("acceptance_criteria") or []),
        "dependencies": list(proposal.get("dependencies") or []),
        "evidence_refs": list(proposal.get("evidence_refs") or []),
        "feature_ids": feature_ids,
        # Script-owned provenance.  Agent-supplied values with these names are ignored.
        "origin_function_key": str(item.identity),
        "origin_stable_id": origin_stable,
        "origin_file": str(item.metadata.get("file") or ""),
        "origin_variant": variant,
        "origin_method_result_sha256": method_result_sha,
        "created_from_stage": "methods",
    }
    row["duplicate_group_id"] = "REQG-" + _request_signature(row)
    return row


def _normalize_risk_proposal(
    proposal: Any,
    *,
    method_obj: Mapping[str, Any],
    item: WorkItem,
    variant: str,
    method_result_sha: str,
    index: int,
) -> dict[str, Any] | None:
    if isinstance(proposal, str):
        proposal = {"title": proposal, "condition": proposal, "consequence": "unknown", "scope_hint": "file_feature"}
    if not isinstance(proposal, Mapping):
        return None
    title = str(proposal.get("title") or "").strip()
    condition = str(proposal.get("condition") or proposal.get("trigger") or "").strip()
    consequence = str(proposal.get("consequence") or proposal.get("impact_description") or "").strip()
    if not title and not condition:
        return None
    hint = str(proposal.get("scope_hint") or proposal.get("scope") or "file_feature").lower()
    scope = "project" if hint == "project" else "file_feature"
    feature_ids = sorted(set(_method_feature_ids(method_obj) + [str(x) for x in (proposal.get("feature_ids") or []) if str(x).strip()]))
    likelihood = str(proposal.get("likelihood") or "L3").upper()
    impact = str(proposal.get("impact") or "I3").upper()
    if likelihood not in LIKELIHOOD_ORDER: likelihood = "L3"
    if impact not in IMPACT_ORDER: impact = "I3"
    origin_stable = str(item.metadata.get("stable_id") or item.identity)
    local_id = str(proposal.get("local_id") or f"K{index+1}")
    risk_id = "RSK-" + sha256_text("\x1f".join([origin_stable, local_id, title, condition, consequence]))[:16]
    row = {
        "risk_id": risk_id,
        "local_id": local_id,
        "scope": scope,
        "scope_hint": hint,
        "title": title or condition,
        "condition": condition,
        "consequence": consequence,
        "likelihood": likelihood,
        "impact": impact,
        "risk_score": LIKELIHOOD_ORDER[likelihood] * IMPACT_ORDER[impact],
        "blocking": bool(proposal.get("blocking", False)),
        "mitigation": str(proposal.get("mitigation") or ""),
        "detection": str(proposal.get("detection") or ""),
        "trigger": str(proposal.get("trigger") or ""),
        "evidence_refs": list(proposal.get("evidence_refs") or []),
        "feature_ids": feature_ids,
        "origin_function_key": str(item.identity),
        "origin_stable_id": origin_stable,
        "origin_file": str(item.metadata.get("file") or ""),
        "origin_variant": variant,
        "origin_method_result_sha256": method_result_sha,
        "created_from_stage": "methods",
    }
    row["duplicate_group_id"] = "RSKG-" + _risk_signature(row)
    return row


def build_registries(root: Path, baseline: dict[str, Any], static: dict[str, list[WorkItem]], policy: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    variant = _variant_label(root)
    file_req: list[dict[str, Any]] = []
    proj_req: list[dict[str, Any]] = []
    file_risk: list[dict[str, Any]] = []
    proj_risk: list[dict[str, Any]] = []
    for item in static.get("methods", []):
        obj = load_valid_result(root, "methods", item, baseline)
        if not obj:
            continue
        rsha = _result_sha(root, "methods", item.item_id) or ""
        for idx, p in enumerate(obj.get("port_requests") or []):
            row = _normalize_request_proposal(p, method_obj=obj, item=item, variant=variant, method_result_sha=rsha, policy=policy, index=idx)
            if row:
                (proj_req if row["scope"] == "project" else file_req).append(row)
        for idx, p in enumerate(obj.get("risks") or []):
            row = _normalize_risk_proposal(p, method_obj=obj, item=item, variant=variant, method_result_sha=rsha, index=idx)
            if row:
                (proj_risk if row["scope"] == "project" else file_risk).append(row)

    def uniq(rows: list[dict[str, Any]], key: str) -> list[dict[str, Any]]:
        out: dict[str, dict[str, Any]] = {}
        for r in rows: out[str(r[key])] = r
        return sorted(out.values(), key=lambda x: (x.get("origin_file") or "", x[key]))

    regs = {
        "file_scope_requests": uniq(file_req, "request_id"),
        "project_scope_requests": uniq(proj_req, "request_id"),
        "file_feature_risks": uniq(file_risk, "risk_id"),
        "project_scope_risks": uniq(proj_risk, "risk_id"),
    }
    d = root / "orchestration" / "registries"
    atomic_write_jsonl(d / "file_scope_requests.jsonl", regs["file_scope_requests"])
    atomic_write_jsonl(d / "project_scope_requests.jsonl", regs["project_scope_requests"])
    atomic_write_jsonl(d / "file_feature_risks.jsonl", regs["file_feature_risks"])
    atomic_write_jsonl(d / "project_scope_risks.jsonl", regs["project_scope_risks"])
    atomic_write_json(d / "summary.json", {k: len(v) for k, v in regs.items()})
    return regs


def load_registries(root: Path) -> dict[str, list[dict[str, Any]]]:
    d = root / "orchestration" / "registries"
    return {
        "file_scope_requests": list(iter_jsonl(d / "file_scope_requests.jsonl")),
        "project_scope_requests": list(iter_jsonl(d / "project_scope_requests.jsonl")),
        "file_feature_risks": list(iter_jsonl(d / "file_feature_risks.jsonl")),
        "project_scope_risks": list(iter_jsonl(d / "project_scope_risks.jsonl")),
    }


def file_method_feature_ids(root: Path, file_name: str, baseline: dict[str, Any], static: dict[str, list[WorkItem]]) -> set[str]:
    out: set[str] = set()
    for m in static.get("methods", []):
        if str(m.metadata.get("file") or "") != file_name:
            continue
        obj = load_valid_result(root, "methods", m, baseline)
        if obj:
            out.update(str(x) for x in (obj.get("feature_ids") or []) if str(x).strip())
    return out


def registry_subset_for_file(file_name: str, method_feature_ids: set[str], registries: Mapping[str, Sequence[dict[str, Any]]]) -> dict[str, Any]:
    own_requests = [r for k in ("file_scope_requests", "project_scope_requests") for r in registries.get(k, []) if r.get("origin_file") == file_name]
    own_risks = [r for k in ("file_feature_risks", "project_scope_risks") for r in registries.get(k, []) if r.get("origin_file") == file_name]
    cross_requests = [r for r in registries.get("project_scope_requests", [])
                      if r.get("origin_file") != file_name and method_feature_ids.intersection(set(r.get("feature_ids") or []))]
    cross_risks = [r for r in registries.get("project_scope_risks", [])
                   if r.get("origin_file") != file_name and method_feature_ids.intersection(set(r.get("feature_ids") or []))]
    return {
        "decision_required_requests": own_requests,
        "decision_required_risks": own_risks,
        "cross_file_project_requests_advisory": cross_requests,
        "cross_file_project_risks_advisory": cross_risks,
    }


def validate_file_registry_decisions(obj: Mapping[str, Any], file_name: str, subset: Mapping[str, Any], policy: Mapping[str, Any]) -> list[str]:
    if not policy.get("registries", {}).get("require_file_decisions", True):
        return []
    errors: list[str] = []
    req_expected = {r["request_id"]: r for r in subset.get("decision_required_requests") or []}
    risk_expected = {r["risk_id"]: r for r in subset.get("decision_required_risks") or []}
    req_rows = obj.get("request_decisions") or []
    risk_rows = obj.get("risk_decisions") or []
    req_seen: dict[str, Mapping[str, Any]] = {}
    risk_seen: dict[str, Mapping[str, Any]] = {}
    for x in req_rows:
        if not isinstance(x, Mapping):
            errors.append("request_decisions entries must be objects")
            continue
        rid = str(x.get("request_id") or "")
        if rid: req_seen[rid] = x
    for x in risk_rows:
        if not isinstance(x, Mapping):
            errors.append("risk_decisions entries must be objects")
            continue
        rid = str(x.get("risk_id") or "")
        if rid: risk_seen[rid] = x
    missing = sorted(set(req_expected) - set(req_seen))
    if missing: errors.append(f"file {file_name}: missing request decisions for {missing}")
    missing = sorted(set(risk_expected) - set(risk_seen))
    if missing: errors.append(f"file {file_name}: missing risk decisions for {missing}")
    allowed_req = set(policy.get("registries", {}).get("request_decisions") or [])
    allowed_risk = set(policy.get("registries", {}).get("risk_decisions") or [])
    for rid, x in req_seen.items():
        if rid not in req_expected:
            errors.append(f"unknown/non-owned request_id in file decision: {rid}")
        if str(x.get("decision") or "") not in allowed_req:
            errors.append(f"request {rid}: invalid decision {x.get('decision')!r}")
        if not str(x.get("rationale") or "").strip():
            errors.append(f"request {rid}: rationale is required")
    for rid, x in risk_seen.items():
        if rid not in risk_expected:
            errors.append(f"unknown/non-owned risk_id in file decision: {rid}")
        if str(x.get("decision") or "") not in allowed_risk:
            errors.append(f"risk {rid}: invalid decision {x.get('decision')!r}")
        if not str(x.get("rationale") or "").strip():
            errors.append(f"risk {rid}: rationale is required")
    return errors


def project_registry_resolution(root: Path, baseline: dict[str, Any], static: dict[str, list[WorkItem]], registries: Mapping[str, Sequence[dict[str, Any]]]) -> dict[str, Any]:
    """Resolve project work after file-porters have triaged method proposals.

    Method agents can hint at project scope, but a file porter is also allowed to
    discover that an originally file-scoped proposal must be promoted.  Such
    ESCALATE_PROJECT decisions are materialized here and cannot disappear from
    final synthesis.
    """
    file_results: dict[str, dict[str, Any]] = {}
    for f in static.get("files", []):
        obj = load_valid_result(root, "files", f, baseline)
        if obj: file_results[str(f.identity)] = obj

    def file_decisions_for(row: Mapping[str, Any], field: str, id_field: str) -> list[dict[str, Any]]:
        origin = str(row.get("origin_file") or "")
        obj = file_results.get(origin)
        if not obj: return []
        rid = row.get(id_field)
        return [dict(x) for x in (obj.get(field) or []) if isinstance(x, Mapping) and x.get(id_field) == rid]

    reqs: list[dict[str, Any]] = []
    for r0 in list(registries.get("project_scope_requests", [])) + list(registries.get("file_scope_requests", [])):
        r = dict(r0)
        decisions = file_decisions_for(r, "request_decisions", "request_id")
        originally_project = r.get("scope") == "project"
        escalated = any(str(x.get("decision") or "") == "ESCALATE_PROJECT" for x in decisions)
        if not originally_project and not escalated:
            continue
        state = "PENDING_PROJECT"
        if decisions:
            d = str(decisions[-1].get("decision") or "")
            if d in {"REJECT", "DUPLICATE", "NOT_APPLICABLE"}: state = "CLOSED_AT_FILE"
            elif d == "NEEDS_EVIDENCE": state = "NEEDS_EVIDENCE"
            elif d == "DEFER": state = "DEFERRED_AT_FILE"
            elif d == "ESCALATE_PROJECT" or originally_project: state = "ENDORSED_FOR_PROJECT"
            elif d in {"IMPLEMENT_IN_FILE", "ADOPT_DESIGN"}: state = "RESOLVED_IN_FILE"
        r["effective_project_scope"] = True
        if not originally_project:
            r["promoted_from_scope"] = r.get("scope") or "file"
            r["promotion_decision"] = decisions[-1] if decisions else None
        reqs.append({**r, "file_decisions": decisions, "project_state": state})

    risks: list[dict[str, Any]] = []
    for r0 in list(registries.get("project_scope_risks", [])) + list(registries.get("file_feature_risks", [])):
        r = dict(r0)
        decisions = file_decisions_for(r, "risk_decisions", "risk_id")
        originally_project = r.get("scope") == "project"
        escalated = any(str(x.get("decision") or "") == "ESCALATE_PROJECT" for x in decisions)
        if not originally_project and not escalated:
            continue
        state = "PENDING_PROJECT"
        if decisions:
            d = str(decisions[-1].get("decision") or "")
            if d in {"REJECT_INVALID", "DUPLICATE"}: state = "CLOSED_AT_FILE"
            elif d == "NEEDS_EVIDENCE": state = "NEEDS_EVIDENCE"
            elif d == "DEFER": state = "DEFERRED_AT_FILE"
            elif d == "ESCALATE_PROJECT" or originally_project: state = "ENDORSED_FOR_PROJECT"
            elif d in {"MITIGATE_IN_FILE", "ACCEPT", "TRANSFER"}: state = "RESOLVED_IN_FILE"
        r["effective_project_scope"] = True
        if not originally_project:
            r["promoted_from_scope"] = r.get("scope") or "file_feature"
            r["promotion_decision"] = decisions[-1] if decisions else None
        risks.append({**r, "file_decisions": decisions, "project_state": state})

    # Deduplicate defensively by script-owned ID while preserving promoted form.
    req_by_id = {str(x.get("request_id")): x for x in reqs}
    risk_by_id = {str(x.get("risk_id")): x for x in risks}
    return {
        "project_requests": sorted(req_by_id.values(), key=lambda x: str(x.get("request_id"))),
        "project_risks": sorted(risk_by_id.values(), key=lambda x: str(x.get("risk_id"))),
    }


def write_registry_resolution_snapshot(root: Path, baseline: dict[str, Any], static: dict[str, list[WorkItem]], registries: Mapping[str, Sequence[dict[str, Any]]]) -> dict[str, Any]:
    """Persist a single audit view of every request/risk and its current disposition."""
    file_results: dict[str, dict[str, Any]] = {}
    for f in static.get("files", []):
        obj = load_valid_result(root, "files", f, baseline)
        if obj: file_results[str(f.identity)] = obj

    request_rows: list[dict[str, Any]] = []
    for kind in ("file_scope_requests", "project_scope_requests"):
        for r0 in registries.get(kind, []):
            r = dict(r0); origin = str(r.get("origin_file") or "")
            obj = file_results.get(origin)
            decisions = [dict(x) for x in (obj.get("request_decisions") or []) if isinstance(x, Mapping) and x.get("request_id") == r.get("request_id")] if obj else []
            d = str(decisions[-1].get("decision") or "") if decisions else ""
            state = "PENDING_FILE"
            if d in {"IMPLEMENT_IN_FILE", "ADOPT_DESIGN"}: state = "RESOLVED_IN_FILE"
            elif d == "ESCALATE_PROJECT": state = "ESCALATED_PROJECT"
            elif d in {"REJECT", "DUPLICATE", "NOT_APPLICABLE"}: state = "CLOSED_AT_FILE"
            elif d == "DEFER": state = "DEFERRED_AT_FILE"
            elif d == "NEEDS_EVIDENCE": state = "NEEDS_EVIDENCE"
            elif r.get("scope") == "project" and decisions: state = "ENDORSED_FOR_PROJECT"
            request_rows.append({**r, "registry_kind": kind, "file_decisions": decisions, "resolution_state": state})

    risk_rows: list[dict[str, Any]] = []
    for kind in ("file_feature_risks", "project_scope_risks"):
        for r0 in registries.get(kind, []):
            r = dict(r0); origin = str(r.get("origin_file") or "")
            obj = file_results.get(origin)
            decisions = [dict(x) for x in (obj.get("risk_decisions") or []) if isinstance(x, Mapping) and x.get("risk_id") == r.get("risk_id")] if obj else []
            d = str(decisions[-1].get("decision") or "") if decisions else ""
            state = "PENDING_FILE"
            if d in {"MITIGATE_IN_FILE", "ACCEPT", "TRANSFER"}: state = "RESOLVED_IN_FILE"
            elif d == "ESCALATE_PROJECT": state = "ESCALATED_PROJECT"
            elif d in {"REJECT_INVALID", "DUPLICATE"}: state = "CLOSED_AT_FILE"
            elif d == "DEFER": state = "DEFERRED_AT_FILE"
            elif d == "NEEDS_EVIDENCE": state = "NEEDS_EVIDENCE"
            elif r.get("scope") == "project" and decisions: state = "ENDORSED_FOR_PROJECT"
            risk_rows.append({**r, "registry_kind": kind, "file_decisions": decisions, "resolution_state": state})

    project = project_registry_resolution(root, baseline, static, registries)
    final_obj = None
    if static.get("final"):
        final_obj = load_valid_result(root, "final", static["final"][0], baseline)
    final_req = {str(x.get("request_id")): dict(x) for x in ((final_obj or {}).get("project_request_decisions") or []) if isinstance(x, Mapping) and x.get("request_id")}
    final_risk = {str(x.get("risk_id")): dict(x) for x in ((final_obj or {}).get("project_risk_decisions") or []) if isinstance(x, Mapping) and x.get("risk_id")}
    for x in project.get("project_requests") or []:
        x["final_decision"] = final_req.get(str(x.get("request_id")))
    for x in project.get("project_risks") or []:
        x["final_decision"] = final_risk.get(str(x.get("risk_id")))

    snapshot = {
        "generated_utc": now_iso(),
        "requests": sorted(request_rows, key=lambda x: str(x.get("request_id"))),
        "risks": sorted(risk_rows, key=lambda x: str(x.get("risk_id"))),
        "project_resolution": project,
        "counts": {
            "requests": len(request_rows), "risks": len(risk_rows),
            "pending_file_requests": sum(x["resolution_state"] == "PENDING_FILE" for x in request_rows),
            "pending_file_risks": sum(x["resolution_state"] == "PENDING_FILE" for x in risk_rows),
            "project_requests": len(project.get("project_requests") or []),
            "project_risks": len(project.get("project_risks") or []),
        },
    }
    atomic_write_json(root / "orchestration" / "registries" / "resolution.json", snapshot)
    return snapshot


def validate_final_registry_decisions(obj: Mapping[str, Any], project_registry: Mapping[str, Any], policy: Mapping[str, Any]) -> list[str]:
    errors: list[str] = []
    req_expected = {x["request_id"] for x in project_registry.get("project_requests") or [] if x.get("project_state") in {"ENDORSED_FOR_PROJECT", "PENDING_PROJECT", "NEEDS_EVIDENCE"}}
    risk_expected = {x["risk_id"] for x in project_registry.get("project_risks") or [] if x.get("project_state") in {"ENDORSED_FOR_PROJECT", "PENDING_PROJECT", "NEEDS_EVIDENCE"}}
    req_rows = [x for x in (obj.get("project_request_decisions") or []) if isinstance(x, Mapping)]
    risk_rows = [x for x in (obj.get("project_risk_decisions") or []) if isinstance(x, Mapping)]
    req_seen = {str(x.get("request_id")): x for x in req_rows if x.get("request_id")}
    risk_seen = {str(x.get("risk_id")): x for x in risk_rows if x.get("risk_id")}
    if req_expected - set(req_seen): errors.append(f"missing final project request decisions: {sorted(req_expected - set(req_seen))}")
    if risk_expected - set(risk_seen): errors.append(f"missing final project risk decisions: {sorted(risk_expected - set(risk_seen))}")
    ar = set(policy.get("registries", {}).get("final_project_request_decisions") or [])
    ak = set(policy.get("registries", {}).get("final_project_risk_decisions") or [])
    for rid, x in req_seen.items():
        if rid not in req_expected: errors.append(f"unknown/non-actionable project request decision {rid}")
        if str(x.get("decision") or "") not in ar: errors.append(f"project request {rid}: invalid decision")
        if not str(x.get("rationale") or "").strip(): errors.append(f"project request {rid}: rationale required")
    for rid, x in risk_seen.items():
        if rid not in risk_expected: errors.append(f"unknown/non-actionable project risk decision {rid}")
        if str(x.get("decision") or "") not in ak: errors.append(f"project risk {rid}: invalid decision")
        if not str(x.get("rationale") or "").strip(): errors.append(f"project risk {rid}: rationale required")
    return errors


# ---- Specification ingestion ------------------------------------------------

def _slug(s: str) -> str:
    s = re.sub(r"[^A-Za-z0-9_.-]+", "-", s.strip()).strip("-")
    return s or "clause"


def ingest_spec_manifest(root: Path, manifest_path: Path) -> dict[str, Any]:
    manifest = load_json(manifest_path, None)
    if not isinstance(manifest, Mapping) or not isinstance(manifest.get("documents"), list):
        raise ValueError("spec manifest must be JSON object with documents[]")
    clauses: list[dict[str, Any]] = []
    docs: list[dict[str, Any]] = []
    base = manifest_path.parent
    for d in manifest["documents"]:
        if not isinstance(d, Mapping): continue
        doc_id = str(d.get("document_id") or "").strip()
        if not doc_id: raise ValueError("every specification document requires document_id")
        doc = dict(d)
        doc["document_id"] = doc_id
        if isinstance(d.get("clauses"), list):
            raw_clauses = d["clauses"]
        else:
            path = Path(str(d.get("path") or ""))
            if not path.is_absolute(): path = (base / path).resolve()
            text = path.read_text(encoding="utf-8", errors="replace")
            doc["resolved_path"] = str(path)
            doc["sha256"] = sha256_file(path)
            raw_clauses = []
            headings: list[tuple[int, int, str, str | None]] = []
            pending_explicit: str | None = None
            lines = text.splitlines()
            for i, line in enumerate(lines, 1):
                m_id = re.search(r"(?:SPEC-ID\s*:\s*|spec-id\s*=\s*[\"']?)([A-Za-z0-9_.:/-]+)", line, re.I)
                if m_id:
                    pending_explicit = m_id.group(1)
                m = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
                if not m:
                    m = re.match(r"^\s*((?:\d+\.)+\d*|\d+)\s+(.+?)\s*$", line)
                    if m: level, title = max(1, m.group(1).count(".")), m.group(2)
                    else: continue
                else:
                    level, title = len(m.group(1)), m.group(2)
                headings.append((i, int(level), str(title), pending_explicit))
                pending_explicit = None
            stack: list[tuple[int, str]] = []
            for idx, (line_no, level, title, explicit) in enumerate(headings):
                while stack and stack[-1][0] >= level: stack.pop()
                stack.append((level, title))
                end = (headings[idx+1][0] - 1) if idx + 1 < len(headings) else len(lines)
                cid = explicit or "::".join(_slug(x[1]) for x in stack)
                raw_clauses.append({
                    "clause_id": cid, "title": title,
                    "text": "\n".join(lines[line_no-1:end]).strip(),
                    "line_start": line_no, "line_end": end,
                })
        for idx, c in enumerate(raw_clauses):
            if not isinstance(c, Mapping): continue
            local = str(c.get("clause_id") or c.get("id") or f"clause-{idx+1}")
            stable = local if local.startswith("SPEC::") else f"SPEC::{doc_id}::{local}"
            text = str(c.get("text") or "")
            clauses.append({
                "spec_clause_id": stable,
                "document_id": doc_id,
                "local_clause_id": local,
                "title": str(c.get("title") or local),
                "text": text,
                "text_sha256": sha256_text(text),
                "line_start": c.get("line_start"), "line_end": c.get("line_end"),
                "source_version": d.get("version"),
                "authority_class": str(d.get("authority_class") or "A5"),
            })
        docs.append(doc)
    tools = normalize_capabilities(manifest.get("mcp-and-tools-list") or manifest.get("mcp_and_tools_list") or [], base, "tool")
    skills = normalize_capabilities(manifest.get("skills") or [], base, "skill")
    sd = root / "orchestration" / "specifications"
    atomic_write_json(sd / "manifest.json", {
        "documents": docs, "ingested_utc": now_iso(), "source_manifest": str(manifest_path),
        "mcp_and_tools_list": tools, "skills": skills,
    })
    atomic_write_jsonl(sd / "clauses.jsonl", clauses)
    atomic_write_jsonl(sd / "mcp_and_tools.jsonl", tools)
    atomic_write_jsonl(sd / "skills.jsonl", skills)
    atomic_write_json(sd / "summary.json", {
        "documents": len(docs), "clauses": len(clauses),
        "mcp_and_tools": len(tools), "skills": len(skills),
    })
    return {"documents": len(docs), "clauses": len(clauses), "mcp_and_tools": len(tools), "skills": len(skills)}


def normalize_capabilities(entries: Any, base: Path, kind: str) -> list[dict[str, Any]]:
    """Normalize authorized MCP/tool and skill entries into stable capability records.

    ``path`` may be a concrete file or a selector pattern such as ``gitnexus/*``;
    only concrete files are resolved and hashed so a pattern stays declarative.
    """
    prefix = "TOOL" if kind == "tool" else "SKILL"
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()
    for idx, entry in enumerate(entries if isinstance(entries, list) else []):
        if isinstance(entry, str):
            entry = {"document_id": entry, "path": entry}
        if not isinstance(entry, Mapping):
            continue
        local = str(entry.get("document_id") or entry.get("id") or entry.get("name") or f"{kind}-{idx+1}").strip()
        capability_id = local if local.startswith(f"{prefix}::") else f"{prefix}::{_slug(local)}"
        if capability_id in seen:
            raise ValueError(f"duplicate {kind} capability id: {capability_id}")
        seen.add(capability_id)
        raw_path = str(entry.get("path") or "").strip()
        row: dict[str, Any] = {
            "capability_id": capability_id,
            "kind": kind,
            "document_id": local,
            "title": str(entry.get("title") or local),
            "description": str(entry.get("description") or ""),
            "version": entry.get("version"),
            "authority_class": str(entry.get("authority_class") or "A5"),
            "path": raw_path,
            "selector": bool(raw_path) and any(ch in raw_path for ch in "*?["),
            "resolved_path": None,
            "sha256": None,
        }
        if raw_path and not row["selector"]:
            p = Path(raw_path)
            if not p.is_absolute():
                p = (base / p).resolve()
            row["resolved_path"] = str(p)
            row["available"] = p.is_file()
            if row["available"]:
                row["sha256"] = sha256_file(p)
        else:
            row["available"] = bool(raw_path)
        rows.append(row)
    rows.sort(key=lambda x: x["capability_id"])
    return rows


def load_spec_clauses(root: Path) -> list[dict[str, Any]]:
    return list(iter_jsonl(root / "orchestration" / "specifications" / "clauses.jsonl"))


def load_authorized_tools(root: Path) -> list[dict[str, Any]]:
    return list(iter_jsonl(root / "orchestration" / "specifications" / "mcp_and_tools.jsonl"))


def load_authorized_skills(root: Path) -> list[dict[str, Any]]:
    return list(iter_jsonl(root / "orchestration" / "specifications" / "skills.jsonl"))


def capability_context(root: Path) -> dict[str, Any]:
    return {
        "authorized_mcp_and_tools": [
            {k: x.get(k) for k in ("capability_id", "document_id", "title", "path", "selector", "version", "authority_class", "description")}
            for x in load_authorized_tools(root)
        ],
        "authorized_skills": [
            {k: x.get(k) for k in ("capability_id", "document_id", "title", "path", "selector", "version", "authority_class", "description")}
            for x in load_authorized_skills(root)
        ],
    }


def add_capability_section(c: "PromptComposer", root: Path, limit: int = 20000) -> None:
    ctx = capability_context(root)
    if not ctx["authorized_mcp_and_tools"] and not ctx["authorized_skills"]:
        return
    c.add(
        "Authorized MCP servers, tools and skills — use only these capabilities for research and verification",
        "```json\n" + pretty(ctx, limit) + "\n```",
    )


# ---- Evidence citation verification ----------------------------------------

def _authority_class(source: Mapping[str, Any], policy: Mapping[str, Any]) -> str:
    explicit = str(source.get("authority_class") or source.get("authority") or "").upper()
    if explicit in AUTHORITY_ORDER: return explicit
    st = str(source.get("source_type") or source.get("kind") or "unknown").lower()
    classes = policy.get("evidence_verification", {}).get("authority_classes") or {}
    for a, names in classes.items():
        if st in set(str(x).lower() for x in names): return str(a)
    ref = str(source.get("url") or source.get("url_or_reference") or "").lower()
    if any(x in ref for x in ("kernel.org", "freebsd.org", "llvm.org", "github.com/torvalds/linux", "github.com/freebsd/freebsd-src")):
        return "A4"
    return "A0"


def verify_evidence_sources(root: Path, baseline: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]], policy: dict[str, Any], online: bool = False) -> dict[str, Any]:
    from urllib.parse import urlparse
    import urllib.request
    clauses = {x.get("spec_clause_id"): x for x in load_spec_clauses(root)}
    report_rows: list[dict[str, Any]] = []
    for row in evidence_rows:
        item = WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=dict(row))
        obj = load_valid_result(root, "evidence", item, baseline)
        if not obj:
            report_rows.append({"evidence_id": row["evidence_id"], "effective_verified": False, "reason": "missing_or_stale_result", "sources": []})
            continue
        verified_sources = []
        for idx, src0 in enumerate(obj.get("sources") or []):
            src = dict(src0) if isinstance(src0, Mapping) else {"url_or_reference": str(src0)}
            authority = _authority_class(src, policy)
            locator = src.get("locator") or src.get("section") or src.get("lines") or src.get("commit") or src.get("spec_clause_id")
            structurally_verified = False
            reachable: bool | None = None
            ref_kind = "unknown"
            detail = ""
            if src.get("spec_clause_id"):
                ref_kind = "spec_clause"
                structurally_verified = src.get("spec_clause_id") in clauses
                detail = "known specification clause" if structurally_verified else "unknown specification clause"
            elif src.get("local_path"):
                ref_kind = "local_file"
                lp = Path(str(src["local_path"]))
                if not lp.is_absolute(): lp = (root / lp).resolve()
                structurally_verified = lp.is_file()
                if structurally_verified and src.get("content_sha256"):
                    structurally_verified = sha256_file(lp) == src.get("content_sha256")
                detail = str(lp)
            else:
                ref = str(src.get("url") or src.get("url_or_reference") or "")
                pr = urlparse(ref)
                if pr.scheme in {"http", "https"} and pr.netloc:
                    ref_kind = "url"
                    structurally_verified = True
                    if online:
                        try:
                            req = urllib.request.Request(ref, method="HEAD", headers={"User-Agent": "nic-port-orchestrator/2"})
                            with urllib.request.urlopen(req, timeout=int(policy.get("evidence_verification", {}).get("online_timeout_seconds", 8))) as resp:
                                reachable = 200 <= int(getattr(resp, "status", 200)) < 400
                        except Exception:
                            reachable = False
                else:
                    ref_kind = "reference"
                    structurally_verified = bool(ref.strip())
            if policy.get("evidence_verification", {}).get("require_locator", True) and not locator:
                structurally_verified = False
                detail = (detail + "; " if detail else "") + "missing locator"
            verified_sources.append({
                "index": idx, "authority_class": authority,
                "authority_score": AUTHORITY_ORDER.get(authority, 0),
                "reference_kind": ref_kind,
                "structurally_verified": structurally_verified,
                "reachable": reachable, "detail": detail,
            })
        threshold = str(policy.get("evidence_verification", {}).get(
            "minimum_critical_authority" if row.get("critical") else "minimum_verified_authority", "A3"))
        best = max([x["authority_score"] for x in verified_sources if x["structurally_verified"]] or [0])
        effective = obj.get("status") == "VERIFIED" and best >= AUTHORITY_ORDER.get(threshold, 3)
        report_rows.append({
            "evidence_id": row["evidence_id"], "critical": bool(row.get("critical")),
            "declared_status": obj.get("status"), "declared_confidence": obj.get("confidence"),
            "required_authority": threshold, "best_authority_score": best,
            "effective_verified": effective, "sources": verified_sources,
        })
    report = {
        "generated_utc": now_iso(), "online_checked": online,
        "verified": sum(1 for x in report_rows if x["effective_verified"]),
        "total": len(report_rows), "items": report_rows,
    }
    atomic_write_json(root / "orchestration" / "evidence" / "citation_verification.json", report)
    return report


# ---- Feature/test traceability ---------------------------------------------

def _feature_id_from_entry(entry: Any) -> str | None:
    if isinstance(entry, str): return entry.strip() or None
    if isinstance(entry, Mapping):
        for k in ("feature_id", "id", "feature"):
            if entry.get(k): return str(entry[k])
    return None


def _test_record(stage: str, origin: str, test: Any, default_features: Sequence[str], index: int) -> dict[str, Any] | None:
    if isinstance(test, str):
        obj = {"title": test, "objective": test}
    elif isinstance(test, Mapping):
        obj = dict(test)
    else:
        return None
    title = str(obj.get("title") or obj.get("name") or obj.get("objective") or obj.get("description") or "").strip()
    if not title: return None
    local = str(obj.get("test_id") or obj.get("local_id") or "")
    tid = local if local.startswith("TEST-") else "TEST-" + sha256_text("\x1f".join([stage, origin, local or str(index), title]))[:16]
    features = sorted(set(str(x) for x in list(default_features) + list(obj.get("feature_ids") or []) if str(x).strip()))
    return {
        "test_id": tid, "stage": stage, "origin": origin,
        "title": title, "objective": str(obj.get("objective") or title),
        "test_type": str(obj.get("test_type") or obj.get("type") or "unspecified"),
        "level": str(obj.get("level") or stage.rstrip("s")),
        "feature_ids": features,
        "spec_clause_ids": sorted(set(str(x) for x in (obj.get("spec_clause_ids") or []) if str(x).strip())),
        "preconditions": list(obj.get("preconditions") or []),
        "procedure": obj.get("procedure") or obj.get("steps") or [],
        "expected_result": obj.get("expected_result") or obj.get("expected") or "",
        "automation": str(obj.get("automation") or obj.get("automation_level") or "TBD"),
    }


def build_traceability(root: Path, baseline: dict[str, Any], static: dict[str, list[WorkItem]], registries: Mapping[str, Sequence[dict[str, Any]]]) -> dict[str, Any]:
    features: dict[str, dict[str, Any]] = {}
    links: list[dict[str, Any]] = []
    tests: dict[str, dict[str, Any]] = {}
    test_conflicts: list[dict[str, Any]] = []
    clauses = {x.get("spec_clause_id"): x for x in load_spec_clauses(root)}

    def register_test(tr: dict[str, Any]) -> dict[str, Any]:
        tid = str(tr["test_id"])
        existing = tests.get(tid)
        if existing is None:
            tr = dict(tr)
            tr["origins"] = [tr.get("origin")] if tr.get("origin") else []
            tests[tid] = tr
            return tr
        semantic_keys = ("title", "objective", "test_type", "level", "procedure", "expected_result")
        conflicts = [k for k in semantic_keys if existing.get(k) != tr.get(k) and tr.get(k) not in (None, "", [], {})]
        if conflicts:
            test_conflicts.append({
                "test_id": tid, "fields": conflicts,
                "existing_origin": existing.get("origin"), "new_origin": tr.get("origin"),
                "existing": {k: existing.get(k) for k in conflicts},
                "new": {k: tr.get(k) for k in conflicts},
            })
        existing["origins"] = sorted(set(str(x) for x in list(existing.get("origins") or []) + [tr.get("origin")] if x))
        existing["feature_ids"] = sorted(set(str(x) for x in list(existing.get("feature_ids") or []) + list(tr.get("feature_ids") or []) if x))
        existing["spec_clause_ids"] = sorted(set(str(x) for x in list(existing.get("spec_clause_ids") or []) + list(tr.get("spec_clause_ids") or []) if x))
        return existing

    arch = load_valid_result(root, "architecture", static["architecture"][0], baseline)
    if arch:
        for e in arch.get("feature_model") or []:
            fid = _feature_id_from_entry(e)
            if not fid: continue
            rec = features.setdefault(fid, {"feature_id": fid, "sources": [], "spec_clause_ids": [], "priority": None})
            rec["sources"].append("architecture")
            if isinstance(e, Mapping):
                rec["title"] = e.get("title") or e.get("name") or rec.get("title")
                rec["priority"] = e.get("priority") or rec.get("priority")
                for cid in e.get("spec_clause_ids") or []:
                    if cid in clauses:
                        rec["spec_clause_ids"].append(cid)
                        links.append({"from_type": "feature", "from_id": fid, "relation": "specified_by", "to_type": "spec_clause", "to_id": cid})

    def add_feature(fid: str, source: str) -> None:
        rec = features.setdefault(fid, {"feature_id": fid, "sources": [], "spec_clause_ids": [], "priority": None})
        if source not in rec["sources"]: rec["sources"].append(source)

    for stage in ("methods", "files", "subsystems"):
        for item in static.get(stage, []):
            obj = load_valid_result(root, stage, item, baseline)
            if not obj: continue
            fids = sorted(set(str(x) for x in (obj.get("feature_ids") or []) if str(x).strip()))
            for fid in fids:
                add_feature(fid, f"{stage}:{item.identity}")
                links.append({"from_type": "feature", "from_id": fid, "relation": {"methods":"implemented_by_source_function","files":"integrated_by_file","subsystems":"owned_by_subsystem"}[stage], "to_type": stage.rstrip("s"), "to_id": str(item.identity)})
            for idx, t in enumerate(obj.get("tests") or []):
                tr = _test_record(stage, str(item.identity), t, fids, idx)
                if tr:
                    tr = register_test(tr)
                    for fid in tr["feature_ids"]:
                        add_feature(fid, f"test:{tr['test_id']}")
                        links.append({"from_type": "feature", "from_id": fid, "relation": "verified_by", "to_type": "test", "to_id": tr["test_id"]})
                    for cid in tr.get("spec_clause_ids") or []:
                        if cid in clauses:
                            links.append({"from_type": "test", "from_id": tr["test_id"], "relation": "verifies_spec_clause", "to_type": "spec_clause", "to_id": cid})
            if stage == "methods":
                for cid in obj.get("spec_clause_ids") or []:
                    if cid in clauses:
                        for fid in fids:
                            links.append({"from_type": "feature", "from_id": fid, "relation": "specified_by", "to_type": "spec_clause", "to_id": cid})

    for kind in ("file_scope_requests", "project_scope_requests"):
        for r in registries.get(kind, []):
            for fid in r.get("feature_ids") or []:
                add_feature(str(fid), f"request:{r['request_id']}")
                links.append({"from_type": "feature", "from_id": str(fid), "relation": "has_port_request", "to_type": "request", "to_id": r["request_id"]})
    for kind in ("file_feature_risks", "project_scope_risks"):
        for r in registries.get(kind, []):
            for fid in r.get("feature_ids") or []:
                add_feature(str(fid), f"risk:{r['risk_id']}")
                links.append({"from_type": "feature", "from_id": str(fid), "relation": "has_risk", "to_type": "risk", "to_id": r["risk_id"]})

    for rec in features.values():
        rec["sources"] = sorted(set(rec.get("sources") or []))
        rec["spec_clause_ids"] = sorted(set(rec.get("spec_clause_ids") or []))
    td = root / "orchestration" / "traceability"
    atomic_write_jsonl(td / "features.jsonl", sorted(features.values(), key=lambda x: x["feature_id"]))
    atomic_write_jsonl(td / "tests.jsonl", sorted(tests.values(), key=lambda x: x["test_id"]))
    atomic_write_jsonl(td / "links.jsonl", sorted(links, key=lambda x: (x["from_type"], x["from_id"], x["relation"], x["to_id"])))
    coverage = []
    for fid, rec in sorted(features.items()):
        flinks = [x for x in links if x["from_type"] == "feature" and x["from_id"] == fid]
        relations = Counter(x["relation"] for x in flinks)
        coverage.append({
            "feature_id": fid,
            "title": rec.get("title"),
            "priority": rec.get("priority"),
            "has_spec": any(x["relation"] == "specified_by" for x in flinks),
            "has_source_method": any(x["relation"] == "implemented_by_source_function" for x in flinks),
            "has_file_integration": any(x["relation"] == "integrated_by_file" for x in flinks),
            "has_subsystem_owner": any(x["relation"] == "owned_by_subsystem" for x in flinks),
            "has_test": any(x["relation"] == "verified_by" for x in flinks),
            "spec_count": relations.get("specified_by", 0),
            "source_method_count": relations.get("implemented_by_source_function", 0),
            "file_count": relations.get("integrated_by_file", 0),
            "subsystem_count": relations.get("owned_by_subsystem", 0),
            "test_count": relations.get("verified_by", 0),
            "request_count": relations.get("has_port_request", 0),
            "risk_count": relations.get("has_risk", 0),
        })
    trace_policy = load_policy(root).get("traceability", {})
    required_priorities = set(str(x) for x in (trace_policy.get("required_test_priorities") or []))
    required_test_gaps = [
        x["feature_id"] for x in coverage
        if str(x.get("priority") or "") in required_priorities and not x["has_test"]
    ]
    report = {
        "features": len(features), "tests": len(tests), "links": len(links),
        "test_id_conflicts": test_conflicts,
        "test_id_conflict_count": len(test_conflicts),
        "coverage": coverage,
        "features_by_priority": dict(Counter(str(x.get("priority") or "UNSPECIFIED") for x in coverage)),
        "features_without_tests": [x["feature_id"] for x in coverage if not x["has_test"]],
        "features_without_spec_links": [x["feature_id"] for x in coverage if not x["has_spec"]],
        "features_without_source_methods": [x["feature_id"] for x in coverage if not x["has_source_method"]],
        "features_without_file_integration": [x["feature_id"] for x in coverage if x["has_source_method"] and not x["has_file_integration"]],
        "features_without_subsystem_owner": [x["feature_id"] for x in coverage if x["has_file_integration"] and not x["has_subsystem_owner"]],
        "required_test_priorities": sorted(required_priorities),
        "required_test_gaps": required_test_gaps,
    }
    atomic_write_json(td / "coverage.json", report)
    return report


# ---- Architecture consistency ----------------------------------------------

def _as_bool(v: Any) -> bool | None:
    if isinstance(v, bool): return v
    if isinstance(v, str):
        if v.lower() in {"true", "yes", "may", "sleepable"}: return True
        if v.lower() in {"false", "no", "must_not", "non_sleepable"}: return False
    return None


def run_consistency_checks(root: Path, baseline: dict[str, Any], static: dict[str, list[WorkItem]], policy: dict[str, Any]) -> dict[str, Any]:
    findings: list[dict[str, Any]] = []
    lock_classes: dict[str, set[str]] = defaultdict(set)
    lock_methods: dict[str, set[str]] = defaultdict(set)
    lock_edges: set[tuple[str, str]] = set()
    guards: dict[str, dict[str, set[str]]] = defaultdict(lambda: defaultdict(set))
    sleepable_classes = set(policy.get("consistency", {}).get("known_sleepable_lock_classes") or [])
    nonsleepable_classes = set(policy.get("consistency", {}).get("known_nonsleepable_lock_classes") or [])

    for item in static.get("methods", []):
        obj = load_valid_result(root, "methods", item, baseline)
        if not obj: continue
        runtime = obj.get("runtime") or {}
        ctx = " ".join(str(x) for x in (runtime.get("execution_context") if isinstance(runtime.get("execution_context"), list) else [runtime.get("execution_context") or runtime.get("context") or ""])).lower()
        may_sleep = _as_bool(runtime.get("may_sleep"))
        if may_sleep is True and any(x in ctx for x in ("hardirq", "interrupt", "softirq", "napi", "poll non-sleep")):
            findings.append({"finding_id": "CONS-"+sha256_text(str(item.identity)+"sleep")[:12], "severity": "HIGH", "kind": "sleep_context_conflict", "methods": [item.identity], "message": f"{item.identity}: marked may_sleep in non-sleeping context {ctx!r}"})
        sync = obj.get("synchronization") or {}
        for lk in sync.get("locks") or []:
            if isinstance(lk, str): lk = {"name": lk}
            if not isinstance(lk, Mapping): continue
            name = str(lk.get("name") or lk.get("lock") or "").strip()
            cls = str(lk.get("class") or lk.get("type") or lk.get("primitive") or "").strip().lower()
            if not name: continue
            if cls: lock_classes[name].add(cls)
            lock_methods[name].add(str(item.identity))
            for fld in lk.get("protects") or []: guards[str(fld)][name].add(str(item.identity))
        order = sync.get("lock_order") or []
        if isinstance(order, list):
            names = [str(x.get("name") if isinstance(x, Mapping) else x) for x in order if x]
            for a, b in zip(names, names[1:]): lock_edges.add((a, b))
        for g in sync.get("guards") or []:
            if isinstance(g, Mapping):
                lock = str(g.get("lock") or g.get("name") or "")
                for fld in g.get("fields") or g.get("protects") or []:
                    if lock: guards[str(fld)][lock].add(str(item.identity))

    for name, classes in lock_classes.items():
        if classes.intersection(sleepable_classes) and classes.intersection(nonsleepable_classes):
            findings.append({"finding_id": "CONS-"+sha256_text(name+"class")[:12], "severity": "HIGH", "kind": "lock_class_conflict", "lock": name, "methods": sorted(lock_methods[name]), "message": f"lock {name!r} has conflicting sleepable/non-sleepable classes: {sorted(classes)}"})

    # Lock-order cycle detection.
    graph: dict[str, set[str]] = defaultdict(set)
    for a, b in lock_edges:
        if a and b and a != b: graph[a].add(b)
    visiting: set[str] = set(); visited: set[str] = set()
    def dfs(n: str, path: list[str]) -> None:
        if n in visiting:
            i = path.index(n) if n in path else 0
            cyc = path[i:] + [n]
            findings.append({"finding_id": "CONS-"+sha256_text("->".join(cyc))[:12], "severity": "HIGH", "kind": "lock_order_cycle", "locks": cyc, "message": "lock order cycle: " + " -> ".join(cyc)})
            return
        if n in visited: return
        visiting.add(n); path.append(n)
        for m in sorted(graph.get(n, [])): dfs(m, path)
        path.pop(); visiting.remove(n); visited.add(n)
    for n in sorted(graph): dfs(n, [])

    for field_name, by_lock in guards.items():
        if len(by_lock) > 1:
            findings.append({"finding_id": "CONS-"+sha256_text(field_name+"guards")[:12], "severity": "MEDIUM", "kind": "guard_inconsistency", "field": field_name, "guards": {k: sorted(v) for k,v in by_lock.items()}, "message": f"field/resource {field_name!r} is attributed to multiple guards; verify intentional lock partitioning"})

    # De-duplicate findings produced by multiple traversal roots.
    uniq = {x["finding_id"]: x for x in findings}
    findings = sorted(uniq.values(), key=lambda x: ({"HIGH":0,"MEDIUM":1,"LOW":2}.get(x.get("severity"), 9), x["finding_id"]))
    report = {"generated_utc": now_iso(), "findings": findings, "counts": dict(Counter(x["severity"] for x in findings))}
    atomic_write_json(root / "orchestration" / "reports" / "consistency.json", report)
    return report

# ---------------------------------------------------------------------------
# Context compaction and prompt construction
# ---------------------------------------------------------------------------

def clip(text: str, limit: int) -> tuple[str, bool]:
    if len(text) <= limit:
        return text, False
    if limit < 200:
        return text[:limit], True
    keep = (limit - 120) // 2
    return text[:keep] + f"\n\n[... ORCHESTRATOR CLIPPED {len(text)-2*keep} CHARACTERS ...]\n\n" + text[-keep:], True


def pretty(obj: Any, max_chars: int | None = None) -> str:
    text = json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=False)
    if max_chars is None or len(text) <= max_chars:
        return text
    # Preserve syntactically valid JSON even when a context object is too large.
    # A raw mid-string clip would make downstream context ambiguous and may cause
    # a model or machine consumer to treat malformed JSON as complete evidence.
    summary = {
        "_orchestrator_truncated": True,
        "_original_chars": len(text),
        "_top_level_keys": sorted(obj.keys()) if isinstance(obj, dict) else None,
        "_record_count": len(obj) if isinstance(obj, list) else None,
        "_preview": clip(text, max(256, max_chars - 600))[0],
    }
    return json.dumps(summary, indent=2, sort_keys=True, ensure_ascii=False)


def bounded_records(records: Sequence[Any], max_chars: int) -> str:
    """Return valid JSON containing as many complete records as fit."""
    full = json.dumps(list(records), indent=2, sort_keys=True, ensure_ascii=False)
    if len(full) <= max_chars:
        return full
    kept: list[Any] = []
    for rec in records:
        candidate = {
            "_orchestrator_truncated": True,
            "_total_records": len(records),
            "_included_records": len(kept) + 1,
            "records": kept + [rec],
        }
        rendered = json.dumps(candidate, indent=2, sort_keys=True, ensure_ascii=False)
        if len(rendered) > max_chars:
            break
        kept.append(rec)
    final = {
        "_orchestrator_truncated": True,
        "_total_records": len(records),
        "_included_records": len(kept),
        "_omitted_records": len(records) - len(kept),
        "records": kept,
    }
    return json.dumps(final, indent=2, sort_keys=True, ensure_ascii=False)


def load_valid_result(root: Path, stage: str, item: WorkItem, baseline: dict[str, Any]) -> dict[str, Any] | None:
    st = result_status(root, stage, item, baseline)
    if st["status"] != "VALID":
        return None
    return load_json(result_path(root, stage, item.item_id), {}) or {}


def compact_architecture(obj: dict[str, Any], max_chars: int) -> str:
    keep = {
        "architectural_decomposition": obj.get("architectural_decomposition"),
        "feature_model": obj.get("feature_model"),
        "data_state_model": obj.get("data_state_model"),
        "runtime_concurrency_model": obj.get("runtime_concurrency_model"),
        "hardware_protocol_boundary": obj.get("hardware_protocol_boundary"),
        "source_os_service_model": obj.get("source_os_service_model"),
        "target_os_architecture": obj.get("target_os_architecture"),
        "implementation_dag": obj.get("implementation_dag"),
        "evidence_gaps": obj.get("evidence_gaps"),
        "open_questions": obj.get("open_questions"),
        "confidence": obj.get("confidence"),
    }
    return pretty(keep, max_chars)


def compact_method_obj(obj: dict[str, Any]) -> dict[str, Any]:
    semantic = obj.get("semantic_contract") or {}
    sync = obj.get("synchronization") or {}
    return {
        "function_key": obj.get("function_key"),
        "architectural_responsibility": obj.get("architectural_responsibility"),
        "feature_ids": obj.get("feature_ids"),
        "semantic_contract": {
            "state_transitions": semantic.get("state_transitions"),
            "ownership": semantic.get("ownership"),
            "hardware_effects": semantic.get("hardware_effects"),
            "firmware_protocol_effects": semantic.get("firmware_protocol_effects"),
            "error_semantics": semantic.get("error_semantics"),
            "cleanup_obligations": semantic.get("cleanup_obligations"),
        },
        "runtime": obj.get("runtime"),
        "synchronization": {
            "semantic_invariants": sync.get("semantic_invariants"),
            "locks_required": sync.get("locks_required"),
            "barriers": sync.get("barriers"),
        },
        "source_os_dependencies": obj.get("source_os_dependencies"),
        "target_design": obj.get("target_design"),
        "tests": obj.get("tests"),
        "spec_clause_ids": obj.get("spec_clause_ids"),
        "port_requests": obj.get("port_requests"),
        "risks": obj.get("risks"),
        "portability_class": obj.get("portability_class"),
        "confidence": obj.get("confidence"),
        "open_questions": obj.get("open_questions"),
    }


def compact_method(obj: dict[str, Any], max_chars: int) -> str:
    return pretty(compact_method_obj(obj), max_chars)


def compact_file_obj(obj: dict[str, Any]) -> dict[str, Any]:
    return {k: obj.get(k) for k in [
        "file", "purpose", "subsystems", "feature_ids", "function_disposition",
        "state_and_resources", "synchronization_invariants", "hardware_protocol_effects",
        "source_os_services", "target_design", "risks", "documentation_search",
        "tests", "request_decisions", "risk_decisions", "portability_class", "confidence",
    ]}


def compact_file(obj: dict[str, Any], max_chars: int) -> str:
    return pretty(compact_file_obj(obj), max_chars)


def compact_subsystem(obj: dict[str, Any], max_chars: int) -> str:
    return pretty(obj, max_chars)


def evidence_for_functions(root: Path, evidence_rows: Sequence[dict[str, Any]], function_keys: set[str], baseline: dict[str, Any], max_each: int) -> list[dict[str, Any]]:
    out = []
    for row in evidence_rows:
        if not function_keys.intersection(set(row.get("affected_functions") or [])):
            continue
        item = WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row)
        obj = load_valid_result(root, "evidence", item, baseline)
        out.append({
            "ledger": row,
            "result": json.loads(pretty(obj, max_each)) if obj and len(pretty(obj)) <= max_each else (obj if obj else None),
        })
    return out


class PromptComposer:
    def __init__(self, budget: int):
        self.budget = budget
        self.parts: list[str] = []
        self.truncations: list[dict[str, Any]] = []

    def add(self, title: str, text: str, preferred_limit: int | None = None) -> None:
        header = f"\n\n# {title}\n\n"
        remaining = self.budget - sum(len(x) for x in self.parts) - len(header)
        if remaining <= 0:
            self.truncations.append({"section": title, "reason": "budget exhausted", "original_chars": len(text), "included_chars": 0})
            return
        lim = min(remaining, preferred_limit) if preferred_limit else remaining
        clipped, did = clip(text, lim)
        self.parts.append(header + clipped)
        if did:
            self.truncations.append({"section": title, "reason": "section/budget limit", "original_chars": len(text), "included_chars": len(clipped)})

    def render(self) -> str:
        body = "".join(self.parts).lstrip()
        if self.truncations:
            body += "\n\n# Context truncation ledger\n\n```json\n" + json.dumps(self.truncations, indent=2) + "\n```\n"
        return body.rstrip() + "\n"


def project_name(root: Path) -> str:
    return str((load_json(root / "manifest" / "project.json", {}) or {}).get("project_name") or "")


def render_architecture(root: Path, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any]) -> str:
    base = Path(item.source_prompt).read_text(encoding="utf-8", errors="replace")
    health = extraction_health(root)
    c = PromptComposer(int(policy["prompt_budgets"]["architecture"]))
    c.add("Base architecture review", base)
    c.add("Extraction health", pretty(dataclasses.asdict(health), 8000))
    c.add("Baseline fingerprint", f"`{baseline['fingerprint']}`\n")
    clauses = load_spec_clauses(root)
    if clauses:
        c.add("Ingested specification clause index — use stable IDs in feature_model",
              "```json\n" + bounded_records([{k: x.get(k) for k in ("spec_clause_id","document_id","title","source_version","authority_class")} for x in clauses], 40000) + "\n```")
    c.add("Orchestration contract", contract_text("architecture", "architecture", baseline["fingerprint"], project_name(root)))
    add_capability_section(c, root)
    return c.render()


def render_method(root: Path, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]]) -> str:
    base = Path(item.source_prompt).read_text(encoding="utf-8", errors="replace")
    arch = load_valid_result(root, "architecture", static["architecture"][0], baseline)
    c = PromptComposer(int(policy["prompt_budgets"]["methods"]))
    c.add("Base Function Analysis Record prompt", base)
    if arch:
        c.add(
            "Reviewed architecture baseline — use as context, correct it when function evidence contradicts it",
            "```json\n" + compact_architecture(arch, int(policy["context_compaction"]["architecture_for_method_chars"])) + "\n```",
        )
    clauses = load_spec_clauses(root)
    if clauses:
        c.add("Specification clause index — cite relevant stable spec_clause_ids",
              "```json\n" + bounded_records([{k: x.get(k) for k in ("spec_clause_id","document_id","title")} for x in clauses], 28000) + "\n```")
    c.add("Method request record template", "```json\n" + template_path(TEMPLATES, "record.method_port_request").read_text(encoding="utf-8").strip() + "\n```")
    c.add("Method risk record template", "```json\n" + template_path(TEMPLATES, "record.method_risk").read_text(encoding="utf-8").strip() + "\n```")
    c.add("Orchestration contract", contract_text("methods", item.identity or item.item_id, baseline["fingerprint"], project_name(root)))
    add_capability_section(c, root)
    return c.render()


def render_target(root: Path, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]]) -> str:
    ctx = load_json(Path(item.source_context), {}) if item.source_context else {}
    arch = load_valid_result(root, "architecture", static["architecture"][0], baseline)
    scope = target_scope(root)

    def bullets(values: Sequence[Any]) -> str:
        return "\n".join(f"- {x}" for x in values) if values else "- (none declared)"

    base = render_template(template_path(TEMPLATES, "builder.target_work_item"), {
        "ITEM_ID": str(ctx.get("target_item_id") or item.identity),
        "TITLE": str(ctx.get("title") or ""),
        "WORKFLOW": f"{(ctx.get('workflow') or {}).get('workflow_id', '')} - {(ctx.get('workflow') or {}).get('title', '')}",
        "TARGET_OS": str(ctx.get("target_os") or ""),
        "DRIVER_FRAMEWORK": str(ctx.get("driver_framework") or ""),
        "MANDATORY": "yes" if ctx.get("mandatory", True) else "no",
        "REPLACES": str(ctx.get("replaces_source_mechanism") or "(nothing; this is new target-side work)"),
        "OBLIGATIONS": bullets(ctx.get("obligations") or []),
        "TARGET_API": bullets([f"`{x}`" for x in (ctx.get("target_api") or [])]),
        "ACCEPTANCE": bullets(ctx.get("acceptance_criteria") or []),
        "REFERENCES": bullets([f"`{x}`" for x in (ctx.get("references") or [])]),
        "SOURCE_INPUTS": "```json\n" + pretty(ctx.get("source_evidence") or {}, 24000) + "\n```",
    })

    c = PromptComposer(int(policy["prompt_budgets"]["target"]))
    c.add("Target work item", base)
    if arch:
        c.add(
            "Reviewed architecture baseline — use as context, correct it when target obligations contradict it",
            "```json\n" + compact_architecture(arch, int(policy["context_compaction"]["architecture_for_method_chars"])) + "\n```",
        )
    superseded = ctx.get("superseded_source_methods") or []
    if superseded:
        c.add(
            "Source methods removed from the porting loop that this item supersedes — their behaviour must be accounted for here",
            "```json\n" + bounded_records(superseded, 16000) + "\n```",
        )
    if scope.get("by_rule"):
        c.add("Source-OS exclusion summary for this baseline",
              "```json\n" + pretty({k: scope.get(k) for k in ("methods_total", "methods_retained", "methods_excluded", "by_rule", "by_disposition")}, 8000) + "\n```")
    clauses = load_spec_clauses(root)
    if clauses:
        c.add("Specification clause index — cite relevant stable spec_clause_ids",
              "```json\n" + bounded_records([{k: x.get(k) for k in ("spec_clause_id", "document_id", "title")} for x in clauses], 24000) + "\n```")
    c.add("Request record template", "```json\n" + template_path(TEMPLATES, "record.method_port_request").read_text(encoding="utf-8").strip() + "\n```")
    c.add("Risk record template", "```json\n" + template_path(TEMPLATES, "record.method_risk").read_text(encoding="utf-8").strip() + "\n```")
    c.add("Orchestration contract", contract_text("target", item.identity or item.item_id, baseline["fingerprint"], project_name(root)))
    add_capability_section(c, root)
    return c.render()


def render_evidence(root: Path, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]]) -> str:
    row = item.metadata
    affected = set(row.get("affected_functions") or [])
    method_results = []
    for m in static["methods"]:
        if m.identity not in affected:
            continue
        obj = load_valid_result(root, "methods", m, baseline)
        if obj:
            method_results.append(compact_method_obj(obj))
    source_os = (load_json(root / "manifest" / "project.json", {}) or {}).get("source_os", "source OS")
    target_os = (load_json(root / "manifest" / "project.json", {}) or {}).get("target_os", "target OS")
    base = render_template(template_path(TEMPLATES, "orchestrator.evidence_task"), {
        "EVIDENCE_ID": row["evidence_id"],
        "KIND": row["kind"],
        "SYMBOL": row.get("symbol") or "(method-level question)",
        "SEMANTIC_CATEGORY": row["semantic_category"],
        "CRITICAL": row["critical"],
        "AFFECTED_FUNCTIONS": ", ".join(row.get("affected_functions") or []) or "(none)",
        "QUESTION": row["question"],
        "SOURCE_QUESTIONS": json.dumps(row.get("source_questions") or [], indent=2),
        "TARGET_QUESTIONS": json.dumps(row.get("target_questions") or [], indent=2),
        "RESEARCH_HINTS": json.dumps(row.get("research_hints") or [], indent=2),
        "SOURCE_OS": source_os,
        "TARGET_OS": target_os,
    })
    c = PromptComposer(int(policy["prompt_budgets"]["evidence"]))
    c.add("Evidence research task", base)
    if method_results:
        c.add("Affected FAR excerpts", "```json\n" + bounded_records(method_results, 45000) + "\n```", preferred_limit=47000)
    c.add("Mandatory result contract", contract_text("evidence", item.identity or item.item_id, baseline["fingerprint"], project_name(root)))
    add_capability_section(c, root)
    return c.render()


def render_file(root: Path, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> str:
    base = Path(item.source_prompt).read_text(encoding="utf-8", errors="replace")
    fkeys = set(item.metadata.get("function_keys") or [])
    methods = []
    for m in static["methods"]:
        if m.identity not in fkeys:
            continue
        obj = load_valid_result(root, "methods", m, baseline)
        if obj:
            methods.append(compact_method_obj(obj))
    evid = []
    for row in evidence_rows:
        if not fkeys.intersection(set(row.get("affected_functions") or [])):
            continue
        eitem = WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row)
        eobj = load_valid_result(root, "evidence", eitem, baseline)
        evid.append({"ledger": row, "result": eobj})
    c = PromptComposer(int(policy["prompt_budgets"]["files"]))
    c.add("Base file integration prompt", base)
    c.add("Validated FAR results for this file", "```json\n" + bounded_records(methods, 90000) + "\n```", preferred_limit=92000)
    c.add("Documentation/evidence affecting this file", "```json\n" + bounded_records(evid, 45000) + "\n```", preferred_limit=47000)
    regs = load_registries(root)
    if not any(regs.values()):
        regs = build_registries(root, baseline, static, policy)
    ffeatures = file_method_feature_ids(root, str(item.identity), baseline, static)
    subset = registry_subset_for_file(str(item.identity), ffeatures, regs)
    c.add("Method-originated request/risk registries — mandatory triage for decision_required entries",
          "```json\n" + pretty(subset, 52000) + "\n```", preferred_limit=54000)
    trace = load_json(root / "orchestration" / "traceability" / "coverage.json", {}) or {}
    tests = [x for x in iter_jsonl(root / "orchestration" / "traceability" / "tests.jsonl") if ffeatures.intersection(set(x.get("feature_ids") or []))]
    c.add("Feature/test traceability relevant to this file",
          "```json\n" + pretty({"feature_ids": sorted(ffeatures), "tests": tests, "coverage": [x for x in trace.get("coverage") or [] if x.get("feature_id") in ffeatures]}, 40000) + "\n```")
    c.add("File request decision template", "```json\n" + template_path(TEMPLATES, "record.file_request_decision").read_text(encoding="utf-8").strip() + "\n```")
    c.add("File risk decision template", "```json\n" + template_path(TEMPLATES, "record.file_risk_decision").read_text(encoding="utf-8").strip() + "\n```")
    c.add("Orchestration contract", contract_text("files", item.identity or item.item_id, baseline["fingerprint"], project_name(root)))
    add_capability_section(c, root)
    return c.render()


def render_subsystem(root: Path, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> str:
    base = Path(item.source_prompt).read_text(encoding="utf-8", errors="replace")
    fkeys = set(item.metadata.get("function_keys") or [])
    files = set(item.metadata.get("files") or [])
    arch = load_valid_result(root, "architecture", static["architecture"][0], baseline)

    file_results = []
    for f in static["files"]:
        if f.identity not in files:
            continue
        obj = load_valid_result(root, "files", f, baseline)
        if obj:
            file_results.append(compact_file_obj(obj))

    method_summaries = []
    for m in static["methods"]:
        if m.identity not in fkeys:
            continue
        obj = load_valid_result(root, "methods", m, baseline)
        if obj:
            method_summaries.append({
                "function_key": obj.get("function_key"),
                "architectural_responsibility": obj.get("architectural_responsibility"),
                "feature_ids": obj.get("feature_ids"),
                "portability_class": obj.get("portability_class"),
                "confidence": obj.get("confidence"),
                "open_questions": obj.get("open_questions"),
            })

    evid = []
    for row in evidence_rows:
        if not fkeys.intersection(set(row.get("affected_functions") or [])):
            continue
        eitem = WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row)
        eobj = load_valid_result(root, "evidence", eitem, baseline)
        evid.append({"ledger": row, "result": eobj})

    c = PromptComposer(int(policy["prompt_budgets"]["subsystems"]))
    c.add("Base subsystem integration prompt", base)
    if arch:
        c.add("Architecture baseline", "```json\n" + compact_architecture(arch, 26000) + "\n```")
    c.add("Validated file architecture records", "```json\n" + bounded_records(file_results, 85000) + "\n```", preferred_limit=87000)
    c.add("Method-level coverage summary", "```json\n" + bounded_records(method_summaries, 35000) + "\n```", preferred_limit=37000)
    c.add("Resolved/unresolved evidence affecting subsystem", "```json\n" + bounded_records(evid, 45000) + "\n```", preferred_limit=47000)
    regs = load_registries(root)
    proj = project_registry_resolution(root, baseline, static, regs) if any(regs.values()) else {"project_requests": [], "project_risks": []}
    feature_ids = set(x for fr in file_results for x in (fr.get("feature_ids") or []))
    proj_relevant = {
        "project_requests": [x for x in proj["project_requests"] if feature_ids.intersection(set(x.get("feature_ids") or []))],
        "project_risks": [x for x in proj["project_risks"] if feature_ids.intersection(set(x.get("feature_ids") or []))],
    }
    c.add("Project-scope requests/risks relevant to subsystem features", "```json\n" + pretty(proj_relevant, 36000) + "\n```")
    c.add("Mandatory result contract", contract_text("subsystems", item.identity or item.item_id, baseline["fingerprint"], project_name(root)))
    add_capability_section(c, root)
    return c.render()


def render_final(root: Path, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> str:
    base = Path(item.source_prompt).read_text(encoding="utf-8", errors="replace")
    arch = load_valid_result(root, "architecture", static["architecture"][0], baseline)
    subs = []
    for s in static["subsystems"]:
        obj = load_valid_result(root, "subsystems", s, baseline)
        if obj:
            subs.append(obj)
    files = []
    for f in static["files"]:
        obj = load_valid_result(root, "files", f, baseline)
        if obj:
            files.append({
                "file": obj.get("file"), "target_design": obj.get("target_design"),
                "risks": obj.get("risks"), "portability_class": obj.get("portability_class"),
                "confidence": obj.get("confidence"),
            })
    evidence_summary = []
    for row in evidence_rows:
        eitem = WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row)
        eobj = load_valid_result(root, "evidence", eitem, baseline)
        evidence_summary.append({
            "evidence_id": row["evidence_id"], "critical": row["critical"],
            "category": row["semantic_category"], "symbol": row.get("symbol"),
            "status": eobj.get("status") if eobj else "MISSING",
            "confidence": eobj.get("confidence") if eobj else None,
            "open_questions": eobj.get("open_questions") if eobj else [],
        })
    report = compute_status(root, baseline, policy, static, evidence_rows)
    c = PromptComposer(int(policy["prompt_budgets"]["final"]))
    c.add("Base final port-design synthesis prompt", base)
    if arch:
        c.add("Validated architecture record", "```json\n" + pretty(arch, 50000) + "\n```")
    c.add("Validated subsystem design records", "```json\n" + bounded_records(subs, 140000) + "\n```", preferred_limit=142000)
    c.add("File-level target/risk summary", "```json\n" + bounded_records(files, 35000) + "\n```", preferred_limit=37000)
    c.add("Evidence ledger summary", "```json\n" + bounded_records(evidence_summary, 40000) + "\n```", preferred_limit=42000)
    c.add("Current orchestration gate/coverage report", "```json\n" + pretty(report, 30000) + "\n```")
    regs = load_registries(root)
    project_regs = project_registry_resolution(root, baseline, static, regs) if any(regs.values()) else {"project_requests": [], "project_risks": []}
    c.add("Project-scope request and risk registry — every actionable item requires a final disposition", "```json\n" + pretty(project_regs, 50000) + "\n```")
    trace = load_json(root / "orchestration" / "traceability" / "coverage.json", {}) or {}
    c.add("Feature/test/spec traceability coverage", "```json\n" + pretty(trace, 36000) + "\n```")
    consistency = load_json(root / "orchestration" / "reports" / "consistency.json", {}) or {}
    c.add("Architecture consistency findings", "```json\n" + pretty(consistency, 30000) + "\n```")
    c.add("Project request decision template", "```json\n" + template_path(TEMPLATES, "record.project_request_decision").read_text(encoding="utf-8").strip() + "\n```")
    c.add("Project risk decision template", "```json\n" + template_path(TEMPLATES, "record.project_risk_decision").read_text(encoding="utf-8").strip() + "\n```")
    c.add("Mandatory final result contract", contract_text("final", "final", baseline["fingerprint"], project_name(root)))
    add_capability_section(c, root)
    return c.render()


# ---------------------------------------------------------------------------
# Gates and status
# ---------------------------------------------------------------------------

def confidence_at_least(value: str | None, minimum: str) -> bool:
    return CONFIDENCE_ORDER.get(value or "C0", -1) >= CONFIDENCE_ORDER.get(minimum, 99)


def stage_summary(root: Path, stage: str, items: Sequence[WorkItem], baseline: dict[str, Any]) -> dict[str, Any]:
    statuses = [result_status(root, stage, x, baseline) for x in items]
    counts = Counter(x["status"] for x in statuses)
    valid = sum(1 for x in statuses if x["status"] == "VALID")
    return {
        "total": len(items),
        "valid": valid,
        "coverage": (valid / len(items)) if items else 1.0,
        "status_counts": dict(counts),
        "confidence_counts": dict(Counter(x.get("confidence") for x in statuses if x.get("confidence"))),
    }


def critical_evidence_summary(root: Path, rows: Sequence[dict[str, Any]], baseline: dict[str, Any], min_conf: str, policy: dict[str, Any] | None = None) -> dict[str, Any]:
    crit = [r for r in rows if r.get("critical")]
    resolved = 0
    details = []
    verification = load_json(root / "orchestration" / "evidence" / "citation_verification.json", {}) or {}
    by_eid = {x.get("evidence_id"): x for x in verification.get("items") or []}
    for row in crit:
        item = WorkItem(stage="evidence", item_id=row["evidence_id"], identity=row["evidence_id"], metadata=row)
        obj = load_valid_result(root, "evidence", item, baseline)
        citation = by_eid.get(row["evidence_id"])
        citation_ok = bool(citation and citation.get("effective_verified"))
        # During a just-ingested result the verification report may not yet have
        # been refreshed.  Never promote on that basis; a refresh/verify pass is
        # required before the critical gate becomes green.
        ok = bool(obj and obj.get("status") == "VERIFIED" and confidence_at_least(obj.get("confidence"), min_conf) and citation_ok)
        resolved += int(ok)
        details.append({
            "evidence_id": row["evidence_id"], "symbol": row.get("symbol"),
            "category": row.get("semantic_category"), "resolved": ok,
            "status": obj.get("status") if obj else "MISSING",
            "confidence": obj.get("confidence") if obj else None,
            "citation_verified": citation_ok,
            "required_authority": (citation or {}).get("required_authority"),
            "best_authority_score": (citation or {}).get("best_authority_score"),
        })
    return {
        "total": len(crit), "resolved": resolved,
        "resolution": resolved / len(crit) if crit else 1.0,
        "unresolved": [x for x in details if not x["resolved"]],
    }


def gate_for_stage(root: Path, stage: str, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> dict[str, Any]:
    reasons: list[str] = []
    warnings: list[str] = []
    health = extraction_health(root)
    if not health.valid:
        reasons.extend(health.errors)
    warnings.extend(health.warnings)

    gates = policy["gates"].get(stage, {})
    if stage == "architecture":
        return {"ready": not reasons, "blocking_reasons": reasons, "warnings": warnings}

    if stage == "methods":
        a = static["architecture"][0]
        ast = result_status(root, "architecture", a, baseline)
        if gates.get("require_architecture_valid", True) and ast["status"] != "VALID":
            reasons.append("Architecture result is not VALID.")
        elif not confidence_at_least(ast.get("confidence"), gates.get("min_architecture_confidence", "C0")):
            reasons.append(f"Architecture confidence {ast.get('confidence')} is below {gates.get('min_architecture_confidence')}.")
        need_target = float(gates.get("require_target_coverage", 0.0))
        if need_target > 0:
            if not target_scope(root):
                reasons.append("Target scope has not been established; run the target triage before the porting loop.")
            tsumm = stage_summary(root, "target", static.get("target", []), baseline)
            if tsumm["coverage"] < need_target:
                reasons.append(f"Target work item coverage {tsumm['coverage']:.1%} is below required {need_target:.1%}.")
            minc = gates.get("min_target_confidence", "C0")
            low = [t.identity for t in static.get("target", [])
                   if (lambda st: st["status"] == "VALID" and not confidence_at_least(st.get("confidence"), minc))(result_status(root, "target", t, baseline))]
            if low:
                reasons.append(f"{len(low)} target work item result(s) are below confidence {minc}.")

    elif stage == "target":
        a = static["architecture"][0]
        ast = result_status(root, "architecture", a, baseline)
        if gates.get("require_architecture_valid", True) and ast["status"] != "VALID":
            reasons.append("Architecture result is not VALID.")
        elif not confidence_at_least(ast.get("confidence"), gates.get("min_architecture_confidence", "C0")):
            reasons.append(f"Architecture confidence {ast.get('confidence')} is below {gates.get('min_architecture_confidence')}.")
        if gates.get("require_target_plan", True) and not static.get("target"):
            reasons.append("No target work items are planned; run 'nic_port_target.py scope' first.")

    elif stage == "evidence":
        summ = stage_summary(root, "methods", static["methods"], baseline)
        need = float(gates.get("require_method_coverage", 1.0))
        if summ["coverage"] < need:
            reasons.append(f"Method coverage {summ['coverage']:.1%} is below required {need:.1%}.")
        minc = gates.get("min_method_confidence", "C0")
        low = []
        for m in static["methods"]:
            st = result_status(root, "methods", m, baseline)
            if st["status"] == "VALID" and not confidence_at_least(st.get("confidence"), minc):
                low.append(m.identity)
        if low:
            reasons.append(f"{len(low)} valid method result(s) are below confidence {minc}.")

    elif stage == "files":
        summ = stage_summary(root, "methods", static["methods"], baseline)
        need = float(gates.get("require_method_coverage", 1.0))
        if summ["coverage"] < need:
            reasons.append(f"Method coverage {summ['coverage']:.1%} is below required {need:.1%}.")
        minc = gates.get("min_method_confidence", "C0")
        low = [m.identity for m in static["methods"]
               if (lambda st: st["status"] == "VALID" and not confidence_at_least(st.get("confidence"), minc))(result_status(root, "methods", m, baseline))]
        if low:
            reasons.append(f"{len(low)} method result(s) are below confidence {minc}.")
        ev = critical_evidence_summary(root, evidence_rows, baseline, gates.get("min_evidence_confidence", "C0"))
        need_ev = float(gates.get("require_critical_evidence_resolution", 0.0))
        if ev["resolution"] < need_ev:
            reasons.append(f"Critical evidence resolution {ev['resolution']:.1%} is below required {need_ev:.1%}.")

    elif stage == "subsystems":
        summ = stage_summary(root, "files", static["files"], baseline)
        need = float(gates.get("require_file_coverage", 1.0))
        if summ["coverage"] < need:
            reasons.append(f"File coverage {summ['coverage']:.1%} is below required {need:.1%}.")
        minc = gates.get("min_file_confidence", "C0")
        low = [f.identity for f in static["files"]
               if (lambda st: st["status"] == "VALID" and not confidence_at_least(st.get("confidence"), minc))(result_status(root, "files", f, baseline))]
        if low:
            reasons.append(f"{len(low)} file result(s) are below confidence {minc}.")

    elif stage == "final":
        a = result_status(root, "architecture", static["architecture"][0], baseline)
        min_arch = gates.get("min_architecture_confidence", "C0")
        if a["status"] != "VALID" or not confidence_at_least(a.get("confidence"), min_arch):
            reasons.append(f"Architecture must be VALID at confidence >= {min_arch}.")
        summ = stage_summary(root, "subsystems", static["subsystems"], baseline)
        need = float(gates.get("require_subsystem_coverage", 1.0))
        if summ["coverage"] < need:
            reasons.append(f"Subsystem coverage {summ['coverage']:.1%} is below required {need:.1%}.")
        minc = gates.get("min_subsystem_confidence", "C0")
        low = [s.identity for s in static["subsystems"]
               if (lambda st: st["status"] == "VALID" and not confidence_at_least(st.get("confidence"), minc))(result_status(root, "subsystems", s, baseline))]
        if low:
            reasons.append(f"{len(low)} subsystem result(s) are below confidence {minc}.")
        ev = critical_evidence_summary(root, evidence_rows, baseline, gates.get("min_evidence_confidence", "C0"))
        need_ev = float(gates.get("require_critical_evidence_resolution", 0.0))
        if ev["resolution"] < need_ev:
            reasons.append(f"Critical evidence resolution {ev['resolution']:.1%} is below required {need_ev:.1%}.")
        if policy.get("consistency", {}).get("block_high_severity_at_final", True):
            cons = load_json(root / "orchestration" / "reports" / "consistency.json", {}) or {}
            high = [x for x in cons.get("findings") or [] if x.get("severity") == "HIGH"]
            if high:
                reasons.append(f"{len(high)} unresolved HIGH architecture consistency finding(s) block final synthesis.")

        trace = load_json(root / "orchestration" / "traceability" / "coverage.json", {}) or {}
        gaps = list(trace.get("required_test_gaps") or [])
        tpol = policy.get("traceability", {})
        if gaps and tpol.get("block_final_missing_required_tests", False):
            reasons.append(f"{len(gaps)} required-priority feature(s) have no linked test: {gaps[:20]}")
        elif gaps and tpol.get("warn_final_missing_required_tests", True):
            warnings.append(f"{len(gaps)} required-priority feature(s) have no linked test: {gaps[:20]}")

    return {"ready": not reasons, "blocking_reasons": reasons, "warnings": warnings}


def gate_for_item(root: Path, stage: str, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> dict[str, Any]:
    """Item-local readiness gate used to make progress monotonically.

    A file can advance as soon as its own methods/evidence are ready; it does not
    wait for unrelated files.  Likewise a subsystem waits only for its member
    files.  Global final synthesis intentionally remains globally gated.
    """
    if stage in {"architecture", "methods", "final"}:
        return gate_for_stage(root, stage, baseline, policy, static, evidence_rows)
    reasons: list[str] = []
    warnings: list[str] = []
    health = extraction_health(root)
    if not health.valid: reasons.extend(health.errors)
    warnings.extend(health.warnings)
    gates = policy.get("gates", {}).get(stage, {})

    if stage == "evidence":
        affected = set(str(x) for x in (item.metadata.get("affected_functions") or []))
        by_identity = {str(x.identity): x for x in static.get("methods", [])}
        minc = gates.get("min_method_confidence", "C0")
        for fk in sorted(affected):
            m = by_identity.get(fk)
            if not m:
                warnings.append(f"Evidence references unknown function {fk}.")
                continue
            st = result_status(root, "methods", m, baseline)
            if st["status"] != "VALID":
                reasons.append(f"Affected method {fk} is not VALID.")
            elif not confidence_at_least(st.get("confidence"), minc):
                reasons.append(f"Affected method {fk} confidence {st.get('confidence')} is below {minc}.")

    elif stage == "files":
        fkeys = set(str(x) for x in (item.metadata.get("function_keys") or []))
        methods = [m for m in static.get("methods", []) if str(m.identity) in fkeys]
        minc = gates.get("min_method_confidence", "C0")
        for m in methods:
            st = result_status(root, "methods", m, baseline)
            if st["status"] != "VALID":
                reasons.append(f"Method {m.identity} is not VALID.")
            elif not confidence_at_least(st.get("confidence"), minc):
                reasons.append(f"Method {m.identity} confidence {st.get('confidence')} is below {minc}.")
        relevant_critical = [r for r in evidence_rows if r.get("critical") and fkeys.intersection(set(str(x) for x in (r.get("affected_functions") or [])))]
        ev = critical_evidence_summary(root, relevant_critical, baseline, gates.get("min_evidence_confidence", "C0"), policy)
        need_ev = float(gates.get("require_critical_evidence_resolution", 0.0))
        if ev["resolution"] < need_ev:
            reasons.append(f"File-local critical evidence resolution {ev['resolution']:.1%} is below required {need_ev:.1%}.")

    elif stage == "subsystems":
        files = set(str(x) for x in (item.metadata.get("files") or []))
        by_identity = {str(x.identity): x for x in static.get("files", [])}
        minc = gates.get("min_file_confidence", "C0")
        for f in sorted(files):
            fi = by_identity.get(f)
            if not fi:
                warnings.append(f"Subsystem references unknown file {f}.")
                continue
            st = result_status(root, "files", fi, baseline)
            if st["status"] != "VALID":
                reasons.append(f"File {f} is not VALID.")
            elif not confidence_at_least(st.get("confidence"), minc):
                reasons.append(f"File {f} confidence {st.get('confidence')} is below {minc}.")
    else:
        return gate_for_stage(root, stage, baseline, policy, static, evidence_rows)
    return {"ready": not reasons, "blocking_reasons": reasons, "warnings": warnings}


def compute_status(root: Path, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> dict[str, Any]:
    evid_items = evidence_items(root, evidence_rows)
    all_items = dict(static)
    all_items["evidence"] = evid_items
    stages = {stage: stage_summary(root, stage, all_items.get(stage, []), baseline) for stage in STAGES}
    gates = {stage: gate_for_stage(root, stage, baseline, policy, static, evidence_rows) for stage in STAGES}
    item_readiness: dict[str, Any] = {}
    for stage in ("evidence", "files", "subsystems"):
        its = all_items.get(stage, [])
        rows = [{"item_id": x.item_id, "identity": x.identity, **gate_for_item(root, stage, x, baseline, policy, static, evidence_rows)} for x in its]
        item_readiness[stage] = {"ready": sum(bool(x.get("ready")) for x in rows), "total": len(rows), "items": rows}
    crit = critical_evidence_summary(root, evidence_rows, baseline, policy["gates"]["final"].get("min_evidence_confidence", "C4"))
    return {
        "generated_utc": now_iso(),
        "baseline_fingerprint": baseline["fingerprint"],
        "stages": stages,
        "gates": gates,
        "item_readiness": item_readiness,
        "critical_evidence": crit,
        "registries": load_json(root / "orchestration" / "registries" / "summary.json", {}) or {},
        "traceability": load_json(root / "orchestration" / "traceability" / "coverage.json", {}) or {},
        "consistency": load_json(root / "orchestration" / "reports" / "consistency.json", {}) or {},
    }


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def get_items_for_stage(stage: str, static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]], root: Path) -> list[WorkItem]:
    if stage == "evidence":
        return evidence_items(root, evidence_rows)
    return static[stage]


def render_item(root: Path, stage: str, item: WorkItem, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> tuple[Path, dict[str, Any]]:
    if stage == "architecture":
        text = render_architecture(root, item, baseline, policy)
    elif stage == "target":
        text = render_target(root, item, baseline, policy, static)
    elif stage == "methods":
        text = render_method(root, item, baseline, policy, static)
    elif stage == "evidence":
        text = render_evidence(root, item, baseline, policy, static)
    elif stage == "files":
        text = render_file(root, item, baseline, policy, static, evidence_rows)
    elif stage == "subsystems":
        text = render_subsystem(root, item, baseline, policy, static, evidence_rows)
    elif stage == "final":
        text = render_final(root, item, baseline, policy, static, evidence_rows)
    else:
        raise ValueError(stage)
    p = enriched_prompt_path(root, stage, item.item_id)
    atomic_write_text(p, text)
    dep_fp = dependency_fingerprint(root, stage, item, baseline, static, evidence_rows)
    meta = {
        "schema_version": SCHEMA_VERSION,
        "orchestrator_version": TOOL_VERSION,
        "generated_utc": now_iso(),
        "stage": stage,
        "item_id": item.item_id,
        "identity": item.identity,
        "baseline_fingerprint": baseline["fingerprint"],
        "dependency_fingerprint": dep_fp,
        "prompt_sha256": sha256_text(text),
        "prompt_chars": len(text),
    }
    atomic_write_json(p.with_suffix(".meta.json"), meta)
    return p, meta


def render_stage(root: Path, stage: str, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]], force: bool = False) -> dict[str, Any]:
    gate = gate_for_stage(root, stage, baseline, policy, static, evidence_rows)
    if not gate["ready"] and not force:
        return {"stage": stage, "rendered": 0, "blocked": True, "gate": gate}
    items = get_items_for_stage(stage, static, evidence_rows, root)
    rendered = []
    for item in items:
        p, meta = render_item(root, stage, item, baseline, policy, static, evidence_rows)
        rendered.append({"item_id": item.item_id, "path": str(p), "sha256": meta["prompt_sha256"]})
    return {"stage": stage, "rendered": len(rendered), "blocked": False, "gate": gate, "items": rendered}


# ---------------------------------------------------------------------------
# Prompt execution (external command or registered LLM provider)
# ---------------------------------------------------------------------------

def format_runner_argv(template: str, prompt: Path, root: Path, stage: str, item_id: str) -> list[str]:
    mapping = {
        "prompt": str(prompt),
        "root": str(root),
        "stage": stage,
        "item_id": item_id,
    }
    try:
        rendered = template.format(**mapping)
    except KeyError as exc:
        raise ValueError(f"Unknown runner placeholder: {exc}. Allowed: {sorted(mapping)}") from exc
    argv = shlex.split(rendered)
    if not argv:
        raise ValueError("runner command is empty")
    return argv


def command_executor(runner: str) -> Any:
    """Execute an enriched prompt through an external CLI; stdout carries the result."""
    def execute(root: Path, stage: str, item_id: str, prompt: Path, timeout: int) -> tuple[int, str, str]:
        argv = format_runner_argv(runner, prompt, root, stage, item_id)
        cp = subprocess.run(argv, cwd=str(root), text=True, capture_output=True, timeout=timeout, check=False)
        return cp.returncode, cp.stdout, cp.stderr
    return execute


def provider_executor(provider: LLMProvider, model: str | None) -> Any:
    """Execute an enriched prompt through a registered LLM provider."""
    def execute(root: Path, stage: str, item_id: str, prompt: Path, timeout: int) -> tuple[int, str, str]:
        text = prompt.read_text(encoding="utf-8", errors="replace")
        try:
            return 0, provider.complete(text, model=model, timeout=timeout), ""
        except ProviderError as exc:
            return 1, "", str(exc)
    return execute


def llm_provider_config(policy: Mapping[str, Any], provider: str | None) -> tuple[str, dict[str, Any]]:
    cfg = policy.get("llm") or {}
    name = str(provider or cfg.get("default_provider") or DEFAULT_PROVIDER)
    return name, dict((cfg.get("providers") or {}).get(name) or {})


def build_llm_provider(args: argparse.Namespace, policy: Mapping[str, Any]) -> LLMProvider:
    name, cfg = llm_provider_config(policy, getattr(args, "llm_provider", None))
    cache = TokenCache(
        path=Path(args.llm_cache).expanduser() if getattr(args, "llm_cache", None) else default_cache_path(),
        enabled=not getattr(args, "no_llm_cache", False),
    )
    return create_provider(
        name,
        token=getattr(args, "llm_token", None),
        interactive=bool(getattr(args, "llm_device_flow", False)),
        api_base=getattr(args, "llm_api_base", None) or cfg.get("api_base"),
        model=getattr(args, "llm_model", None) or cfg.get("model"),
        cache=cache,
        verbose=bool(getattr(args, "llm_verbose", False)),
        options=cfg,
    )


def run_one(root: Path, stage: str, item: WorkItem, execute: Any, timeout: int, baseline: dict[str, Any], retries: int) -> dict[str, Any]:
    prompt = enriched_prompt_path(root, stage, item.item_id)
    if not prompt.exists():
        return {"item_id": item.item_id, "ok": False, "error": "enriched prompt missing"}
    last: dict[str, Any] = {}
    current_prompt = prompt
    for attempt in range(retries + 1):
        try:
            returncode, stdout, stderr = execute(root, stage, item.item_id, current_prompt, timeout)
        except subprocess.TimeoutExpired:
            last = {"item_id": item.item_id, "ok": False, "error": f"runner timeout after {timeout}s", "attempt": attempt + 1}
            continue
        raw = raw_result_path(root, stage, item.item_id)
        atomic_write_text(raw, stdout)
        if returncode != 0:
            last = {
                "item_id": item.item_id, "ok": False,
                "error": f"runner exit code {returncode}",
                "stderr": stderr[-4000:], "attempt": attempt + 1,
            }
            continue
        vr = ingest_result(root, stage, item, raw, baseline)
        if vr.valid:
            return {"item_id": item.item_id, "ok": True, "attempt": attempt + 1, "warnings": vr.warnings}
        last = {"item_id": item.item_id, "ok": False, "validation_errors": vr.errors, "attempt": attempt + 1}
        if attempt < retries:
            # Produce a deterministic repair prompt.  The next external call sees
            # the previous raw answer and exact validator errors.
            repair = root / "orchestration" / "prompts" / stage / f"{safe_id(item.item_id)}.repair{attempt+1}.md"
            base_text = prompt.read_text(encoding="utf-8", errors="replace")
            raw_text = raw.read_text(encoding="utf-8", errors="replace")
            repair_text = render_template(template_path(TEMPLATES, "orchestrator.repair"), {
                "ORIGINAL_PROMPT": base_text,
                "VALIDATION_ERRORS": json.dumps(vr.errors, indent=2),
                "PREVIOUS_RAW_ANSWER": clip(raw_text, 50000)[0],
            })
            atomic_write_text(repair, repair_text)
            current_prompt = repair
    return last


# ---------------------------------------------------------------------------
# Workspace initialization/refresh
# ---------------------------------------------------------------------------

def configured_spec_manifests() -> list[Path]:
    if not get_path(INPUT_MANIFEST, "workflow.ingest_specifications", True):
        return []
    refs = get_path(INPUT_MANIFEST, "inputs.specification_manifests", []) or []
    if isinstance(refs, (str, Path)):
        refs = [refs]
    out: list[Path] = []
    for ref in refs:
        p = manifest_ref(INPUT_MANIFEST, ref)
        if p is not None:
            out.append(p)
    return out


def update_exit_manifest(root: Path, phase: str, report: dict[str, Any] | None = None) -> Path:
    project = load_json(root / "manifest" / "project.json", {}) or {}
    stage = next_stage(report) if report else None
    catalog = OUTPUT_CATALOG.get("orchestrator") or {}
    def render_obj(obj: Any) -> Any:
        if isinstance(obj, dict):
            return {k: render_obj(v) for k,v in obj.items()}
        if isinstance(obj, list):
            return [render_obj(v) for v in obj]
        if isinstance(obj, str):
            return (obj.replace("{{INPUT_MANIFEST}}", str(INPUT_MANIFEST_PATH))
                       .replace("{{STAGE}}", str(stage or ""))
                       .replace("{{OUTPUT_MANIFEST}}", str(root / "output_manifest.json")))
        return obj
    next_steps: list[dict[str, Any]] = []
    for row in catalog.get("next_steps") or []:
        if row.get("action") == "advance_stage" and not stage:
            continue
        x = render_obj(row)
        if row.get("action") == "advance_stage" and stage:
            x["action"] = f"advance_{stage}"
            x["order"] = 1
        else:
            x["order"] = len(next_steps) + 1
        next_steps.append(x)
    run_state = load_json(root / "manifest" / "run_state.json", {}) or {}
    extraction_quality = load_json(root / "manifest" / "extraction_quality.json", {}) or {}
    status_payload = dict(report or {})
    status_payload["extraction"] = {
        "run_id": run_state.get("run_id"),
        "run_state": run_state.get("status"),
        "phase": run_state.get("phase"),
        "quality": extraction_quality.get("status"),
        "translation_units_total": extraction_quality.get("translation_units_total"),
        "translation_units_failed_or_partial": extraction_quality.get("translation_units_failed_or_partial"),
        "translation_units_recovered": extraction_quality.get("translation_units_recovered"),
    }
    return write_output_manifest(
        root, input_manifest=INPUT_MANIFEST_PATH, phase=phase,
        project={
            "name": project.get("project_name"), "variant": project.get("build_variant"),
            "source_os": project.get("source_os"), "target_os": project.get("target_os"),
            "source_root": project.get("source_root"), "git_commit": project.get("git_commit"),
            "run_id": run_state.get("run_id"),
        },
        directories=[(str(x["role"]), str(x["path"]), str(x.get("description") or "")) for x in catalog.get("directories") or []],
        significant=[(str(x["role"]), str(x["path"]), str(x.get("description") or ""), bool(x.get("required", False))) for x in catalog.get("significant_files") or []],
        know_how=[str(x) for x in catalog.get("know_how") or []],
        next_steps=next_steps,
        status=status_payload,
        vision=str(OUTPUT_CATALOG.get("vision") or ""),
        auto_index_description=str(OUTPUT_CATALOG.get("auto_index_description") or ""),
    )


def configure_from_manifest(path: Path) -> dict[str, Any]:
    global INPUT_MANIFEST, INPUT_MANIFEST_PATH, TEMPLATES, DEFAULT_POLICY
    global STAGES, CONFIDENCE_ORDER, PORTABILITY_CLASSES, RESULT_SCHEMAS, FRAMEWORK_MODEL, OUTPUT_CATALOG, EXTRACTION_RUNTIME
    global AUTHORITY_ORDER, LIKELIHOOD_ORDER, IMPACT_ORDER
    manifest = load_manifest(path, strict_env=True)
    errs = validate_manifest_basics(manifest)
    if errs:
        raise ManifestError("; ".join(errs))
    INPUT_MANIFEST = manifest
    INPUT_MANIFEST_PATH = path.resolve()
    TEMPLATES = load_template_index(manifest)
    DEFAULT_POLICY = load_policy_from_manifest(manifest)
    FRAMEWORK_MODEL = load_framework_model(manifest)
    OUTPUT_CATALOG = load_output_catalog(manifest)
    EXTRACTION_RUNTIME = load_extraction_runtime(manifest)
    if not DEFAULT_POLICY:
        raise ManifestError("No orchestration policy was loaded from configuration.orchestration_policy")
    STAGES = tuple(str(x) for x in (FRAMEWORK_MODEL.get("stages") or []))
    if not STAGES:
        raise ManifestError("framework model must define non-empty stages")
    confidence_levels = [str(x) for x in (FRAMEWORK_MODEL.get("confidence_levels") or [])]
    portability_classes = [str(x) for x in (FRAMEWORK_MODEL.get("portability_classes") or [])]
    CONFIDENCE_ORDER = {name: idx for idx, name in enumerate(confidence_levels)}
    PORTABILITY_CLASSES = set(portability_classes)
    AUTHORITY_ORDER = {name: idx for idx, name in enumerate(str(x) for x in (FRAMEWORK_MODEL.get("authority_levels") or []))}
    LIKELIHOOD_ORDER = {name: idx + 1 for idx, name in enumerate(str(x) for x in (FRAMEWORK_MODEL.get("risk_likelihood_levels") or []))}
    IMPACT_ORDER = {name: idx + 1 for idx, name in enumerate(str(x) for x in (FRAMEWORK_MODEL.get("risk_impact_levels") or []))}
    RESULT_SCHEMAS = {}
    for stage, ref in (FRAMEWORK_MODEL.get("result_schemas") or {}).items():
        sp = Path(str(ref)).expanduser().resolve()
        if not sp.is_file():
            raise ManifestError(f"configured result schema does not exist for {stage}: {sp}")
        RESULT_SCHEMAS[str(stage)] = json.loads(sp.read_text(encoding="utf-8"))
    missing = [x for x in STAGES if x not in RESULT_SCHEMAS]
    if missing:
        raise ManifestError("framework model missing result schemas for: " + ", ".join(missing))
    return manifest


def load_policy(root: Path, override: Path | None = None) -> dict[str, Any]:
    # Manifest policy is authoritative.  Workspace copies are generated outputs,
    # not an independent configuration source.
    base = copy.deepcopy(DEFAULT_POLICY)
    if override:
        base = deep_merge(base, load_json(override, {}) or {})
    return base


def initialize(root: Path, policy_override: Path | None = None) -> dict[str, Any]:
    require_builder_root(root)
    health = extraction_health(root)
    if not health.valid:
        raise SystemExit("Builder extraction is not eligible for orchestration:\n  " + "\n  ".join(health.errors))
    config_drift = validate_configuration_drift(root)
    orch = root / "orchestration"
    orch.mkdir(parents=True, exist_ok=True)
    policy = load_policy(root, policy_override)
    atomic_write_json(orch / "policy.json", policy)
    baseline = compute_baseline(root)
    old_baseline = load_json(orch / "baseline.json", {}) or {}
    atomic_write_json(orch / "baseline.json", baseline)
    inventory = build_inventory(root, baseline)
    atomic_write_json(orch / "inventory.json", inventory)
    write_schemas(root)
    write_process_templates(root)
    ingested_specs = []
    for spec_manifest in configured_spec_manifests():
        if spec_manifest.is_file():
            ingested_specs.append(ingest_spec_manifest(root, spec_manifest))
        else:
            raise FileNotFoundError(f"Configured specification manifest does not exist: {spec_manifest}")
    static = all_static_items(root)
    evidence_rows = aggregate_evidence(root, static, policy)
    atomic_write_json(orch / "evidence" / "inventory.json", evidence_rows)
    registries = build_registries(root, baseline, static, policy)
    traceability = build_traceability(root, baseline, static, registries)
    evidence_verification = verify_evidence_sources(root, baseline, static, evidence_rows, policy, online=False)
    consistency = run_consistency_checks(root, baseline, static, policy)
    registry_resolution = write_registry_resolution_snapshot(root, baseline, static, registries)
    # Architecture is the only stage that should be rendered immediately.
    arch_render = render_stage(root, "architecture", baseline, policy, static, evidence_rows, force=False)
    status = compute_status(root, baseline, policy, static, evidence_rows)
    atomic_write_json(orch / "reports" / "status.json", status)
    output_manifest = update_exit_manifest(root, "orchestration_initialized", status)
    return {
        "baseline_fingerprint": baseline["fingerprint"],
        "configuration_drift": config_drift,
        "baseline_changed": bool(old_baseline and old_baseline.get("fingerprint") != baseline["fingerprint"]),
        "inventory_counts": {k: len(v) for k, v in static.items()},
        "evidence_items": len(evidence_rows),
        "registry_counts": {k: len(v) for k, v in registries.items()},
        "traceability": {k: traceability.get(k) for k in ("features", "tests", "links")},
        "evidence_citations_verified": evidence_verification.get("verified"),
        "consistency_counts": consistency.get("counts"),
        "registry_resolution_counts": registry_resolution.get("counts"),
        "architecture_render": arch_render,
        "specification_manifests_ingested": len(ingested_specs),
        "authorized_capabilities": {
            "mcp_and_tools": sum(int(x.get("mcp_and_tools") or 0) for x in ingested_specs),
            "skills": sum(int(x.get("skills") or 0) for x in ingested_specs),
        },
        "status_path": str(orch / "reports" / "status.json"),
        "output_manifest": str(output_manifest),
    }


def refresh_workspace(root: Path, policy_override: Path | None = None) -> tuple[dict[str, Any], dict[str, Any], dict[str, list[WorkItem]], list[dict[str, Any]]]:
    require_builder_root(root)
    health = extraction_health(root)
    if not health.valid:
        raise SystemExit("Builder extraction is not eligible for orchestration refresh:\n  " + "\n  ".join(health.errors))
    validate_configuration_drift(root)
    policy = load_policy(root, policy_override)
    baseline = compute_baseline(root)
    static = all_static_items(root)
    evidence_rows = aggregate_evidence(root, static, policy)
    atomic_write_json(root / "orchestration" / "baseline.json", baseline)
    atomic_write_json(root / "orchestration" / "inventory.json", build_inventory(root, baseline))
    atomic_write_json(root / "orchestration" / "evidence" / "inventory.json", evidence_rows)
    write_schemas(root)
    write_process_templates(root)
    registries = build_registries(root, baseline, static, policy)
    build_traceability(root, baseline, static, registries)
    verify_evidence_sources(root, baseline, static, evidence_rows, policy, online=False)
    run_consistency_checks(root, baseline, static, policy)
    write_registry_resolution_snapshot(root, baseline, static, registries)
    report = compute_status(root, baseline, policy, static, evidence_rows)
    atomic_write_json(root / "orchestration" / "reports" / "status.json", report)
    update_exit_manifest(root, "orchestration_refreshed", report)
    return baseline, policy, static, evidence_rows


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def print_status(report: dict[str, Any], verbose: bool = False) -> None:
    print(f"Baseline: {report['baseline_fingerprint']}")
    print("\nStage coverage:")
    for stage in STAGES:
        s = report["stages"][stage]
        g = report["gates"][stage]
        gate = "READY" if g["ready"] else "BLOCKED"
        local = report.get("item_readiness", {}).get(stage)
        local_text = f" item-ready={local['ready']}/{local['total']}" if local is not None else ""
        print(f"  {stage:12s} {s['valid']:5d}/{s['total']:<5d} {s['coverage']:6.1%}  gate={gate:7s}{local_text:18s}  {s['status_counts']}")
        if verbose and g["blocking_reasons"]:
            for x in g["blocking_reasons"]:
                print(f"      BLOCK: {x}")
        if verbose:
            for x in g.get("warnings") or []:
                print(f"      WARN:  {x}")
    ev = report["critical_evidence"]
    print(f"\nCritical evidence: {ev['resolved']}/{ev['total']} ({ev['resolution']:.1%})")
    if verbose and ev["unresolved"]:
        for x in ev["unresolved"][:100]:
            print(f"  unresolved {x['evidence_id']} {x.get('category')} {x.get('symbol') or ''} status={x['status']} confidence={x['confidence']}")
        if len(ev["unresolved"]) > 100:
            print(f"  ... {len(ev['unresolved'])-100} more")
    regs = report.get("registries") or {}
    if regs:
        print("\nRegistries: " + ", ".join(f"{k}={v}" for k, v in sorted(regs.items()) if isinstance(v, int)))
    tr = report.get("traceability") or {}
    if tr:
        print(f"Traceability: features={tr.get('features',0)} tests={tr.get('tests',0)} required-test-gaps={len(tr.get('required_test_gaps') or [])}")
    cons = report.get("consistency") or {}
    if cons:
        counts = cons.get("counts") or {}
        print("Consistency: " + ", ".join(f"{k}={v}" for k,v in sorted(counts.items())))


def audit_workspace(root: Path, baseline: dict[str, Any], policy: dict[str, Any], static: dict[str, list[WorkItem]], evidence_rows: Sequence[dict[str, Any]]) -> dict[str, Any]:
    """Non-regression audit: flag degradation without requiring project completion."""
    findings: list[dict[str, Any]] = []
    health = extraction_health(root)
    for e in health.errors:
        findings.append({"severity": "HIGH", "kind": "extraction", "message": e})
    for w in health.warnings:
        findings.append({"severity": "MEDIUM", "kind": "extraction", "message": w})

    # Existing accepted work may be incomplete, but it must not silently become
    # invalid/stale without being visible to the next iteration.
    for stage in STAGES:
        items = get_items_for_stage(stage, static, evidence_rows, root)
        for item in items:
            st = result_status(root, stage, item, baseline)
            if st["status"] == "INVALID":
                findings.append({"severity": "HIGH", "kind": "invalid_result", "stage": stage, "item_id": item.item_id, "errors": st.get("errors") or []})
            elif st["status"] == "STALE":
                findings.append({"severity": "MEDIUM", "kind": "stale_result", "stage": stage, "item_id": item.item_id, "reasons": st.get("stale_reasons") or []})

    trace = build_traceability(root, baseline, static, load_registries(root))
    for c in trace.get("test_id_conflicts") or []:
        findings.append({"severity": "HIGH", "kind": "test_id_conflict", **c})
    cons = run_consistency_checks(root, baseline, static, policy)
    for c in cons.get("findings") or []:
        if c.get("severity") in {"HIGH", "MEDIUM"}:
            findings.append({"kind": "architecture_consistency", **c})

    # Specification IDs are control-plane identifiers and must be unique.
    clauses = load_spec_clauses(root)
    dup_specs = [k for k, n in Counter(str(x.get("spec_clause_id")) for x in clauses).items() if k and n > 1]
    for sid in dup_specs:
        findings.append({"severity": "HIGH", "kind": "duplicate_spec_clause_id", "spec_clause_id": sid})

    # Stable function IDs likewise must be one-to-one within a baseline.
    funcs = load_functions(root)
    dup_fns = [k for k, n in Counter(_stable_function_id(x) for x in funcs).items() if k and n > 1]
    for sid in dup_fns:
        findings.append({"severity": "HIGH", "kind": "duplicate_function_stable_id", "stable_id": sid})

    verification = verify_evidence_sources(root, baseline, static, evidence_rows, policy, online=False)
    for x in verification.get("items") or []:
        if x.get("critical") and x.get("declared_status") == "VERIFIED" and not x.get("effective_verified"):
            findings.append({"severity": "HIGH", "kind": "unverified_critical_evidence", "evidence_id": x.get("evidence_id"), "required_authority": x.get("required_authority")})

    regs = load_registries(root)
    resolution = write_registry_resolution_snapshot(root, baseline, static, regs)
    counts = Counter(str(x.get("severity") or "INFO") for x in findings)
    report = {
        "generated_utc": now_iso(),
        "baseline_fingerprint": baseline["fingerprint"],
        "pass": counts.get("HIGH", 0) == 0,
        "counts": dict(counts),
        "findings": findings,
        "registry_resolution_counts": resolution.get("counts") or {},
        "traceability": {k: trace.get(k) for k in ("features", "tests", "links", "test_id_conflict_count", "required_test_gaps")},
    }
    atomic_write_json(root / "orchestration" / "reports" / "audit.json", report)
    return report


def next_stage(report: dict[str, Any]) -> str | None:
    for stage in STAGES:
        s = report["stages"][stage]
        g = report["gates"][stage]
        if g["ready"] and s["coverage"] < 1.0:
            return stage
    return None


# ---------------------------------------------------------------------------
# Cross-variant comparison packet (read-only helper)
# ---------------------------------------------------------------------------

def compare_variants(roots: Sequence[Path], out: Path) -> dict[str, Any]:
    rows = []
    fn_presence: dict[str, set[str]] = defaultdict(set)
    subsystem_counts: dict[str, dict[str, int]] = {}
    for root in roots:
        require_builder_root(root)
        project = load_json(root / "manifest" / "project.json", {}) or {}
        label = str(project.get("build_variant") or root.name)
        funcs = load_functions(root)
        for f in funcs:
            fn_presence[str(f.get("key"))].add(label)
        subsystem_counts[label] = dict(Counter(s for f in funcs for s in (f.get("subsystems") or [])))
        rows.append({
            "variant": label,
            "root": str(root),
            "baseline": compute_baseline(root),
            "summary": load_json(root / "manifest" / "summary.json", {}) or {},
        })
    all_labels = sorted(x["variant"] for x in rows)
    matrix = [
        {"function_key": key, "present_in": sorted(labels), "missing_from": sorted(set(all_labels) - labels)}
        for key, labels in sorted(fn_presence.items())
        if set(labels) != set(all_labels)
    ]
    result = {
        "generated_utc": now_iso(),
        "variants": rows,
        "subsystem_counts": subsystem_counts,
        "configuration_sensitive_functions": matrix,
    }
    atomic_write_json(out, result)
    return result



# ---------------------------------------------------------------------------
# Multi-variant semantic merge and implementation-delta analysis
# ---------------------------------------------------------------------------

def _stable_function_id(row: Mapping[str, Any]) -> str:
    return str(row.get("stable_id") or f"FN::{row.get('file')}::{row.get('name')}")


def _far_core(obj: Mapping[str, Any]) -> dict[str, Any]:
    return {k: obj.get(k) for k in (
        "architectural_responsibility", "feature_ids", "semantic_contract", "runtime",
        "synchronization", "portable_core", "source_os_specific", "source_os_dependencies",
        "target_design", "portability_class", "spec_clause_ids",
    )}


def semantic_merge_variants(roots: Sequence[Path], out_dir: Path) -> dict[str, Any]:
    if len(roots) < 2: raise ValueError("semantic merge requires at least two variants")
    out_dir.mkdir(parents=True, exist_ok=True)
    variant_data: dict[str, dict[str, Any]] = {}
    all_fns: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    feature_presence: dict[str, set[str]] = defaultdict(set)
    registry_groups: dict[str, dict[str, Any]] = {}

    for root in roots:
        require_builder_root(root)
        baseline = compute_baseline(root)
        static = all_static_items(root)
        base_label = _variant_label(root)
        label = base_label
        if label in variant_data:
            # Never collapse two roots merely because their human variant labels
            # are identical.  Preserve the configured label and disambiguate the
            # merge identity deterministically with the analysis-root name.
            suffix = safe_id(root.name) or "variant"
            label = f"{base_label}@{suffix}"
            n = 2
            while label in variant_data:
                label = f"{base_label}@{suffix}#{n}"
                n += 1
        fn_rows = load_functions(root)
        item_by_stable = {str(x.metadata.get("stable_id") or x.identity): x for x in static["methods"]}
        fars: dict[str, Any] = {}
        for sid, item in item_by_stable.items():
            obj = load_valid_result(root, "methods", item, baseline)
            if obj: fars[sid] = obj
        for f in fn_rows:
            sid = _stable_function_id(f)
            far = fars.get(sid)
            core = _far_core(far) if far else None
            all_fns[sid][label] = {
                "function_key": f.get("key"), "file": f.get("file"), "name": f.get("name"),
                "semantic_fingerprint": f.get("semantic_fingerprint") or f.get("source_sha256"),
                "source_sha256": f.get("source_sha256"),
                "preprocessor_conditions": f.get("preprocessor_conditions") or [],
                "far_core": core,
                "far_core_sha256": sha256_bytes(canonical_json(core)) if core else None,
                "far_confidence": far.get("confidence") if far else None,
            }
            if far:
                for fid in far.get("feature_ids") or []: feature_presence[str(fid)].add(label)
        arch = load_valid_result(root, "architecture", static["architecture"][0], baseline)
        if arch:
            for e in arch.get("feature_model") or []:
                fid = _feature_id_from_entry(e)
                if fid: feature_presence[fid].add(label)
        regs = load_registries(root)
        if not any(regs.values()):
            regs = build_registries(root, baseline, static, load_policy(root))
        for k in regs:
            for r in regs[k]:
                gid = str(r.get("duplicate_group_id") or (r.get("request_id") or r.get("risk_id")))
                g = registry_groups.setdefault(gid, {"group_id": gid, "kind": k, "variants": {}, "feature_ids": set()})
                g["variants"][label] = r
                g["feature_ids"].update(r.get("feature_ids") or [])
        variant_data[label] = {
            "root": str(root), "baseline_fingerprint": baseline["fingerprint"],
            "architecture_surface_fingerprint": architecture_surface_fingerprint(root),
            "function_count": len(fn_rows),
        }

    labels = sorted(variant_data)
    merged_rows = []
    review_rows = []
    for sid, per in sorted(all_fns.items()):
        present = sorted(per)
        source_fps = {x.get("semantic_fingerprint") for x in per.values() if x.get("semantic_fingerprint")}
        far_fps = {x.get("far_core_sha256") for x in per.values() if x.get("far_core_sha256")}
        if len(present) < len(labels) and len(source_fps) <= 1:
            cls = "conditional_identical"
        elif len(source_fps) <= 1 and len(far_fps) <= 1:
            cls = "common_identical"
        elif len(source_fps) <= 1 and len(far_fps) > 1:
            cls = "analysis_divergence"
        elif len(source_fps) > 1 and len(far_fps) <= 1:
            cls = "variant_source_semantics"
        else:
            cls = "mixed_divergence"
        common_far = None
        if len(far_fps) == 1:
            common_far = next((x.get("far_core") for x in per.values() if x.get("far_core")), None)
        row = {
            "stable_id": sid, "classification": cls,
            "present_in": present, "missing_from": sorted(set(labels)-set(present)),
            "variants": per, "merged_far_core": common_far,
            "requires_merge_review": cls in {"analysis_divergence", "variant_source_semantics", "mixed_divergence"},
        }
        merged_rows.append(row)
        if row["requires_merge_review"]: review_rows.append(row)

    features = [{"feature_id": fid, "present_in": sorted(vs), "missing_from": sorted(set(labels)-vs)} for fid, vs in sorted(feature_presence.items())]
    reg_rows = []
    for g in registry_groups.values():
        g = dict(g); g["feature_ids"] = sorted(g["feature_ids"]); reg_rows.append(g)
    atomic_write_jsonl(out_dir / "functions.jsonl", merged_rows)
    atomic_write_jsonl(out_dir / "semantic_review_queue.jsonl", review_rows)
    atomic_write_jsonl(out_dir / "features.jsonl", features)
    atomic_write_jsonl(out_dir / "registry_groups.jsonl", sorted(reg_rows, key=lambda x: x["group_id"]))
    summary = {
        "generated_utc": now_iso(), "variants": variant_data,
        "function_count": len(merged_rows), "review_required": len(review_rows),
        "classification_counts": dict(Counter(x["classification"] for x in merged_rows)),
        "feature_count": len(features), "registry_group_count": len(reg_rows),
    }
    atomic_write_json(out_dir / "summary.json", summary)
    prompt = render_template(template_path(TEMPLATES, "orchestrator.variant_merge"), {
        "VARIANTS": ", ".join(labels),
        "SUMMARY_JSON": json.dumps(summary, indent=2, sort_keys=True),
    })
    atomic_write_text(out_dir / "merge_review_prompt.md", prompt)
    return summary


def _function_maps(root: Path) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    rows = load_functions(root)
    by_stable = {_stable_function_id(x): x for x in rows}
    key_to_stable = {str(x.get("key")): sid for sid, x in by_stable.items()}
    return by_stable, key_to_stable


def implementation_delta(old_root: Path, new_root: Path) -> dict[str, Any]:
    old_f, old_key = _function_maps(old_root)
    new_f, new_key = _function_maps(new_root)
    old_ids, new_ids = set(old_f), set(new_f)
    added, removed = new_ids-old_ids, old_ids-new_ids
    modified = {sid for sid in old_ids & new_ids if (old_f[sid].get("semantic_fingerprint") or old_f[sid].get("source_sha256")) != (new_f[sid].get("semantic_fingerprint") or new_f[sid].get("source_sha256"))}
    initial = set(added) | set(modified)

    # Reverse direct-call dependency graph over both revisions.
    reverse: dict[str, set[str]] = defaultdict(set)
    for rows, keymap in ((old_f, old_key), (new_f, new_key)):
        for sid, f in rows.items():
            for ck in f.get("calls_internal") or []:
                callee_sid = keymap.get(str(ck))
                if callee_sid: reverse[callee_sid].add(sid)
    impacted = set(initial)
    q = list(initial | removed)
    while q:
        changed = q.pop()
        for caller in reverse.get(changed, set()):
            if caller in new_ids and caller not in impacted:
                impacted.add(caller); q.append(caller)

    # Shared-state coupling: a changed writer can affect readers/writers of the same canonical path.
    state_users: dict[str, set[str]] = defaultdict(set)
    changed_state: set[str] = set()
    for sid, f in new_f.items():
        for a in f.get("field_accesses") or []:
            path = str(a.get("object_path") or a.get("field") or "")
            if path: state_users[path].add(sid)
            if sid in impacted and str(a.get("mode")) in {"write", "readwrite", "address_taken"} and path:
                changed_state.add(path)
    for p in changed_state: impacted.update(state_users[p])

    impacted_files = sorted({str(new_f[s].get("file")) for s in impacted if s in new_f})
    impacted_subsystems = sorted({x for s in impacted if s in new_f for x in (new_f[s].get("subsystems") or [])})
    architecture_changed = architecture_surface_fingerprint(old_root) != architecture_surface_fingerprint(new_root)
    return {
        "generated_utc": now_iso(),
        "old_root": str(old_root), "new_root": str(new_root),
        "old_baseline": compute_baseline(old_root)["fingerprint"], "new_baseline": compute_baseline(new_root)["fingerprint"],
        "architecture_surface_changed": architecture_changed,
        "added_functions": sorted(added), "removed_functions": sorted(removed), "modified_functions": sorted(modified),
        "impacted_functions": sorted(impacted), "unaffected_functions": sorted(new_ids-impacted),
        "impacted_files": impacted_files, "impacted_subsystems": impacted_subsystems,
        "changed_state_paths": sorted(changed_state),
    }


def _copy_result_via_ingest(old_root: Path, new_root: Path, old_stage: str, old_item: WorkItem, new_item: WorkItem,
                            old_baseline: dict[str, Any], new_baseline: dict[str, Any], new_policy: dict[str, Any],
                            new_static: dict[str, list[WorkItem]], new_evidence: Sequence[dict[str, Any]]) -> bool:
    if result_status(old_root, old_stage, old_item, old_baseline)["status"] != "VALID": return False
    obj = load_json(result_path(old_root, old_stage, old_item.item_id), {}) or {}
    obj, _ = normalize_result_v2(old_stage, obj)
    if old_stage == "architecture": obj["baseline_fingerprint"] = new_baseline["fingerprint"]
    if old_stage == "methods": obj["function_key"] = new_item.identity
    if old_stage == "final": obj["baseline_fingerprint"] = new_baseline["fingerprint"]
    # Render current prompt directly; delta migration intentionally does not use global stage gate.
    render_item(new_root, old_stage, new_item, new_baseline, new_policy, new_static, new_evidence)
    tmp = new_root / "orchestration" / "raw_results" / old_stage / f"{safe_id(new_item.item_id)}.delta.json"
    atomic_write_json(tmp, obj)
    vr = ingest_result(new_root, old_stage, new_item, tmp, new_baseline)
    if vr.valid:
        mp = result_meta_path(new_root, old_stage, new_item.item_id)
        meta = load_json(mp, {}) or {}
        meta["carried_forward_from"] = {"root": str(old_root), "item_id": old_item.item_id, "result_sha256": _result_sha(old_root, old_stage, old_item.item_id)}
        atomic_write_json(mp, meta)
    return vr.valid


def carry_forward_delta(old_root: Path, new_root: Path, delta: Mapping[str, Any]) -> dict[str, Any]:
    # Specifications and policy are project inputs, not analysis results. Preserve
    # them across source revisions before dependency fingerprints are evaluated.
    old_specs = old_root / "orchestration" / "specifications"
    new_specs = new_root / "orchestration" / "specifications"
    if old_specs.is_dir() and not new_specs.exists():
        shutil.copytree(old_specs, new_specs)
    old_policy_path = old_root / "orchestration" / "policy.json"
    new_policy_path = new_root / "orchestration" / "policy.json"
    if old_policy_path.is_file() and not new_policy_path.exists():
        new_policy_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(old_policy_path, new_policy_path)
    # New workspace must use V2 schemas/policy before migration.
    initialize(new_root)
    old_baseline, old_policy, old_static, old_ev = refresh_workspace(old_root)
    new_baseline, new_policy, new_static, new_ev = refresh_workspace(new_root)
    impacted = set(delta.get("impacted_functions") or [])
    carried = Counter(); skipped = []

    # Architecture can be reused only when its architecture surface is unchanged.
    if not delta.get("architecture_surface_changed"):
        if _copy_result_via_ingest(old_root, new_root, "architecture", old_static["architecture"][0], new_static["architecture"][0], old_baseline, new_baseline, new_policy, new_static, new_ev):
            carried["architecture"] += 1
    new_by_stable = {str(x.metadata.get("stable_id") or x.identity): x for x in new_static["methods"]}
    old_by_stable = {str(x.metadata.get("stable_id") or x.identity): x for x in old_static["methods"]}
    for sid in sorted(set(new_by_stable) & set(old_by_stable) - impacted):
        if _copy_result_via_ingest(old_root, new_root, "methods", old_by_stable[sid], new_by_stable[sid], old_baseline, new_baseline, new_policy, new_static, new_ev):
            carried["methods"] += 1
    # Rebuild registries/evidence after migrated FARs.
    new_baseline, new_policy, new_static, new_ev = refresh_workspace(new_root)

    # Evidence with stable IDs and no impacted affected function can be reused.
    old_ev_items = {x.item_id: x for x in evidence_items(old_root, old_ev)}
    new_ev_items = {x.item_id: x for x in evidence_items(new_root, new_ev)}
    new_method_stable = {str(x.identity): str(x.metadata.get("stable_id") or x.identity) for x in new_static["methods"]}
    for eid in sorted(set(old_ev_items) & set(new_ev_items)):
        affected_stable = {new_method_stable.get(str(k), str(k)) for k in new_ev_items[eid].metadata.get("affected_functions") or []}
        if impacted.intersection(affected_stable): continue
        if _copy_result_via_ingest(old_root, new_root, "evidence", old_ev_items[eid], new_ev_items[eid], old_baseline, new_baseline, new_policy, new_static, new_ev):
            carried["evidence"] += 1
    new_baseline, new_policy, new_static, new_ev = refresh_workspace(new_root)

    impacted_files = set(delta.get("impacted_files") or [])
    old_files = {str(x.identity): x for x in old_static["files"]}; new_files = {str(x.identity): x for x in new_static["files"]}
    for name in sorted(set(old_files)&set(new_files)-impacted_files):
        if _copy_result_via_ingest(old_root, new_root, "files", old_files[name], new_files[name], old_baseline, new_baseline, new_policy, new_static, new_ev):
            carried["files"] += 1
    new_baseline, new_policy, new_static, new_ev = refresh_workspace(new_root)

    impacted_subs = set(delta.get("impacted_subsystems") or [])
    old_sub = {str(x.identity): x for x in old_static["subsystems"]}; new_sub = {str(x.identity): x for x in new_static["subsystems"]}
    for name in sorted(set(old_sub)&set(new_sub)-impacted_subs):
        if _copy_result_via_ingest(old_root, new_root, "subsystems", old_sub[name], new_sub[name], old_baseline, new_baseline, new_policy, new_static, new_ev):
            carried["subsystems"] += 1
    refresh_workspace(new_root)
    return {"carried_forward": dict(carried), "skipped": skipped}

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def find_item(items: Sequence[WorkItem], item_id: str) -> WorkItem:
    matches = [x for x in items if x.item_id == item_id or x.identity == item_id]
    if not matches:
        raise SystemExit(f"Unknown item {item_id!r}")
    if len(matches) > 1:
        raise SystemExit(f"Ambiguous item {item_id!r}: {[x.item_id for x in matches]}")
    return matches[0]


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    default_manifest = Path(__file__).resolve().parent / "config" / "default.manifest.json"
    ap = argparse.ArgumentParser(
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description="Orchestrate schema-validated production NIC-driver port analysis from one manifest entrypoint.",
    )
    ap.add_argument("--manifest", default=str(default_manifest), help="Single input manifest entrypoint used by builder and orchestrator")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def root_arg(p: argparse.ArgumentParser) -> None:
        p.add_argument("--root", help="Override manifest output.root")
        p.add_argument("--policy", help="Optional emergency policy overlay; prefer manifest configuration.orchestration_policy")

    def llm_args(p: argparse.ArgumentParser) -> None:
        p.add_argument("--llm-provider", choices=provider_names(), help=f"LLM provider (default: policy llm.default_provider or {DEFAULT_PROVIDER})")
        p.add_argument("--llm-model", help="Model id; run 'list-models' to discover available ids")
        p.add_argument("--llm-token", help="Explicit provider token; never written to the credential cache")
        p.add_argument("--llm-device-flow", action="store_true", help="Authenticate interactively once; the token is cached locally and reused")
        p.add_argument("--llm-api-base", help="Override the provider API base URL")
        p.add_argument("--llm-cache", help=f"Credential cache file (default: {default_cache_path()})")
        p.add_argument("--no-llm-cache", action="store_true", help="Neither read nor write the local credential cache")
        p.add_argument("--llm-verbose", action="store_true", help="Report token acquisition on stderr")

    p = sub.add_parser("init", help="Initialize/refresh orchestration workspace")
    root_arg(p)

    p = sub.add_parser("refresh", help="Refresh inventory/baseline/evidence and report staleness")
    root_arg(p)

    p = sub.add_parser("status", help="Show coverage, validity, staleness and stage gates")
    root_arg(p)
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--json", action="store_true", help="Emit JSON instead of text")

    p = sub.add_parser("render", help="Render enriched prompts for one stage")
    root_arg(p)
    p.add_argument("--stage", required=True)
    p.add_argument("--item", help="Render only one item ID/identity")
    p.add_argument("--force", action="store_true", help="Render despite a blocked stage gate (result will still be governed by validation/staleness)")

    p = sub.add_parser("ingest", help="Ingest and validate one externally produced result")
    root_arg(p)
    p.add_argument("--stage", required=True)
    p.add_argument("--item", required=True, help="Item ID or identity")
    p.add_argument("--result", required=True, help="Raw model/research result file")

    p = sub.add_parser("validate", help="Revalidate stored results and provenance")
    root_arg(p)
    p.add_argument("--stage", )
    p.add_argument("--item")
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("run", help="Synchronously execute ready enriched prompts through an external CLI runner or a registered LLM provider")
    root_arg(p)
    p.add_argument("--stage", required=True)
    p.add_argument("--item", help="Run only one item ID/identity")
    p.add_argument("--runner", help="argv template; allowed placeholders: {prompt} {root} {stage} {item_id}; stdout must contain result. Omit to use the configured LLM provider")
    llm_args(p)
    p.add_argument("--jobs", type=int)
    p.add_argument("--timeout", type=int)
    p.add_argument("--retries", type=int)
    p.add_argument("--force", action="store_true", help="Run despite gate block")
    p.add_argument("--rerun-valid", action="store_true", help="Also rerun currently VALID/non-stale items")

    p = sub.add_parser("list-models", help="List models the authenticated account may use; run before choosing --llm-model")
    llm_args(p)
    p.add_argument("--json", action="store_true")
    p.add_argument("--raw", action="store_true", help="Print the unmodified provider response")

    p = sub.add_parser("llm-auth", help="Inspect, create or clear locally cached provider credentials")
    llm_args(p)
    p.add_argument("--login", action="store_true", help="Acquire and cache a token now")
    p.add_argument("--clear", action="store_true", help="Delete cached credentials for the selected provider")

    p = sub.add_parser("capabilities", help="Show authorized MCP servers/tools and skills ingested from the specification manifest")
    root_arg(p)
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("audit", help="Run non-regression audit without requiring project completion")
    root_arg(p)
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("next", help="Print the next stage whose gate is ready and coverage incomplete")
    root_arg(p)

    p = sub.add_parser("ingest-spec", help="Ingest specification manifest and create stable clause IDs")
    root_arg(p)
    p.add_argument("--spec-manifest", required=True, help="Specification manifest to ingest")

    p = sub.add_parser("verify-evidence", help="Verify evidence citations and score source authority")
    root_arg(p)
    p.add_argument("--online", action="store_true", help="Also perform best-effort URL reachability checks")

    p = sub.add_parser("traceability", help="Rebuild and report feature/spec/method/file/test/request/risk traceability")
    root_arg(p)
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("registries", help="Show script-owned request and risk registries")
    root_arg(p)
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("consistency", help="Run cross-FAR architecture consistency checks")
    root_arg(p)
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("merge-variants", help="Semantically merge analyzed build variants using stable function IDs")
    p.add_argument("--roots", nargs="+", required=True)
    p.add_argument("--out-dir", required=True)

    p = sub.add_parser("delta", help="Compute affected analysis region between source/build baselines")
    p.add_argument("--old", required=True, help="Old builder/orchestration root")
    p.add_argument("--new", required=True, help="New builder root")
    p.add_argument("--out", required=True, help="Delta report JSON")
    p.add_argument("--carry-forward", action="store_true", help="Migrate validated unaffected analysis into the new root")

    p = sub.add_parser("compare-variants", help="Build a read-only cross-variant extraction comparison packet")
    p.add_argument("--roots", nargs="+", required=True, help="Two or more builder output roots")
    p.add_argument("--out", required=True, help="Output JSON path")

    return ap.parse_args(argv)


def _resolve_manifest_path_value(manifest: dict[str, Any], dotted: str) -> Path | None:
    value = get_path(manifest, dotted)
    if value in (None, ""):
        return None
    p = Path(str(value)).expanduser()
    if not p.is_absolute():
        p = Path(manifest["_manifest"]["directory"]) / p
    return p.resolve()


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        manifest_path = Path(args.manifest).expanduser().resolve()
        manifest = configure_from_manifest(manifest_path)
    except (ManifestError, OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: input manifest: {exc}", file=sys.stderr)
        return 2

    if getattr(args, "stage", None) and args.stage not in STAGES:
        print(f"ERROR: stage {args.stage!r} is not configured; expected one of {list(STAGES)}", file=sys.stderr)
        return 2

    if args.cmd in {"list-models", "llm-auth"}:
        # Provider inspection/authentication is workspace independent.
        try:
            provider = build_llm_provider(args, DEFAULT_POLICY)
            if args.cmd == "llm-auth":
                if args.clear:
                    print(json.dumps({"cleared": provider.clear_cache(), "cache_path": str(provider.cache.path)}, indent=2))
                if args.login:
                    provider.get_token()
                print(json.dumps(provider.auth_status(), indent=2, sort_keys=True))
                return 0
            models = provider.list_models()
            if args.raw:
                print(json.dumps(models, indent=2, sort_keys=True))
            elif args.json:
                print(json.dumps(summarize_models(models), indent=2, sort_keys=True))
            else:
                print_models(summarize_models(models))
            return 0
        except ProviderError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 3

    if args.cmd == "merge-variants":
        roots = [Path(x).expanduser().resolve() for x in args.roots]
        res = semantic_merge_variants(roots, Path(args.out_dir).expanduser().resolve())
        print(json.dumps(res, indent=2, sort_keys=True))
        return 0

    if args.cmd == "delta":
        old_root = Path(args.old).expanduser().resolve(); new_root = Path(args.new).expanduser().resolve()
        report = implementation_delta(old_root, new_root)
        if args.carry_forward:
            report["carry_forward"] = carry_forward_delta(old_root, new_root, report)
        atomic_write_json(Path(args.out).expanduser().resolve(), report)
        print(json.dumps(report, indent=2, sort_keys=True))
        return 0

    if args.cmd == "compare-variants":
        roots = [Path(x).expanduser().resolve() for x in args.roots]
        if len(roots) < 2:
            raise SystemExit("compare-variants requires at least two roots")
        result = compare_variants(roots, Path(args.out).expanduser().resolve())
        print(json.dumps({"variants": len(result["variants"]), "configuration_sensitive_functions": len(result["configuration_sensitive_functions"]), "out": args.out}, indent=2))
        return 0

    root = Path(args.root).expanduser().resolve() if getattr(args, "root", None) else _resolve_manifest_path_value(manifest, "output.root")
    if root is None:
        print("ERROR: manifest output.root is required for this command", file=sys.stderr)
        return 2
    policy_override = Path(args.policy).expanduser().resolve() if getattr(args, "policy", None) else None

    with workspace_lock(root):
        if args.cmd == "init":
            res = initialize(root, policy_override)
            print(json.dumps(res, indent=2, sort_keys=True))
            return 0

        baseline, policy, static, evidence_rows = refresh_workspace(root, policy_override)

        if args.cmd == "ingest-spec":
            res = ingest_spec_manifest(root, Path(args.spec_manifest).expanduser().resolve())
            baseline, policy, static, evidence_rows = refresh_workspace(root, policy_override)
            print(json.dumps(res, indent=2, sort_keys=True))
            return 0

        if args.cmd == "verify-evidence":
            res = verify_evidence_sources(root, baseline, static, evidence_rows, policy, online=args.online)
            print(json.dumps(res, indent=2, sort_keys=True))
            return 0

        if args.cmd == "traceability":
            regs = build_registries(root, baseline, static, policy)
            res = build_traceability(root, baseline, static, regs)
            print(json.dumps(res, indent=2, sort_keys=True))
            return 0

        if args.cmd == "registries":
            regs = build_registries(root, baseline, static, policy)
            if args.json:
                print(json.dumps(regs, indent=2, sort_keys=True))
            else:
                print(json.dumps({k: len(v) for k,v in regs.items()}, indent=2, sort_keys=True))
            return 0

        if args.cmd == "consistency":
            res = run_consistency_checks(root, baseline, static, policy)
            print(json.dumps(res, indent=2, sort_keys=True))
            return 0

        if args.cmd == "capabilities":
            ctx = capability_context(root)
            if args.json:
                print(json.dumps(ctx, indent=2, sort_keys=True))
            else:
                for label, key in (("MCP/tools", "authorized_mcp_and_tools"), ("Skills", "authorized_skills")):
                    rows = ctx[key]
                    print(f"{label}: {len(rows)}")
                    for x in rows:
                        print(f"  {x['capability_id']:<40} {x.get('path') or ''}")
            return 0

        if args.cmd == "refresh":
            report = compute_status(root, baseline, policy, static, evidence_rows)
            atomic_write_json(root / "orchestration" / "reports" / "status.json", report)
            print_status(report, verbose=True)
            return 0

        if args.cmd == "status":
            report = compute_status(root, baseline, policy, static, evidence_rows)
            atomic_write_json(root / "orchestration" / "reports" / "status.json", report)
            if args.json:
                print(json.dumps(report, indent=2, sort_keys=True))
            else:
                print_status(report, verbose=args.verbose)
            return 0

        if args.cmd == "audit":
            report = audit_workspace(root, baseline, policy, static, evidence_rows)
            if args.json:
                print(json.dumps(report, indent=2, sort_keys=True))
            else:
                print(f"audit pass={report['pass']} counts={report['counts']}")
                for x in report["findings"]:
                    print(f"  {x.get('severity','INFO'):6s} {x.get('kind')}: {x.get('message') or x.get('item_id') or x.get('evidence_id') or x.get('stable_id') or x.get('spec_clause_id') or ''}")
            return 0 if report["pass"] else 9

        if args.cmd == "next":
            report = compute_status(root, baseline, policy, static, evidence_rows)
            stage = next_stage(report)
            print(stage or "COMPLETE_OR_BLOCKED")
            return 0

        items = get_items_for_stage(args.stage, static, evidence_rows, root) if hasattr(args, "stage") and args.stage else []

        if args.cmd == "render":
            selected = [find_item(items, args.item)] if args.item else list(items)
            blocked_items = []
            if args.item or args.stage in {"architecture", "methods", "final"}:
                gate = gate_for_item(root, args.stage, selected[0], baseline, policy, static, evidence_rows) if args.item else gate_for_stage(root, args.stage, baseline, policy, static, evidence_rows)
                if not gate["ready"] and not args.force:
                    print(json.dumps({"stage": args.stage, "blocked": True, "gate": gate}, indent=2))
                    return 5
            elif not args.force:
                ready = []
                for wi in selected:
                    ig = gate_for_item(root, args.stage, wi, baseline, policy, static, evidence_rows)
                    if ig["ready"]: ready.append(wi)
                    else: blocked_items.append({"item_id": wi.item_id, "identity": wi.identity, "gate": ig})
                selected = ready
                if not selected:
                    print(json.dumps({"stage": args.stage, "blocked": True, "reason": "no item-local work is ready", "blocked_items": blocked_items}, indent=2))
                    return 5
            rendered = []
            for item in selected:
                p, meta = render_item(root, args.stage, item, baseline, policy, static, evidence_rows)
                rendered.append({"item_id": item.item_id, "identity": item.identity, "path": str(p), "sha256": meta["prompt_sha256"]})
            print(json.dumps({"stage": args.stage, "rendered": len(rendered), "items": rendered, "blocked_items": blocked_items}, indent=2))
            return 0

        if args.cmd == "ingest":
            item = find_item(items, args.item)
            # Ensure current enriched prompt exists; rendering is safe and makes
            # provenance deterministic. Gate is not bypassed silently.
            if not enriched_prompt_path(root, args.stage, item.item_id).exists():
                gate = gate_for_item(root, args.stage, item, baseline, policy, static, evidence_rows)
                if not gate["ready"]:
                    print(json.dumps({"valid": False, "errors": ["stage gate is blocked"], "gate": gate}, indent=2))
                    return 5
                render_item(root, args.stage, item, baseline, policy, static, evidence_rows)
            raw = Path(args.result).expanduser().resolve()
            if not raw.is_file():
                raise SystemExit(f"Result file not found: {raw}")
            vr = ingest_result(root, args.stage, item, raw, baseline)
            if vr.valid:
                # Immediately materialize registries, traceability, citation scoring and consistency
                # so the next agent sees the value added by this result without a manual refresh.
                refresh_workspace(root, policy_override)
            print(json.dumps(dataclasses.asdict(vr), indent=2))
            return 0 if vr.valid else 6

        if args.cmd == "validate":
            stages = [args.stage] if args.stage else list(STAGES)
            rows = []
            for stage in stages:
                its = get_items_for_stage(stage, static, evidence_rows, root)
                if args.item:
                    its = [find_item(its, args.item)]
                for item in its:
                    rows.append({"stage": stage, "item_id": item.item_id, "identity": item.identity, **result_status(root, stage, item, baseline)})
            invalid = [x for x in rows if x["status"] not in {"VALID", "MISSING"}]
            if args.json:
                print(json.dumps(rows, indent=2, sort_keys=True))
            else:
                for x in rows:
                    print(f"{x['stage']:12s} {x['status']:8s} {x['item_id']} confidence={x.get('confidence')}")
                    for err in x.get("errors") or []:
                        print(f"  ERROR: {err}")
                    for why in x.get("stale_reasons") or []:
                        print(f"  STALE: {why}")
            return 7 if invalid else 0

        if args.cmd == "run":
            selected = [find_item(items, args.item)] if args.item else list(items)
            blocked_items = []
            if args.item or args.stage in {"architecture", "methods", "final"}:
                gate = gate_for_item(root, args.stage, selected[0], baseline, policy, static, evidence_rows) if args.item else gate_for_stage(root, args.stage, baseline, policy, static, evidence_rows)
                if not gate["ready"] and not args.force:
                    print(json.dumps({"stage": args.stage, "blocked": True, "gate": gate}, indent=2))
                    return 5
            elif not args.force:
                ready = []
                for wi in selected:
                    ig = gate_for_item(root, args.stage, wi, baseline, policy, static, evidence_rows)
                    if ig["ready"]: ready.append(wi)
                    else: blocked_items.append({"item_id": wi.item_id, "identity": wi.identity, "gate": ig})
                selected = ready
                if not selected:
                    print(json.dumps({"stage": args.stage, "blocked": True, "reason": "no item-local work is ready", "blocked_items": blocked_items}, indent=2))
                    return 5
            # Render all selected prompts first, then evaluate status.  This is
            # essential for detecting upstream-driven staleness.
            for item in selected:
                render_item(root, args.stage, item, baseline, policy, static, evidence_rows)
            if not args.rerun_valid:
                selected = [x for x in selected if result_status(root, args.stage, x, baseline)["status"] != "VALID"]
            jobs = args.jobs if args.jobs is not None else int(policy["runner"].get("jobs", 1))
            timeout = args.timeout if args.timeout is not None else int(policy["runner"].get("timeout_seconds", 1800))
            retries = args.retries if args.retries is not None else int(policy["runner"].get("retries", 1))
            if args.runner:
                execute = command_executor(args.runner)
                execution = {"mode": "command", "runner": args.runner}
            else:
                try:
                    provider = build_llm_provider(args, policy)
                    provider.get_token()
                except ProviderError as exc:
                    print(f"ERROR: {exc}", file=sys.stderr)
                    return 3
                model = args.llm_model or provider.default_model
                if not model:
                    print("ERROR: no model selected; pass --llm-model or configure policy llm.providers.<name>.model", file=sys.stderr)
                    return 3
                execute = provider_executor(provider, model)
                execution = {"mode": "provider", "provider": provider.name, "model": model}
            results = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
                futs = [pool.submit(run_one, root, args.stage, x, execute, timeout, baseline, max(0, retries)) for x in selected]
                for fut in concurrent.futures.as_completed(futs):
                    results.append(fut.result())
                    r = results[-1]
                    print(f"[{ 'OK' if r.get('ok') else 'FAIL' }] {r.get('item_id')}", file=sys.stderr)
            baseline, policy, static, evidence_rows = refresh_workspace(root, policy_override)
            report = compute_status(root, baseline, policy, static, evidence_rows)
            atomic_write_json(root / "orchestration" / "reports" / "status.json", report)
            failures = [x for x in results if not x.get("ok")]
            print(json.dumps({"stage": args.stage, "execution": execution, "attempted": len(results), "failures": len(failures), "results": results}, indent=2))
            return 8 if failures else 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
