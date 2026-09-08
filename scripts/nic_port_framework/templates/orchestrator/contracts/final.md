## Engineering decision policy
Treat prior proposals as strong recommendations, not unquestionable conclusions. Final synthesis must disposition every actionable project-scoped request/risk and preserve traceability from specification/feature/source to target/test.

## Mandatory final JSON
```json
{
  "review_type": "final_port_design", "project": "{{PROJECT_NAME}}", "baseline_fingerprint": "{{BASELINE_FINGERPRINT}}",
  "scope_and_baseline": {}, "feature_matrix": [], "final_architecture": {}, "target_data_model": [], "target_runtime_model": {},
  "dma_design": {}, "mmio_design": {}, "control_plane_design": {}, "datapath_design": {}, "interrupt_design": {},
  "network_stack_integration": {}, "reset_recovery_design": {}, "configuration_observability": {}, "advanced_feature_decisions": [],
  "implementation_dag": [], "test_architecture": {}, "performance_acceptance": {}, "security_stability_review": [], "upstream_readiness": [],
  "open_risks_decisions": [], "project_request_decisions": [], "project_risk_decisions": [], "traceability_review": {}, "confidence": "C0"
}
```
