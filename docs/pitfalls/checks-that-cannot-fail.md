# Checks that cannot fail

A test that stays green after the property it names is removed proves nothing.
The same holds one level up, of the gates themselves, and one level up again, of
the harness that runs the mutations. This repository has found the failure in all
three places, and by 2026-08-11 it had found enough instances that listing them
stopped being the useful thing.

**A name exists, the name promises something, and nothing verifies the promise.**
That is the family. What follows is the shapes it takes, an instance behind each,
and — the part a reader who has just written a test actually owes — which of them
that test is exposed to and what would detect it.

Note what none of these are. Not one is a bug in a check's logic; every one works
correctly on the inputs it receives. The defect is upstream of the logic, in what
reaches it, or downstream, in what consumes the result. That is why review does
not catch them: reading the check tells you nothing, because the check is fine.

## What detects what

Mutation is the reflex and it finds fewer than half of these. Match the form to
the method before spending a campaign.

| Form | What finds it |
|---|---|
| Two sides of one computation | Reading, not mutation — ask where each side came from. A mutation returns green and reads as coverage |
| A stand-in for a validator nobody wrote | Ask what parses a byte, then an input the stand-in accepts and a real validator refuses |
| A second mechanism refuses first | A negative control: with the check removed, the same input must be **accepted** |
| An assertion another refusal already satisfies | Reading the failure text, never the colour or the assertion count |
| A call inside a negative assertion | Mutating the callee's behaviour, never grepping the call site; the control is a case where the same callee must **succeed** |
| A branch nothing reaches | A positive control — the identical mutation in the sibling branch must be red |
| A value nothing reads back | Mutation, and only mutation: corrupt the stored value and expect red |
| A refusal the caller's channel cannot express | A positive control on the *composer*: force the refusal and require the next step not to have run |
| A selector that selected nothing | A denominator — `ctest -N`, the file list, the object count |
| A pattern the engine reads differently | A probe run through the engine itself, with a known-positive control beside it |
| A fixture value that disarms the check it arms | A rule that refuses the disarming value, in the loader rather than in a document |
| A mutation that never reached the binary | Confirming the compile step ran, and observing green between mutations |
| A promise that lives only in prose | Grep the pin outside `docs/`; then a gate whose absent-input path is loud |
| Two spellings, each tested against itself | Shared vectors both answer, with unanswerable rows refused rather than skipped |
| A check delegated to a consumer | Pin the bytes the delegate answers for, or probe it with an input it must reject |
| A schema nothing compares the code against | Name the producer and the consumer of the shape, and the assertion holding them together |
| A ruling archived unexecuted | The archived artifact must state what it still owes and who owns it |

## The falsification rule this repository states is not enough

The rule on the books is that a test counts only once removing the property makes
it red. The record falsifies it. A mutation's red can land at a different
assertion, in a different mechanism, or in the build system, and the colour is
identical in every case.

**A mutation counts only when the red lands at the assertion that names the
property, and the failure text has been read.** Three reds on this branch would
have been counted as passes on the weaker rule:

`a07`'s "a takeover resolves only its own target's dispatches" has a mutation
that goes red on a bind-parameter range error, not on the scoping rule — and
every fixture holds exactly one controlled target, so the property is never
observed at all
([the next block](../plans/2026-08-10-next-block.md) §2, the `a07` scoping
bullet).

A harness that rewrote sources with Python's `write_text` translated a mirrored
document to CRLF, and a case searching that document for `"\n"`-delimited text
went red on an assertion the mutation had nothing to do with (`5990cb8`;
[concurrent agent builds](concurrent-agent-builds.md), the robocopy section).

`member-toolname` and `risk-lookup` in `b9ef6e7` both abort after exactly 22
assertions. Colour and count are identical; only the contract message separates
them — `p_member != nullptr` from `risk != k_risks.end()`. Two mutations were
distinguishable by nothing but the text.

Two more were caught before they were counted, which is what the rule buys. The
first draft of `eb238cb`'s cases asserted only that a refusal contained
`base_resolution`; a zero-extent model is also refused by the rectangle bounds
check, whose message reads "must stay inside base_resolution", so the mutation
removing the zero refusal would have gone green while the case went red anyway.
And in `df5a73d`, deleting the loop that holds a Tool Catalog to what its
deployment carries left the directory still refused by the opposite direction, so
`REQUIRE_FALSE` stayed satisfied and **only the assertions on the refusal's
message went red**.

The corollary is an assertion rule, not a process rule. Assert a phrase that is
unique in the tree, never an error kind: several distinct refusals share
`AutomationErrorKind::InvalidResource`, so a kind-only assertion passes on a
refusal from somewhere else entirely (`4ae4fcf`, and
`conformance/source/suite-project-authority.cpp`'s uiAction case, where
`deployment::readStepIntent` refuses a malformed document with the same kind).

## Two sides of one computation

The comparison is real, the values are real, and both were produced by the same
call. `hashOf(x) == hashOf(x)` keeps the appearance of an attestation and
attests nothing.

The instance that named the form is the consumer's: `contract/provider.cpp`
assembles the registration document at run time from the digests of the files it
pins, and `ProjectSchemaOwner::create` then compares those digests against the
bytes handed in, so both sides descend from one `hashOf`
([project as data](../plans/2026-08-11-project-as-data.md) §0.1, lines 127-132).
The Q3 ruling did not remove it — it **adopted** it. The loader now computes
every digest, so every comparison inside a single load is between two quantities
the loader itself produced, deliberately (§7.0, lines 1595-1600). The framework
now wears the costume knowingly instead of a consumer wearing it by accident.

