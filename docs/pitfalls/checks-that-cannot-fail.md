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

Mutation is strongest for a local condition. Registration inspection is
stronger for discovering tests that never run. Independent recomputation is
required for hashes and generated documents. Review remains necessary for
claims that cannot be made mechanical without inventing a second implementation.

## Current repository examples

`scripts/check_spec_bundle.py` carries the consumer bundle v1.13 pin, while the
hardening rewrite carries an independent checked-in copy. The gate can prove
those copies agree. Freshness is proved only when the gate is run against the
real consumer directory; comparing the two repository copies alone cannot
detect that the consumer moved again.

ProjectPlugin documents have an executable producer/consumer join:
`ProjectSchemaOwner::validate` applies the pinned function-specific schema on
every call. The value returned by its canonical validator is also the value the
plugin executes against, so the check does not validate one byte sequence while
executing a value cached by a different owner.

`tests/CMakeLists.txt` owns the concrete doctest/CTest registration rules. When
changing them, inspect the discovered test list in addition to running the
aggregate; a passing aggregate with a missing child is the defining false green.

## Checks that are review-only by design

Some classifications are judgement calls, such as whether a test deserves a
`contract-` name or whether two independently authored mechanisms are truly
independent. Such a declaration is acceptable only when it says at the point of
use that no executable gate enforces it. An unqualified imperative in a comment
is otherwise read as a promise and reviewed as a missing check.

## Review checklist

- Can the named production rule be neutralized without breaking fixture setup?
- Does the intended assertion, rather than an earlier guard, turn red?
- Is either side of a comparison derived from the other?
- Does CTest discover and run the concrete case?
- Is an external pin checked against the external source, not only another pin?
- For every schema, where are its producer, consumer, and joining assertion?
- If no executable detector is practical, is the limitation explicit?
