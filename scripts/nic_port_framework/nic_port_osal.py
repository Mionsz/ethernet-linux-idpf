#!/usr/bin/env python3
"""
nic_port_osal.py

Operating System Abstraction Layer (OSAL) policy for the NIC Port Framework.

Two modes, selected by `configuration.osal_rules` -> `mode`:

  optional  (default)
      The OSAL is described, not imposed. The tool indexes the OSAL headers and
      publishes the API surface plus a usage plan so prompts and reviewers can
      see which call sites have an abstraction available. Sources are untouched.

  mandatory
      The OSAL is imposed before any source is analysed. A staged copy of the
      source tree is rewritten onto the OSAL API, the compile database is
      retargeted at the staged tree, and everything downstream analyses the
      OSAL-normalised code. The original repository is never modified.

Substitution safety: an entry is rewritten only when the OSAL macro accepts the
same number of arguments as the Linux original. Arity is read from the OSAL
header itself, not from the mapping file, so the check tracks the real API.
Arity-mismatched entries are reported for manual work instead of being applied
(override with apply_arity_incompatible).

Commands:
    index    Index the OSAL API surface           -> kb/osal_index.json
    plan     Report OSAL opportunities per file   -> manifest/osal_plan.json
    stage    Rewrite a staged tree (mandatory)    -> manifest/osal_staging.json
    check    Verify a staged tree                 -> stdout / exit code
    status   Print the effective OSAL policy
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import sys
from pathlib import Path
from typing import Any

from nic_port_manifest import (
    ManifestError, get_path, load_manifest, load_osal_rules, load_semantic_rules,
    validate_manifest_basics,
)

TOOL_VERSION = "1.0.0"
MODES = ("optional", "mandatory")

RE_OS_MACRO = re.compile(r"^\s*#\s*define\s+(os_[A-Za-z0-9_]+)\s*(\(([^)]*)\))?")
RE_OSAL_PROTO = re.compile(r"\b(nic_os_[a-z0-9_]+)\s*\(")


def macro_arity(params: str | None) -> int:
    """Argument count of a macro parameter list; -1 for variadic."""
    if params is None:
        return -2  # object-like macro, not callable
    params = params.strip()
    if not params:
        return 0
    if "..." in params:
        return -1
    return len([p for p in params.split(",") if p.strip()])


def index_osal(osal_root: Path, header_exts: set[str]) -> dict[str, Any]:
    if not osal_root.is_dir():
        raise ManifestError(f"OSAL root does not exist: {osal_root}")
    macros: dict[str, dict[str, Any]] = {}
    functions: set[str] = set()
    headers: list[dict[str, Any]] = []
    for path in sorted(osal_root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in header_exts:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = str(path.relative_to(osal_root))
        local_macros: list[str] = []
        for line in text.splitlines():
            m = RE_OS_MACRO.match(line)
            if m:
                name = m.group(1)
                macros[name] = {"arity": macro_arity(m.group(3)), "header": rel}
                local_macros.append(name)
        local_fns = sorted(set(RE_OSAL_PROTO.findall(text)))
        functions.update(local_fns)
        headers.append({
            "path": rel,
            "macros": sorted(local_macros),
            "functions": local_fns,
        })
    return {
        "generated_by": f"nic_port_osal/{TOOL_VERSION}",
        "osal_root": str(osal_root),
        "headers": headers,
        "header_count": len(headers),
        "macros": macros,
        "macro_count": len(macros),
        "functions": sorted(functions),
        "function_count": len(functions),
    }


def resolve_mapping(rules: dict[str, Any], index: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Bind each configured mapping to the real OSAL macro and judge applicability."""
    macros = index.get("macros") or {}
    resolved: dict[str, dict[str, Any]] = {}
    for linux_symbol, spec in (rules.get("symbol_map") or {}).items():
        if not isinstance(spec, dict) or not spec.get("replacement"):
            raise ManifestError(f"osal_rules.symbol_map['{linux_symbol}'] must define a replacement")
        replacement = str(spec["replacement"])
        linux_arity = int(spec.get("linux_arity", -1))
        entry = macros.get(replacement)
        if entry is None:
            status, osal_arity = "missing_in_osal", None
        else:
            osal_arity = int(entry["arity"])
            if osal_arity == -1 or linux_arity == -1 or osal_arity == linux_arity:
                status = "applicable"
            else:
                status = "arity_mismatch"
        resolved[linux_symbol] = {
            "replacement": replacement,
            "domain": spec.get("domain", "unclassified"),
            "linux_arity": linux_arity,
            "osal_arity": osal_arity,
            "status": status,
            "header": (entry or {}).get("header"),
        }
    return resolved


