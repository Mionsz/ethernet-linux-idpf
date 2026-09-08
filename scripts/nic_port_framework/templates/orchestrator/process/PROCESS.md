# Agent Request/Risk and File-Porter Triage Process

1. Method porter performs method-scope work first.
2. It proposes a request only when work is required outside the method.
3. It registers a risk only for a concrete condition/consequence pair.
4. The method porter never supplies authoritative provenance IDs; the orchestrator assigns stable IDs, origin method/file/variant, effective scope and duplicate groups.
5. A file porter may start as soon as all methods and critical evidence for that file meet the configured gate; unrelated files do not block it.
6. The file porter receives the script-generated registry subset and must disposition every `decision_required` item exactly once.
7. Accepted file-scoped work must appear in the file design and link implementation/design/test references.
8. Project-scoped items remain visible until explicitly resolved by subsystem/final synthesis.
9. Final project synthesis must disposition every actionable project-scoped request/risk.
10. No agent may silently widen scope, delete a registry item, overwrite script-owned provenance, or convert unresolved uncertainty into fact.
