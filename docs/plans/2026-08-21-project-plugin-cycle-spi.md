# Unified Tool Runtime, Framework Luau SDK, and replayable Project automation

Date: 2026-08-21
Scope: `operator`, `service`, `script`, `deployment`, `task`, Agent bindings,
Workbench/CLI, schemas, Project Kit, conformance, release publication, and
consumer migration
Status: **owner-approved direction; internal implementation in progress**

The ruling is frozen in
[`tools are the shared game-driving boundary`](../decisions/2026-08-21-tools-are-the-shared-game-driving-boundary.md).
Nine later rulings refine it and are equally binding on this plan.

Four fix the shape of the generation being built:
[`scoped Tool execution is a third program type`](../decisions/2026-08-22-scoped-tool-execution-is-a-third-program-type.md)
fixes the scoped program type, the native call shape, facade authorship, the
replay key, and the audit path;
[`a Tool handler and an automation script are one mechanism`](../decisions/2026-08-22-handler-and-automation-script-are-one-mechanism.md)
fixes the unified entry declaration, where the binding table lives, one compiled
program per registration generation, the issuing context and its replay
semantics, and the root-positioned call row;
[`caller independence is structural`](../decisions/2026-08-22-caller-independence-is-structural.md)
fixes how every caller reaches admission and what the four-way fixture is for;
and
[`production reachability is the cut invariant`](../decisions/2026-08-22-production-reachability-is-the-cut-invariant.md)
fixes how the generation is staged and flipped.

Five more fix the final cut itself:
[`one job, one vehicle`](../decisions/2026-08-22-one-job-one-vehicle.md)
fixes what survives of the pure program type — `reduce` and nothing else — and
makes a registration carry two closures with two manifest hashes, breaking the
"ONE closure, ONE manifest hash" invariant outright;
[`provisioning rekeys to the registration generation`](../decisions/2026-08-22-provisioning-rekeys-to-the-registration-generation.md)
fixes what provisions a scoped-generation Project and what the flip owes each
stored baseline;
[`a Tool name is owned by its namespace`](../decisions/2026-08-22-a-tool-name-is-owned-by-its-namespace.md)
fixes the Tool-name grammar, the positive ownership check, and the premise the
deleted reserved-prefix refusal rests on;
[`a join needs two independent sources`](../decisions/2026-08-22-a-join-needs-two-independent-sources.md)
fixes where `exportedEntryPoints` comes from and forbids the loader deriving or
observing it; and
[`re-entry is keyed on recorded children`](../decisions/2026-08-22-re-entry-is-keyed-on-recorded-children.md)
fixes which crashed dispatches re-enter and which stay uncertain.

This live plan owns the experiments, the atomic implementation, and the stage
order those rulings are implemented in — section 13 holds that order, because an
ordering is work rather than a frozen ruling. The current five-function
ProjectPlugin code and generated public contract remain the executable contract
until this plan's replacement generation lands in full.

Checkpoint paragraphs below state what is in the tree at the commit or date each
one names. Every other statement is a requirement on the finished generation,
and a requirement is not weakened because the tree has not met it yet — nor is
it discharged because a checkpoint reports it met.

Internal checkpoint on 2026-08-21: `ValidatedToolInvocation` carries a
provider-neutral Framework-or-Project identity, and Operator owns the
content-addressed Framework Tool Catalog for `framework.screen.observe` and
`framework.workflow.wait`. Root request and call-position identities now have
durable Operator rows. Exact reuse of a caller namespace/root key or
root/parent/sequence rejoins the existing row; changed canonical root bytes or
caller-fixed call material is refused as conflict/nondeterminism. Parent calls
must already exist under the same root. Exact canonical preimages and arguments
are retained and rechecked, so hash-only or stored-byte tampering cannot pass
replay. A registered exact schema migration adds the two tables without
rewriting existing audit rows, and restart tests prove deterministic rejoin.

The persisted call identity covers run, Framework release, Tool Runtime
protocol, environment, provider/catalog, Tool name/version and canonical
arguments. It deliberately has no session, admission, result, delivery, or
provider execution state.

The first read-only runtime checkpoint now persists a separate immutable run
owner and append-only admission attempts derived from the live controller,
session, policy, capability profile, lease/fence, registration, and budget
snapshot. Dispatching is committed before provider execution; exact terminal
results replay without authority or execution, while restart converts an
unanswered dispatch to durable `possible` and refuses redispatch. An
`admitted` call whose dispatch never began may append a fresh, non-expanding
current-epoch attempt. Sibling positions remain ordered, child calls are
refused until delegation grants land, and Agent admission charges the Tool-call
budget exactly once. That persistence checkpoint deliberately exposed no
provider adapter and explicitly refused mutating admission at that point. Of the
four things that checkpoint left open, nested delegation has since landed
(section 3.3) and the public contract generation has been extended to the scoped
facade surface; public actor adapters and Project handlers/automation remain
unimplemented.

The first caller-neutral executor now owns terminal fast-path replay, live
read-only admission, durable dispatch, exactly one provider call, conversion of
provider errors into canonical terminal failures, and final outcome replay.
`ProductLifecycle::invokeFrameworkReadOnlyTool` attaches the production
`framework.screen.observe` and `framework.workflow.wait` providers to that seam.
The authenticated controller binding supplies the root namespace; observe
returns an opaque durable snapshot reference plus pinned resolution metadata,
wait uses the caller's validated bounded duration, and exact terminal replay
performs neither operation again. Public actor adapters and Project providers
remain to be implemented.

That seam is deliberately dark: it has no production caller. The shipped CLI
observe verb calls `ProductLifecycle::observe` directly, so no production run
creates a `tool_call_history` row, and `ToolRuntimeExecutor` is constructed
nowhere but inside this unreachable function. Its only exercise is
`tests/cli/test-observe.cpp`. Under
[`production reachability is the cut invariant`](../decisions/2026-08-22-production-reachability-is-the-cut-invariant.md)
that is the intended intermediate state, not unfinished wiring, and the absence
of a production caller is the property that keeps it legitimate.

The first mutating checkpoint now uses the same executor, live authority rows,
dispatch-before-provider boundary, durable replay, and admission-attempt model.
Only one mutating Tool or legacy Operation may be active on a controlled target.
After dispatch, a provider error or attempted terminal-failure answer is stored
conservatively as `possible`; restart applies the same classification to an
unanswered mutating dispatch. `possible` freezes mutation across roots and
actors while read-only observation remains available. A live same-origin
reconciliation with mandatory canonical evidence may classify the call
`confirmed` or `proven_absent` and release the barrier, or
`terminally_unresolved` and retain it. Agent Tool-call, mutation, and Framework
observation budgets are charged together in the first admitted transaction and
not charged again on exact rejoin.

Mutating admission now also requires a concrete effect set and the verified
Operator plan authority for the active session. The shared effect canonicalizer
orders and hashes the set exactly as EffectivePlan does; descriptor bounds and
the pinned PolicyArtifact are evaluated before admission, and the canonical
envelope, hash, policy hash, and verdict are immutable fields of the durable
attempt. A rule requiring approval now accepts only call/root/effect/policy and
live lease/fence-bound tokens. Every required capability needs one token; they
are consumed atomically with the admitted attempt, remain immutable on exact
rejoin, cannot cross calls, and must still be unexpired at dispatch. Trusted
Framework/Project compilation of the proposed root effects remains before a
production mutating provider may be exposed.

The pure SDK checkpoint now reserves `@umbraflow/` inside the closed Project
module resolver and exposes the existing embedded RFC 8785 module as
`@umbraflow/jcs`, plus the frozen `@umbraflow/collections` and
`@umbraflow/result` vocabularies and strict `@umbraflow/json` value API.
Reserved requests never fall through to Project modules; Framework modules
cannot import the Project graph; exports are deep-frozen and cached per fresh
VM. `plugin_environment_hash` now covers exact SDK module names, per-module
source hashes, project-visibility flags and per-module dependency depth, a
framework module budget literal, a freeze-behaviour literal, and the pure-data
environment material — which itself carries the `require` contract
`closed_project_relative_plus_reserved_framework_cached_value_v2`, the module
grammar `ascii_slash_segments_relative_prefix_reserved_umbraflow_v2` and the
resource grammar `ascii_dotted_segments_reserved_umbraflow_v2`. It still does
not cover resolver *behaviour*: those three are fixed literals in
`modules/script/source/script/ffi/program-runtime.hpp`, and nothing derives them
from `resolveModuleRequest`, so a resolver behaviour change can still ship under
an unmoved digest. E6 requires that it cannot; closing that is tracked in the
resolver follow-up risks in section 13. UTF-8 traversal and Unicode 15.0
major-category classification are pinned in `@umbraflow/utf8`, and normalization
and case folding are pinned in `@umbraflow/text`. Strict JSON parsing,
deterministic encoding, empty object/array identity, and immutable value updates
are pinned in `@umbraflow/json`. Their Luau algorithms are maintained as runtime
source, while generated Unicode data lives in embedded Framework-internal
modules that Project source cannot require directly.

Internal checkpoint in the working tree on 2026-08-22, superseding the "all
scoped modules remain pending" clause above: `script::ScopedToolProgram`
(`modules/script/source/script/scoped-tool-program.hpp`,
`ffi/scoped-tool-program.cpp`) is the third program type, the four scoped
facades exist as Luau sources under `modules/task/runtime/`, nested Tool calls
and the snapshot-observation refusal matrix are implemented, and the Framework
Tool Catalog is eight tools wide. All of it is production-unreachable: it is
reached only from tests, `ProductLifecycle` is unchanged, and no selector
exists. Under
[`production reachability is the cut invariant`](../decisions/2026-08-22-production-reachability-is-the-cut-invariant.md)
that is the intended state, not the cut. The per-section checkpoints below say
which half of each requirement the tree now meets.

## 1. Product boundary

Agent, human-operated clients, and Project Luau automation scripts drive a game
through the same Tool Runtime:

```text
Agent ------------------+
Human Workbench / CLI --+--> Operator-owned Tool Runtime
Project automation -----+              |
                                       +--> framework.* Tools
                                       +--> <project>.* Tools
```

Framework and Project are both tool providers. Project automation is a tool
caller, not a privileged Host callback. Project may also provide tool handlers,
and a handler may make child calls only through the same runtime.

The shared call mechanics do not flatten authority. The actor identity and
profile, Tool Catalog descriptor, policy, approvals, EffectivePlan, lease,
fence, observation freshness, parent authority, and remaining budgets all
remain inputs to admission.

## 2. Target game-driving flow

All three actors can express the same sequence:

```text
framework.screen.observe
  -> project interpretation / recognition Tool
  -> project planning Tool or actor-local decision
  -> framework semantic input Tool
  -> optional caller-selected bounded wait
  -> framework.screen.observe
  -> project reconciliation / completion Tool
```

An input Tool never captures the next frame implicitly. Agent, human, or Luau
automation chooses when to call the separate bounded wait Tool and when to call
`framework.screen.observe` again. The returned observation is newly captured,
not certified stable: a transition, loading screen, animation, or unrelated
overlay is a valid result. The caller may pass that snapshot to Project
recognition and repeat wait-observe-recognize until the desired state appears
or its timeout/budget expires.

An Agent may assemble that sequence dynamically. A human may invoke the same
Tools through Workbench or CLI. A Project may load a pinned Luau automation
script that performs the sequence in a normal loop. A Project may also expose
the complete automation as one high-level semantic Tool for another actor to
call.

No actor is required to drive project-specific microsteps when the Project
already exposes a high-level Tool. Conversely, Framework does not hide the
lower-level, policy-admitted Tools needed for Agent exploration, human control,
or a Project-authored automation loop.

## 3. Unified Tool Runtime

### 3.1 One invocation envelope

Every root and child call compiles into one internal invocation, whose material
is split across the four durable records section 5.2 defines. That split is
normative and 5.2 is its authority; this section only says which record owns
what.

The **external root idempotency preimage** carries the authenticated caller
idempotency namespace, the stable root request key, and nothing but
caller-supplied pre-admission request material.

The **call position** carries the material fixed by caller and runtime before
admission, and is addressed by its ordinal coordinate:

- run and root-invocation identity;
- parent position and the seam-assigned child index;
- Tool name and version;
- canonical arguments;
- Framework release and Tool Runtime protocol identity;
- Luau environment identity, and the identity of the compiled scoped program the
  caller runs when Luau is the caller — one program per Project registration
  generation, not one per entry;
- Project registration and Tool Catalog identity when a Project is involved; and
- the observation/snapshot reference when the Tool consumes one.

The **admission attempts** carry everything Operator selects rather than the
caller supplies: origin actor kind and profile, the current executing principal,
session epoch, policy and approvals, EffectivePlan and the effect envelope,
lease and fence, delegation or continuation grant, controlled target, and the
admitted budget snapshot. A replaying caller never reproduces this material, and
it is deliberately not part of either request identity.

The **durable outcome** carries dispatch state, provider result or error,
evidence, and delivery classification.

Agent, human, and automation adapters may have different transport envelopes,
but they must compile into this one internal invocation. No adapter gets a
second execution path, and under
[`caller independence is structural`](../decisions/2026-08-22-caller-independence-is-structural.md)
that is enforced by construction rather than by adapter discipline: an adapter
translates transport specifics into one internal admission-request value and can
construct nothing that is executable.

Internal checkpoint in the working tree on 2026-08-22: the split is implemented
as described for the principals — `ToolExecutionIdentity` deliberately excludes
origin and executing principals, policy, approval, lease, fence and admitted
budget, and `tool_admission_attempts` holds them, now including the delegation
grant (`delegation_grant_id` referencing `tool_delegation_grants`). Two of the
three position members named in the previous checkpoint have landed. The
seam-assigned child index is real: `ToolCallIssuingContext` owns the per-context
counter and `ToolCallPositionIdentity::create` is private to it. The
observation term is real: it enters `callIdentityMaterial`, the
`tool_call_positions.observation_reference_hash` column, and the field-by-field
divergence comparison. Two gaps remain, and both stay requirements.
`ToolExecutionIdentity` still carries no identity for the compiled scoped
program a Luau caller runs — it is four hashes and nothing more. And
`environmentIdentity` (like
`toolRuntimeProtocolIdentity`) is still an opaque caller-supplied hash on the
production seam: it arrives as a parameter of
`ProductLifecycle::invokeFrameworkReadOnlyTool`, and nothing there derives it
from release bytes. A derived scoped environment digest does now have a consumer
above `script`: `currentScopedToolEnvironmentHash()` in
`modules/operator/source/operator/project-tool-program.cpp` hashes
`script::scopedToolEnvironmentMaterial()` together with the source hash, depth
and Project visibility of every Framework module the scoped closure admits and
the reserved name of the pinned catalog resource, and
`ProjectToolProgramRegistrar::registerProject` stamps it on the loaded program
handle. Project Tools are dispatched now (section 5.1), and a dispatched call
does carry that digest: the only caller that mints such a position today,
`tests/operator/test-tool-dispatch.cpp`, sets
`ToolExecutionIdentity::environmentIdentity` to
`ProjectToolProgramHandle::environmentIdentity()`, so a run that reached a
different scoped generation diverges on a named field. Nothing outside a test
mints one, because the producer that would is section 13's next stage, and
`ToolExecutionIdentity` still carries no member of its own for the compiled
program. Both gaps above stay requirements.

