# V3.3 validation report

The following tests were executed in the development container.

## Static/configuration

- current V3.3 Python modules: `py_compile` PASS;
- 37 JSON configuration/schema/example files: parse PASS;
- default manifest load/merge: PASS;
- input manifest JSON schema: PASS;
- generated output manifest JSON schema: PASS.

## Resumable cache

Synthetic two-TU LibTooling-contract fixture:

- first run: 0 cache hits / 2 fresh extractions / quality PASS;
- second identical run: 2 cache hits / 0 fresh extractions / quality PASS;
- checkpoint records both TUs;
- two compressed semantic TU cache entries produced.

## Semantic baseline stability

After orchestration initialization, another identical cache-served extraction and reinitialization produced the same semantic baseline fingerprint:

`ab1c4fb7ae987dc4430553c20bbea81b60edf238251bbeabbf36afd348c7731e`

The exact hash is fixture-specific; the asserted property is equality across fresh/cache execution modes.

## Interrupted-run recovery

Synthetic fixture forced the second TU extractor to fail with a frontend return code while the first TU succeeded.

- initial build: non-zero exit; first TU cached;
- failure marker removed without changing extractor binary/source;
- `resume`: first TU cache hit, second TU fresh extraction;
- final quality: PASS;
- final run state: COMPLETE.

## Determinism hardening

Parallel TU results are no longer globally deduplicated in worker-completion order. Global function/struct/enum duplicate resolution is applied only after results are ordered by the authoritative compile-unit sequence.

## Local-source quality gate

Diagnostics now record local and header function counts. The healthy synthetic fixture produced one local function and zero header functions for each TU, and the local-source quality gate passed.

## C++ note

The V3.3 C++ LibTooling source was not compiled in this container because the environment lacks the Clang/LLVM development package set used on the target test host. The existing Clang-22 host remains the authoritative compilation/runtime test for that component.
