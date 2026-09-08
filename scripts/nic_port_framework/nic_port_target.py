#!/usr/bin/env python3
"""
nic_port_target.py

Destination-OS scoping for the NIC Port Framework.

Two responsibilities, both executed before the per-method porting loop:

  triage   Classify every extracted method against the target profile's exclusion
           rules. Linux-only surfaces (kernel compatibility shims, devlink, the
           auxiliary bus, XDP/AF_XDP, VFIO mediated devices, ethtool) are removed
           from the porting loop instead of being carried through it. Nothing is
           deleted: every exclusion is recorded with the rule and rationale that
           produced it, and the disposition says whether the behaviour is dropped,
           replaced by a target work item, or deferred.

  plan     Instantiate the target-mandated work items. These have no source
           counterpart - they exist because the destination OS and its driver
           framework require them - so they must be created rather than translated.
           Each item is grounded against the retained baseline: the subsystems and
           feature families it draws from are resolved to real symbols.

Outputs (under the analysis root):
    orchestration/target/exclusions.json      consumed by the orchestrator
    orchestration/target/excluded_methods.jsonl
    orchestration/target/items/<ITEM_ID>.json target-stage work item contexts
    orchestration/target/plan.json
    reports/target_scope.md

Usage:
    python3 nic_port_target.py --manifest M scope   --root <analysis-root>
    python3 nic_port_target.py --manifest M triage  --root <analysis-root>
    python3 nic_port_target.py --manifest M plan    --root <analysis-root>
    python3 nic_port_target.py --manifest M status  --root <analysis-root>
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable

from nic_port_manifest import (
    ManifestError, get_path, load_manifest, load_target_profile, validate_manifest_basics,
)

TOOL_VERSION = "1.0.0"
SCHEMA_VERSION = "1.0"
DISPOSITIONS = ("drop", "replace", "defer")
MAX_SYMBOLS_PER_INPUT = 40


def now_iso() -> str:
    from datetime import datetime, timezone
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def load_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return default


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def method_records(root: Path) -> list[dict[str, Any]]:
    records = []
    for ctx in sorted((root / "contexts" / "methods").glob("*.json")):
        obj = load_json(ctx, None)
        if not isinstance(obj, dict):
            continue
        records.append({
            "item_id": ctx.stem,
            "key": str(obj.get("key") or ctx.stem),
            "name": str(obj.get("name") or ""),
            "file": str(obj.get("file") or ""),
            "subsystems": [str(x) for x in (obj.get("subsystems") or [])],
            "feature_families": sorted({
                str(x.get("feature_family")) for x in (obj.get("linux_specific_features") or [])
                if isinstance(x, dict) and x.get("feature_family")
            }),
            "portability_score": obj.get("portability_score"),
            "porting_risk": obj.get("porting_risk") or {},
        })
    return records


def path_matches(record: dict[str, Any], globs: Iterable[str]) -> bool:
    path = record["file"]
    base = path.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatch(path, g) or fnmatch.fnmatch(base, g) for g in globs)


def rule_matches(record: dict[str, Any], match: dict[str, Any]) -> bool:
    if not match:
        return False
    globs = match.get("path_globs")
    if globs and path_matches(record, globs):
        return True
    for pattern in match.get("name_regex") or []:
        if re.search(str(pattern), record["name"]):
            return True
    families = match.get("feature_family_only")
    if families and record["feature_families"] and set(record["feature_families"]).issubset(set(families)):
        return True
    subsystems = match.get("subsystem_only")
    if subsystems and record["subsystems"] and set(record["subsystems"]).issubset(set(subsystems)):
        return True
    return False


def validate_profile(profile: dict[str, Any]) -> None:
    ids = {str(x.get("item_id")) for x in profile.get("work_items") or []}
    if len(ids) != len(profile.get("work_items") or []):
        raise ManifestError("target_profile contains duplicate work item ids")
    for rule in (profile.get("exclusion") or {}).get("rules") or []:
        disposition = str(rule.get("disposition") or "")
        if disposition not in DISPOSITIONS:
            raise ManifestError(
                f"exclusion rule {rule.get('rule_id')!r} has disposition {disposition!r}; expected one of {DISPOSITIONS}")
        unknown = [x for x in (rule.get("replaced_by") or []) if x not in ids]
        if unknown:
            raise ManifestError(
                f"exclusion rule {rule.get('rule_id')!r} references unknown work items: {', '.join(unknown)}")
    for wf in profile.get("workflows") or []:
        unknown = [x for x in (wf.get("items") or []) if x not in ids]
        if unknown:
            raise ManifestError(f"workflow {wf.get('workflow_id')!r} references unknown work items: {', '.join(unknown)}")


def triage(root: Path, profile: dict[str, Any]) -> dict[str, Any]:
    exclusion = profile.get("exclusion") or {}
    overrides = exclusion.get("keep_overrides") or []
    rules = exclusion.get("rules") or []
    records = method_records(root)

    excluded: dict[str, dict[str, Any]] = {}
    retained: list[dict[str, Any]] = []
    kept_by_override: list[dict[str, Any]] = []

    for record in records:
        override = next((o for o in overrides if rule_matches(record, o.get("match") or {})), None)
        if override is not None:
            kept_by_override.append({**record, "rule_id": str(override.get("rule_id"))})
            retained.append(record)
            continue
        rule = next((r for r in rules if rule_matches(record, r.get("match") or {})), None)
        if rule is None:
            retained.append(record)
            continue
        excluded[record["item_id"]] = {
            "item_id": record["item_id"],
            "function_key": record["key"],
            "name": record["name"],
            "file": record["file"],
            "rule_id": str(rule.get("rule_id")),
            "disposition": str(rule.get("disposition")),
            "rationale": str(rule.get("rationale") or ""),
            "replaced_by": [str(x) for x in (rule.get("replaced_by") or [])],
            "subsystems": record["subsystems"],
            "feature_families": record["feature_families"],
        }

    by_rule: dict[str, int] = {}
    by_disposition: dict[str, int] = {}
    by_file: dict[str, int] = {}
    for row in excluded.values():
        by_rule[row["rule_id"]] = by_rule.get(row["rule_id"], 0) + 1
        by_disposition[row["disposition"]] = by_disposition.get(row["disposition"], 0) + 1
        by_file[row["file"]] = by_file.get(row["file"], 0) + 1

    payload = {
        "schema_version": SCHEMA_VERSION,
        "generated_by": f"nic_port_target/{TOOL_VERSION}",
        "generated_utc": now_iso(),
        "target_profile_id": str(profile.get("target_profile_id")),
        "target_os": str(profile.get("target_os")),
        "driver_framework": str(profile.get("driver_framework")),
        "methods_total": len(records),
        "methods_retained": len(retained),
        "methods_excluded": len(excluded),
        "kept_by_override": len(kept_by_override),
        "by_rule": dict(sorted(by_rule.items())),
        "by_disposition": dict(sorted(by_disposition.items())),
        "by_file": dict(sorted(by_file.items(), key=lambda kv: -kv[1])),
        "excluded": excluded,
    }
    write_json(root / "orchestration" / "target" / "exclusions.json", payload)

    jl = root / "orchestration" / "target" / "excluded_methods.jsonl"
    jl.parent.mkdir(parents=True, exist_ok=True)
    jl.write_text("".join(json.dumps(v, sort_keys=True) + "\n" for v in excluded.values()), encoding="utf-8")
    return {**payload, "_retained": retained}


def ground_source_inputs(retained: list[dict[str, Any]], source_inputs: dict[str, Any]) -> dict[str, Any]:
    """Resolve an item's declared source inputs to real symbols in the retained baseline."""
    subsystems = [str(x) for x in (source_inputs.get("subsystems") or [])]
    families = [str(x) for x in (source_inputs.get("feature_families") or [])]
    matched: set[str] = set()
    per_subsystem: dict[str, Any] = {}
    for name in subsystems:
        hits = [r for r in retained if name in r["subsystems"]]
        matched.update(r["key"] for r in hits)
        per_subsystem[name] = {
            "method_count": len(hits),
            "symbols": sorted({r["name"] for r in hits})[:MAX_SYMBOLS_PER_INPUT],
            "files": sorted({r["file"] for r in hits}),
        }
    per_family: dict[str, Any] = {}
    for name in families:
        hits = [r for r in retained if name in r["feature_families"]]
        matched.update(r["key"] for r in hits)
        per_family[name] = {
            "method_count": len(hits),
            "symbols": sorted({r["name"] for r in hits})[:MAX_SYMBOLS_PER_INPUT],
        }
    return {
        "subsystems": per_subsystem,
        "feature_families": per_family,
        "total_methods": len(matched),
    }


