# Checks that cannot fail

A green check is evidence only when the property it names can make that check
red. This entry records the current review method. Historical mutation diaries,
counts that cannot be reproduced, and references to retired source layouts are
deliberately not retained here; Git history is their archive.

## The falsification rule

For every new or repaired check:

1. State one property in the test name or assertion.
2. Neutralize the production clause that is supposed to enforce it.
3. Run the smallest owning gate.
4. Confirm that the failure lands at the assertion naming that property, and
   read the failure text.
5. Restore the production clause and re-run the gate.

A red somewhere else is not a successful mutation. It may mean an earlier
validator, fixture parse, process crash, or unrelated invariant prevented the
named path from running.

## Detection matrix

| Failure shape | What a green run proves | Required detector |
| --- | --- | --- |
| The tested branch never executes | Only the fixture/setup path | Branch-targeted mutation or a witness produced inside the branch |
| A second guard refuses first | The combined refusal, not the named guard | Neutralize each guard independently and verify the intended assertion fails |
| Expected and actual come from one producer | Internal consistency | An independently derived oracle or an external committed value |
| A test executable exists but CTest does not run it | Compilation only | `ctest -N`/label inspection plus a deliberate failing case |
| An aggregate runs, but concrete case membership can drift | Whatever remains registered | Exact declared-case-to-discovered-case comparison |
| Two checked-in pins agree with each other | The two copies are synchronized | Compare with the real external bundle or upstream source |
| A schema file exists but no call applies it | File syntax at most | Name the producer, consumer, and executable assertion joining them |
| A comment or gate name claims a semantic property | Nothing executable | Add a detector, or label the claim explicitly as review-only |
| A fixture hands the subject an input its real caller cannot assemble | That the subject handles a shape nothing sends | Build the input from the producer's own envelope, or drive the subject through the boundary that assembles it |

Mutation is strongest for a local condition. Registration inspection is
stronger for discovering tests that never run. Independent recomputation is
required for hashes and generated documents. Review remains necessary for
claims that cannot be made mechanical without inventing a second implementation.

## Current repository examples

ProjectPlugin documents have an executable producer/consumer join:
`ProjectSchemaOwner::validate` applies the pinned function-specific schema on
every call. The value returned by its canonical validator is also the value the
plugin executes against, so the check does not validate one byte sequence while
executing a value cached by a different owner.

A mutation written into a file rather than through the software that owns it can
land on bytes nothing reads. `tests/operator/test-ledger.cpp` rewrites one
separator inside the Operator database's stored DDL to prove the identity
refusal fires; a SQLite b-tree split leaves the pre-split cell bytes in the
freed space of the page it split, so the statement's text appears more than once
and the live copy is not the earliest. Patching the first match alone left the
database opening cleanly, and the case's own read-back of that byte still
passed, because both were looking at dead space. Every occurrence is rewritten
now, and the refusal is asserted by the message that names schema identity so an
integrity or application-id refusal cannot stand in for it.

A subject exercised only below its own boundary is exercised against an input
nobody sends. Every gate for the declarative tier compiled the generated adapter
bare through `script::PureDataProgram::compile` and handed `next_step` a
`canonical_args` member, so the tier looked whole. The step envelope
`stepEnvelopeJcs` in `modules/operator/source/operator/ledger.cpp` builds, and
`k_stepInputSchema` in `modules/deployment/source/deployment/project-deployment.cpp`
closes, carries `frozen_plan_hash`, the observation, the state and `step_index`
and nothing else — so the generated `next_step` could never dispatch a UI action
at the real boundary, and no green run could say so. What found it was putting
the generated adapter through `ProjectPluginRegistrar::registerPlugin`
(`tests/project/test-authoring-path-parity.cpp`), not another fixture. A test
that assembles the subject's input by hand is testing the fixture's idea of the
caller.

`tests/CMakeLists.txt` owns the concrete doctest/CTest registration rules. When
changing them, inspect the discovered test list in addition to running the
aggregate; a passing aggregate with a missing child is the defining false green.

## Checks that are review-only by design

Some classifications are judgement calls, such as whether a test deserves a
`contract-` name or whether two independently authored mechanisms are truly
independent. Such a declaration is acceptable only when it says at the point of
use that no executable gate enforces it. An unqualified imperative in a comment
is otherwise read as a promise and reviewed as a missing check.

### Demoting a hand-written plugin to the declarative tier

Half of this rule is executable. Every deployment block of
`umbraflow-project.json` that names a hand-written plugin must carry a non-empty
`plugin_justification` naming the member or semantic of
`umbraflow-declarative-workflow-tool/v1` that cannot express it.
`validatePluginJustifications` in
`modules/project/source/project/project-kit.cpp` refuses an absent or blank one
from both `project build` and `project check`, and `k_projectSchema` in
`modules/deployment/source/deployment/project-directory.cpp` refuses the same
document at load through `required` and `"pattern": "\\S"`. Both check
**presence only**.

The other half — anything expressible at the declarative tier must be demoted to
it — has no gate and will not be given one. Deciding it means deciding whether a
five-function Luau module and some `umbraflow-declarative-workflow-tool/v1`
declaration compute the same thing, which is program equivalence. Every
syntactic approximation is one of the two failures this file exists to name: a
Luau-to-declaration decompiler nobody will maintain, or a heuristic no authored
plugin ever trips, which is a green that proves nothing.

It is therefore a review obligation at plugin acceptance, written down here so
the absence is explicit rather than inferred. At acceptance, read the stated
justification against the schema and refuse the plugin **by review** when the
behaviour it describes is one the declarative tier already expresses. A
justification that is present and false leaves `project check` green, by design;
it is a review finding and never a gate finding.

## Review checklist

- Can the named production rule be neutralized without breaking fixture setup?
- Does the intended assertion, rather than an earlier guard, turn red?
- Is either side of a comparison derived from the other?
- Does CTest discover and run the concrete case?
- Is an external pin checked against the external source, not only another pin?
- For every schema, where are its producer, consumer, and joining assertion?
- If no executable detector is practical, is the limitation explicit?
