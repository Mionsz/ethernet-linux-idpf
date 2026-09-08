# NIC Port Engineering Review

## Instance
- Kind: `architecture`
- Project: **{{PROJECT_NAME}}**
- Port: **{{SOURCE_OS}}** -> **{{TARGET_OS}}**

## Baseline
```json
{{BASELINE_JSON}}
```

## Compile units
{{COMPILE_UNITS}}

## Extraction summary
- Functions: **{{FUNCTION_COUNT}}**
- Structures: **{{STRUCT_COUNT}}**

Subsystem heuristic distribution:
```json
{{SUBSYSTEM_DISTRIBUTION}}
```

External semantic dependency distribution:
```json
{{EXTERNAL_DEPENDENCY_DISTRIBUTION}}
```

Initial portability distribution:
```json
{{PORTABILITY_DISTRIBUTION}}
```

High-risk functions:
{{HIGH_RISK_FUNCTIONS}}

## Mission
Reconstruct the driver as semantic subsystems before source translation. Produce a production architecture baseline covering architectural decomposition, feature model, state/lifetime model, runtime/concurrency, hardware/protocol boundary, source-OS service model, target-native architecture, portability heat map, implementation DAG, verification architecture, evidence gaps and production gates.

## Engineering rules
- Do not default to a source-OS compatibility layer.
- Preserve hardware/protocol semantics; redesign OS integration around target-native lifecycle and packet models.
- Do not infer API equivalence from names.
- Treat DMA, memory ordering, reset/teardown, queue ownership and interrupt quiescence as safety-critical.
- Distinguish requirements from implementation accidents.
- Mark unsupported claims as inference/open question.
