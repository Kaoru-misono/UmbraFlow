# Project-as-data: an inventory of what the conformance suite reaches for

Status: measurement only. Nothing here is a proposal, a ruling, or a design.
It exists so that the design correcting `provideProject` is sized from counted
evidence rather than from anyone's memory of the header.

Date: 2026-08-11
Measured at: `design/annotation-system-v2`, HEAD `6bfe1d6`. `conformance/`,
`modules/`, `docs/`, `tests/`, `entry/`, `scripts/` and `tools/` were clean at
that commit while every measurement below was taken; the only dirty paths in
the tree were `cmake/build.cmake` and `cmake/manifest.cmake`, neither of which
is cited here.

Scope: `conformance/` in this repository, the two in-tree exemplars under
`conformance/exemplars/`, and the one real consumer at
`E:\umbraflow-projects\uf-chaos` (read only; nothing in that tree was
modified).

Every claim below carries `file:line`. Where a fact could not be established
it says **not established** and names what was tried. Where a number here
disagrees with a number a document in this tree states, both are given and the
measured one is identified.

---

## 0. The counts, up front

| Class | Leaf members | What it means |
|---|---:|---|
| **Data** | **23** | the suite only reads bytes, strings or numbers out of it |
| **C++ construction** | **5** | an object whose only mint takes a `std::function` the deployment must implement |
| **Live behaviour** | **2** | the project must run code *during* the suite and leave bytes behind |
| **Total leaf members** | **30** | |

`ProvidedProject` has 12 top-level members
(`conformance/include/conformance/provider.hpp:166-205`). Counting leaves —
`vocabulary` expanded to its 17 fields, `runtimeArtifact` to `{model, assets}`,
`probeFrame` to `{fingerprint, png}` — gives 30. Classification is at the leaf,
because that is the granularity a directory loader would have to satisfy.

Those 30 leaves are reached **106 times** across the four
`conformance/source/*.cpp` files, plus further leaf reads inside the exported
fixture headers (§A.4).

Requirement IDs closed by a gate inside the conformance suite: **7** —
C-01, C-06, C-09, C-10, C-11, C-12, C-13 (§D.4).

### Three findings that change the sizing

**1. Neither in-tree exemplar has a real schema anywhere.** Both pin their five
authorities to `sha256` of a made-up word — `hashOf("catalogue")`,
`hashOf("state")`, `"{salt}/reconcile.manifest"` — and both substitute a
hardcoded allowlist of literal byte strings for the canonical-JSON validator.
Only uf-chaos validates anything. **The two exemplars will pass a data-driven
loader that never parses a byte** (§B.2, §C.6).

**2. The eight suite cases that close no requirement are the ones most
entangled with C++ construction.** All four `suite-project-authority.cpp`
cases — the ones that exist to prove the five authorities cannot be forged —
and both cases that read `lastReduceInput`/`lastDeriveInput` carry prose names
and run only under an aggregate. The seven ID-bearing cases are ledger
behaviour that merely needs *a* project (§D.5).

**3. The framework already validates JSON Schema Draft 2020-12 — in Python.**
`tools/annotate/contracts.py:10` imports `jsonschema.Draft202012Validator`,
the same draft uf-chaos hand-wrote 828 lines of C++ for. Beside it sit four
hand-rolled positional JSON readers in `modules/` and `entry/`, and — in the
consumer's Luau — three byte-identical copies of one JSON parser (§C.5).

**4. The seam the design needs already exists, and the plugin half already runs
through it.** `ProjectSchemaOwner::create` names the two validators as
"trusted deployment code" (`project-plugin.hpp:90-94`); `registerPlugin` takes
plugin *bytes* (`:232-235`) which become a `PureDataProgram` — string in,
string out, fresh quota-bound VM per call
(`modules/script/source/script/pure-data-program.hpp:13-18`). Only the five
authorities still require a compiler. And no shipped binary has ever assembled
any of it: **zero** references to the Operator's authorities, the registrar, or
`OperatorCoordinator` anywhere under `entry/` (§A.1.1).

---

## A. Every reach into a project-supplied C++ object

### A.1 Reach counts per member

Measured with `rg -o "\.<member>\b" conformance/source/*.cpp`.

The last column answers the sharper question: not "could a loader produce
this", but **is there any implementation anywhere that builds it from committed
project bytes rather than from a C++ literal?** Derived from §B.1.

| Member | Class | Reach sites in `conformance/source/*.cpp` | Built from committed project bytes by |
|---|---|---:|---|
| `registration` | C++ construction | 23 | uf-chaos only |
| `schemaOwner` | C++ construction | 6 | uf-chaos only |
| `journalSchemaOwner` | C++ construction | 1 | uf-chaos only |
| `toolCatalogSchemaOwner` | C++ construction | 3 | uf-chaos only |
| `reconcileSchemaOwner` | C++ construction | 1 | uf-chaos only |
| `pluginBytes` | Data | 5 | **nobody** — a C++ literal in all three |
| `artifactBlobs` | Data | 5 | **nobody** — literal in two, empty in umbraflow |
| `runtimeArtifact` | Data (2 leaves) | 1 | uf-chaos only |
| `probeFrame` | Data (2 leaves) | 3 | uf-chaos only |
| `lastReduceInput` | Live behaviour | 3 | n/a — not material |
| `lastDeriveInput` | Live behaviour | 3 | n/a — not material |
| `vocabulary` | Data (17 leaves) | 52 | **nobody** — a C++ literal in all three |
| **Total** | | **106** | |

The asymmetry is the first thing worth stating plainly: **five members carry
the entire C++-construction burden and 34 of the 106 reaches (32 %); the
seventeen `vocabulary` fields carry 52 reaches (49 %) and require no C++ at
all.**

And the last column says something the design has to plan around: **for 9 of
the 12 members, exactly one implementation in existence — the real consumer —
builds them from real material. For the other 3, nobody does.** A loader
specified against the in-tree exemplars would be specified against literals.

### A.1.1 Every assembly point today is a fixture, an exemplar, or the consumer

There are exactly three places in existence where a `ProvidedProject` is
assembled:

| Assembly point | Kind | Site |
|---|---|---|
| `conformance/exemplars/umbraflow/provider.cpp:80-93` | fixture adapter — the aggregate init is filled from `test_support::makeProject` in `project-fixture.hpp:342-…`, which is also the Operator's own test fixture | in-tree |
| `conformance/exemplars/arcana-expedition/provider.cpp:1028-1042` | exemplar | in-tree |
| `E:\umbraflow-projects\uf-chaos\contract\provider.cpp:1439-1452` | the real consumer | out-of-tree |

**No shipped binary assembles one, and no shipped binary reaches the Operator
at all.** Grepping `entry/` for `ProjectPluginRegistrar`, `ProjectSchemaOwner`,
`ProjectJournalSchemaOwner`, `ProjectToolCatalogSchemaOwner`,
`ProjectReconcileSchemaOwner`, `VerifiedProjectRegistration`,
`ProjectRegistration::verifyExact` and `OperatorCoordinator` returns **zero
hits across all 24 source files under `entry/cli/`**.

`ProjectPluginRegistrar::registerPlugin` confirms it independently. Measured
call sites (declaration `modules/operator/source/operator/project-plugin.hpp:232`,
definition `project-plugin.cpp`):

| Location | Count |
|---|---:|
| `tests/operator/test-project-plugin-contract.cpp` | 27 |
| `conformance/source/suite-project-authority.cpp` | 4 |
| `conformance/source/suite-support.cpp` | 1 |
| `conformance/exemplars/umbraflow/project-fixture.hpp` | 1 |
| **`entry/`** | **0** |

The coordinator's count is exact: 27 in the plugin contract test, 6 across
`conformance/`, zero in `entry/`.

**This is the load-bearing consequence.** `registerPlugin` already takes
plugin *bytes* rather than a plugin object
(`project-plugin.hpp:232-235`: `std::string exactPluginBytes`,
`std::vector<ArtifactBlob> exactArtifactBlobs`), and the thing those bytes
become is `script::PureDataProgram` — `modules/script/source/script/pure-data-program.hpp`,
whose header states the shape: compilation and exact export validation happen
once in `compile()`; `invoke()` loads those bytes into a fresh quota-bound VM
and passes one decoded JSON value in and one decoded JSON value out; a frozen
`artifact.read(name)` reader answers with the decoded, frozen value of a
registered artifact, built at most once per VM; no host installer or native
capability seam is part of this API. Its two entry points are
`compile(moduleId, source, entryPoints, artifacts)` and
`invoke(entryPoint, immutableInput) -> Result<json::Value>`.

