# Framework capability survey — 2026-08-14

This is a measured map of the framework tree, not an unfinished-work ledger.
“Exists” means the named implementation or schema definition was opened and
checked. A guard is named only when its assertion identifies the property; where
no such test was found, the row says **unguarded**. The consumer repository's
implementation plan remains the only owner of unfinished work.

## The two questions

Every row answers two questions, in two columns. The first is whether an
implementation and a falsifiable guard exist. The second, added 2026-08-17, is
whether production reaches that implementation at all. "Has a guard" is not "is
reached", and this document had already named entry points that do not exist
anywhere in the tree — `recordProjectObservation` when it was written, and
`databaseSchemaIdentity`, `migrateDatabaseSchema` and
`OperatorPlanAuthority::freeze` found by this pass. A reach cell that says
"reached" without naming the call site is worth nothing, so each one names it.

The method needs no uncommitted tool. Take the public entry points a capability
is reached through; search `modules/`, `entry/` and `tools/` for each name
followed by an open parenthesis, excluding the declaration and definition in its
own module. If every remaining hit is under `tests/`, production does not reach
that name. A name with no production caller is not automatically a defect, so
three outcomes are graded apart and must not be collapsed:

- **Bypassed** — production takes a different path that does the same work
  without the check. The most dangerous case: the guard is green, and real data
  goes past it.
- **Wired to nothing** — the capability has no production entry at all. There is
  no false path; the feature simply cannot be triggered.
- **By design** — a reader, an internal step, or a test-only accessor that is
  meant to have no production caller. Its absence from production is the design,
  not a gap, and calling it one would make this survey wrong in a new way. No
  whole row here falls in this class; three of the five names the release row
  gave as entry points do, being internal steps of the two that are.

A fourth state is not a grade but an admission, and rows carry it explicitly:

- **Not decidable by search** — the capability is reached through a callback, a
  template, or a Luau global published to project-authored source, so a name
  search inside this tree cannot settle it in either direction. Such a row says
  so rather than guessing.

`modules/conformance/` is counted apart from both `tests/` and production: it is
the acceptance harness, so a capability whose only non-test caller sits in a
conformance fixture is exercised without the product taking it.

The reach column was measured at `5cfe94a` rather than against the working tree,
because `modules/cli/`, `modules/service/`, `modules/operator/` and `entry/`
were mid-change while the pass ran, and one unrelated verdict was already moving
under it. That verdict moved: the same batch rewired `createSnapshot` through
the mint, so the operator rows and the findings below were re-measured against
the working tree on 2026-08-17, and their line numbers name that tree.

## Runtime model and resolution

