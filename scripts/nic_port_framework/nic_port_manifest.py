#!/usr/bin/env python3
"""Shared manifest, rules, template and output-manifest utilities for NIC Port Framework V3."""
from __future__ import annotations
import copy, datetime as dt, hashlib, json, os, re
from pathlib import Path
from typing import Any, Iterable

MANIFEST_SCHEMA_VERSION = "3.3"

class ManifestError(RuntimeError):
    pass

_ENV = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
_TPL = re.compile(r"\{\{([A-Z0-9_][A-Z0-9_.-]*)\}\}")


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for b in iter(lambda: f.read(1024 * 1024), b""):
            h.update(b)
    return h.hexdigest()


def _load_data(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise ManifestError(f"Configuration file does not exist: {path}")
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() in {".yaml", ".yml"}:
        try:
            import yaml  # type: ignore
        except ImportError as exc:
            raise ManifestError(f"YAML manifest requested but PyYAML is unavailable: {path}") from exc
        data = yaml.safe_load(text)
    else:
        data = json.loads(text)
    if not isinstance(data, dict):
        raise ManifestError(f"Top-level configuration must be an object: {path}")
    return data


def _merge(base: Any, overlay: Any) -> Any:
    """Deep merge with explicit list directives.

    Dicts merge recursively. Lists/scalars replace by default. To extend a list:
      {"$append": [..]} or {"$prepend": [..]} or {"$replace": [..]}.
    This avoids accidental list accumulation in generic manifest includes.
    """
    if isinstance(overlay, dict) and set(overlay).issubset({"$append", "$prepend", "$replace"}) and overlay:
        current = list(base) if isinstance(base, list) else []
        if "$replace" in overlay:
            return copy.deepcopy(overlay["$replace"])
        if "$prepend" in overlay:
            current = list(copy.deepcopy(overlay["$prepend"])) + current
        if "$append" in overlay:
            current.extend(copy.deepcopy(overlay["$append"]))
        return current
    if isinstance(base, dict) and isinstance(overlay, dict):
        result = copy.deepcopy(base)
        for k, v in overlay.items():
            if k == "includes":
                continue
            result[k] = _merge(result[k], v) if k in result else copy.deepcopy(v)
        return result
    return copy.deepcopy(overlay)


def _expand_env(obj: Any, strict: bool) -> Any:
    if isinstance(obj, dict):
        return {k: _expand_env(v, strict) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_expand_env(v, strict) for v in obj]
    if not isinstance(obj, str):
        return obj
    def repl(m: re.Match[str]) -> str:
        key = m.group(1)
        if key not in os.environ:
            if strict:
                raise ManifestError(f"Environment variable {key!r} referenced by manifest is not set")
            return m.group(0)
        return os.environ[key]
    return _ENV.sub(repl, obj)


def _set_path(obj: dict[str, Any], dotted: str, value: Any) -> None:
    parts = dotted.split(".")
    cur = obj
    for part in parts[:-1]:
        if not isinstance(cur.get(part), dict):
            cur[part] = {}
        cur = cur[part]
    cur[parts[-1]] = value


def _normalize_path_value(base_dir: Path, value: Any) -> Any:
    if value in (None, ""):
        return value
    if isinstance(value, list):
        return [_normalize_path_value(base_dir, x) for x in value]
    if isinstance(value, dict) and set(value).issubset({"$append", "$prepend", "$replace"}):
        return {k: [_normalize_path_value(base_dir, x) for x in (v or [])] for k, v in value.items()}
    if not isinstance(value, str):
        return value
    p = Path(value).expanduser()
    return str((base_dir / p).resolve()) if not p.is_absolute() else str(p.resolve())


def _normalize_manifest_path_fields(raw: dict[str, Any], base_dir: Path) -> dict[str, Any]:
    # These fields are references, not arbitrary strings. Normalize them while
    # the declaring manifest file's directory is still known, before includes
    # are merged into a different entrypoint manifest.
    for dotted in (
        "inputs.source_root", "inputs.compile_commands", "inputs.kernel_build_dir",
        "inputs.specification_manifests", "inputs.osal_root", "output.root", "tools.libtooling_extractor",
        "configuration.semantic_rules", "configuration.orchestration_policy", "configuration.template_index",
        "configuration.api_catalogs", "configuration.linux_specific_rules", "configuration.include_classification",
        "configuration.portability_scoring", "configuration.tooling_normalization", "configuration.report_layout",
        "configuration.context_layout", "configuration.framework_model", "configuration.output_catalog",
        "configuration.extraction_runtime", "configuration.osal_rules", "configuration.target_profile",
        "schemas.input_manifest", "schemas.output_manifest",
    ):
        value = get_path(raw, dotted, None)
        if value not in (None, "", []):
            _set_path(raw, dotted, _normalize_path_value(base_dir, value))
    return raw


def load_manifest(path: Path, *, strict_env: bool = False, _seen: set[Path] | None = None) -> dict[str, Any]:
    path = path.expanduser().resolve()
    seen = _seen if _seen is not None else set()
    if path in seen:
        raise ManifestError(f"Manifest include cycle detected at {path}")
    seen.add(path)
    raw = _load_data(path)
    raw = _expand_env(raw, strict_env)
    includes = list(raw.get("includes") or [])
    raw = _normalize_manifest_path_fields(raw, path.parent)
    merged: dict[str, Any] = {}
    for inc in includes:
        inc_path = resolve_ref(path.parent, str(inc))
        merged = _merge(merged, load_manifest(inc_path, strict_env=strict_env, _seen=seen))
    merged = _merge(merged, raw)
    merged.pop("includes", None)
    merged["_manifest"] = {
        "entrypoint": str(path),
        "directory": str(path.parent),
        "schema_version": str(merged.get("manifest_version") or MANIFEST_SCHEMA_VERSION),
    }
    seen.remove(path)
    return merged


def resolve_ref(base_dir: Path, value: str | Path | None) -> Path:
    if value is None:
        raise ManifestError("Attempted to resolve a null path reference")
    p = Path(str(value)).expanduser()
    if not p.is_absolute():
        p = base_dir / p
    return p.resolve()


def manifest_dir(manifest: dict[str, Any]) -> Path:
    return Path(manifest["_manifest"]["directory"])


def manifest_ref(manifest: dict[str, Any], value: str | Path | None) -> Path | None:
    if value in (None, ""):
        return None
    return resolve_ref(manifest_dir(manifest), value)


def get_path(obj: dict[str, Any], dotted: str, default: Any = None) -> Any:
    cur: Any = obj
    for part in dotted.split("."):
        if not isinstance(cur, dict) or part not in cur:
            return default
        cur = cur[part]
    return cur


def require_paths(manifest: dict[str, Any], dotted_names: Iterable[str]) -> None:
    missing = [x for x in dotted_names if get_path(manifest, x) in (None, "")]
    if missing:
        raise ManifestError("Manifest is missing required values: " + ", ".join(missing))


def _unique(seq: Iterable[Any]) -> list[Any]:
    out: list[Any] = []
    for x in seq:
        if x not in out:
            out.append(x)
    return out


def load_semantic_rules(manifest: dict[str, Any]) -> dict[str, Any]:
    """Load and extend semantic rules with category-aware additive semantics."""
    refs = get_path(manifest, "configuration.semantic_rules", []) or []
    if isinstance(refs, (str, Path)):
        refs = [refs]
    result: dict[str, Any] = {
        "linux_api_patterns": {},
        "freebsd_target_hints": {},
        "subsystem_patterns": {},
        "risk_weights": {},
        "ignore_dirs": [],
        "source_extensions": [],
        "generated_translation_unit_patterns": [],
    }
    files = []
    for ref in refs:
        p = manifest_ref(manifest, ref)
        assert p is not None
        data = _load_data(p)
        files.append({"path": str(p), "sha256": sha256_file(p)})
        for cat, pats in (data.get("linux_api_patterns") or {}).items():
            result["linux_api_patterns"][str(cat)] = _unique(result["linux_api_patterns"].get(str(cat), []) + [str(x) for x in pats])
        result["freebsd_target_hints"].update({str(k): str(v) for k, v in (data.get("freebsd_target_hints") or {}).items()})
        for sub, pats in (data.get("subsystem_patterns") or {}).items():
            result["subsystem_patterns"][str(sub)] = _unique(result["subsystem_patterns"].get(str(sub), []) + [str(x) for x in pats])
        result["risk_weights"].update({str(k): int(v) for k, v in (data.get("risk_weights") or {}).items()})
        result["ignore_dirs"] = _unique(result["ignore_dirs"] + [str(x) for x in (data.get("ignore_dirs") or [])])
        result["source_extensions"] = _unique(result["source_extensions"] + [str(x) for x in (data.get("source_extensions") or [])])
        result["generated_translation_unit_patterns"] = _unique(result["generated_translation_unit_patterns"] + [str(x) for x in (data.get("generated_translation_unit_patterns") or [])])
        for key in ("external_unclassified_target_hint",):
            if key in data:
                result[key] = data[key]
    result["_files"] = files
    return result



def load_config_refs(manifest: dict[str, Any], dotted: str, *, merge: str = "deep") -> dict[str, Any]:
    """Load one or more manifest-referenced configuration objects.

    `merge=deep` recursively overlays objects. `merge=catalog` merges `entries`
    by exact key while recording conflicting redefinitions. All source files are
    returned under `_files` so callers can fingerprint configuration provenance.
    """
    refs = get_path(manifest, dotted, []) or []
    if isinstance(refs, (str, Path)):
        refs = [refs]
    result: dict[str, Any] = {}
    files: list[dict[str, Any]] = []
    conflicts: list[dict[str, Any]] = []
    for ref in refs:
        p = manifest_ref(manifest, ref)
        assert p is not None
        data = _load_data(p)
        files.append({"path": str(p), "sha256": sha256_file(p)})
        if merge == "catalog":
            base_entries = result.setdefault("entries", {})
            for key, value in (data.get("entries") or {}).items():
                skey = str(key)
                if skey in base_entries and base_entries[skey] != value:
                    conflicts.append({"symbol": skey, "previous": base_entries[skey], "replacement": value, "source": str(p)})
                base_entries[skey] = copy.deepcopy(value)
            for key, value in data.items():
                if key != "entries":
                    result[key] = copy.deepcopy(value)
        else:
            result = _merge(result, data)
    result["_files"] = files
    if conflicts:
        result["_conflicts"] = conflicts
    return result


def load_single_config(manifest: dict[str, Any], dotted: str) -> dict[str, Any]:
    value = get_path(manifest, dotted)
    if not value:
        return {"_files": []}
    refs = value if isinstance(value, list) else [value]
    if len(refs) != 1:
        raise ManifestError(f"{dotted} expects exactly one configuration file")
    p = manifest_ref(manifest, refs[0])
    assert p is not None
    data = _load_data(p)
    data["_files"] = [{"path": str(p), "sha256": sha256_file(p)}]
    data["_source_path"] = str(p)
    return data


def load_api_catalog(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_config_refs(manifest, "configuration.api_catalogs", merge="catalog")

def load_linux_specific_rules(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_config_refs(manifest, "configuration.linux_specific_rules", merge="deep")

def load_include_classification(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_single_config(manifest, "configuration.include_classification")

def load_portability_scoring(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_single_config(manifest, "configuration.portability_scoring")

def load_tooling_normalization(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_single_config(manifest, "configuration.tooling_normalization")

def load_report_layout(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_single_config(manifest, "configuration.report_layout")

def load_output_catalog(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_single_config(manifest, "configuration.output_catalog")

def load_context_layout(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_single_config(manifest, "configuration.context_layout")

def load_extraction_runtime(manifest: dict[str, Any]) -> dict[str, Any]:
    return load_single_config(manifest, "configuration.extraction_runtime")

def load_osal_rules(manifest: dict[str, Any]) -> dict[str, Any]:
    rules = load_single_config(manifest, "configuration.osal_rules")
    if not rules.get("_source_path"):
        raise ManifestError("Manifest configuration.osal_rules is required")
    mode = str(rules.get("mode") or "optional").lower()
    if mode not in {"optional", "mandatory"}:
        raise ManifestError(f"osal_rules.mode must be 'optional' or 'mandatory', got '{mode}'")
    rules["mode"] = mode
    return rules

def load_target_profile(manifest: dict[str, Any]) -> dict[str, Any]:
    profile = load_single_config(manifest, "configuration.target_profile")
    if not profile.get("_source_path"):
        raise ManifestError("Manifest configuration.target_profile is required")
    for key in ("target_profile_id", "target_os", "driver_framework"):
        if not profile.get(key):
            raise ManifestError(f"target_profile is missing required field '{key}'")
    if not profile.get("work_items"):
        raise ManifestError("target_profile declares no work_items")
    return profile

def load_framework_model(manifest: dict[str, Any]) -> dict[str, Any]:
    model = load_single_config(manifest, "configuration.framework_model")
    source = Path(str(model.get("_source_path"))) if model.get("_source_path") else None
    if source and isinstance(model.get("result_schemas"), dict):
        model["result_schemas"] = {k: str(resolve_ref(source.parent, v)) for k, v in model["result_schemas"].items()}
    return model

def load_policy_from_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    refs = get_path(manifest, "configuration.orchestration_policy", []) or []
    if isinstance(refs, (str, Path)):
        refs = [refs]
    result: dict[str, Any] = {}
    files = []
    for ref in refs:
        p = manifest_ref(manifest, ref)
        assert p is not None
        data = _load_data(p)
        result = _merge(result, data)
        files.append({"path": str(p), "sha256": sha256_file(p)})
    result["_files"] = files
    return result


def load_template_index(manifest: dict[str, Any]) -> dict[str, Path]:
    ref = get_path(manifest, "configuration.template_index")
    if not ref:
        raise ManifestError("Manifest configuration.template_index is required")
    idx_path = manifest_ref(manifest, ref)
    assert idx_path is not None
    data = _load_data(idx_path)
    files = data.get("templates") or data
    if not isinstance(files, dict):
        raise ManifestError(f"Template index must contain an object named templates: {idx_path}")
    result: dict[str, Path] = {}
    for name, ref_value in files.items():
        result[str(name)] = resolve_ref(idx_path.parent, str(ref_value))
    result["__index__"] = idx_path
    return result


def render_template(path: Path, variables: dict[str, Any], *, strict: bool = True) -> str:
    text = path.read_text(encoding="utf-8")
    mapping = {str(k).upper(): (json.dumps(v, indent=2, sort_keys=True) if isinstance(v, (dict, list)) else str(v)) for k, v in variables.items()}
    missing: set[str] = set()
    def repl(m: re.Match[str]) -> str:
        key = m.group(1)
        if key not in mapping:
            missing.add(key)
            return m.group(0)
        return mapping[key]
    out = _TPL.sub(repl, text)
    if strict and missing:
        raise ManifestError(f"Template {path} has unresolved variables: {', '.join(sorted(missing))}")
    return out.rstrip() + "\n"


def template_path(index: dict[str, Path], name: str) -> Path:
    p = index.get(name)
    if p is None:
        raise ManifestError(f"Template {name!r} not found in {index.get('__index__')}")
    if not p.is_file():
        raise ManifestError(f"Template {name!r} does not exist: {p}")
    return p


def _file_entry(path: Path, root: Path, description: str, role: str, required: bool = False) -> dict[str, Any]:
    exists = path.exists()
    entry = {
        "role": role,
        "description": description,
        "path": str(path),
        "relative_path": str(path.relative_to(root)) if path.is_relative_to(root) else None,
        "exists": exists,
        "required": required,
    }
    if exists and path.is_file():
        entry.update({"size_bytes": path.stat().st_size, "sha256": sha256_file(path)})
    return entry


def write_output_manifest(root: Path, *, input_manifest: Path | None, phase: str, project: dict[str, Any] | None = None,
                          significant: list[tuple[str, str, str, bool]] | None = None,
                          directories: list[tuple[str, str, str]] | None = None,
                          next_steps: list[dict[str, Any]] | None = None,
                          know_how: list[str] | None = None,
                          status: dict[str, Any] | None = None,
                          vision: str | None = None,
                          auto_index_description: str | None = None,
                          preserve_existing: bool = True) -> Path:
    """Write the single output/exit manifest. Paths are references, never concatenated contents."""
    root = root.resolve(); root.mkdir(parents=True, exist_ok=True)
    out = root / "output_manifest.json"
    existing: dict[str, Any] = {}
    if preserve_existing and out.is_file():
        try: existing = json.loads(out.read_text(encoding="utf-8"))
        except Exception: existing = {}
    # Refresh metadata for previously indexed significant files instead of carrying
    # stale hashes/existence bits across orchestration updates.
    files: dict[str, Any] = {}
    for x in existing.get("significant_files", []):
        if not isinstance(x, dict) or not x.get("role") or not x.get("path"):
            continue
        files[x["role"]] = _file_entry(
            Path(x["path"]), root, str(x.get("description") or ""),
            str(x["role"]), bool(x.get("required", False))
        )
    for role, rel_or_abs, desc, required in significant or []:
        p = Path(rel_or_abs); p = p if p.is_absolute() else root / p
        files[role] = _file_entry(p, root, desc, role, required)

    # Preserve semantic directory roles, then auto-index every produced
    # directory so output_manifest.json remains a complete navigation entrypoint
    # even when future stages add new artifact subtrees.
    dirs: dict[str, Any] = {}
    curated_by_path: dict[str, str] = {}
    for x in existing.get("directories", []):
        if not isinstance(x, dict) or not x.get("role") or not x.get("path"):
            continue
        p = Path(x["path"])
        rel = str(p.relative_to(root)) if p.is_relative_to(root) else x.get("relative_path")
        dirs[x["role"]] = {
            "role": x["role"], "description": str(x.get("description") or ""),
            "path": str(p), "relative_path": rel, "exists": p.is_dir()
        }
        if rel:
            curated_by_path[rel] = str(x["role"])
    for role, rel_or_abs, desc in directories or []:
        p = Path(rel_or_abs); p = p if p.is_absolute() else root / p
        rel = str(p.relative_to(root)) if p.is_relative_to(root) else None
        dirs[role] = {"role": role, "description": desc, "path": str(p), "relative_path": rel, "exists": p.is_dir()}
        if rel:
            curated_by_path[rel] = role
    # The output root is itself a directory in the navigation contract.
    if "output_root" not in dirs:
        dirs["output_root"] = {
            "role": "output_root",
            "description": "Root directory of this framework run.",
            "path": str(root), "relative_path": ".", "exists": root.is_dir()
        }
        curated_by_path["."] = "output_root"
    for p in sorted((x for x in root.rglob("*") if x.is_dir()), key=lambda x: str(x)):
        rel = str(p.relative_to(root))
        if rel in curated_by_path:
            continue
        role = f"directory::{rel}"
        dirs[role] = {
            "role": role,
            "description": auto_index_description or existing.get("auto_index_description") or "Generated artifact directory (auto-indexed).",
            "path": str(p), "relative_path": rel, "exists": True,
            "auto_indexed": True
        }
    history = list(existing.get("history") or [])
    history.append({"phase": phase, "updated_utc": now_iso()})
    payload = {
        "manifest_type": "nic-port-output",
        "manifest_version": MANIFEST_SCHEMA_VERSION,
        "generated_utc": now_iso(),
        "phase": phase,
        "project": project or existing.get("project") or {},
        "input_manifest": str(input_manifest.resolve()) if input_manifest else existing.get("input_manifest"),
        "output_root": str(root),
        "vision": vision or existing.get("vision") or "NIC port framework output manifest.",
        "auto_index_description": auto_index_description or existing.get("auto_index_description"),
        "directories": sorted(dirs.values(), key=lambda x: x["role"]),
        "significant_files": sorted(files.values(), key=lambda x: x["role"]),
        "know_how": know_how or existing.get("know_how") or [],
        "next_steps": next_steps or existing.get("next_steps") or [],
        "status": status or existing.get("status") or {},
        "history": history[-100:],
    }
    out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return out


def validate_manifest_basics(manifest: dict[str, Any]) -> list[str]:
    errs: list[str] = []
    if str(manifest.get("manifest_version", "")).split(".")[0] != "3":
        errs.append("manifest_version must be 3.x for this framework")
    for key in (
        "configuration.template_index", "configuration.semantic_rules", "configuration.orchestration_policy",
        "configuration.api_catalogs", "configuration.linux_specific_rules", "configuration.include_classification",
        "configuration.portability_scoring", "configuration.tooling_normalization", "configuration.report_layout",
        "configuration.context_layout", "configuration.framework_model", "configuration.output_catalog",
        "configuration.extraction_runtime",
    ):
        if get_path(manifest, key) in (None, "", []):
            errs.append(f"missing {key}")
    return errs
