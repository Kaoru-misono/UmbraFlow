# Unified Tool Runtime, Framework Luau SDK, and replayable Project automation

Date: 2026-08-21
Scope: `operator`, `service`, `script`, `deployment`, `task`, Agent bindings,
Workbench/CLI, schemas, Project Kit, conformance, release publication, and
consumer migration
Status: **the generation landed at `afcebd2`; WP9 and the shipped entry point
followed; WP11, WP12 and the remaining obligations of section 14 are open**

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

This plan owns the experiments, the atomic implementation, and the stage order
those rulings are implemented in. Section 13 holds that order, because an
ordering is work rather than a frozen ruling.

The generation this plan specifies landed as `afcebd2`, which deleted the
five-function ProjectPlugin contract rather than deprecating it. That commit ends
the staging this document was written to sequence; it does not discharge the
document. Every statement outside section 14 is a requirement on the finished
generation, and a requirement is not weakened because the tree has not met it
yet — nor is it discharged because the flip happened.

Section 14 is the one place that says where the tree stood at the cut and what
the cut left owed. The per-section "internal checkpoint" paragraphs that used to
carry that job were written while the generation was still dark, and every one
of them described a repository in which the Tool Runtime had no production
caller; they are removed rather than restated, and this document's own revisions
hold them. Two work packages of section 11 are unstarted, section 12's
acceptance list is unmet, and most of section 10's experiments are unrun, which
is why this plan is live rather than archived.

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

**Settled 2026-08-23.** An earlier wording of this paragraph read "A Project
declares the SDK generation it targets". That was an abandoned direction, and
the phrase is struck: a Project targets a generation by being published against
that release's bytes, and the derived `plugin_environment_hash` pinned into its
registration is the strongest available spelling of that fact. The registration
carries the derived digest and nothing else, and `registerGeneration`'s exact
inequality refusal is the whole contract. No `sdk_generation` member exists, and
none will be added.

An authored generation name could not be joined to anything. Naming a
generation, resolving that name to release bytes, and deriving the digest from
those bytes is one production chain, so a check that the name agrees with the
digest compares one producer's output against itself — the shape
[`checks that cannot fail`](../pitfalls/checks-that-cannot-fail.md) names
directly, and no test could make it red. The existing check has two genuinely
independent sources instead: the publisher derives the digest from the release
it is itself running, and the registrar re-derives it from the release the
executing side is running, so a publisher and an executor on different releases
turn it red. The authored member would therefore be either dead data no code
branches on, or a key the publisher dispatches on to choose between generations
— the selector the no-selector invariant exists to forbid.

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

### 5.2 Durable call history

The Operator persists each call position as a state machine. At minimum it
distinguishes proposed, admitted, dispatching, confirmed result, proven absent,
possible/unknown delivery, terminally unresolved, and terminal
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
| `confirmed`, `proven_absent`, or terminal failure | Replay the exact durable outcome without provider execution. |
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

## 13. Implementation order, as it was followed

The stage order this section held was a sequence for building the generation
dark and flipping it in one commit, under
[`production reachability is the cut invariant`](../decisions/2026-08-22-production-reachability-is-the-cut-invariant.md).
That sequence ran to its end — stages NAME, REGISTRATION, PROVISIONING,
RE-ENTRY, MUTATING PROVIDERS, ROOT PRODUCER and ADAPTERS, then the flip — so it
is history and is read from this document's revisions rather than restated here.
What the flip left open is section 14.

Two rules of section 9.1 outlive the cut and stay checkable at every commit:
exactly one generation is production-reachable with the gate passing, and no
commit introduces a selector between two. The flip having happened is not
permission for a second generation afterwards.

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

## 14. State at the generation cut, and what it left owed

The generation landed as `afcebd2`: the five-function ProjectPlugin contract is
deleted rather than deprecated, a registration carries a reducer closure and a
tool closure with two manifest hashes, and the production entry registers,
provisions, pins and drives a project through the Tool Runtime. The
`ProjectAutomationAdapter`, `AgentToolAdapter` and `HumanToolAdapter` translators
and the one `ToolAdmissionRequest` they build exist, so section 7's funnel half
and section 5.1's root producer are no longer open.