| Capability | Measured implementation | Property-specific guard | Reached from production (working tree, 2026-08-17) |
|---|---|---|---|
| Two-stage resolution | **Exists.** `resolution.resolve_state` and `resolution.resolve_binding` are separate functions in `modules/task/runtime/resolution.luau`; `cycle:resolve_state` and `cycle:resolve_binding` expose them separately in `modules/task/runtime/observe.luau`. State results contain a Surface stack, while Binding results add the UiTarget, Binding and variant. | `tests/task/test-resolution-v2.luau`, assertion “T-004 T06 the same shared target must resolve and authorize through bindings on two different Surfaces”; its earlier state assertions also require the state to carry no Binding. | **Half reached, half not decidable by search.** `resolution.resolve_state` runs on every `uf observe`: `cycle:resolve_state` (`observe.luau:242`) is called by the embedded `k_observeSource` at `task-host.cpp:81`, which `TaskHost::observe` runs, which `ProductLifecycle::observe` calls at `product-lifecycle.cpp:385`, which `modules/cli/source/cli/observe.cpp:87` reaches. `resolve_binding` is not on that path: outside `modules/task/runtime/`, `cycle:resolve_binding` appears only at `modules/conformance/source/conformance/host-delivery-fixture.hpp:92`, and no framework module calls it. It is published to project-authored Luau — `runtimeProjectGlobals()` (`framework-bundle.cpp:53`) names `observe`, installed at `task-host.cpp:585` — so whether a project takes it cannot be settled from this tree. |
| Binding variants | **Exists.** `bindingVariant`/`bindingBuilder` in `modules/task/runtime/model.luau` compile the required `variants = [{ name, detector }]` list and reject duplicate names. `evaluateBinding` in `modules/task/runtime/resolution.luau` reports multiple present variants as ambiguous. The normative shape is `$defs.binding_variant` and `$defs.binding` in `schema/umbraflow-runtime-v2.schema.json`. | `tests/task/test-resolution-v2.luau`, assertion “two satisfied variants in one Binding must report ambiguous_binding”. | **Not decidable by search.** `evaluateBinding` has no caller outside `resolution.luau`, so the only way in is `cycle:resolve_binding`, whose reach is the row above's. The framework never calls it; a project script can. |
| Surface identity | **Exists.** `identity` and `surface` in `modules/task/runtime/model.luau` compile one list of Binding ids; `evaluateSurface` in `modules/task/runtime/resolution.luau` requires those Bindings. `$defs.surface_identity` in `schema/umbraflow-runtime-v2.schema.json` is a unique, non-empty identifier array. | `tests/task/test-resolution-v2.luau`, T-001 assertion that the same interrupt Surface resolves over two different base scenes. The exact schema spelling is also structurally checked by `tests/test-runtime-surface.py`. | **Reached.** `evaluateSurface` is called inside `resolution.resolve_state`, so it runs on the `uf observe` chain named in the two-stage row. |
| Collection detection and completeness | **Exists.** `collectionBuilder` in `modules/task/runtime/model.luau` requires a detected placement, block Reader, order and slot layout. `orderedCollectionLines`, `fittedCollection`, `collectionItems` and `resolve_collection` in `modules/task/runtime/resolution.luau` derive stable indices and `complete`/`partial`/`unknown`. `$defs.detected_collection_placement` and `$defs.resolved_collection` in `schema/umbraflow-runtime-v2.schema.json` match that result. | `tests/task/test-resolution-v2.luau`, T-002 assertions for detector-order-independent indices and all three completeness values; end-to-end case `A detected collection reports completeness stable slot index and exact rectangle` in `tests/task/test-runtime-v2-contract.cpp`. | **Not decidable by search.** The only entry is `cycle:resolve_collection` (`observe.luau:300`); outside `modules/task/runtime/` that name appears only under `tests/`, and no framework module and no conformance fixture calls it. Reachable from project-authored Luau through the `observe` global, which a name search in this tree cannot follow. |
| Collection actions and reads | **Exists.** `actionBuilder(true)`/`collectionBuilder` in `modules/task/runtime/model.luau` admit item-relative actions. `collection_action_point` and `collection_read_targets` in `modules/task/runtime/resolution.luau` add offsets to measured origins; reads use the declared absolute size. `$defs.collection` and `$defs.collection_read` in `schema/umbraflow-runtime-v2.schema.json` keep Collection offsets distinct from Binding action points. | `tests/task/test-resolution-v2.luau`, assertions “a Collection click point must derive from the named item's measured rectangle”, “a Collection action point outside the frame must be refused”, and “a Collection read's derived width must not change with the item's width”. | **Not decidable by search.** `collection_action_point` and `collection_read_targets` have callers only inside `observe.luau` — behind `cycle:resolve_collection` and `cycle:authorize` — and at `tests/task/test-resolution-v2.luau:913` and `:967`. Same Luau-global boundary as the row above. |
| Reader layouts and reading shape | **Exists.** `reader` in `modules/task/runtime/model.luau` requires `single_line` or `block`; `readText` in `modules/task/runtime/observe.luau` passes that layout to the Host. `resolve_readings` in `modules/task/runtime/resolution.luau` always emits a list of `{ rect, text }` lines. `$defs.reader`, `$defs.reading_line` and `$defs.binding_reading` in `schema/umbraflow-runtime-v2.schema.json` declare the same shape. | `tests/task/test-runtime-v2-contract.cpp`, `TaskHost::observe reports every line a block Reader read`, checks that the Host received `TextLayout::Block` and that all three line rectangles survive. `tests/task/test-runtime-model-v2.luau` also rejects a missing layout. | **Reached.** `k_observeSource` calls `cycle:resolve_readings` at `task-host.cpp:89`; inside it `readText` (`observe.luau:279`) invokes the `runtime_read` native, which lands on `TaskContext::cycleRead`. Same `uf observe` chain as the two-stage row. |
| Declared and observed transitions | **Exists.** `transition` in `modules/task/runtime/model.luau` compiles declared `from_surfaces`, one-owner trigger and `to_surfaces`. `resolution.compare_transition` in `modules/task/runtime/resolution.luau` compares an external observed destination without changing the declaration. `$defs.transition`, `$defs.observed_transition` and `$defs.transition_comparison` in `schema/umbraflow-runtime-v2.schema.json` separate policy from observation. | `tests/task/test-resolution-v2.luau`, T-003 assertions require both mismatch and match reports and require the model's canonical bytes to remain unchanged. `tests/task/test-runtime-model-v2.luau` refuses a trigger naming both Binding and Collection. | **Wired to nothing.** `resolution.compare_transition` has callers only at `tests/task/test-resolution-v2.luau:96` and `:116`. It is not among the eight verbs `observe.luau` publishes on `cycle`; `resolution` is not among `runtimeProjectGlobals()`; and the project environment is an explicit whitelist with no `__index` chain to the framework environment (`modules/script/source/script/ffi/sandbox.hpp`, boot step 7), so no project script can require it either. Declared-versus-observed transition comparison has no entry point in any process. |
| Unknown-state diagnostic | **Exists.** `unknownState` in `modules/task/runtime/resolution.luau` accepts an optional diagnostic, and `$defs.unknown_state` in `schema/umbraflow-runtime-v2.schema.json` declares it. | `tests/task/test-runtime-v2-contract.cpp`, `T-004 T10 visible unmatched content emits a diagnostic`, requires `visible_content_matched_nothing`. | **Reached, on a data-driven branch.** `unknownState` (`resolution.luau:225`) is called from `resolve_state`'s own competitor branches (`:321`, `:323`, `:343`), and `k_observeSource` projects `diagnostic = state.diagnostic`. It fires on the `uf observe` chain whenever a state fails to resolve, and never otherwise. |