def plan(root: Path, profile: dict[str, Any], triage_payload: dict[str, Any]) -> dict[str, Any]:
    retained = triage_payload.get("_retained")
    if retained is None:
        exclusions = load_json(root / "orchestration" / "target" / "exclusions.json", {}) or {}
        excluded_ids = set((exclusions.get("excluded") or {}).keys())
        retained = [r for r in method_records(root) if r["item_id"] not in excluded_ids]

    workflows = {str(w.get("workflow_id")): w for w in (profile.get("workflows") or [])}
    replaced_by_index: dict[str, list[dict[str, str]]] = {}
    for row in (triage_payload.get("excluded") or {}).values():
        for target_id in row.get("replaced_by") or []:
            replaced_by_index.setdefault(target_id, []).append({
                "function_key": row["function_key"], "file": row["file"], "rule_id": row["rule_id"],
            })

    items_dir = root / "orchestration" / "target" / "items"
    items_dir.mkdir(parents=True, exist_ok=True)
    for stale in items_dir.glob("*.json"):
        stale.unlink()

    written: list[dict[str, Any]] = []
    for spec in profile.get("work_items") or []:
        item_id = str(spec.get("item_id"))
        workflow = workflows.get(str(spec.get("workflow_id")), {})
        superseded = replaced_by_index.get(item_id, [])
        context = {
            "schema_version": SCHEMA_VERSION,
            "generated_by": f"nic_port_target/{TOOL_VERSION}",
            "generated_utc": now_iso(),
            "target_item_id": item_id,
            "title": str(spec.get("title") or item_id),
            "mandatory": bool(spec.get("mandatory", True)),
            "target_os": str(profile.get("target_os")),
            "driver_framework": str(profile.get("driver_framework")),
            "target_profile_id": str(profile.get("target_profile_id")),
            "workflow": {
                "workflow_id": str(spec.get("workflow_id") or ""),
                "title": str(workflow.get("title") or ""),
                "order": workflow.get("order"),
                "items": workflow.get("items") or [],
            },
            "replaces_source_mechanism": str(spec.get("replaces_source_mechanism") or ""),
            "target_api": [str(x) for x in (spec.get("target_api") or [])],
            "obligations": [str(x) for x in (spec.get("obligations") or [])],
            "acceptance_criteria": [str(x) for x in (spec.get("acceptance_criteria") or [])],
            "references": [str(x) for x in (spec.get("references") or [])],
            "source_inputs": spec.get("source_inputs") or {},
            "source_evidence": ground_source_inputs(retained, spec.get("source_inputs") or {}),
            "superseded_source_methods": sorted(superseded, key=lambda r: r["function_key"])[:200],
            "superseded_source_method_count": len(superseded),
        }
        write_json(items_dir / f"{item_id}.json", context)
        written.append({
            "target_item_id": item_id,
            "title": context["title"],
            "workflow_id": context["workflow"]["workflow_id"],
            "order": context["workflow"]["order"],
            "mandatory": context["mandatory"],
            "obligations": len(context["obligations"]),
            "acceptance_criteria": len(context["acceptance_criteria"]),
            "grounded_methods": context["source_evidence"]["total_methods"],
            "superseded_source_methods": len(superseded),
        })

    payload = {
        "schema_version": SCHEMA_VERSION,
        "generated_by": f"nic_port_target/{TOOL_VERSION}",
        "generated_utc": now_iso(),
        "target_profile_id": str(profile.get("target_profile_id")),
        "target_os": str(profile.get("target_os")),
        "driver_framework": str(profile.get("driver_framework")),
        "work_item_count": len(written),
        "mandatory_count": sum(1 for x in written if x["mandatory"]),
        "workflows": [
            {"workflow_id": str(w.get("workflow_id")), "title": str(w.get("title") or ""),
             "order": w.get("order"), "items": [str(x) for x in (w.get("items") or [])]}
            for w in sorted(profile.get("workflows") or [], key=lambda w: w.get("order") or 0)
        ],
        "items": sorted(written, key=lambda r: (r["order"] or 0, r["target_item_id"])),
    }
    write_json(root / "orchestration" / "target" / "plan.json", payload)
    return payload


