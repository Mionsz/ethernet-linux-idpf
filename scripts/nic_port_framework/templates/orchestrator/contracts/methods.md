## Engineering decision policy
Treat prior proposals as strong recommendations, not unquestionable conclusions. Preserve provenance/schema/gate invariants, but challenge design recommendations when evidence supports a safer, simpler or more portable alternative. Never change scope silently.

## FAR V3 additions
The builder FAR JSON remains canonical. In the SAME final JSON object also include `port_requests`, `risks`, and `spec_clause_ids`. These are proposals only. Do not invent provenance fields; the orchestrator assigns stable IDs, origin method/file/variant, scope and duplicate groups.

Use the separately supplied record templates `method_port_request` and `method_risk`. Only create a request when another scope actually needs work. Only create a risk for a concrete condition/consequence pair; research-only unknowns belong in `documentation_search`/`open_questions`.

Baseline fingerprint: `{{BASELINE_FINGERPRINT}}`.
