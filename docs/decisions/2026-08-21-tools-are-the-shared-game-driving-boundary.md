# 2026-08-21 — Tools are the shared game-driving boundary

## Decision

Agent, human-operated clients, and project-owned Luau automation scripts are
first-class callers of one Tool Runtime. Framework and Project are both tool
providers. Every call, including a call made from project Luau or from inside a
project tool handler, crosses the same Operator-owned ToolInvocation,
authorization, budget, delivery, durable-outcome, and audit path.

A Project may ship both:

- semantic Project Tools callable by Agent, human, and automation script; and
- multi-file Luau automation scripts that compose Framework Tools and Project
  Tools into game-driving loops.

An actor's use of the same protocol does not give every actor the same
authority. Agent profile, human approval context, automation-script grant,
Tool Catalog descriptor, policy, EffectivePlan, lease, fence, and remaining
budgets still decide which calls are offered and admitted.

Project declarations and Tool descriptors state requested ceilings; neither is
an authority grant. For a nested call, Operator distinguishes the origin actor
from the currently executing handler principal. The effective child authority
is the intersection of the admitted root effect envelope, the parent Tool's
registered child-effect declaration, current policy and approvals, target and
session authority, lease and fence, and remaining budgets. This permits an
actor admitted to a high-level semantic Project Tool to delegate its declared
and approved effects without making the corresponding low-level Framework Tool
directly callable by that actor. Project code cannot widen that envelope.

Framework also publishes a versioned Luau SDK under the reserved
`@umbraflow/` module namespace. Pure reusable modules provide deterministic
text, UTF-8, JSON, JCS, collection, and result operations. Scoped modules such
as `tools`, `screen`, `workflow`, and `audit` are frozen facades over the
current Tool Runtime execution context; they are not Host FFI and cannot bypass
ToolInvocation. Project modules continue to resolve only inside their pinned
closed module graph.

Completing an input Tool does not trigger an automatic screen capture. The
caller chooses whether and how long to wait, then explicitly calls the
Framework observation Tool. Wait and observation are separate bounded Tool
calls. An observation guarantees a newly captured snapshot at that request; it
does not claim the game is stable, the transition is over, or the prior action
is complete. Agent, human, or Project automation may inspect and repeat the
wait-observe-recognize loop until Project semantics report the desired state or
a budget/timeout stops it.

Project automation scripts use ordinary Luau control flow, including loops.
They are crash-resumable by deterministic restart and tool-call replay rather
than by serializing a VM stack. Every tool call is a durable suspension
boundary. On restart, an exact prior call returns its recorded result without
executing again; the first new call may execute. A changed tool name, version,
canonical argument, parent, sequence, observation reference, or pinned
execution identity at a replayed position is nondeterminism and stops the run.
A possible or otherwise uncertain delivery is replayed as that recorded
classification and freezes further mutation in that call tree until an
Operator-owned reconciliation transition resolves it; it is never blindly
redelivered at a later sequence or through a new parent. For the first
generation, Operator freezes the controlled target's entire mutation lane and
refuses every new mutating root for that target, so changing the request key or
actor cannot bypass the freeze. A terminally unresolved run leaves that simple
target-wide barrier in place.

Every externally initiated root run also has a caller-stable request key inside
an authenticated idempotency namespace. An exact retry rejoins the existing run
and reuses its durable outcome; the same key with a different request preimage
is refused. Replay of existing rows needs no new authority because it performs
no effect. The first new dispatch after restart must be admitted under a
current session re-authenticated as the same immutable origin principal, plus
current policy, approval, lease, fence, and budget state. Its authority can only
narrow the original root envelope; another principal cannot attach to the old
run.

Deterministic Journal reduction remains a separate Project entry. A reducer
folds an Operator-admitted, frozen prospective commit batch and trusted commit
context into a candidate opaque ProjectState; it is not an Agent tool and
cannot observe a live screen or call another tool. Only the final Operator CAS
makes the batch committed and publishes the state together.

## Context

The five-function ProjectPlugin SPI made Framework choose the public sequence
`derive -> plan -> next_step -> reconcile -> reduce`. That shape allowed pure
calls but split one game's workflow among Framework choreography, controller
microsteps, protocol envelopes, and project code. Replacing four online calls
with one `cycle` or `advance` function would move that choreography into one
ProjectPlugin callback, but it would still make ProjectPlugin the only online
driver.