## Image and project-extension capabilities

| Capability | Measured implementation | Property-specific guard | Reached from production (working tree, 2026-08-17) |
|---|---|---|---|
| Masked template matcher | **Exists.** The masked `matchTemplateSad` overload in `modules/vision/source/vision/sad.hpp` accepts a parallel grayscale weight plane. `GrayImage::candidateSad` and the masked search path in `modules/vision/source/vision/sad.cpp` weight SAD and normalize it by the summed mask weight. | `tests/vision/test-sad.cpp`: `an opaque mask reproduces the unmasked matcher exactly`, `a masked label template survives a changed background`, `masked scores normalize by the weight actually summed`, and `an unusable template mask is rejected`. | **Reached on a data-driven branch, through a callback traced by reading rather than by name.** The masked overload's only caller is `matchGrayTemplateImage` (`template-match.cpp:172`), which takes it exactly when `GrayTemplateImage::mask` is non-empty; `decodeTemplateImage` (`:109`) leaves the mask empty for a fully opaque alpha plane and fills it otherwise. Production decodes there, at `modules/task/source/task/template-store.cpp:77`. The match runs `TaskContext::cycleMatch` (`task-context.cpp:438`) under the `runtime_match` native (`ffi/uf-tables.cpp:2140`), which `measureLocator` (`observe.luau:48`) calls through the predicate closure `resolve_state` is handed at `observe.luau:247`. So it is reached whenever a project's Locator PNG carries a non-opaque alpha channel, and not otherwise. |
| Declared template cuts, reachable from the command line | **Exists, and reaches a project as of 2026-08-15.** A project declares its Locator templates in `template_cuts` at the root of `umbraflow-project.json`, stated even when empty; each names a `template` path, its `source_sha256s` and the `rect`. `schema/umbraflow-project-v1.schema.json` is the only statement of that shape, and `readProjectManifest` in `modules/project/source/project/project-kit.cpp` reads the member out of the document that schema has just accepted. `generatedTemplates` cuts each one through `image::cutRgba8Template`, re-hashes whatever the resolver answered, and refuses a mismatch. Resolving a hash to bytes stays outside the kit — `TemplateSourceResolver` takes a `ContentHash` and returns bytes, so `uf::project` never learns what a directory is — and `project build`, `project check` and `project freeze` are that caller, reading a hash-named store given by `--frames-root PATH`. Until 2026-08-15 the C++ API existed and no command line reached it: all three passed an empty resolver and `ProjectBuildSpec` carried a `templateCuts` member no CLI filled, so a project could not declare a cut at all. | `tests/project/test-project-cli.cmake`, through the real executable: `project build must cut a declared template when its source resolves` (positive control), `project build must refuse a declared cut it cannot resolve rather than skipping it` with the refusal required to name both the hash and `--frames-root`, `project build must refuse resolved bytes whose hash is not the one declared` naming both hashes, and the same two verdicts required of `check` and `freeze`. `tests/project/test-project-kit.cpp`, `a declared template cut` (`is cut from the sources the document names`, `whose source cannot be resolved is refused by name`, `whose resolved bytes hash to something else is refused`). | **Reached.** `readProjectManifest` (`project-kit.cpp:660`) is called by `buildProject` (`:2175`) and `checkProject` (`:2213`); `generatedTemplates` (`:909`) by `generatedProjectBuild` (`:1205`), which both take. `runProjectBuild`, `runProjectCheck` and `runProjectFreeze` (`modules/project/source/project/command.cpp:427`, `:455`, `:484`) are the `build`, `check` and `freeze` rows of the command table at `:536`, dispatched from `entry/project/main.cpp:64`. |
| Five-function ProjectPlugin SPI | **Exists.** `ProjectPluginFunction` and `ProjectPluginHandle` in `modules/operator/source/operator/project-plugin.hpp`, with the exact `derive`, `plan`, `next_step`, `reconcile`, `reduce` map and wrappers in `modules/operator/source/operator/project-plugin.cpp`. | `tests/operator/test-project-plugin-contract.cpp`, `ProjectPlugin registrar binds verified identity and exact bytes`, invokes all five; `ProjectPlugin validates complete schemas before and after execution` guards the data boundary. | **Two of the five reached; three wired to nothing.** `plugin.reduce` (`ledger.cpp:5122`) sits in `provisionProjectInstance`, called on the start path at `product-lifecycle.cpp:274`. `plugin.derive` (`:6668`) sits in `createSnapshot`, called at `product-lifecycle.cpp:379` inside `ProductLifecycle::observe`. `plugin.plan` (`:8417`, in `freezePlan`) and `plugin.nextStep` (`:8847`, in `mintNextStep`) are reached only from `ProductLifecycle::execute`, which nothing calls: outside `modules/service/`, `ProductLifecycle` is named only by `modules/cli/source/cli/observe.cpp`, which uses `start`, `identity`, `observe` and `shutdown`. `ProjectPluginHandle::reconcile` is invoked nowhere under `modules/operator/`, `modules/service/`, `modules/cli/` or `entry/` — its only callers are `modules/conformance/source/conformance/suite-support.cpp:211`, `tests/support/umbraflow/project-fixture.hpp:609` and `tests/operator/test-project-plugin-contract.cpp:259`. So the guard invoking all five exercises three functions no production path can reach. |
| Direct plugin authoring tier | **Exists.** Exact plugin bytes are registered and precompiled by `ProjectPluginRegistrar::registerPlugin` in `modules/operator/source/operator/project-plugin.cpp`. There is no game-specific C++ interface. | `tests/operator/test-project-plugin-contract.cpp`, `ProjectPlugin registrar binds verified identity and exact bytes`. | **Reached.** `ProjectPluginRegistrar::registerPlugin` is called at `modules/service/source/service/product-lifecycle.cpp:236` on the start path and at `modules/cli/source/cli/open-project.cpp:46` for `uf open`. |
| Direct-plugin tier justification gate | **Exists, presence only, and in both directions.** A deployment block of `umbraflow-project.json` declares which authoring path wrote its plugin in `plugin_authoring`; one that says `hand-written` must carry a non-empty `plugin_justification`, and one that says `generated` must carry none. `schema/umbraflow-project-v1.schema.json` states the rule once and both readers compile those published bytes out of the framework schema catalog: `readProjectManifest` in `modules/project/source/project/project-kit.cpp` for `project build` and `project check`, which reads the source root's document whether or not the author declared it as an input, and `ProjectLoader::load` in `modules/deployment/source/deployment/project-directory.cpp` at load. Whether the stated reason is true is deliberately not gated, and neither is a justification made only of non-ASCII whitespace — see [checks that cannot fail](pitfalls/checks-that-cannot-fail.md). | `tests/project/test-project-manifest-shape.cpp`, `umbraflow-project.json has one reader` (both readers, one document, one sentence, over nine documents including a generated adapter that owes nothing) and `the justification pattern refuses ASCII whitespace and says so`; `tests/project/test-project-kit.cpp`, `project refuses a hand-written plugin with no stated justification` (absent, blank, stated, generated-stating-none, generated-stating-one) and `project check judges the root document the author did not declare`; `tests/deployment/test-project-directory.cpp`, `a hand-written plugin without a stated justification is refused` and `a generated adapter owes no justification and is accepted`. | **Reached, both readers.** `readProjectManifest` as in the template-cut row. `ProjectLoader::load` (`project-directory.cpp:1093`) is called by `loadProductionProject` (`:1399`), reached at `modules/cli/source/cli/open-project.cpp:98` for `uf open` and at `product-lifecycle.cpp:184` on the `uf observe` start path. |
| Declarative bounded-workflow tier | **Exists.** `generateDeclarativeWorkflowAdapter` in `modules/project/source/project/declarative-workflow-tool.cpp` turns `umbraflow-declarative-workflow-tool/v1` data into the same five-function SPI; `generatedAdapters` in `modules/project/source/project/project-kit.cpp` regenerates it from declared project inputs. | `tests/project/test-project-kit.cpp`, `project build regenerates five-function adapters solely from declared source` and `generated bounded workflow runs through the five-function SPI`. | **Reached, on a data-driven branch.** `generatedAdapters` (`project-kit.cpp:754`) is called by `generatedProjectBuild` (`:1200`) and calls `generateDeclarativeWorkflowAdapter` (`declarative-workflow-tool.cpp:773`) at `project-kit.cpp:801`. It therefore runs under `project build` and `project check`, exactly when the project declares a workflow-tool input. |
| Two authoring paths, one SPI | **Exists.** A generated adapter is admitted through the same `ProjectPluginRegistrar::registerPlugin`, the same `ProjectSchemaOwner` bidirectional validation, and the same `ProjectPluginHandle` as a hand-written module, and one dismiss-overlay tool is implemented on both paths and compared function by function. This is what caught the generated `next_step` reading a `canonical_args` member the operator step envelope does not carry — see [checks that cannot fail](pitfalls/checks-that-cannot-fail.md). | `tests/project/test-authoring-path-parity.cpp`: `a generated adapter is admitted through the hand-written plugin boundary` (with a pinned-schema refusal subcase), `the two authoring paths agree function by function over one tool`, and its positive control `a deviating hand-written twin makes the parity comparison red`. | **Both ends reached; the junction is not.** Generation runs under `project build` (the row above); admission runs under `uf open` and the lifecycle start (the `registerPlugin` row). No production path generates an adapter and registers it in one process, so the equality this row asserts is established by `tests/project/test-authoring-path-parity.cpp` and by nothing production does. |
| Declarative single-step tier | **Exists only as the one-state subset of the bounded-workflow tier; no separate generator exists.** `tests/project/test-project-kit.cpp` states that the old generator was deleted and exercises a one-state/one-step declaration through `generateDeclarativeWorkflowAdapter`. The standalone `schema/umbraflow-declarative-single-step-tool-v1.schema.json` was deleted in the same change that added the workflow schema, so only one spelling of the declaration is checked in; `modules/project/source/project/declarative-workflow-tool.cpp` accepts only the workflow-v1 spelling. This is therefore not a third independent generator. | `tests/project/test-project-kit.cpp`, `a one-state, one-step schedule is still a whole tool`, guards the absorbed capability. | **Reached exactly as the bounded-workflow row is**, by construction: there is no second generator to reach. |

