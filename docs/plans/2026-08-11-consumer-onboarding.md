# Consumer onboarding: measuring what a new project actually has to write

Status: specification proposal. Nothing here is implemented. Ten questions in
§11 need a ruling. Every file-level claim was read at `c23efd3`; the counts in
§2 are reproducible from that tree.
Date: 2026-08-11
Scope: `umbraflow-cpp` only. It proposes moving code into `contract-suite/`,
`cmake/` and possibly `modules/core`; it writes nothing in a consumer tree and
specifies no consumer-side artifact.

Related: [consumer attestation](2026-08-11-consumer-attestation.md) specifies
what a consumer *claims*. This specifies what a consumer *builds* before it can
claim anything. §9 joins them.

## 0. The one-line answer

Of the 880 lines a consuming repository writes today, **213 are things only that
project can know.** 626 are framework work that currently lands on the consumer,
and 58 of those 626 are a verbatim copy of another block in the same file. The
yardstick is roughly right and the gap is roughly 3×. Of the 626, **360 can move
with no ruling on authority at all**; only 208 touch the validator question, and
that question splits cleanly along a line the registration itself already draws.

## 1. Correcting the premise before measuring against it

Two facts in the brief this document answers are stale, and both matter to the
arithmetic.

**`contract-suite/fixtures/arcana-expedition/provider.cpp` is 880 lines, not
660.** It gained 192 lines in `848e390` (the EffectivePlan mint) and 5 more in
`93698b4`. Any description of this surface written before `93698b4` understates
it by a quarter.

**The suite is four sources and four headers, not one header.** The exported
include directory is `contract-suite/include/operator-contract/` and holds
`project-under-test.hpp` (140), `operator-protocol.hpp` (649),
`observation-fixture.hpp` (777) and `host-delivery-fixture.hpp` (139). A
consumer's provider includes only the first two. The other two are compiled into
the consumer's binary regardless, and one of them is why the fixtures need a
`LIBS` entry (§7.4).

## 2. The measurement

`contract-suite/fixtures/arcana-expedition/provider.cpp`, every block, in file
order. Line spans are exact and sum to 880.

Classes:

| | |
|---|---|
| **P** | only the project can know it |
| **F** | the framework could supply it, and no authority question arises — it is either the framework's own schema or mechanical assembly |
| **A** | the framework could supply it only by shipping a default validator — the authority question, §6 |
| **D** | verbatim duplicate of another block in the same file |
| **N** | comment, blank line, namespace brace |

| Lines | Span | Block | P | F | A | D | N |
|---|---|---|---:|---:|---:|---:|---:|
| 1–13 | 13 | file comment | | | | | 13 |
| 14–39 | 26 | 20 `#include`s | | 26 | | | |
| 40–43 | 4 | namespace open | | | | | 4 |
| 44–54 | 11 | `ProjectIdentity` — carries the two roles | | 11 | | | |
| 56–113 | 58 | `k_expeditionPlugin` — the five functions | 58 | | | | |
| 115–172 | 58 | `k_rivalPlugin` — **56 of 58 lines byte-identical** to the above | | | | 58 | |
| 174–178 | 5 | project constants (artifact root, blob, provenance, state, observation) | 5 | | | | |
| 180–186 | 7 | `hashOf` | | 7 | | | |
| 188–192 | 5 | `refuse` | | 5 | | | |
| 194–223 | 30 | `looksLikeDeriveEnvelope` | | 30 | | | |
| 225–286 | 62 | `looksLikeReduceEnvelope` (2 project literals) | 2 | 60 | | | |
| 288–311 | 24 | `looksLikeOrderedMembers` | | 24 | | | |
| 313–324 | 12 | `looksLikePlanEnvelope` | | 12 | | | |
| 326–336 | 11 | `looksLikeStepEnvelope` | | 11 | | | |
| 338–378 | 41 | `vocabularyOf` — the `ProjectVocabulary` | 41 | | | | |
| 380–406 | 27 | `DeploymentSchemas` + `schemasOf` (7 identity strings) | 7 | 20 | | | |
| 408–438 | 31 | `registrationBytes` | | 31 | | | |
| 440–489 | 50 | `verifiedRegistration` | | 50 | | | |
| 491–529 | 39 | `canonicalValidator` — "are these bytes exact JCS?" | | | 39 | | |
| 531–597 | 67 | `documentValidator` (4 project literal comparisons) | 7 | | 60 | | |
| 599–653 | 55 | `journalPayloadValidator` (4-row table) | 20 | | 35 | | |
| 655–742 | 88 | `toolCatalogValidator` (8-row catalog + args literal) | 43 | | 45 | | |
| 744–790 | 47 | `dispositionReader` (4-row mapping) | 18 | | 29 | | |
| 792–859 | 68 | `makeProject` — build the four owners, return `ProjectUnderTest` | | 68 | | | |
| 862–879 | 18 | `projectUnderTest` — two identities + role switch | 12 | 6 | | | |
| — | 21 | blank lines between blocks | | | | | 21 |
| 860–861, 880 | 3 | namespace close | | | | | 3 |
| **Total** | **880** | | **213** | **360** | **208** | **58** | **41** |

### 2.1 The counts

| | Lines | Share |
|---|---:|---:|
| Only the project can know it (**P**) | 213 | 24% |
| Framework could supply it, no authority question (**F**) | 360 | 41% |
| Framework could supply it only with a default validator (**A**) | 208 | 24% |
| Verbatim duplicate within the file (**D**) | 58 | 7% |
| Comment, blank, brace (**N**) | 41 | 5% |

**Framework work currently landing on the consumer: 626 lines, 71%.**

Three of these deserve their number stated rather than summarised.

**The 58-line duplicate is exactly that.** `diff` over lines 56–113 and 115–172
reports two differing lines: the C++ variable name, and `plugin_id =
"arcana.expedition"` versus `"arcana.rival"`. Everything else — every plan
variant, every effect, the step intent, all five functions — is copied. It is
copied rather than parameterised because `ProjectIdentity::pluginSource` is a
`std::string_view` (line 52), so it can only hold a literal. The other fixture
does not have this problem: `test_support::pluginSource(pluginId)` takes the id
and builds the source. **The exemplar teaches the duplication.**

**The 137 lines of `looksLike*Envelope` validate the framework's own output.**
`modules/operator/source/operator/ledger.cpp` assembles the derive, reduce, plan
and step envelopes itself (lines 2584, 3748, 5015, 5288, 6297) and then calls
`plugin.canonicalize(...)` on bytes it just wrote. The consumer's validator is
being asked whether the Operator emitted the Operator's own schema correctly.
There is no project knowledge in that question and no authority in the answer.

**The `ProjectVocabulary` is the only block that is purely P and purely
irreducible.** All 41 lines of `vocabularyOf` are names, documents and mappings
nothing but the project can supply. It is also, per §5.4, the only part of the
consumer's surface that should be allowed to grow.

## 3. The second implementation, and what the two share

`contract-suite/fixtures/umbraflow/` does the same job a second time, split
across two files because one of them is also the Operator's own test fixture:

| File | Lines | Of which is the same job as arcana's provider |
|---|---:|---:|
| `provider.cpp` | 95 | 95 |
| `project-fixture.hpp` | 1532 | ~770 |

