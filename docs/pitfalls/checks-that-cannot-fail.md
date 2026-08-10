# Checks that cannot fail

A test that stays green after the property it names is removed proves nothing.
The same is true one level up, of the gates themselves: a configured check that
can never report is worse than a missing one, because the missing one is
visible. This repository has now found the failure in both places — in test
bodies, recorded in
[the runtime hardening review](../reviews/2026-08-10-runtime-hardening-review.md),
and in the build's own wiring, below.

The discipline that catches both is the same. Remove the property, watch the
check turn red, put the property back. An untested check is a claim, not a
gate.

## They are one family, not a run of coincidences

Four separate instances were found on 2026-08-10, in four unrelated files, and
six more on 2026-08-11 — one in a database column, three in the harness that
was running the mutations meant to catch the rest, one in the exported
contract suite itself, and one between a schema file and the DDL that stores
what it describes. They are the same defect wearing ten costumes: **a name
exists, the name promises something, and nothing verifies the promise.**

| Instance | The name that promised | What verified it |
|---|---|---|
| `HeaderFilterRegex` in `.clang-tidy` | "headers outside `tests/external` are analyzed" | nothing — the pattern matched no path, so every header diagnostic was dropped as non-user code |
| `SOURCE_ROOTS` in `scripts/check_cpp_format.py` and `scripts/check_safety.py` | "every first-party C++ source is formatted and safety-checked" | nothing — `contract-suite/` was absent from the tuple, so an entire exported module was unscanned |
| `test-annotate-backend` | "the authoring authority is gated" | nothing — the suite existed and ran in no CTest at all |
| `cpp_add_contract_suite` | "these cases run" | nothing — the helper builds the binary `NO_CTEST` and registers one CTest per `CASES` entry, so seven compiled cases in `tests/operator/test-project-plugin-contract.cpp` executed in no gate |
| `authority_decisions.decision_basis_hash` (found 2026-08-11) | "the ledger records which decision basis authorised this dispatch" | nothing — the column is written and never read. `reserveDispatch` takes the basis from `operation_plans`, and no public surface reads `authority_decisions` back, so corrupting the stored value turns no test red |
| `ctest -R <name>` in a mutation harness (found 2026-08-11) | "the case this mutation targets ran and passed" | nothing — `ctest -R` exits 0 when the filter matches no test, so a prose-named case, a typo or a renamed gate reads exactly like a pass. Four results were falsely green on one campaign's first pass |
| restoring the mutated file (found 2026-08-11) | "the tree is back to its original state" | nothing — the restore preserved the file's original modification time, so Ninja saw nothing newer than its output and skipped the rebuild. Every run after the first tested a mutation that had supposedly been reverted |
| three refusal assertions in one new case (found 2026-08-11) | "the budget refuses this" | nothing — the fixture named an instance that already had an active write session, so a unique index refused all three regardless, and the mutation they existed to catch came back green |
| `JournalProvenanceValidator` (found 2026-08-11, **closed 2026-08-11**) | "this document conforms to the fixed `JournalProvenance` schema" — the type's own comment, the call site's `UF_TRY_CONTEXT` string, and both fixtures' refusal messages all say so | nothing — the framework hands the check to the project and never cross-checks it against `schema/umbraflow-journal-v1.schema.json`. Both shipped validators compare bytes to the one literal they ship, and **both literals violate that schema**: `{"kind":"fixture"}` fails the `kind` enum and omits three required members, `{"witness":"suite"}` omits all four and breaks `additionalProperties: false`. A suite run is green either way |
| The record-shape half of `contract-state-s06` and `contract-agent-a04` (found 2026-08-11, **closed 2026-08-11**) | "JR:`ProjectState` and JR:`JournalEvent` are the shapes the store holds" | nothing — both gates asserted the shape by searching the schema *text* for member names, and nothing anywhere compared the schema with the DDL that stores those records. Four of the nineteen column names had drifted from their member names, one of them (`journal_events.canonical_event`) naming bytes that are not an event, and every gate was green throughout |

Note what they are *not*. None is a bug in a check's logic; every one of the
ten works correctly on the inputs it receives. The defect is upstream of the
logic, in what reaches it — an unmatchable filter, a missing root, an
unregistered binary, an un-run case, a stale object file, a fixture nothing can
succeed against — or downstream of it, in a result nothing consumes. That
is why review does not catch them: reading the check tells you nothing, because
the check is fine.

