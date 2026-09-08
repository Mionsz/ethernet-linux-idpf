#!/usr/bin/env python3
"""Single manifest-driven entrypoint for NIC Port Framework V3.3."""
from __future__ import annotations
import argparse, json, subprocess, sys
from pathlib import Path

from nic_port_manifest import (
    ManifestError, get_path, load_manifest, validate_manifest_basics,
    load_semantic_rules, load_policy_from_manifest, load_template_index,
    load_api_catalog, load_linux_specific_rules, load_include_classification,
    load_portability_scoring, load_tooling_normalization, load_report_layout,
    load_context_layout, load_framework_model, load_output_catalog,
    load_extraction_runtime,
)


def run(argv: list[str]) -> int:
    return subprocess.run(argv, check=False).returncode


def run_bootstrap(manifest: dict, manifest_path: Path) -> int:
    steps = list(get_path(manifest, "bootstrap.steps", []) or [])
    if not steps:
        print(json.dumps({"bootstrap":"NO_STEPS","entrypoint":str(manifest_path)}, indent=2))
        return 0
    import os
    for idx, step in enumerate(steps, 1):
        if not isinstance(step, dict) or step.get("enabled", True) is False:
            continue
        argv = [str(x) for x in (step.get("argv") or [])]
        if not argv:
            print(f"ERROR: bootstrap step {step.get('id', idx)!r} has empty argv", file=sys.stderr)
            return 2
        cwd = None
        if step.get("cwd"):
            cwd = Path(str(step["cwd"])).expanduser()
            if not cwd.is_absolute(): cwd = manifest_path.parent / cwd
            cwd = cwd.resolve()
        env = dict(os.environ)
        env.update({str(k):str(v) for k,v in (step.get("env") or {}).items()})
        print(f"BOOTSTRAP [{idx}/{len(steps)}] {step.get('id') or idx}: {step.get('description') or ''}")
        cp = subprocess.run(argv, cwd=str(cwd) if cwd else None, env=env, check=False)
        if cp.returncode and not bool(step.get("allow_failure", False)):
            print(f"ERROR: bootstrap step {step.get('id') or idx} failed rc={cp.returncode}", file=sys.stderr)
            return cp.returncode
    return 0