The remaining ~760 lines of `project-fixture.hpp` are `PreparedStore`,
`prepareStore`, `addController`, `createReadyOperation`, `reconcilingOperation`,
`agentProfileFor`, `TemporaryDirectory` and friends — machinery for
`tests/operator/*`, which the suite's own `harness.hpp`/`harness.cpp` duplicates
for suite use. That is a third spelling of the same store setup, but it is not
consumer work and is out of scope here.

**Comparable total: ~865 lines against arcana's 880.** Two independent
implementations of one job, agreeing to within 2%.

### 3.1 What they have in common, measured

Five functions are byte-identical modulo `inline` and namespace qualification.
Verified by `diff` over the exact spans:

| Function | arcana | umbraflow | Result |
|---|---|---|---|
| `hashOf` | 180–186 | 283–289 | identical |
| `looksLikeDeriveEnvelope` | 198–223 | 64–89 | identical |
| `looksLikeOrderedMembers` | 290–311 | 235–256 | identical |
| `looksLikePlanEnvelope` | 313–324 | 258–269 | identical |
| `looksLikeStepEnvelope` | 326–336 | 271–281 | identical |
| `looksLikeReduceEnvelope` | 228–286 | 169–230 | differs only in the two project literals (`prior_project_state` values, provenance bytes) |

Beyond the six functions, the two share a **shape** that neither could have
arrived at independently:

- the same eight-step `makeProject`: registration bytes → claims →
  `ProjectRegistrationSchemaOwner::create` with an exact-bytes-compare closure →
  `ProjectRegistration::verifyExact` → `ProjectSchemaOwner::create` →
  `ProjectJournalSchemaOwner::create` → `ProjectToolCatalogSchemaOwner::create`
  → `ProjectReconcileSchemaOwner::create`, each followed by `REQUIRE(...)`;
- the same four validator *shapes*: an allowlist standing in for a canonical
  validator, a `function × direction` switch, an `{eventType, payload} →
  schemaIdentity` table, a `{name, version, mutability, surface}` catalog, and a
  `document → disposition` table — each with its own hand-written `find_if`;
- the same plugin shape: `plan` matches `"tool_name":"([^"]*)"` out of the
  envelope and indexes a table of proposals; `derive`, `next_step` and `reduce`
  return near-constants; `reconcile` maps input to verdict.

`CLAUDE.md` calls two spellings of one fact a defect to remove. This is two
spellings of about 500 lines, and the second one was written *deliberately*, as
proof that the first is what a consumer writes. That makes it evidence rather
than an accident — and the evidence says the framework is asking for the same
500 lines from everyone.

## 4. The yardstick, applied

The ruling to design against is that a new project writes only what only it can
know. Measured against the file:

| Yardstick item | Present in arcana | Lines | Verdict |
|---|---|---:|---|
| its own schemas — state, observation, precondition, journal payloads | yes, as identity strings + the tables that stand in for the schemas | 7 + 20 | **honoured**, though see §4.1 |
| its tool catalog — names, versions, mutability, `ToolSurface` | yes, 8 rows | 43 | **honoured** |
| its plugin's five functions | yes, once — and again, copied | 58 (+58 D) | **honoured once, violated twice** |
| its `RuntimeArtifact` — page model and assets | **no** | 0 | **not asked for at all** — §4.2 |
| its reconcile disposition mapping | yes, 4 rows | 18 | **honoured** |
| *everything else is framework work* | — | 626 | **violated** |

### 4.1 The schemas are named, not written

