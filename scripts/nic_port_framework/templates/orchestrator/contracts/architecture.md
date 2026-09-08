## Engineering decision policy
Treat prior proposals as strong recommendations, not unquestionable conclusions. Preserve provenance/schema/gate invariants, but challenge design recommendations when evidence supports a safer, simpler or more portable alternative. Never change scope silently.

## Mandatory final JSON
```json
{
  "review_type": "architecture",
  "project": "{{PROJECT_NAME}}",
  "baseline_fingerprint": "{{BASELINE_FINGERPRINT}}",
  "architectural_decomposition": [], "feature_model": [], "data_state_model": [], "runtime_concurrency_model": {},
  "hardware_protocol_boundary": [], "source_os_service_model": [], "target_os_architecture": [], "portability_heatmap": [],
  "implementation_dag": [], "verification_architecture": {}, "evidence_gaps": [], "production_gates": [],
  "confidence": "C0", "open_questions": []
}
```