What follows is what the cut left owed. It is a list of obligations rather than a
status board: an item stays here until something closes it, and each one names
enough to be picked up. Several were being closed while this section was written,
so read the code for the state of one of them today — a document cannot keep
that true by hand.

### 14.1 Surfaces left standing with nothing behind them

Closed at `2797241`, which cut them rather than filling them in: the Operation
dispatch spine and the cascade of constants and unreachable states it was the
only writer for; `ProjectPluginFunction` entirely, because with only `Reduce`
left the refolded-function comparison was a check no test could redden; the
reconcile subsystem and the observation schema `Derive`'s death left nothing
judging; and `framework.screen.capture` and `framework.workflow.reconcile`,
whose blockers had not expired at the flip — no public API recovers a position
identity from the `call_identity` hash, and `ToolReconciliationQuery` is an
oracle the Framework has no source for, so both were new capabilities rather
than provider attachments.

Also closed at `2797241`: **the three actor doors now have a shipped verb.**
`umbra-flow invoke --actor agent|human|project` is the production entry point,
and `--actor` names the principal rather than only the translator — `ProductStart`
carries an explicit `ControllerKind` with no default, so every construction site
states whose call it is, and the controller id is composed from the kind's own
wire name so the two cannot disagree.

Closed since: the wider protocol cut this section flagged as undecided has
happened. `schema/umbraflow-operator-v1.schema.json` no longer publishes
`Operation`, `DispatchRecord`, `AuthorityDecision`, `ApprovalToken`,
`ReconcileProposal`, `MutationChain` or `PlanVersion`; `submitCommand`,
`transitionOperation` and the `operations` table are deleted with them.

What is still standing:

- **No Framework input provider resolves an observation reference through
  `SnapshotObservationAuthority`.** Section 6's eleven-way refusal matrix is
  built; its input-authority sentences stay requirements on whatever resolves
  through it.
- **The two Journal commit doors have no production caller.**
  `OperatorCoordinator::proposeJournalBatch` and `publishJournalProposal` are
  test-reachable only: nothing in `entry/` or `modules/cli` proposes or
  publishes, and a Project handler has no scoped seam to propose through. Under
  section 9.1 that is a legitimate intermediate state rather than drift, but it
  is the same shape as the actor-door gap this section just closed, and it
  closes the same way.

### 14.2 The Journal reducer

Closed by WP9. The reduce envelope is now section 8's block byte for byte —
`commit_context {prior_revision, next_revision}`, `prior_project_state`,
`prospective_journal_batch` — and nothing a Project supplies reaches either
revision: both are derived from the `project_state` row the Operator holds,
through `nextProjectStateRevision`, the single statement of the increment rule.
`prior_project_state` is carried as one optional value rather than two
parameters, so "prior state absent if and only if prior revision null" is
structural rather than checked.

Call-bound proposals exist as three tables and two doors. A proposal may only be
made from a dispatching Project-provided call of the named run; it records the
ProjectInstance and the frozen prior revision from rows the Operator holds,
never from the caller, and is content-addressed over everything the proposer
stated, so the same batch from the same incarnation rejoins rather than
duplicating. Publication is one `BEGIN IMMEDIATE` transaction that re-verifies
durable-run authority, the call-tree identity, the referenced effect identities,
section 5.4's barrier and the prior revision before the fold, then writes
`journal_events`, `project_state` and the published revision together. The word
`provisional` is deliberately absent from the schema: it already spells the
operation plan's lifecycle, and one word for two lifecycles is a reader's trap.

Section 5.4's dependent-commit clause is implemented, and what "dependent" means
is now decided rather than assumed: `publishJournalProposal` consults
`requireNoActiveToolMutation` **excluding the proposing call and its ancestors**.
Consulting it unconditionally would freeze a commit proposed from inside a
mutating parent that is merely still dispatching — section 3.3's one live
mutation chain that the commit is part of, not a delivery it depends on.