Neither fixture carries a real JSON Schema document. Both hash an *identity
string* — `"arcana/expedition/observation.schema"`, `"observation"` — and use
that hash as the schema hash the registration pins. A real consumer will have
real schema files, and the byte-level requirement is severe: the registration
pins `sha256` over the exact schema bytes, and the schema owners require those
exact bytes to be handed in (`ProjectDocumentSchemaBytes`,
`ProjectToolCatalogSchemaOwner::create`, and so on — the reason is stated at
`modules/operator/source/operator/tool-invocation.hpp:150-153`: "without them an
owner is bound to a registration whose `tool_catalog_hash` it never has to
satisfy, and any validator at all could answer for that catalog").

So neither fixture exercises the path a real consumer takes, and the first thing
a real consumer discovers is that it must load six schema files as exact bytes
and keep six hashes in agreement with them. Nothing in the exported surface
helps with that. See prediction **R7**.

### 4.2 The `RuntimeArtifact` seam does not exist

The yardstick lists the consumer's page model and assets as consumer-owned.
The suite does not ask for one. `contract-suite/source/harness.cpp:155-158`:

```cpp
auto runtimeRelease(std::filesystem::path const& root) -> RuntimeRelease
{
    return observationRelease(root, observationRuntimeModel());
}
```

`observationRuntimeModel()` and `observationAssets()` are the *suite's* model and
the suite's PNGs, defined in
`contract-suite/include/operator-contract/observation-fixture.hpp:149` and `:226`.
`prepareStore` installs that artifact, activates a Host over it, and composes
every snapshot from it. `ProjectUnderTest` has no member for a RuntimeArtifact.

The consequence is visible in arcana's own bytes. Its `next_step` returns
`surface_id: "expedition.surface"`, `ui_target_id: "expedition.target"`,
`action_id: "expedition.press"` — names that cannot exist in the suite's model.
They are never resolved: the delivery leg runs `k_authorizeClickSource`
(`host-delivery-fixture.hpp:44-51`), a trusted Luau chunk baked into the suite
that resolves the binding `"confirm"`/`"activate"` from the suite's own model,
and `readStepIntent` reads only `step_key` out of the intent. So a consumer's UI
vocabulary passes the suite while corresponding to nothing.

That is not necessarily wrong — separating "the Operator sequences steps
correctly" from "this project's page model resolves" is a defensible split. But
it is undocumented, and it means **the suite pass a consumer earns is weaker
than the yardstick implies.** Ruling needed: Q6.

## 5. What moves into the framework, and what stays

Design constraint, taken as given: prefer moving code *into* the framework over
generating it *into* the consumer. §5.5 argues the one case where a generator
looks right and rejects it.

### 5.1 Move now, no ruling required — 360 lines

These are class **F**. None of them touches a project-owned schema, so none of
them touches authority.

**M1. The four envelope validators.** Export
`readDeriveEnvelope`, `readReduceEnvelope`, `readPlanEnvelope`,
`readStepEnvelope` from `operator-contract/operator-protocol.hpp`, beside the two
readers already there. This is not a new position; it is the position that header
already states, at lines 27-31:

> They belong to the suite rather than to a project provider because the operator
> protocol is the Operator's own schema and is the same for every project. What a
> provider supplies is the documents; what the Operator supplies is the reading of
> them. **A consumer therefore writes a `ProjectVocabulary` and never a JSON
> reader.**

Two of the six document families already follow that sentence. Four do not.
Applying it consistently removes 137 lines per consumer and removes the class of
bug where a consumer's structural matcher accidentally accepts a malformed
envelope — which no consumer test could catch, because the consumer did not write
the envelope.

The two project literals inside `looksLikeReduceEnvelope` (the accepted
`prior_project_state` values and the provenance bytes) stay with the consumer, as
a predicate parameter.

**M2. `hashOf`, `refuse`, `TemporaryDirectory`.** `hashOf` is already declared at
`contract-suite/source/harness.hpp:41`, in `source/` rather than `include/`, so
no consumer can reach it through the public header set — and both fixtures
re-implement it identically. Move the declaration to
`operator-contract/project-under-test.hpp` or a new
`operator-contract/provider-support.hpp`. 12 lines per consumer, and it removes
the incentive to `#include "harness.hpp"` (prediction **R4**).

**M3. Registration assembly.** `registrationBytes` + `verifiedRegistration` +
`makeProject` = 149 lines that do one thing: turn a set of hashes and a schema
layout into a `VerifiedProjectRegistration` and four schema owners. The framework
cannot supply the *layout* — that is the project's registration schema — but it
can supply everything either side of it. Proposed shape: the consumer supplies a
`ProjectRegistrationSource { exactJcs, claims }` and the framework does the
owner-creation, `verifyExact` and `REQUIRE` plumbing.

Note what makes this safe: `ProjectRegistrationClaims` is already the complete
list of what the registration must state, and `validateClaims` in
`modules/operator/source/operator/manifest.cpp:116-163` already refuses claims
that disagree with the schema hash or that carry mis-ordered artifact roots. The
framework is not deciding anything; it is calling functions in a fixed order.

**M4. The second role.** Delete `ProjectRole` from the provider's problem.
`ProjectUnderTest` should carry the plugin as `pluginSourceFor(std::string_view
pluginId) -> std::string` rather than as fixed bytes, and the suite should derive
the foreign registration itself by re-salting. This removes the 58-line duplicate,
the `ProjectIdentity` struct, and the role switch — 87 lines — and it removes the
failure mode where a consumer's two roles drift apart silently.

The property under test survives: `suite-project-authority.cpp:127` ("authority
does not cross ProjectRegistrations") needs two registrations that can each mint
documents, not two registrations a human typed twice.

**Total: ~360 lines per consumer, no ruling needed.**

### 5.2 The 208 contested lines, resolved by §6

Class **A**. See §6 for the argument; the outcome is:

| Seam | Whose schema decides? | Framework default? |
|---|---|---|
| `CanonicalJsonValidator` | RFC 8785 — nobody's project | **yes**, and it belongs in `core`, not in the suite |
| `ProjectDocumentValidator` dispatch | the framework's (`function × direction` is an Operator enum) | **yes**, as a composer over consumer-supplied per-function predicates |
| `ProjectDocumentValidator` per-function predicates | the project's | **no** |
| `JournalPayloadSchemaValidator` lookup | mechanical | **yes**, as a lookup over a consumer-supplied table |
| the journal payload table itself | the project's | **no** |
| `ToolCatalogValidator` lookup | mechanical | **yes**, same shape |
| the tool catalog itself | the project's | **no** |
| `ReconcileDispositionReader` lookup | mechanical | **yes**, same shape |
| the disposition mapping itself | the project's | **no** |

Moving the mechanism and keeping the table removes roughly 170 of the 208, and
leaves every decision where it is.

### 5.3 What stays with the consumer, and should

213 lines, unchanged: the plugin's five functions, the `ProjectVocabulary`, the
tool catalog rows, the journal payload table, the disposition mapping, the seven
schema identities, and the handful of project literals inside the validators.
Plus, for a real consumer, the six schema documents themselves (§4.1) — which
neither fixture has and which are likely the largest single item a real project
writes.

### 5.4 The one thing that should grow

`ProjectVocabulary` is 20 fields. Five of them —
`mismatchedPlanTool`, `oversizedPlanTool`, `twoStepPlanTool`,
`approvalRequiredPlanTool`, `reorderedEffectsTool` — exist so the suite can reach
five plan shapes, and they oblige a real project to add five tools to its
*catalog* that its game does not have, and five plans to its *plugin* that
nothing will ever invoke. That is a real cost and the header is honest about why
it is paid (`project-under-test.hpp:61-80`): the suite must not carry a proposal
no plugin produced.

> **Tested and partly confirmed, 2026-08-11 (`07abc3e`). Four of the five are
> gone; one stands.** The objection recorded here and at R10/Q7 was acted on:
> `mismatchedPlanTool`, `oversizedPlanTool`, `twoStepPlanTool` and
> `reorderedEffectsTool` are removed from the public header and from the tree
> entirely — zero occurrences anywhere under `contract-suite/`, `tests/` or
> `modules/`. `approvalRequiredPlanTool` remains, at
> `project-under-test.hpp:61-71`, and is still read by both fixtures and by
> `contract-suite/source/suite-control-ledger.cpp`. So `ProjectVocabulary` is
> **16 fields, one of them a synthetic tool**, not 20 and five.
>
> **The deciding test was not the objection but a falsification.** Each of the
> four was mutated to confirm it asserted something real, and all four turned out
> to be Operator invariants that do not vary by project — so the framework can
> and does test them itself, and charging every consumer a permanent
> tool-catalog row for them was the wrong trade. `approvalRequiredPlanTool`
> survives because the shape it reaches, a proposal whose risk requires human
> approval before the first dispatch, genuinely depends on the project's own
> plugin: only the project can say which of its tools has that risk. Umbraflow's
> own copies of the four stay, because `tests/operator/` reaches them directly
> and pays no registration cost for them.
>
> **What this vindicates is the recommendation, not the fear.** §5.4's rule —
> `ProjectVocabulary` is the right place to grow, and a vocabulary field is a
> fact only the project knows — is exactly the test that removed the four and
> kept the fifth. The four were not project facts.

It is nevertheless the right place for growth, and the attestation document's Q4
already proposes another field there (an unconfirmed-fact document, to gate
`A-04`'s project half). Recommendation: accept that `ProjectVocabulary` grows,
and make every other part of the provider shrink to compensate. A vocabulary
field is a fact only the project knows. A `find_if` is not.

But see Q7: five synthetic tools in a shipping tool catalog are inside
`tool_catalog_hash`, therefore inside `project_registration_hash`, therefore
inside every session identity. A consumer is being asked to permanently attest a
catalog containing test scaffolding. *(2026-08-11: one synthetic tool, not five
— see the note above.)*

### 5.5 The generator case, argued and rejected

There is one place a scaffolding generator looks obviously right: the provider is
mostly a fill-in-the-blanks form, and `cmake -P generate-provider.cmake` would
produce a compiling one in a second.

It is wrong here, for a reason specific to this design rather than a general
preference. **The generated lines would be validators, and a validator the
consumer owns but did not think about is exactly the failure this repository
spent a day cataloguing.** `docs/pitfalls/checks-that-cannot-fail.md` records
nine instances of "a name exists, the name promises something, and nothing
verifies the promise" — including one where a CMake helper named
`cpp_add_contract_suite` built its binary `NO_CTEST`, so seven compiled cases in
`tests/operator/test-project-plugin-contract.cpp` executed in no gate, and one
where a *consumer-owned* validator named for the fixed `JournalProvenance`
schema compared bytes to a literal that violates it (§6.4) — which is this
argument's own prediction, arriving before the generator did. A generated
`canonicalValidator` that returns `ok()` for everything is that defect,
pre-installed, in every consumer, with the consumer's name on it. When the
framework later tightens what canonical means, nothing tells the consumer which
of its generated lines to change.

Moving the code in has the opposite property: the consumer's build breaks, or
does not, and the framework's own tests are what keep the moved code honest.

The one place a generator-shaped tool *is* defensible is a **verifier**, not a
generator: a mode that reads a consumer's provider and reports what it did not
supply. That is deferred — it is only worth building once §5.1 and §5.2 have
shrunk the surface it would inspect.

## 6. The validator-authority question

### 6.1 What is actually recorded

The refusal is deliberate, per-seam, and stated six times in near-identical
words. `modules/operator/source/operator/project-plugin.hpp:90-94`:

> These validators are trusted deployment code. The canonical validator must
> reject anything other than exact RFC 8785 JCS. The document validator must
> validate the complete function-specific JSON Schema, including every
> project-owned nested payload. Neither callable is passed to plugin code or
> published in a business VM.

`modules/operator/source/operator/manifest.hpp:39-42`:

> The implementation must parse the complete registration, validate it against
> the owner's exact JSON Schema, and reject bytes that are not the exact RFC 8785
> JCS serialization. Returning claims without doing all three is a schema-owner
> bug, **never an extension point for project code.**

`modules/operator/source/operator/manifest.hpp:115-116`:

> The sole mint for `VerifiedProjectRegistration`. There is deliberately no loose
> field spec and **no API that canonicalizes caller-provided fields.**

And the reason it is a *callable* rather than framework code, stated four times
in the same form — `tool-invocation.hpp:150-153`, `journal-entry.hpp:82-85`,
`reconcile-outcome.hpp:94-95`, `project-plugin.hpp:100-104`:

> The exact Tool Catalog bytes are required, not merely referenced: without them
> an owner is bound to a registration whose `tool_catalog_hash` it never has to
> satisfy, and any validator at all could answer for that catalog.

The suite's own half, `project-under-test.hpp:24-26` and `:93-96`:

> The suite invents no project bytes: the schemas that judge them belong to the
> supplying deployment, so the documents that satisfy them must come from there
> too.
>
> No member carries an in-class initializer. Every one of them must come from the
> deployment, and **a defaulted authority would be an authority nobody granted.**

**Read carefully, this is not one argument. It is a mint-authority argument about
*project-owned schemas*, applied uniformly to six seams — four of which carry
project-owned schemas and two of which do not.**

### 6.2 The line the registration already draws

`ProjectRegistrationClaims` enumerates exactly which schemas the registration
pins:

```
manifest_schema_hash                    tool_catalog_hash
project_state_schema_hash               reconcile_payload_schema_manifest_hash
project_observation_schema_hash         journal_event_schema_manifest_hash
project_tool_precondition_schema_hash
```

Seven project-owned schemas. It pins **nothing** for:

- the derive, reduce, plan-input and step-input envelope shapes — the Operator
  builds those itself in `ledger.cpp` and hands them to the project's validator
  for approval;
- `PlanProposal`, `UIActionIntent`, `WaitIntent` — pinned by
  `operator_protocol_schema_hash` in the **session manifest**, which is the
  deployment's, not the project's;
- RFC 8785 canonicality, which is not a schema at all.

**That is the test, and it is mechanical: for any seam, ask which hash inside
`project_registration_hash` pins the schema being applied. If none does, the
framework may supply a default and no authority moves. If one does, it may not.**

This is not a new principle being introduced against the recorded design. It is
the recorded design's own principle, applied to a distinction the recorded design
did not need to make when every construction site was a fixture.
`docs/plans/2026-08-10-w2-effective-plan.md:1185-1189` says as much:

> The other four schema owners are built by "trusted deployment code", and **no
> production deployment exists yet** — every construction site today is a test
> fixture.

And the design has already applied the distinction once, deliberately, in the
sentence quoted in §5.1: `readPlanProposal` and `readStepIntent` are
framework-supplied readers for framework-owned protocol documents, and the header
gives that exact reason. The proposal here is to stop making an exception of the
other four.

### 6.3 The stated worry, tested

The brief's reading is that a consumer *choosing* to link a framework default is
still the consumer's decision, that the registration hash binds what they chose,
and that this survives only if the default is genuinely opt-in and genuinely
replaceable. Tested:

**Does the registration hash bind the choice? For the project-schema seams, yes —
and that is precisely why the framework must not default there.** The registration
pins `tool_catalog_hash`. If the framework shipped a default `ToolCatalogValidator`
carrying its own idea of a catalog, the consumer's registration would name a
`tool_catalog_hash` that the default validator never had to satisfy — the exact
failure `tool-invocation.hpp:150-153` names. So for those four seams the answer is
not "attribution survives"; it is "the mechanism that produces attribution is the
thing being bypassed".

**For the two framework-schema seams, the question does not arise.** No hash binds
the answer, because the framework already decided it when it wrote the bytes.

**So the line can be held, and it is not held by discipline.** It is held by
asking a question with a mechanical answer. A framework default that needed a new
hash in `ProjectRegistrationClaims` to be legitimate is a default that should not
ship; a framework default that needs no hash at all is validating the framework's
own output.

### 6.4 The JCS canonicaliser is a separate product, and belongs in `core`

`CanonicalJsonValidator` is the sharpest case and the one where the current
arrangement is least defensible. The question it answers — "are these bytes exact
RFC 8785 JCS?" — has one right answer, defined by an RFC, containing no project
content of any kind. Both fixtures fake it with an allowlist of 17 literals,
because no C++ implementation exists to call.

The tree's position on this is already recorded, in `core` itself
(`modules/core/source/core/text/json-text.hpp:15-19`):

> It lives in core because three components write JSON that a fourth reader
> compares byte for byte … **A second spelling of this transform cannot fail a
> test — it produces bytes that merely disagree.**

`core` today holds two of the three RFC 8785 rules — the string escape
(`appendJsonString`) and member-name ordering (`jsonMemberNameLess`) — and
`docs/reviews/2026-08-10-third-round-review.md:493-500` records that number
formatting and container framing "have no C++ implementation; every C++ emitter
hand-writes them inline". A complete canonicaliser exists twice outside C++:
`tools/annotate/jcs.py` and `modules/task/runtime/jcs.luau`. The same review's
finding R3-F8 calls the *second Python* spelling a defect that `CLAUDE.md`
forbids outright.

There are therefore already three-and-a-half spellings of RFC 8785 in this tree,
and the current onboarding design asks every consuming repository to add one
more. That is the defect the repository has spent two days removing, scaled by
the number of consumers.

**Recommendation: a JCS canonicalise-and-validate entry point in
`modules/core/source/core/text/`, validated against `tests/vectors/jcs-vectors.txt`
alongside the Python and Luau implementations, and offered to consumers as the
default `CanonicalJsonValidator`.** This needs `evaluate-core-capability` before
it is written; it is a genuine core-admission question, not a formality. Q3.

**A second validator has the same shape and a worse ending, found 2026-08-11.**
`JournalProvenanceValidator` is delegated to the project exactly as
`CanonicalJsonValidator` is — and unlike JCS, the schema it answers for is not
merely project-independent, it is already **written down in this repository**, as
`JournalProvenance` in `schema/umbraflow-journal-v1.schema.json`:
`additionalProperties: false`, four required members, `kind` restricted to five
values. Nothing connects the callback to that file. Both fixtures implement the
validator as one byte-comparison against the single provenance literal they
ship, and **both literals violate the schema** — umbraflow's `{"kind":"fixture"}`
fails the enum and omits three required members; arcana's `{"witness":"suite"}`
omits all four and breaks `additionalProperties`. A consumer's suite therefore
passes while producing provenance documents the framework's own schema rejects,
and three separate strings — the type's comment, the `UF_TRY_CONTEXT` at
`modules/operator/source/operator/journal-entry.cpp:121`, and both refusal
messages — assert that the fixed schema was enforced.

**This is a gap, not a design decision**, and the deciding evidence is inside the
same class rather than a matter of taste. `ProjectJournalSchemaOwner::create`
demands `exactJournalSchemaManifestBytes` so the *payload* validator "provably
answers for the manifest this registration named; without them the recorded
`payload_schema_hash` is whatever an arbitrary validator chose to return"
(`journal-entry.hpp`). The provenance validator has no such pin and returns
`Status` rather than a derived value, so nothing in the record shows what it
answered for. One constructor, two validators, one anchored and one free. The
"a project must participate" reading is true of provenance *values* —
`principal_id`, `observation_ids` are project-shaped — and does not reach the
schema decision, which is fixed and framework-owned.

**Recommendation: delete `JournalProvenanceValidator` and validate the document
in the framework against its own schema.** It costs the consumer one fewer
callback, which makes it the rare correction that reduces onboarding cost, and
it is the answer §6.2's registration-pin line already implies. D1 in §6.5 catches
it directly: no member of `ProjectRegistrationClaims` pins a provenance schema,
because there is nothing project-specific to pin. If delegation must survive for
a reason not yet stated, the framework must probe each supplied validator with a
known-bad document at construction and refuse one that accepts it — D2, applied
to a validator instead of a default. Recorded as the ninth instance in
[checks that cannot fail](../pitfalls/checks-that-cannot-fail.md), which carries
the generalisable form: delegating a check gives away the enforcement and keeps
the promise.

### 6.5 How you would know the line was being crossed

Three detectors, in increasing order of cost. All three are things this
repository already does elsewhere.

**D1 — the hash test, run at review time.** Before shipping any framework
default, ask which member of `ProjectRegistrationClaims` pins the schema it
applies. If the honest answer requires adding a member, the default is
illegitimate. Cost: one question per proposal. This is the tripwire that actually
prevents the drift, because it fires before code exists.

**D2 — the falsification test, run in CI.** Every framework-supplied default must
have a case that goes red when the default is replaced by a permissive stub. This
is the discipline `docs/pitfalls/checks-that-cannot-fail.md` already mandates:
"Remove the property, watch the check turn red, put the property back." A default
nothing can distinguish from `return ok();` is not a default, it is a hole.

**D3 — the census, run per release.** Across all consumers, count how many supply
a non-default implementation of each seam. The expected reading is *zero* for the
two framework-schema seams — nobody should ever need to re-read the Operator's own
envelope — and *all of them* for the four project-schema seams, because the
framework ships no default there to fall back to. **If a consumer is ever observed
supplying a project-schema validator that is a thin wrapper around a framework
helper, the line has moved and the helper should be withdrawn.** This is a real
measurement only once there are two consumers; today the census is 2 of 2 for
every seam, because there are no defaults at all.

Concretely: the failure mode the brief worries about — "the framework's validator
becomes the path of least resistance to the point that nobody writes their own" —
cannot occur for the four project-schema seams under this proposal, because
nothing is shipped for them. It is *guaranteed* to occur for the two
framework-schema seams, and that is the intended outcome.

## 7. The build system

### 7.1 What the framework offers today

One function: `uf_add_operator_contract_suite(TARGET PROJECT SOURCES LIBS
CASES)` in `cmake/operator-contract-suite.cmake:91`. It builds an executable from
the suite's four sources plus the consumer's provider, applies
`cpp_apply_safety_profile` and `cpp_apply_utf8_manifest`, links
`${PROJECT_NAME}_operator`, stages the ONNX runtime DLLs, and registers a CTest
named `contract-suite-<PROJECT>` guarded by `uf_require_executed_assertions`.

That is the entire offer. There is no `install()` rule anywhere in the tree, no
exported package config, and no `PROJECT_IS_TOP_LEVEL` or `BUILD_TESTING` guard
anywhere in any `CMakeLists.txt`.

### 7.2 `add_subdirectory` does not work today, and the reasons are mechanical

Not "would be awkward" — would not configure, and where it configures would
produce a binary that fails at runtime. Four blocking defects:

**B1 — `CMAKE_SOURCE_DIR` is used where `PROJECT_SOURCE_DIR` is meant.** Under
`add_subdirectory` it resolves to the *consumer's* root. Affected:
`CMakeLists.txt:40,44,57,86,90,91`; `cmake/build.cmake:26` (the path to
`scripts/embed_luau.py`) and `:182`; `cmake/static-analysis.cmake:23` (the path
to `.clang-tidy`); `entry/CMakeLists.txt:128`; and about ten sites in
`tests/CMakeLists.txt`. Configure fails at the first one.

**B2 — `${PROJECT_NAME}` is re-expanded inside the function at call time.**
`cmake/operator-contract-suite.cmake:204` reads
`if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)`. Called from a consumer's directory,
`PROJECT_NAME` is the consumer's, the target does not exist, the branch is not
taken, and `cpp_stage_runtime_libraries` silently does not run. The binary links
and then fails to start because `onnxruntime.dll` is not beside it. **This is a
check that cannot fail, in the ninth costume:** the staging step is present, named
correctly, and unconditionally skipped for exactly the caller it exists for.

The same trap is in the documentation the fixtures constitute:
`contract-suite/fixtures/arcana-expedition/CMakeLists.txt` is headed "Everything
a consuming repository writes to run the suite, in full" and writes
`LIBS ${PROJECT_NAME}_image`. A real consumer copying that line gets
`<Consumer>_image`, which does not exist, and the link fails on
`uf::image::encodeRgbaPng`. **The exemplar is not runnable by the audience it is
written for.**

**B3 — doctest lives under `tests/`.** `UF_CONTRACT_SUITE_DOCTEST_DIR` is
`"${CMAKE_CURRENT_LIST_DIR}/../tests/external/doctest"`
(`operator-contract-suite.cmake:25`), so the suite that was deliberately placed
outside `tests/` still requires `tests/` to be present. It is vendored in-tree
(three tracked files), not a submodule.

**B4 — payload preconditions.** Configure hard-fails unless
`modules/ocr/external/onnxruntime/include/onnxruntime_c_api.h` exists (fetched by
`python scripts/fetch_external.py --module ocr`, ~47 MB), the Luau submodule is
checked out (`.gitmodules` has exactly one entry,
`modules/script/external/luau`), and `find_package(Python3 Interpreter)` succeeds
for `task`'s `[embed]` step. A consumer inherits all three.

**Cost to fix: mechanical and small.** B1 is a substitution across about twenty
sites. B2 is freezing `PROJECT_NAME` into a cached variable at include time, as
line 26 already does correctly for `UF_CONTRACT_SUITE_OPERATOR_TARGET`, and
adding a `UF_CONTRACT_SUITE_IMAGE_TARGET` so the fixtures stop spelling it. B3 is
moving or duplicating the doctest path. B4 is not fixable, only documented.
**Recommendation: fix B1–B3 and make `add_subdirectory` the supported route.**

### 7.3 `FetchContent` costs more than `add_subdirectory`, for less

`FetchContent` is `add_subdirectory` plus a download, so it inherits every defect
above and adds two: it does not initialise git submodules unless
`GIT_SUBMODULES` is spelled explicitly (Luau, 0.730), and it cannot run
`scripts/fetch_external.py`, so the 47 MB ONNX payload has to arrive some other
way. A consumer would still be running two commands before configuring.
**Recommendation: not now. Revisit only if the ONNX payload leaves the tree.**

### 7.4 The binary-package finding still holds — but the recorded reason is
imprecise

`cmake/operator-contract-suite.cmake:13-16` says:

> A test binary must be built with the consumer's own safety profile and
> sanitizers to mean anything, and this repository installs nothing today — the
> Operator's public headers reach task, engine and ocr, so a binary package would
> have to carry a vendored ONNX runtime payload before it carried a single
> contract.

**The conclusion is right. The header-reach premise is only partly right, and the
real reason is stronger.**

At the header level: only two of the twelve `modules/operator` public headers
reach `task` at all (`ledger.hpp:14-16` and `runtime-installation.hpp:3`), and
the chain stops at `ocr/engine.hpp`, which is a pure port — it includes no ONNX
header. The only file naming `Ort` is `modules/ocr/source/ocr/ffi/onnx-engine.cpp`.
A provider translation unit that includes only `journal-entry`, `manifest`,
`project-plugin`, `reconcile-outcome`, `tool-invocation` and `effective-plan`
touches no `task` header.

At the link level the reach is total and unavoidable. Every module is `type =
static`, so PRIVATE dependencies still propagate as `$<LINK_ONLY:>`. Linking
`UmbraFlow_operator` links **all four vendored third-parties**: SQLite 3.53.4
(amalgamation, `modules/operator/external/sqlite`), stb
(`modules/image/external/stb`), Luau 0.730 (submodule), and the imported ONNX
Runtime 1.28.0 shared library. That is what makes a binary package wrong, and it
does not depend on which headers a consumer happens to include.

There is also a claim in that same comment worth correcting. "A test binary must
be built with the consumer's own safety profile and sanitizers to mean anything"
is the stated intent, but `cpp_apply_safety_profile(${ARG_TARGET})` applies
**this repository's** profile, composed from this repository's options
(`CPP_ENABLE_HARDENING`, `CPP_WARNINGS_AS_ERRORS`, the three sanitizer options).
A consumer gets the consumer's profile only insofar as it pre-sets this
repository's cache variables. Q8.

### 7.5 One more `add_subdirectory` consequence worth stating

`enable_testing()` at `CMakeLists.txt:65` is unconditional, `add_subdirectory(tests)`
is gated on `CPP_BUILD_TESTS` (default ON), and `contract-suite` is added
unconditionally whenever `${PROJECT_NAME}_operator` exists — deliberately outside
`CPP_BUILD_TESTS`, per the comment at lines 68-73. So a consumer that adds this
repository as a subdirectory acquires, in its own CTest, this repository's entire
test suite *and* both in-tree contract-suite runs (`contract-suite-umbraflow`,
`contract-suite-arcana`) alongside its own. Recommendation: leave `contract-suite`
unconditional (that is the point) but gate the two in-tree fixture runs on
`PROJECT_IS_TOP_LEVEL`, so a consumer gets the suite and not this repository's
two rehearsals of it.

## 8. The gates, the presets, the toolchain

### 8.1 The four gates: no, and plainly

| Gate | What it checks | Ship to consumers? |
|---|---|---|
| `scripts/fix_format.py` | CRLF→LF, trailing whitespace, tabs→4 spaces, single trailing newline, UTF-8, no NUL | **No.** House style. |
| `scripts/check_cpp_format.py` | column alignment of adjacent member declarations and designated initialisers; its own docstring calls it "a conservative recognizer, not a C++ formatter" | **No.** House style, and idiosyncratic. |
| `scripts/check_modules.py` | parses `modules/*/manifest.txt`; no cycles, `core` declares no dependencies, `[embed]` validity | **No.** A consumer using its own build system has no `manifest.txt`. |
| `scripts/check_safety.py` | `reinterpret_cast`/`const_cast`/`new`/`delete`/`malloc` only under `ffi`/`platform`/`unsafe` with a `// SAFETY:` comment; no `std::unreachable`, no `.detach()`; `modules/core` must not `throw`; `[[nodiscard]]` on `Result`-returning header functions; **ten Win32 foreground/input APIs forbidden everywhere** | **Mostly no — one exception, §8.2.** |
| `scripts/ci-local.*` | runs the four, then configures/builds/`ctest -L CI` against `build/$Preset` | **No.** Hard-codes this repository's preset names and binary-dir convention. |

All four hard-code this repository's layout — `SOURCE_ROOTS = ("modules",
"entry", "tests", "contract-suite")` in two of them, `root / "modules"` in a
third, the literal substring `"modules/core"` in a fourth. They take `--root`,
but the directory *names* are baked in.

The honest statement is the one the brief anticipated: **these encode this
project's house rules, not the contract, and shipping them would be generosity
that a consumer has to maintain.** A consumer that wants them can copy them; that
is a copy of a style guide, not a second spelling of a fact.

### 8.2 The one rule that is not house style

`check_safety.py` forbids `SetForegroundWindow`, `SetFocus`, `SendInput`,
`mouse_event`, `keybd_event`, `SetCursorPos`, `BringWindowToTop`,
`SwitchToThisWindow`, `AttachThreadInput` and `SetActiveWindow` everywhere, citing
`docs/plans/2026-07-21-product-form-and-roadmap.md` decision 1. That is a
**product invariant of the whole host**, not a formatting preference: a consumer
that calls `SendInput` defeats the background-only guarantee every layer above it
is built on, and no Operator contract case can detect it, because the suite is a
compiled binary and the violation is in the consumer's source.

That shape — a fact upstream cannot verify, in the consumer's tree, that matters
— is exactly what
[consumer attestation](2026-08-11-consumer-attestation.md) exists for.
**Recommendation: the background-only rule becomes a consumer attestation, not a
shipped script.** Whether it is a tenth requirement or a field on the existing
nine is Q9.

### 8.3 Presets and toolchain

`CMakePresets.json` carries 20 configure presets across three hosts
(`x64-*`, `linux-*`, `macos-*`, plus `vs2022`). A consumer using
`add_subdirectory` uses **its own** presets and inherits none of these; what it
inherits is the requirement set, which is: C++23; MSVC / clang / gcc as the three
presets configure them; Python 3 on `PATH` for the Luau embed step; git with
submodules initialised for Luau 0.730; and the ONNX payload fetched before
configure. The `x64-*` presets additionally expect MSVC activated in the shell
(`.claude/skills/build-project/script/windows/build-env.bat`).

**Recommendation: document these five as the toolchain contract, in the suite's
own header comment rather than in a separate file, and add a configure-time
`message(FATAL_ERROR)` naming each missing one.** Two of the five already fail
loudly (ONNX header, Python3); Luau-submodule-not-initialised currently fails as
a confusing `add_subdirectory` error.

## 9. Relation to consumer attestation

[Consumer attestation](2026-08-11-consumer-attestation.md) §7 orders a consumer's
work in six steps. Onboarding is step 4, and it is the only step in that table
whose cost is upstream's to set:

| Step | Owner of the cost |
|---|---|
| 1. Build the ContentPack | consumer |
| 2. Attest `D-01`–`D-07`, `D-09` | consumer |
| 3. Implement the ProjectPlugin and registration | consumer, mostly (§4.1 says the schemas are the bulk) |
| **4. Run the exported suite against the real plugin** | **upstream — this document** |
| 5. Attest `D-08` | consumer |
| 6. Register with the complete `attestations` root | consumer |

Three joins are worth stating explicitly, because each is a place where the two
documents constrain each other:

**J1 — `D-08` is blocked on step 4, and only on step 4.** The attestation
document rules that the suite is a precondition of `D-08` and of nothing else.
So every line removed from §2's 626 is a line removed from the critical path to
the ninth attestation, and to `attest-dual-game-p05` behind it. That is the
concrete value of this work, and it is why the measurement matters more than the
recommendations.

**J2 — the `attestations` root already has a seam, and it has a trap.** The
attestation set is proposed as one entry in `project_artifact_roots`. A consumer
whose registration names that root must also supply the blob to the suite, via
`ProjectUnderTest::artifactBlobs` — arcana already supplies one such blob (`map`
/ `expedition-map-bytes`), so the mechanism works. But `verifyArtifactClosure`
enforces exact closure in both directions, so a consumer that adds the
`attestations` root to its registration and forgets the blob fails at
`loadPlugin`, inside `prepareStore`, with a message about artifact closure rather
than about attestations. That is a loud failure in an unhelpful place. Worth one
sentence in `project-under-test.hpp`.

**J3 — `A-04`'s project half, and where the vocabulary should grow.** The
attestation document's Q4 recommends gating `A-04`'s project half by adding an
unconfirmed-fact document to `ProjectVocabulary` rather than by attesting it.
§5.4 here agrees and gives the general rule: `ProjectVocabulary` is the one part
of the consumer surface where growth is honest, because every field in it is a
fact only the project knows. This document's §5 should be read as the budget that
makes that growth affordable.

## 10. What I predict the real attempt hits

Written to be falsifiable. Each is a specific thing a real consumer standing up
the suite in a project with no build system should hit, in roughly the order it
would hit them. A prediction that turns out wrong is the useful outcome.

**Configure and link, before any C++ is written:**

- **R1.** `add_subdirectory` fails at configure on `CMAKE_SOURCE_DIR`, at
  `cmake/build.cmake:26` (`scripts/embed_luau.py` not found) or
  `cmake/static-analysis.cmake:23` (`.clang-tidy` not found). §7.2 B1.
- **R2.** After working around R1, `LIBS ${PROJECT_NAME}_image` copied from the
  arcana exemplar resolves to a non-existent target and the link fails on
  `uf::image::encodeRgbaPng`, pulled in through `observation-fixture.hpp:130`.
  The fix — writing `UmbraFlow_image` literally — is not discoverable from any
  documentation. §7.2 B2.
- **R3.** On Windows, the suite binary links and then fails to start: the
  `if(TARGET ${PROJECT_NAME}_ocr_onnxruntime)` guard is false in the consumer's
  scope, so no DLL is staged. The symptom is a missing-DLL dialog with no
  reference to anything in this repository. §7.2 B2. **This is the prediction I
  am most confident about and the one that will cost the most time**, because
  nothing in the failure names the cause.
- **R4.** The consumer reaches past `include/operator-contract/` into
  `contract-suite/source/` — most likely for `hashOf`, which every provider needs
  and which is declared only at `harness.hpp:41`. The suite's own CMake puts
  `source/` on the include path, so this compiles and looks intended.
- **R5.** Before any of the above: the ONNX payload (~47 MB, via
  `scripts/fetch_external.py --module ocr`) and the Luau submodule are both
  required and neither is mentioned in the suite's documentation. Configure fails
  on the first with a good message and on the second with a bad one.

**Writing the provider:**

- **R6.** The provider is written by copying `arcana-expedition/provider.cpp` and
  substituting strings — because there is no other way to learn the shapes of
  the four Operator envelopes. The five `looksLike*Envelope` functions arrive
  byte-identical for the third time.
- **R7.** The consumer has *real* JSON Schema documents and discovers that the
  registration pins `sha256` over their exact bytes and that each owner requires
  those exact bytes to be handed in. Neither fixture models this — both hash
  identity strings — so there is no example to follow, and the consumer builds
  its own six-file load-and-hash step. **I expect this to be the single largest
  block of code the real attempt writes that neither fixture contains.** §4.1.
- **R8.** The consumer looks for where to supply its own RuntimeArtifact / page
  model, finds no member on `ProjectUnderTest`, and either concludes the suite
  does not test that (correct, §4.2) or spends time looking. Its
  `ui_target_id` / `surface_id` / `action_id` values will be accepted while
  corresponding to nothing.
- **R9.** `projectUnderTest(ProjectRole::Foreign)` is satisfied by duplicating
  the plugin source with one identifier changed, reproducing arcana's 56-of-58
  copy — unless the consumer independently invents umbraflow's
  `pluginSource(pluginId)` parameterisation, which the exemplar does not show.
- **R10.** The consumer objects to adding five tools to its shipping tool catalog
  (`mismatchedPlanTool`, `oversizedPlanTool`, `twoStepPlanTool`,
  `approvalRequiredPlanTool`, `reorderedEffectsTool`) that its game does not
  have and that will be inside `tool_catalog_hash` forever. §5.4, Q7.
  **Resolved for four, stands for one (2026-08-11, `07abc3e`).** All but
  `approvalRequiredPlanTool` are removed from the public header and the tree;
  each was first mutated to confirm it asserted something real, and all four
  proved to be Operator invariants that do not vary by project, so the framework
  tests them itself. The fifth depends on the project's own plugin and stays.
  The prediction was therefore correct as an objection and correct as a cost —
  this is a consumer's objection acted on, not overruled — but the cost was
  four-fifths avoidable, which R10 did not claim either way.
- **R11.** The canonical validator is implemented as an allowlist, like both
  fixtures, because writing RFC 8785 in C++ for one suite run is not worth it —
  and the consumer will not notice that this means its suite run proves nothing
  about canonicality. §6.4.
- **R12.** The `observedReduceInput` / `observedDeriveInput` recorders are missed
  on the first pass: nothing in the type system requires the document validator
  to write to them, and the failure surfaces as a suite case comparing against an
  empty string.

**What I predict will *not* be a problem**, stated so it can be falsified in the
other direction: the `ProjectVocabulary` itself, once its 20 fields are
understood *(16 since `07abc3e` on 2026-08-11 — see §5.4)*; the
`baselineEntry.eventType` / `registration.baselineEventType()`
agreement, which `prepareStore` checks with a `REQUIRE` and a comment naming the
provider; and `readPlanProposal` / `readStepIntent`, which are exactly the seam
that works and which the consumer will never think about.

**Two composite predictions:**

- **R13.** The real attempt's provider lands between 700 and 950 lines, of which
  fewer than 300 are project-specific. If it lands materially below 700, either
  the schemas were faked (as both fixtures fake them) or a framework surface I
  have not read was used.
- **R14.** The single most-cited complaint will not be any validator. It will be
  the build: R1–R3 in some order, and specifically that the exemplar
  `CMakeLists.txt` labelled "everything a consuming repository writes, in full"
  is not a thing a consuming repository can write.

## 11. Open questions

Ten. Each carries a recommendation. None is decided here.

**Q1 — do the four envelope readers move into `operator-protocol.hpp`?** §5.1 M1.
*Recommend yes.* It applies a rule that header already states, removes 137 lines
per consumer, and no hash in `ProjectRegistrationClaims` pins those shapes. The
cost is that the Operator's envelope layout becomes a published surface, which it
effectively already is — both fixtures encode it.

**Q2 — do the three lookup mechanisms move (journal payload, tool catalog,
disposition)?** §5.2. *Recommend yes, as composers over consumer-supplied
tables.* The project keeps every row; the framework supplies the `find_if`. The
risk is that a composer with a convenient default argument becomes a default
table; the mitigation is that the composer must have no defaulted parameter, the
same reason `ProjectUnderTest` carries no in-class initialiser.

**Q3 — does `core` gain a complete RFC 8785 canonicaliser?** §6.4. *Recommend
yes, gated on `evaluate-core-capability`.* Three-and-a-half spellings exist and
the current design asks every consumer to add one. The counter-argument is real
and should be heard: `core` deliberately holds only the two rules a cross-language
byte comparison needs, and the third-round review notes number formatting and
container framing "cannot [be implemented] until C++ gains a value-tree entry
point" — so this is not a small addition, it is a JSON value tree in `core`.
**This is the question most likely to be answered no**, and if it is, the
consequence must be stated plainly rather than left implicit: every consumer's
canonical validator is an allowlist, and no consumer's suite run proves anything
about canonicality.

**Q4 — is the two-role duplication fixed by parameterising the plugin source?**
§5.1 M4. *Recommend yes:* `ProjectUnderTest` carries `pluginSourceFor(pluginId)`
and the suite derives the foreign registration. Removes 87 lines and a silent
drift. The cost is that a consumer whose plugin source genuinely cannot be
parameterised by id has to fake it — I know of no such case.

**Q5 — is `add_subdirectory` the supported route, with B1–B3 fixed?** §7.2.
*Recommend yes*, and that `FetchContent` and any binary package stay
unsupported and be said to be unsupported. The fix is mechanical; the alternative
is that every consumer rediscovers R1–R3.

**Q6 — should the suite take the consumer's RuntimeArtifact?** §4.2. *Recommend
no for now, and that the header say so.* Adding a `RuntimeArtifact` member to
`ProjectUnderTest` would make every consumer produce a page model before it can
run a single Operator contract case, which inverts the dependency the attestation
document's §7 relies on. But the current silence is worse than either answer:
today a consumer can believe its UI vocabulary was exercised when it was not.

**Q7 — do the five plan-shape tools belong in a shipping tool catalog?** §5.4.
They are inside `tool_catalog_hash`, therefore inside
`project_registration_hash`, therefore inside every session identity a consumer
ever creates. *Recommend accepting them and documenting why*, because the
alternative — a second catalog for suite runs — would mean the suite proves
things about a catalog production never uses, which is the defect
`tool-invocation.hpp:150-153` exists to prevent. But the owner should decide
knowingly, because it is permanent.

> **Answered 2026-08-11 (`07abc3e`), and by a third option this question did not
> list: remove four of the five from the consumer's surface entirely.** The
> question framed a binary — one catalog with the scaffolding in it, or a second
> catalog the suite alone uses — and correctly rejected the second. What neither
> branch asked was whether each tool had to be a *project* fact at all. Mutating
> the four showed they assert Operator invariants that do not vary by project, so
> the framework tests them directly and no consumer pays a registration row.
> `approvalRequiredPlanTool` is the one that survives the same test, because
> which of a project's tools carries approval-requiring risk is decided by that
> project's plugin. Q7's recommendation now applies to one tool instead of five,
> and is accepted on those terms: it stays in the shipping catalog, and §5.4
> documents why. **The generalisable part is the test, not the answer** — before
> asking whether a consumer should pay for a vocabulary field, ask whether the
> property it reaches varies by project.

**Q8 — whose safety profile does a consumer's suite binary carry?** §7.4. The
comment says the consumer's; the code applies this repository's. *Recommend
correcting the comment and adding an opt-out*, so a consumer can build the suite
under its own sanitizer configuration. The stated rationale for compiling the
suite into the consumer's binary is only true if this is true.

**Q9 — how is the background-only rule carried to consumers?** §8.2. It is the
one rule in `check_safety.py` that is a product invariant rather than house
style, and no compiled contract case can detect a violation. *Recommend a
consumer attestation*, either as a tenth requirement or as a field on the
existing set — the mechanism specified in
[consumer attestation](2026-08-11-consumer-attestation.md) §2 fits it exactly.

**Q10 — are the two in-tree fixture runs gated on `PROJECT_IS_TOP_LEVEL`?**
§7.5. *Recommend yes.* A consumer should get the suite, not this repository's two
rehearsals of it in its own CTest namespace. The cost is one guard; the
alternative is that `contract-suite-arcana` appears in a consumer's test report
and nobody can say why.

## 12. Index entry

This document is not registered anywhere. `docs/INDEX.md` and
`docs/plans/README.md` are owned by another agent in this session, so the entries
are stated here rather than added.

For `docs/INDEX.md`, after the Consumer attestation entry (currently line 25):

```markdown
- [Consumer onboarding](plans/2026-08-11-consumer-onboarding.md) — what a
  consuming repository actually has to write, measured block by block against
  the exported contract suite: 213 of 880 lines are project facts and 626 are
  framework work. Proposal only; ten questions await a ruling.
```

For `docs/plans/README.md`, beside the Consumer attestation entry:

```markdown
- [Consumer onboarding](2026-08-11-consumer-onboarding.md) — **specification
  proposal, nothing implemented.** The line-by-line measurement of
  `contract-suite/fixtures/arcana-expedition/provider.cpp` and of the second
  fixture doing the same job, what moves into the framework and what stays, the
  validator-authority ruling and the test that decides it, why
  `add_subdirectory` does not work today and what it costs to fix, why the four
  repository gates are not shipped, and fourteen falsifiable predictions about
  what a real consumer attempt will hit. Ten questions in §11 need a ruling.
```
