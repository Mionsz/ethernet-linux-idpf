#!/usr/bin/env python3
"""Derived portability indexes for NIC Port Framework.

This module deliberately consumes the canonical compiler/source KB and external
configuration. It never acts as the source of truth for calls, types, or source
locations. Exact registry/regex scans are supplemental classification and triage
signals that improve auditability and report ergonomics.
"""
from __future__ import annotations
import csv, json, re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable


def _ids(text: str) -> set[str]:
    return set(re.findall(r"\b[A-Za-z_]\w*\b", text or ""))


def _threshold(value: float, rows: list[dict[str, Any]], key: str) -> str:
    for row in rows:
        limit = row.get("max_score")
        if limit is None or value <= float(limit):
            return str(row[key])
    return str(rows[-1][key]) if rows else "UNKNOWN"


def _category_weights(semantic_rules: dict[str, Any], scoring: dict[str, Any]) -> dict[str, int]:
    out = {str(k): int(v) for k, v in (semantic_rules.get("risk_weights") or {}).items()}
    out.update({str(k): int(v) for k, v in (scoring.get("category_weights") or {}).items()})
    return out


def analyze_function(fn: Any, *, api_catalog: dict[str, Any], semantic_rules: dict[str, Any],
                     linux_specific: dict[str, Any], scoring: dict[str, Any]) -> dict[str, Any]:
    entries = api_catalog.get("entries") or {}
    regex_map = semantic_rules.get("linux_api_patterns") or {}
    weights = _category_weights(semantic_rules, scoring)
    source_weights = {str(k): float(v) for k, v in (scoring.get("detection_source_weights") or {}).items()}
    hits: list[dict[str, Any]] = []
    seen: set[tuple[str, str, str]] = set()

    def add(symbol: str, category: str, source: str, *, line: Any = None, entry: dict[str, Any] | None = None) -> None:
        k = (symbol, category, source)
        if k in seen:
            return
        seen.add(k)
        e = entry or {}
        hits.append({
            "symbol": symbol,
            "semantic_category": category,
            "source_group": e.get("source_group"),
            "symbol_kind": e.get("symbol_kind"),
            "detection_source": source,
            "line": line,
            "risk_weight": int(e.get("risk_weight", weights.get(category, 1))),
            "technical_annotation": e.get("technical_annotation") or e.get("description") or "",
        })

    # 1. Compiler-derived external calls are the highest-value location signal.
    for call in getattr(fn, "calls_external", []) or []:
        symbol = str(call.get("name") or "")
        if not symbol or symbol == "<indirect>":
            continue
        ent = entries.get(symbol)
        if ent:
            cats = ent.get("semantic_categories") or [ent.get("semantic_category")]
            for cat in [x for x in cats if x]:
                add(symbol, str(cat), "ast_external_call", line=call.get("line"), entry=ent)
        else:
            for cat in call.get("categories") or []:
                add(symbol, str(cat), "ast_external_call", line=call.get("line"))

    # 2. Compiler-derived macro expansions catch important APIs that are not calls.
    for ev in getattr(fn, "macro_expansions", []) or []:
        symbol = str(ev.get("name") or "")
        ent = entries.get(symbol)
        if ent:
            cats = ent.get("semantic_categories") or [ent.get("semantic_category")]
            for cat in [x for x in cats if x]:
                add(symbol, str(cat), "macro_expansion", line=(ev.get("location") or {}).get("line") or ev.get("line"), entry=ent)

    # 3. Exact registry lexical presence finds types/constants/helpers invisible to a call-only graph.
    identifiers = _ids(getattr(fn, "full_source", ""))
    for symbol in sorted(identifiers.intersection(entries.keys())):
        ent = entries[symbol]
        cats = ent.get("semantic_categories") or [ent.get("semantic_category")]
        for cat in [x for x in cats if x]:
            add(symbol, str(cat), "lexical_exact", entry=ent)

    # 4. Regex family coverage extends beyond the exact catalogue. Apply to identifiers,
    # not arbitrary source text, so comments/strings do not dominate the result.
    exact_symbols = {h["symbol"] for h in hits}
    for symbol in sorted(identifiers):
        if symbol in exact_symbols:
            continue
        for category, patterns in regex_map.items():
            if any(re.search(str(p), symbol) for p in patterns):
                add(symbol, str(category), "regex_family")

    # Linux-specific feature-family scans are intentionally source-text based because
    # some families identify declarations or struct forms rather than API symbols.
    feature_rows: list[dict[str, Any]] = []
    text = getattr(fn, "full_source", "") or ""
    for family, patterns in (linux_specific.get("families") or {}).items():
        matches: list[str] = []
        for pat in patterns:
            try:
                matches.extend(m.group(0) for m in re.finditer(str(pat), text, re.M))
            except re.error:
                continue
        if matches:
            feature_rows.append({"feature_family": str(family), "match_count": len(matches), "sample_matches": sorted(set(matches))[:8]})

    # Scoring: aggregate unique symbols per category, cap density, and prefer stronger
    # detection sources. The score is only a triage signal; FAR/file decisions remain authoritative.
    cap = int(scoring.get("category_hit_cap", 3))
    by_cat: dict[str, dict[str, float]] = defaultdict(dict)
    for hit in hits:
        cat = str(hit["semantic_category"])
        sym = str(hit["symbol"])
        sw = source_weights.get(str(hit["detection_source"]), 1.0)
        by_cat[cat][sym] = max(sw, by_cat[cat].get(sym, 0.0))
    score = 0.0
    reasons: list[str] = []
    for cat, symbols in sorted(by_cat.items()):
        cat_weight = float(weights.get(cat, 1))
        strength = sum(sorted(symbols.values(), reverse=True)[:cap])
        subtotal = cat_weight * strength
        score += subtotal
        reasons.append(f"{cat}: {len(symbols)} unique hit(s), weighted {subtotal:.2f}")
    for subsystem, delta in (scoring.get("subsystem_adjustments") or {}).items():
        if subsystem in (getattr(fn, "subsystems", []) or []):
            score += float(delta)
            reasons.append(f"subsystem adjustment {subsystem}: +{delta}")
    rounded = round(score, 2)
    portability = _threshold(rounded, list(scoring.get("class_thresholds") or []), "class")
    band = _threshold(rounded, list(scoring.get("risk_bands") or []), "band")
    recommendation = _threshold(rounded, list(scoring.get("recommendation_rules") or []), "recommendation")
    omit_groups = set(str(x) for x in (scoring.get("omit_candidate_source_groups") or []))
    omit_min = float(scoring.get("omit_min_score", 10**9))
    omit_candidate = rounded >= omit_min and any(str(h.get("source_group")) in omit_groups for h in hits)

    return {
        "api_hits": hits,
        "linux_specific_features": feature_rows,
        "risk": {
            "score": rounded,
            "band": band,
            "portability_class": portability,
            "recommendation": recommendation,
            "omit_candidate": omit_candidate,
            "reasons": reasons,
        },
    }