def rewritable_files(source: Path, exts: set[str], ignore_dirs: set[str], skip_parts: set[str]) -> list[Path]:
    files: list[Path] = []
    for p in sorted(source.rglob("*")):
        if not p.is_file():
            continue
        parts = p.relative_to(source).parts
        if any(part in ignore_dirs or part in skip_parts for part in parts):
            continue
        if p.suffix.lower() in exts:
            files.append(p)
    return files


def compile_skip_patterns(rules: dict[str, Any]) -> list[re.Pattern[str]]:
    return [re.compile(p) for p in (rules.get("skip_line_patterns") or [])]


def substitution_pattern(symbols: list[str]) -> re.Pattern[str]:
    # Only call sites: identifier not qualified by `.` or `->`, followed by `(`.
    alternation = "|".join(sorted((re.escape(s) for s in symbols), key=len, reverse=True))
    return re.compile(rf"(?<![\w.])(?<!->)\b({alternation})\b(?=\s*\()")


def rewrite_text(text: str, pattern: re.Pattern[str], mapping: dict[str, dict[str, Any]],
                 skip_patterns: list[re.Pattern[str]]) -> tuple[str, dict[str, int]]:
    counts: dict[str, int] = {}
    out: list[str] = []
    for line in text.splitlines(keepends=True):
        if any(p.search(line) for p in skip_patterns):
            out.append(line)
            continue

        def repl(m: re.Match[str]) -> str:
            name = m.group(1)
            counts[name] = counts.get(name, 0) + 1
            return mapping[name]["replacement"]

        out.append(pattern.sub(repl, line))
    return "".join(out), counts