### 3.2 Namespaces and discovery

[`A Tool name is owned by its namespace`](../decisions/2026-08-22-a-tool-name-is-owned-by-its-namespace.md)
fixes this section's naming rule, the check that enforces it, and the deletion
that follows from the check. This plan implements it as ruled.

Framework owns `framework.*`. A Project owns its registered namespace, normally
its `plugin_id`. A Tool name is namespaced — the dot is required, not optional —
and the namespace is the owner's **whole registered namespace**: `framework` for
the Framework, one segment, and a Project's entire `plugin_id`, at least two.
The local name is everything after the dot that ends the namespace, so Tools are
spelled `framework.screen.observe`, `arcana.expedition.move`,
`fixture.control.command-1`, and the check is a prefix test at a dot boundary
rather than a split at the first dot. The ruling records why the first-dot
reading is not available: a `plugin_id` is itself namespaced, so a first-dot
split would put every Project Tool outside the namespace its own registrant owns
and refuse the entire Project tier. Tool discovery returns only what the current
actor may see. A descriptor that is not offered may not be invoked by spelling
its name anyway.

Framework initially needs read-only observation/capture/status Tools, a
separate bounded wait Tool, and input Tools capable of consuming
snapshot-scoped semantic targets.
Bare coordinates and other low-level input remain privileged Tool surfaces;
being a Framework Tool does not make them generally available.

Project Tools carry game interpretation, recognition, planning,
reconciliation, and high-level goal execution. Their exact handlers, schemas,
descriptors, and resources are registration-pinned.

Internal checkpoint in the working tree on 2026-08-22: the positive half is
enforced and the negative half is gone. A Tool name is now a namespaced name in
every document that carries one -- the Tool Catalog's `name`, a plan's
`tool_name`, each `child_tool_names` entry, and the registration's binding
`tool_name` all validate as the `ToolName` type, whose grammar is the
registration's own `namespaced_name` -- so the dot is required and every Tool
has an owner. `ProjectToolCatalogSchemaOwner::create` and
`project::generateToolCatalog` both refuse a descriptor whose namespace is not
the registrant's `plugin_id`, through the one
`operator_runtime::validateToolNameOwnership`, so a Project can no longer
declare a Tool in another Project's namespace or in the Framework's. The
reserved-prefix refusal that was the negative half is deleted: it became
unreachable once `validateClaims` refused a `plugin_id` inside `framework.`,
which is where the Framework's ownership of `framework.*` is now stated once.
The rename half landed with the schema rather than beside it: an undotted name
no longer validates anywhere, so every fixture, shipped example and recorded
name that still declares a Tool declares a namespaced one, and the schema is
what says so rather than a survey.

The Framework catalog is now eight tools wide, declared in
`modules/operator/source/operator/tool-invocation.cpp`:
`framework.audit.record`, `framework.input.coordinate`,
`framework.input.semantic_target`, `framework.screen.capture`,
`framework.screen.observe`, `framework.workflow.reconcile`,
`framework.workflow.status` and `framework.workflow.wait`. Six are read-only;
the two input Tools are the only `Mutating` ones. The surface split this section
requires is implemented in the descriptors: `framework.input.semantic_target` is
`Semantic`, while `framework.input.coordinate`, `framework.workflow.reconcile`
and `framework.screen.capture` are `Privileged`. What exists is descriptors,
argument validation and catalog identity — only `framework.screen.observe` and
`framework.workflow.wait` have providers attached, and those only behind the
production-unreachable `ProductLifecycle::invokeFrameworkReadOnlyTool`. Every
other tool in the list is a catalog entry no provider answers yet.

A resource-name namespace is reserved alongside the module namespace:
`umbraflow.` is the Framework's. `PureDataProgram::compile` and
`ScopedToolProgram::compile` take Project `resources` and Framework
`frameworkResources` as separate parameters admitted by opposite rules — a
Framework resource name must start with the reserved prefix and a Project one
must not — so the two name sets are disjoint by construction, no resource value
satisfies both admissions, and nothing carries a trust flag saying which side a
resource arrived from.

### 3.3 Nested calls

A Project automation script may call Framework or Project Tools. A Project Tool
handler may call child Tools when its descriptor and execution profile permit
it. Every child call:

- records root and parent identity;
- consumes the root run's call/depth/elapsed budgets;
- is admitted from the intersection of the root effect envelope, the parent
  descriptor's registered child-effect declaration, current Operator policy and
  approvals, target/session authority, lease/fence, and remaining budgets;
- cannot introduce a new mutation objective, approval, project registration,
  or target;
- remains in the one live mutation chain; and
- is cycle-detected and depth-bounded.

Shared implementation that needs no Tool authority remains an ordinary Project
module function and should not pay Tool Runtime cost.

Direct Tool visibility and delegated effect authority are distinct. An actor
may be admitted to a high-level Project Tool without being allowed to invoke its
low-level Framework child directly. Operator compiles the admitted root Tool
and its declared effects into an EffectivePlan; a Project descriptor can
request that envelope but cannot grant or widen it. The handler execution
principal is recorded separately from the origin actor and never substitutes
its own profile for the origin's admitted objective.

Internal checkpoint in the working tree on 2026-08-22: child calls are no longer
refused outright — the mechanics of this section are implemented in
`OperatorCoordinator::admitToolCall` and exercised by
`tests/operator/test-tool-nested-calls.cpp`. `ToolDelegationGrant` is a durable
type with its own `tool_delegation_grants` table; a child call presents a grant
and every ceiling is read back from that durable row rather than from the
caller, so a handler cannot widen what its own descriptor declared.
`ToolDescriptor::childEffects` carries a `ChildEffectDeclaration` — permitted
child tool names, maximum child surface, mutability, risk and call count — and
that declaration's bytes are inside `tool_catalog_hash`, so widening a tool's
blast radius moves the catalog identity. Depth bounding and cycle detection both
read the durable parent chain through one `WITH RECURSIVE` ancestor query: depth
is refused above `k_maximumToolCallDepth`, and a call re-entering a tool already
executing in its own ancestry is refused by name. A child is admitted only while
its parent's history row is `dispatching`, and the ancestors join the live
mutation chain passed to `requireNoActiveToolMutation`, so the child stays in
the one live mutation chain rather than opening a second.

Two parts of this section are not met, and both stay requirements. The
intersection is taken against the grant, the descriptor, policy, target/session
authority and lease/fence, but not against a durable *root effect envelope*:
`tool_runs` stores no envelope, which is the same gap section 5.4's checkpoint
records. And the root run's elapsed and call-count budgets are not charged per
child; what bounds a child is the per-parent `maximumChildCalls` ceiling and the
session-scoped Agent budgets section 5.3's item 6 describes.

### 3.4 Root idempotency

Every Agent, human, or automation root request carries a stable request key in
an authenticated caller namespace. Operator creates at most one run for one
key. The same key plus the same canonical request preimage returns or follows
that run; the same key plus different material is a conflict. Workbench/CLI and
Agent adapters must persist the key before sending, so a client crash cannot
mint a second effect tree while retrying.

A different key represents new user intent, not a transport retry. It undergoes
fresh admission and cannot reuse a consumed observation, approval, plan, lease,
fence, or unresolved mutation chain from the earlier run. While any delivery is
unresolved, Operator rejects every new mutating root for the same controlled
target; changing actor or idempotency namespace is not an escape hatch. The
first generation deliberately uses this conservative target-wide mutation lane
instead of defining finer conflict scopes.

## 4. Framework Luau SDK

Framework publishes an immutable SDK under the reserved `@umbraflow/` resolver
namespace. Project modules cannot shadow it, and no request falls through to a
filesystem, working directory, environment variable, registry, network, or
package search path.

The resolver reserves the package alias `umbraflow`. When locked packages are
implemented, a lock file that attempts to claim that alias must be rejected
before hashing, and Framework-name resolution must never fall through to the
locked-package or Project module resolver. No package or lock mechanism exists
in the tree yet — the `@alias/...` grammar is refused outright — so this is a
constraint that release R2 of
[Project Luau modules, resources, locked packages, and explicit capabilities](2026-08-20-project-luau-module-vfs-capabilities.md)
must satisfy, not a rule anything enforces today.

### 4.1 Pure modules

The first SDK generation provides at least:

- `@umbraflow/text`: normalization, case folding, trimming, whitespace
  collapse, splitting, tokenization, and deterministic match helpers;
- `@umbraflow/utf8`: validation, code-point traversal, length, slicing, and
  stable classification;
- `@umbraflow/json`: parse, encode, and immutable value helpers;
- `@umbraflow/jcs`: canonicalization and canonical equality;
- `@umbraflow/collections`: map/filter/fold, stable sort, set/list helpers, and
  immutable updates; and
- `@umbraflow/result`: one composable success/error vocabulary.

Internal checkpoint in the working tree on 2026-08-22: `@umbraflow/jcs`,
`@umbraflow/collections`, `@umbraflow/result`, `@umbraflow/json`, and the
Unicode-15.0 `@umbraflow/utf8` and `@umbraflow/text` modules are live through the
reserved resolver. Their hand-maintained algorithms require generated, embedded
data through Framework-only internal module names. `@umbraflow/jcs` now exports
`encode`, `null` and `equals`, so canonical equality has shipped beside
canonicalization: `modules/task/runtime/jcs.luau` compares two canonical
encodings rather than walking structure, and raises rather than answering
`false` for a value that has no canonical form and therefore no canonical
equality. `tests/task/test-sdk-canonical-equality.cpp` is its coverage. The
bullet above is met in the tree and remains the requirement.

The six pure modules are also the only Framework modules a `PureDataProgram`
admits. `pureFrameworkScriptModules()` deliberately excludes the four scoped
facades, so their names are unresolvable on the pure path — the property section
8 depends on.

Pure module calls create no ToolInvocation, consume no Tool-call budget, and
have no access to the current execution scope. Exported tables and reachable
mutable values are deep-frozen.

Text behavior must be platform-independent. Unicode normalization, case
folding, and classification use data pinned by the Framework release rather
than host locale or operating-system Unicode tables.

Runtime JCS is a pure utility for Project-owned values. It does not author or
validate registration, release, ledger, or other Framework identity bytes;
those canonical bytes remain produced and verified by trusted offline/native
code, with Python retained for the existing build and data pipelines.

### 4.2 Scoped modules

The first scoped set provides at least:

- `@umbraflow/tools`: discovery, description, root/child call, and result
  access through the current Tool Runtime context;
- `@umbraflow/screen`: ergonomic wrappers over Framework observation and
  capture Tools plus snapshot-handle lifetime/result helpers;
- `@umbraflow/workflow`: bounded wait, delivery classification, human stop,
  child-flow composition, and recovery helpers with no blind retry; and
- `@umbraflow/audit`: project-semantic records automatically attributed to the
  current run and call.

These modules are frozen facades whose closures carry the current run scope.
They are not present outside an admitted Tool handler or automation run, cannot
be retained into another run, and expose no native primitive except by making a
normal Tool call.

[`Scoped Tool execution is a third program type`](../decisions/2026-08-22-scoped-tool-execution-is-a-third-program-type.md)
fixes how that is built, and this plan implements it as ruled:

- The scoped set is a property of a new program type, `script::ScopedToolProgram`.
  `script::PureDataProgram` is not extended and keeps its stated invariant that
  no host installer or native capability seam is part of its API; the
  five-function ProjectPlugin path and the Journal reducer keep running on it
  unchanged. Both Project automation scripts and Project Tool handlers run on
  `ScopedToolProgram`. The closed-graph compiler, host-owned resolver and
  deep-freeze machinery are factored into shared internals, not copied.
- `ScopedToolProgram`'s environment-hash preimage covers the scoped module
  catalog and the Tool Runtime facade contract — arity, result shape, failure
  behaviour — and is distinct from `pluginEnvironmentMaterial()`.
- The four modules above are Luau sources in the Framework bundle, exactly like
  the pure modules. Boot builds one private capability table holding a single
  `invoke` C closure, hands it to each scoped module as its chunk argument, and
  drops it. Every wrapper, bounded wait, delivery classification, recovery
  helper and result helper is pure Luau over `invoke`. Discovery and description
  in `@umbraflow/tools` is a frozen data table baked at program construction
  from the pinned catalog bytes, not a native call.
- `invoke` is synchronous: it runs `ToolRuntimeExecutor::invoke*` to completion
  and returns the decoded result. There is no yield protocol, no coroutine
  suspension and no async completion callback. Blocking is bounded because every
  waiting primitive is a Tool with a validated bounded duration. The existing
  `deepFreeze` rule that `__index` must be a table and never a function stands;
  its rationale mentions a "future yield protocol", and that phrase is option
  preservation, not an unpaid debt this plan owes.
- In `@umbraflow/tools`, a "root call" is a call the run's own issuing context
  issues: its position names the run's existing `ToolRootRequestIdentity` as its
  parent coordinate. A "child call" is one a handler's context issues, naming
  that handler's own position row. No position is parentless, and the root
  request itself gets no `tool_call_positions` row — inventing a shadow position
  row per root request would be a second spelling of the root, so the write path
  instead proves the named coordinate exists in whichever of the two tables
  holds it, with a named refusal on either miss. This corrects the "parentless
  position" wording of
  [`scoped Tool execution is a third program type`](../decisions/2026-08-22-scoped-tool-execution-is-a-third-program-type.md),
  which
  [`a Tool handler and an automation script are one mechanism`](../decisions/2026-08-22-handler-and-automation-script-are-one-mechanism.md)
  supersedes on exactly that word. Scripts never mint root request identities.
- Tool results are references, not payloads, on the template of the existing
  `framework.screen.observe` opaque snapshot handle. Handles are plain JSON data
  wrapped by frozen helper functions; there is no userdata and no metatable
  machinery, so replay equality is byte equality of recorded JSON.
- "Cannot be retained into another run" is structural and must not be backed by
  a runtime generation token. The fresh per-invocation VM and its per-VM module
  cache, the single decoded-JSON egress that cannot carry a closure, and the
  dropped unnameable capability table close every channel between them. A
  generation token would be a branch whose failure path can never be exercised.