What makes that defensible is that exactly one comparison on the chain survives
with two independently produced sides, and it is named at the site:
`modules/deployment/source/deployment/project-directory.cpp:1326-1333` is a
stored session's recorded `project_registration_hash` against a directory whose
bytes have moved, and it prints both. It was proven red rather than reasoned to
be — `tests/deployment/test-project-directory.cpp`, "a project whose bytes moved
under a stored session is refused", where removing the comparison in
`commitmentFor` leaves the case green (`6449b46`,
[TODO](../TODO.md), "The registration chain must be proven to have teeth").
The flipped byte is in the plugin, whose bytes
reach the hash through `plugin_hash` and nothing else, and the case reloads the
flipped directory a third time with no commitment and requires that load to
**succeed** — which is the negative control that stops a second mechanism from
standing in.

**A digest written beside the path of the file it describes is a second spelling
of one thing.** That is Q3's reason and it now holds throughout the project
directory format (§7.0, lines 1611-1621). Where it does not hold, coverage
disappears instead: `df5a73d` measured an effect payload schema whose bytes
reached no digest anywhere — flipping one byte of `effect-run-v1.schema.json` on
the real uf-chaos directory left every hash byte-identical and the load accepted.
It is now a required `effect_payload_sha256s` in the Tool Catalog, held to the
deployment's list in both directions.

**Detection is reading, not mutation.** Ask of every comparison where each side
came from. A mutation here returns green and is easily filed as coverage.

## A stand-in for a validator nobody wrote

A seam exists, its comment states an obligation, and what is plugged into it
recognises literals.

`ProjectSchemaOwner::create` takes a canonical validator and a document validator
under a comment beginning "These validators are trusted deployment code", and
neither had ever existed. The umbraflow exemplar filled the hole with a
fifteen-entry allowlist of literal byte strings and arcana with a seventeen-entry
one, plus a `starts_with`/`==` switch for documents, and
`ProjectDocumentSchemaBytes` that were the three words `"state"`,
`"observation"`, `"precondition"`. Neither parsed a byte, so a shipped validator
that accepted non-canonical JCS would have left both conformance runs green
([the inventory](../plans/2026-08-11-project-as-data-inventory.md) §C.0).

**Closed by `8277fe3`**, which built `modules/deployment` on `modules/json` and
deleted 1,040 lines that were almost entirely those constants. The proof is the
right shape: for each exemplar there is one input the old constant accepted and
the new validator refuses, asserted in both directions in the same test —
`{"canonical_args":0,…,"a":0}` passes `oldPlanEnvelopeRecognizer` and fails the
canonical validator because `"a"` sorts before every other member, and the old
document switch accepted `{"disposition":"forged"}` on a `starts_with`.

The same form one level down is in the evaluator itself. An implementation that
reads `"required": "id"` as an empty name list, or `"minLength": "3"` as the
bound zero, has a check that cannot fail; `modules/json/source/json/schema.hpp:27-31`
and `schema.cpp:86-89` say so and shape-check every keyword's value before any
instance is judged, with 26 cases behind it. `a0ae304` also dropped 11 keywords
the seed implemented and no schema in either tree uses, on the same reasoning: a
keyword nothing exercises is a check that cannot fail. Two survivors are recorded
rather than fixed — `format` is annotation-only under the 2020-12 default
vocabulary, so 11 `"format": "date-time"` uses constrain nothing, and
`umbraflow-trace-v2.schema.json` does not compile at all because four numeric
bounds exceed 2^53.

**Detection is asking what parses a byte**, then producing an input the stand-in
accepts and the real thing refuses. Counting checks does not do it; the
seventeen-entry allowlist looks like more checking than a schema.

## A second mechanism refuses first

The check can fail. It cannot fail on the input the test hands it, because
something else refuses that input earlier. Delete the check and the case stays
red — or, worse, stays green with the refusal coming from somewhere else — and
the property is unguarded either way.

Three of `8277fe3`'s mutations stayed green in exactly this shape, and isolating
cases were written for all three: a plan envelope whose only fault is a stray
member, a substitute precondition schema carrying the same definition under
different bytes so that only the catalog's sha256 separates them, and the `$id`
defect placed on the reconcile schema, which is named by hash and never by
`$ref`. Re-run: 15 of 15 red.

W2's version was a fixture rather than a rule. Three refusal assertions in one
new case named an instance that already had an active write session, so a unique
index refused all three regardless and the mutation they existed to catch came
back green. The repair was a fresh fixture **and** a positive control — the same
pin, against a matching profile, succeeding.

The form also lives inside production code. `a0ae304`'s leading-zero rule was
unreachable because the lone-`0` branch consumed the digit first, so
`refuses("01")` had been passing on the trailing-content rule; and an
`std::isfinite` guard sat dead behind a second range check. Both were fixed in
the code rather than explained.

**A refusal assertion proves nothing until you show the same call can succeed.**
Three refusals in a row are equally consistent with a correct guard and with a
fixture nothing can succeed against, and the two are indistinguishable from the
assertion side. `5990cb8` made this the standard for a whole landing: five loads
that used to succeed are now refusals, measured verbatim on the real uf-chaos
corpus, and **every "check deleted" run has the load succeeding**, which is what
proves no second mechanism was doing the refusing.