def _read_json(path: Path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return default


def _read_jsonl(path: Path) -> list[dict]:
    rows=[]
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip(): rows.append(json.loads(line))
    except Exception:
        pass
    return rows


def diagnose(root: Path) -> int:
    run_state = _read_json(root / "manifest/run_state.json", {})
    quality = _read_json(root / "manifest/extraction_quality.json", {})
    checkpoint = _read_json(root / "manifest/extraction_checkpoint.json", {})
    budget = _read_json(root / "manifest/artifact_budget.json", {})
    preflight = _read_json(root / "manifest/preflight.json", {})
    diagnostics = _read_jsonl(root / "manifest/translation_unit_diagnostics.jsonl")
    failed = [x for x in diagnostics if x.get("status") != "PASS"]
    payload = {
        "run_state": run_state,
        "extraction_quality": quality,
        "checkpoint": {
            "translation_units_total": checkpoint.get("translation_units_total"),
            "translation_units_recorded": checkpoint.get("translation_units_recorded"),
            "cache_hits": sum(1 for x in (checkpoint.get("entries") or []) if x.get("cache_hit")),
        },
        "artifact_budget": budget,
        "preflight": preflight,
        "failed_or_partial_translation_units": failed,
        "next_action": (
            "Extraction is healthy; continue with status/next."
            if run_state.get("status") == "COMPLETE" and quality.get("status") == "PASS"
            else "Resolve failed TU diagnostics, then run 'resume'. Successful matching TUs will be reused from the semantic cache."
        ),
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if not failed and quality.get("status") == "PASS" else 3


def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve().parent
    default_manifest = here / "config/default.manifest.json"
    ap = argparse.ArgumentParser(description="NIC Port Framework V3.3 manifest-driven entrypoint")
    ap.add_argument("--manifest", default=str(default_manifest), help="Input manifest entrypoint")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("validate-manifest")
    sub.add_parser("bootstrap")
    sub.add_parser("build", help="Run deterministic extraction only")
    sub.add_parser("prepare", help="Run a sequence of bootstrap, build, then init-orchestration")
    sub.add_parser("resume", help="Resume extraction without bootstrap; reuse valid cached TUs, then initialize orchestration")
    sub.add_parser("init-orchestration")
    p=sub.add_parser("status"); p.add_argument("--verbose", action="store_true")
    sub.add_parser("next")
    sub.add_parser("audit")
    sub.add_parser("diagnose-extraction")
    args=ap.parse_args(argv)

    manifest_path=Path(args.manifest).expanduser().resolve()
    try:
        manifest=load_manifest(manifest_path, strict_env=True)
        errors=validate_manifest_basics(manifest)
        if errors: raise ManifestError("; ".join(errors))
        semantic_rules=load_semantic_rules(manifest); policy=load_policy_from_manifest(manifest)
        templates=load_template_index(manifest); api_catalog=load_api_catalog(manifest)
        linux_specific=load_linux_specific_rules(manifest); include_rules=load_include_classification(manifest)
        scoring=load_portability_scoring(manifest); tooling=load_tooling_normalization(manifest)
        report_layout=load_report_layout(manifest); context_layout=load_context_layout(manifest)
        framework_model=load_framework_model(manifest); output_catalog=load_output_catalog(manifest)
        extraction_runtime=load_extraction_runtime(manifest)
        conflicts=api_catalog.get("_conflicts") or []
        if conflicts and str((framework_model.get("configuration_integrity") or {}).get("api_catalog_conflict_policy") or "error") == "error":
            raise ManifestError(f"API catalogue has {len(conflicts)} conflicting exact-symbol redefinition(s)")
    except Exception as exc:
        print(f"ERROR: manifest: {exc}", file=sys.stderr); return 2

    out_root=Path(str(get_path(manifest,"output.root"))).expanduser().resolve() if get_path(manifest,"output.root") else None
    if args.cmd == "validate-manifest":
        print(json.dumps({
            "valid":True,"entrypoint":str(manifest_path),"project":manifest.get("project"),"inputs":manifest.get("inputs"),"output":manifest.get("output"),
            "configuration":manifest.get("configuration"),"semantic_categories":sorted((semantic_rules.get("linux_api_patterns") or {}).keys()),
            "exact_api_catalog_entries":len(api_catalog.get("entries") or {}),"api_catalog_conflicts":len(conflicts),
            "linux_specific_feature_families":sorted((linux_specific.get("families") or {}).keys()),
            "include_classification_loaded":bool(include_rules),"portability_scoring_loaded":bool(scoring),"tooling_normalization_loaded":bool(tooling),
            "report_layout_loaded":bool(report_layout),"context_layout_loaded":bool(context_layout),"framework_stages":list(framework_model.get("stages") or []),
            "output_catalog_sections":sorted(k for k in output_catalog if not str(k).startswith("_")),"policy_loaded":bool(policy),
            "extraction_runtime_loaded":bool(extraction_runtime),"cache_enabled":bool((extraction_runtime.get("cache") or {}).get("enabled",False)),
            "template_count":len([k for k in templates if k != "__index__"]),
        },indent=2,sort_keys=True)); return 0
    if args.cmd == "diagnose-extraction":
        if out_root is None: print("ERROR: output.root is required",file=sys.stderr); return 2
        return diagnose(out_root)

    builder=here / "nic_port_context_builder.py"; orch=here / "nic_port_orchestrator.py"; base=[sys.executable]
    if args.cmd == "bootstrap": return run_bootstrap(manifest, manifest_path)
    if args.cmd == "prepare" and bool(get_path(manifest,"bootstrap.run_on_prepare",False)):
        rc=run_bootstrap(manifest,manifest_path)
        if rc: return rc
    if args.cmd in {"build","prepare","resume"}:
        rc=run(base+[str(builder),"--manifest",str(manifest_path)])
        if rc:
            if out_root: diagnose(out_root)
            return rc
        if args.cmd == "build": return 0
    if args.cmd in {"prepare","resume","init-orchestration"}:
        return run(base+[str(orch),"--manifest",str(manifest_path),"init"])
    if args.cmd == "status":
        cmd=base+[str(orch),"--manifest",str(manifest_path),"status"]
        if args.verbose: cmd.append("--verbose")
        return run(cmd)
    if args.cmd == "next": return run(base+[str(orch),"--manifest",str(manifest_path),"next"])
    if args.cmd == "audit": return run(base+[str(orch),"--manifest",str(manifest_path),"audit"])
    return 0

if __name__ == "__main__": raise SystemExit(main())