> **Restated 2026-08-12.** This paragraph quoted a header that said "one
> immutable string in and one immutable string out" and that
> `artifact.read` "lazily copies only registered immutable blobs". Both halves
> moved to a value — the call boundary first, the artifact reader after — so the
> quotation is paraphrased above rather than reproduced, and the `file:line`
> ranges are dropped because they were the drifting part.

So **the plugin half of a project is already data-shaped end to end** —
bytes in, value in, value out — and has never been assembled anywhere but a
fixture or a provider. The five authorities are the only half that is not.

### A.2 The five C++-construction members, with the exact mechanism

For each of the five, the blocking mechanism is **not** primarily the private
constructor. It is that the *only public mint* takes a `std::function` the
deployment must implement. The private constructor and `friend` declaration
stop a caller from fabricating the value; the `std::function` parameter is what
makes the caller a compiler.

Both are quoted below.

#### 1. `VerifiedProjectRegistration` — `ProvidedProject::registration`

Private constructor and friend, `modules/operator/source/operator/manifest.hpp:80-86`:

```cpp
        VerifiedProjectRegistration(
            ProjectRegistrationClaims claims,
            std::string canonicalJcs,
            ContentHash rootHash
        );

        friend class ProjectRegistration;
```

Sole mint, `manifest.hpp:117-126`:

```cpp
    class ProjectRegistration final
    {
    public:
        [[nodiscard]]
        static auto verifyExact(
            std::string canonicalJcs,
            ContentHash expectedRootHash,
            ProjectRegistrationSchemaOwner const& schemaOwner
        ) -> Result<VerifiedProjectRegistration>;
    };
```

`verifyExact` requires a `ProjectRegistrationSchemaOwner`, whose own private
constructor is at `manifest.hpp:52-57` (`friend class ProjectRegistration;`)
and whose public `create` at `manifest.hpp:64-68` takes:

```cpp
    using ProjectRegistrationExactValidator = std::function<
        Result<ProjectRegistrationClaims>(std::string_view exactJcs)
    >;
```
(`manifest.hpp:43-45`)

The header states the obligation on that callable at `manifest.hpp:39-42`: it
"must parse the complete registration, validate it against the owner's exact
JSON Schema, and reject bytes that are not the exact RFC 8785 JCS
serialization." **That sentence is the whole of §C's problem: it makes a JSON
Schema validator a precondition of holding a registration.**

#### 2. `ProjectSchemaOwner` — `ProvidedProject::schemaOwner`

Private constructor and friend, `modules/operator/source/operator/project-plugin.hpp:118-120`:

```cpp
        explicit ProjectSchemaOwner(std::shared_ptr<State const> p_state) noexcept;

        friend class ProjectPluginHandle;
```

Public mint, `project-plugin.hpp:138-142`:

```cpp
        static auto create(VerifiedProjectRegistration const& registration,
                           ProjectDocumentSchemaBytes const& exactSchemas,
                           CanonicalJsonValidator validateCanonicalJson,
                           ProjectDocumentValidator validateDocument) -> Result<ProjectSchemaOwner>;
```

Two `std::function`s, `project-plugin.hpp:95-98`:

```cpp
    using CanonicalJsonValidator = std::function<Status(std::string_view exactJcs)>;
    using ProjectDocumentValidator = std::function<Status(ProjectPluginFunction function,
                                                          ProjectDocumentDirection direction,
                                                          std::string_view exactJcs)>;
```

This is the only one of the five that carries **two** callables, and it is also
the one the live-behaviour members hang off (§A.3).

Note that the values this owner stamps are separately fenced: `CanonicalJson`
has a private constructor and `friend class ProjectSchemaOwner;`
(`project-plugin.hpp:46-48`), as does `ValidatedDocument`
(`project-plugin.hpp:68-73`).

#### 3. `ProjectJournalSchemaOwner` — `ProvidedProject::journalSchemaOwner`

Private constructor, `modules/operator/source/operator/journal-entry.hpp:77-80`.
The `friend` is on the value it mints, `journal-entry.hpp:21-23`:

```cpp
    class ValidatedJournalEntryData final
    {
        friend class ProjectJournalSchemaOwner;
```

Public mint, `journal-entry.hpp:87-92`, taking
`JournalPayloadSchemaValidator` — `journal-entry.hpp:65-70`:

```cpp
    using JournalPayloadSchemaValidator = std::function<
        Result<ContentHash>(
            std::string_view namespacedEventType,
            std::string_view exactPayloadJcs
        )
    >;
```

The callable must "select the complete schema by namespaced event type,
validate the payload, and return that schema's real content hash"
(`journal-entry.hpp:55-57`). A data-only project can supply the
`{eventType → schemaPath}` table and the schema bytes; it cannot supply the
validation.

#### 4. `ProjectToolCatalogSchemaOwner` — `ProvidedProject::toolCatalogSchemaOwner`

Private constructor, `modules/operator/source/operator/tool-invocation.hpp:143-147`.
Friend on the minted value, `tool-invocation.hpp:74-76`:

```cpp
    class ValidatedToolInvocation final
    {
        friend class ProjectToolCatalogSchemaOwner;
```

Public mint, `tool-invocation.hpp:154-159`, taking `ToolCatalogValidator` —
`tool-invocation.hpp:130-135`:

```cpp
    using ToolCatalogValidator = std::function<
        Result<ToolDescriptor>(
            std::string_view toolName,
            std::string_view exactArgsJcs
        )
    >;
```

#### 5. `ProjectReconcileSchemaOwner` — `ProvidedProject::reconcileSchemaOwner`

Private constructor, `modules/operator/source/operator/reconcile-outcome.hpp:87-91`.
Friend on the minted value, `reconcile-outcome.hpp:36-38`:

```cpp
    class ValidatedReconcileOutcome final
    {
        friend class ProjectReconcileSchemaOwner;
```

Public mint, `reconcile-outcome.hpp:96-101`, taking
`ReconcileDispositionReader` — `reconcile-outcome.hpp:77-79`:

```cpp
    using ReconcileDispositionReader = std::function<
        Result<ReconcileDisposition>(std::string_view exactJcs)
    >;
```

This one is the cheapest of the five to make declarative: the callable's whole
job is a `{document → one of five enumerators}` mapping. uf-chaos implements it
as a five-branch string compare over the first member of the object
(`E:\umbraflow-projects\uf-chaos\contract\provider.cpp:1191-1230`).

#### The sixth mint the suite reaches, not on `ProvidedProject`

`OperatorPlanAuthority::create` is reached through the suite's own
`planAuthority(...)` helper
(`conformance/include/conformance/operator-protocol.hpp:37-74`), and it too
takes callables — but no project supplies either. Both are the deployment's:
`deployment::readPlanProposal` at `:51`, and `deployment::readStepIntent` inside
a lambda closing over `uiAction` at `:52-72`, which adds the suite's one refusal
of its own. **The project supplies only the `UiActionUnderTest` value.** This
is the existing proof that the pattern can be inverted: the framework already
owns one of the six validators outright, and takes data for the part that is
the project's.

### A.3 The two live-behaviour members

`lastReduceInput` and `lastDeriveInput` are
`std::shared_ptr<std::string>` (`provider.hpp:196`, `provider.hpp:202`). The
header states the contract at `provider.hpp:193-202`: "Where the deployment's
document validator records the exact bytes it last saw as a Reduce input.
Shared and mutable because the validator writes it and the suite reads it."

**Who writes them, exactly.** In all three implementations the writer is the
`ProjectDocumentValidator` lambda handed to `ProjectSchemaOwner::create`, which
captures the two `shared_ptr`s and assigns through them on the `Input`
direction of the `Reduce` and `Derive` cases:

- `conformance/exemplars/umbraflow/project-fixture.hpp:461-481` — the lambda
  captures `[lastReduceInput, lastDeriveInput]` at `:461`, writes
  `*lastReduceInput = std::string{candidateJcs};` at `:471` under
  `case ProjectPluginFunction::Reduce:` and
  `*lastDeriveInput = ...` at `:479` under `case ProjectPluginFunction::Derive:`.
  The pair is allocated at `:411-412` and returned in `ProjectFixture` at
  `:757-758`.