**Adding a gate upstream disarms every fixture that is invalid to it, and the
disarming is silent.** 2026-08-12 moved `artifact.read` from bytes to a decoded
JSON value, which put a parse in front of every artifact case. "artifact reads
remain inside the fresh VM memory quota" registered four 4 MiB blobs of one
repeated byte and required registration to fail; it still failed — at
`json::parse` — and stayed **green while never reaching the quota it is named
for**. Two sibling cases carried equally invalid bytes and went red instead,
which is the only reason the third was looked at. The rule is cheap to apply and
it is the one worth carrying: **a fixture whose bytes must reach a late check
has to be valid input to every earlier one.** That case now registers 400,000
empty JSON arrays, which parse, sit inside every byte ceiling, and exhaust the
VM allocator; and each artifact ceiling in that file was measured by removing it
in a mirror and observing the same registration succeed — including the two
ceilings that are spelled twice, in `script` and in `operator`, where removing
one spelling leaves the other refusing and proves nothing.

### The second mechanism can be the operating system, so a positive control has a platform

Recorded 2026-08-12 from the 2026-08-10 runtime-hardening review, which is the
only place it had been written down. **A positive control is a claim about one
platform until it has been run on the others**, because the earlier refusal that
disarms it may belong to the kernel rather than to the code.

The capability-swap case swaps a capability file under its open descriptor and
requires the refusal to hold. On POSIX the identity re-check in
`_descriptor_document` is what refuses. On Windows the open handle refuses the
replacement outright, so that comparison never runs and **the case passes with
it deleted** — the mutation is green on the developer's own host and red only in
a lane nobody runs locally. The case is written to name both mechanisms rather
than to pass on whichever holds, which is the fix; the residue is that its
falsification is POSIX-only and says so.

The same shape has a second instance one review later: `requireChildName`'s `.`
and `..` clauses cannot be falsified on Windows, because the platform refuses
those components before the code sees them.

**What to do.** State the platform beside every positive control, in the test
and not only in a review. A control with no platform named is read as universal,
and on a Windows-only local gate — which is what `scripts/ci-local.ps1` is — that
reading is wrong for every check whose first refusal is a file-system rule.

## An assertion another refusal already satisfies

A weaker relative of the last, and it hides better, because the case does contain
an assertion about the message.

`tests/deployment/test-project-directory.cpp:516-525` carries the measurement in
its own comment. R1 requires both root documents at fixed names; reading an
absent document as empty bytes also fails the load, on "is not JSON", naming the
document it was looking for — so a case that asked only whether the load failed
is satisfied by the "absent means empty" reading the rule exists to forbid. That
mutation left an earlier version of the case green across all 39 of its
assertions. It now asserts the directory and both fixed names.

`6449b46` records the same defect one file over: a refusal assertion on the
substring `umbraflow-conformance.json` was satisfied by a second mechanism's
message. And `b6c7e72` records a pair that cannot be separated at all —
`payload_schema_hash`'s pattern and `ContentHash::parse` refuse exactly the same
inputs, so neither is independently falsifiable through the reader; the combined
mutation is red and the test says it is not claiming the pattern.

**Detection is reading the red, and the assertion rule above.** A substring is a
claim about the whole tree's refusal vocabulary, so it has to be checked against
the whole tree's refusal vocabulary.

## A call inside a negative assertion

The last two are about the check. This one is about the thing the check calls,
and it is more dangerous than a path nothing reaches at all, because a reader who
greps for the call site finds one and concludes the subject is covered.

**An invocation inside a negative assertion provides no coverage of the thing
invoked: the failure mode and the asserted outcome are the same value.** Break
the callee and it produces a refusal; the case asserts a refusal; the case
passes.

The instance is the exported Operator conformance suite, and it is the widest one
recorded here because the suite is what a consuming repository runs to certify
its own project. The suite drives the deployment in the `UnderTest` role and
nothing else: `modules/conformance/source/conformance/suite-support.cpp:242-279`
loads that deployment's plugin, registers it, provisions the instance, pins the
session and takes the lease, all under one registration. Exactly one case reaches
the FOREIGN deployment's plugin —
`suite-control-ledger.cpp:462-478`, "the reconciler owns the disposition, not the
requester" — where it calls `loadPlugin(prepared.project, ProjectRole::Foreign)`
and mints an outcome through `reconcileOutcome(..., ProjectRole::Foreign, ...)`.

That call sits inside
`CHECK_FALSE(prepared.store.commitReconciliation(prepared.plugin, foreignCommit).has_value())`.
The case asserts that an outcome minted against another registration is refused,
which is the right rule and a real one — and it is satisfied identically by a
foreign plugin that cannot answer at all.

Measured 2026-08-12 against `examples/umbraflow`, one mutation at a time, the
suite observed at 16 of 16 and 1,303 assertions before each:

- foreign `reconcile` rewritten to answer `{"disposition":"rejected"}` whatever
  it is asked — **16 of 16, 1,303 of 1,303**. This is the one entry point the
  case actually calls, and its answer is unmeasured.
- foreign `next_step` rewritten to name a `ui_target_id` no model declares —
  **16 of 16, 1,303 of 1,303**. Together with `derive`, `plan` and `reduce` it is
  never called at all, which is the ordinary uninvoked-path form.
- foreign `plugin_id` changed — **15 of 16**, and this is the sharp part: the red
  lands at `suite-support.cpp:197`, `REQUIRE(result.has_value())` inside
  `loadPlugin`, because the registrar refuses to load a module whose id is not
  the one the registration names. It never lands at the case's own `CHECK_FALSE`.
  So the only mutation that moves the number moves it in a different mechanism,
  which under the stronger rule above is not coverage of this case's property
  either.

