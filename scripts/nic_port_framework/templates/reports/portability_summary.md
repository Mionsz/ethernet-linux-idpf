# NIC Port Portability Index Summary

## Instance
- Project: `{{PROJECT_NAME}}`
- Variant: `{{VARIANT}}`
- Port: `{{SOURCE_OS}}` -> `{{TARGET_OS}}`
- Functions indexed: **{{FUNCTION_COUNT}}**
- Exact/derived API hits: **{{API_HIT_COUNT}}**
- API catalogue gaps requiring review: **{{API_CATALOG_GAP_COUNT}}**

## Purpose
This report is a derived review surface over the compiler/source knowledge base. It does not replace `kb/functions.jsonl`, compiler provenance, FAR analysis, or authoritative documentation. Exact API-catalog matches provide high-confidence classification hints; regex family matches provide broader coverage hints.

## Portability distribution
{{PORTABILITY_DISTRIBUTION}}

## Risk bands
{{RISK_BANDS}}

## Highest-risk functions
{{TOP_RISK_FUNCTIONS}}

## Most-used semantic API categories
{{TOP_API_CATEGORIES}}

## Linux-specific feature families
{{LINUX_SPECIFIC_FAMILIES}}

## Include classification
{{INCLUDE_CLASSIFICATION}}

## How to use these artifacts
1. Use `indexes/function_inventory.csv` to prioritize FAR review and identify functions with high OS-coupling density.
2. Use `indexes/function_api_hits.csv` to inspect exact symbol/family detections and their detection provenance.
3. Use `indexes/linux_api_usage.csv` for project-wide API concentration and abstraction opportunities.
4. Use `indexes/linux_specific_functionality.csv` as a coverage hint for source-OS-only integration that may require rewrite/defer decisions.
5. Use `indexes/include_report.csv` to identify header-level source-OS coupling.
6. Use `indexes/api_catalog_gaps.csv` as a review queue for symbols that may deserve an exact-catalogue extension; do not auto-promote gaps.
7. Treat all scores/recommendations as triage signals, not engineering decisions. The file/subsystem/final porters remain responsible for evidence-backed design decisions.