- The `invoke` upvalue borrows run-scoped C++ state. The run context owns the VM
  as a member, so the lifetime contract is one line at the declaration and is
  enforced by member order.
- Cancellation is teardown, not unwind. A cooperative stop arrives as the
  recorded outcome of a Tool call; a hard stop signals a stop token into the
  provider and destroys the VM without resuming the script, deliberately
  indistinguishable from a crash. No third path returns a cancellation error
  into script code. Waiting and input providers take the stop token, the Luau
  interrupt hook is armed with the same token, and the run executes on a
  structured worker owned by the run context.
- `@umbraflow/audit` is Luau sugar over a genuine `framework.audit.record` Tool
  with a Framework Tool Catalog descriptor, invoked through
  `ToolRuntimeExecutor::invoke`. The record, auto-attributed by the
  seam-supplied root and position, is the call's own durable outcome, and the
  provider performs no external work. Read-only here means no external-world
  effect requiring plan authority, so audit needs no `OperatorPlanAuthority` and
  no `ProposedEffect`, but it does spend Tool-call budget. Audit records are not
  attached as evidence on the current call.
- Scoped-call budget weight is per-descriptor policy carried in the catalog. If
  a scoped operation later proves too expensive, the lever is that weight —
  policy inside the one path — never an exemption from the path.

Internal checkpoint in the working tree on 2026-08-22: WP5 has landed. The
program type, the four facades and the single native seam are built as the
ruling fixes them; three requirements of this section are still open, and they
are named at the end of this checkpoint.

`script::ScopedToolProgram` exists in
`modules/script/source/script/scoped-tool-program.hpp` and
`ffi/scoped-tool-program.cpp`. `PureDataProgram` was not extended: its purity
invariant is intact and the five-function ProjectPlugin path and the Journal
reducer still run on it. The shared closed-graph compiler, host-owned resolver,
quota-bound VM and deep-freeze machinery were factored into
`ffi/program-runtime.{hpp,cpp}` rather than copied, and `ffi/pure-data-program.cpp`
shrank to a thin API layer over it. `scopedToolEnvironmentMaterial()` continues
the shared environment material with the scoped module catalog, the scoped
ceilings, and the Tool Runtime facade contract — `invoke` arity, argument shape,
result shape, failure behaviour and the capability-table contract — and is
distinct from `pluginEnvironmentMaterial()` by construction.

The four facades are Luau sources in the Framework bundle:
`modules/task/runtime/{tools,screen,workflow,audit}.luau`, registered under
their reserved names in `k_scopedModuleBindings`. `scopedFrameworkScriptModules()`
is the only list that admits them; they are absent from
`pureFrameworkScriptModules()`, from the trusted `frameworkScriptModules()`
bundle, and from all three project-global whitelists
(`frameworkProjectGlobals()`, `explorationProjectGlobals()`,
`runtimeProjectGlobals()`). Boot builds one private capability table carrying a
single `invoke` closure, deep-freezes it, and holds it only as a VM ref that
nothing in either environment can name; it is handed as the single chunk
argument to exactly the modules the program type's catalog named, so a
Project-authored module's `...` is empty just as it is under a program type with
no native seam. `tools.luau` owns the one spelling of a Tool call and the other
three reach the runtime through it. Discovery and description are
a frozen data table read from the pinned catalog resource
`umbraflow.tool-catalog`, not a native call. `invoke` is synchronous with no
yield protocol, a refusal is terminal VM teardown no `pcall` can observe, and
the run-scoped state it borrows is declared before the VM in `ScopedToolRun` so
member order enforces the lifetime contract.

Three things this section requires are not met. `framework.audit.record` has a
Framework Tool Catalog descriptor and argument validation, but no production
provider answers it, so outside a test's own provider the audit path is a
declared Tool rather than a durable outcome yet. A loader now constructs a
`ScopedToolProgram` outside the tests that first exercised it —
`ProjectToolProgramRegistrar::registerProject` compiles one per Project
registration generation (section 5.1) — and a first-party `ToolRuntimeInvoke`
now binds the facades to `ToolRuntimeExecutor`:
`ProjectToolDispatcher::toolRuntimeSeam()` in
`modules/operator/source/operator/project-tool-dispatch.{hpp,cpp}` is the seam
every program of a registration is compiled with, and it funnels into the same
`ToolRuntimeExecutor::invoke` a Framework provider call already goes
through. What is absent is a production caller: nothing in `ProductLifecycle`
builds a dispatcher or reaches that registrar, so only tests start a run. And
"the run executes on a structured worker owned by the run context" is
unimplemented:
`ScopedToolProgram::invoke` runs on its caller's thread, and only the stop token
and the interrupt hook of the cancellation contract exist.

### 4.3 Resolver and environment identity

The host resolver distinguishes reserved Framework names from canonical
Project names before lookup. Project relative imports continue to resolve only
inside the registered Project closure. Framework modules use an exact release
map and never enter the Project module namespace or its cache keys.

The observable SDK contract enters the Luau environment identity, including:

- exact Framework module identities and native-backed behavior;
- SDK generation and Unicode data version;
- resolver grammar, cache, cycle, failure, and freezing semantics;
- pure versus scoped module availability;
- Tool Runtime facade arity, result, and failure behavior; and
- every relevant execution and error-size limit.

A Project never hand-writes Framework module digests. Publisher and runtime
derive the exact environment identity from the release's own bytes and
behaviour, and registration refuses on exact inequality between the derived
identity and the registered one — the model
[`project execution identity is closure plus environment`](../decisions/2026-08-20-project-execution-identity-is-closure-plus-environment.md)
froze and `plugin_environment_hash` implements.

Internal checkpoint in the working tree on 2026-08-22. The resolver distinction
this section requires is implemented and is now a property of the program type:
`detail::compileClosure` refuses a Framework name outside the reserved
`@umbraflow/` prefix and a Project relative import that escapes its closure, and
`ScopedToolProgram`'s catalog is the only place the four scoped names are
admitted, so a `PureDataProgram` require of one fails in the resolver. That
refusal now names the module — `"pure data require rejected an unknown module: "`
plus the resolved name — which is what E7's "fail by module name" asserts.

Of the environment-identity list above, four bullets are met and two are not.
`plugin_environment_hash` covers exact Framework module identities, per-module
source hashes, project visibility and dependency depth, the freeze and
release-owned budget contracts, the grammar/interrupt/module-failure contracts,
every numeric limit, and the pinned Luau implementation revision.
`scopedToolEnvironmentHash()` covers the scoped catalog and the Tool Runtime
facade contract. Two bullets are unmet. Native-backed *behaviour* enters both
preimages only as fixed contract literals, so a resolver behaviour change can
still ship under an unmoved digest. And the scoped digest still pins nothing
durable. `currentScopedToolEnvironmentHash()` now derives one over
`script::scopedToolEnvironmentMaterial()`, the source hash, depth and Project
visibility of every Framework module the scoped closure admits, and the reserved
name of the pinned catalog resource; the loader stamps it on a
`ProjectToolProgramHandle`. But no registration, manifest or
`ToolExecutionIdentity` carries it, so the scoped module *source hashes* still
enter no registration-level digest and nothing refuses on inequality between a
registered scoped identity and the running one. Both remain requirements, and
the second is what section 9's "Project registration and SessionManifest
transitively pinning the new roots" owes.

**Open question, unresolved as of 2026-08-22.** An earlier wording of this
paragraph read "A Project declares the SDK generation it targets", and no
document settles what that declaration was meant to be. Two readings survive.
Under the first it is an abandoned direction: the registration carries only the
derived digest, exact-equality refusal is the whole contract, and the phrase is
stale. Under the second it is an unimplemented requirement: a Project names a
generation, the publisher resolves that generation's release bytes, and the
derived digest is pinned from them — which would add a registration member and
therefore increment the registration format. No `sdk_generation` symbol exists
in `modules/`, `schema/`, `tests/`, or `docs/PUBLIC-CONTRACT.md`, no work
package assigns one, and neither frozen decision mentions such a declaration.
Resolve this before WP11 regenerates the public contract; until then treat only
the derived-identity paragraph above as binding.

## 5. Replayable Project automation

### 5.1 Project declaration

[`A Tool handler and an automation script are one mechanism`](../decisions/2026-08-22-handler-and-automation-script-are-one-mechanism.md)
fixes this section's declaration, its two halves, what the loader compiles from
them, and the coordinate a run of one occupies. This plan implements it as ruled.

A Tool handler and an automation script are one mechanism with one declaration.
A Project declares an entry an actor may start at the top of a run exactly as it
declares any other Tool: a Tool Catalog descriptor, plus a binding row naming the
closure entry that answers it. There is no second declaration shape and no
`kind` or `is_script` field anywhere. Apply this test to any proposal of one: if
code branches on it, it is a flag; if no code branches on it, it is dead data.
Both are forbidden. What makes an entry startable at the top of a run is that
the actor is admitted to start it, which is the same authority and policy
question that already exists for an Agent-issued root call.

The forcing argument is recorded in full in that decision: a Tool's tuple and a
script's tuple are field for field the same tuple under two sets of names, so
two declaration shapes for it would be the "two spellings of one thing" the
house rules ban. The field correspondence an implementer needs is that the
requested Tool set and profile are the `ChildEffectDeclaration`'s child Tool
names — `childToolNames` — together with its `maximumChildSurface`,
`maximumChildMutability` and `maximumChildRisk` ceilings, and the requested
budget ceilings are its `maximumChildCalls` together with the descriptor's
workflow limits and timeout policy.

One declaration therefore pins all of it, across the two documents section 5.2
keeps apart. The descriptor, inside `tool_catalog_hash`, pins the argument and
result schemas, the requested child Tool set and profile, and every requested
ceiling and budget. The binding row, inside the registration root and outside
`tool_catalog_hash`, pins the entry. The closed Project module closure both are
read against is the registration's own tool closure.

A registration carries **two** closures, not one.
[`One job, one vehicle`](../decisions/2026-08-22-one-job-one-vehicle.md) breaks
the "ONE closure, ONE manifest hash" invariant outright: a registration carries a
reducer closure compiled on `PureDataProgram` and a tool closure compiled on
`ScopedToolProgram`, each with its own module manifest hash, and both slots are
mandatory. There is no absent-means-pure reading and no absent-means-scoped one.
The pure type's whole contract after the flip is the single entry `reduce`;
`derive`, `plan`, `next_step` and `reconcile` die with the contract the Tool
Runtime replaces, and the bridge's exact-export admission runs per closure —
`{plugin_id, reduce}` for the reducer, `plugin_id` plus the binding union for the
tool closure.

Each closure states the entries it exports.
[`A join needs two independent sources`](../decisions/2026-08-22-a-join-needs-two-independent-sources.md)
fixes where that statement comes from: `exportedEntryPoints` is produced by the
authoring path on every authoring build, carried by deployment into the
registration, covered by the manifest hash, cross-checked against the binding
union at load, and proven against the closure's actual exports by the bridge at
execution. The loader may neither derive it from the binding table nor execute
the module to observe it, because either collapse makes the check compare one
source to itself and leaves a refusal no test can make fail. The authoring tool
computes it by running the module in the author's own sandbox — nothing short of
running reads a Luau table's keys — but what ships is a committed, hashed
statement rather than a re-derivation.

None of that is a grant. Every declared bound is an input to Operator admission,
never a permission: the effective Tool set and budgets are the intersection of
the declaration, actor/session authority, Tool descriptors, policy, approvals,
and runtime limits. The loader and registrar bind that tool closure, the SDK
generation, the Tool Runtime generation, the Project Tool Catalog, and the
Project registration before a run starts.

Project code at a bound entry may use normal functions, modules, branches,
loops, and local variables. It receives no ambient filesystem, network, process,
clock, randomness, Host FFI, Binding, Receipt, or native-input primitive.
External work is a Tool call.

A bound entry runs on `ScopedToolProgram`, not `PureDataProgram`. Its one native
seam is the synchronous `invoke` primitive of section 4.2, and the
per-invocation fresh VM, closed module graph, resource closure and deep-freeze
discipline are unchanged from the pure program type.

Three things genuinely differ between the two producers of a run — Operator
admission of an actor's start, and runtime dispatch of an admitted parent call —
and each lives where it cannot become a flag. Who mints the run's coordinate is
resolved in the producer and arrives as a value: dispatch derives the position
from the admitted parent's durable row, a start at the top of a run mints a root
coordinate, and the shared invoke path never asks which kind it is serving. What
feeds the authority intersection is the parent's `ChildEffectDeclaration` plus
its delegation grant in one case, and actor/session authority plus policy in the
other; both producers emit the same effective-envelope value, and the
intersection engine that consumes it is one. Where the result row is written
does not differ at all: a root run is a root-positioned call, so both results are
the terminal durable row of the call the run implements, validated against that
entry's declared result schema.

Internal checkpoint in the working tree on 2026-08-22: the declaration, the
loader and the registrar exist, and nothing starts a run from them. A binding row
is `ProjectToolBinding {toolName, entryPoint}`, and `project_tool_bindings` is a
required member of the ProjectRegistration document — sorted and unique by tool
name, inside the registration root, outside `tool_catalog_hash`, and the reason
`project_registration_format` is 4.
`ProjectToolBindingTable::bind(registration, catalog, exportedEntryPoints)` in
`tool-invocation.{hpp,cpp}` is its only mint, so holding a value of that type is
proof that all three refusals passed: a binding naming an entry the closure does
not export, a Project-provided descriptor with no binding, and a binding no
descriptor covers. Each has its own test in
`tests/operator/test-tool-binding.cpp`, and again through the loader in
`tests/operator/test-project-tool-program.cpp`.

`ProjectToolProgramRegistrar::registerProject` in
`modules/operator/source/operator/project-tool-program.{hpp,cpp}` performs that
join before it compiles anything, then compiles exactly one
`script::ScopedToolProgram` per registration generation over
`bindingTable().entryPoints()`, the sorted and unique union of bound entries. It
returns a `ProjectToolProgramHandle` holding the verified registration, its
catalog owner, its binding table, that one program, the Framework Tool Catalog
hash and the scoped environment identity. It refuses a closure whose module
manifest is not the pinned one, an environment other than the one running, and a
re-registration of the same root; and through the declared-Tool-with-no-binding
refusal it also refuses any registration whose catalog declares a Tool while its
binding table is empty, which is every registration the project directory loader
derives today. Per-entry variance is catalog data the compiler never sees: no
child Tool set, ceiling or budget reaches `compile`, and a test asserts it. The
seam the one program is compiled against is run-stateless as the ruling
requires: `script::ToolRuntimeInvoke` takes the call's coordinate and stop token
as parameters, and `ScopedRunRequest` carries the run's parent position, so
nothing run-scoped is an upvalue of the shared callable. The loader also
renders the pinned discovery projection of both catalogs into the Framework
resource `umbraflow.tool-catalog` that `@umbraflow/tools` reads, so description
costs no Tool-call budget and cannot move under a running script.

