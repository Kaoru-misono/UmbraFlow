# ProjectPlugin cycle-owned decision SPI — discussion draft

Date: 2026-08-21
Scope: `deployment`, `operator`, `service`, `script`, conformance, schemas,
examples, Project Kit scaffolds, generated public contract, and consumer
migration
Status: **discussion draft; no contract change is approved by this document**

## Why this draft exists

The implemented ProjectPlugin boundary exposes five pure calls: `derive`,
`plan`, `next_step`, `reconcile`, and `reduce`. The framework owns the
choreography between those calls. A release-facing consumer exercise exposed
an ownership problem: a game workflow is divided between the controller, the
Operator operation state machine, five protocol envelopes, and the plugin that
owns the game semantics.

The most concrete symptom is context loss at a deliberately narrow call
boundary. A reducer that must stamp project facts with the commit revision does
not receive that framework-owned revision. Copying a second revision counter
into opaque project state would make the immediate consumer work, but it would
not answer whether the framework is withholding a mechanical fact that the
plugin needs while simultaneously prescribing too much of the semantic
workflow.

This draft tests a different split:

- the framework owns trusted inputs, authority, validation, execution,
  persistence, replay infrastructure, and budgets;
- the plugin owns game interpretation and the game-specific online workflow
  state machine;
- a controller or Agent supplies a high-level goal or ToolInvocation rather
  than driving game-specific microsteps;
- the public online SPI becomes one cycle transition instead of four staged
  callbacks, while deterministic Journal reduction stays separate.

The implemented schemas, bridge, and generated `docs/PUBLIC-CONTRACT.md` remain
the contract until a later decision accepts an exact replacement and its
atomic migration passes. This plan creates no compatibility obligation.

## Problem statement

### Framework choreography is not framework authority

The framework must decide whether a proposed action is permitted and how it is
safely delivered. It does not need to decide which game-semantic phase the
plugin is currently in. The current five-call protocol mixes those two jobs:

```text
framework observes
  -> derive
controller selects a tool
  -> plan
framework freezes a plan and selects a step phase
  -> next_step
framework delivers and selects a reconciliation phase
  -> reconcile
framework commits Journal events
  -> reduce
```

Each function is pure, but purity alone does not put ownership in the right
place. The framework selects the callback and therefore owns the public
workflow grammar. The plugin can answer only the question selected for it; it
cannot return one coherent game decision over the complete current cycle.

### The passive-adapter model is a product choice

The current model is coherent if ProjectPlugin is deliberately a passive domain
adapter and an external controller owns every semantic transition. It is the
wrong model if the intended product gives the plugin a high-level objective and
expects it to own the game-specific progression needed to satisfy that
objective.

This draft assumes the latter candidate product boundary:

```text
Agent/controller owns: high-level objective and user intent
ProjectPlugin owns:    game interpretation and game workflow progression
framework owns:        trust, bounds, delivery, durable truth and recovery
```

Acceptance of this assumption is an entry decision, not something an API shape
may decide accidentally.

### Narrow inputs can remove facts without removing the need for them

A trusted framework fact is not a capability. Supplying the exact commit
revision to a pure reducer does not let project code mint a revision; it lets
the project stamp a value the framework will independently verify. Similar
mechanical context includes the current operation transition, proven delivery
outcome, frozen-plan identity, and remaining budgets.

The replacement must continue to deny ambient policy, filesystem, network,
coordinates, Binding, Receipt, native input, and mutable process state. It must
not deny explicit immutable context merely because the five-call split did not
previously carry it.

## Candidate public boundary

The candidate has two pure entry points:

```text
cycle(input)  -> proposal
reduce(input) -> opaque ProjectState
```

The exact names are open. `advance` is the leading alternative to `cycle`.
The important cut is online decision versus deterministic Journal fold, not the
spelling.

The entry module would return one plugin id and those two functions. Internal
modules may retain any number of helpers named derive, plan, next step, or
reconcile; those names would no longer define framework choreography.

### `cycle`: one atomic online decision

The framework calls `cycle` with one immutable snapshot of everything the
plugin is allowed to know for this online turn. Candidate input families are:

```json
{
  "ui_observation": {},
  "project_state": {},
  "prior_project_observation": null,
  "invocation": null,
  "operation": null,
  "previous_delivery": null,
  "pending_transition": null,
  "pinned_project_artifact_identities": [],
  "budgets": {}
}
```

This is not a promise that every family becomes one flat object. The schema
must distinguish framework-owned protocol context from project-owned opaque
payloads, and it must carry only values required by a decision or by a
falsifiable authority check.

The proposal contains an observation and one explicit decision union:

```json
{
  "observation": {
    "canonical_opaque_payload": {},
    "project_tool_preconditions": [],
    "observed_instance_proposals": []
  },
  "decision": {
    "kind": "wait"
  },
  "journal_proposals": []
}
```

Candidate decision variants are:

- `idle` — publish observation and take no operation action;
- `propose_plan` — state effects, action bounds and workflow limits for
  framework validation and approval;
- `act` — propose one semantic UI action under an already frozen authority;
- `wait` — request another observation under a bounded timeout policy;
- `complete` — propose a terminal successful or rejected project outcome;
- `ambiguous` — state that an external effect cannot be classified safely;
- `diverged` — state that the world no longer matches the operation basis;
- `ask_human` — stop automated progression with a project-semantic reason.

The final set must be closed and justified by executable recovery behavior. A
variant is not admitted merely because one current callback can return it.

### Observation and action in one cycle

Observed-instance identity remains Operator-owned. Two designs require a
focused experiment:

1. **two-cycle mint** — a proposal first publishes observed-instance proposals;
   the next cycle receives minted ids and may act on them;
2. **transactional local reference** — an action in the same proposal may name a
   local reference whose identity the Operator mints and resolves before
   validating the action.

The second removes an avoidable cycle but couples mint and action validation in
one transaction. It is accepted only if authority never crosses a registration,
parent/child minting remains atomic, and a failed action does not publish an
observation that claims more than the evidence proved.

### ToolInvocation remains the user-intent boundary

A cycle with no invocation may observe, wait, advise, or request a human; it
must not silently invent a new mutating objective. A controller supplies a
high-level semantic ToolInvocation or objective. The plugin owns the
project-specific microsteps needed to carry it out.

This keeps Script, Agent, and human tool control on one trusted invocation path
without forcing an Agent to know page transitions, retry rules, overlays, or
reconciliation details that belong to the game integration.

### EffectivePlan remains framework-owned

Collapsing callbacks does not remove plan authority. A `propose_plan` decision
is validated against the Tool Catalog, policy, risk, effects, action bounds,
approvals, and workflow limits. The framework returns the resulting immutable
plan identity in a later cycle. An `act` decision must name that authority and
stay within it.

The plugin decides when its workflow needs a plan or a step. It does not decide
whether the proposed plan grants authority.

### Delivery remains Host-owned

An `act` decision names only semantic action material admitted by the frozen
plan. The framework still:

1. validates the decision against the current operation and budgets;
2. resolves the current observed instance and Binding;
3. checks lease, fence, policy and approval;
4. has the Host mint the exact-cycle Receipt;
5. linearizes native input in `Host.deliver`;
6. records the proven delivery outcome;
7. presents that outcome in the next cycle.

No coordinate, Binding, Receipt or native-input primitive enters the plugin VM.

### Reconciliation becomes a plugin decision, not a callback phase

The next cycle after delivery carries the durable delivery classification and a
fresh observation. The plugin may return `complete`, `wait`, `ambiguous`,
`diverged`, another bounded `act`, or confirmed Journal proposals. The
framework no longer decides that the plugin is “in reconcile”; it supplies the
facts and validates the result.

Possible delivery is still never treated as rejection. Non-idempotent work is
still never automatically repeated. Continue-style partial facts must commit
atomically while the one-live-mutation-chain authority remains held.

### `reduce`: deterministic committed-state fold

Journal reduction remains a separate function so replay cannot accidentally
read a live frame, operation, delivery outcome, clock, or mutable VM state.
Candidate input is:

```json
{
  "prior_project_state": null,
  "commit_context": {
    "prior_revision": null,
    "next_revision": 0
  },
  "journal_events": []
}
```

`commit_context` is trusted mechanical context. It does not grant project code
the authority to choose a revision. The framework supplies it, the project uses
it when its state schema needs a revision stamp, and the framework verifies the
returned state against the same commit before publication.