## Operator authority and delivery

| Capability | Measured implementation | Property-specific guard | Reached from production (working tree, 2026-08-17) |
|---|---|---|---|
| Operator-owned observed-instance authority | **Exists, and reached from production — corrected 2026-08-17.** `ObservedInstanceProposal` in `modules/operator/source/operator/project-observation.hpp` cannot carry a final id. `mintObservedInstanceBinding` in `ledger.cpp` (`:2408`) mints and persists the binding — first-query-then-insert on the canonical authority, writing the `local_ref` (the model target the instance was observed at) that the delivery receipt compares against — and `resolveObservedInstance` checks exact world scope and fresh membership. **`OperatorCoordinator::recordProjectObservation`, named here as the entry point, does not exist**: the symbol appears in this document and nowhere in the tree. `publishProjectObservation` (`ledger.cpp:7200`) is one entry to the mint (`:7219`), test-only; production takes the other one, inside `createSnapshot` (`:6396`), which composes the proposal from the derive return (`proposalFromDerived`, `:6683`) and mints it (`:6691`) against a context re-read from the same transaction. So the guards in the right-hand column protect the path production takes. | `tests/operator/test-ledger.cpp`: `the proposal cannot state an observed instance ID or authority binding`, `fresh observed instance membership is accepted`, `absent fresh member emits ObservedInstanceStale`, and `observed instance authority isolates exact registrations`. | **Reached.** `createSnapshot` (`ledger.cpp:6396`) mints every observation it closes — `proposalFromDerived` at `:6683`, `mintProjectObservation` at `:6691` — and the fingerprint it records names the minted bytes, not the derive return. `createSnapshot` runs on the `uf observe` chain (`product-lifecycle.cpp:379`), so real observations carry the mint's guards. `publishProjectObservation` (`:7200`) is the second entry to the same mint (`:7219`), test-only. |
| Exact DDL identity and registered migrations | **Exists, under different names than this document first gave — corrected 2026-08-17.** `databaseSchemaIdentity` and `migrateDatabaseSchema` appear nowhere in the tree except in this row's earlier wording. The real symbols in `modules/operator/source/operator/ledger.cpp` are `k_operatorDatabaseSchemaIdentity` (`:478`), `verifyExactDatabaseSchema` (`:779` and `:804`), `k_schemaMigrations` (`:1218`) and `upgradeOrVerifyExactDatabaseSchema` (`:1288`). Together they hash stored DDL text, select only exact source/target pairs, apply inside a transaction, and verify the target before commit. `PRAGMA user_version` is deliberately outside identity. | `tests/operator/test-ledger.cpp`: `PRAGMA user_version is no part of Operator schema identity`, `an unregistered exact identity pair is refused byte-identical`, and `a registered exact identity pair upgrades a populated audit chain`. | **Reached, verify half only.** `initialize` (`ledger.cpp:2558`) calls `upgradeOrVerifyExactDatabaseSchema` at `:2608` for any database that already holds schema objects; `initialize` runs from `OperatorCoordinator::open` (`:4335`) at `:4442`, and `open` is called at `product-lifecycle.cpp:211`. So the exact-identity verification runs on every `uf observe`. The migration half fires only when a stored identity differs from the current one, which no production path can yet produce — the 2026-08-17 DDL-mutation tests (removing the session world-scope columns, and the binding `local_ref` column) are the only places the half fires at all. |
| Policy evaluation | **Exists, and `OperatorPlanAuthority::freeze` does not — corrected 2026-08-17.** `VerifiedPolicyArtifact::evaluate` in `modules/operator/source/operator/policy.cpp` evaluates ordered selectors, controller capabilities, risk, explicit decisions, approval requirements, default deny and unknown-effect deny. Its one caller is `modules/operator/source/operator/effective-plan.cpp:968`, inside `OperatorPlanAuthority::mintPlan` (`:838`); no member named `freeze` exists on that class. | `tests/operator/test-tool-authority.cpp`: `required_approvals is the ruled approver set, not a risk flag`, `policy denies what no rule allows`, and `a rule speaks only to a controller holding its capabilities`. | **Wired to nothing.** `mintPlan` is reached from `OperatorCoordinator::freezePlan` (`ledger.cpp:8273`), whose only non-test caller is `product-lifecycle.cpp:453` inside `ProductLifecycle::execute` — and nothing calls `execute` (see the five-function SPI row). No production path freezes a plan, so no production path evaluates policy. There is no second path doing the same work without the check, so this is an absent entry rather than a bypass. |
| Tool offering and accept-side re-evaluation | **Exists, and the exposure this row first named does not — corrected 2026-08-17.** `ProjectToolCatalogSchemaOwner::offeredTools` in `modules/operator/source/operator/tool-invocation.cpp` filters by controller surface and required capabilities. `OperatorCoordinator::availableTools`, `ProductLifecycle::offeredTools` and `offeredProductTools` were deleted on 2026-08-17; the offered set is now computed once, into the snapshot's availability identity (`SnapshotRecord::availableTools`, `ledger.cpp:6534`), and a re-observation that finds the offer changed bumps the availability revision instead of reusing it (`:6844`–`:6856`). Command submission re-evaluates acceptance on the arguments rather than the catalog: the U2c entry gate in `submitCommand` resolves every `oi1_` id the canonical arguments spell against the persisted binding and refuses unknown, foreign or out-of-scope ids before the operation row is created (`:7557` onward), and `mintNextStep` gates the step's `ui_target_id` again. | `tests/operator/test-product-contract.cpp`, `contract-product-p03`, explicitly checks Agent and Human offered sets before submission, including absence of `raw-coordinate-click` for the Agent and presence for the Human. | **The filter runs; the accept side is still wired to nothing.** `ProjectToolCatalogSchemaOwner::offeredTools` is reached in production at `ledger.cpp:6534`, inside `createSnapshot`, on the `uf observe` chain — where it computes the snapshot's availability identity, not a set handed to a controller, and no exposure of the offer survives, so the identity travels as `SnapshotRecord::availableTools`. The accept side is the U2c gate in `submitCommand` (`ledger.cpp:7294`, gate at `:7557` onward), reached only from `ProductLifecycle::execute` (`product-lifecycle.cpp:425`), which nothing calls. So `contract-product-p03` guards an offer no production caller asks for, and a refusal no production caller can trigger. |

