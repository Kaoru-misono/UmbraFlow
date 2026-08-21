# Unified Tool Runtime, Framework Luau SDK, and replayable Project automation

Date: 2026-08-21
Scope: `operator`, `service`, `script`, `deployment`, `task`, Agent bindings,
Workbench/CLI, schemas, Project Kit, conformance, release publication, and
consumer migration
Status: **owner-approved direction; internal implementation in progress**

The ruling is frozen in
[`tools are the shared game-driving boundary`](../decisions/2026-08-21-tools-are-the-shared-game-driving-boundary.md).
This live plan owns the experiments and atomic implementation. The current
five-function ProjectPlugin code and generated public contract remain the
executable contract until this plan's replacement generation lands in full.

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
provider adapter and explicitly refused mutating admission at that point.
Nested delegation, public actor adapters, Project handlers/automation, and the
public contract generation remain to be implemented.

The first caller-neutral executor now owns terminal fast-path replay, live
read-only admission, durable dispatch, exactly one provider call, conversion of
provider errors into canonical terminal failures, and final outcome replay.
`ProductLifecycle` now attaches the production `framework.screen.observe` and
`framework.workflow.wait` providers to that seam. The authenticated controller
binding supplies the root namespace; observe returns an opaque durable snapshot
reference plus pinned resolution metadata, wait uses the caller's validated
bounded duration, and exact terminal replay performs neither operation again.
Public actor adapters and Project providers remain to be implemented.

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
VM. Exact SDK module names/source hashes,
resolver semantics, freeze behavior, and limits now move
`plugin_environment_hash`. UTF-8 traversal and Unicode 15.0 major-category
classification are pinned in `@umbraflow/utf8`, and normalization and case
folding are pinned in `@umbraflow/text`. Strict JSON parsing, deterministic
encoding, empty object/array identity, and immutable value updates are pinned in
`@umbraflow/json`; all scoped modules remain pending. Their Luau algorithms are
maintained as runtime source, while generated Unicode data lives in embedded
Framework-internal modules that Project source cannot require directly.

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

Every root and child call has one durable identity and carries at least:

- run and root-invocation identity;
- authenticated caller idempotency namespace and stable root request key;
- call sequence and `parent_call_id` when nested;
- origin actor kind/profile and current executing principal identity;
- Tool name and version;
- canonical arguments;
- Framework release and Tool Runtime protocol identity;
- Project registration and Tool Catalog identity when a Project is involved;
- automation-script and Luau environment identity when Luau is the caller;
- observation/snapshot reference when the Tool consumes one;
- EffectivePlan, approval, lease, and fence material required for a mutation;
- inherited and remaining budgets;
- an external root idempotency preimage derived only from authenticated
  caller-supplied request material;
- an internal call-position fingerprint over caller/runtime material fixed
  before admission;
- separately stored append-only Operator admission-attempt records; and
- a separately stored durable dispatch, result, and delivery classification.

Agent, human, and automation adapters may have different transport envelopes,
but they must compile into this one internal invocation. No adapter gets a
second execution path.

### 3.2 Namespaces and discovery

Framework owns `framework.*`. A Project owns its registered namespace, normally
its `plugin_id`. Tool discovery returns only what the current actor may see. A
descriptor that is not offered may not be invoked by spelling its name anyway.

Framework initially needs read-only observation/capture/status Tools, a
separate bounded wait Tool, and input Tools capable of consuming
snapshot-scoped semantic targets.
Bare coordinates and other low-level input remain privileged Tool surfaces;
being a Framework Tool does not make them generally available.

Project Tools carry game interpretation, recognition, planning,
reconciliation, and high-level goal execution. Their exact handlers, schemas,
descriptors, and resources are registration-pinned.

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

The resolver reserves the package alias `umbraflow`. A lock file that attempts
to claim it is rejected before hashing, and Framework-name resolution never
falls through to the locked-package or Project module resolver.

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

Internal checkpoint: `@umbraflow/jcs`, `@umbraflow/collections`,
`@umbraflow/result`, `@umbraflow/json`, and the Unicode-15.0 `@umbraflow/utf8`
and `@umbraflow/text` modules are live through the reserved resolver. Their
hand-maintained algorithms require generated, embedded data through
Framework-only internal module names.

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

A Project declares the SDK generation it targets, not author-written module
digests. Publisher and runtime derive the exact environment identity from the
matching release bytes and behavior.

## 5. Replayable Project automation

### 5.1 Project declaration

A Project may declare one or more automation scripts. Each declaration pins an
entry, closed Project module closure, arguments/result schemas, requested Tool
set/profile, and requested budget ceilings. These declarations are inputs to
Operator admission, never grants: the effective Tool set and budgets are the
intersection of the declaration, actor/session authority, Tool descriptors,
policy, approvals, and runtime limits. The loader and registrar bind the script
closure, SDK generation, Tool Runtime generation, Project Tool Catalog, and
Project registration before a run starts.

Automation scripts may use normal functions, modules, branches, loops, and
local variables. They receive no ambient filesystem, network, process, clock,
randomness, Host FFI, Binding, Receipt, or native-input primitive. External
work is a Tool call.