**What the suite's green therefore means.** It is an under-test-deployment
conformance suite with a foreign authority foil, and reporting its green as
whole-project or all-plugin conformance is broader than what it measures. The
foreign deployment is measured as bytes and identity — its registration hash, its
plugin id, its schemas — and not as behaviour. A consumer that wants its second
deployment's behaviour measured swaps the two roles in that project's
`umbraflow-conformance.json` and runs the suite again. That is where a consumer
should look, and it is how the second plugin was actually proven.

One correction belongs here rather than where it was written. `uf-chaos` commit
`012b15e` states the stronger claim that the suite never invokes the foreign
plugin at all. That is false — it is invoked, in the case above — and the true
statement is the weaker and more useful one: it is invoked and the invocation
covers nothing.

**Detection is mutating the callee, never grepping the call site.** A refusal
assertion says nothing about what produced the refusal, so the control is a case
in which the same callee must **succeed** — the negative control of
`## A second mechanism refuses first`, applied to the invoked component instead
of to the input.

## A branch nothing reaches

The code is correct, the case is green, and no input in the estate takes that
edge.

`readStepIntent`'s wait branch
(`modules/deployment/source/deployment/project-deployment.cpp:1374-1381`) is
unreachable because no plugin in the estate answers `next_step` with a
`WaitIntent`; `b9ef6e7` records the positive control that proves the reading —
the identical mutation in the UI-action branch is red. `StepKind::Wait` being
unexercised also leaves the ledger's `kind() == StepKind::UiAction` guard
untested on that side
([the next block](../plans/2026-08-10-next-block.md) §2, the paragraph on
W2's three stored-and-unenforced ceilings).

The build-system instance is sharper because the mirror mutation is red.
Deleting `[sources.other]` from `modules/cli/manifest.txt` puts both unsupported
translation units back into the compiled set, and the Windows build stays green
and links: a static library contributes only the members needed to resolve a
symbol, so the duplicate definitions sit in members nothing pulls, and which one
wins is archive search order rather than anything the manifest states.
Reassigning `targets-windows.cpp` to `[sources.linux]` **is** red —
`unresolved external symbol uf::cli::targetsProduct`. So the removal half of the
grammar is enforced and the restoration half is not, on any platform that has a
section of its own. Ruled 2026-08-11 ([TODO](../TODO.md), "Build-system shape"): the
property is "this translation unit compiles only on that platform", the honest
verification is compiling it there, so the Linux and macOS jobs are the gate —
and those jobs are blocked by the repository's CI billing state, so the gate
exists and has not run.

**Detection is the sibling positive control.** A branch's mutation going green
means either the branch is dead or the mutation was ineffective, and the
identical mutation next door tells you which.

## A value nothing reads back

The row is written under the right transaction, the column is `NOT NULL`, the
value is derived rather than accepted — and nothing ever reads it. The property
"this dispatch was authorised by that basis" is unfalsifiable: replace the stored
hash with any other and every test still passes, because the code path that would
notice reads the basis from `operation_plans` instead.

`authority_decisions.decision_basis_hash` is that instance
(`modules/operator/source/operator/ledger.cpp:821-830`; W2's T13, recorded in
[the next block](../plans/2026-08-10-next-block.md) §2, the T13 bullet).
`dispatches.delivery_reason` is the pair that makes the point:
`ledger.cpp:846-865` guards it harder than most columns, with a three-way `CHECK`
tying its nullness to `delivery_outcome`, so no row can carry a reason without an
outcome or an outcome that needs one without a reason. **Every one of those
guards is about the column's shape, and nothing reads its text back.** The
constraint and the reader are different questions, and satisfying the first is
the most common way to stop asking the second.

**A write-only column is not evidence, whatever its constraints say.** Two
questions settle it before the column is added: what reads this back, and what
turns red when the stored value is wrong. If the answer to the first is "an
auditor, eventually", the column is unguarded until that auditor exists, and it
should say so at the declaration. Neither of these two says so today.

The counter-example is in the same block and shows the discipline discharged.
`ledger_events`' descriptive columns — `kind`, `controlled_target_id` (spelled
`controlled_target_key` until `07abc3e`), `subject_id` — were written one landing
before anything read them, recorded as a known zero at the time with the reader
named and scheduled, and `subscribe` became that reader one landing later.
**A write-only column is a debt, not a defect, when the commit that adds it says
so and names what will read it.**

**Detection is mutation and nothing else.** Reading the code does not find it:
the write site looks correct because it is correct.

## A refusal the caller's channel cannot express

The nearest relative of the last, one level out. The value *is* read back — by a
person, in the payload — and the thing that decides what happens next reads a
different channel, one that has no way to say "refused". The check is sound; its
verdict is advisory. This sits in this family rather than in an entry of its own
because it is the family's downstream half exactly — nothing is wrong upstream of
the logic or inside it, only in what consumes the result — and the question it
forces applies to every gate this repository composes with `&&`.

The exploration driver is the instance. It queues one Luau chunk, waits for the
answer, and exits `0 if answer.get("ok") else 1`. **`ok` means the chunk ran.** A
chunk's guard — the read that proves the screen in front of it is the screen it
was written for — refuses by *returning a string*:
`GUARD REFUSED: wanted '一縷光芒', read '縷光芒' (9375 bp) — no press sent`. That
string is `answer["value"]`, and `ok` is `true` beside it. One night's session
recorded eleven refusals and all eleven exited 0.