### External input and the control fence are different obligations

Settled 2026-08-17. `OperatorCoordinator::recordExternalInput` (`ledger.hpp:844`,
`ledger.cpp:7841`) has no production caller — its only callers are
`tests/operator/test-product-contract.cpp:415`, `:436` and `:489`. Fence
semantics for takeover are implemented in `modules/task/`'s `ControlFence` and
`TaskHost::adoptControlFence`. Whether those cover the same thing had never been
established. They do not.

The fence answers **who may act next**. `OperatorCoordinator::takeoverLease`
(`ledger.hpp:749`) raises `fencing_high_water`, rewrites the `control_leases`
row with a new revision, records a `control_transitions` row, and resolves every
unanswered dispatch to `transport_unknown` in the same transaction.
`OperatorTaskHost` then hands the derived fence to
`TaskHost::adoptControlFence` (`host-controller.cpp:108`, `task-host.cpp:889`),
whose monotonicity rule makes the Host refuse every Receipt minted under a lower
token. A takeover does invalidate the displaced controller's outstanding
snapshots, but only as a consequence of the lease revision moving — the
`s.lease_revision=lease.revision` join in `submitCommand` (`ledger.cpp:7522`).
That is deliberate and narrow: `SnapshotRecord`'s own comment (`ledger.hpp:228`)
states that a lease takeover moves `identityHash` and **must not** move
`decisionBasisHash`. Authority changed; the world did not.

