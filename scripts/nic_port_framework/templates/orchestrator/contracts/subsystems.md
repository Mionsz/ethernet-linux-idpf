## Engineering decision policy
Treat prior proposals as strong recommendations, not unquestionable conclusions. Reconcile cross-file semantics; do not erase real variant/hardware requirements to force uniformity.

## Mandatory final JSON
```json
{
  "subsystem": "{{IDENTITY}}", "semantic_responsibility": "", "feature_ids": [], "priority": "P0|P1|P2|P3|P4|P5",
  "entry_points": [], "state_machine": [], "data_ownership": [], "runtime_concurrency": {}, "hardware_protocol_effects": [],
  "failure_reset_quiesce": [], "source_os_incidental": [], "semantics_to_preserve": [], "target_architecture": {},
  "dependencies": [], "implementation_order": [], "tests": [], "risks": [], "evidence_gaps": [], "confidence": "C0", "open_questions": []
}
```