def classify_include(include: str, style: str, rules: dict[str, Any]) -> str:
    if include in set(str(x) for x in (rules.get("standard_c_headers") or [])):
        return str(rules.get("standard_category") or "standard_c")
    for row in rules.get("prefix_categories") or []:
        if re.search(str(row.get("pattern") or ""), include):
            return str(row.get("category") or "classified")
    if style == "quote":
        return str(rules.get("local_quote_category") or "project_or_local")
    return str(rules.get("other_angle_category") or "external_or_platform")


def scan_includes(source_root: Path, source_files: Iterable[str], rules: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    inc_re = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
    for rel in sorted(set(str(x) for x in source_files)):
        p = source_root / rel
        if not p.is_file():
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for m in inc_re.finditer(text):
            style = "angle" if m.group(1) == "<" else "quote"
            inc = m.group(2)
            rows.append({"file": rel, "include": inc, "include_style": style, "classification": classify_include(inc, style, rules)})
    return rows


def _csv(path: Path, rows: list[dict[str, Any]], columns: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        w.writeheader()
        for row in rows:
            cooked = {}
            for k in columns:
                v = row.get(k)
                if isinstance(v, (list, dict)):
                    v = json.dumps(v, sort_keys=True, separators=(",", ":"))
                cooked[k] = v
            w.writerow(cooked)


def build_indexes(*, out: Path, source_root: Path, functions: list[Any], semantic_rules: dict[str, Any],
                  api_catalog: dict[str, Any], linux_specific: dict[str, Any], include_rules: dict[str, Any],
                  scoring: dict[str, Any], report_layout: dict[str, Any], source_files: Iterable[str]) -> dict[str, Any]:
    index_dir = out / "indexes"; index_dir.mkdir(parents=True, exist_ok=True)
    analyses: dict[str, dict[str, Any]] = {}
    api_rows: list[dict[str, Any]] = []
    function_rows: list[dict[str, Any]] = []
    linux_rows: list[dict[str, Any]] = []

    for fn in functions:
        a = analyze_function(fn, api_catalog=api_catalog, semantic_rules=semantic_rules, linux_specific=linux_specific, scoring=scoring)
        analyses[str(fn.stable_id or fn.key)] = a
        fn.catalog_api_hits = a["api_hits"]
        fn.linux_specific_features = a["linux_specific_features"]
        fn.porting_risk = a["risk"]
        # Use the richer derived portability class as the initial triage estimate.
        fn.portability_score = str(a["risk"]["portability_class"])
        fn.portability_reasons = list(a["risk"]["reasons"])
        for h in a["api_hits"]:
            api_rows.append({"stable_id": fn.stable_id, "function_key": fn.key, "file": fn.file, "function": fn.name, **h})
        for feat in a["linux_specific_features"]:
            linux_rows.append({"stable_id": fn.stable_id, "function_key": fn.key, "file": fn.file, "function": fn.name, **feat})
        function_rows.append({
            "stable_id": fn.stable_id, "function_key": fn.key, "name": fn.name, "file": fn.file,
            "line_start": fn.line_start, "line_end": fn.line_end, "line_count": max(0, fn.line_end-fn.line_start+1),
            "subsystems": fn.subsystems, "portability_class": fn.portability_score,
            "risk_score": a["risk"]["score"], "risk_band": a["risk"]["band"], "recommendation": a["risk"]["recommendation"],
            "api_hit_count": len(a["api_hits"]), "api_categories": sorted(set(h["semantic_category"] for h in a["api_hits"])),
            "linux_specific_features": sorted(x["feature_family"] for x in a["linux_specific_features"]),
            "callback_reference_count": len(getattr(fn, "callback_references", []) or []),
            "indirect_call_count": len(getattr(fn, "indirect_call_candidates", []) or []),
        })

    include_rows = scan_includes(source_root, source_files, include_rules)
    agg: dict[tuple[str, str, str | None], dict[str, Any]] = {}
    for row in api_rows:
        k = (str(row["symbol"]), str(row["semantic_category"]), row.get("source_group"))
        a = agg.setdefault(k, {"symbol": k[0], "semantic_category": k[1], "source_group": k[2], "functions": set(), "occurrences": 0, "detection_sources": set()})
        a["functions"].add(row["function_key"]); a["occurrences"] += 1; a["detection_sources"].add(row["detection_source"])
    api_usage = [{**v, "functions": sorted(v["functions"]), "detection_sources": sorted(v["detection_sources"])} for v in agg.values()]
    api_usage.sort(key=lambda x: (-x["occurrences"], x["semantic_category"], x["symbol"]))

    # Iterative catalogue feedback: symbols observed by compiler/regex classification but
    # absent from the exact API catalogue become explicit review candidates. This does
    # not mutate the catalogue; an engineer may add a reviewed extension in a later run.
    exact_entries = api_catalog.get("entries") or {}
    gap_agg: dict[tuple[str, str], dict[str, Any]] = {}
    for row in api_rows:
        symbol = str(row.get("symbol") or "")
        if not symbol or symbol in exact_entries:
            continue
        if str(row.get("detection_source")) not in {"ast_external_call", "regex_family"}:
            continue
        key = (symbol, str(row.get("semantic_category") or "unclassified"))
        g = gap_agg.setdefault(key, {
            "symbol": symbol, "semantic_category": key[1], "functions": set(),
            "files": set(), "detection_sources": set(), "occurrences": 0,
            "suggested_action": "REVIEW_FOR_API_CATALOG_EXTENSION",
        })
        g["functions"].add(str(row.get("function_key") or ""))
        g["files"].add(str(row.get("file") or ""))
        g["detection_sources"].add(str(row.get("detection_source") or ""))
        g["occurrences"] += 1
    catalog_gaps = [
        {**v, "functions": sorted(x for x in v["functions"] if x),
         "files": sorted(x for x in v["files"] if x),
         "detection_sources": sorted(x for x in v["detection_sources"] if x)}
        for v in gap_agg.values()
    ]
    catalog_gaps.sort(key=lambda x: (-x["occurrences"], x["semantic_category"], x["symbol"]))

    cols = report_layout
    _csv(index_dir / "function_inventory.csv", function_rows, list(cols.get("function_inventory_columns") or function_rows[0].keys() if function_rows else []))
    _csv(index_dir / "function_api_hits.csv", api_rows, list(cols.get("function_api_hits_columns") or []))
    _csv(index_dir / "include_report.csv", include_rows, list(cols.get("include_report_columns") or []))
    _csv(index_dir / "linux_specific_functionality.csv", linux_rows, list(cols.get("linux_specific_columns") or []))
    _csv(index_dir / "linux_api_usage.csv", api_usage, list(cols.get("api_usage_columns") or []))
    _csv(index_dir / "api_catalog_gaps.csv", catalog_gaps, list(cols.get("api_catalog_gap_columns") or []))
    (index_dir / "function_portability.jsonl").write_text("".join(json.dumps({"stable_id": k, **v}, sort_keys=True)+"\n" for k,v in sorted(analyses.items())), encoding="utf-8")

    return {
        "analyses": analyses,
        "function_rows": function_rows,
        "api_rows": api_rows,
        "include_rows": include_rows,
        "linux_specific_rows": linux_rows,
        "api_usage": api_usage,
        "api_catalog_gaps": catalog_gaps,
        "summary": {
            "functions": len(function_rows), "api_hits": len(api_rows), "api_catalog_gaps": len(catalog_gaps),
            "portability": dict(Counter(x["portability_class"] for x in function_rows)),
            "risk_bands": dict(Counter(x["risk_band"] for x in function_rows)),
            "api_categories": dict(Counter(x["semantic_category"] for x in api_rows)),
            "linux_specific_families": dict(Counter(x["feature_family"] for x in linux_rows)),
            "include_classification": dict(Counter(x["classification"] for x in include_rows)),
        },
    }