So `send.py pick-016 … && send.py pick-017 …` runs the second chunk on a screen
the first deliberately refused to touch, and the `&&` reads as a precondition
while expressing nothing. It cost a card pick on 2026-08-12: a guard on a card
title read `縷光芒` for `一縷光芒` — the leading character clipped — and refused
with no press sent; the chained follow-up pressed a card slot and confirmed it,
taking a card nobody chose.

**The fix is to make the refusal an exit status.** The guard's verdict has to
reach the channel the composer reads, which means the chunk protocol needs a
refusal outcome distinct from "ran" and the driver has to exit non-zero on it.
**The workaround is to never chain acting chunks** — one invocation per command,
read the payload, then decide. That is what stopped the bleeding, and it is not
the repair: it holds only for as long as every caller remembers, and the shape of
the mistake is one keystroke.

**Detection is a positive control on the composer, not on the check.** Force the
check to refuse and require that the downstream step did not run. Reading the
guard finds nothing, because the guard is correct; mutating the guard finds
nothing either, because the refusal a mutation produces is the same refusal that
already fails to propagate.

**The general question is cheap and worth asking of every refusal.** A check has
two audiences — a reader and a composer — and they consume different channels:
prose, a return value, a log line, an exit status. Name the channel the refusal
travels on and the channel the next step consumes; where they differ, the refusal
governs nobody. `&&`, `set -e`, CI step conditions and `ctest` all consume exit
status and nothing else.

## A selector that selected nothing

The check runs. It runs over an empty set, and an empty set passes.

Seven instances, all found between 2026-08-10 and 2026-08-12 and all now closed.
`HeaderFilterRegex` in `.clang-tidy` matched no path at all, so every header
diagnostic was dropped as non-user code (detailed below). `SOURCE_ROOTS` in
`scripts/check_cpp_format.py:24` and `scripts/check_safety.py:18` omitted
`conformance/`, leaving an entire exported module unscanned. `test-annotate-backend`
existed and ran in no CTest. `cpp_add_contract_suite` built its binary `NO_CTEST`
and registered one CTest per `CASES` entry, so seven compiled cases in
`tests/operator/test-project-plugin-contract.cpp` — 416 assertions — executed in
no gate at all; `dcc43b5` gave the helper an aggregate, and
`tests/CMakeLists.txt:336-341` now carries the reason. `scripts/check_modules.py`
globbed `modules/*/manifest.txt` and printed a correct `OK (11 modules)` while
`conformance/` — first-party C++ on every consumer's include path — was outside
the graph; `DECLARED_SOURCE_TREES` at `check_modules.py:20-28` closes it and says
in as many words that **`tests/support/` is still in that position**, because it
declares no manifest yet.

The sixth reports a number rather than a pass, which is why it is worth its own
sentence. `LABELS "CI;CONTRACT-SUITE"` on the four aggregates made
`ctest -L CONTRACT` select 44 where 40 is meant, because `-L` is a regex and
`CONTRACT` prefixes `CONTRACT-SUITE` — and a measurement had already been taken
against the wrong number. The label is `CONFORMANCE` now, and
`tests/CMakeLists.txt:343-346` and `cmake/conformance-run.cmake:165-168` both
say why. **An aggregate label must share no substring with a per-case label.**

`319bdb1` found the same shape in a diagnostic rather than a selector:
`cpp_parse_manifest` promoted an unknown `[module].type` from `message(WARNING)`
to `FATAL_ERROR`, because that warning never fired — which is exactly how
`type = sources` sat unread in `conformance/manifest.txt`.

The instance that cost the most was in the harness. **`ctest -R` exits 0 when the
filter matches no test**, so a prose-named case, a typo or a renamed gate reads
exactly like a pass; four results were falsely green on one campaign's first
pass. A mutation harness is a gate and needs its own positive control before its
results mean anything.

**doctest's `--test-case` has the same hole, and a comma is enough to open it.**
`--test-case=` takes a comma-separated filter list, so a prose case name
containing a comma — "an artifact is handed over decoded, frozen, and once per
VM" — is read as three patterns, none of which matches. The run prints
`0 passed | 0 failed | 50 skipped` and `Status: SUCCESS!`, and exits 0. Three
mutations of 2026-08-12 were reported green that way and had to be re-measured
against a comma-free filter. **Read the denominator line, not the status line**:
`0 passed` is the tell, and it is one line above the word SUCCESS.

The seventh is a regex narrower than the set it was meant to cover.
`SCHEMA_AUTHORITIES` in `tests/test-runtime-surface.py` held
`schema/umbraflow-runtime-v2.schema.json` to its C++ pin, and its pattern
required `std::string_view{"..."}` — so `model.schema_hash` in
`modules/task/runtime/model.luau`, which pins the same digest for the trusted
parser, was outside the scanned set entirely. A stale value there refuses every
artifact at activation, in a lane no local gate exercises, while
`check-repository-surface` prints PASS. Closed 2026-08-12: the table carries the
Luau file and the reader picks the pattern from the suffix. The proof needed the
negative control, because the same flipped byte reads identically either way —
with the row present the check prints
`model.schema_hash does not match exact schema/umbraflow-runtime-v2.schema.json bytes`,
and with the row removed the *same* flipped pin prints `repository surface: PASS`.

