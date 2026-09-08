#!/usr/bin/env python3
"""
nic_port_llm.py

Provider-agnostic LLM access layer for the NIC porting framework.

The orchestrator must be able to (a) discover which models an account may use
and (b) execute an enriched prompt without depending on a vendor SDK.  This
module provides that behind one small interface so additional providers can be
registered later without touching orchestration logic.

Design invariants
-----------------
1. One interface: `LLMProvider.list_models()` and `LLMProvider.complete()`.
   Providers are registered by name; `copilot` is the built-in default.
2. Credentials are cached on disk per provider, owner-readable only, so an
   interactive device flow is performed once rather than per invocation.
   Explicitly supplied tokens (CLI argument or environment) are never written.
3. Only the standard library is used; no vendor SDK or network framework.
4. Failures raise `ProviderError` with the server response body attached, so a
   caller can record an auditable reason instead of a generic traceback.

Cache file location priority:
    1. NIC_PORT_LLM_TOKEN_CACHE environment variable
    2. COPILOT_TOKEN_CACHE environment variable (compatibility)
    3. ~/.cache/nic_porting/llm_tokens.json

CLI usage
---------
    python3 nic_port_llm.py providers
    python3 nic_port_llm.py models --device-flow
    python3 nic_port_llm.py token --github-token ghp_xxx
    python3 nic_port_llm.py status
    python3 nic_port_llm.py clear
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Sequence

USER_AGENT = "NICPortingPipeline/1.0"
EDITOR_VERSION = "NICPortingPipeline/1.0"
EDITOR_PLUGIN_VERSION = "porting-pipeline/1.0"

DEFAULT_CACHE_PATH = Path.home() / ".cache" / "nic_porting" / "llm_tokens.json"
TOKEN_REFRESH_BUFFER_SECS = 120

COPILOT_API_BASE = "https://api.githubcopilot.com"
COPILOT_TOKEN_URL = "https://api.github.com/copilot_internal/v2/token"
COPILOT_DEVICE_CODE_URL = "https://github.com/login/device/code"
COPILOT_DEVICE_TOKEN_URL = "https://github.com/login/oauth/access_token"
COPILOT_CLIENT_ID = "Iv1.b507a08c87ecfe98"


class ProviderError(RuntimeError):
    """Raised for authentication, transport and protocol failures."""


# ---------------------------------------------------------------------------
# Credential cache
# ---------------------------------------------------------------------------

def default_cache_path() -> Path:
    raw = os.environ.get("NIC_PORT_LLM_TOKEN_CACHE") or os.environ.get("COPILOT_TOKEN_CACHE")
    return Path(raw).expanduser() if raw else DEFAULT_CACHE_PATH


@dataclass
class ProviderCredentials:
    """Per-provider credential record persisted between runs."""

    identity_token: str = ""          # long-lived user token (device flow result)
    api_token: str = ""               # short-lived service token
    api_token_expires_at: float = 0.0
    api_base: str = ""
    saved_at: float = 0.0

    def api_token_valid(self) -> bool:
        return bool(self.api_token) and self.api_token_expires_at - TOKEN_REFRESH_BUFFER_SECS > time.time()

    def to_json(self) -> dict[str, Any]:
        return {
            "identity_token": self.identity_token,
            "api_token": self.api_token,
            "api_token_expires_at": self.api_token_expires_at,
            "api_base": self.api_base,
            "saved_at": self.saved_at,
        }

    @classmethod
    def from_json(cls, raw: Mapping[str, Any]) -> "ProviderCredentials":
        try:
            return cls(
                identity_token=str(raw.get("identity_token") or raw.get("github_token") or ""),
                api_token=str(raw.get("api_token") or raw.get("copilot_token") or ""),
                api_token_expires_at=float(raw.get("api_token_expires_at") or raw.get("copilot_expires_at") or 0.0),
                api_base=str(raw.get("api_base") or ""),
                saved_at=float(raw.get("saved_at") or 0.0),
            )
        except (TypeError, ValueError):
            return cls()


@dataclass
class TokenCache:
    """Owner-only JSON cache holding credentials for every provider."""

    path: Path = field(default_factory=default_cache_path)
    enabled: bool = True

    def _read(self) -> dict[str, Any]:
        if not self.enabled or not self.path.exists():
            return {}
        try:
            obj = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {}
        if not isinstance(obj, dict):
            return {}
        # A legacy single-provider file has flat github_token/copilot_token keys.
        if "providers" not in obj and ("github_token" in obj or "copilot_token" in obj):
            return {"version": 1, "providers": {"copilot": obj}}
        return obj

    def load(self, provider: str) -> ProviderCredentials:
        providers = self._read().get("providers") or {}
        raw = providers.get(provider)
        return ProviderCredentials.from_json(raw) if isinstance(raw, Mapping) else ProviderCredentials()

    def store(self, provider: str, creds: ProviderCredentials) -> None:
        if not self.enabled:
            return
        obj = self._read()
        providers = dict(obj.get("providers") or {})
        creds.saved_at = time.time()
        providers[provider] = creds.to_json()
        obj["version"] = 1
        obj["providers"] = providers
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        tmp.chmod(stat.S_IRUSR | stat.S_IWUSR)
        tmp.replace(self.path)

    def clear(self, provider: str | None = None) -> bool:
        if not self.path.exists():
            return False
        if provider is None:
            self.path.unlink()
            return True
        obj = self._read()
        providers = dict(obj.get("providers") or {})
        if provider not in providers:
            return False
        providers.pop(provider)
        obj["providers"] = providers
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        self.path.chmod(stat.S_IRUSR | stat.S_IWUSR)
        return True


# ---------------------------------------------------------------------------
# HTTP helper
# ---------------------------------------------------------------------------

def http_json(
    url: str,
    *,
    method: str = "GET",
    headers: Mapping[str, str] | None = None,
    payload: Any = None,
    timeout: int = 60,
) -> Any:
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    req = urllib.request.Request(url, data=data, headers=dict(headers or {}), method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise ProviderError(f"HTTP {exc.code} from {url}: {detail[:4000]}") from exc
    except urllib.error.URLError as exc:
        raise ProviderError(f"connection to {url} failed: {exc}") from exc
    if not body.strip():
        return {}
    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise ProviderError(f"non-JSON response from {url}: {body[:2000]}") from exc


# ---------------------------------------------------------------------------
# Provider interface
# ---------------------------------------------------------------------------

class LLMProvider:
    """Base class every provider implements."""

    name = "base"

    def __init__(
        self,
        *,
        token: str | None = None,
        interactive: bool = False,
        api_base: str | None = None,
        model: str | None = None,
        cache: TokenCache | None = None,
        verbose: bool = False,
        options: Mapping[str, Any] | None = None,
    ) -> None:
        self._explicit_token = token or None
        self._interactive = interactive
        self._api_base = (api_base or "").rstrip("/")
        self._model = model
        self._cache = cache or TokenCache()
        self._verbose = verbose
        self._options = dict(options or {})

    @property
    def api_base(self) -> str:
        return self._api_base

    @property
    def cache(self) -> TokenCache:
        return self._cache

    @property
    def default_model(self) -> str | None:
        return self._model

    def log(self, message: str) -> None:
        if self._verbose:
            print(f"[{self.name}] {message}", file=sys.stderr, flush=True)

    def get_token(self) -> str:
        raise NotImplementedError

    def list_models(self, timeout: int = 30) -> list[dict[str, Any]]:
        raise NotImplementedError

    def complete(self, prompt: str, *, model: str | None = None, timeout: int = 1800) -> str:
        raise NotImplementedError

    def auth_status(self) -> dict[str, Any]:
        creds = self._cache.load(self.name)
        return {
            "provider": self.name,
            "cache_path": str(self._cache.path),
            "cache_enabled": self._cache.enabled,
            "explicit_token": bool(self._explicit_token),
            "identity_token_cached": bool(creds.identity_token),
            "api_token_cached": bool(creds.api_token),
            "api_token_valid": creds.api_token_valid(),
            "api_token_expires_in": max(0.0, creds.api_token_expires_at - time.time()) if creds.api_token_expires_at else 0.0,
            "api_base": self.api_base,
            "default_model": self.default_model,
        }

    def clear_cache(self) -> bool:
        return self._cache.clear(self.name)

    @staticmethod
    def _parse_expiry(value: Any) -> float:
        if isinstance(value, (int, float)) and value > 0:
            return float(value)
        if isinstance(value, str) and value.strip():
            from datetime import datetime
            try:
                return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
            except ValueError:
                pass
        return time.time() + 1800

    @staticmethod
    def _extract_message(data: Any) -> str:
        choices = data.get("choices") if isinstance(data, Mapping) else None
        if not isinstance(choices, list) or not choices:
            raise ProviderError(f"completion response contained no choices: {json.dumps(data)[:2000]}")
        message = choices[0].get("message") if isinstance(choices[0], Mapping) else None
        content = message.get("content") if isinstance(message, Mapping) else None
        if isinstance(content, list):
            content = "".join(str(x.get("text") or "") for x in content if isinstance(x, Mapping))
        if not isinstance(content, str) or not content.strip():
            raise ProviderError(f"completion response contained no content: {json.dumps(data)[:2000]}")
        return content


class CopilotProvider(LLMProvider):
    """GitHub Copilot: GitHub identity token exchanged for a short-lived API token."""

    name = "copilot"

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._explicit_token = self._explicit_token or os.environ.get("GITHUB_TOKEN") or None
        self._api_base = (self._api_base or os.environ.get("COPILOT_API_BASE_URL") or COPILOT_API_BASE).rstrip("/")
        self._token_url = str(self._options.get("token_url") or os.environ.get("COPILOT_TOKEN_URL") or COPILOT_TOKEN_URL)
        self._client_id = str(self._options.get("client_id") or COPILOT_CLIENT_ID)
        self._mem_token: str | None = None
        self._mem_expires_at: float = 0.0

    def _headers(self, token: str) -> dict[str, str]:
        return {
            "Authorization": f"Bearer {token}",
            "Accept": "application/json",
            "Content-Type": "application/json",
            "User-Agent": USER_AGENT,
            "Copilot-Integration-Id": "vscode-chat",
            "Editor-Version": EDITOR_VERSION,
            "Editor-Plugin-Version": EDITOR_PLUGIN_VERSION,
            "Openai-Intent": "conversation-panel",
        }

    def get_token(self) -> str:
        if self._mem_token and self._mem_expires_at - TOKEN_REFRESH_BUFFER_SECS > time.time():
            return self._mem_token
        creds = self._cache.load(self.name)
        if creds.api_token_valid():
            self.log("reusing cached API token")
            self._mem_token, self._mem_expires_at = creds.api_token, creds.api_token_expires_at
            if creds.api_base:
                self._api_base = creds.api_base
            return creds.api_token

        identity = self._resolve_identity_token(creds)
        data = http_json(
            self._token_url,
            headers={
                "Authorization": f"token {identity}",
                "Accept": "application/json",
                "User-Agent": USER_AGENT,
                "Editor-Version": EDITOR_VERSION,
                "Editor-Plugin-Version": EDITOR_PLUGIN_VERSION,
                "Copilot-Integration-Id": "vscode-chat",
            },
            timeout=30,
        )
        token = str((data or {}).get("token") or "")
        if not token:
            raise ProviderError(f"token exchange returned no token: {json.dumps(data)[:2000]}")
        expires_at = self._parse_expiry((data or {}).get("expires_at"))
        endpoint_api = ((data or {}).get("endpoints") or {}).get("api") if isinstance(data, Mapping) else None
        if isinstance(endpoint_api, str) and endpoint_api.strip():
            self._api_base = endpoint_api.rstrip("/")
        self._mem_token, self._mem_expires_at = token, expires_at
        # A caller-supplied PAT must not leak into the cache; only its short-lived exchange result is reusable.
        creds.api_token = token
        creds.api_token_expires_at = expires_at
        creds.api_base = self._api_base
        if not self._explicit_token:
            self._cache.store(self.name, creds)
            self.log(f"API token cached to {self._cache.path}")
        self.log(f"API token valid for {expires_at - time.time():.0f}s")
        return token

    def _resolve_identity_token(self, creds: ProviderCredentials) -> str:
        if self._explicit_token:
            return self._explicit_token
        if creds.identity_token:
            self.log(f"reusing cached GitHub token from {self._cache.path}")
            return creds.identity_token
        if not self._interactive:
            raise ProviderError(
                "no GitHub token available; supply --llm-token, set GITHUB_TOKEN, "
                "or authenticate once with --llm-device-flow"
            )
        identity = self._device_flow()
        creds.identity_token = identity
        self._cache.store(self.name, creds)
        self.log(f"GitHub token cached to {self._cache.path}")
        return identity

    def _device_flow(self) -> str:
        data = http_json(
            COPILOT_DEVICE_CODE_URL,
            method="POST",
            headers={"Content-Type": "application/json", "Accept": "application/json", "User-Agent": USER_AGENT},
            payload={"client_id": self._client_id, "scope": "copilot"},
            timeout=30,
        )
        device_code = str(data["device_code"])
        interval = int(data.get("interval") or 5)
        deadline = time.time() + int(data.get("expires_in") or 900)
        print(
            f"\n[{self.name}] Open {data['verification_uri']} and enter code: {data['user_code']}\n",
            file=sys.stderr,
            flush=True,
        )
        while time.time() < deadline:
            time.sleep(interval)
            try:
                poll = http_json(
                    COPILOT_DEVICE_TOKEN_URL,
                    method="POST",
                    headers={"Content-Type": "application/json", "Accept": "application/json", "User-Agent": USER_AGENT},
                    payload={
                        "client_id": self._client_id,
                        "device_code": device_code,
                        "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                    },
                    timeout=30,
                )
            except ProviderError:
                continue
            if poll.get("access_token"):
                print(f"[{self.name}] authorization complete", file=sys.stderr, flush=True)
                return str(poll["access_token"])
            error = str(poll.get("error") or "")
            if error == "authorization_pending":
                continue
            if error == "slow_down":
                interval += 5
                continue
            raise ProviderError(f"device flow failed: {error} {poll.get('error_description') or ''}".strip())
        raise ProviderError("device flow timed out waiting for user authorization")

    def list_models(self, timeout: int = 30) -> list[dict[str, Any]]:
        token = self.get_token()
        data = http_json(f"{self.api_base}/models", headers=self._headers(token), timeout=timeout)
        models = data if isinstance(data, list) else (data.get("data") if isinstance(data, Mapping) else None)
        if not isinstance(models, list):
            raise ProviderError(f"unexpected /models response: {json.dumps(data)[:2000]}")
        return [x for x in models if isinstance(x, Mapping)]

    def complete(self, prompt: str, *, model: str | None = None, timeout: int = 1800) -> str:
        selected = model or self._model
        if not selected:
            raise ProviderError("no model selected; pass --llm-model or configure policy llm.providers.copilot.model")
        token = self.get_token()
        payload: dict[str, Any] = {
            "model": selected,
            "messages": [{"role": "user", "content": prompt}],
            "stream": False,
        }
        if self._options.get("temperature") is not None:
            payload["temperature"] = self._options["temperature"]
        if self._options.get("max_output_tokens"):
            payload["max_tokens"] = int(self._options["max_output_tokens"])
        data = http_json(
            f"{self.api_base}/chat/completions",
            method="POST",
            headers=self._headers(token),
            payload=payload,
            timeout=timeout,
        )
        return self._extract_message(data)


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

DEFAULT_PROVIDER = "copilot"
_PROVIDERS: dict[str, type[LLMProvider]] = {}


def register_provider(cls: type[LLMProvider]) -> type[LLMProvider]:
    _PROVIDERS[cls.name] = cls
    return cls


def provider_names() -> list[str]:
    return sorted(_PROVIDERS)


def create_provider(name: str | None = None, **kwargs: Any) -> LLMProvider:
    key = (name or DEFAULT_PROVIDER).strip().lower()
    cls = _PROVIDERS.get(key)
    if cls is None:
        raise ProviderError(f"unknown provider {key!r}; available: {', '.join(provider_names())}")
    return cls(**kwargs)


register_provider(CopilotProvider)


def summarize_models(models: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for m in models:
        caps = m.get("capabilities") if isinstance(m.get("capabilities"), Mapping) else {}
        endpoints = caps.get("supports") or caps.get("endpoints") or {}
        chat = True
        if isinstance(endpoints, Mapping):
            chat = "chat_completions" in endpoints or "chat" in str(caps.get("type") or "") or bool(endpoints)
        elif isinstance(endpoints, list):
            chat = any("chat" in str(x) for x in endpoints)
        rows.append({
            "id": str(m.get("id") or m.get("name") or "?"),
            "name": str(m.get("name") or ""),
            "vendor": str(m.get("vendor") or ""),
            "version": str(m.get("version") or ""),
            "family": str((caps.get("family") if isinstance(caps, Mapping) else "") or ""),
            "picker_enabled": bool(m.get("model_picker_enabled", False)),
            "chat": chat,
        })
    rows.sort(key=lambda x: x["id"])
    return rows


def print_models(rows: Sequence[Mapping[str, Any]]) -> None:
    print(f"{'MODEL ID':<48} {'CHAT':^6} {'PICKER':^7} {'VENDOR / FAMILY'}")
    print("-" * 88)
    for r in rows:
        family = r.get("family") or r.get("version") or ""
        vendor = r.get("vendor") or ""
        print(f"{r['id']:<48} {'yes' if r['chat'] else 'no':^6} {'yes' if r['picker_enabled'] else 'no':^7} {vendor} {family}".rstrip())
    print(f"\nTotal: {len(rows)} model(s). Use the MODEL ID with --llm-model <id>.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument("--provider", default=DEFAULT_PROVIDER, choices=provider_names())
    p.add_argument("--token", default=None, help="Explicit provider token; never written to the credential cache")
    p.add_argument("--device-flow", action="store_true", help="Authenticate interactively and cache the result")
    p.add_argument("--api-base", default=None)
    p.add_argument("--cache-path", default=None)
    p.add_argument("--no-cache", action="store_true")
    p.add_argument("--verbose", action="store_true")


def _provider_from_args(args: argparse.Namespace) -> LLMProvider:
    cache = TokenCache(
        path=Path(args.cache_path).expanduser() if args.cache_path else default_cache_path(),
        enabled=not args.no_cache,
    )
    return create_provider(
        args.provider,
        token=args.token,
        interactive=args.device_flow,
        api_base=args.api_base,
        cache=cache,
        verbose=args.verbose,
    )


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Inspect and authenticate LLM providers used by the porting orchestrator.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("providers", help="List registered providers")

    p = sub.add_parser("models", help="List models available to the authenticated account")
    _add_common(p)
    p.add_argument("--json", action="store_true")

    p = sub.add_parser("token", help="Print a valid API token")
    _add_common(p)

    p = sub.add_parser("status", help="Show cached credential state")
    _add_common(p)

    p = sub.add_parser("clear", help="Delete cached credentials")
    _add_common(p)
    p.add_argument("--all-providers", action="store_true")

    args = ap.parse_args(argv)
    if args.cmd == "providers":
        print("\n".join(provider_names()))
        return 0

    provider = _provider_from_args(args)
    try:
        if args.cmd == "models":
            rows = summarize_models(provider.list_models())
            if args.json:
                print(json.dumps(rows, indent=2, sort_keys=True))
            else:
                print_models(rows)
        elif args.cmd == "token":
            print(provider.get_token())
        elif args.cmd == "status":
            print(json.dumps(provider.auth_status(), indent=2, sort_keys=True))
        elif args.cmd == "clear":
            cleared = provider.cache.clear(None if args.all_providers else args.provider)
            print("cleared" if cleared else "nothing to clear")
    except ProviderError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