### 5.2 Durable call history

The Operator persists each call position as a state machine. At minimum it
distinguishes proposed, admitted, dispatching, confirmed result, proven absent,
possible/unknown delivery, rejected, and terminal failure. The exact transition
must be durable before an external effect can cross its corresponding boundary.

Four identities/records remain distinct:

1. The external root idempotency preimage contains the authenticated caller's
   pre-admission request and is matched only inside that caller namespace.
2. For every internal call position, the call-position fingerprint contains
   caller/runtime material fixed before admission:

   - script/run, root, parent, and sequence identity;
   - Tool name and version;
   - canonical arguments;
   - relevant observation reference;
   - Tool/Project/Framework/environment identities.

3. The call id is derived from the call-position fingerprint. Append-only
   Operator admission-attempt records keyed by call id plus attempt number bind
   the origin and executing principals; session epoch; policy and approvals;
   EffectivePlan; lease and fence; delegation/continuation grant; controlled
   target; and admitted budget snapshot. Replaying code does not have to
   reproduce Operator-selected authority material, and re-admission never
   rewrites a historical attempt.
4. The provider result, error, dispatch state, evidence, and delivery
   classification form the durable outcome keyed by call id. They are never
   inputs to either request identity and never need to be supplied by a
   replaying caller.

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
| `dispatching` or provider state with no terminal outcome | Atomically classify `possible/unknown` and never redispatch that attempt. A trusted idempotent provider query may instead prove `confirmed` or `proven_absent`; only a later explicit call may act after proven absence. |
| `confirmed`, `proven_absent`, `rejected`, or terminal failure | Replay the exact durable outcome without provider execution. |
| `possible/unknown` | Replay that classification and keep the target-wide mutation barrier. |

No startup path may infer `proven_absent` merely from a process crash. The
trusted provider reconciliation query and its evidence are themselves durable.

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

## 7. Project Tool handlers

The Project Tool Catalog remains the descriptor and argument-schema authority.
The replacement generation additionally binds each Tool to its exact handler
entry/function and module closure. A handler receives canonical arguments and a
scoped SDK environment, returns a schema-validated Tool result, and may make
admitted child calls.

Handler globals and module state do not carry durable truth. A handler that
needs durable project facts proposes Journal events; one that needs operation
progress relies on its call tree and recorded child results. A restart replays
the handler on the same terms as a top-level automation script.

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
its caller is Agent, human, another Project Tool, or automation script. Tests
compare those actor paths against one semantic fixture.

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

## 9. Contract and identity cut

Implementation requires one atomic generation across:

- Framework and Project Tool catalogs and tool-handler binding;
- actor-neutral ToolInvocation and nested-call envelopes;
- Operator ledger call-tree, dispatch, delivery, and replay rows;
- automation-script declaration, loader, registrar, and environment identity;
- reserved Framework SDK resolver and exact module catalog;
- scoped Tool Runtime facades;
- screen observation/reference and input Tool contracts;
- Project registration and SessionManifest transitively pinning the new roots;
- policy, approval, budget, lease, and recovery paths;
- Workbench/CLI and Agent adapters;
- Project Kit scaffolds and generated Project Tools/scripts;
- conformance, public contract, publisher, and consumer migration.

The old five-function registration and persisted sessions remain audit-only.
There is no dual entry table, optional Tool bridge, fallback dispatch, or replay
under a different SDK generation.

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
5. Implement scoped SDK facades over the Tool Runtime.
6. Implement Project Tool handler binding and nested calls.
7. Implement automation declarations, loader, deterministic restart, and
   replay.
8. Implement Framework observation and input Tools over existing Host/runtime
   authority.
9. Keep reducer execution pure and add trusted commit context.
10. Replace ProjectPlugin five-call schemas, bridge, registration generation,
    service choreography, scaffolds, examples, and conformance atomically.
11. Regenerate the public contract and publish one matching release.
12. Migrate consumers only against that release and run release-facing
    positive, negative, tamper, replay, recovery, and production-admission
    gates.

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

## 13. Implementation handoff checkpoint

Implementation has reached the pure Framework SDK and the exact full-Framework
dependency resolver. Continue in this order:

1. scoped `tools`, `screen`, `workflow`, and `audit` modules plus Project Tool
   handlers;
2. production mutating providers, nested calls, and Agent/human/Luau adapters;
3. durable automation with fresh observations, caller-selected post-action
   delay, `possible`/`unknown` reconciliation, and Project Kit/template
   migration; then
4. cross-repository review, synchronization, publication, one final full CI,
   commits, and pushes.

Until all implementation is complete, run only affected incremental tests; run
the full CI exactly once at the end. Do not use `git pull`. Concrete
implementation continues through medium-effort agents and is reviewed and
accepted by the main agent. Python remains an offline data-processing tool for
Unicode, unpacked database, and artifact inputs; the final runtime and Project
artifact are Luau.

Resolver follow-up risks remain explicit: `frameworkBundleHash()` does not yet
cover resolver aliases or dependency-depth topology; the full and pure resolver
grammars are separately implemented and can drift; every new release-owned
dependency must declare its resolver alias and dependency depth.