Internal checkpoint on what the tree does not meet; each of these stays a
requirement. The wire spellings this section needs now exist, and what is
missing is a project that uses them. The Project Tool Catalog wire schema in
`modules/deployment/source/deployment/project-deployment.cpp` requires
`child_effects` on every row beside `argument_schema` and `result_schema`, with
every member of the declaration required and no absent form, and the Project Kit
scaffold writes the empty declaration out rather than omitting it — so the
requested child Tool set and profile above have a wire spelling on the Project
side and not only on the Framework's. The entry has one too: an authoring
document states `tool_bindings`, `readToolBindings` in
`modules/deployment/source/deployment/project-directory.cpp` canonicalises and
de-duplicates them into `project_tool_bindings`, and it asks neither whether the
catalog declares the named Tool nor whether the closure exports the named entry,
because those are the two halves `ProjectToolBindingTable::bind` refuses a
disagreement between. What no project does yet is bind anything: the scaffold
ships the pure five-function shape and writes an empty array, so the only
non-empty binding table outside a test fixture is still one a test builds.

`exportedEntryPoints` has no wire spelling at all, and the ruling that governs
it is unimplemented. The registration schema
`schema/umbraflow-project-registration-v2.schema.json` declares no member for
it, so nothing carries an authored statement of a closure's exports into the
registration or under the manifest hash; the loader receives the span from its
caller, and the only caller that supplies one is a test that writes the list by
hand. Every clause of
[`a join needs two independent sources`](../decisions/2026-08-22-a-join-needs-two-independent-sources.md)
therefore stays a requirement: authored production, deployment carriage, hash
coverage, and the standing prohibition on the loader deriving or observing it.
A run does start above the loader now, but only from
inside dispatch of an already-admitted call: `ProjectToolDispatcher`
(section 5.3) establishes the issuing context, takes the call through the one
shared path — the same replay, admission, dispatch boundary and budget charging
a Framework provider call already goes through — validates the answer against
the entry's `result_schema` and writes the terminal row, while
`ProjectToolProgramHandle::invokeBoundTool` still deliberately stops at running
the entry. The registrar and the dispatcher are both reached only from tests,
and no producer admits an actor to start an entry at the top of a run, so
nothing yet starts a run that is not already a child of one. That producer stays
a requirement.

### 5.2 Durable call history

The Operator persists each call position as a state machine. At minimum it
distinguishes proposed, admitted, dispatching, confirmed result, proven absent,
possible/unknown delivery, rejected, terminally unresolved, and terminal
failure. The exact transition must be durable before an external effect can
cross its corresponding boundary.

Four identities/records remain distinct:

1. The external root idempotency preimage contains the authenticated caller's
   pre-admission request and is matched only inside that caller namespace.
2. Every internal call position is *addressed* by its ordinal coordinate — root,
   parent position, and the child index the seam assigned. That coordinate alone
   is the replay lookup key. The following are stored attributes of the position
   rather than parts of its address, and are compared field by field at that
   coordinate:

   - run identity, and the identity of the compiled scoped program when Luau is
     the caller;
   - Tool name and version;
   - canonical arguments, JCS-canonicalised before comparison;
   - relevant observation reference;
   - Tool/Project/Framework/environment identities.

   The first attribute mismatch stops the run, and the diagnostic names the
   field that diverged. Folding name and arguments into the lookup key is
   forbidden: a changed argument would then miss the lookup, be indistinguishable
   from a first new call, and execute — converting mandated stop-the-run
   nondeterminism into nondeterministic re-execution.

   The child index is a monotone counter per issuing context — the root
   script's context, and one per live handler invocation — assigned and
   incremented exclusively by the C++ seam. Scripts never see or influence it,
   and no envelope accepts a caller-supplied sequence for a scoped call.
   Per-parent rather than run-global numbering is what keeps each context's
   numbering a function of its own behaviour: a replayed parent's handler never
   runs, so a replayed child must cost its parent exactly one increment
   regardless of how large its subtree was.

3. A call id derived from the position is a content address, not the lookup key.
   Append-only Operator admission-attempt records keyed by call id plus attempt
   number bind the origin and executing principals; session epoch; policy and
   approvals; EffectivePlan; lease and fence; delegation/continuation grant;
   controlled target; and admitted budget snapshot. Replaying code does not have
   to reproduce Operator-selected authority material, and re-admission never
   rewrites a historical attempt.
4. The provider result, error, dispatch state, evidence, and delivery
   classification form the durable outcome keyed by call id. They are never
   inputs to either request identity and never need to be supplied by a
   replaying caller.

The coordinate, counter, and named-field rules are fixed by
[`scoped Tool execution is a third program type`](../decisions/2026-08-22-scoped-tool-execution-is-a-third-program-type.md).
[`A Tool handler and an automation script are one mechanism`](../decisions/2026-08-22-handler-and-automation-script-are-one-mechanism.md)
fixes the rest of the coordinate model: the parent coordinate is never absent, a
root-positioned call carries `parent_call_identity = root_identity` naming its
`tool_root_requests` row, the root request itself gets no position row, and the
issuing context a dispatcher builds is fresh on every entry and numbers from 1
with no persisted next-ordinal anywhere.

Internal checkpoint in the working tree on 2026-08-22:

- `ToolCallState` implements all nine states, and `tool_call_history`'s CHECK
  constraint admits all nine.
- `rejected` still has no producer. Every admission refusal returns before any
  `UPDATE`, no `ToolCallCompletionKind` or `ToolCallReconciliationKind` maps to
  it, and the tests assert the row stays `proposed`. It stays in the list above
  as a requirement; what is missing is the path that writes it.
- The replay lookup queries `tool_call_positions` by
  `(root_identity, parent_call_identity, call_sequence)` and compares the stored
  attributes field by field, so the coordinate-as-key rule holds. Both parts
  that were missing have landed: `divergedToolCallField` names the first
  mismatching attribute — run, framework release, protocol, environment,
  provider kind, project registration, catalog hash, tool name, tool version,
  canonical arguments and their hash, observation reference, call identity — and
  `observation_reference_hash` is a real column carrying a real comparison.
- Per-parent numbering is implemented. `ToolCallIssuingContext` assigns and
  increments the child index, `ToolCallPositionIdentity::create` is private to
  it so no caller can hand the seam an ordinal, and
  `ToolCallIssuingContext::forRoot` and `forHandler` are the two issuing
  contexts this section names.
- The parent coordinate is never absent, so no query switches on its presence,
  and it is now the real durable identity rather than a placeholder ordinal.
  `ToolCallPositionIdentity::m_parentIdentity` is a plain `ContentHash`;
  `script::ToolCallCoordinate::parentPosition` and
  `ScopedRunRequest::parentPosition` are `ContentHash` with no in-class
  initializer, so a run anchored on nothing cannot be spelled and the closure
  admission `compile()` performs builds no request at all — it installs a
  second capability table whose `invoke` records a terminal failure and breaks
  the VM without minting a coordinate, rather than answering "no" on behalf of a
  run that does not exist. `tool_call_positions.parent_call_identity` is
  `NOT NULL`, and the
  sibling-predecessor and coordinate queries bind it directly. A call the run's
  own context issues names the root request; a handler's child names the
  handler's position. `persistToolCallPosition` proves the named coordinate
  exists — the root request row, or the parent position row — with a named
  refusal on either miss, and the schema migration backfilled the rows an
  earlier generation left null with their own `root_identity`.

### 5.3 Restart and replay

Framework never serializes a Luau VM stack. After a process or VM failure it
starts the exact script closure in the exact pinned environment from its entry:

1. a call matching a terminal durable history row receives its recorded result;
2. replay performs no provider execution or external delivery;
3. a mismatch at an existing position is deterministic-replay divergence and
   terminates the run;
4. the first position beyond history may enter normal admission and execution
   only after continuation authorization and uncertain-delivery gates pass;
5. a possible/unknown delivery is returned as that classification and cannot be
   converted to rejection, moved to a new parent/sequence, or silently retried;
   it freezes further mutation in the run until Operator reconciliation; and
6. instruction, memory, wall-time, call-count, mutation, observation, nesting,
   and no-progress budgets apply during both replay and new work.

Tool providers whose result includes nondeterministic evidence must return that
evidence in the recorded Tool result. Replay reuses and verifies the exact
recorded material rather than recomputing it.

Non-terminal matching rows recover by state, rather than pretending they have a
recorded result:

| Durable state | Recovery rule |
|---|---|
| `proposed` | No admission or dispatch occurred; repeat current admission for the same call id. |
| `admitted` with durable proof dispatch never began | Recheck current continuation authority, append a new admission attempt, and dispatch once under the same call id. |
| `dispatching` or provider state with no terminal outcome, for a **mutating direct-effect leaf** — both halves required, and each read off a durable column: `tool_call_positions.provider_kind` names an answerer that reaches the world directly rather than through recorded children (today only `framework`), **and** `tool_call_history.mutating` is 1 | Atomically classify `possible/unknown` and never redispatch that attempt. A trusted idempotent provider query may instead prove `confirmed` or `proven_absent`; only a later explicit call may act after proven absence. |
| `dispatching` with no terminal outcome, for **every call the row above does not name** — a composed call, whose `provider_kind` names an answerer that reaches the world only through recorded children (today `project`, a bound handler), at either mutability; or a direct-effect leaf whose row carries `mutating` 0 | Left `dispatching` and re-entered under the same call id, because neither shape can have moved the world without a durable row saying so. A composed call's bound handler runs again from the top on a fresh issuing context numbering from 1, meets its recorded children without executing them, and executes only the first position beyond history; re-running the handler is replay and not redispatch, because every external effect it caused is one of those recorded children, and that holds however mutating the handler's own descriptor is. A read-only leaf has no recorded children and needs none: it declares no external effect for a delivery to be uncertain about, so running its provider again delivers nothing twice. The restart itself never classifies either `possible`. |
| `confirmed`, `proven_absent`, `rejected`, or terminal failure | Replay the exact durable outcome without provider execution. |
| `possible/unknown` | Replay that classification and keep the target-wide mutation barrier. |

No startup path may infer `proven_absent` merely from a process crash. The
trusted provider reconciliation query and its evidence are themselves durable.

The **first** `dispatching` row is the exception, and the second is simply what
is left once the one non-replayable shape has been named exactly. Which row
applies is decided by two facts the call's own durable rows already carry, never
by a judgement made at recovery time: `tool_call_positions.provider_kind`, which
fixes whether the call is composed of recorded children or reaches the world as a
direct-effect leaf, and `tool_call_history.mutating`, which is the descriptor's
declared mutability copied onto the row at admission. Both are columns, both were
written before the crash, and the first row is their **conjunction**.

Neither half is sufficient alone, and reading either half as sufficient inverts
the rule in one direction or the other. Leaf alone is not enough: a read-only
Framework call is a leaf and is re-entered, because there is no declared effect
for a delivery to be uncertain about. Mutating alone is not enough: a mutating
Project handler's row says mutating and is re-entered, because its whole effect
surface is children that already carry their own classification. So the second
row is not a relaxation granted to a kind of call, and it cannot be read as one:
it governs whatever the conjunction does not, and the conjunction can only widen
by adding an answerer to the closed set of answerers that reach the world
directly — a decision taken once, at that set, rather than a default inherited at
recovery time. Which calls the first row governs is therefore enumerable rather
than judged: today it is exactly the Framework-provided calls whose descriptor
declares them mutating.

The rule is written once and both places that enforce it are derived from that
one statement. The restart's row filter over `tool_call_history` joined to
`tool_call_positions` is *generated* from the same predicate the re-entry gate
calls, across the closed set of (answerer, mutability) pairs, so every pair the
predicate calls unrecordable becomes one disjunct of the filter. The
classification a restart applies and the shape a re-entry refuses cannot
disagree, because there is one statement rather than two kept in step. A
hand-written SQL copy of the rule beside the predicate is forbidden for that
reason: the two are read months apart and only a crash exercises them together.
This is
[`re-entry is keyed on recorded children`](../decisions/2026-08-22-re-entry-is-keyed-on-recorded-children.md),
which this plan implements as ruled.

The reason is not that re-entry is cheaper. A dispatch may be re-entered only
when nothing outside the ledger can have happened that the ledger does not
already classify. The property that decides this is **composed of recorded
children, versus direct-effect leaf** — not read-only versus mutating, and not
Project versus Framework. A composed frame re-enters under this invariant:

> A scoped handler's only channel to the world is a bound tool call; every bound
> call is an admitted, durably recorded child; on re-entry, a child call matching
> the next recorded child consumes the recorded result without live dispatch;
> live dispatch is permitted only past the recorded frontier; a divergence from
> the record refuses the frame loudly.

Under it, re-execution is effect-free up to the frontier, and "no unrecorded
effect" is enforced structurally rather than by audit, because scoped Luau has no
other capability. A call answered by a bound handler is composed: the
handler's only channel to the world is a bound tool call, every bound call is an
admitted, durably recorded child, and each child already carries its own durable
classification — so re-running the handler re-derives the same child coordinates,
meets the recorded rows, and executes only the first position beyond history,
which is this section's replay contract rather than a redelivery. That is
equally true whether the handler's own descriptor is read-only or mutating,
because a mutating handler's mutating work is likewise child calls. A
Framework-provided call is a leaf: it is answered by a provider this side cannot
re-run deterministically, so an unanswered one has no recorded child standing
between the record and the world. Being a leaf is what makes uncertainty
*possible*, and the descriptor's mutability is what makes it *real* — a
read-only leaf declares no effect for the record to be silent about, so the
ledger classifies everything that could have happened by classifying nothing.
[`A Tool handler and an automation script are one mechanism`](../decisions/2026-08-22-handler-and-automation-script-are-one-mechanism.md)
requires exactly this for its "parent `dispatching`, crashed mid-run" case, so
classifying a composed call `possible` would both contradict a frozen ruling and
convert a replayable handler into a barrier nothing can resolve. Nothing else in
this section moves with it: no outcome this section calls uncertain becomes
certain, a `possible/unknown` classification still cannot be converted to
rejection, moved to another parent or sequence, or retried, and rule 5 above
stands unchanged.