The intended product has three drivers. An Agent may inspect a screen and
compose tools, a human may do the same through Workbench or CLI, and a Project
may load an automation script that performs the same sequence without an Agent.
All three need the same observable tool contract. Creating a second privileged
Luau execution path would make a project script more powerful and less
auditable than the Agent and human paths it is meant to automate.

Natural project automation also needs reusable, framework-maintained modules.
Making normalization, canonical JSON, collections, and workflow-result handling
into Tool calls would add durable side effects and budget cost to pure local
calculation. Loading them from the filesystem or a package search path would
make behavior ambient. A reserved, pinned Framework SDK gives project authors
reuse without either defect.

An ordinary Luau loop cannot have its native stack serialized safely across a
process crash. Restarting without a replay contract can repeat a delivered
input. Recording exact calls and replaying their exact results lets the script
be written naturally while leaving delivery classification, deduplication,
recovery, and authority in Framework.

## Superseded ruling parts

This decision supersedes only these callback and online-orchestration parts of
earlier rulings:

- the consequence in
  [`2026-08-09-policy-is-operator-owned.md`](2026-08-09-policy-is-operator-owned.md)
  that ProjectPlugin remains a five-function boundary;
- the exact five-function entry-table requirement in
  [`2026-08-20-project-execution-identity-is-closure-plus-environment.md`](2026-08-20-project-execution-identity-is-closure-plus-environment.md);
- that decision's separation of every external capability into a program that
  returns evidence to a later pure callback, where the new Tool Runtime instead
  admits a scoped, durable Tool call from an automation script or tool handler;
  and
- that decision's statement that Python remains the offline owner of JCS, only
  to permit the deterministic `@umbraflow/jcs` Project-data utility at runtime.
  Python remains the offline owner of registration and release canonical bytes,
  artifact compilation, database extraction and transforms, and packaging.

The earlier decisions' remaining rulings survive: policy is Operator-owned;
project modules, resources, and the observable environment are registration
pinned; protocol envelopes are framework-owned; Host alone linearizes native
input; Journal alone publishes durable ProjectState; and old execution
generations become audit-only rather than gaining a compatibility reader.

The implemented five-function generation remains the executable contract until
the shared Tool Runtime, SDK, registration generation, replay ledger, Project
Tool bridge, automation loader, conformance suite, and consumer migration land
as one atomic replacement. This decision creates no mixed-generation runtime.

## Consequences

- `cycle` or `advance` is not the sole online Project SPI. Online work is a
  graph of Tool calls driven by Agent, human, or project automation.
- Framework Tools occupy a reserved namespace; Project Tools occupy their
  registered project namespace. Collisions and undeclared aliases are refused.
- `@umbraflow/` is a Framework-reserved resolver prefix. A locked package may
  not claim the `umbraflow` alias, and Framework lookup never falls through to
  Project or package resolution.
- A nested call records root invocation, parent call, actor, registration,
  environment, canonical arguments, budgets, and terminal delivery state.
  Child authority is a subset of parent authority and cannot mint a new user
  objective or approval.
- Low-level input tools can be privileged while semantic Project Tools remain
  available to ordinary online actors. Shared protocol does not mean shared
  permission.
- Framework SDK modules are part of execution identity. A change to module
  bytes, native-backed behavior, Unicode data, resolver grammar, or scoped
  facade semantics moves the environment identity.
- Pure SDK calls do not create ToolInvocation records or spend tool-call
  budgets. Scoped SDK calls do.
- Root retry matches an authenticated caller-supplied idempotency preimage. An
  internal call-position fingerprint separately binds caller/runtime material
  fixed before admission. Operator-selected admission material and the durable
  outcome are two further records. A result or delivery classification is never
  part of either request identity.
- Project code receives no filesystem, network, process, package-search,
  coordinate, Binding, Receipt, or native-input FFI merely by loading the SDK.
  Any such ability must be an explicitly offered Tool and pass normal policy.
- Automation replay pins the script closure, Framework SDK, Tool Runtime
  protocol, Framework and Project tool catalogs, registration, and other
  execution identities needed to make the same call sequence meaningful.
- Project Journal proposals that depend on a call tree cannot commit until the
  relevant child effects have a terminal, non-uncertain classification. The
  commit is CAS-bound to the exact call tree and outcomes it interprets.
- Existing registration and session bytes retain their exact historical
  identity and remain inspectable, but cannot resume under the new tool and SDK
  environment.