def write_report(root: Path, triage_payload: dict[str, Any], plan_payload: dict[str, Any],
                 profile: dict[str, Any]) -> Path:
    rules = {str(r.get("rule_id")): r for r in (profile.get("exclusion") or {}).get("rules") or []}
    lines = [
        f"# Target scope — {profile.get('name')}",
        "",
        f"Target OS: **{triage_payload['target_os']}**  ",
        f"Driver framework: **{triage_payload['driver_framework']}**  ",
        f"Profile: `{triage_payload['target_profile_id']}`",
        "",
        "## Method triage",
        "",
        "| Metric | Count |",
        "|---|---:|",
        f"| methods extracted | {triage_payload['methods_total']} |",
        f"| retained for the porting loop | {triage_payload['methods_retained']} |",
        f"| excluded as source-OS only | {triage_payload['methods_excluded']} |",
        f"| protected by a keep override | {triage_payload['kept_by_override']} |",
        "",
        "### Exclusions by rule",
        "",
        "| Rule | Disposition | Methods | Rationale |",
        "|---|---|---:|---|",
    ]
    for rule_id, count in sorted(triage_payload["by_rule"].items(), key=lambda kv: -kv[1]):
        rule = rules.get(rule_id, {})
        lines.append(f"| `{rule_id}` | {rule.get('disposition', '-')} | {count} | {rule.get('rationale', '')} |")

    if triage_payload["by_file"]:
        lines += [
            "",
            "### Exclusions by file",
            "",
            "| File | Methods excluded |",
            "|---|---:|",
        ]
        for path, count in triage_payload["by_file"].items():
            lines.append(f"| `{path}` | {count} |")

    lines += [
        "",
        "## Target-mandated work items",
        "",
        f"These have no source counterpart. Total: **{plan_payload['work_item_count']}**, "
        f"mandatory: **{plan_payload['mandatory_count']}**.",
        "",
    ]
    by_workflow: dict[str, list[dict[str, Any]]] = {}
    for item in plan_payload["items"]:
        by_workflow.setdefault(item["workflow_id"], []).append(item)
    for wf in plan_payload["workflows"]:
        items = by_workflow.get(wf["workflow_id"], [])
        if not items:
            continue
        lines += [
            f"### {wf['order']}. {wf['title']}",
            "",
            "| Item | Title | Obligations | Acceptance | Grounding methods | Supersedes |",
            "|---|---|---:|---:|---:|---:|",
        ]
        for item in items:
            lines.append(
                f"| `{item['target_item_id']}` | {item['title']} | {item['obligations']} | "
                f"{item['acceptance_criteria']} | {item['grounded_methods']} | "
                f"{item['superseded_source_methods']} |"
            )
        lines.append("")

    path = root / "reports" / "target_scope.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def resolve_root(manifest: dict[str, Any], override: str | None) -> Path:
    return Path(override or str(get_path(manifest, "output.root"))).expanduser().resolve()