The eighth is in that same file and is the same shape one level up: a rule whose
name covers a set and whose reader covers one member of it.
`FORBIDDEN_BUSINESS_GLOBALS` named 23 privileged and direct-action spellings, and
`business_global_errors` inspected the single first-party definition of
`frameworkProjectGlobals()` and nothing else. That function returns `{}`, so the
rule was satisfied by an empty body while **four other lists published project
globals entirely unread** — `explorationProjectGlobals()`,
`runtimeProjectGlobals()`, `scriptProjectGlobals()` and `k_projectStandardGlobals`
in `modules/script/source/script/ffi/environment.cpp` — as was anything a boot
site wrote inline at `.projectGlobals` or `.frameworkProjectGlobals`. Promoting
`key` from a method of the cycle view `explore` hands out to a published global in
any of them was green. Closed 2026-08-12: `published_global_errors` reads all five
lists from their own definitions, holds each to the forbidden set minus the names
that one list may publish, refuses a list whose entries it cannot resolve, refuses
an allowance the source has stopped exercising, and fails when no boot site
assigns the member at all. Measured one mutation at a time against a mirrored
tree: **8 of 8 red at the assertion naming the rule, with the superseded checker
green on 7 of the 8** — the negative control that says the widening is what
catches them rather than something already in the file. Two zeros are carried
rather than closed, and both are named at the declaration: the rule reads a
binding and never the members of the table bound to it, which is why `key`,
`drag`, `scroll`, `long_press` and `move_pointer` are in the forbidden set and
legal as cycle-view methods at the same time; and the boot-site denominator fires
only when the member vanishes from every site, so one VM of several dropping its
publication is measured green.

**Detection is a denominator.** "clang-tidy passed" means nothing without how
many objects it analyzed; "the format check passed" means nothing without which
roots it walked; "`ctest` passed" means nothing without how many compiled cases
were registered. `scripts/check_spec_bundle.py:248-254` is the shape to copy: a
pass that would otherwise be silent about a skipped pin fails on
`internal: N of M pinned files were hashed`. Any claim of the form "X passed"
that does not state its denominator is where this family hides.

## A pattern the engine reads differently than the author

Two regex instances, opposite directions, one lesson. `HeaderFilterRegex` used a
PCRE negative lookahead where clang-tidy matches with `llvm::Regex`, which is
POSIX ERE and has no lookahead: the pattern matched nothing and reported nothing,
with no diagnostic (below). `a0ae304` found the mirror in MSVC's `std::regex`,
which applies multiline anchor semantics whatever `syntax_option_type` it is
given, so `^[0-9a-f]{4}$` matches `"zzzz\n0123"` — a false **accept** on exactly
the patterns that pin content hashes and identifiers, 96 pattern uses across the
two corpora, and present in the consumer's evaluator too. The fix leaves the
engine no anchor to misread: an unanchored pattern is searched as written, a
fully anchored one has both anchors removed and must match the whole subject, and
any other anchoring is refused at schema compile.

**Detection is a probe run through the engine, never a reading of the pattern.**
Treat any regex handed to another tool as that tool's dialect until its
documentation says otherwise; a foreign-dialect pattern is usually accepted and
then matches nothing, which is the failure mode with no error message.

The heuristic version of this has a documented refusal.
`5f6c1a1` considered a regex over the eight prose statements of the spec-bundle
root hash and rejected it: the eight use six different phrasings, so any pattern
covering them fails by matching nothing — the same shape as the dead header
filter. Seven of the eight remain unverified, and the durable fix is for them to
link the authority rather than restate the digest.

## A fixture value that disarms the check it exists to arm

The newest form, and the one this branch was most pleased to catch, because it
appeared inside the format built to end the family.

`umbraflow-conformance.json` declares an `absent_tool`: a tool name the
deployment's Tool Catalog must **not** carry, whose entire purpose is to make the
catalog's refusal of an unknown tool falsifiable. A directory that fills that
member with a name the catalog does carry leaves the case passing with nothing
red anywhere. The loader now refuses it, and the refusal says so in its own text
rather than delegating that to a document
(`modules/deployment/source/deployment/project-directory.cpp:1026-1037`); the
case is at `tests/deployment/test-project-directory.cpp:861-868`, and its
preamble at `:804-811` states the general rule for all five provisioned tool
names. Every substitution in that case names a tool of the project's own catalog
or no tool at all, so nothing but the agreement itself can refuse it — the
negative control, applied per row.

**Detection is a rule in the loader, not a sentence in a specification.** Any
fixture parameter whose job is to be absent, invalid, or unmatched is a
parameter a project can quietly fill with a valid value; the code that reads it
has to check that it still does its job.

## A mutation that never reached the binary

The measurement is what failed. The check under test is fine and the result about
it is fiction.

`robocopy /MIR` preserves the source file's timestamp, so restoring a mutated
file replaces a file whose mtime is *now* with one older than the object built
from the mutation, and ninja rebuilds only when an input is newer than its
output. The mutated object stays, and the check the mutation deleted stays
deleted, for every run until something else touches that source. Measured
2026-08-11: **one deleted check survived four later runs, and four results had to
be discarded and re-measured** (`5990cb8`; the mechanism and the fix are in
[concurrent agent builds](concurrent-agent-builds.md)). Reading and writing
mutated files as bytes closes the CRLF half of the same hazard.

`5f6c1a1` records a third variant, from a gate's own falsification: the first
byte-flip attempt died on a console encoding error *before writing*, and the
check dutifully reported a pass. Every mutation there now asserts it landed
before the check runs.

**Detection is watching the build.** Confirm the compile step actually
recompiled the mutated file — a build that reports only the link step has
restored nothing — and require the suite to be observed green between every pair
of mutations. `df5a73d` states that discipline as part of its result: six
mutations, zero green, source touched and the suite observed green between every
pair.

## A promise that lives only in prose