- `E:\umbraflow-projects\uf-chaos\contract\provider.cpp:1053-1083` — same
  shape: `documentValidator(deployment, lastReduceInput, lastDeriveInput)` at
  `:1054-1058`, captured at `:1060-1062`,
  `*lastReduceInput = std::string{candidateJcs};` at `:1074`,
  `*lastDeriveInput = std::string{candidateJcs};` at `:1082`. Allocated at
  `:1397-1398`, passed at `:1408`, returned at `:1449-1450`.
- `conformance/exemplars/arcana-expedition/provider.cpp:742-767` — identical
  shape, writes at `:759` and `:767`, returned at `:1038-1039`.

**When they are written.** Not at provider time. They are written as a side
effect of the Operator calling into the plugin during the run: the Operator
assembles the reduce and derive envelopes itself and calls
`plugin.canonicalize(...)` / the plugin function, which routes through the
project's `ProjectDocumentValidator`. The suite then reads the recorded string.
Reads are at `conformance/source/suite-control-ledger.cpp:503,508,528`
(`lastReduceInput`) and `:540,542,578` (`lastDeriveInput`).

**Why this class is not "Data".** The value is not something the project
*knows*; it is something the project *observes* while the suite runs. A
directory of data has nowhere to put it. Two cases depend on it and only on it:
`"the reducer is handed exactly the Journal prefix that is appended"`
(`suite-control-ledger.cpp:498-534`) and
`"the deriver is handed an envelope no caller could have supplied"`
(`suite-control-ledger.cpp:536-591`).

**The property is the framework's, not the project's.** Both cases assert
things about what the *Operator* assembled — member order in the derive
envelope (`:548-561`), `"prior_project_observation":null` versus a document
(`:565`, `:584`), that a non-committed entry never reaches the reducer
(`:531-532`). Nothing about the recorded bytes is a project fact. The only
reason the project is in the loop is that the project's validator happens to be
the last code that sees the bytes before the plugin does.

### A.4 Where each Data leaf is actually read

The top-level reach sites in `conformance/source/` frequently pass a member
into a fixture header, which reads the leaf. For the design's sizing, these are
the reads that matter:

| Leaf | Passed at | Leaf read at |
|---|---|---|
| `runtimeArtifact.model` | `suite-support.cpp:175` | `observation-fixture.hpp:200`, then `publishRuntimeArtifact` writes it to disk at `:146` |
| `runtimeArtifact.assets` | `suite-support.cpp:175` | `observation-fixture.hpp:201`, sorted at `:145`, written file-by-file at `:151` |
| `probeFrame.png` | `suite-support.cpp:171,229,287` | decoded at `observation-fixture.hpp:235` and `:274` |
| `probeFrame.fingerprint` | same | `observation-fixture.hpp:239-240`, `:250-252`, and as both `liveFingerprint` and `projectFingerprint` at `:492-493` |
| `vocabulary.uiAction` | `suite-support.cpp:237,260,286` | `operator-protocol.hpp:703-705`; stored in `DeliveringHost::m_action` (`observation-fixture.hpp:650`) |

**`runtimeArtifact` is already a directory.** `publishRuntimeArtifact`
(`observation-fixture.hpp:139-178`) takes the model string and the asset vector
and *writes them back out as files* into a temporary root, then hashes the
manifest it generated. The suite's very first act with this member is to undo
the in-memory representation. A directory-shaped project would hand it a
directory and delete this function's reason to exist.

`probeFrame.fingerprint` is four `uint32`s behind a public
`ProjectFingerprint::create(width, height, dpiX, dpiY)`
(`modules/domain/source/domain/space.hpp:315-320`; private constructor at
`:298-303`). A loader reading four numbers out of a manifest can build it.

### A.5 A second struct with the same five authorities

`test_support::ProjectFixture`
(`conformance/exemplars/umbraflow/project-fixture.hpp:49-63`) declares the same
five authority members plus the same two `shared_ptr<std::string>` recorders.
It is not `ProvidedProject`: the framework's own `tests/operator/*` reach the
authorities through it (`tests/operator/test-state-contract.cpp:207,213` and
`tests/operator/test-ledger.cpp:1340,1357` read `prepared.project.last*Input`
off *this* struct, via the `PreparedStore` at `project-fixture.hpp:1351-1355`).

Any design that changes how a project is acquired has two structs to account
for, not one, and `conformance/exemplars/umbraflow/provider.cpp:80-93` is the
adapter that converts between them.

---

## B. What the two in-tree exemplars and the real consumer disagree about

Three implementations of one interface:

- `conformance/exemplars/umbraflow/provider.cpp` (95 lines) +
  `conformance/exemplars/umbraflow/project-fixture.hpp` (1804 lines)
- `conformance/exemplars/arcana-expedition/provider.cpp` (1063 lines)
- `E:\umbraflow-projects\uf-chaos\contract\provider.cpp` (1462 lines) +
  `contract/json-value.cpp` (719) + `contract/json-schema.cpp` (828) +
  `contract/CMakeLists.txt` (269)

> **Count disagreement.** `docs/plans/2026-08-11-consumer-onboarding.md:29`
> states arcana's `provider.cpp` is **880 lines** and `:131` states
> `project-fixture.hpp` is **1532**, and `:45` says the line spans "sum to
> 880". That document dates its measurement explicitly: "Every file-level claim
> was read at `c23efd3`" (`:4`). **Measured at `6bfe1d6` today: 1063 and
> 1804.** The same doc's header inventory at `:36-37` (`provider.hpp` 140,
> `operator-protocol.hpp` 649, `observation-fixture.hpp` 777,
> `host-delivery-fixture.hpp` 139) measures today as **221, 716, 821, 195**.
> The document is not wrong so much as pinned; anything sized from its
> percentages needs re-deriving. The 880-line total and the 213/360/208/58/41
> split are the numbers a design would otherwise inherit.

### B.1 Per-member: file, literal, or generated at configure time?

`file` = the bytes exist as a committed file and reach C++ as such.
`generated` = a committed file crosses into C++ through a configure-time
generated header. `literal` = the bytes exist only as a C++ literal.
`computed` = built in C++ from other members.