Two things this closure changed that were not in its brief. The envelope's third
member was renamed from `journal_events` to `prospective_journal_batch`: section
8 writes three snake_case names at one level and spells `prior_project_state`
exactly as implemented, so reading the third as prose while the other two are
names is not a reading the block supports. And the operator database schema
identity moved to `f3462667`, with a registered migration and a fixture that
winds a fresh schema back by dropping exactly the three new tables and asserts
the old identity before reopening.

### 14.3 Verification and publication

- **Conformance covers the Tool Runtime in part, as of 2026-08-23.**
  `suite-tool-runtime.cpp` names `admitToolCall`, `beginToolCallDispatch`,
  `replayToolCall`, `persistToolRootRequest`, `persistToolCallPosition`,
  `ToolCallState`, the three actor adapters and `ProjectToolDispatcher`, across
  three cases: identical admission for every actor over this project's own
  Tools, a recorded call answered again from the ledger alone after the whole
  runtime is destroyed, and a call interrupted mid-dispatch re-entering to the
  same answer. Still uncovered: nested calls and delegation,
  observation-consuming calls, `possible` and reconciliation, the target-wide
  mutation barrier, and anything needing a Framework provider.

  The seam that refused was not refusing because a conformance run has no Host —
  `prepareStore` activates a real `task::TaskHost`. It refused on an ordering
  fact: provisioning needs the generation's fold and nothing else, and at that
  point no session, controller, lease or observation authority exists. It is now
  `provisioningToolRuntime()` and says so. What a conformance run genuinely
  lacks is a Framework `ToolProvider`, and it cannot invent one: a second answer
  for `framework.*` written in the suite would be a second account of what those
  Tools do. That refusal names the Tool, and it is reachable only by a consumer
  directory whose handler calls one.
- **Three refusals have neither a caller nor a test**: the accept-side discovery
  checks in `admitToolCall`, the origin-principal continuation gate, and the
  root-namespace binding.
- **WP11's release-facing gates have nothing to gate against, and what to
  publish is now ruled on.** Neither `schema/` nor the generated public contract
  describes a Tool Runtime call state, envelope or identity preimage. What must
  be published, and which bytes each fact is read from, is settled in
  [`what the framework publishes for the Tool Runtime`](../decisions/2026-08-23-what-the-framework-publishes-for-the-tool-runtime.md),
  along with the order the three pieces land in. WP12's positive, negative,
  tamper, replay, recovery and production-admission gates are unstarted, and
  WP12's subject is the consumer, so it follows the release rather than
  preceding it.
- **`tool_runtime_protocol_identity` is derived from the wrong preimage**, which
  is a sharper defect than the "no published definition" this section recorded
  before. It *is* derived, at `product-lifecycle.cpp:623`, from the framework
  Tool catalog hash — but that hash covers framework Tool descriptors and
  nothing else, so the state vocabulary, the preimage tags, the completion
  kinds, the durable record split and the JCS contract can each change without
  moving it. The equality at `ledger.cpp:10236` is therefore named for a
  property it cannot observe. The provider identity for a framework call is the
  same hash, so the preimage additionally carries a same-source duplicate that
  cannot mismatch. The ruling above replaces the derivation with protocol
  material of its own.
- **Most of section 10 is unrun.** E2's subject exists as
  `tests/operator/test-tool-automation-loop.cpp`; the dispatch and
  snapshot-reference fixtures cover parts of E3 and E5; and E1 has a three-actor
  fixture in `tests/operator/test-tool-dispatch.cpp`, and it is **audited and done**: it compares canonical
  argument bytes and hash, the seven admission-attempt attributes, all thirteen
  durable position columns and the result, across all four producers, with the
  fourth's one recorded difference — a non-empty `delegation_grant_id` —
  asserted rather than ignored. The caveat is that only the read-only fixture
  drives all four; the mutating one drives the three adapters. E4, E6's cross-platform half, E7 and E8 have no runner.

  E4 cannot be written in conformance today: every descriptor in both shipped
  example catalogs declares `child_effects.maximum_child_calls: 0`, so no
  handler can issue a child call and no delegation grant can be minted. Its
  attack list is a set of Operator refusals over a synthetic catalog, which
  `tests/operator/test-tool-nested-calls.cpp` is the right home for; the half
  that is genuinely the consumer's — does this project's descriptor declare a
  child-effect envelope its handler stays inside — needs an example directory
  that declares one.

  E7 belongs in conformance beside the existing Journal-prefix fold case, but
  its subject is WP9's: there is no `commit_context` and no call-bound
  provisional proposal to prove parity against yet.

  **E8's blocker is upstream of the `terminally_unresolved` item below.** No
  `possible` outcome has any producer a conformance run can reach:
  `toolCallEffectMayBeUnrecorded(RecordedChildren, Mutating)` is false, so a
  Project handler never goes uncertain, and only a mutating Framework leaf can —
  which needs the Framework provider a conformance run does not have. E8 has no
  starting state, not merely no exit.

