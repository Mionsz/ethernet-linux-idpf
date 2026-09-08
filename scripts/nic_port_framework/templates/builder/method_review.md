# NIC Port Engineering Review

## Instance
- Kind: `method`
- Project: `{{PROJECT_NAME}}`
- Port: `{{SOURCE_OS}}` -> `{{TARGET_OS}}`
- Identity: `{{FUNCTION_KEY}}`
- Source: `{{SOURCE_LOCATION}}`
- Heuristic subsystem(s): {{SUBSYSTEMS}}
- Initial portability estimate: **{{PORTABILITY_SCORE}}**
- Active TU definitions: {{DEFINES}}

## Mission
Reconstruct the semantic contract that must survive OS re-hosting. Do not perform syntactic API replacement. Separate hardware/protocol behavior from source-OS integration, identify runtime/concurrency/resource invariants, and propose target-native design only after source semantics are established.

## Required analysis
1. Architectural responsibility and feature attribution.
2. Preconditions, postconditions, state reads/writes/transitions, ownership, hardware/firmware effects, errors and cleanup.
3. Execution context, sleepability, reentrancy and reset/detach races.
4. Locks, atomics, barriers and semantic synchronization invariants.
5. Portable core versus source-OS-specific integration.
6. Per external dependency: semantic service first, target candidate second.
7. Documentation/evidence requirements.
8. Target design recommendation and alternatives.
9. Verification/tests.
10. Portability P0-P5 and confidence C0-C5.

## Context — file preamble
{{FILE_PREAMBLE}}

## Context — direct includes
{{INCLUDES}}

## Context — function source
{{FUNCTION_SOURCE}}

## Context — direct callers
{{CALLERS}}

## Context — caller excerpts
{{CALLER_SOURCES}}

## Context — direct callees
{{CALLEES}}

## Context — callee excerpts
{{CALLEE_SOURCES}}

## Context — callback/non-call references
{{CALLBACKS}}

## Context — external / OS-facing calls
{{EXTERNAL_CALLS}}

## Context — state/field accesses
{{FIELD_ACCESSES}}

## Context — relevant structures
{{STRUCTURES}}

## Context — referenced project macros
{{MACROS}}

## Derived portability index
This block is a triage/review signal built from the exact API catalogue, compiler-derived call/macro facts, configurable regex families, and configurable risk policy. It is not an implementation decision.

```json
{{PORTABILITY_INDEX}}
```

## Extraction caveats
- Direct callers/callees come from the active compilation database and compiler AST; unresolved indirect calls remain explicit.
- Field access and alias mutation are static analyses and may require PE review for escaping aliases.
- Preprocessor provenance is compiler-derived with LibTooling and lower-fidelity with the fallback backend.
- Another build configuration may compile different semantics; the TU definitions are part of the evidence.
- Exact API-catalog and regex-family detections are supplemental classification/triage signals, not proof of runtime semantics.

## Required final JSON
```json
{
  "function_key": "{{FUNCTION_KEY}}",
  "architectural_responsibility": "",
  "feature_ids": [],
  "semantic_contract": {
    "preconditions": [], "postconditions": [], "state_reads": [], "state_writes": [], "state_transitions": [],
    "ownership": [], "hardware_effects": [], "firmware_protocol_effects": [], "error_semantics": [], "cleanup_obligations": []
  },
  "runtime": {"execution_context": [], "may_sleep": "unknown", "reentrancy": "unknown", "detach_reset_races": []},
  "synchronization": {"locks_required": [], "locks_acquired": [], "atomics": [], "barriers": [], "semantic_invariants": []},
  "portable_core": [],
  "source_os_specific": [],
  "source_os_dependencies": [{"symbol": "", "semantic_service": "", "source_semantics": "", "target_candidates": [], "mapping_risk": ""}],
  "documentation_search": [{"question": "", "preferred_source": "", "status": "NEEDED"}],
  "target_design": {"recommended_strategy": "", "target_subsystem": "", "abstractions_needed": [], "rewrite_required": [], "unsupported_or_deferred": []},
  "tests": [],
  "portability_class": "{{PORTABILITY_SCORE}}",
  "confidence": "C0",
  "open_questions": []
}
```