**The one non-replayable atom is a leaf mutating provider in flight**: a
recorded child with no recorded result, where the world may or may not have
changed and no record can say. That child stays `possible` and needs its own
protocol — an idempotency key, an effect-confirmation handshake, or operator
adjudication. The parent handler above it re-enters, replays to that child, and
**inherits its unresolved state rather than re-issuing it**; re-issuing at that
point is a duplicate effect, and the fact that the parent is replaying is not
authority to produce one. Re-entry safety is compositional, and the leaf is where
it bottoms out.

Whether a mutating handler may re-enter is **decided**, not deferred to whoever
lands mutating handler dispatch. It may, on the conjunction above, and the
forcing argument is that the alternative is not a decision at all. The
restriction that kept a mutating handler from re-entering lived entirely in what
the recovery pass converted; the re-entry path refused on provider kind alone
and would have admitted a mutating Project call the moment one reached it. So
giving a mutating handler a dispatch path would have widened re-entry **by
default**, with no line of code expressing the choice and no test failing to
announce it. A property acquired that way is inherited silently, and the flip
must not be where it is discovered. Deciding it ahead of the capability is the
only way the decision exists anywhere.

The gate re-key and its pinning test therefore land **before any production
mutating provider exists**, not with the first one. The reclassification of a
mutating Project call to `possible` is exactly what made the admission gate's
mutating-admit path unreachable, so that path becomes reachable with the re-key
and must be exercised rather than inherited green: an admit is not a check, which
is the technicality that let an unreachable path survive the dead-branch rule
without anything going red. The pin is one test: kill after child *N*'s effect
and its record, restart, prove no duplicate effect and prove completion, then
force a divergence and prove the loud refusal.

Two further divergences stop the run: a restarted script that terminates leaving
recorded calls unconsumed for some issuing context, and a call arriving under a
different parent, which manifests as a coordinate mismatch and is reported as a
changed parent.

The frozen ruling's changed-field list names `sequence` alongside tool name,
version, arguments, parent, observation reference, and pinned execution
identity. Under 5.2 the sequence *is* part of the address, so a changed sequence
can only ever surface as a changed-parent or changed-ordinal divergence, never
as a changed attribute at a matched coordinate. It is not a separate field
comparison to implement.

Internal checkpoint in the working tree on 2026-08-22:

- Rules 1, 2 and 3 have a real restart path behind them now, for a dispatched
  handler. `ProjectToolDispatcher` in
  `modules/operator/source/operator/project-tool-dispatch.{hpp,cpp}` is the
  dispatch executor, and its kill-and-restart fixtures in
  `tests/operator/test-tool-dispatch.cpp` cover a terminal parent whose handler
  never runs, an admitted parent dispatching against empty history, a handler
  left mid-flight that re-enters and executes only past history, and a
  fenced-out incarnation that can neither re-enter nor complete. Rule 4's
  continuation gates are not what re-entry runs — it re-reads the live binding
  and lease and refuses a different origin principal or controlled target, which
  is narrower than section 5.4's recheck. Rule 5 now has a dispatch path and
  coverage: `tests/operator/test-tool-executor.cpp` drives a mutating call to
  `possible`, proves the target-wide freeze, and proves that only a trusted
  query's evidence lifts it. That path is not reachable from a scoped script,
  because `ProjectToolDispatcher` builds no mutation on the seam and so no
  mutating call can be issued from Luau; the mutating calls under test are
  admitted directly. Nothing starts a run at the top of a root either, so all
  six rules over a *root* run remain untested and stay requirements.
- Rule 3 is still a per-call refusal, not a run-level kill. Divergence is
  detected *and named by field* at both replay sites, and both sites still
  return before any write; what has changed is that the returned divergence is
  now converted into the exact terminal-failure outcome of the call that was
  running, so a diverging handler stops and the field that diverged is inside
  its own durable row. That is still one call, not the run: `tool_runs` has no
  state column and nothing marks a root terminated. "Terminates the run" stays
  as the requirement.
- The first of the two further divergences at the end of this section is
  implemented. `OperatorCoordinator::sealToolCallContext` closes an issuing
  context at handler teardown and refuses it when a recorded call beyond the
  context's own issued count is left unconsumed, naming the ordinal; the
  dispatcher calls it on every bound-entry run and
  `tests/operator/test-tool-dispatch.cpp` proves it across a restart. The second
  is unchanged: a call arriving under a different parent is caught — it is a
  coordinate miss — but it is reported as an absent position rather than as a
  changed parent.
- The gate re-key has landed and the two `dispatching` rows above are what the
  tree does. The rule has one statement: `toolCallEffectMayBeUnrecorded` in
  `modules/operator/source/operator/ledger.cpp` is true exactly when a call's
  composition is `DirectLeaf` and its declared mutability is `Mutating`.
  `reenterToolCallDispatch` calls it to refuse, and
  `unrecordedEffectDispatchFilter` *generates* `recoverUncertainToolCalls`'s row
  filter from the same predicate by evaluating it over every (answerer,
  mutability) pair, so the restart writes `possible` only to
  `(history.mutating=1 AND position.provider_kind='framework')` and the two
  enforcement points cannot disagree. Composed-versus-leaf is not judged either:
  the closed `k_toolAnswerers` table is the one place the durable
  `provider_kind` vocabulary is joined to what an answerer can do, and it rests
  on the load-time refusal of a Project descriptor with no binding
  (`ProjectToolBindingTable::bind`), which is what makes `provider_kind='project'`
  mean "answered by a bound scoped entry". The crash/replay pin landed with it:
  `tests/operator/test-tool-dispatch.cpp` kills a *mutating* handler after a
  child's effect and its record and proves no duplicate effect and completion,
  proves the loud refusal on a forced divergence, and proves that a restart
  classifies the mutating leaf uncertain while leaving every other dispatch
  re-enterable. The admission gate's mutating-admit path is reached by that
  first test rather than inherited green.
- The trusted-provider-query seam exists. `OperatorCoordinator::reconcileMutatingToolCall`
  takes a `ToolReconciliationQuery` and **invokes** it, outside any ledger
  transaction and only after it has proven the row `possible`, the descriptor
  mutating, and the binding, lease and durable run authority live; it then
  writes the answer's outcome and its mandatory evidence under a
  `state='possible' AND mutating=1` CAS. `proven_absent` now has producers on
  both sides — a mutating provider may report it at completion, and a
  reconciliation answer may reach it — and
  `tests/operator/test-tool-executor.cpp` covers both. No startup path reaches
  the query, so the section's "no startup path may infer `proven_absent`" still
  holds by construction.
- The read-only `possible` gap is closed, and how it closed matters: **no
  resolution path for a read-only `possible` was added — its two producers were
  removed.** The restart was one producer, and the conjunction above ended it: a
  read-only leaf is no longer converted. Completion was the other, and
  `completeToolCallDispatch` now refuses a read-only row that reports `possible`
  or `proven_absent`, on the ground that both are claims about whether an
  external effect landed and a read-only Tool declares none. `Possible` is
  therefore structurally mutating-only, and reconciliation — mutating by
  construction, because only a mutation can be uncertain — can resolve every row
  that can reach it. A resolution path for a read-only `possible` is no longer a
  requirement, because no such row can be minted; if a future change gives one a
  producer it must supply the path in the same change.
- Completion-time classification is still keyed on **mutability alone**, and
  re-keying it on the same composed-versus-leaf property is a requirement this
  section now owns. `completeToolCallDispatch` refuses terminal failure for any
  row whose `mutating` column is 1, and `ToolRuntimeExecutor::invoke` converts a
  mutating provider's returned error or terminal failure into `possible`. Both
  read the descriptor's mutability and neither reads the answerer. For a
  mutating *handler* — whose whole effect surface is its own recorded children,
  each already terminally classified — that turns an outcome the ledger can be
  certain about into a target-wide barrier that only reconciliation can lift.
  The property that decides a restart decides this too, and re-keying completion
  on it is the natural completion of this cut. Whoever lands production mutating
  providers must decide it; until then a mutating handler that fails cleanly is
  over-classified.
- A fixture-reality mismatch to convert before anything leans on it. Several
  cases in `tests/operator/test-tool-executor.cpp` build a *Project*-provided
  call — `mutatingProjectCall` mints a position whose `provider_kind` is
  `project` — and answer it with an arbitrary lambda `ToolProvider` rather than
  through a bound entry. Production cannot reach that shape: a Project
  descriptor with no binding is refused at load, so `provider_kind='project'`
  means "answered by a bound scoped entry" everywhere outside those fixtures. It
  is harmless today because the executor is provider-agnostic and the restart
  rule reads the durable column rather than the callable. It stops being
  harmless the moment a change leans harder on "project-provided implies bound
  handler" — a completion re-key on composition would be exactly such a change —
  and those fixtures must be converted to real bound handlers in the same change
  rather than after it.
- Item 6's budget set is far from met. For a Script controller none of it
  applies: the controller profile sets `budgetsRequired=false`, admission
  refuses an `AgentProfile` for that kind, and every charge is skipped. For an
  Agent only call-count, mutation, observation, risk units and a wall-clock
  deadline exist; there is no instruction, memory or nesting budget, and
  no-progress is read only on the legacy Operation path. The budgets that do
  exist are session-scoped, keyed by `session_id`, not run-scoped. Item 6 stays
  as written; the gap is implementation, not specification.

### 5.4 Continuation authorization and uncertain delivery

A durable run record fixes the immutable origin principal identity/objective,
target, Project, script, requested ceilings, admitted root effects, and cumulative
budget consumption separately from the current automation or handler execution
principal. Returning an existing recorded outcome does not consult a live lease
because it performs no work. Before the first new provider dispatch after
restart, Operator rechecks the current session, actor/profile, policy,
approvals, lease, fence, target, registration, plan, and remaining budgets. The
new admission may only preserve or narrow the original run; it cannot change
the objective, target, Project, script, or Tool catalog.

The current session must authenticate the same origin principal. A restarted
automation or handler execution principal may be recreated under its pinned
identity, but a different origin principal must create a new root and cannot
attach to the old call history.

Historical admission rows retain their original session epoch. After restart,
a continuation may bind a newly active session epoch, lease, and fence when the
current authenticated authority forms a valid non-expanding intersection with
the durable run record; this does not rewrite historical admission. If no such
current re-admission can be formed, the run remains replayable for audit/result access
but cannot execute new work. A newly authorized different intent must start
under a new stable root key and fresh references; it does not reinterpret the
old run.

Any possible/unknown mutating child freezes mutation for the whole root tree.
Only a Framework-owned reconciliation Tool/transition may consume fresh Host
evidence and classify the original call as confirmed, proven absent, or
terminally unresolved. `confirmed` and `proven_absent` resolve the ambiguity;
`terminally_unresolved` ends the run but deliberately leaves its target-wide
barrier in place until trusted evidence resolves it or Operator durably retires
that controlled-target generation without reusing its authority.
Read-only observation needed for reconciliation may continue under a distinct
bounded allowance, but no new mutation, alternate parent, new sequence,
conflicting mutating root, or dependent Journal commit is admitted while
frozen.

Internal checkpoint in the working tree on 2026-08-22 — four parts of this
section are still unimplemented, and every one of them stays a requirement:

- Neither release path for `terminally_unresolved` exists.
  `reconcileMutatingToolCall`'s CAS is `WHERE ... state='possible'`, so a
  terminally unresolved call cannot be reconciled afterwards, and there is no
  controlled-target-generation retirement API anywhere in `operator`. The
  barrier is therefore a permanent irreversible freeze rather than one held
  until trusted evidence or durable retirement releases it. E8 asserts the
  releasable behaviour and cannot pass today.
- Continuation admission compares only the eight `tool_runs` columns —
  controller id and kind, controlled target, project registration hash, and the
  four execution identities. The run record stores no policy hash, capability
  profile, or admitted root effect envelope, so a restarted continuation at a
  *new* sequence under the same root is admitted under whatever policy and
  profile the current session holds, including a widened one. Non-expansion is
  enforced only when re-admitting the same already-admitted call. Section 12's
  "restart cannot reuse expired authority or expand the original root envelope"
  is not satisfiable until the run record carries that envelope.
- The dependent-Journal-commit clause is unimplemented.
  `requireNoActiveToolMutation` has exactly two call sites, `submitCommand` and
  `admitToolCall`; `commitReconciliation` and its `journal_events` insert never
  consult it.
- `framework.workflow.reconcile` is now a Framework Tool Catalog descriptor —
  read-only, `Privileged`, taking exactly one `call_identity` content hash — but
  no provider answers it, and it is still not what `reconcileMutatingToolCall`
  consults. That half stands. What the transition consults has changed: it no
  longer stores a caller-supplied outcome verbatim. It takes a trusted
  `ToolReconciliationQuery` and **invokes** it over the durable call position the
  Coordinator itself holds, after proving the row `possible`, the descriptor
  mutating, and the binding, lease and durable run authority live — so a querier
  cannot choose which call an answer is about — and every reconciliation kind
  carries mandatory evidence that is stored beside the outcome it produced. What
  is still missing is the binding of that evidence to a snapshot, observation,
  Host generation or lease-fresh capture, and a Framework-owned Tool with a
  provider standing where the in-process query stands. The evidence is durable
  and mandatory rather than verified. "Only a Framework-owned reconciliation
  Tool/transition may consume fresh Host evidence" therefore stays a
  requirement, now with a narrower gap than a declared Tool beside an unbound
  field.

## 6. Observation and action authority

`framework.screen.observe` produces a snapshot-scoped reference bound to target,
Host generation, RuntimeArtifact, Project registration, frame identity, and
expiry/budget state. An Agent or human may inspect the returned view; Project
Tools consume the reference through Framework resolution rather than trusting
caller-copied observation JSON.

`observe` captures only when called. It makes no stability or action-completion
claim. Requested wait duration is selected by the caller but remains subject to
Operator timeout and run budgets. Game-specific transition handling belongs in
Project recognition or automation, not in a fixed Framework capture interval.

Project interpretation may return snapshot-local semantic target references.
A Framework input Tool validates those references against the same snapshot,
Binding, plan, lease, fence, and action bounds before Host delivery. At most one
native input consumes one observation authority. A stale, foreign,
cross-registration, duplicate, already-consumed, or parent-mismatched reference
is refused.

Whether a profile may receive raw image bytes is an offered-Tool and policy
decision. It does not change the reference or input-authority rules and does not
give Project Luau a direct frame pointer.