def count_occurrences(text: str, pattern: re.Pattern[str]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for m in pattern.finditer(text):
        counts[m.group(1)] = counts.get(m.group(1), 0) + 1
    return counts


def inject_include(text: str, header: str) -> tuple[str, bool]:
    needle = f'#include "{header}"'
    if needle in text or f"#include <{header}>" in text:
        return text, False
    lines = text.splitlines(keepends=True)
    last_include = -1
    for i, line in enumerate(lines):
        if re.match(r"^\s*#\s*include\b", line):
            last_include = i
    if last_include < 0:
        return text, False
    lines.insert(last_include + 1, needle + "\n")
    return "".join(lines), True


def build_plan(source: Path, files: list[Path], mapping: dict[str, dict[str, Any]],
               skip_patterns: list[re.Pattern[str]]) -> dict[str, Any]:
    symbols = list(mapping)
    pattern = substitution_pattern(symbols) if symbols else None
    per_file: list[dict[str, Any]] = []
    totals: dict[str, int] = {}
    for path in files:
        if pattern is None:
            break
        text = path.read_text(encoding="utf-8", errors="replace")
        kept = "".join(l for l in text.splitlines(keepends=True) if not any(p.search(l) for p in skip_patterns))
        counts = count_occurrences(kept, pattern)
        if not counts:
            continue
        for k, v in counts.items():
            totals[k] = totals.get(k, 0) + v
        per_file.append({
            "path": str(path.relative_to(source)),
            "total": sum(counts.values()),
            "applicable": sum(v for k, v in counts.items() if mapping[k]["status"] == "applicable"),
            "manual_review": sum(v for k, v in counts.items() if mapping[k]["status"] != "applicable"),
            "symbols": dict(sorted(counts.items())),
        })
    by_domain: dict[str, int] = {}
    for sym, n in totals.items():
        by_domain[mapping[sym]["domain"]] = by_domain.get(mapping[sym]["domain"], 0) + n
    manual = {s: n for s, n in totals.items() if mapping[s]["status"] != "applicable"}
    return {
        "generated_by": f"nic_port_osal/{TOOL_VERSION}",
        "files_with_opportunities": len(per_file),
        "call_sites_total": sum(totals.values()),
        "call_sites_applicable": sum(totals.values()) - sum(manual.values()),
        "call_sites_manual_review": sum(manual.values()),
        "by_domain": dict(sorted(by_domain.items())),
        "by_symbol": dict(sorted(totals.items(), key=lambda kv: -kv[1])),
        "manual_review_symbols": {
            s: {"count": n, "reason": mapping[s]["status"], "replacement": mapping[s]["replacement"],
                "linux_arity": mapping[s]["linux_arity"], "osal_arity": mapping[s]["osal_arity"]}
            for s, n in sorted(manual.items(), key=lambda kv: -kv[1])
        },
        "files": sorted(per_file, key=lambda r: -r["total"]),
    }


def stage_sources(source: Path, staged: Path, files: list[Path], mapping: dict[str, dict[str, Any]],
                  rules: dict[str, Any], skip_patterns: list[re.Pattern[str]]) -> dict[str, Any]:
    apply_all = bool(rules.get("apply_arity_incompatible"))
    active = {k: v for k, v in mapping.items() if v["status"] == "applicable" or (apply_all and v["status"] == "arity_mismatch")}
    pattern = substitution_pattern(list(active)) if active else None
    inject = str(rules.get("inject_include") or "")
    rewrite_exts = {str(x).lower() for x in (rules.get("rewrite_extensions") or [".c"])}

    if staged.exists():
        shutil.rmtree(staged)
    staged.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, staged, dirs_exist_ok=True, symlinks=True)

    changed: list[dict[str, Any]] = []
    totals: dict[str, int] = {}
    injected = 0
    for path in files:
        rel = path.relative_to(source)
        target = staged / rel
        text = path.read_text(encoding="utf-8", errors="replace")
        new_text, counts = (text, {}) if pattern is None else rewrite_text(text, pattern, active, skip_patterns)
        added = False
        if counts and inject and path.suffix.lower() in rewrite_exts:
            new_text, added = inject_include(new_text, inject)
            injected += 1 if added else 0
        if new_text != text:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(new_text, encoding="utf-8")
            for k, v in counts.items():
                totals[k] = totals.get(k, 0) + v
            changed.append({
                "path": str(rel),
                "replacements": sum(counts.values()),
                "include_injected": added,
                "symbols": dict(sorted(counts.items())),
            })
    return {
        "generated_by": f"nic_port_osal/{TOOL_VERSION}",
        "staged_source_root": str(staged),
        "original_source_root": str(source),
        "files_rewritten": len(changed),
        "replacements_total": sum(totals.values()),
        "includes_injected": injected,
        "applied_symbols": dict(sorted(totals.items(), key=lambda kv: -kv[1])),
        "skipped_symbols": {
            k: v["status"] for k, v in sorted(mapping.items()) if k not in active
        },
        "files": sorted(changed, key=lambda r: -r["replacements"]),
    }