The last three are worth their own emphasis because of *where* they were found.
All three were in the machinery built to run falsifying mutations — the
discipline this document prescribes. **A mutation harness is a gate, and it needs
its own positive control before its results mean anything.** Two of them cost a
full campaign rerun and one produced four false greens that were reported before
they were caught. Three questions settle a harness cheaply: does the run fail
when the case name does not exist, does the binary actually rebuild between the
mutated and restored states, and does the case still pass for the reason it
names when nothing is mutated.

The third has a fix that generalises past mutation work. **A refusal assertion
proves nothing until you show the same call can succeed.** Three refusals in a
row are equally consistent with a correct guard and with a fixture nothing can
succeed against, and the two are indistinguishable from the assertion side. The
repair was a fresh fixture *and* a positive control — the same pin, against a
matching profile, succeeding — which is the negative-result rule applied one
level down: an empty result excludes nothing until the experiment is shown able
to produce a non-empty one.

Two consequences worth carrying:

- **A green result is evidence only when the run states its scope.** "clang-tidy
  passed" means nothing without how many objects it analyzed; "the format check
  passed" means nothing without which roots it walked; "`ctest` passed" means
  nothing without how many of the compiled cases were registered. Any claim of
  the form "X passed" that does not state its denominator is the shape this
  family hides in.
- **Every gate needs its own positive control**, not only every test. Prove the
  filter matches something, the root list reaches the file, the binary is in
  `ctest -N`, the case name is registered. The control is cheap and it is the
  only thing that separates a gate from a claim.

The shape has a documentation form, found 2026-08-11. A review can make a ruling,
be closed, and be archived with that ruling still unexecuted — the queue then
reports done while nothing ever ran. The 2026-07-25 sweep's `core` ruling sat
that way for seventeen days (W12 in
[the next block](../plans/2026-08-10-next-block.md)). `CLAUDE.md` now blocks
archiving until every unexecuted ruling has a live owner named in the file being
archived, which is this family's positive control one level out: the artifact has
to state what it still owes.

### A stored column is a claim until something reads it back

The fifth row is a different costume from the first four and worth stating on its
own, because it is the one that will recur. `authority_decisions` exists to make
a dispatch's authorisation auditable. The row is written under the right
transaction, the column is `NOT NULL`, the value is derived rather than accepted
— and nothing ever reads it. The property "this dispatch was authorised by that
basis" is therefore unfalsifiable: replace the stored hash with any other and
every test still passes, because the code path that would notice reads the basis
from `operation_plans` instead.

**A write-only column is not evidence, whatever its constraints say.** The
CHECKs, the foreign keys and the derivation all guard the value's *shape*; none
of them guards the claim that the value means what the schema says. Two questions
settle it before the column is added: what reads this back, and what turns red
when the stored value is wrong. If the answer to the first is "an auditor,
eventually", the column is unguarded until that auditor exists, and it should say
so at the declaration.