Internal checkpoint in the working tree on 2026-08-22: the reference and its
refusal matrix exist. `SnapshotObservationReference` and
`SnapshotObservationAuthority` live in
`modules/operator/source/operator/snapshot-reference.{hpp,cpp}`, and
`ObservationRefusal` enumerates eleven distinct verdicts — `Unminted`,
`AlreadyConsumed`, `Stale`, `ForeignTarget`, `ForeignRegistration`,
`ForeignRuntimeArtifact`, `ChangedGeneration`, `MissingParent`,
`DuplicateLocal`, `UnknownLocalTarget` and `ActionRefused` — each with its own
comparison and diagnostic rather than one lumped "invalid reference". Every
named E5 attack has a verdict, a refusal spends nothing so an action refused on
its bounds leaves the authority available, and the single-consumption rule is
the authority's own spend. `tests/operator/test-tool-snapshot-reference.cpp` is
its only caller: no Framework input Tool provider resolves a reference through
it yet, so the boundary is built and unwired. The input-authority sentences
above stay as requirements on the providers that will use it.

## 7. Project Tool handlers

The Project Tool Catalog remains the descriptor and argument-schema authority.
The replacement generation additionally binds each Tool to its exact handler
entry/function and module closure. A handler receives canonical arguments and a
scoped SDK environment, returns a schema-validated Tool result, and may make
admitted child calls.

Handler globals and module state do not carry durable truth. A handler that
needs durable project facts proposes Journal events; one that needs operation
progress relies on its call tree and recorded child results. A restart replays
the handler on the same terms as a top-level automation script, and on the same
terms whether the handler is read-only or mutating:
[`re-entry is keyed on recorded children`](../decisions/2026-08-22-re-entry-is-keyed-on-recorded-children.md)
keys that on the handler's effect surface being exhausted by its own recorded
children rather than on its declared mutability, and section 5.3 states the
recovery rule it fixes.

Journal proposals are persisted only as call-bound provisional records against
the exact call-tree and outcome revision; they are not Journal events or durable
Project facts at that stage. A proposal depending on an effect cannot commit
while that effect or any relevant child is possible/unknown. Once every
referenced terminal outcome and evidence object is already durable, Operator
runs the reducer over the frozen prior state and proposed batch. One final CAS
transaction re-verifies those outcome/evidence identities and the prior
revision, appends the Journal events, and publishes the reducer result. A crash
before that CAS publishes neither fact nor state; a crash after it observes
both.

The same Project Tool must have the same behavior and authority checks whether
its caller is Agent, human, another Project Tool, or a Project run started at the
top.
[`Caller independence is structural`](../decisions/2026-08-22-caller-independence-is-structural.md)
rules that this is a property of the structure and not only of a test: every
producer builds one internal admission-request value — actor identity, target
tool name, canonical arguments, issuing coordinate context — consumed by exactly
one admission function, and the position identity, delegation grant and dispatch
capability downstream of it are constructible only inside the runtime, so an
adapter is definitionally a translator that can construct nothing executable.
The one semantic fixture is still required and is not a substitute: it compares
canonical argument bytes, admission outcome, durable row attributes, and result
across all four producers, modulo actor identity, because structure cannot see
whether two producers canonicalise the same input the same way. Both are
required, and neither discharges the other.

Internal checkpoint in the working tree on 2026-08-22: the binding, the compiled
program and the path that runs a handler all exist; no producer starts one from
outside a call, and neither half of the caller-independence ruling is finished.
The binding is not a `ToolDescriptor` member and is not meant to become one —
it is `project_tool_bindings` on the ProjectRegistration, beside
`tool_catalog_hash` and outside it, and the module closure it names is the
registration's own, so
"which code answers this Tool" now has an answer a durable row could record.
`ProjectToolProgramRegistrar` joins the two and compiles one
`script::ScopedToolProgram` per registration generation over the bound entries
(section 5.1). The answer half of an entry's data contract is now declared as
well: `result_schema` is required on every Tool Catalog row beside
`argument_schema`, and a deployment naming a definition its tool-precondition
schema does not declare is refused.

A handler now runs, and it runs on the one path. `ProjectToolDispatcher`
(`modules/operator/source/operator/project-tool-dispatch.{hpp,cpp}`) constructs
the issuing context for a handler invocation from that call's own durable
position, runs the bound entry with its canonical arguments, judges the answer
against the `result_schema` its entry declared through
`ProjectToolProgramHandle::validateToolResult`, and writes the terminal row —
all of it inside `ToolRuntimeExecutor::invoke` with the bound handler
supplied as the provider, so replay, admission, the durable dispatch boundary,
budget charging and the terminal write are the same code a Framework provider
call goes through. "Returns a schema-validated Tool result" is therefore
enforced by an executed path, and "a restart replays the handler on the same
terms as a top-level automation script" is met for the handler half by re-entry
(section 5.3). The nested-call machinery of section 3.3 is what a handler's
child calls take, and the dispatcher's two seams — `dispatch(...)` for a
producer and `toolRuntimeSeam()` for the `script::ToolRuntimeInvoke` every
program of a registration is compiled with — are the same private
`dispatchCall`, which is what keeps one Tool calling another from being a second
path.

What this section still owes. Nothing above dispatch admits an actor to start a
handler at the top of a run, so the "whether its caller is Agent, human, another
Project Tool, or a Project run started at the top" claim has only one producer
to compare, and no test compares the actor paths against a shared semantic
fixture. There are no call-bound provisional Journal proposals. Neither half of
the caller-independence ruling is finished, though the two halves stand
differently. The private-mint half is largely in the tree already:
`ToolCallPositionIdentity::create` is private to the issuing context, and
`ToolCallAdmission`, `ToolCallDispatch` and `ToolDelegationGrant` each have a
private constructor befriended only to `OperatorCoordinator`, so an adapter can
construct none of them — but they are copyable values rather than the move-only
capability the ruling names. The funnel half does not exist at all:
`OperatorCoordinator::admitToolCall` still takes controller, lease, root,
position, mutability, plan authority, effects, approvals and delegation as nine
separate parameters, and there is no one internal admission-request value for a
producer to build. Both halves, and the fixture, stay as this section states
them.

## 8. Journal reduction

Reduction remains structurally separate from online Tools and computes a
candidate over a frozen prospective commit:

```text
reduce {
  prior_project_state,
  commit_context { prior_revision, next_revision },
  prospective_journal_batch
} -> candidate opaque ProjectState
```

The reducer loads only pure Framework SDK modules and registered Project
resources. `@umbraflow/tools`, `screen`, `workflow`, and `audit` are absent. It
cannot observe a live frame or call a Tool. Framework independently validates
the returned state, revision, hash, and prospective batch. The final Operator
CAS simultaneously marks that exact batch committed and publishes the candidate
state; reducer execution alone commits nothing.

Their absence is a property of the program type, not of a reducer-specific
admission list: the reducer runs on `PureDataProgram`, whose resolver has no
scoped module in it at all, while scoped code runs on `ScopedToolProgram`. That
is what makes E7's "fail by module name" a load-time resolver property rather
than a runtime check.

After the flip, `reduce` is the **whole** of the pure type's contract.
[`One job, one vehicle`](../decisions/2026-08-22-one-job-one-vehicle.md)
retires `derive`, `plan`, `next_step` and `reconcile` with the contract the Tool
Runtime replaces, so the pure registrar's expected entry set becomes `{reduce}`
and the exactly-five admission check and its pins die with it. The resolver
refusal above is explicitly **not** in that deletion set: it is retargeted onto
the single-entry reducer type in the same change, because it is what makes the
reducer's isolation a property of the type rather than a comment, and losing it
with the four entries would delete the evidence for this section's whole claim.

Internal checkpoint in the working tree on 2026-08-22, on two points:

- The reducer's freedom from scoped modules is no longer vacuous, and it is a
  property of the program type exactly as this section requires. The four scoped
  facades exist, and the reducer cannot name one: it runs on `PureDataProgram`,
  whose Framework closure is `pureFrameworkScriptModules()`, and that list
  deliberately excludes them — `scopedFrameworkScriptModules()` is the only list
  that admits them. The refusal is now a load-time resolver refusal that names
  the module: `"pure data require rejected an unknown module: "` plus the
  resolved name, in `modules/script/source/script/ffi/program-runtime.cpp`. E7's
  last sentence is met and asserted: `tests/task/test-scoped-framework-modules.cpp`
  compiles a `PureDataProgram` reducer fixture that requires each of the four
  scoped names in turn and checks the refusal carries that name. The rest of E7
  — the prospective batch, the crash windows either side of the final CAS, and
  byte-identical replay — is unrun.
- The implemented reduce envelope is still exactly
  `{"journal_events":[...],"prior_project_state":...}`. Neither `commit_context`
  nor a separately identified prospective batch exists anywhere first-party;
  `commit_context` appears in no source, schema or test. The block above is the
  target envelope and WP9's trusted commit context is the work that produces it.

## 9. Contract and identity cut

Implementation requires one atomic generation across:

- Framework and Project Tool catalogs and tool-handler binding;
- actor-neutral ToolInvocation and nested-call envelopes;
- Operator ledger call-tree, dispatch, delivery, and replay rows;
- the one entry declaration — Tool Catalog descriptor plus binding row — with
  its loader, registrar, and scoped execution environment identity;
- reserved Framework SDK resolver and exact module catalog;
- scoped Tool Runtime facades;
- screen observation/reference and input Tool contracts;
- Project registration and SessionManifest transitively pinning the new roots;
- policy, approval, budget, lease, and recovery paths;
- Workbench/CLI and Agent adapters;
- Project Kit scaffolds and generated Project Tools/scripts;
- conformance, public contract, publisher, and consumer migration.

The old five-function registration and persisted sessions remain audit-only.
When the cut closes there is no dual entry table, optional Tool bridge, fallback
dispatch, or replay under a different SDK generation.

**The registration break is part of the cut, not a consequence of it.**
[`One job, one vehicle`](../decisions/2026-08-22-one-job-one-vehicle.md) makes a
registration carry two closures with two manifest hashes and shrinks the pure
type's contract to `{reduce}`, so "ONE closure, ONE manifest hash" is broken
outright in this generation rather than adapted. Keeping `derive`, `plan`,
`next_step` and `reconcile` alive beside tool dispatch past the flip would be two
generations of one job — project orchestration — simultaneously reachable with
something choosing between them per project, which is the forbidden
compatibility path of section 9.1 word for word, and a promise to delete them in
a later commit does not change what exists in the interval.

**At the flip commit, only mechanics remain.** Rewire the production entry to
the root producer; delete the five-function registrar, `ProjectPluginHandle`, the
old provisioning path, the four dead entries and their pins; refold-verify — or
regenerate-and-state — every instance row and re-stamp it; nothing new lands.
Four preconditions make that one commit rather than a series leaving a mixed
state: every new-generation path is already test-reachable end to end; every
consumer's new call site is already written, because the flip switches call sites
and does not author them; the deletion set is enumerated up front as a greppable
inventory; and no selector exists at any instant before the commit, the new
generation staying test-only to the last moment. Five items must land before the
flip while reading like something else, and section 13 sequences them: the
section 5.3 gate re-key, which reads as mutating-provider work; the refold-verify
harness, which reads as flip tooling but must pass beforehand or the flip cannot
prove the claim it makes; authored `exportedEntryPoints` production, without
which flip pressure invites the loader-derivation that
[`a join needs two independent sources`](../decisions/2026-08-22-a-join-needs-two-independent-sources.md)
forbids; the data migration of already-authored Tool names under section 3.2;
and retargeting the pure-resolver refusal test onto the single-entry reducer type
so it survives the deletion of the five-entry pins.

**What provisions a scoped-generation Project, and why the cut owns it.** No
session can be pinned for a Project that has no `project_instances`
row: `pinSession` looks the registration-hash and instance-key pair up and
refuses outright when it is absent. The only path that writes that row is
`OperatorCoordinator::provisionProjectInstance`, which requires the exact pinned
`ProjectPluginHandle` and reduces the baseline through it — that is, a closure
admitted under `PureDataProgram` against all five of `derive`, `plan`,
`next_step`, `reconcile` and `reduce`. A Project whose closure is genuinely
scoped — bound handler entries plus a reducer, and none of the other four — can
therefore hold no session at all, and a Project that cannot hold a session can
drive nothing. This is visible today only as a test workaround: the dispatch
fixture registers one closure twice against one registration, once as the pure
plugin its instance is provisioned from and once as the scoped program the
dispatcher runs, and names its handler entries after the five plugin functions
so the same bytes admit under both program types.

[`Provisioning rekeys to the registration generation`](../decisions/2026-08-22-provisioning-rekeys-to-the-registration-generation.md)
answers it, and this plan implements it as ruled. The question is not answered by
deleting the five-function path: the ProjectInstance row and its reduced baseline
are Journal state that outlives the entry surface that happens to produce them
today, so something must still establish an instance and its initial state for a
Project that exports no `derive`, `plan`, `next_step` or `reconcile`. The row
therefore survives in shape — instance identity plus reduced baseline — and its
provenance moves from `ProjectPluginHandle` to the **registration generation**,
which is already the unit of compilation and now carries both compiled programs.
`provisionProjectInstance` becomes a function of the registration;
`ProjectPluginHandle` dies with the old registrar; `pinSession` chains session to
instance to generation, with no step naming a superseded type.

The stored baselines are proved rather than re-stamped. The reducer's logic
survives the flip unchanged — the same authored fold recompiled on the shrunk
pure type — so the flip commit must **refold each instance's admitted-batch
history through the new-generation reducer and assert byte equality with the
stored baseline**, then re-stamp the row to the generation. Determinism is what
makes that a real proof that changing the vehicle changed nothing, and where the
equality holds, migrating and regenerating are the same act. Where the Journal
does not retain the fold inputs, do not re-stamp unverified bytes across a
trust-boundary rewrite: regenerate from what is retained, state the recomputed
baseline as the new truth — nothing is released, so restating is available — and
say in the commit message which of the two happened. The refold-verify harness
is built and passing on test data **before** the flip commit is written; it is a
precondition of WP10 rather than a consequence of it.

### 9.1 What makes the intermediate state legitimate

The list above cannot land in one commit, so for the length of the cut the new
generation exists in the tree beside the live one. That is not a violation, and
a reader must not mistake it for drift.
[`Production reachability is the cut invariant`](../decisions/2026-08-22-production-reachability-is-the-cut-invariant.md)
draws the line:

- A **forbidden compatibility path** is two generations simultaneously reachable
  from the production entry, with a selector choosing between them.
- A **legitimate intermediate state** is new-generation code reachable only from
  tests.

Two rules are checkable at every commit of this plan. Exactly one generation is
production-reachable, and the gate passes. And no commit introduces a selector —
no flag, no version field, no dual entry table, no "if the new catalog is
present" branch, not even temporarily. The moment a runtime value chooses a
generation, the forbidden thing exists regardless of the intent to remove it.
Tests referencing dark code are not a dual entry table; the rule forbids a
mixed-generation runtime, and tests are not the production runtime.

WP10 is the flip: one terminal commit rewires `ProductLifecycle` to the new
generation, migrates every consumer, and deletes the superseded paths in the
same change. Deferring the deletion by even one commit produces a commit in
which both generations are reachable.

