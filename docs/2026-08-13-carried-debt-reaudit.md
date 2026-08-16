# Carried-debt re-audit against current code

Status: completed review evidence for `D-004`
Date: 2026-08-13
Historical input: `docs/archive/plans/2026-08-12-carried-debt-ledger.md`
Execution authority: `E:/umbraflow-projects/uf-chaos/docs/architecture/parallel-implementation-plan.md`

This is a disposition audit, not a second unfinished-work ledger. Open rows are
named only to point to their single work-package owner in the consumer plan.
Line locators below were remeasured against the current tree on 2026-08-13;
the archived ledger's old locators are evidence of what was found, not current
instructions.

## Current changes that close carried rows

- `SnapshotRecord` now carries `availableTools` and `eventCursor`
  (`modules/operator/source/operator/ledger.hpp:217,230`); snapshot construction
  fills both (`ledger.cpp:5704,5706`). `C-W3-8` is therefore fully closed, not
  half open.
- Snapshot identity now includes the policy hash, exact offered-tool set and an
  availability revision (`ledger.cpp:2979-2987,5481-5576`). This closes
  `C-W3-6`, `C-W3-9` and `C-W3-10`.
- `ledger_events` now has `operation_state_changed` and
  `delivery_outcome_recorded` kinds (`ledger.cpp:493-509,2265-2267`) and current
  state/delivery paths append them. This closes `C-W67-1` and `C-W67-8`.
- The Agent/read ruling is an explicit refusal (`ledger.cpp:4492-4494`), the
  unused millisecond field is absent and guarded by
  `tests/operator/test-agent-audit-contract.cpp:381-392`, and only the one
  implemented no-progress counter remains. This closes `C-W67-3`, `C-W67-5`
  and `C-W67-6`.
- The ToolDescriptor, policy and filtered offer set now exist in production
  (`modules/operator/source/operator/tool-descriptor.hpp:176-207`,
  `ledger.cpp:5207-5218,5919`). This closes `C-W67-2`, `C-W67-11`, `C-R-1`
  and `C-R-2`.
- Operator schema identity now explicitly gives `PRAGMA user_version` no
  identity or upgrade role (`ledger.cpp:475-476`), closing `C-W4-2`; the
  negative seam measurement in `C-R-4` remains historical evidence of the
  exact-byte rule.
- Snapshot/observation retention preserves live joins and ledger-event
  retention makes both resync directions reachable
  (`ledger.cpp:2416-2520`; `tests/operator/test-ledger.cpp:3190,3236`). This
  closes `C-W3-5` and `C-W67-4`. The corrected DDL comment at
  `ledger.cpp:1641-1656` and the two live join clauses at `:6187-6188` close
  `C-W3-1` and `C-W3-2` without claiming either clause alone is the guard.

## Every carried ID

| ID | Current disposition |
|---|---|
| `C-W2-1` | Open only under `U3` (production Operator/schema reader and lifecycle owner). |
| `C-W2-2` | `U12e`; documentation of three deliberate landing shapes. |
| `C-W2-3` | `U12e`; historical evidence that the literal T4 mutation is inapplicable. |
| `C-W3-1` | **Closed**; prose now states that recapture moves `identity_hash` while the decision basis remains stable. |
| `C-W3-2` | **Closed**; prose now describes the guarded conjunction rather than either clause as independently necessary. |
| `C-W3-3` | `U12d`; historical false-green instance. |
| `C-W3-4` | `U12d`; historical false-green surface-rule instance. |
| `C-W3-5` | **Closed** by bounded snapshot/observation retention that preserves live joins. |
| `C-W3-6` | **Closed** by policy/tool-set availability identity. |
| `C-W3-7` | Open only under `U11c` (schema/terminology alignment). |
| `C-W3-8` | **Closed**; both `event_cursor` and `available_tools` landed. |
| `C-W3-9` | **Closed**; the available-tool set now moves availability identity. |
| `C-W3-10` | **Closed** with the same availability implementation. |
| `C-W3-11` | Open only under `U12c`. |
| `C-W4-1` | Open only under `U2d`. |
| `C-W4-2` | **Closed** by `U2a`'s schema-identity ruling. |
| `C-W4-3` | Open only under `U2d`. |
| `C-W4-4` | Open only under `U10b`. |
| `C-W4-5` | `U12e`; consumer-visible suite cost record. |
| `C-W4-6` | `U12d`; historical unfalsifiable T-10 instance. |
| `C-W4-7` | `U12e`, with the reusable context/cache fact destined for `docs/pitfalls/`. |
| `C-W4-8` | Open only under `U12b`. |
| `C-W4-9` | **Closed historical evidence**: the canonical plan index superseded the requested second index entry. |
| `C-W67-1` | **Closed** by delivery/state ledger events. |
| `C-W67-2` | **Closed** by U8's filtered offer side. |
| `C-W67-3` | **Closed** by deleting the unread millisecond field. |
| `C-W67-4` | **Closed** by bounded event retention and both-direction resync coverage. |
| `C-W67-5` | **Closed** by explicit Agent/read refusal. |
| `C-W67-6` | **Closed** by retaining the single step counter and no millisecond ceiling. |
| `C-W67-7` | `U12e`; record the two deliberate landing refusals. |
| `C-W67-8` | **Closed**; operation-state events are now part of the stream. |
| `C-W67-9` | `U12d`; historical second-mechanism masking instance. |
| `C-W67-10` | **Closed** by D-001's current-authority terminology sweep. |
| `C-W67-11` | **Closed** by U8's capability-derived offer set. |
| `C-R-1` | **Closed** by the full per-tool descriptor and workflow limits. |
| `C-R-2` | **Closed** by policy-owned approval requirements. |
| `C-R-3` | Open only under `U2f` (schema/DDL `controller_kind` alignment). |
| `C-R-4` | **Closed historical evidence** supporting the U2a exact-byte ruling. |
| `C-R-5` | **Closed by deletion 2026-08-16** (`a49ba85`). The exclusion had no subject left: `SCHEMA_AUTHORITIES` and its checker were removed once no schema digest was pinned outside its schema file, and `tests/test-runtime-surface.py` says so where the table used to be. Nothing to retain. |
| `C-R-6` | `U12e`; retain the atomic pre-plugin budget-debit rule. |
| `C-R-7` | **Closed historical evidence** after U2a; scalar join columns and exact-byte identity remain deliberate. |
| `C-R3-1` | `U12e`; review-only contract/schema test-name classification. |
| `C-R3-2` | `U12e`; pure history-rewrite ordering evidence. |
| `C-R3-3` | Open only under `U11d`. |
| `C-R3-4` | Open only under `U11e`. |
| `C-CO-1` | Open only under `V1c`. |
| `C-CO-2` | Open only under `U11b`; V1a has already ruled the container/error shape. |
| `C-CO-3` | **Closed historical evidence** by the 2026-08-12 archive/index move. |
| `C-CO-4` | **Closed historical evidence** by the archive closure note and landed JSON module. |

The consumer plan's summary paragraph still sends `C-W3-1`, `C-W3-2`,
`C-W3-5`, `C-W3-8` and `C-W67-4` to work that current code has closed. That
authority requires the cross-lane edit recorded in the L4 report; this file
does not open a second copy of any row.