| Member | umbraflow exemplar | arcana exemplar | uf-chaos |
|---|---|---|---|
| `registration` (bytes) | **literal** — `std::format` template at `project-fixture.hpp:355-374` | **literal** — `registrationBytes`, `provider.cpp:621` | **computed from generated** — `provider.cpp:800-829`, every hash taken over embedded schema bytes |
| registration *schema* | **fake** — `hashOf("registration-schema")`, `project-fixture.hpp:347`; the validator is `candidate != exactJcs` (`:387-402`) | **fake** — `"{salt}/registration.schema"`, `provider.cpp:607` | **generated** — real JSON Schema, `schema/registration-v1.schema.json`, embedded by `contract/CMakeLists.txt:24`, compiled at `provider.cpp:917` |
| `schemaOwner` schema bytes | **fake** — the literal words `"state"`, `"observation"`, `"precondition"` (`project-fixture.hpp:415-419`) | **fake** — `"{salt}/project-state.schema"` etc. (`provider.cpp:603-615`) | **generated** — three real schemas via `schemaBytes(paths.*)` (`provider.cpp:1402-1406`, paths at `:780-792`) |
| `journalSchemaOwner` bytes | **fake** — `hashOf("journal")` (`project-fixture.hpp:354`) | **fake** — `"{salt}/journal.manifest"` (`provider.cpp:613`) | **generated** — `dream/journal-manifest-v1.json` + 13 per-event payload schemas (`provider.cpp:1414`, loader at `:992-1016`) |
| `toolCatalogSchemaOwner` bytes | **fake** — `hashOf("catalogue")` (`:349`) | **fake** — `"{salt}/tool-catalog.json"` (`provider.cpp:611`) | **generated** — `dream/tool-catalog-v1.json` (`provider.cpp:1421`, parsed at `:961-990`) |
| `reconcileSchemaOwner` bytes | **fake** — `hashOf("reconcile")` (`:353`) | **fake** — `"{salt}/reconcile.manifest"` (`provider.cpp:612`) | **generated** — `dream/reconcile-v1.schema.json` (`provider.cpp:1428`) |
| `pluginBytes` | **computed literal** — `test_support::pluginSource(pluginId)` builds the source around the id | **literal ×2** — `k_expeditionPlugin` (`provider.cpp:61`), `k_rivalPlugin` (`:114`) | **literal ×2** — `k_dreamPlugin` (`provider.cpp:142`), `k_archivePlugin` (`:217`) |
| `artifactBlobs` | **empty** — `.artifactBlobs = {}` (`provider.cpp:87`) | **literal** — one blob, `provider.cpp:1022-1023` | **literal** — `.artifactBytes` in the definition (`provider.cpp:1244`, `:1321`), wrapped at `:1433-1437` |
| `runtimeArtifact.model` | **literal TOML** — `umbraflowRuntimeModel()` raw string ending `project-fixture.hpp:1022` | **literal TOML** — `k_runtimeModel` (`provider.cpp:209`), used at `:584` | **generated from file** — `runtime/artifact/page-model.toml` read at `contract/CMakeLists.txt:171`, emitted as `k_runtimeModel` at `:224`, used at `provider.cpp:455` |
| `runtimeArtifact.assets` | **computed** — 2 PNGs synthesized by `image::encodeRgbaPng` (`project-fixture.hpp:1062-1077`, `:1082-1094`) | **computed** — 2 PNGs cropped from the composed probe (`provider.cpp:572-587`) | **generated from files** — 9 committed PNGs listed at `contract/CMakeLists.txt:121-131`, hex-embedded by `chaos_append_bytes` (`:139-169`), verified against the committed manifest at `provider.cpp:415-459` |
| `probeFrame.png` | **computed** — 3-pixel image, `project-fixture.hpp:1141-1145` | **computed** — 1600×900 canvas synthesized then patched, `provider.cpp:530-551` | **generated from file** — `runtime/probe-frame.png`, embedded at `contract/CMakeLists.txt:230-233`, used at `provider.cpp:477` |
| `probeFrame.fingerprint` | **literal** — `ProjectFingerprint::create(3, 1, 96, 96)` (`project-fixture.hpp:1108`) | **literal** — `k_frameWidth/Height/Dpi` (`provider.cpp:173-175`), used at `:556-561` | **derived from the model at configure time** — regex over `page-model.toml` at `contract/CMakeLists.txt:185-194`, emitted as `k_baseWidth/Height/DpiX/DpiY` at `:219-222`, used at `provider.cpp:469-472` |
| `vocabulary` (17 fields) | **literal** — `vocabulary()` at `provider.cpp:27-64` | **literal** — `vocabularyOf` (`provider.cpp:430`) | **literal** — `dreamDefinition()` / `archiveDefinition()` (`provider.cpp:1237-1372`) |
| `lastReduceInput`/`lastDeriveInput` | live | live | live |

### B.2 What that table says

**1. Only one of the three implementations has real schemas at all.** Both
in-tree exemplars pin their authorities to the hash of a *made-up word*. The
umbraflow fixture's registration validator is literally
`if (candidate != exactJcs) return fail(...)` (`project-fixture.hpp:393-399`) —
a string equality test standing in for "parse, schema-validate, and verify JCS".
Its `ProjectDocumentSchemaBytes` are the three words `"state"`,
`"observation"`, `"precondition"` (`:415-419`). Its canonical validator is a
15-entry allowlist of exact byte strings (`:422-438`).

The consequence for the correction: **the two exemplars cannot be used to
estimate what a real project's data directory must contain, and cannot be
used to test a schema-driven loader at all.** They will pass a loader that
never parses anything.

**2. Everything uf-chaos does at configure time is a file→C++ bridge, and it
is the complete inventory of what should have been a directory.**
`contract/CMakeLists.txt` embeds:

- **26 schema files** — `CHAOS_SCHEMA_FILES` at `:23-50`; independently, 26
  files exist under `E:\umbraflow-projects\uf-chaos\schema\` (measured). The
  two agree exactly, and the list is deliberately explicit rather than a glob
  (`:19-21`).
- **1 model file** — `runtime/artifact/page-model.toml`, read at `:171`,
  emitted at `:224`.
- **1 manifest file** — `runtime/artifact/runtime-artifact.manifest.json`,
  read at `:172`, emitted at `:226`.
- **10 binary blobs** — 9 artifact PNGs (`:121-131`) plus
  `runtime/probe-frame.png` (`:230-233`), each hex-expanded into a
  `std::array<uint8, N>` by `chaos_append_bytes` (`:139-169`).
- **4 integers** derived by regex from the model (`:185-194`).

**38 files and 4 numbers cross the file→C++ boundary for one reason: the suite
takes C++ objects.** The CMake file says so itself at `:14-17` — "They are
embedded at configure time rather than read at run time: a suite that opened
files would pass or fail on where it was started from." That justification is
about the *suite* having no defined root, not about anything the project needs.
Give the suite a project root and the entire apparatus — the two generated
headers, the hex expander, the regex, the `FATAL_ERROR` guards, the
`CMAKE_CONFIGURE_DEPENDS` bookkeeping — is deleted rather than ported.

**3. What is only ever a literal, in all three.** `pluginBytes` and all 17
`vocabulary` fields. uf-chaos keeps its Luau plugin as a C++ raw string
literal (`provider.cpp:142`, `:217`) even though it has a working file-embedding
mechanism sitting in the same CMakeLists — nothing forces this, and there are
no `.luau` files under `uf-chaos/contract/` (measured: the tree's `.luau` files
are all under `map/`, `tasks/`, `tools/`). The vocabulary is the one block the
onboarding document already identified as irreducibly the project's
(`docs/plans/2026-08-11-consumer-onboarding.md:118-121`), and this measurement
agrees: 17 fields, 52 reaches, zero C++ construction.

**4. The three disagree about `artifactBlobs` to the point of one supplying
none.** umbraflow passes `{}` (`provider.cpp:87`); arcana and uf-chaos each
pass exactly one. A loader spec that requires the member cannot be validated
against the in-tree exemplar that omits it.

---

## C. The JSON Schema validator that must move upstream

### C.0 The seam already exists, and only one of three implementations honours it

`ProjectSchemaOwner::create` takes the two callables
(`modules/operator/source/operator/project-plugin.hpp:138-142`), and the
comment directly above them assigns the obligation
(`project-plugin.hpp:90-94`):

```cpp
    // These validators are trusted deployment code. The canonical validator
    // must reject anything other than exact RFC 8785 JCS. The document validator
    // must validate the complete function-specific JSON Schema, including every
    // project-owned nested payload. Neither callable is passed to plugin code or
    // published in a business VM.