The four-document consumer bundle is the normative input this whole framework
defers to, and its authority document says that if any byte differs,
implementation stops. A cross-repository audit found the root hash
`c4760bb5…` stated in eight prose documents under `docs/` and in **zero**
scripts, CMake files or tests
([cross-repository drift](../plans/2026-08-11-cross-repository-drift.md), the
"Nothing checks the pin" finding). A byte could have changed and the only
consequence would have been that every later reader believed something false.

**Closed by `5f6c1a1`**, and the closure is the template for this whole document.
`scripts/check_spec_bundle.py` holds the digests in two independent copies — the
script's own and the ones stated in the authority document — because a gate that
read its expectation out of the document it checks would agree with any edit to
that document (`:67-70`). Its self-test builds a synthetic bundle and
demonstrates the verifier going red on six controls on **every** invocation
before the run is allowed to say anything about the real bundle (`:316-362`). And
because the bundle lives in another repository at an absolute path on one
machine, absence is exit 2 rather than a quiet pass: no invocation can report
success without either hashing five files or saying `NOT VERIFIED` in its success
line and naming the path it did not read. The gate is called `check-spec-pins`
rather than `check-spec-bundle`, because it verifies that this repository agrees
with itself about the pin and does not verify the bundle in CI — naming it after
the bundle would have been another instance rather than the fix for one.

**Detection is grepping the pin outside `docs/`.** A number that appears only in
prose is enforced by a human remembering.

**2026-08-12: the two independent copies are also two things to forget.** The
bundle moved through v1.10, v1.11 and v1.12 in one day. Both of the gate's copies
were left at v1.10, so they still agree with each other and `check-spec-pins`
stays green while asserting a bundle two versions old — the gate cannot detect
its own staleness, only a disagreement between its halves, and a full run against
the real directory is what reports it. Duplication for independence buys
detection of an edited pin at the price of a pin nobody edits.

**Re-pinned to v1.12 later on 2026-08-12**, and the instance above is kept
because the mechanism did not change: both copies now read
`b3306dde9337a70e5e33bb5676f9da5b0e99b4b1acd2fec1ef4d16dbde51cda5` and agree,
which is exactly the state that hid two versions of staleness before. The
detection is still "grep the pin outside `docs/`, then run the gate against the
real directory"; nothing in the tree turns red when the bundle moves again.

## Two spellings, each tested against itself

Where one rule is implemented twice, each implementation's own tests pass and the
divergence is invisible, because a divergence does not fail a test — it produces
bytes that merely disagree. `modules/core/source/core/text/json-text.hpp:15-19`
gives that as the reason those two functions live in `core` at all, and
`tests/vectors/jcs-vectors.txt:1-8` names the three spellings of RFC 8785 in this
repository (C++, `tools/annotate/jcs.py`, `modules/task/runtime/jcs.luau`) and
exists to hold all three to one set of expectations.

The vector file has the family's own trap built into it and closed. Rows may
carry a `cpp=absent` override, which would be a quiet skip; `a0ae304` answered
all 47 rows including the 27 marked absent, 23 of them asserting exact bytes, and
the test ignores an `absent` override and **fails on any other**, so a future
claim about C++ cannot be skipped quietly. The same landing measured uf-chaos's
`appendNumber` — the one RFC 8785 number implementation in either tree, and the
one with no coverage — correct over 20,000 doubles agreeing three ways across V8,
the Python and the C++.

The related trap is a differential harness with no positive control. `a0ae304`'s
first two runs against `jsonschema.Draft202012Validator` had 11 and then 3
schemas the oracle accepted nothing for, where agreement proved only that both
sides reject everything; the final run drove a repair loop from the oracle so
every one of the 30 corpus documents has an accepting instance, and 1,532
instances agree over 29 of them. **An empty experimental result excludes nothing
until the experiment is shown able to produce a non-empty one.**

## Unfalsifiable on purpose, and said so

Not every check that cannot fail is a defect. Some properties are true,
load-bearing, and unreachable by any mutation of this tree, and the correct
treatment is to name them at the declaration, not to delete them. A reader who
finishes this document believing every such check is a bug will delete real
guards.

The rule that separates the two: **if the check would notice a writer other than
the one that produced the value, keep it and say it is unfalsifiable today; if it
only re-reads one writer's own output, delete the conjunct.**

`requireOutputOf`
(`modules/deployment/source/deployment/project-deployment.cpp:724-756`) is the
clean example of keeping. Its `function()` half is falsifiable — that mutation
aborts on the very next member read. Its `direction()` half is not:
`ProjectSchemaOwner::validateOutput` is the sole mint of a `ValidatedDocument`
and stamps `Output` every time, so no value carries `Input`. The comment at
`:731-735` says so, and says why it is kept: it is what would notice a second
mint. The loader's single `verifyExact` call site is the same
(`project-directory.cpp:1345-1350`): it passes the digest of the bytes it is
passing in, states that this is the legal and empty case, and points at the one
comparison two lines up that is not.

`requireLiveBinding` is the example of deleting. It compared four fields that had
each been copied out of the very row it compared against, so each conjunct was
masked by the others and none was individually falsifiable; the repair was to
drop two, and `modules/operator/source/operator/ledger.cpp:1449-1462` now
explains that activity is the whole of what the row can tell us that the binding
cannot, and that re-testing the epoch is a conjunct that cannot fail on its own.