### 14.4 Runtime obligations

Four of the five are discharged, and one of those four was already discharged
before it was picked up.

**Completion-time classification was never keyed on mutability alone.**
`completeToolCallDispatch` already joins `tool_call_positions` for
`provider_kind` and asks `toolCallEffectMayBeUnrecorded`, and
`ToolRuntimeExecutor::invoke` already computes the composition from
`call.provider()`. The restart filter, the re-entry gate, the completion refusal
and the executor conversion are all four generated from the one predicate. What
was actually missing was the proof for the composed direction — that a mutating
composed handler failing cleanly leaves no barrier — and that now exists as a
subcase driving a real bound handler. Breaking the predicate to
`mutability == Mutating` reproduces exactly the over-refusal this bullet
described: the call lands in `possible` and the target freezes.

**`rejected` is deleted rather than given a producer.** A durable `rejected`
would be a terminal row, but every admission refusal is a function of live
authority — lease, epoch, policy, budget, approvals — so writing one would
permanently freeze a call a fresh lease legitimately re-admits. Section 5.3's
`proposed` recovery rule is already the right recovery for a refused admission.
It also could not satisfy the history CHECK, since terminal states require a
non-null outcome payload and a refusal has no provider payload. The value is
gone from the enum, the wire names, both CHECK clauses, and the two Luau
facades. Two of its three pins are independently breakable — the exhaustive
switches refuse at compile time, and the Luau state list reddens a task test;
the durable pin is not, because restoring the value in the CHECK moves the
schema identity and `initialize()` refuses before any assertion runs, so it is
pinned by the migration fixture asserting the rebuilt DDL instead.

**A divergence now terminates the run, and the state lives on
`tool_root_requests` rather than `tool_runs`.** `tool_runs` was the wrong home:
it exists only after the first successful admission, while a divergence is
detectable at a coordinate that was never admitted — so keying on it would have
needed a "no run row yet" branch whose false arm nothing could redden. What
terminated means is decided rather than implied: a new position, an admission
and a re-entry are refused, while an already-recorded coordinate still rejoins,
`replayToolCall` still answers from the durable outcome, and a dispatch already
across its boundary is still recorded. `beginToolCallDispatch` and
`reserveToolCallDispatch` are deliberately ungated, standing downstream of a
gated admission — a second gate would be a second copy of one rule. The persist
site commits the termination mark before returning the failure, because its
transaction is otherwise read-only and the RAII rollback would discard the only
record that the run stopped.

**A changed parent is reported as one.** The coordinate-miss branch now asks
whether the presented parent is a coordinate this run recorded — R4 admits
exactly two parent shapes and no third, so a miss whose parent is neither is a
call arriving under a parent this run never had. The other half says what it is
honestly: an ordinal past that context's frontier, rather than "call position is
not durable" for both. The writer keeps the original message, because a writer
hanging a new position off a coordinate that does not exist is an ordering
mistake inside a live run, not a replay divergence. The production-reachable
form of this defect was **already** correctly reported: a child whose delegation
grant names a parent that is no longer dispatching is refused by name in
`admitToolCall`.

Still open:

- `tests/operator/test-tool-executor.cpp` answers Project calls with arbitrary
  lambdas rather than through a bound handler. The composed side is now covered
  by real bound handlers in `test-tool-dispatch.cpp`, but the executor's own
  cases still stand on a stand-in.