```

**What each of the three implementations actually passes — measured, and the
answer is red:**

| | `CanonicalJsonValidator` — "reject anything other than exact RFC 8785 JCS" | `ProjectDocumentValidator` — "validate the complete function-specific JSON Schema" |
|---|---|---|
| **umbraflow** (`project-fixture.hpp:413-…`) | **allowlist of 15 exact byte strings** (`:420-460`): `std::ranges::find(accepted, candidateJcs)`, plus `k_fixtureProvenance`, a violations list, and five `looksLike*` shape recognizers. Parses nothing. | **`function × direction` switch over substring tests** (`:461-520`): `looksLikeReduceEnvelope`, `candidateJcs.starts_with("{\"disposition\":\"")`, `candidateJcs == "{\"revision\":0\}"`. No schema. |
| **arcana** (`provider.cpp:701-738`) | **allowlist of 17 exact byte strings** (`:705-723`) plus five shape recognizers (`:726-731`). Parses nothing. | **same switch shape** (`:741-806`): `candidateJcs == k_projectState` (`:783`), `starts_with("{\"verdict\":\"")` (`:786`), `candidateJcs == k_visible` (`:790`). No schema. |
| **uf-chaos** (`contract/provider.cpp:1040-1051`) | **real** — `chaos::requireExactCanonicalJson(candidateJcs)` (`:1049`), i.e. parse, re-serialize, compare bytes (`json-value.cpp:705-718`). Comment at `:1042-1046`: "It recognizes no shape, so a document nobody anticipated reaches the schema that judges it instead of being refused for being unfamiliar." | **real** — `chaos::parseJson` then a compiled `JsonSchema` per function/direction (`:1054-1122`): `validateReduceInput`, `deployment->reconcileSchema.validateDefinition("ReconcileRequest", …)`, `deployment->projectStateSchema.validate(document)`, `observationSchema.validate(document)`. |

**The framework's own conformance runs have never exercised the validation the
header demands.** Both in-tree exemplars substitute exact-byte allowlists and
`starts_with`/`==` substring tests for "exact RFC 8785 JCS" and "the complete
function-specific JSON Schema". Only the real consumer, out of tree, implements
either.

Three consequences the design must carry:

1. **The exemplars cannot falsify a validator.** A shipped validator that
   accepted non-canonical bytes, or that ignored a schema keyword, would leave
   `conformance-umbraflow` and `conformance-arcana` green. This is precisely
   the defect `docs/pitfalls/checks-that-cannot-fail.md` names, sitting in the
   two artifacts a consumer is told to copy.
2. **The allowlists are why.** An exemplar that enumerates the 15 or 17 byte
   strings its own run will produce cannot be re-pointed at a different project;
   it encodes the run, not the contract. That is also why they can never be
   promoted upstream as a default.
3. **uf-chaos's 1547 lines are the framework's specification, written in the
   wrong repository.** They are the only existing implementation of the
   paragraph quoted above.

**Nothing upstream implements an RFC 8785 canonical-JSON validator.** Grepping
`modules/`, `entry/`, `tests/` and `conformance/` for `CanonicalJsonValidator`
returns five hits and **not one is an implementation**: the alias
(`project-plugin.hpp:95`), the `create` parameter (`:141`), the stored member
(`project-plugin.cpp:134`), the `create` parameter again (`:202`), and arcana's
allowlist (`provider.cpp:701`). `core/text/json-text.hpp` is emit-only (§C.4).
**The framework defines the obligation and ships nothing that meets it.**

### C.1 What uf-chaos's evaluator implements

`E:\umbraflow-projects\uf-chaos\contract\json-schema.hpp:3-4` states the target:
"A JSON Schema Draft 2020-12 evaluator, scoped to the keywords this
repository's own schemas use."

**There is no draft dispatch.** `$schema` sits in the plain-keyword allowlist
(`json-schema.cpp:58`) and no code reads its value; a schema declaring draft-07
would compile identically. **Not established** that this matters in practice —
all 22 schema documents declare 2020-12 (measured, §C.2).

**The load-bearing property is refusal, not coverage.**
`json-schema.cpp:217-228` makes any member name at a schema position that is
not in one of four allowlists a **compile-time refusal**. The header states the
reason at `:8-11`: "a misspelled keyword is a build-time refusal here, not a
hole that opens years later." So the implemented set is exactly the union of
those four arrays — **41 accepted names**:

| Array | Site | Members |
|---|---|---|
| `k_subschemaKeywords` (8) | `json-schema.cpp:25-34` | `additionalProperties`, `contains`, `else`, `if`, `items`, `not`, `propertyNames`, `then` |
| `k_subschemaMapKeywords` (3) | `:37-41` | `$defs`, `patternProperties`, `properties` |
| `k_subschemaListKeywords` (4) | `:44-49` | `allOf`, `anyOf`, `oneOf`, `prefixItems` |
| `k_plainKeywords` (26) | `:54-81` | `$comment`, `$id`, `$ref`, `$schema`, `const`, `default`, `deprecated`, `description`, `enum`, `examples`, `exclusiveMaximum`, `exclusiveMinimum`, `maxItems`, `maxLength`, `maxProperties`, `maximum`, `minItems`, `minLength`, `minProperties`, `minimum`, `multipleOf`, `pattern`, `required`, `title`, `type`, `uniqueItems` |

Of those 41, **9 are inert** — never read by any evaluation branch:
`$comment`, `$id`, `$schema`, `default`, `deprecated`, `description`,
`examples`, `title`, and `$defs` (a container, reached only by
`hasDefinition` `:804-808` / `validateDefinition` `:810-827`).

Evaluation sites, for sizing an upstream port:

| Keyword | Site |
|---|---|
| `type` | `:381-399`, type-name map `:245-276` |
| `const` / `enum` | `:400-406` / `:407-418` |
| `minLength` / `maxLength` | `:423-428` / `:429-434`; code points via `codePointCount` `:279-290` |
| `pattern` | `:435-446`; `std::regex` **ECMAScript**, `regex_search`, compiled by `matchesPattern` `:293-312`; uncompilable pattern refuses at `:306-310` |
| `minimum` / `maximum` / `exclusive*` / `multipleOf` | `:452-480` |
| `required` / `minProperties` / `maxProperties` | `:492-506` / `:507-520` |
| `properties` / `patternProperties` / `additionalProperties` / `propertyNames` | `:539-553` / `:554-574` / `:575-596` / `:534-538` |
| `minItems` / `maxItems` / `uniqueItems` / `prefixItems` / `items` / `contains` | `:609-676` (`uniqueItems` is O(n²), `:621-634`) |
| `allOf` / `anyOf` / `oneOf` / `not` / `if`-`then`-`else` | `:687-742` |
| `$ref` | compile-time resolution `:229-239`, evaluation `:766-770`, pointer walk `:118-162` |
| boolean schema, depth guard (128) | `:757-764`, `:22` + `:753-756` |

`$ref` is **same-document-only by construction**: `:125-131` refuses anything
not starting `#/`, with the reason at `:115-116` — a cross-document `$ref`
would take validation outside the bytes the Operator authority pinned. That
constraint is a framework property, not a project preference, and any upstream
port must keep it.

**Draft 2020-12 keywords absent** (a compile refusal if used): `format`,
`dependentRequired`, `dependentSchemas`, `unevaluatedItems`,
`unevaluatedProperties`, `$anchor`, `$dynamicRef`, `$dynamicAnchor`,
`$vocabulary`, `minContains`, `maxContains`, `contentEncoding`,
`contentMediaType`, `contentSchema`, `readOnly`, `writeOnly`, and the draft-07
spellings `definitions` / `additionalItems`.

**Three soft spots worth carrying into a port** (found by reading, not by a
failing test): `:390-393` reads `type: [...]` entries with `option.string()`
and no kind check — a non-string yields the empty string
(`json-value.cpp:629-632`) and silently matches nothing rather than refusing;
same shape at `:495` (`required`) and `:409-412` (`enum`). And `:621-622`
enforces `uniqueItems` only when `boolean()` is true, so a non-boolean value
reads as `false` (`json-value.cpp:625`) and skips the check.

### C.2 The parser under it

`contract/json-value.cpp` `parseJson` (`:692-703`) is a real RFC 8259 reader
producing a value tree. (`json-value.hpp:3` says "8262", which appears to be a
typo for 8259.)

