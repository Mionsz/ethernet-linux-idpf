# Target work item analysis — {{ITEM_ID}}

**Work item:** {{TITLE}}
**Workflow:** {{WORKFLOW}}
**Target OS:** {{TARGET_OS}}   **Driver framework:** {{DRIVER_FRAMEWORK}}
**Mandatory:** {{MANDATORY}}

This work item has no direct counterpart in the source driver. It exists because the
destination OS and its driver framework require it. Design it from the obligations below
and from the evidence this baseline already contains; do not translate a source function.

## Source mechanism being superseded

{{REPLACES}}

## Obligations

{{OBLIGATIONS}}

## Target interfaces in scope

{{TARGET_API}}

## Acceptance criteria

{{ACCEPTANCE}}

## Reference material

{{REFERENCES}}

## Source material that feeds this item

{{SOURCE_INPUTS}}

## What to produce

- `obligation_analysis`: one entry per obligation, stating how the design satisfies it and what
  in the extracted baseline supports that claim.
- `design`: target-side structure — entry points, call order, ownership, lifetime.
- `target_interfaces`: framework symbols this item implements or calls, and the contract each carries.
- `source_mapping`: which source functions, subsystems or feature families feed this item. Name real
  symbols from the baseline. If nothing in the source maps to it, say so and explain what supplies
  the behaviour instead.
- `data_structures`: target-side structures introduced, and which source state they carry.
- `synchronization`: locks held, the context each entry point runs in, sleep restrictions.
- `error_handling`: failure modes and the unwind path for each.
- `tests`: how each acceptance criterion is verified, and at which level.
- `acceptance_criteria_status`: per criterion, whether the design satisfies it and the residual gap.

State what is unknown instead of filling it in. Unknowns belong in `open_questions`, work another
scope must do belongs in `port_requests`, and concrete condition/consequence pairs belong in `risks`.