State in the working tree on 2026-08-22: exactly one generation is
production-reachable, and the new generation has grown a great deal without
becoming reachable. Every production Tool call still runs the legacy
`submitCommand` -> `freezePlan` -> `mintNextStep` -> dispatch pipeline through
`ProductLifecycle::execute`, and `ProductLifecycle::observe` serves the CLI
observe verb directly. `ProductLifecycle` gained no new entry point: it still
exposes exactly one Tool Runtime function, `invokeFrameworkReadOnlyTool`, whose
sole caller is `tests/cli/test-observe.cpp`.

Everything added since — `script::ScopedToolProgram` and the four scoped
facades, the eight-tool Framework Tool Catalog, `ToolDelegationGrant` and the
nested-call admission path, `SnapshotObservationAuthority`,
`ToolRuntimeExecutor::invokeMutating`, and now `ProjectToolDispatcher` with the
handler re-entry path it drives — is reached only from
`tests/script/test-scoped-tool-program.cpp`,
`tests/task/test-scoped-framework-modules.cpp`,
`tests/operator/test-tool-nested-calls.cpp`,
`tests/operator/test-tool-snapshot-reference.cpp`,
`tests/operator/test-tool-executor.cpp` and
`tests/operator/test-tool-dispatch.cpp`. The dispatcher is dark by construction
rather than by omission: nothing in `ProductLifecycle` builds one, and the
Framework Tools a scoped run reaches are answered by a provider its caller
installs rather than by a registry the module owns, so there is no production
composition to disable. No selector exists anywhere: no flag, no version field,
no dual entry table, no catalog-presence branch. Both rules of this section
hold, and a reader must not read the size of the dark generation as evidence
that the cut has happened. It has not; WP10 has not started.

Nothing is released, so no consumer's bytes have to be migrated at the flip, and
where development-time ledgers exist they are regenerated rather than taught to
read two row shapes. That freedom does not extend to the reduced baselines:
[`provisioning rekeys to the registration generation`](../decisions/2026-08-22-provisioning-rekeys-to-the-registration-generation.md)
requires each one to be refolded through the new-generation reducer and proven
byte-equal before it is re-stamped, or regenerated with the commit message
saying so. Regenerating a ledger that nothing derives state from is cheap;
re-stamping a baseline every later revision is derived from without recomputing
it is the one place "nothing is released" would buy an unchecked claim.

## 10. Required experiments

### E1 — Same game sequence, three callers

Drive one read-only interpretation and one bounded mutation using Agent, a human
test adapter, and a Project automation script. All three must produce the same
internal ToolInvocation shapes, authority verdicts, and project-semantic
results, differing only in actor identity/profile material.

### E2 — Natural Luau automation loop

Run a multi-step Project script that observes, calls Project interpretation and
planning Tools, requests Framework input, chooses a bounded delay, observes a
transition frame, repeats wait-observe-recognize, reconciles, and terminates.
The source must use an ordinary loop and shared SDK modules, with no hand-written
external `advance` state machine or Framework-imposed capture interval.

### E3 — Crash and deterministic replay

Crash before admission, after admission, before provider dispatch, after proven
delivery, after possible delivery, after result persistence, and before the
next call. Restart must reuse exact recorded results, never redeliver a possible
effect at another position/parent, and detect one changed replay argument at
the assertion naming it. Repeat the root request after a client-side crash with
the same idempotency key and prove it rejoins the original run; change the
preimage under that key and prove conflict. Mint a different root key and actor
for the same controlled target after possible delivery and prove the durable
target-wide barrier still refuses mutation.

### E4 — Nested authority

Have a Project Tool call a read-only Project child, a Framework observation
child, and an admitted Framework input child. Attack with authority expansion,
new project/target, missing approval, recursion, excessive depth, and exhausted
root budget. Prove that direct low-level Tool visibility is unnecessary for an
approved high-level Tool, while removing the parent child-effect declaration or
Operator-compiled root envelope makes the nested call fail.

### E5 — Snapshot-reference isolation

Attack Project interpretation and Framework input with stale, foreign-target,
foreign-registration, changed-generation, missing-parent, duplicate-local,
already-consumed, and action-refused references. No rejected action may publish
stronger observation or Journal facts than its evidence proved.

### E6 — SDK identity and determinism

Use `text.normalize`, UTF-8 classification, JCS, stable collections, Project
relative modules, and scoped SDK modules on every supported platform. Moving
one SDK module byte, Unicode table, resolver rule, or native behavior must move
the environment identity. Locale and host Unicode differences must not move
results.

### E7 — Journal/live parity

Propose one and multiple events, freeze an Operator-admitted prospective batch,
compute its candidate state, and crash before and after the final CAS. Before
CAS, neither the events nor state are committed; after CAS, both are visible at
one revision. Replay the automation and reducer and prove byte-identical state
and framework revision. Reducer attempts to load scoped modules must fail by
module name.

### E8 — Resume authority and uncertain-outcome freeze

Restart after actor revocation, approval expiry, lease loss, fence change, and
session-epoch replacement. Existing rows remain replayable without effects; the
first new dispatch is refused unless current authority is durably re-admitted as
a non-expanding continuation by the same origin principal. Prove a different
principal cannot attach to the old run. After a possible mutating child, attempt
mutation at the next sequence, another parent, a restarted handler, another
root/actor on the same target, and a new dependent Journal commit; all remain
frozen. Prove `terminally_unresolved` does not release the barrier, while
confirmed, proven-absent, or durable retirement of the old controlled-target
generation does.

## 11. Work packages

1. Specify exact Tool, actor, nested-call, result, and replay envelopes.
2. Extend Operator persistence with root idempotency, origin/execution
   principals, continuation grants, call-tree/replay state machines, and
   uncertain-outcome mutation freeze.
3. Implement actor adapters for Agent, human Workbench/CLI, and automation.
4. Publish the first pure Framework SDK modules and cross-platform identity
   tests.
5. Implement `script::ScopedToolProgram`, its single synchronous `invoke` seam,
   and the Luau scoped SDK facades over it, including the
   `framework.audit.record` Tool the `@umbraflow/audit` facade wraps.
6. Implement Project Tool handler binding and nested calls.
7. Implement the one entry declaration, its loader and registrar, deterministic
   restart, and replay.
8. Implement Framework observation and input Tools over existing Host/runtime
   authority.
9. Keep reducer execution pure and add trusted commit context.
10. Replace ProjectPlugin five-call schemas, bridge, registration generation,
    service choreography, scaffolds, examples, and conformance atomically.
11. Regenerate the public contract and publish one matching release.
12. Migrate consumers only against that release and run release-facing
    positive, negative, tamper, replay, recovery, and production-admission
    gates.

Internal checkpoint in the working tree on 2026-08-22. No work package is
deleted or reworded by this paragraph; it only says where each one stands.

- WP1 is partly done and partly unwritten. The internal envelopes exist as C++
  types — `ToolRootRequestIdentity`, `ToolCallPositionIdentity`,
  `ToolCallIssuingContext`, `ToolExecutionIdentity`, `ToolDelegationGrant`,
  `ToolCallState` — and the result envelope is now declared per entry, because
  `result_schema` is required on every Tool Catalog row. The published side has
  moved only for the registration: `project_tool_bindings` is stated in
  `schema/umbraflow-project-registration-v2.schema.json` and carried into
  `docs/PUBLIC-CONTRACT.md`. Neither of those still states a Tool Runtime call
  state, envelope or identity preimage, and `tool_runtime_protocol_identity`
  remains an opaque caller-supplied hash with no published definition.
- WP2 is substantially done: root idempotency, origin and execution principals,
  delegation grants, the nine-state call machine, the ordinal replay key, the
  admission-attempt ledger and the target-wide mutation freeze all exist, and a
  root-positioned call is now an ordinary position row rather than a null parent
  — `tool_call_positions.parent_call_identity` is `NOT NULL` and names the root
  request row, with every row an earlier generation left null migrated to its
  own `root_identity`. Restart recovery is now keyed on the conjunction section
  5.3 states: `recoverUncertainToolCalls` classifies a `dispatching` row
  uncertain only when it is a mutating direct-effect leaf, through a row filter
  *generated* from the same predicate `OperatorCoordinator::reenterToolCallDispatch`
  calls to refuse, and re-entry admits everything else — a composed call at
  either mutability, and a read-only leaf — while still refusing to widen the
  admission it re-enters (section 5.3). The trusted provider-query seam landed
  with it: `reconcileMutatingToolCall` invokes a `ToolReconciliationQuery` after
  proving the row `possible`, the descriptor mutating, and the binding, lease
  and durable run authority live. Its remaining gaps are the ones sections 5.2,
  5.3 and 5.4 name — no `rejected` producer, no run-level termination
  (`tool_runs` still has no state column, so a divergence stops one call and not
  the run), no admitted root effect envelope on `tool_runs`, neither release
  path for `terminally_unresolved`, and the completion-time classification that
  section 5.3 now requires re-keying onto the same composed-versus-leaf property
  that decides a restart.
- WP3 has not started. There are no Agent, Workbench/CLI or automation adapters.
- WP4 is done except for the cross-platform identity tests, which do not exist:
  the identity is computed and asserted on the host that runs the suite, and
  nothing yet compares it across platforms. `@umbraflow/jcs` canonical equality
  has since landed (section 4.1).
- WP5 is done. See section 4.2's checkpoint for what landed and the three
  parts of it that are still unwired.
- WP6 is done except for the authoring path. Nested calls are implemented
  (section 3.3); Project Tool handler binding is too — the binding table is a
  registration member, the loader joins it against the pinned catalog and the
  exact closure with three refusals, and one program per registration generation
  is compiled from it (section 5.1); and the dispatch that runs a bound handler
  now exists — `ProjectToolDispatcher` establishes the issuing context, takes
  the call through admission and the durable dispatch boundary, validates the
  answer against the entry's `result_schema` and writes the terminal row
  (sections 5.3 and 7). The authoring path now has a wire spelling — an
  authoring document declares `tool_bindings` and the directory loader carries
  them into `project_tool_bindings` — so what is still missing is a project that
  binds anything: the Project Kit scaffold writes the empty array, and nothing
  states `exportedEntryPoints`, which stage REGISTRATION of section 13 owns
  (section 5.1).
- WP7's declaration, loader and per-handler restart are done; restart and
  replay of a run started at the top are not. Under section 5.1 there is no
  separate automation declaration left to build — an entry an actor starts at
  the top of a run is a catalog descriptor plus a binding row, and the loader
  and registrar that bind it to the SDK generation, the Tool Runtime seam, the
  Project Tool Catalog and the Project registration exist. Deterministic restart
  and replay now exist for a dispatched handler: a killed incarnation re-enters,
  its context is fresh and numbers from 1, recorded children replay without
  executing, an unconsumed recorded call and a diverged attribute each stop the
  run with the offending ordinal or field named, and a fenced-out incarnation
  can neither re-enter nor write (section 5.3). The same rules over a run an
  actor started at the top are untested and unbuilt, because no producer starts
  one.
- WP8 is descriptors only. All eight Framework Tool descriptors exist and the
  snapshot-reference boundary exists, but only `framework.screen.observe` and
  `framework.workflow.wait` have providers, and no input Tool resolves a
  reference through `SnapshotObservationAuthority`.
- WP9 is half done: reducer purity is now enforced by the program-type split
  (section 8), and the trusted commit context does not exist.
- WP10, WP11 and WP12 have not started.

## 12. Planning-stage acceptance

Implementation starts only after an independent review confirms:

- the three actor paths enter one Tool Runtime;
- no scoped SDK module exposes a second Host or delivery path;
- pure SDK behavior and Unicode data are environment-pinned;
- nested calls cannot expand authority or escape root budgets;
- root retry and deterministic replay cannot repeat a possible or confirmed
  effect at a new run, sequence, or parent;
- restart cannot reuse expired authority or expand the original root envelope;
- snapshot references cannot cross target, generation, registration, or call
  authority;
- reducer execution cannot load Tool capabilities;
- Journal publication cannot outrun the terminal Tool outcomes it interprets;
- the identity cut covers Framework SDK, scripts, handlers, catalogs, and
  replay protocol; and
- the plan contains no game-specific branch or mixed-generation compatibility
  path.

An implemented refusal with no test is not verified capability, and staging a
subsystem dark does not exempt it: section 9.1's rules make an unreachable
subsystem legitimate, not reviewed.

Internal checkpoint in the working tree on 2026-08-22. Unit coverage of the dark
generation grew with it — `tests/operator/test-tool-nested-calls.cpp`,
`tests/operator/test-tool-snapshot-reference.cpp`,
`tests/operator/test-tool-binding.cpp`,
`tests/operator/test-project-tool-program.cpp`,
`tests/operator/test-tool-dispatch.cpp`,
`tests/script/test-scoped-tool-program.cpp` and
`tests/task/test-scoped-framework-modules.cpp` are all new and all registered in
`tests/CMakeLists.txt` — and none of it is conformance coverage. The same three
refusals still have neither a caller nor a test: the accept-side discovery
checks in `admitToolCall`, the origin-principal continuation gate, and the
root-namespace binding. `modules/conformance/` names no Tool Runtime type,
table or entry point at all, so the "runnable cases" the consumer contract is
made of still describe only the legacy generation. Neither
`docs/PUBLIC-CONTRACT.md` nor `schema/` describes any Tool Runtime call state,
envelope, or identity preimage. The generated contract now publishes the scoped
facade surface — the four reserved scoped module names, their exports and source
hashes, and the statement that they are a different contract from the pure
modules — but that is the Luau environment, not the runtime protocol, and
`tool_runtime_protocol_identity` still has no published definition. WP11 and
WP12's release-facing gates therefore still have nothing to gate against. Each
of these is required before the flip, not after it.

## 13. Implementation handoff checkpoint

Implementation has reached the scoped program type, the four scoped facades, the
eight-tool Framework Tool Catalog, nested Tool calls, the snapshot-observation
refusal matrix, the Project Tool binding table inside the registration root, the
root-positioned call row that replaced the null parent, the loader and registrar
that compile one program per Project registration generation, the dispatch
executor that runs a bound handler from an admitted call to its terminal durable
row — those four as
[`a Tool handler and an automation script are one mechanism`](../decisions/2026-08-22-handler-and-automation-script-are-one-mechanism.md)
fixes them — and the restart re-key that decides which crashed dispatches
re-enter, as
[`re-entry is keyed on recorded children`](../decisions/2026-08-22-re-entry-is-keyed-on-recorded-children.md)
fixes it. All of it is production-unreachable; see section 9's state
paragraph.

