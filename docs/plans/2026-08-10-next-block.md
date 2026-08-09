# The next block: complete coverage of the remaining design

Status: proposed; no code changed on its account
Date: 2026-08-10
Scope: `umbraflow-cpp` only. No consumer-project writes.
Bundle: v1.9, root `c4760bb59e7df28e13a676446a4cfbb4a62b067741420ecf13f4b939bfb6a966`

This plan is derived from the requirement matrix, not from what happened to be
found. Every `REQUIRED_CORE` requirement appears in §2 exactly once, with the
work item that closes it. Nothing in the design is left unassigned.

## 1. What the matrix says we owe

51 requirements. `D-01`-`D-09` are `PROJECT_CONTRACT` — the Chaos content
pipeline, owned by the consumer repository, correctly ungated here. The other
**42 are `REQUIRED_CORE` and every one is ours.**

All 42 have a registered CTest ID, and `ctest -N` lists 43 gates. But a gate is
not a proof. Splitting the 42 by what their assertions actually do:

- **22 exercise the code.**
- **20 read a schema file and assert that a definition exists with certain
  members.** They pass whether or not the behaviour exists.

The 20 are: `p01 p02 p03`, `s01 s02 s03 s04 s06`, `c02 c03 c04 c05 c08`,
`a01 a02 a03 a04 a05 a06 a07`. Renaming them (§4, ruled) makes the matrix
honest; it does not close them. Closing them is what this plan is for.

## 2. Every requirement, and what closes it

**Done — behavioural gate exists (22).** `p04 p05 p06`, `u01`-`u08`,
`s05`, `c01 c06 c07 c09 c10 c11 c12 c13 c14`, `a08`. No further work; they are
re-verified by the existing suite on every run.

**The 20 open ones**, each with its state and its owning work item:

| ID | Requirement | Implementation state | Closed by |
|---|---|---|---|
| `c04` | caller submits only a minimal ToolInvocation | **exists** (`ValidatedToolInvocation`) | W1 |
| `c02` | no lease auto-expiry; restart uses a new session epoch | **exists** (`beginSessionEpoch`) | W1 |
| `s03` | SnapshotToken is an opaque CAS reference, not a permission | **exists** | W1 |
| `s06` | `project_instance_key` prevents revision ABA | **exists** | W1 |
| `a04` | the Journal records only confirmed or provable facts | **exists** (`ValidatedJournalEntryData`) | W1 |
| `a06` | authoring capability root isolated from production | **exists** (generation kinds, confined-file) | W1 |
| `a07` | human takeover and Host delivery share one linearization | **partial** — `takeoverLease` exists, not joined to `Host::deliver` | W4 |
| `c03` | `Host::deliver` is the only linearization point | **partial** — holds inside `task`, not joined to the ledger | W4 |
| `s04` | `decision_basis_hash` covers only semantic decision input | **partial** — a caller argument today | W2 |
| `s02` | Snapshot Coordinator publishes a complete snapshot atomically | **partial** — `createSnapshot` takes a caller identity, composes nothing | W3 |
| `s01` | the five state kinds have separate owners | **absent** — `ProjectObservation` does not exist | W3 |
| `c05` | the Operator mints EffectivePlan from a plugin PlanProposal | **absent** | W2 |
| `c08` | one Operation runs a bounded multi-step workflow | **absent** — no step sequencing exists | W2 |
| `a03` | Audit Trace, Ledger, Journal and Replay Bundle are separate | **partial** — three exist; `ReplayBundle` is schema only | W5 |
| `a05` | UI replay and project/operation replay are independent gates | **absent** — `ReplayGate` is schema only | W5 |
| `p01` | Script, Agent and Human share one Operation path | **absent** — no controller facade | W6 |
| `p02` | out-of-band human input is not disguised as a ToolInvocation | **absent** | W6 |
| `p03` | an online Agent gets semantic tools only | **absent** | W6 |
| `a01` | Agent uses snapshot plus `subscribe(after_cursor)` | **absent** | W7 |
| `a02` | Agent has action, risk, time, observation and no-progress budgets | **absent** | W7 |

## 3. The work items, ordered by dependency