The event projection must expose only the committed fields needed for project
semantics. Sequence uniqueness, project-instance identity, registration
identity, state hash, and commit durability remain framework checks. A
consumer test must prove that adding a framework-owned field to a project
negative vector is impossible rather than treating its absence as reduced
coverage.

## Authority matrix

| Concern | Candidate owner |
|---|---|
| trusted frame, RuntimeArtifact generation and readings | Host/runtime |
| atomic cycle context | Snapshot Coordinator/Operator |
| game-semantic observation | ProjectPlugin |
| high-level objective or ToolInvocation | controller/Agent/user |
| game-specific workflow progression | ProjectPlugin |
| Tool Catalog, effect/action bounds and EffectivePlan | Operator |
| policy, risk, approval, budgets, lease and fence | Operator |
| observed-instance minting and Binding resolution | Operator/Host |
| Receipt and native delivery | Host |
| delivery classification and durable operation record | Operator |
| game-semantic interpretation of the new observation | ProjectPlugin |
| Journal admission, ordering and commit | Operator/Journal |
| deterministic opaque state fold | ProjectPlugin `reduce` |
| state revision, hash and CAS publication | Operator |

## Alternatives to test

### A. Keep five calls and add missing context

This is the smallest implementation change. It retains independent schemas and
existing conformance structure, but leaves workflow choreography in the
framework and addresses only the observed context-loss symptom. It is the
control alternative, not the default answer.

### B. Keep `derive` and `reduce`, combine the operation calls into `advance`

This yields three calls: semantic observation, online operation progression,
and Journal fold. It keeps an independently publishable observation boundary
for Agents while moving plan/step/reconcile choreography back into the plugin.
It may be preferable if observation publication must proceed independently of
all operation decisions.

### C. Use `cycle` plus `reduce`

This is the candidate described above. It gives the plugin one coherent online
turn and the framework one proposal to validate. It has the largest schema cut
and must prove that observation publication and action authority remain
separable even when returned together.

### D. Use one generic transition for live and replay

A tagged `transition` input could carry either an online cycle or a Journal
commit. This minimizes entry-point count but weakens the structural guarantee
that replay cannot observe live context. This draft rejects it unless an
experiment finds a property that two functions cannot express.

## Invariants that survive every alternative

- Project code remains pure JSON-in/JSON-out Luau in a fresh quota-bound VM.
- Module closure, resources and execution environment remain registration-pinned.
- No policy, filesystem, network, package search, coordinates, Binding, Receipt,
  native input or mutable hidden state reaches the pure plugin call.
- Project code proposes; trusted framework code validates and applies.
- Host delivery remains the sole native-input linearization point.
- Journal is the only source of durable ProjectState facts.
- Replay uses committed bytes and no live observation.
- Unknown or possible delivery fails closed and cannot trigger blind retry.
- Core carries no game name, entity, tool, state field or Journal event.
- Production project declarations do not gain test-only foreign deployments.
- The replacement is a breaking atomic generation. No compatibility reader,
  optional old callback, dual export table or fallback dispatch is added.

## Contract and identity consequences

Acceptance would require one atomic contract cut across:

- ProjectPlugin bridge entry-table shape and function enum;
- framework-owned Operator protocol identities and schemas;
- project-directory and registration generations if their member semantics
  change;
- `plugin_environment_hash`, because bridge/API behavior changes;
- Tool Catalog decision and action-bound semantics;
- Operator operation and recovery transitions;
- service composition and product loop;
- Project Kit generated and hand-written scaffolds;
- examples and both-role conformance fixtures;
- generated public contract and release publication;
- persisted registration/session execution policy;
- consumer migration and release-facing acceptance.

Existing registration bytes remain audit-only if the running bridge no longer
implements their environment. They are not reinterpreted under the new SPI.

The current rulings remain authoritative while this draft is open:

- [policy is Operator-owned](../decisions/2026-08-09-policy-is-operator-owned.md)
  preserves pure explicit inputs and currently names the five-function boundary;
- [Project execution identity is closure plus environment](../decisions/2026-08-20-project-execution-identity-is-closure-plus-environment.md)
  binds the exact entry-table and bridge behavior;