Four more are carried without repair, each recorded beside the requirement it
qualifies in [the next block](../plans/2026-08-10-next-block.md) §2. A dispatch's
lease identity is one fact spelled three times — `lease_id`, `fencing_token` and
`lease.revision` always move together, and a case written specifically to
separate them, using the one schedule where the audit row and the live lease
disagree, could not. `HostDeliveryReport`'s friend count is inexpressible: that
its constructor has exactly one friend is the entire mechanism stopping a test
harness from fabricating a delivery that never ran, and adding a second friend
compiles with every case green — confirmed empirically rather than assumed, which
is the only honest form of that claim, and stated at
`modules/task/source/task/host-delivery.hpp:79-84` and
`conformance/include/conformance/host-delivery-fixture.hpp:33-36`. `OperationMachine`
takes no controller kind and cannot, so `p01`'s state-machine claim has no
kind-varying mutation, and that impossibility *is* the property. And the
budget-presence invariant — "a budget row is present exactly when the controller
kind requires one" — has both sides written by the same authority, so any
mutation moves them in step; it is kept because it converts a silent no-charge
into a loud failure.

Two more shapes are worth recognising on sight. A scoping property needs two of
the thing it scopes: **"only its own X" is untested until a fixture holds two
Xs**, however many assertions surround it. And a set written out by hand rather
than derived is sometimes the point — `tests/CMakeLists.txt:129-132` says that
deriving `UF_REQUIRED_CORE_REQUIREMENTS` from the gate list above it would make
it unfalsifiable, so it is written out and dropping both of a requirement's gates
has to fail there.

What to do with each is the same: record the zero where the requirement is
recorded, so the next reader meets it beside the thing it qualifies, and do not
let a green campaign be reported as if these were part of it.

## Delegating a check does not delegate the promise

The form deserves its own section because it was the first found in the
*exported* surface, and the only one with a live victim: **a real consumer's
suite passed while producing provenance documents the framework's own schema
would reject.**

Nothing malfunctioned. The framework called a validator; the validator refused
documents; the refusal message named `JournalProvenance`. What no code did was
connect any of that to `schema/umbraflow-journal-v1.schema.json`, where
`JournalProvenance` is actually defined — `additionalProperties: false`, four
required members, and `kind` restricted to five values.
`JournalProvenanceValidator` was a `std::function<Status(std::string_view)>`
supplied by the project, and both shipped implementations were one byte
comparison against the single literal that fixture also shipped:
`{"kind":"fixture"}` fails the `kind` enum and omits three required members,
`{"witness":"suite"}` omits all four and breaks `additionalProperties: false`.
Every suite run was green.

The tell was that the same class already solved the same problem one member away
and said why. `ProjectJournalSchemaOwner::create` demands
`exactJournalSchemaManifestBytes` so that, in the header's own words, "the
payload validator provably answers for the manifest this registration named;
without them the recorded `payload_schema_hash` is whatever an arbitrary
validator chose to return." One constructor, two validators, one anchored and one
free.

**The generalisable rule.** When a framework delegates a check to a consumer, it
keeps the promise and gives away the enforcement. That trade is safe only if the
framework can still tell that the delegate did the job — by pinning the bytes the
delegate must answer for, by taking back a value derived from them, or by probing
the delegate with an input it must reject. A callback typed
`Status(std::string_view)` offers none of the three, and a name like "the fixed
`X` schema" on such a callback is the promise with nothing behind it.

> **Closed 2026-08-11.** `JournalProvenanceValidator` is deleted;
> `ProjectJournalSchemaOwner::validate` enforces `$defs.JournalProvenance` in the
> framework, as a positional reader over exact JCS where the canonical member
> order is what enforces `required` and `additionalProperties: false` at once.
> The one thing worth carrying forward is what the repair had to prove:
> `contract-agent-a04` now drives six documents that each violate exactly one
> rule of that schema and one that violates none, because a framework check that
> merely compared bytes against the conforming document would have refused all
> six for the wrong reason and read as a fix. See
> [the journal record binding](../archive/plans/2026-08-11-journal-record-binding.md).

## A schema file nothing compares against is the same shape

The family applied to a *contract document* rather than to a check.
`schema/umbraflow-journal-v1.schema.json` is pinned by
`journal_envelope_schema_hash` and names two records this framework stores as
SQLite rows; `contract-state-s06` and `contract-agent-a04` both asserted those
shapes by searching the schema *text* for member names. Every assertion passed
for as long as the file existed, whatever the DDL said — and the DDL said
`canonical_event` for a column bound to the event's payload and nothing else.
Four of the nineteen column names had drifted, and every gate was green
throughout.

The generalisable form is one question, and it is cheap: **for every schema this
repository ships, name the code that produces or consumes the shape, and the
assertion that compares the two.** Where the answer is "a test greps the schema
file", the shape is documented and unenforced, and the two are indistinguishable
from the gate's name. The repair was to read the columns back from a database the
Operator created and compare the set with the schema's own `required` list —
which also made the reverse true for the first time: an edit to the schema file
alone can now turn a behavioural gate red.

## The documentation form

A review can make a ruling, be closed, and be archived with that ruling still
unexecuted — the queue then reports done while nothing ever ran. The 2026-07-25
sweep's `core` ruling sat that way for seventeen days (W12 in
[the next block](../plans/2026-08-10-next-block.md)). `CLAUDE.md` now blocks
archiving until every unexecuted ruling has a live owner named in the file being
archived, which is this family's positive control one level out: the artifact has
to state what it still owes.

`45b0fda` shows the same defect in citations rather than rulings. `b6c7e72` moved
two readers, and four plans went on placing them at their old home or at an
`operator-envelope.{hpp,cpp}` that never existed, with two inventory citations
pointing into a header body that no longer exists. A `file:line` in a plan is a
claim, and nothing gates it.

---

Instance detail follows for the two members that cost the most.

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
