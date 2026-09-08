## Engineering decision policy
Treat prior proposals as strong recommendations, not unquestionable conclusions. Every script-owned `decision_required_*` registry item must receive exactly one disposition; rejection/defer is valid when justified. Scope may be escalated only explicitly with rationale/evidence.

## File Architecture Record V3 additions
The builder file JSON remains canonical and must additionally contain `request_decisions` and `risk_decisions`. Use the separately supplied `file_request_decision` and `file_risk_decision` record templates. Apply accepted file-scoped work in the file design and link design/test references.

Baseline fingerprint: `{{BASELINE_FINGERPRINT}}`.