- [Operator protocol identities are framework-owned](../decisions/2026-08-19-operator-protocol-identities-are-framework-owned.md)
  assigns the call-envelope identities to the framework without assigning game
  semantics to it.

The accepted decision, if any, must explicitly supersede only the callback-shape
parts of the existing five-function rulings. Module closure, environment
identity, policy ownership, sandbox purity, resources, persistence and external
capability isolation survive unless the decision says otherwise.

## Required experiments before a decision

### E1 — Express one real read-only cycle

Using a project directory rather than a C++ fixture, demonstrate:

```text
trusted observation -> project observation -> high-level read-only invocation
-> terminal project answer
```

The Agent must not drive project-specific microsteps. The plugin must not gain
Host authority.

### E2 — Express one multi-step mutation with recovery

Demonstrate action, fresh re-observation, partial progress, completion,
possible-delivery recovery, and human stop through repeated calls to the same
candidate online entry point. Delete one decision variant at a time and prove
the matching scenario fails at the assertion naming it.

### E3 — Revision and batch parity

Supply commit context from the framework and prove:

- baseline produces the first revision;
- one event and multiple events in one commit advance once;
- a commit that confirms no new fact still advances once;
- retry with identical prior bytes is byte-identical;
- replay reaches the same opaque state and framework revision as live commit.

No project-maintained mirror revision is introduced for this experiment.

### E4 — Observation/action authority

Compare two-cycle mint against transactional local reference. Attack with a
foreign registration, missing parent, duplicate local reference, stale
snapshot, changed Binding, and action refusal after a valid observation.

### E5 — Conformance shape

Build the same semantic project twice: once with internal helpers named after
the five current callbacks and once without them. Both must pass the same
candidate conformance suite, proving internal module structure is not public
workflow choreography.

### E6 — Crash boundaries

Crash before plan acceptance, after plan acceptance, before Host delivery,
after possible delivery, after confirmed Journal commit, and before state
publication. Recovery must be determined entirely from durable framework state
plus a fresh cycle, never from plugin globals.

## Decision questions

A decision is not ready until the owner answers:

1. Is ProjectPlugin a passive adapter, or the owner of game-specific workflow
   progression under a high-level invocation?
2. Must semantic observation be publishable independently of an operation
   decision, selecting alternative B over C?
3. Can one proposal safely contain both observed-instance proposals and an
   action referencing a local identity, or is two-cycle mint mandatory?
4. Which decision variants are irreducible after E1/E2/E6?
5. Does Journal proposal construction belong in `cycle`, a project-owned
   adapter behind it, or a separate pure reducer input writer?
6. Which exact commit context does `reduce` need, and which fields remain
   deliberately invisible because the framework has already enforced them?
7. Does a high-level ToolInvocation remain mandatory for every mutation, and
   what non-mutating autonomous decisions are permitted without one?
8. Which persisted operation generations remain inspectable but
   non-executable after the cut?

## Work packages after acceptance

No implementation starts merely because this draft exists. If an exact
alternative is accepted:

1. write a new frozen decision naming the five-function rulings it supersedes;
2. rewrite current architecture and terminology in one blast-radius sweep;
3. version the protocol schemas and registration/environment identities;
4. implement the script bridge and deployment loader as one breaking shape;
5. rewrite Operator/service choreography around cycle validation and durable
   outcomes;
6. migrate Project Kit scaffolds and generated adapters;
7. replace conformance cases with authority and recovery properties, not a
   callback-count assertion;
8. regenerate the public contract from authoritative bytes;
9. publish one matching release;
10. migrate consumers only against that release and run release-facing positive,
    negative, tamper, replay and recovery gates.

## Acceptance for this planning stage

This draft is ready for an owner decision only when:

- E1 through E6 have executable results or a written reason an experiment is
  impossible;
- alternatives A, B and C are compared against the same scenarios;
- the authority matrix has no shared writer;
- the chosen input contains every fact a project must use and no ambient
  capability;
- the chosen output cannot bypass Tool Catalog, policy, approval, Host delivery,
  Journal commit or state publication;
- a breaking migration and persisted-history policy are explicit;
- an independent reviewer finds no game-specific branch or hidden-state path.