`recordExternalInput` answers **what is still known**. It raises no fence, moves
no lease, and resolves no dispatch. It writes one `external_input_findings` row
carrying `invalidated_snapshot_revision` (`ledger.cpp:7962`), moves the single
non-terminal Operation on that target with `OperationEvent::DecisionInputsChanged`
rather than by any authority verdict (`:7913`), and appends
`LedgerEventKind::ExternalInputDetected` (`:7994`) so `subscribe` can carry the
notice its own comment says a controller most needs (`ledger.hpp:834`).

The gap between them is the case the fence structurally cannot see: a human who
acts on the target **without holding a lease** — clicking the game window
directly. No lease moves, so no fence rises, so the Host accepts every Receipt
already minted and every outstanding snapshot still joins its lease revision.
Only a finding marks that world stale.

What production loses by never calling it:

- The `NOT EXISTS (SELECT 1 FROM external_input_findings ...)` clause in
  `submitCommand` (`ledger.cpp:7525`–`:7528`) is the only consumer of the table,
  and `recordExternalInput` is its only writer. With no production writer the
  table is always empty, so that clause can never exclude a snapshot — a check
  that cannot fail in production, of the kind
  [checks that cannot fail](pitfalls/checks-that-cannot-fail.md) names. Its own
  comment claims it is "what makes out-of-band human input stop the automation".