Refuses: duplicate member names (`:368-374`), trailing content after the root
(`:698-701`), depth beyond 64 (`:26`, `:401-404`), non-finite and out-of-range
numbers (`:300-307`), leading zeros / missing integer part (`:252-266`), empty
fraction (`:267-278`), empty exponent (`:279-294`), unescaped control bytes
(`:172-175`), unknown escapes (`:231`, allowlist `:191-198`), lone or
unpaired surrogates (`:202-222`), and any string that is not valid UTF-8 after
escape resolution (`:234-237`, via the framework's `core/text/utf8.hpp`).
No comments, no trailing commas, no `NaN`/`Infinity`.

Numbers are `double` only: `isInteger()` (`:638-642`) is "finite and equal to
its truncation", so `1e100` satisfies `"type": "integer"`.

### C.3 Which of uf-chaos's schemas use unimplemented keywords

**None. Zero.** And this is structural rather than lucky: `JsonSchema::compile`
refuses unknown keywords (`json-schema.cpp:217-228`) and the provider compiles
every embedded schema at startup (`contract/provider.cpp:324-332`, with
`REQUIRE_MESSAGE` at `:330`), so an unimplemented keyword cannot reach a run.

> **Count nuance, measured.** `schema/` holds **26 `.json` files** — and the
> `CHAOS_SCHEMA_FILES` list at `contract/CMakeLists.txt:23-50` holds exactly
> those 26. But only **22 are JSON Schema documents**: 22 files match
> `*.schema.json` and 22 declare
> `"$schema": "https://json-schema.org/draft/2020-12/schema"`. The other 4 —
> `{archive,dream}/journal-manifest-v1.json` and
> `{archive,dream}/tool-catalog-v1.json` — carry no `$schema` and are pinned
> *data*, read through `parseJson` at `contract/provider.cpp:966` and `:996`,
> never through `JsonSchema::compile`. **"26 schemas" should be read as "26
> pinned contract documents, of which 22 are JSON Schemas."**

Keyword usage across those 22 documents, counted by a structural walk that
mirrors `checkKeywords` (so property *names* are not miscounted as keywords):
**30 distinct keywords, all 30 implemented.** The heaviest are `$ref` (379
occurrences), `required` (322), `type` (293), `properties` (248), `const`
(107), `additionalProperties` (98), `if`/`then` (95 each), `not` (79),
`pattern` (68), `enum` (65). A separate raw scan of every object key at any
position found **zero** occurrences of any of the 18 absent keywords listed in
§C.1 — there is no near-miss even at non-keyword positions.

**Dead capability: 11 keywords implemented and never used** — `contains`,
`patternProperties`, `prefixItems`, `propertyNames`, `exclusiveMaximum`,
`exclusiveMinimum`, `maxLength`, `multipleOf`, plus the three inert
annotations `default`, `deprecated`, `examples`. That is 8 live assertion paths
no real schema has ever driven, and (see §C.6) they have no vector suite behind
them either.

### C.4 What `core/text/json-text` already provides — and what it does not

**It does not parse. It is two RFC 8785 primitives.** The complete public API of
`modules/core/source/core/text/json-text.hpp` is two free functions:

| Function | Declaration | Definition |
|---|---|---|
| `appendJsonString(std::string& output, std::string_view value) -> void` | `json-text.hpp:20` | `json-text.cpp:38-68` |
| `jsonMemberNameLess(std::string_view left, std::string_view right) -> bool` | `json-text.hpp:41-42` | `json-text.cpp:70-91` |

One file-local helper, `utf16SortKey` (`json-text.cpp:23-35`). No types, no
value tree, no reader, no number formatter, no container walk.

`appendJsonString` emits one complete JSON string token with RFC 8785 escaping
(`:46-64`) and explicitly does **not** validate UTF-8 — the caller must have
(`json-text.hpp:11-13`). `jsonMemberNameLess` is RFC 8785 §3.2.3 UTF-16
code-unit ordering (`:84-90`) with a documented fallback that sorts invalid
UTF-8 last (`:75-82`).

The header's own reason for living in `core` (`json-text.hpp:15-19`) is the
rule this inventory keeps running into: "A second spelling of this transform
cannot fail a test — it produces bytes that merely disagree."

**Callers today.** Seven in this tree, one across the boundary:

| File | Include | Calls |
|---|---|---|
| `modules/trace/source/trace/event.cpp` | `:3` | `appendJsonString` ×11 (`:25`, `:51`, `:72`, `:74`, `:161`-`:179`); `jsonMemberNameLess` `:155` |
| `modules/operator/source/operator/ledger.cpp` | `:6` | `appendJsonString` ×11 (`:1553`-`:1764`, `:3741`, `:3747`) |
| `modules/operator/source/operator/effective-plan.cpp` | `:4` | `appendJsonString` ×11 (`:64`-`:352`) |
| `modules/operator/source/operator/manifest.cpp` | `:3` | `appendJsonString` `:27`; `jsonMemberNameLess` `:152` |
| `modules/cli/source/cli/explore-protocol.cpp` | `:6` | `appendJsonString` `:393`, `:410`, `:415`, `:420` |
| `modules/json/source/json/value.cpp` | `:6` | `appendJsonString` `:560`, `:602`; `jsonMemberNameLess` `:589` |
| `tests/core/test-json-text.cpp` | `:1` | both, `:303`-`:409`, `:372`-`:378` |
| `E:\umbraflow-projects\uf-chaos\contract\json-value.cpp` | `:3` | `appendJsonString` `:526`; `jsonMemberNameLess` `:555` |

**No overlap, and no conflict.** `json-text` supplies exactly the two halves of
canonicalization that uf-chaos does *not* re-spell — the consumer calls the
framework's functions directly. What uf-chaos adds on top (number formatting,
null/boolean spelling, the container walk) has no counterpart in `core` at all.
A JSON parser landing beside `json-text` would not be a second spelling of
anything currently there.

### C.5 What else already parses JSON — the two-spellings check

This repository forbids two spellings of one thing, so this was searched
exhaustively. The result is that **there is no C++ JSON *parser* in the
framework to collide with, but there are already four hand-rolled positional
readers, two full JSON Schema implementations across the two trees, and — in
the consumer's Luau — three byte-identical copies of one parser.**

**Framework, first-party C++ — four narrow positional readers, none producing a
value tree, none in `core`, mutually independent:**

1. `modules/operator/source/operator/journal-entry.cpp` — `JcsCursor`
   (`:92-…`), reading the fixed 4-member `JournalProvenance` shape by exact-JCS
   member order (`:193-235`). The design note at `:24-29` says the ordering
   itself is what enforces `additionalProperties: false`.
2. `modules/cli/source/cli/explore-protocol.cpp` — `LineReader` (`:40-…`), a real narrow
   reader with escapes. Its comment at `:32-39` records that it was
   deliberately *not* reused from the retired operator protocol reader, and
   that what is shared is `appendJsonString`.
3. `modules/operator/source/operator/runtime-installation.cpp` —
   `ReleaseReader` (`:66-…`), error text at `:74-79`.
4. `modules/task/source/task/runtime-model-file.cpp` — `ManifestReader`
   (`:220-…`), error text at `:229-231`.

**Framework, Python — and this is a genuine second JSON Schema implementation:**

- `tools/annotate/contracts.py:10` — `from jsonschema import Draft202012Validator`;
  `:24-27` calls `check_schema` and constructs a validator. **The framework
  already validates documents against Draft 2020-12 — in Python, with a
  third-party library.** uf-chaos's C++ evaluator targets the same draft.
- `tools/annotate/jcs.py:124-132` — `load_exact_jcs`, a full JSON parse plus
  re-serialize to prove exact JCS (`json.loads` at `:128`).
- `tools/annotate/store.py:129`, `tools/annotate/serve.py:254,343`,
  `scripts/check_spec_bundle.py:149`, `tests/test-runtime-surface.py:492,539`.

**Framework, Luau:** `modules/task/runtime/jcs.luau` is an encoder only
(`jcs.encode` `:333`); its header at `:17-20` requires byte-for-byte agreement
with `appendJsonString` and `tools/annotate/jcs.py`.
`modules/task/runtime/project.luau:121-269` has `parseValue`/`parseArray`/… but
it parses **TOML-like** page-model text, not JSON (`[[section]]` headers
`:238-241`, bare `key = value` `:251-255`, `#` comments `:187`) — it is not a
JSON reader and should not be counted as one.

**Framework, vendored:** `modules/script/external/luau/Config/src/Config.cpp:247`
(`parseJson`, for `.luaurc`, unreachable from project code) and
`modules/operator/external/sqlite/sqlite3.c:223014` (`geopolyParseJson` —
SQLite's JSON1 surface is **not called**: grepping `modules/`, `entry/`,
`tests/`, `conformance/` for `json_extract|json_valid|json_each|json_object|json_array`
returns zero hits). The ONNX Runtime third-party notices list nlohmann/json and
rapidjson (`modules/ocr/external/onnxruntime/ThirdPartyNotices.txt:3075`,
`:4962`) but those are notices for a **prebuilt binary** — its shipped
`include/` holds only the ONNX Runtime API headers, so no JSON API is reachable.

**No `vcpkg.json`, no `FetchContent`/`CPMAddPackage`/`ExternalProject`
anywhere.** The only `find_package` in the build is `Python3`
(`cmake/build.cmake:7`); the only submodule is Luau. **No JSON library is
declared as a dependency in either tree.**

**uf-chaos, Luau — three byte-identical copies of one parser.** Verified by
`diff` over the exact spans; all three blocks are equal:

- `map/plan.luau:451-585` (`parseString`:454, `parseNumber`:475,
  `parseArray`:491, `parseObject`:518, `parseValue`:558; entry `M.decode`:604)
- `tasks/map-route.luau:460-594` (same functions at `:463`, `:484`, `:500`,
  `:527`, `:567`; `M.decode`:613)
- `tasks/daily.luau:1983-2117` (same at `:1986`, `:2007`, `:2023`, `:2050`,
  `:2090`; `M.decode`:2136)

It parses a deliberate JSON subset — escapes refused (`map/plan.luau:464-467`),
integers only (`:481-485`), `null` refused (`:581-583`), duplicate keys refused
(`:533-535`), trailing text refused (`:617-620`). Under this repository's rule
that is one thing spelled three times, and it is a *fifth* JSON reader across
the two trees, independent of `contract/json-value.cpp`.

### C.6 Canonicalization: who owns which half, and where the coverage gap is