def retarget_compile_db(db_path: Path, source: Path, staged: Path, osal_root: Path,
                        out_path: Path) -> dict[str, Any]:
    if not db_path.is_file():
        raise ManifestError(f"compile database not found: {db_path}")
    entries = json.loads(db_path.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise ManifestError(f"compile database is not a JSON array: {db_path}")
    src_str, staged_str = str(source), str(staged)
    include_flag = f"-I{osal_root}"
    rewritten = 0
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        touched = False
        for key in ("file", "directory", "output"):
            value = entry.get(key)
            if isinstance(value, str) and value.startswith(src_str):
                entry[key] = staged_str + value[len(src_str):]
                touched = True
        if isinstance(entry.get("arguments"), list):
            entry["arguments"] = [
                (staged_str + a[len(src_str):]) if isinstance(a, str) and a.startswith(src_str) else a
                for a in entry["arguments"]
            ]
            if include_flag not in entry["arguments"]:
                entry["arguments"].insert(1, include_flag)
            touched = True
        elif isinstance(entry.get("command"), str):
            entry["command"] = entry["command"].replace(src_str, staged_str)
            if include_flag not in entry["command"]:
                entry["command"] = entry["command"].replace(" -c ", f" {include_flag} -c ", 1)
            touched = True
        rewritten += 1 if touched else 0
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(entries, indent=1) + "\n", encoding="utf-8")
    return {"compile_database": str(out_path), "entries": len(entries), "entries_retargeted": rewritten}


def effective_mode(rules: dict[str, Any], override: str | None) -> str:
    mode = (override or rules.get("mode") or "optional").strip().lower()
    if mode not in MODES:
        raise ManifestError(f"OSAL mode must be one of {MODES}, got '{mode}'")
    return mode


def resolve_osal_root(manifest: dict[str, Any], rules: dict[str, Any], override: str | None) -> Path:
    value = override or get_path(manifest, "inputs.osal_root") or rules.get("osal_root")
    if not value:
        raise ManifestError("OSAL root is not configured (inputs.osal_root or osal_rules.osal_root)")
    return Path(str(value)).expanduser().resolve()


