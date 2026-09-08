# Multi-Variant Semantic Merge Review

Variants: {{VARIANTS}}

The deterministic merger classified each stable function by compiler-derived semantic fingerprint and validated FAR core. Review ONLY entries in `semantic_review_queue.jsonl`. For each divergence decide whether it is a legitimate compile-time variant, a semantically equivalent source variation, an upstream analysis inconsistency, or a target feature-scope difference.

Never erase variant-specific hardware/protocol requirements merely to make the model uniform. Preserve preprocessor conditions and feature presence. Output merge decisions keyed by stable ID.

Summary:
```json
{{SUMMARY_JSON}}
```