| Tree | Language | Site | Scope |
|---|---|---|---|
| framework | C++ | `core/text/json-text.cpp:38-68`, `:70-91` | **the two primitives only** — string escaping, member ordering |
| framework | Python | `tools/annotate/jcs.py` (`_string`:17, `_member_order_key`:26, `_shortest_digits`:40, `_number`:56, `_encode`:86, `jcs_bytes`:114) | full RFC 8785 |
| framework | Luau | `modules/task/runtime/jcs.luau` (`quote`:130, `utf16Units`:106, `unitsLess`:120, `jcs.encode`:333) | full RFC 8785 |
| uf-chaos | C++ | `contract/json-value.cpp` (`appendNumber`:435-517, `appendCanonical`:519-575, `requireExactCanonicalJson`:705-718) | full RFC 8785, **built on the framework's two primitives** (`:3`, `:526`, `:555`) |
| uf-chaos | Python | `modules/jcs/canonical.py` (`_number`:105, `_encode`:131, `jcs_bytes`:170) | full RFC 8785, near-clone of `tools/annotate/jcs.py` |

**They are held to a shared vector file, and it is byte-identical across the
two trees.** `tests/vectors/jcs-vectors.txt` in both trees hashes to
`2fb2abe1…0fbba7`; uf-chaos pins that hash at `tests/jcs/test_canonical.py:30`
and compares the files directly at `:151-152` when the framework is on disk.
The file declares `count 47` at `:69` and 47 data rows follow (measured;
`docs/plans/2026-08-11-cross-repository-drift.md:399` claims 47 — **agrees**).

**But the vector file states the framework's own C++ gap outright.** 27 rows
carry `cpp=absent` — every integer and double row (`:22-46`) plus
`empty-object` (`:47`) and `empty-array` (`:48`). "absent" is defined at `:42`
as "the implementation has no entry point for this rule at all." That is the
suite recording that **the framework has no C++ number canonicalizer and no C++
container canonicalizer.**

**And uf-chaos's C++ canonicalizer has no vector coverage at all.** Grepping
uf-chaos for `jcs-vectors` / `vectors` across `*.cpp`, `*.hpp` and
`CMakeLists.txt` returns zero hits; only `tests/jcs/test_canonical.py` consumes
the vectors, and it exercises the *Python* module. So `appendNumber`
(`contract/json-value.cpp:435-517`), a fourth independent hand-rolled ES6
`Number::toString`, is the one RFC 8785 spelling in either tree with **no
vector coverage** — and it gates every `CanonicalJsonValidator` decision the
consumer makes.

**`CanonicalJson` is owned by `operator`, not `core`** — declared
`modules/operator/source/operator/project-plugin.hpp:41-57`, defined
`project-plugin.cpp:146-158`. Its contract (`:38-39`) is "Immutable exact JCS
with no schema authority."

**`ProjectSchemaOwner::canonicalize` does not canonicalize. It verifies.**
`modules/operator/source/operator/project-plugin.cpp:240-249`:

1. `:242-245` reject empty, oversized, or non-UTF-8 input;
2. `:246` call the injected `m_state->validateCanonicalJson(exactJcs)`;
3. `:247` `sha256` the bytes;
4. `:248` construct `CanonicalJson{hash, std::move(exactJcs)}`.

**The bytes arrive already canonical from the caller.** The framework
contributes a hash and an injected predicate — nothing else. That predicate is
re-run at every plugin call boundary (`:255-256`), and
`ProjectPluginHandle::canonicalize` (`:318-322`) merely forwards.

**The framework never supplies a real one.** The only
`CanonicalJsonValidator` implementation in this tree is
`conformance/exemplars/arcana-expedition/provider.cpp:701-738` — a hardcoded
allowlist of 17 literal byte strings (`:705-723`) plus five shape recognizers
(`:726-731`). It does not parse. The umbraflow fixture's is the same shape, a
15-entry allowlist (`project-fixture.hpp:422-438`). uf-chaos supplies the only
real one (`contract/provider.cpp:1040-1051` → `requireExactCanonicalJson`), and
its comment at `:1042-1046` names the difference exactly: "bytes are canonical
when re-serializing what they parse to reproduces them exactly. It recognizes
no shape."

### C.7 What C measures, in one line

The framework defines the canonical-JSON *interface*, owns the two byte-level
primitives, and ships **zero** C++ code that parses JSON into a value tree or
validates a JSON Schema. The consumer was forced to write **1547 lines**
(`json-value.cpp` 719 + `json-schema.cpp` 828) implementing 41 accepted
keywords over an RFC 8259 parser, of which 30 keywords are exercised and 11 are
dead. Meanwhile the framework already validates Draft 2020-12 in Python via a
third-party library (`tools/annotate/contracts.py:10`), and already has four
hand-rolled positional JSON readers of its own.

---

## D. The blast radius on the requirement matrix

### D.1 Where the matrix is

**The matrix is not in this repository.** Searching `docs/` for `REQUIRED_CORE`
and `PROJECT_CONTRACT` returns files that *cite* it; none contains it. The
matrix is a frozen, hash-pinned artifact in the consumer tree:
`E:\umbraflow-projects\uf-chaos\docs\architecture\requirements-traceability.md`
("v1.9 规范性附录", `:1-3`), named as the deciding artifact at
`docs/plans/2026-08-11-consumer-attestation.md:142` and hash-pinned at
`docs/plans/2026-08-09-claude-handoff.md:48`.

**It names no test IDs.** Its columns are `ID | 需求 | 归属 | 验收`, where
`验收` is prose. `:130-131` explicitly defers the ID mapping to a bound
migration report.

**The requirement → gate mapping lives here**, in two machine-readable forms:

- `docs/plans/2026-08-09-runtime-migration-report.md:130-184` — "Requirement
  ownership and test map", 51 rows, columns `ID | Owner | Schema location |
  Verification`. This is the authority for §D.3.
- `tests/CMakeLists.txt:68-128` (`UF_REQUIRED_DOCTEST_CONTRACTS`, 59 gate IDs),
  `:134-177` (`UF_REQUIRED_CORE_REQUIREMENTS`, 42 IDs, hard-asserted
  `EQUAL 42` at `:179`), `:192-195` (`UF_SCHEMA_ONLY_REQUIREMENTS`).

### D.2 Requirement count

**Measured: 51 rows** — P=6, D=9, U=8, S=6, C=14, A=8. By label: 42
`REQUIRED_CORE`, 8 `PROJECT_CONTRACT`-only (D-01…D-08), 1 `PHASED` (D-09), and
2 rows carrying both `REQUIRED_CORE` and `PROJECT_CONTRACT` (C-11, A-04). The
matrix states no total anywhere; the counts 51/42/8/1 appear in this tree at
`docs/plans/2026-08-11-consumer-attestation.md:729-730` and
`docs/plans/2026-08-10-next-block.md:83`. **Measurement agrees with both.**

### D.3 Where each requirement's closing gate runs

| Where the closing gate runs | Count | IDs |
|---|---:|---|
| **Inside the exported conformance suite** | **7** | **C-01, C-06, C-09, C-10, C-11, C-12, C-13** |
| Framework `tests/` — `test-contract-operator` | 25 | P-01…P-06, S-01…S-06, C-02, C-03, C-04, C-05, C-07, C-08, C-14, A-01, A-02, A-04, A-06, A-07, A-08 |
| Framework `tests/` — `test-contract-runtime` | 8 | U-01…U-08 |
| Framework `tests/` — aggregate only, no per-requirement ID | 2 | A-03, A-05 |
| Consuming project, `EXTERNAL` attestation | 9 | D-01…D-09 |
| Script / CI check | 0 | — |
| **Total** | **51** | |

### D.4 The seven suite-run requirements — the gates whose project-acquisition path must be re-wired

| Requirement | Closing case | file:line |
|---|---|---|
| C-01 | `contract-control-c01` | `conformance/source/suite-control-ledger.cpp:23` |
| C-06 | `contract-control-c06` | `conformance/source/suite-control-ledger.cpp:49` |
| C-09 | `contract-control-c09` | `conformance/source/suite-control-ledger.cpp:83` |
| C-10 | `contract-control-c10` | `conformance/source/suite-control-ledger.cpp:125` |
| C-11 | `contract-control-c11` | `conformance/source/suite-control-ledger.cpp:155` |
| C-12 | `contract-control-c12` | `conformance/source/suite-control-ledger.cpp:247` |
| C-13 | `contract-control-c13` | `conformance/source/suite-control-ledger.cpp:349` |

Corroborated three independent ways:

- The `CASES` list in this repository's own run,
  `conformance/exemplars/umbraflow/CMakeLists.txt:14-22`, with the reason at
  `:5-8`: "C-09 through C-13 are here because their store behaviour is here."
- The complement: `tests/CMakeLists.txt:512-518` gives `test-contract-operator`
  the *other* seven control contracts (c02, c03, c04, c05, c07, c08, c14).
- `docs/plans/2026-08-09-runtime-migration-report.md:204-206` names exactly this
  file and this set.

All seven reach `provideProject` through `prepareStore`
(`conformance/source/suite-support.cpp:157`), so all seven are re-wired by any
change to project acquisition.

### D.5 The eight suite cases that close no requirement

The suite declares **15** `TEST_CASE`s. Seven carry contract IDs (above). The
other eight carry prose names and run only under the aggregate CTests
`conformance-umbraflow` / `conformance-arcana` / `conformance-chaos`:

- `suite-control-ledger.cpp:383` "the reconciler owns the disposition, not the requester"
- `suite-control-ledger.cpp:452` "a reconcile outcome cannot be moved to another Operation"
- `suite-control-ledger.cpp:498` "the reducer is handed exactly the Journal prefix that is appended"
- `suite-control-ledger.cpp:536` "the deriver is handed an envelope no caller could have supplied"
- `suite-project-authority.cpp:54` "the Tool Catalog owns mutability and tool version"
- `suite-project-authority.cpp:83` "a schema owner cannot answer for a schema its registration never named"
- `suite-project-authority.cpp:126` "authority does not cross ProjectRegistrations"
- `suite-project-authority.cpp:163` "a ProjectPlugin cannot be registered against foreign bytes"

Zero `TEST_CASE`s exist in the four exported headers under
`conformance/include/conformance/`, in `suite-main.cpp`, in `suite-support.cpp`,
or in either exemplar `provider.cpp`.

**This split matters to the design more than the seven do.** The two
live-behaviour members (§A.3) are read *only* by
`suite-control-ledger.cpp:498` and `:536`, both un-ID'd. And **all four**
project-authority cases — the ones that exist specifically to prove the five
authorities cannot be forged — are un-ID'd. So the part of the suite most
entangled with C++ construction closes no requirement, while the seven that do
close requirements are ledger behaviour that merely needs *a* project.

### D.6 `conformance/manifest.txt` is not a coverage list

All 23 lines are a `check_modules.py` graph node: a header comment (`:1-15`)
explaining that the suite is not a module, then `[module]` (`:17-20`) and
`[dependencies] public = core domain engine image operator script task trace`
(`:22-23`). It names no requirement and no test.

### D.7 Requirements adjacent to this change, and what was not established

- **A-04's project half is closed by nothing a consumer can reach.** A-04
  carries both labels (`requirements-traceability.md:113`), but
  `docs/plans/2026-08-11-cross-repository-drift.md:420-434` finds
  `contract-agent-a04` is **not** project-parameterised — "`conformance/`
  contains zero occurrences of `a04`" — and it sits behind `CPP_BUILD_TESTS`,
  which a consumer never sets. C-11's project half *is* covered, because its
  closer is inside the suite.
- **A-07 was reopened when this was measured, and the reopening was wrong.**
  `contract-agent-a07` existed (`tests/operator/test-agent-audit-contract.cpp`)
  and proved the second acceptance clause only, which is what this row recorded.
  What it inherited from `runtime-migration-report.md` was a misreading: the
  frozen bundle puts *both* consequences inside `a07`'s 验收, and `07abc3e`
  substituted its 需求 sentence for the first of them, reading a clause as
  unimplemented when it was only untested — `requireLiveLease`, called from
  `reserveDispatch`, had implemented it all along. `bed456f` extended that same
  gate to run the schedule and closed it. **42 of 42 `REQUIRED_CORE` own a
  closing gate**, of which 40 are per-requirement and `a03`/`a05` share an
  aggregate. *(Corrected 2026-08-11; this row read 39 of 42 and cited
  `docs/INDEX.md:24`, which no longer says that.)*
- **A-03 and A-05: not established.** The migration report (`:179`, `:181`)
  calls them behaviourally covered by the aggregate CTest
  `test-annotate-backend` (`tests/CMakeLists.txt:898-902`). The CTest and the
  Python file exist; **whether that aggregate actually exercises A-03/A-05 was
  not verified** — the test bodies were not read.
- **The contract/schema split is a naming claim, not a checked one.**
  `tests/CMakeLists.txt:59-67` states that a `contract-` gate whose assertions
  only read a schema file "is caught by review and by nothing else."
- **G0–G5 (`requirements-traceability.md:119-131`)** are the matrix's own phase
  gates. Nothing in this tree evaluates them per requirement; whether any is
  currently satisfied is **not established**.
- **No document in `docs/` discusses a data-only project.** `project-as-data`,
  `data-only`, `data-driven`, `declarative`, `non-C++` return zero hits under
  `docs/`. The closest prior art is
  `docs/plans/2026-08-11-consumer-onboarding.md:469-497`, "The generator case,
  argued and rejected" — a `generate-provider.cmake` scaffold was refused
  because the generated lines "would be validators the consumer owns but never
  thought about", the `docs/pitfalls/checks-that-cannot-fail.md` defect
  "pre-installed, in every consumer, with the consumer's name on it." That
  refusal is about *generating C++ validators into a consumer*; it does not
  bear on shipping one validator in the framework binary, but a design that
  moves the validator upstream must say why it is not the same mistake.

---

## E. Numbers this inventory disagrees with

| Claim in the tree | Site | Measured today |
|---|---|---|
| arcana `provider.cpp` is 880 lines | `docs/plans/2026-08-11-consumer-onboarding.md:29,45,86` | **1063** |
| umbraflow `project-fixture.hpp` is 1532 lines | same, `:131` | **1804** |
| `provider.hpp` 140 / `operator-protocol.hpp` 649 / `observation-fixture.hpp` 777 / `host-delivery-fixture.hpp` 139 | same, `:36-37` | **221 / 716 / 821 / 195** |
| 213 P / 360 F / 208 A / 58 D / 41 N line split | same, `:86` | **stale by construction** — derived from the 880-line file |

That document dates its own measurement — "Every file-level claim was read at
`c23efd3`" (`:4`) — so it is pinned rather than wrong. But every percentage in
it needs re-deriving before a design is sized from it, and its headline "the gap
is roughly 3×" is computed over a file that has since grown by 21 %.

Two claims in the task framing were checked and hold, with one nuance each:

- **"26 schemas"** — 26 files, exactly matching `CHAOS_SCHEMA_FILES`
  (`contract/CMakeLists.txt:23-50`). Nuance: only 22 are JSON Schemas (§C.3).
- **"10 binary assets"** — 10 `chaos_append_bytes` byte-arrays, but the asset
  *list* is 9 (`contract/CMakeLists.txt:121-131`); the tenth is
  `runtime/probe-frame.png` (`:230-233`). Two further files
  (`page-model.toml`, `runtime-artifact.manifest.json`) are embedded as text
  (`:171-172`, `:224`, `:226`), so **38 files and 4 configure-time integers**
  cross the file→C++ boundary in total.

---

## F. What could not be established

1. **Whether `test-annotate-backend` actually exercises A-03 and A-05.** The
   CTest (`tests/CMakeLists.txt:898-902`) and
   `tools/annotate/tests/test_backend.py` exist; the test bodies were not read.
2. **Whether any of the matrix's G0–G5 phase gates is currently satisfied.**
   Nothing in this tree evaluates them; `docs/TODO.md:51-56` notes G2/G4 ticks
   predate seven schema breaks.
3. **Which document, if any, states "26 schemas and 10 binary assets."** Both
   trees' markdown was searched for `26 schema`, `twenty-six`, `22 schema`;
   the phrasing was not located. The CMake list itself was measured instead.
4. **Whether uf-chaos's `contract/json-schema.cpp` has ever been differentially
   tested against a reference implementation.** No vector suite, no
   cross-check against `tools/annotate/contracts.py`'s
   `Draft202012Validator`, and no JSON-Schema-Test-Suite reference was found in
   either tree. Its correctness rests on the 22 schemas compiling and the
   conformance run passing.
5. **Nothing here was compiled or run.** No preset was configured, no build was
   performed, and `scripts/ci-local.ps1` was not invoked. Every claim is from
   reading the tree at `6bfe1d6`.