def write_report(out_root: Path, mode: str, index: dict[str, Any], mapping: dict[str, dict[str, Any]],
                 plan: dict[str, Any] | None, staging: dict[str, Any] | None) -> Path:
    lines = [
        "# OSAL Policy",
        "",
        f"Mode: **{mode}**  ",
        f"OSAL root: `{index['osal_root']}`  ",
        f"Headers: **{index['header_count']}**, macros: **{index['macro_count']}**, "
        f"functions: **{index['function_count']}**",
        "",
        "| Mapping status | Symbols |",
        "|---|---:|",
    ]
    status_counts: dict[str, int] = {}
    for spec in mapping.values():
        status_counts[spec["status"]] = status_counts.get(spec["status"], 0) + 1
    for status, count in sorted(status_counts.items()):
        lines.append(f"| {status} | {count} |")

    if plan:
        lines += [
            "",
            "## Call sites with an OSAL equivalent",
            "",
            "| Metric | Count |",
            "|---|---:|",
            f"| files with opportunities | {plan['files_with_opportunities']} |",
            f"| call sites total | {plan['call_sites_total']} |",
            f"| directly substitutable | {plan['call_sites_applicable']} |",
            f"| needs manual review | {plan['call_sites_manual_review']} |",
            "",
            "| Domain | Call sites |",
            "|---|---:|",
        ]
        for domain, count in plan["by_domain"].items():
            lines.append(f"| {domain} | {count} |")
        if plan["manual_review_symbols"]:
            lines += [
                "",
                "### Manual review required",
                "",
                "The OSAL form takes a different number of arguments than the Linux original,",
                "so a mechanical rename would not compile. Port these by hand.",
                "",
                "| Linux symbol | OSAL form | linux arity | osal arity | call sites | reason |",
                "|---|---|---:|---:|---:|---|",
            ]
            for sym, info in plan["manual_review_symbols"].items():
                lines.append(
                    f"| `{sym}` | `{info['replacement']}` | {info['linux_arity']} | "
                    f"{info['osal_arity'] if info['osal_arity'] is not None else '-'} | "
                    f"{info['count']} | {info['reason']} |"
                )

    if staging:
        lines += [
            "",
            "## Staging (mandatory mode)",
            "",
            "| Metric | Value |",
            "|---|---|",
            f"| staged source root | `{staging['staged_source_root']}` |",
            f"| files rewritten | {staging['files_rewritten']} |",
            f"| replacements applied | {staging['replacements_total']} |",
            f"| includes injected | {staging['includes_injected']} |",
            "",
            "The original repository was not modified.",
        ]
    else:
        lines += [
            "",
            "Optional mode: no source was rewritten. The OSAL surface above is supplied as",
            "context only; call sites remain on their Linux forms.",
        ]

    path = out_root / "reports" / "osal_summary.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def json_write(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="OSAL indexing, planning and mandatory-mode source staging.")
    ap.add_argument("--manifest", default=str(here / "config" / "default.manifest.json"))
    ap.add_argument("--mode", choices=MODES, help="Override configuration.osal_rules.mode")
    ap.add_argument("--osal-root", help="Override the OSAL directory")
    ap.add_argument("--source", help="Override inputs.source_root")
    ap.add_argument("--root", help="Override output.root")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="Print the effective OSAL policy")
    sub.add_parser("index", help="Index the OSAL API surface")
    sub.add_parser("plan", help="Report OSAL substitution opportunities")
    p_stage = sub.add_parser("stage", help="Rewrite a staged source tree (mandatory mode)")
    p_stage.add_argument("--compile-db", help="Compile database to retarget at the staged tree")
    p_stage.add_argument("--force", action="store_true", help="Stage even when mode is optional")
    sub.add_parser("check", help="Verify that a staged tree has no substitutable Linux call sites left")
    args = ap.parse_args(argv)

    try:
        manifest = load_manifest(Path(args.manifest).expanduser().resolve(), strict_env=True)
        errors = validate_manifest_basics(manifest)
        if errors:
            raise ManifestError("; ".join(errors))
        rules = load_osal_rules(manifest)
        semantic = load_semantic_rules(manifest)
        mode = effective_mode(rules, args.mode)
        osal_root = resolve_osal_root(manifest, rules, args.osal_root)
        source = Path(args.source or str(get_path(manifest, "inputs.source_root"))).expanduser().resolve()
        out_root = Path(args.root or str(get_path(manifest, "output.root"))).expanduser().resolve()
    except (ManifestError, OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: configuration: {exc}", file=sys.stderr)
        return 2

    header_exts = {str(x).lower() for x in (rules.get("header_extensions") or [".h"])}
    rewrite_exts = {str(x).lower() for x in (rules.get("rewrite_extensions") or [".c"])}
    ignore_dirs = {str(x) for x in (semantic.get("ignore_dirs") or [])}
    skip_parts = {str(x) for x in (rules.get("skip_path_parts") or [])}
    # The staged tree is a source tree, not an artifact: keep it beside the
    # analysis root so nothing that walks the artifact tree has to skip it.
    staged_ref = rules.get("staged_source_root")
    if staged_ref:
        staged = Path(str(staged_ref)).expanduser().resolve()
    else:
        staged = out_root.parent / str(rules.get("staged_source_dirname") or "osal_staged_source")

    if args.cmd == "status":
        print(f"mode          : {mode}")
        print(f"osal root     : {osal_root}")
        print(f"source root   : {source}")
        print(f"staged root   : {staged}")
        print(f"mapped symbols: {len(rules.get('symbol_map') or {})}")
        print(f"inject include: {rules.get('inject_include') or '-'}")
        return 0

    try:
        index = index_osal(osal_root, header_exts)
        mapping = resolve_mapping(rules, index)
    except (ManifestError, OSError) as exc:
        print(f"ERROR: osal: {exc}", file=sys.stderr)
        return 2

    out_root.mkdir(parents=True, exist_ok=True)
    json_write(out_root / "kb" / "osal_index.json", {**index, "mode": mode, "mapping": mapping})

    if args.cmd == "index":
        missing = [k for k, v in mapping.items() if v["status"] == "missing_in_osal"]
        mismatch = [k for k, v in mapping.items() if v["status"] == "arity_mismatch"]
        write_report(out_root, mode, index, mapping, None, None)
        print(f"osal headers   : {index['header_count']}")
        print(f"osal macros    : {index['macro_count']}")
        print(f"osal functions : {index['function_count']}")
        print(f"mapped symbols : {len(mapping)} (applicable "
              f"{len(mapping) - len(missing) - len(mismatch)}, arity mismatch {len(mismatch)}, missing {len(missing)})")
        if missing:
            print("missing in OSAL: " + ", ".join(sorted(missing)))
        return 0

    if not source.is_dir():
        print(f"ERROR: source root does not exist: {source}", file=sys.stderr)
        return 2
    skip_patterns = compile_skip_patterns(rules)
    scan_exts = rewrite_exts | {".h"}
    files = rewritable_files(source, scan_exts, ignore_dirs, skip_parts)

    if args.cmd == "plan":
        plan = build_plan(source, files, mapping, skip_patterns)
        json_write(out_root / "manifest" / "osal_plan.json", {**plan, "mode": mode})
        report = write_report(out_root, mode, index, mapping, plan, None)
        print(f"mode                 : {mode}")
        print(f"files with call sites: {plan['files_with_opportunities']}")
        print(f"call sites           : {plan['call_sites_total']}")
        print(f"  substitutable      : {plan['call_sites_applicable']}")
        print(f"  manual review      : {plan['call_sites_manual_review']}")
        print(f"report               : {report}")
        return 0

    if args.cmd == "check":
        if not staged.is_dir():
            print(f"ERROR: staged tree does not exist: {staged}", file=sys.stderr)
            return 2
        staged_files = rewritable_files(staged, scan_exts, ignore_dirs, skip_parts)
        remaining = build_plan(staged, staged_files, mapping, skip_patterns)
        left = remaining["call_sites_applicable"]
        print(f"staged tree          : {staged}")
        print(f"remaining call sites : {remaining['call_sites_total']}")
        print(f"  substitutable left : {left}")
        print(f"  manual review left : {remaining['call_sites_manual_review']}")
        return 1 if left else 0

    # stage
    if mode != "mandatory" and not args.force:
        print(f"OSAL mode is '{mode}': nothing staged. The OSAL surface is supplied as context only.")
        print("Use --mode mandatory (or --force) to rewrite a staged tree.")
        write_report(out_root, mode, index, mapping, build_plan(source, files, mapping, skip_patterns), None)
        return 0

    plan = build_plan(source, files, mapping, skip_patterns)
    staging = stage_sources(source, staged, files, mapping, rules, skip_patterns)
    db_info: dict[str, Any] = {}
    db_ref = args.compile_db or get_path(manifest, "inputs.compile_commands")
    if db_ref:
        try:
            db_info = retarget_compile_db(
                Path(str(db_ref)).expanduser().resolve(), source, staged, osal_root,
                out_root / "manifest" / "compile_commands.osal.json",
            )
        except (ManifestError, OSError, json.JSONDecodeError) as exc:
            print(f"ERROR: compile database retarget failed: {exc}", file=sys.stderr)
            return 2
    payload = {**staging, "mode": mode, "plan": plan, **db_info}
    json_write(out_root / "manifest" / "osal_staging.json", payload)
    report = write_report(out_root, mode, index, mapping, plan, staging)
    print(f"mode              : {mode}")
    print(f"staged source root: {staging['staged_source_root']}")
    print(f"files rewritten   : {staging['files_rewritten']}")
    print(f"replacements      : {staging['replacements_total']}")
    print(f"includes injected : {staging['includes_injected']}")
    print(f"manual review     : {plan['call_sites_manual_review']} call sites")
    if db_info:
        print(f"compile database  : {db_info['compile_database']} ({db_info['entries_retargeted']} entries retargeted)")
    print(f"report            : {report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