This was found by running a mutation — corrupt the stored value, expect red, get
green — which is the only method that finds it. Reading the code does not: the
write site looks correct because it is correct. It came out of the first full
execution of a block's falsifying mutations, on 2026-08-11, where 23 mutations
across two work items had been specified and never run. Three stayed green;
[W2's specification](../plans/2026-08-10-w2-effective-plan.md) records all three,
and this is the one that generalises beyond its own requirement.

A second instance landed the same day, and the pair is the useful thing.
`dispatches.delivery_reason` is written, and it is guarded harder than most
columns: a three-way `CHECK` ties its nullness to `delivery_outcome`, so no row
can carry a reason without an outcome or an outcome that needs one without a
reason. **Every one of those guards is about the column's shape, and nothing
reads its text back.** Replace the stored reason with any other string of the
right nullness and no test notices. The constraint and the reader are different
questions, and satisfying the first is the most common way to stop asking the
second.

The counter-example is in the same block and shows the discipline discharged
rather than only violated. `ledger_events`' descriptive columns — `kind`,
`controlled_target_id` (spelled `controlled_target_key` until `07abc3e`),
`subject_id` — were written one landing before anything
read them. That was recorded as a known zero at the time, with the reader named
and scheduled, and `subscribe` became it one landing later. A write-only column
is a debt, not a defect, **when the commit that adds it says so and names what
will read it.** The defect is the unrecorded one.

### Delegating a check does not delegate the promise

The ninth row is the first instance found in the *exported* surface, and it is
the one with a live victim: **a real consumer's suite passed while producing
provenance documents the framework's own schema would reject.** Nothing
malfunctioned. The framework calls a validator; the validator refuses documents;
the refusal message names `JournalProvenance`. What no code does is connect any
of that to `schema/umbraflow-journal-v1.schema.json`, where `JournalProvenance`
is actually defined — `additionalProperties: false`, four required members, and
`kind` restricted to five values. `JournalProvenanceValidator` is a
`std::function<Status(std::string_view)>` supplied by the project, and both
shipped implementations are one byte-comparison against the single literal that
fixture also ships. Each literal violates the schema several times over. Each
suite run is green.

**The tell is that the same class already solves the same problem one member
away, and says why.** `ProjectJournalSchemaOwner::create` demands
`exactJournalSchemaManifestBytes` so that, in the header's own words, "the
payload validator provably answers for the manifest this registration named;
without them the recorded `payload_schema_hash` is whatever an arbitrary
validator chose to return." That argument transfers word for word to provenance,
and the provenance half has no pin at all — no bytes, no manifest, and a `Status`
return that leaves no evidence in the record that the check ran against anything
in particular. One constructor, two validators, one of them anchored and one of
them free.

**Verdict: a gap, not a design.** The defensible kernel of the "by design"
reading is that provenance carries project-shaped values — `principal_id`,
`observation_ids` — so a project must participate. But supplying the *values* is
not supplying the *schema decision*. `JournalProvenance` is fixed, it is
framework-owned, and it lives in a framework schema file, so the framework can
validate it without asking anyone. The correction that matches "Break it rather
than bridge it" is to delete `JournalProvenanceValidator` and validate the
document in `journal-entry.cpp` against the schema the comment already names. If
delegation has to survive for a reason not yet stated, then the framework must
feed each project validator a known-bad provenance document at construction and
refuse a validator that accepts it — a negative control, which is the same
discipline this document exists to enforce, applied to a check rather than to a
test.

**The generalisable rule.** When a framework delegates a check to a consumer, it
keeps the promise and gives away the enforcement. That trade is only safe if the
framework can still tell that the delegate did the job — by pinning the bytes the
delegate must answer for, by taking back a value derived from them, or by
probing the delegate with an input it must reject. A callback typed
`Status(std::string_view)` offers none of the three, and a name like "the fixed
`X` schema" on such a callback is the promise with nothing behind it.

Recorded 2026-08-11. The consumer-cost side of the same finding is in
[consumer onboarding](../plans/2026-08-11-consumer-onboarding.md) §6.4, beside
the canonical-validator instance it rhymes with.

> **Closed 2026-08-11.** `JournalProvenanceValidator` is deleted;
> `ProjectJournalSchemaOwner::validate` enforces `$defs.JournalProvenance` in
> the framework. The one thing worth carrying forward is what the repair had to
> prove: `contract-agent-a04` now drives six documents that each violate exactly
> one rule of that schema and one that violates none, because a framework check
> that merely compared bytes against the conforming document would have refused
> all six for the wrong reason and read as a fix. See
> [the journal record binding](../plans/2026-08-11-journal-record-binding.md).

### A schema file nothing compares against is the same shape

The tenth row is the family applied to a *contract document* rather than to a
check. `schema/umbraflow-journal-v1.schema.json` is pinned by
`journal_envelope_schema_hash` and names two records this framework stores as
SQLite rows; `contract-state-s06` and `contract-agent-a04` both assert those
shapes by searching the schema text for member names. Every assertion passed for
as long as the file existed, whatever the DDL said — and the DDL said
`canonical_event` for a column bound to the event's payload and nothing else.

The generalisable form is one question, and it is cheap: **for every schema this
repository ships, name the code that produces or consumes the shape, and the
assertion that compares the two.** Where the answer is "a test greps the schema
file", the shape is documented and unenforced, and the two are indistinguishable
from the gate's name. The repair here was to read the columns back from a
database the Operator created and compare the set with the schema's own
`required` list — which also made the reverse true for the first time: an edit
to the schema file alone can now turn a behavioural gate red.

## Properties no mutation can reach

The same block produced a second, opposite family: properties that are true,
load-bearing, and **unfalsifiable by construction**. Nothing here is a gate that
silently passes, so this is not the family above — but it is what an honest
campaign turns up once the harness is trustworthy, and the failure mode is the
same in the end: a green suite that a reader takes as coverage. Four shapes,
each recognisable on sight.

- **One fact spelled N times.** A dispatch's lease identity is `lease_id`,
  `fencing_token` and `lease.revision`; the revision is always set equal to the
  fencing token and both move with the lease id on every acquire, release and
  takeover. Only the conjunction is falsifiable. A test was written specifically
  to isolate them, using the one schedule where the audit row and the live lease
  disagree, and even that could not separate them. The same shape, worse:
  `requireLiveBinding` compared four fields that had each been copied out of the
  very row it compared against, so each conjunct was masked by the others and
  none was individually falsifiable. **The repair for the second was to delete
  two conjuncts**; the repair for the first was to say so. Tell them apart by
  asking whether the redundancy buys defence in depth against a *different*
  writer, or merely re-reads one writer's own output.
- **An invariant whose two sides move together.** "A budget row is present
  exactly when the controller kind requires one" cannot be turned red, because
  the same authority writes both sides and any mutation moves them in step. It
  is still worth keeping: it converts a silent no-charge into a loud failure.
  Keep it, and name it as unfalsifiable at the declaration, rather than counting
  it as coverage.
- **A property of a declaration, not of behaviour.** `HostDeliveryReport` has
  exactly one friend, and that count is the entire mechanism keeping a test
  harness from fabricating a delivery it never performed. No test can express
  it: adding a second friend compiles, and every case stays green. This was
  confirmed empirically rather than assumed — which is the only honest way to
  claim a zero. Only reading the header catches a change here, so the header
  says why the count matters.
- **A scoping property with one instance in every fixture.** "A takeover
  resolves only its own target's dispatches" has no test at all. Its mutation
  goes red, but for the wrong reason — a bind-parameter range error — and every
  fixture has exactly one controlled target, so the property itself is never
  observed. **A rule of the form "only its own X" needs two Xs in the fixture or
  it is untested**, however many assertions surround it.

What to do with each is the same: record the zero where the requirement is
recorded, so the next reader meets it beside the thing it qualifies, and do not
let a green campaign be reported as if these were part of it. The ones this
repository carries are in
[the next block](../plans/2026-08-10-next-block.md) §2, beside the requirements
they qualify.

Returning to the first family: instance detail follows for the member that cost
the most.

## A PCRE lookahead in `HeaderFilterRegex` silently discards every header diagnostic

### Symptom

The `clang-analysis` CI job passes. clang-tidy reports nothing from any header,
ever, on any check — and clang-tidy's own summary line says so if you read it:

```text
Suppressed 2 warnings (2 in non-user code).
Use -header-filter=.* to display errors from all non-system headers.
```

Nothing errors. The configuration is accepted without complaint, so the job
looks like a working gate.

### Root cause

`.clang-tidy` carried:

```yaml
HeaderFilterRegex: '^(?!.*[\\/]tests[\\/]external[\\/]).*$'
```

`(?!...)` is a PCRE negative lookahead. clang-tidy matches header filters with
`llvm::Regex`, which is POSIX ERE and has no lookahead at all. The pattern
matches no path whatsoever, so every diagnostic whose location is a header is
classified as non-user code and dropped. clang-tidy does not diagnose the
unusable pattern; it just matches nothing.

What that costs here is not marginal. In this repository class definitions live
in headers, so a check like `cppcoreguidelines-pro-type-member-init` reports at
the constructor in the `.hpp` and is discarded every time.
`.claude/skills/cpp-coding/references/coding-standard.md` names that check as
the thing covering the indeterminate-member cases its own Python recognizer
cannot see, and names the `clang-analysis` job as the only enforcement for part
of `## Ownership`. Both statements were true about the intent and false about
the effect: the check had never fired on anything.

### Fix

Write the exclusion in POSIX ERE. `llvm::Regex` supports alternation, character
classes and anchors, and a negated-set construction expresses "does not
contain" without lookahead. Whatever form is chosen, prove it with the
regression check below rather than by reading it.

Treat any regex handed to an LLVM tool as ERE unless that tool documents
otherwise. A PCRE-shaped pattern will usually be accepted and then match
nothing, which is the failure mode with no error message.

### Regression check

Falsify the filter, do not inspect it. In a scratch directory, beside a copy of
the repository's `.clang-tidy`:

```cpp
// probe.hpp
#pragma once
struct Probe
{
    int value;
    Probe() {}
};
```

```bash
clang-tidy probe.cpp -- -std=c++23                      # must report the header
clang-tidy --header-filter='.*' probe.cpp -- -std=c++23 # the positive control
```

The second run is the control: it must report
`cppcoreguidelines-pro-type-member-init` at `probe.hpp`. If the control reports
and the first run does not, the repository's filter is matching nothing. With
the lookahead pattern in place that is exactly what happens — confirmed on
clang-tidy 19.1.5 on Windows and on clang 23.1.0 on Linux, so it is a property
of `llvm::Regex` and not of one toolchain.

Keep the control in the loop whenever the filter changes. A filter is the one
piece of lint configuration whose failure looks identical to success.

### The blast radius is not CI-only

clang-tidy is not a Linux job. `CMakePresets.json` carries an `x64-analysis`
configure preset inheriting `x64-debug` with `CPP_ENABLE_CLANG_TIDY=ON`, and
`cmake/static-analysis.cmake` has an MSVC branch adding `--extra-arg=/EHsc`
precisely so it runs there. So the dead filter suppressed header diagnostics on
every developer's Windows analysis run as well as in CI, for as long as it stood.
When the filter starts matching, the diagnostic count rises on both.

## A `.clang-tidy` key the local clang-tidy does not know deletes the whole file

Found 2026-08-10 by inspection rather than by damage, while checking the fix
above. Same family, one level further out: not a filter that matches nothing,
but a configuration that is not loaded at all.

### Symptom

None. clang-tidy prints `unknown key '<Name>'` and
`Error parsing <path>/.clang-tidy: invalid argument` on stderr, then analyses
the translation unit anyway and **exits 0**. Under
`CMAKE_CXX_CLANG_TIDY` the build proceeds, so the lane is green.

### Root cause

clang-tidy parses `.clang-tidy` with LLVM's YAML I/O, which treats an
unrecognised key as an error for the whole document. It does not skip the key
and keep the rest: the file is discarded and clang-tidy falls back to its
built-in defaults — a different check set, and no `WarningsAsErrors`. Every
check the repository selected, and the strictness that makes them fail a build,
are gone.

This repository is exposed through one key. `ExcludeHeaderFilterRegex` needs
clang-tidy 19 or newer. CI pins clang-tidy 23, but the `x64-analysis` preset
uses whatever is on `PATH`; on this machine that is the 19.1.5 shipped inside
Visual Studio 2022, which supports the key. An older one on any host silently
turns that host's analysis run into a pass over nothing.

Note the direction, because guessing gets it backwards: an unsupported key does
not *widen* what is analysed, it removes the configuration.

### Regression check

Measured with clang-tidy 19.1.5 on Windows, in a scratch directory, over the
`probe.hpp`/`probe.cpp` pair above:

```bash
# A: repository .clang-tidy with one bogus key added
clang-tidy probe.cpp -- -std=c++23   # "unknown key"; 0 member-init reports; exit 0
# B: the same file with the bogus key removed
clang-tidy probe.cpp -- -std=c++23   # 1 member-init report; "warning treated as error"; exit 1
```

B is the positive control: without it, A's silence is indistinguishable from
clean code. Run this pair whenever a key is added to `.clang-tidy`, and check
`clang-tidy --help` for the key's presence on the oldest clang-tidy any lane may
resolve.

### Related: a clang-tidy claim cannot be checked by the local gate

`scripts/ci-local.*` configures the host debug preset, and clang-tidy runs only
under `CPP_ENABLE_CLANG_TIDY` — the three `*-analysis` presets and the
`clang-analysis` CI job. A commit message that says a suppression or a check
"turns the gate red" is talking about a lane no local gate exercises, and which
does not compile today (W11 in `../plans/2026-08-10-next-block.md`). State which
lane, and whether it built.