- No `ExternalInputDetected` event is ever appended, so the reason `subscribe`
  reads other controllers' events cannot be exercised in production.
- `ExternalInputAction::FreezeAndReobserve` and `FreezeAndReconcile` have no
  production producer.
- `ControllerProfile::mayReportExternalInput` is true for `ControllerKind::Human`
  alone (`controller.hpp:64`), and production pins exactly that kind
  (`product-lifecycle.cpp:304`). The gate is therefore not what keeps the call
  from happening; there is simply no caller.

**This does not affect `U10a`.** Its completion condition is that one production
owner holds both the `OperatorCoordinator` and the `TaskHost`, that takeover and
delivery share one target serialization, and that a case proves a displaced fence
cannot start another dispatch. That owner is `OperatorTaskHost`, constructed at
`product-lifecycle.cpp:222` and reached by `uf observe`; `takeoverLease` and
`dispatch` take the same `m_impl->targetSerialization` lock
(`host-controller.cpp:106`, `:123`). `recordExternalInput` is neither a takeover
nor a delivery — it mints no authority and fences no Host — so it is outside
every clause of that condition, and the evidence does not support calling `U10a`
unmet. Two adjacent facts belong beside it rather than inside it: neither
`OperatorTaskHost::takeoverLease` nor `OperatorTaskHost::dispatch` has a
production caller (only `acquireLease`, at `product-lifecycle.cpp:311`), so the
owner is constructed in production while the two verbs whose serialization it
guarantees are exercised only from `tests/` and `modules/conformance/`.

