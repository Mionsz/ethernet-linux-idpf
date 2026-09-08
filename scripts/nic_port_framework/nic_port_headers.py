#!/usr/bin/env python3
"""
nic_port_headers.py

Header inventory for the NIC Port Framework.

Compiler-derived extraction only records headers that a translation unit
actually pulled in, and only the entities the compiler kept. A port also needs
the declarative surface of every header in the repository: aggregate types,
include obligations, configuration macros and the guard structure - including
headers that no current build configuration compiles.

This scanner is deliberately lexical and supplemental. It never overrides
`kb/*.jsonl`; compiler facts stay authoritative. It answers one question the
compiler cannot: what does the repository declare that the current build did
not see?

Outputs (under the analysis root):
    kb/headers.jsonl                 one record per header
    manifest/header_coverage.json    covered / uncovered classification
    indexes/header_inventory.csv     review surface
    reports/header_summary.md        human summary

Usage:
    python3 nic_port_headers.py --manifest config/default.manifest.json scan
    python3 nic_port_headers.py --manifest ... scan --json
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Any, Iterable

from nic_port_manifest import (
    ManifestError, get_path, load_manifest, load_semantic_rules, validate_manifest_basics, sha256_file,
)

TOOL_VERSION = "1.0.0"

DEFAULT_HEADER_EXTS = (".h", ".hh", ".hpp", ".hxx", ".inc", ".inl")

RE_INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
RE_GUARD = re.compile(r"^\s*#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")
RE_DEFINE_OBJ = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?!\()")
RE_DEFINE_FN = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\(")
RE_COND = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif)\b(.*)$")
RE_COND_SYMBOL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
RE_AGGREGATE = re.compile(r"^\s*(?:typedef\s+)?(struct|union|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{")
RE_TYPEDEF_NAME = re.compile(r"^\s*typedef\s+.*?\b([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$")
RE_STATIC_INLINE = re.compile(r"^\s*(?:static\s+(?:__always_)?inline|__always_inline\s+static)\b.*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
RE_PROTOTYPE = re.compile(r"^\s*(?!#)(?:extern\s+)?[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*;\s*$")

COND_KEYWORDS = {"defined", "if", "ifdef", "ifndef", "elif"}


def strip_comments(text: str) -> str:
    """Remove block and line comments without changing line numbering."""
    out: list[str] = []
    i, n = 0, len(text)
    in_block = in_line = in_str = in_chr = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_block:
            if c == "*" and nxt == "/":
                in_block = False
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
        elif in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            else:
                out.append(" ")
        elif in_str or in_chr:
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if (in_str and c == '"') or (in_chr and c == "'"):
                in_str = in_chr = False
        else:
            if c == "/" and nxt == "*":
                in_block = True
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "/":
                in_line = True
                out.append("  ")
                i += 2
                continue
            if c == '"':
                in_str = True
            elif c == "'":
                in_chr = True
            out.append(c)
        i += 1
    return "".join(out)


def classify_include(name: str, kind: str, local_headers: set[str]) -> str:
    base = name.split("/")[-1]
    if kind == '"' or base in local_headers or name in local_headers:
        return "project"
    if name.startswith(("linux/", "asm/", "asm-generic/", "uapi/", "net/", "trace/")):
        return "linux_kernel"
    if "/" not in name and base.endswith(".h"):
        return "system_or_project"
    return "system"


def scan_header(path: Path, rel_path: str, local_headers: set[str]) -> dict[str, Any]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_comments(raw)
    lines = text.splitlines()

    includes: list[dict[str, str]] = []
    structs: list[str] = []
    unions: list[str] = []
    enums: list[str] = []
    typedefs: list[str] = []
    object_macros: list[str] = []
    function_macros: list[str] = []
    static_inline: list[str] = []
    prototypes: list[str] = []
    conditions: list[str] = []
    guard: str | None = None
    max_depth = depth = 0

    for idx, line in enumerate(lines):
        m = RE_INCLUDE.match(line)
        if m:
            includes.append({
                "name": m.group(2),
                "kind": "angle" if m.group(1) == "<" else "quote",
                "classification": classify_include(m.group(2), m.group(1), local_headers),
            })
            continue

        m = RE_COND.match(line)
        if m:
            if m.group(1) in {"if", "ifdef", "ifndef"}:
                depth += 1
                max_depth = max(max_depth, depth)
            for sym in RE_COND_SYMBOL.findall(m.group(2)):
                if sym not in COND_KEYWORDS:
                    conditions.append(sym)
            if guard is None and idx < 40:
                g = RE_GUARD.match(line)
                if g:
                    guard = g.group(1)
            continue
        if re.match(r"^\s*#\s*endif\b", line):
            depth = max(0, depth - 1)
            continue

        m = RE_DEFINE_FN.match(line)
        if m:
            function_macros.append(m.group(1))
            continue
        m = RE_DEFINE_OBJ.match(line)
        if m:
            object_macros.append(m.group(1))
            continue

        m = RE_AGGREGATE.match(line)
        if m:
            {"struct": structs, "union": unions, "enum": enums}[m.group(1)].append(m.group(2))
            continue

        m = RE_TYPEDEF_NAME.match(line)
        if m:
            typedefs.append(m.group(1))
            continue

        m = RE_STATIC_INLINE.search(line)
        if m:
            static_inline.append(m.group(1))
            continue

        m = RE_PROTOTYPE.match(line)
        if m and m.group(1) not in {"if", "for", "while", "switch", "return", "sizeof"}:
            prototypes.append(m.group(1))

    def uniq(seq: Iterable[str]) -> list[str]:
        return sorted(set(x for x in seq if x))

    stat = path.stat()
    return {
        "path": rel_path,
        "bytes": stat.st_size,
        "lines": len(lines),
        "sha256": sha256_file(path),
        "include_guard": guard,
        "max_conditional_depth": max_depth,
        "includes": includes,
        "include_count": len(includes),
        "project_includes": [x["name"] for x in includes if x["classification"] == "project"],
        "kernel_includes": [x["name"] for x in includes if x["classification"] == "linux_kernel"],
        "structs": uniq(structs),
        "unions": uniq(unions),
        "enums": uniq(enums),
        "typedefs": uniq(typedefs),
        "object_macros": uniq(object_macros),
        "function_macros": uniq(function_macros),
        "static_inline_functions": uniq(static_inline),
        "declared_prototypes": uniq(prototypes),
        "configuration_symbols": uniq(x for x in conditions if x.isupper()),
    }


def kb_covered_files(root: Path) -> tuple[set[str], set[str]]:
    """Files the compiler actually produced records for."""
    functions: set[str] = set()
    aggregates: set[str] = set()
    for name, sink in (("functions.jsonl", functions), ("structs.jsonl", aggregates), ("enums.jsonl", aggregates)):
        p = root / "kb" / name
        if not p.exists():
            continue
        with p.open("r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                f = row.get("file")
                if isinstance(f, str):
                    sink.add(f)
    return functions, aggregates


def scan(manifest: dict[str, Any], out_root: Path, source_root: Path) -> dict[str, Any]:
    rules = load_semantic_rules(manifest)
    exts = {str(x).lower() for x in (rules.get("source_extensions") or [])} or set(DEFAULT_HEADER_EXTS)
    header_exts = {x for x in exts if x in set(DEFAULT_HEADER_EXTS)} or set(DEFAULT_HEADER_EXTS)
    ignore_dirs = {str(x) for x in (rules.get("ignore_dirs") or [])}

    paths: list[Path] = []
    for p in sorted(source_root.rglob("*")):
        if not p.is_file() or p.suffix.lower() not in header_exts:
            continue
        if any(part in ignore_dirs for part in p.relative_to(source_root).parts):
            continue
        paths.append(p)

    local_headers = {p.name for p in paths}
    fn_files, agg_files = kb_covered_files(out_root)

    records: list[dict[str, Any]] = []
    for p in paths:
        rel_path = str(p.relative_to(source_root))
        rec = scan_header(p, rel_path, local_headers)
        rec["extraction_has_functions"] = rel_path in fn_files
        rec["extraction_has_aggregates"] = rel_path in agg_files
        rec["extraction_covered"] = rec["extraction_has_functions"] or rec["extraction_has_aggregates"]
        rec["declares_types"] = bool(rec["structs"] or rec["unions"] or rec["enums"] or rec["typedefs"])
        records.append(rec)

    uncovered = [r for r in records if not r["extraction_covered"]]
    uncovered_with_types = [r for r in uncovered if r["declares_types"]]
    coverage = {
        "generated_by": f"nic_port_headers/{TOOL_VERSION}",
        "source_root": str(source_root),
        "headers_total": len(records),
        "headers_covered_by_extraction": len(records) - len(uncovered),
        "headers_not_covered": len(uncovered),
        "headers_not_covered_declaring_types": len(uncovered_with_types),
        "coverage_ratio": round((len(records) - len(uncovered)) / len(records), 4) if records else 1.0,
        "not_covered": [r["path"] for r in uncovered],
        "not_covered_declaring_types": [r["path"] for r in uncovered_with_types],
        "totals": {
            "structs": sum(len(r["structs"]) for r in records),
            "unions": sum(len(r["unions"]) for r in records),
            "enums": sum(len(r["enums"]) for r in records),
            "typedefs": sum(len(r["typedefs"]) for r in records),
            "object_macros": sum(len(r["object_macros"]) for r in records),
            "function_macros": sum(len(r["function_macros"]) for r in records),
            "static_inline_functions": sum(len(r["static_inline_functions"]) for r in records),
            "declared_prototypes": sum(len(r["declared_prototypes"]) for r in records),
        },
    }

    write_outputs(out_root, records, coverage)
    return coverage


def write_outputs(out_root: Path, records: list[dict[str, Any]], coverage: dict[str, Any]) -> None:
    kb = out_root / "kb" / "headers.jsonl"
    kb.parent.mkdir(parents=True, exist_ok=True)
    kb.write_text("".join(json.dumps(r, sort_keys=True) + "\n" for r in records), encoding="utf-8")

    cov = out_root / "manifest" / "header_coverage.json"
    cov.parent.mkdir(parents=True, exist_ok=True)
    cov.write_text(json.dumps(coverage, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    idx = out_root / "indexes" / "header_inventory.csv"
    idx.parent.mkdir(parents=True, exist_ok=True)
    with idx.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow([
            "path", "lines", "include_guard", "includes", "kernel_includes", "structs", "unions",
            "enums", "typedefs", "object_macros", "function_macros", "static_inline_functions",
            "declared_prototypes", "max_conditional_depth", "extraction_covered",
        ])
        for r in records:
            writer.writerow([
                r["path"], r["lines"], r["include_guard"] or "", r["include_count"], len(r["kernel_includes"]),
                len(r["structs"]), len(r["unions"]), len(r["enums"]), len(r["typedefs"]),
                len(r["object_macros"]), len(r["function_macros"]), len(r["static_inline_functions"]),
                len(r["declared_prototypes"]), r["max_conditional_depth"], "yes" if r["extraction_covered"] else "no",
            ])

    top = sorted(records, key=lambda r: (len(r["structs"]) + len(r["unions"]) + len(r["enums"])), reverse=True)[:15]
    lines = [
        "# Header Inventory",
        "",
        f"Headers scanned: **{coverage['headers_total']}**  ",
        f"Covered by compiler extraction: **{coverage['headers_covered_by_extraction']}** "
        f"({coverage['coverage_ratio']:.0%})  ",
        f"Not covered: **{coverage['headers_not_covered']}** "
        f"(declaring types: **{coverage['headers_not_covered_declaring_types']}**)",
        "",
        "This index is lexical and supplemental. `kb/functions.jsonl`, `kb/structs.jsonl` and",
        "`kb/enums.jsonl` remain the authoritative compiler-derived facts.",
        "",
        "## Declaration totals",
        "",
        "| Entity | Count |",
        "|---|---:|",
    ]
    for key, value in coverage["totals"].items():
        lines.append(f"| {key.replace('_', ' ')} | {value} |")
    lines += [
        "",
        "## Largest type-declaring headers",
        "",
        "| Header | structs | unions | enums | typedefs | macros | covered |",
        "|---|---:|---:|---:|---:|---:|:--:|",
    ]
    for r in top:
        lines.append(
            f"| `{r['path']}` | {len(r['structs'])} | {len(r['unions'])} | {len(r['enums'])} | "
            f"{len(r['typedefs'])} | {len(r['object_macros']) + len(r['function_macros'])} | "
            f"{'yes' if r['extraction_covered'] else 'NO'} |"
        )
    if coverage["not_covered_declaring_types"]:
        lines += [
            "",
            "## Type-declaring headers no translation unit compiled",
            "",
            "Each of these declares types that the current build configuration never parsed.",
            "Confirm the build variant is intended before treating the baseline as complete.",
            "",
        ]
        lines += [f"- `{x}`" for x in coverage["not_covered_declaring_types"]]
    rep = out_root / "reports" / "header_summary.md"
    rep.parent.mkdir(parents=True, exist_ok=True)
    rep.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Scan repository headers for declarations the compiler pass may not cover.")
    ap.add_argument("--manifest", default=str(here / "config" / "default.manifest.json"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("scan", help="Scan headers and write the header inventory")
    p.add_argument("--source", help="Override inputs.source_root")
    p.add_argument("--root", help="Override output.root")
    p.add_argument("--json", action="store_true", help="Emit the coverage report as JSON")
    args = ap.parse_args(argv)

    try:
        manifest = load_manifest(Path(args.manifest).expanduser().resolve(), strict_env=True)
        errors = validate_manifest_basics(manifest)
        if errors:
            raise ManifestError("; ".join(errors))
    except (ManifestError, OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: manifest: {exc}", file=sys.stderr)
        return 2

    source = Path(args.source or str(get_path(manifest, "inputs.source_root"))).expanduser().resolve()
    out_root = Path(args.root or str(get_path(manifest, "output.root"))).expanduser().resolve()
    if not source.is_dir():
        print(f"ERROR: source root does not exist: {source}", file=sys.stderr)
        return 2
    out_root.mkdir(parents=True, exist_ok=True)

    coverage = scan(manifest, out_root, source)
    if args.json:
        print(json.dumps(coverage, indent=2, sort_keys=True))
    else:
        print(f"headers scanned            : {coverage['headers_total']}")
        print(f"covered by extraction      : {coverage['headers_covered_by_extraction']} ({coverage['coverage_ratio']:.0%})")
        print(f"not covered                : {coverage['headers_not_covered']}")
        print(f"  of which declare types   : {coverage['headers_not_covered_declaring_types']}")
        print(f"structs/unions/enums/typedefs: {coverage['totals']['structs']}/{coverage['totals']['unions']}"
              f"/{coverage['totals']['enums']}/{coverage['totals']['typedefs']}")
        print(f"report                     : {out_root / 'reports' / 'header_summary.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
