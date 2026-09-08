## Engineering decision policy
Treat prior proposals as strong recommendations, not unquestionable conclusions. Preserve provenance/schema/gate invariants and distinguish facts from inference.

## Mandatory final JSON
```json
{
  "evidence_id": "{{IDENTITY}}",
  "kind": "api_semantics|method_question",
  "question": "",
  "semantic_category": "",
  "source_semantics": {"summary": "", "execution_context": [], "blocking_sleeping": "unknown", "ownership_lifetime": [], "error_semantics": [], "memory_ordering": []},
  "target_semantics": {"native_candidates": [], "recommended_candidate": "", "semantic_differences": [], "lifecycle_constraints": []},
  "mapping_analysis": {"recommendation": "", "risk": "", "affected_functions": []},
  "sources": [{"citation_id": "SRC-...", "source_type": "formal_spec|hardware_spec|protocol_spec|official_os_docs|upstream_source|vendor_source|upstream_commit|reviewed_mailing_list|secondary|unknown", "authority_class": "A0|A1|A2|A3|A4|A5", "title": "", "url_or_reference": "", "spec_clause_id": null, "locator": "section/lines/commit", "claim": "", "supports": []}],
  "status": "VERIFIED", "confidence": "C0", "open_questions": []
}
```