## Project release

| Capability | Measured implementation | Property-specific guard | Reached from production (working tree, 2026-08-17) |
|---|---|---|---|
| Immutable release closure | **Exists.** `freezeProject`, `releaseArtifactRows`, `makeReadOnly`, `validateReleaseTree` and `loadProjectRelease` in `modules/project/source/project/project-kit.cpp` derive a release id from the artifact manifest, copy exactly its rows, make the tree read-only, and verify path, membership and digest closure on load. | `tests/project/test-project-kit.cpp`, `project release is immutable stable complete and excludes inputs`, checks stable identity, exclusion of hand-written inputs, read-only entries, zero Python files, and digest refusal after a byte mutation. | **Reached, through two of the five names.** `freezeProject` (`project-kit.cpp:2258`) is called at `modules/project/source/project/command.cpp:484` for `project freeze`, and `loadProjectRelease` (`:2339`) at `command.cpp:513` for `project run`; both are dispatched from `entry/project/main.cpp:64`. The other three are internal steps of those two rather than entry points — `releaseArtifactRows` (`:1691`) at `:2271` and `:2362`, `makeReadOnly` (`:1767`) at `:2334`, `validateReleaseTree` (`:1863`) at `:2363` — and have no caller anywhere else. |

## Findings

The requested runtime, matcher, Operator and release capabilities all have
property-specific guards in this tree.

> Qualified 2026-08-17. "Has a guard" is not "is reached". The
> observed-instance row above turned out to guard a path production does not
> take, and this document named an entry point that does not exist — which is
> the failure mode a survey written from measurement is supposed to prevent,
> found by tracing callers rather than by opening the named symbols. The reach
> column added the same day answers the second question for every row, and two
> more phantom symbols fell out of writing it: `databaseSchemaIdentity` and
> `migrateDatabaseSchema` in the DDL row, and `OperatorPlanAuthority::freeze` in
> the policy row, none of which exist anywhere in the tree. Naming the caller,
> not the symbol, is what caught all three.

What the reach pass found, graded rather than counted:

- **Bypassed** — observed-instance authority, until the batch that landed the
  2026-08-17 corrections put `createSnapshot` through the mint
  (`ledger.cpp:6683`/`:6691`). Production's observations now carry the mint's
  guards, and no row is graded bypassed.
- **Wired to nothing** — declared-versus-observed transition comparison, which
  has no entry in any process, not even from a project script; policy evaluation
  and the accept-side re-evaluation of a submitted command; the SPI's `plan` and
  `next_step`; the SPI's `reconcile`, which is invoked from no production module
  at all rather than merely from an unreached one; and
  `recordExternalInput`, settled above. None of these has a rival path, so none
  is a wrong answer being produced — each is a capability that cannot be
  triggered.
- **Not decidable by search** — Binding resolution, Binding variants, and both
  Collection rows. All four are reached only through `cycle:resolve_binding` or
  `cycle:resolve_collection`, which are published to project-authored Luau as the
  `observe` global (`framework-bundle.cpp:53`, installed at `task-host.cpp:585`).
  The framework itself never calls them, so this tree cannot say whether a
  project does, and this document declines to guess in either direction.

Most of the second group has one cause. `ProductLifecycle::execute` is the door
to plan freezing, step minting, command submission and dispatch, and nothing
opens it: the `uf` binary's five commands are `explore`, `observe`, `ocr`, `open`
and `targets`, and only `observe` constructs a `ProductLifecycle` at all. Wiring
one command that submits a tool invocation would move four of those entries at
once, which is worth knowing before any of them is treated as a separate defect.

The authoring stack does not contain
three independent SPI generators: it contains direct five-function plugins and
one bounded-workflow generator, with single-step behavior absorbed as a tested
degenerate schedule. The standalone single-step schema is gone, so only one
spelling of the declaration is checked in. That distinction was ruled on in the
consumer plan's `U5` rows on 2026-08-14: there are two authoring paths, not
three tiers, and reviving a third generator to satisfy the old wording would be
a second spelling of the one-state workflow.
