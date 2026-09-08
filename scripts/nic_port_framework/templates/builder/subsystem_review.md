# NIC Port Engineering Review

## Instance
- Kind: `subsystem`
- Project: `{{PROJECT_NAME}}`
- Port: `{{SOURCE_OS}}` -> `{{TARGET_OS}}`
- Identity: `{{SUBSYSTEM}}`

## Candidate membership
```json
{{FUNCTION_SUMMARY_JSON}}
```

## Mission
Correct subsystem membership if needed and produce a subsystem design record covering semantic responsibility, features/priority, entry points/state machine, ownership/lifetimes, runtime/concurrency, hardware/protocol effects, failure/reset/quiesce/detach, semantics to preserve, target-native architecture, dependency/order constraints, tests, risks and evidence gaps.

Do not mechanically map APIs. If a target framework conflicts with required lifecycle or queue semantics, state the conflict and redesign explicitly.