def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Remove source-OS-only surfaces and plan target-mandated work items.")
    ap.add_argument("--manifest", default=str(here / "config" / "default.manifest.json"))
    ap.add_argument("--root", help="Analysis root (defaults to output.root)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scope", help="Run triage and plan")
    sub.add_parser("triage", help="Classify methods against the target exclusion rules")
    sub.add_parser("plan", help="Instantiate the target-mandated work items")
    sub.add_parser("status", help="Print the current target scope state")
    args = ap.parse_args(argv)

    try:
        manifest = load_manifest(Path(args.manifest).expanduser().resolve(), strict_env=True)
        errors = validate_manifest_basics(manifest)
        if errors:
            raise ManifestError("; ".join(errors))
        profile = load_target_profile(manifest)
        validate_profile(profile)
        root = resolve_root(manifest, args.root)
    except (ManifestError, OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: configuration: {exc}", file=sys.stderr)
        return 2

    if not (root / "contexts" / "methods").is_dir():
        print(f"ERROR: no method contexts under {root}; run the extraction first", file=sys.stderr)
        return 2

    if args.cmd == "status":
        excl = load_json(root / "orchestration" / "target" / "exclusions.json", {}) or {}
        pl = load_json(root / "orchestration" / "target" / "plan.json", {}) or {}
        print(f"profile        : {profile.get('target_profile_id')} ({profile.get('target_os')}/{profile.get('driver_framework')})")
        print(f"triage         : {'present' if excl else 'MISSING'}"
              + (f" (retained {excl.get('methods_retained')}/{excl.get('methods_total')}, excluded {excl.get('methods_excluded')})" if excl else ""))
        print(f"work item plan : {'present' if pl else 'MISSING'}"
              + (f" ({pl.get('work_item_count')} items, {pl.get('mandatory_count')} mandatory)" if pl else ""))
        return 0 if excl and pl else 1

    triage_payload: dict[str, Any]
    if args.cmd in ("triage", "scope"):
        triage_payload = triage(root, profile)
    else:
        triage_payload = load_json(root / "orchestration" / "target" / "exclusions.json", {}) or {}
        if not triage_payload:
            print("ERROR: run 'triage' before 'plan'", file=sys.stderr)
            return 2

    if args.cmd == "triage":
        print(f"methods extracted : {triage_payload['methods_total']}")
        print(f"retained          : {triage_payload['methods_retained']}")
        print(f"excluded          : {triage_payload['methods_excluded']}")
        for rule_id, count in sorted(triage_payload["by_rule"].items(), key=lambda kv: -kv[1]):
            print(f"  {rule_id:<32} {count}")
        return 0

    plan_payload = plan(root, profile, triage_payload)
    report = write_report(root, triage_payload, plan_payload, profile)
    print(f"target profile    : {profile.get('target_profile_id')} ({profile.get('target_os')}/{profile.get('driver_framework')})")
    print(f"methods extracted : {triage_payload['methods_total']}")
    print(f"retained          : {triage_payload['methods_retained']}")
    print(f"excluded          : {triage_payload['methods_excluded']} "
          f"({', '.join(f'{k}={v}' for k, v in sorted(triage_payload['by_disposition'].items()))})")
    print(f"target work items : {plan_payload['work_item_count']} "
          f"({plan_payload['mandatory_count']} mandatory) across {len(plan_payload['workflows'])} workflows")
    print(f"report            : {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