```
W0 merge readiness ─── independent
W1 coverage debt ───── independent
W2 EffectivePlan ──┬── W3 Snapshot Coordinator ──┐
                   │                              ├── W4 delivery join
                   └──────────────────────────────┘
W5 replay gates ───── independent of W2-W4
W6 controller facade ─ needs W2
W7 Agent surface ───── needs W6
W8 artifact GC ─────── independent
W9 third review ────── after W2-W4
```

| # | Item | Closes | Depends on | Cost |
|---|---|---|---|---|
| W0 | Merge readiness: run `linux-analysis` and the three sanitizer presets locally. CI runs on `master` only, so the first CI sight of this work is post-merge; seven of eight configurations are reproducible locally, macOS is not | — | none | 1 day |
| W1 | **Coverage debt**: write behavioural cases for the eight requirements whose implementation already exists but whose gate only reads a schema | `c02 c04 s03 s06 a04 a06` | none | 2-3 days |
| W2 | **EffectivePlan authority**: mint it from a plugin `PlanProposal` bound to registration, command fingerprint and decision basis; derive the frozen plan, step intent and effect envelope hashes from it; add bounded step sequencing; `reserveDispatch` takes the minted plan instead of three caller hashes | `c05 c08 s04` | none | 5-7 days |
| W3 | **Snapshot Coordinator**: introduce `ProjectObservation`; compose UI observation, `plugin.derive` and current ProjectState atomically; derive the snapshot identity instead of accepting one | `s01 s02` | W2 | 4 days |
| W4 | **Join Host delivery to the ledger**: `recordDeliveryOutcome` takes what `Host::deliver` returned, inside the fence; the takeover path enters the same linearization | `c03 a07` | W2, W3 | 3 days |
| W5 | **Replay Bundle and the two gates**: implement the bundle closure and both publication gates rather than declaring them | `a03 a05` | none | 4 days |
| W6 | **Controller facade**: one path for Script, Agent and Human; out-of-band human input enters as an external source; the Agent surface is semantic-only | `p01 p02 p03` | W2 | 4 days |
| W7 | **Agent subscription and budgets**: `subscribe(after_cursor)`, and action/risk/time/observation/no-progress budgets | `a01 a02` | W6 | 4 days |
| W8 | Artifact GC by database refcount for orphaned `runtime-artifacts/<hash>/` and `.staging` | — | none | 1-2 days |
| W9 | Third adversarial review round | — | W2-W4 | 1 day per reviewer |
| W10 | Rename the schema-shape gates `schema-*` as each requirement gains a behavioural gate, so the matrix never overstates | — | tracks W1-W7 | folded in |

`W2` is the keystone: five requirements and three later items hang off it.

## 4. Rulings already made

- The schema-shape gates become `schema-*`; `contract-*` is reserved for
  behaviour. Applied incrementally by W10 rather than as one rename, so no gate
  is ever renamed into a promise it does not keep.
- `OperatorCoordinator` grows to hold observation and plan data. A second
  trusted object is a second place authority can leak from.
- Artifact GC is by database refcount, not mark-and-sweep. A sweep must decide
  what "orphan" means while a concurrent publisher is mid-install, which is the
  hazard behind accepted finding A-F8.

## 5. Outside these work items

- **Publishing a consumable contract suite.** The suite is compiled inside this
  repository; no consumer can run it against its own registration. Phase 2C and
  therefore the external dual-game gate cannot start without it, and it has no
  owner. Raised, not assigned.
- Phases 2B, 2C, 3, 4 and anything in a consumer repository.
- The real dual-game attestation: `EXTERNAL / NOT_RUN`, unmovable by fixtures.
- Production `click`, `key`, `drag`, `run`: closed by design, and still closed
  by construction — nothing outside a test calls `Host::deliver`.
- The accepted findings A-F8 and B-F4, which stay accepted.

## 6. What finishing this plan means

W1 through W7 close all 20 open requirements. At that point all 42
`REQUIRED_CORE` requirements have a behavioural gate, `contract-*` means what it
says, and the remaining distance to production mutation is the external
dual-game attestation plus the consumer-side phases — none of which this
repository can close alone.
