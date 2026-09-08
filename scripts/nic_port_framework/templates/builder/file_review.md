# NIC Port Engineering Review

## Instance
- Kind: `file`
- Project: `{{PROJECT_NAME}}`
- Port: `{{SOURCE_OS}}` -> `{{TARGET_OS}}`
- Identity: `{{FILE}}`

## Mission
Integrate method evidence into a File Architecture Record. A file is an implementation container, not necessarily a subsystem boundary. Determine responsibility, features, state/resources, runtime/concurrency, hardware/protocol content, source-OS services, target disposition, risks and tests.

## Function summary
```json
{{FUNCTION_SUMMARY_JSON}}
```

## Source file
{{SOURCE_FILE}}

## Required final JSON
```json
{
  "file": "{{FILE}}",
  "purpose": "",
  "subsystems": [],
  "feature_ids": [],
  "function_disposition": [{"function_key": "", "action": "reuse|adapt|wrap|rewrite|defer", "reason": ""}],
  "state_and_resources": [],
  "synchronization_invariants": [],
  "hardware_protocol_effects": [],
  "source_os_services": [],
  "target_design": [],
  "risks": [],
  "documentation_search": [],
  "tests": [],
  "portability_class": "P0",
  "confidence": "C0"
}
```