**Gate state.** The `fe6dbe0` breakage this checkpoint used to report was that
`scripts/generate_public_contract.py` expected two-field `PureModuleBinding`
rows while `modules/task/source/task/framework-bundle.cpp` emitted three. That
repair is in the working tree: the generator now reads a binding's fields
through `braced_span` rather than a fixed arity, reads the shared closed-graph
constants out of `ffi/program-runtime.{hpp,cpp}`, and has its own
`ScopedModuleBinding` pattern feeding a separate scoped section;
`docs/PUBLIC-CONTRACT.md` carries that section. The dispatch executor's sources
and its test are likewise in the working tree, and so are stage RE-ENTRY's: the
one-statement rule and its generated filter in
`modules/operator/source/operator/ledger.cpp`, and the crash/replay coverage in
`tests/operator/test-tool-dispatch.cpp` and
`tests/operator/test-tool-executor.cpp`. Whether the whole gate passes is
not established by this checkpoint — nothing here ran it. Confirm `GATE: PASS`
before treating any unstarted step below as startable.

Continue in the dependency order fixed by
[`production reachability is the cut invariant`](../decisions/2026-08-22-production-reachability-is-the-cut-invariant.md),
building each stage dark — complete, compiled and conformance-tested with no
production caller — until the flip. Completed steps stay in the list: the order
that was followed is as much a part of the handoff as what remains.

Three things have moved since the previous checkpoint. Identity and registration
shape landed first — the binding table inside the registration root and outside
`tool_catalog_hash`, the required per-tool `result_schema`, the root-positioned
call row, and the recorded fixtures migrated in the same change — because every
later step pins on those digests, and changing hash inputs late invalidates
every fixture and replay row built on top, which is the most expensive rework
available. The loader and registrar of step 5 landed next, ahead of step 4.

The dispatch executor has now landed as well:
`modules/operator/source/operator/project-tool-dispatch.{hpp,cpp}` carries an
admitted call in `dispatching` to an issuing context keyed on its own durable
parent position, a program invoke with canonical arguments, result-schema
validation and a terminal row write, and `tests/operator/test-tool-dispatch.cpp`
is the kill-and-restart coverage for fresh-from-1 replay, the
mismatch-names-the-field halt, the unconsumed-recorded-call halt and fence
exclusion. Two seams reach it and they are one path: `dispatch(...)` for a
producer, and `toolRuntimeSeam()` as the `script::ToolRuntimeInvoke` every
program of a registration is compiled with. Both funnel into
`ToolRuntimeExecutor::invoke` with the bound handler supplied as the
provider, so it is the shared bottom of every caller path and it replaces step
3's fake-executor coverage for a Project Tool. There is one provider protocol
and one seam rather than a read-only and a mutating pair: which of the two a
call is comes from the descriptor inside its coordinate, so a second callable
would only be a second spelling of one signature.

**The order was re-cut again by the five rulings the header's second group
names, and the stages below are the sequence to follow from here.** They
supersede the previous re-cut, which ran dispatch executor, root producer, actor
adapters, production mutating providers, flip. The work is the same; the gating
is not. What moves a stage now is a ruling rather than a scheduling preference,
and every stage is still built dark under the two rules of section 9.1. These
stages are named rather than numbered, because the numbered list further down
this section is the separate bottom-up dependency order and its step numbers do
not correspond.

- **Stage NAME — the Tool-name tightening and its ownership enforcement.**
  Section 3.2's grammar, the positive ownership check, the deletion of the
  negative refusal, and the rename of every fixture, shipped example and recorded
  name. **First**, because every artifact authored afterwards is born valid,
  while done late it re-migrates everything landed in between; it is also the
  smallest and the only fully independent stage. *Landed — section 3.2's
  checkpoint says what is in the tree.*
- **Stage REGISTRATION — the registration break and authored
  `exportedEntryPoints`, together.** The two closures with two manifest hashes
  and the single-entry pure contract of
  [`one job, one vehicle`](../decisions/2026-08-22-one-job-one-vehicle.md),
  and the authored, hashed statement of each closure's exports that
  [`a join needs two independent sources`](../decisions/2026-08-22-a-join-needs-two-independent-sources.md)
  requires. **One cluster**, because the registration schema and its only
  production producer must move together. Test-reachable only.
- **Stage PROVISIONING — provisioning rekeyed to the generation**, per
  [`provisioning rekeys to the registration generation`](../decisions/2026-08-22-provisioning-rekeys-to-the-registration-generation.md),
  with the refold-verify harness built and passing on test data. Depends on
  REGISTRATION, because the generation it rekeys to is the one that stage
  defines.
- **Stage RE-ENTRY — the restart re-key**, per
  [`re-entry is keyed on recorded children`](../decisions/2026-08-22-re-entry-is-keyed-on-recorded-children.md):
  the restart classification, the gate re-key, and the crash/replay pin of
  section 5.3. Independent of REGISTRATION and PROVISIONING, and **a hard
  precondition for MUTATING PROVIDERS**. *Landed — section 5.3's checkpoint says
  what is in the tree, including the two open items it leaves behind: the
  completion-time classification is still keyed on mutability alone, and the
  executor fixtures that answer a Project-provided call with a lambda must be
  converted before anything leans on "project-provided implies bound handler".*
- **Stage MUTATING PROVIDERS — the production mutating providers**, only after
  RE-ENTRY, whose landing is what unblocks this stage. This is where the sequence
  differs from the previous re-cut, which placed them after every adapter. What
  moved them is not a relaxed risk judgement but a precondition that did not
  exist before: without RE-ENTRY, a mutating handler dispatch inherits re-entry
  silently and the admission gate's mutating-admit path is unreachable and
  untested. Everything the earlier reading protected still holds: a production
  mutating provider stays production-unreachable until the flip like every other
  stage, and the stages around it still run end to end against test providers
  whose doubles are cheap. The two open items RE-ENTRY left are this stage's to
  decide, because a production mutating provider is the first caller that makes
  either of them observable.
- **Stage ROOT PRODUCER — Operator admission of an actor's start at the top of a
  run**, constructing the same admission request with a root coordinate, built
  against the post-REGISTRATION schema and proven end to end by test: author,
  deploy, register, provision, pin, dispatch, crash-replay, reduce, baseline. It
  is a stage of its own because the moment it exists automation scripts exist and
  no separate feature remains to build.
- **Stage ADAPTERS — the actor adapters**, as thin translators onto the one
  internal admission request
  [`caller independence is structural`](../decisions/2026-08-22-caller-independence-is-structural.md)
  requires, with that ruling's four-way semantic fixture landing with the second
  adapter and growing with each. Each consumer's new-generation call site is
  written and test-proven here and left dormant from production, because the flip
  switches call sites and does not author them.

Then the flip. The read-only providers of the numbered list's step 4 are
unblocked by the dispatch executor and may land at any point once it exists, but
its two `framework.input.*` providers belong to stage MUTATING PROVIDERS. The
minimum chain for the first real Project Tool end to end is identity and
registration shape, the loader and registrar, and the dispatch executor, plus one
admission producer, plus a provider only if that Tool's children need one.

Nothing yet admits an actor to start an entry at the top of a run, so every run
in the tree is still a child of a call some test admitted by hand.

1. **Mostly done** — identity, envelope and ledger row types, and their
   conformance coverage. The types exist and carry unit coverage, and the
   registration shape every later step pins on is settled:
   `project_tool_bindings` inside the registration root and outside
   `tool_catalog_hash`,
   `project_registration_format` 4, and a root-positioned call as an ordinary
   `tool_call_positions` row whose `parent_call_identity` names its own root
   request row, with the rows an earlier generation left null migrated in the
   same change. The *conformance* half still does not exist at all (section 12), and
   neither `schema/` nor the public contract publishes a Tool Runtime envelope
   or call state. This step is not closed.
2. **Done** — Framework and Project Tool catalogs and their validation. The
   Framework catalog is eight tools with descriptors, argument validation and a
   catalog identity that covers the child-effect declaration; every catalog row
   now declares a `result_schema` beside its `argument_schema`, and a deployment
   naming a definition the tool-precondition schema does not declare is refused;
   Project catalog validation refuses duplicates. The two rules this step used to
   be missing have both landed. The positive namespace binding of section 3.2 is
   enforced by one `validateToolNameOwnership` that the Operator and the
   authoring tier both call, and the negative reserved-prefix refusal it made
   unreachable is deleted, its premise now stated once at registration-claim
   validation. And `child_effects` is a required member of the Project catalog
   wire schema with every sub-member required and no absent form, so a Project
   descriptor's child-effect declaration is a stated document rather than empty
   by construction (section 5.1).
3. **Done** — `ScopedToolProgram`, its resolver, and the scoped facades, tested
   against a fake executor. `tests/script/test-scoped-tool-program.cpp` and
   `tests/task/test-scoped-framework-modules.cpp` are that fake-executor
   coverage. Section 4.2's checkpoint lists the three parts of the ruling this
   step left unwired; the seam half of the second has since been built, because
   the dispatch executor is the first-party `ToolRuntimeInvoke` those facades
   are compiled against for a Project registration — but no production caller
   reaches it, and the other two are untouched.
4. **Unstarted, and unblocked** — providers, and the observation and input
   contracts. It is the earliest step of this list nothing has started. Its
   read-only half may land at any point now, but it is split across two stages of
   the re-cut order above: the two `framework.input.*` providers are stage
   MUTATING PROVIDERS, whose one gate — stage RE-ENTRY — has landed, while the
   rest is gated on nothing.
   `SnapshotObservationReference`/`Authority` and the eleven-way refusal matrix
   landed ahead of this step and are its input; what is missing is the provider
   side. `framework.audit.record`, `framework.screen.capture`,
   `framework.workflow.status`, `framework.workflow.reconcile` and both
   `framework.input.*` Tools are descriptors no production provider answers, and
   nothing resolves an observation reference through the authority. WP6's
   nested-call admission landed alongside step 1's ledger work rather than after
   this step, and step 5's loader and registrar and the dispatch executor landed
   ahead of it as well, so nothing here waits on any of them. The dispatcher
   takes the Framework provider as a constructor parameter, so this step
   replaces a test double rather than reopening the dispatch path.
5. **Started ahead of step 4** — registration and manifest pinning, then policy,
   budget, lease, and recovery. The registration half has landed:
   `ProjectToolProgramRegistrar::registerProject` binds the verified
   registration, its pinned Tool Catalog, its exact module and resource closure,
   its binding table, the scoped SDK generation and the Tool Runtime seam into
   one compiled program per generation, and each of the three bind-time refusals
   carries a test (section 5.1). Its first debt has moved and is not paid:
   `currentScopedToolEnvironmentHash()` now has a consumer and stamps the loaded
   handle, but no registration or manifest carries that digest, so nothing
   refuses on inequality between a registered scoped environment and the running
   one (section 4.3). The recovery half has moved: section 5.3's restart
   classification, its re-entry gate, the generated filter that keeps the two in
   one statement, and the crash/replay pin under them are stage RE-ENTRY and are
   in the tree, and `reconcileMutatingToolCall` invokes a trusted query rather
   than storing a caller-supplied outcome. Policy, budget and lease are
   untouched, and so is the rest of recovery — continuation authority, the
   admitted root effect envelope on `tool_runs`, and either release path for
   `terminally_unresolved` (section 5.4).
6. **Not started, and its root producer is the next stage to build** — adapters
   and Project Kit scaffolds. WP6's other half, the Project Tool handler
   binding, is no longer among what is missing here: it landed with step 5's
   loader, as a registration member rather than a `ToolDescriptor` field
   (section 5.1), and the dispatch that runs a bound handler has landed too.
   What this step still owes is the producers — first the producer that admits
   an actor's start at the top of a run (the moment it exists, automation
   scripts exist and no separate feature remains to build), then the Agent and
   human Workbench/CLI adapters, each a thin translator onto one internal
   admission request. The authoring half has moved and is not finished. The
   directory format through which a project states a handler entry exists: an
   authoring document declares `tool_bindings` and `readToolBindings` carries
   them into `project_tool_bindings`. What no scaffold does is use it — the
   Project Kit scaffold ships the pure five-function shape and writes an empty
   array — and no authoring document states `exportedEntryPoints` at all, which
   is stage REGISTRATION's other half rather than this step's (section 5.1).
7. WP10, the flip: rewire `ProductLifecycle`, migrate every consumer, and delete
   the superseded paths in one commit, followed by cross-repository review,
   synchronization, publication, one final full CI, commits, and pushes.

This order supersedes an earlier reading that placed the `screen` and `workflow`
facades first. That reading could not work while the Framework catalog was two
read-only tools wide, with no capture, status, or input Tool, while
`ToolDescriptor` carried no child-effect declaration — which section 3.3 makes a
precondition for facade-issued child calls — and while nothing bound a Tool to
the code that answers it. All three now exist in the tree alongside the facades:
the eight-tool catalog and `ToolDescriptor::childEffects` are step 2's work and
are what made step 3 writable, and the binding landed as a registration member
rather than a descriptor field (section 5.1).

The facades that resulted wrap Tools that mostly have no provider yet, which is
step 3 working as specified rather than a shortcut — step 3 says "tested against
a fake executor", and a facade proven against a fake executor is what makes step
4's providers replaceable without reopening the facade. It is also the reason
step 4 cannot be skipped: until it lands, `@umbraflow/screen` and
`@umbraflow/workflow` are shaped correctly over a catalog that cannot answer
them.

Every commit in the sequence must satisfy the two rules of section 9.1: exactly
one generation production-reachable with the gate passing, and no selector
introduced.

Until all implementation is complete, run only affected incremental tests; run
the full CI exactly once at the end. Do not use `git pull`. Concrete
implementation continues through medium-effort agents and is reviewed and
accepted by the main agent. Python remains an offline data-processing tool for
Unicode, unpacked database, and artifact inputs; the final runtime and Project
artifact are Luau.

Resolver follow-up risks remain explicit, and one of the three has moved.
`plugin_environment_hash` now carries each pure Framework module's reserved name
and dependency depth, so the pure tier's topology is pinned at the registration
level. `frameworkBundleHash()` still does not: its recipe is module name, one
NUL, module source, so a changed alias or depth alone leaves it unmoved. The
four scoped modules' names, depths and source hashes now enter
`currentScopedToolEnvironmentHash()`, which the loader stamps on the program
handle — but that digest reaches no registration-level digest, so the scoped
tier's topology is still unpinned where it would be refused. The full and pure
resolver grammars are still separately implemented —
`validFrameworkResolverName` in `ffi/environment.cpp` beside the module grammar
in `ffi/program-runtime.cpp` — and can drift. Every new release-owned
dependency, scoped or pure, must declare its resolver alias and dependency
depth.
