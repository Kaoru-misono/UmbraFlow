# 2026-08-23 — The framework stops interpreting a Project's state

This ruling supersedes the **sequencing conclusion** of
[`2026-08-23-the-framework-enforces-limits-it-was-given.md`](2026-08-23-the-framework-enforces-limits-it-was-given.md).
That decision's line stands unchanged and is the reason for this one. What it
deferred, this schedules.

## What changed

That decision found the Journal, the reducer, the revision and the final
compare-and-swap **non-conforming** — they define how a Project's state evolves,
which is the Project's decision taken by the framework. It declined to schedule
removal on one stated ground:

> Nothing here is scheduled by this ruling. The non-conforming machinery is
> built, green and falsified, and removing it costs more than leaving it while
> a downstream consumer has not yet run.

Both halves of that ground have since failed.

The downstream consumer ran. `uf-chaos` was taken through a published release on
this date and was refused at its declaration, before reaching any behaviour.
And the machinery is not merely idle cost: it is the **dominant** cost. Of the
five schema files a Project must author before the framework will accept it at
all, three exist only because the framework folds the Project's state.

Two measured facts decide it.

**The machinery has never executed in production.** The only production writer
of `journal_events` is the baseline entry written once at provisioning
(`sequence = 0`, `prior_project_state_revision = NULL`). A production run
produces no Journal event, so the reducer is never asked to fold anything new
and a Project's state stays at revision 0 forever. The two commit doors are
migrated, falsified, and reachable only from tests. What is being weighed is
built and green and unreached.

**The onboarding floor is five JSON files.** `project_state_schema`,
`tool_precondition_schema`, `tool_catalog` and `journal_event_schema_manifest`
are mandatory file paths, and `journal_payload_schemas` is an array with
`minItems: 1`. A Project that binds no Tool and does nothing must still author
five schema documents. The owner's verdict on that threshold was that it is
indefensible, and it is treated here as a first-class design constraint rather
than a cost of doing business.

## The ruling

**The framework deletes the interpretation layer entirely.** Gone: the Journal
event schemas, the reducer contract, revision semantics, and the final
compare-and-swap.

What replaces it is what the superseded decision already named — a durable,
opaque, Project-owned channel. The framework records, for each write a Project
makes through the scoped seam: a per-Project monotonic gap-free sequence, the
call coordinates, the actor and the authority it acted under, a timestamp, and
the bytes together with a content digest that chains to the previous entry.
Replay is byte-exact, order-exact, gap-free and Project-scoped. Append is atomic
per call, because refusing a torn write is safety rather than interpretation.
Quotas are declared by the Project and enforced by the framework, and exceeding
one is reported with what was exceeded and what the limit was. **The sequence is
a log fact, not a concurrency primitive: there is no compare-and-swap.**

The framework asserts nothing about what the bytes mean.

**The channel's implementation is deliberately not on the critical path.** Its
contract is frozen by the paragraph above; the implementation lands after the
flip, when a consumer needs it. Nothing has ever used durable Project state —
revision 0 forever is the proof — so the blocked consumer's first light does not
need it. This trades breadth of coverage for speed of arrival, which is the
correct trade while a consumer is blocked, and it is stated rather than taken
silently.

### Why keeping a typed record was rejected

Deleting the fold while keeping typed Journal events is the same interpretation
with its verb removed. Once nothing folds, the framework has no guarantee of its
own that the event type system buys: a digest over opaque bytes and a digest
over typed JSON are exactly equivalent in what they let anyone prove. Every
payload schema the framework validates purchases nothing the framework itself
offers. Reading a Project's meaning and then declining to act on it is still
reading it.

### Why deferring again was rejected

A second reprieve rests on a premise already known to be false. Its remedies for
the onboarding threshold — optional slots, defaults — are the forbidden
"absent means the old behaviour" bridge. And the machinery's greenness measures
only the tree's self-consistency: ninety-one gates were green while the only
consumer was broken. Code no production path has ever reached is not an asset to
be preserved.

## What a Project declares after the cut

The two-closure shape does not survive the reducer. A Project ships exactly one
closure, `tool_closure`, on the scoped program type. `reducer_closure` existed
only to carry the fold; keeping a mandatory pure closure that exports `reduce`
into a void would be a requirement the framework invented. One spelling remains,
so the "absent means pure" ambiguity dies with the slot rather than needing to
be guarded against.

**Argument validation is a Project-optional guard.** For admission the framework
must know which Tools exist and who may call them; it need not know their
argument shapes. The JSON-to-Luau construction is total — object to table, array
to table, scalar to scalar — so the trusted seam builds values without a schema,
and validation is separable from construction. A Project that declares no
argument shape simply gets no argument enforcement; the framework still records
the bytes it actually passed, their digest and their coordinates, so
traceability is undiminished. This is the textbook case of enforcing a limit the
Project gave, on all fours with the Luau memory ceiling.

The declining is spelled as a value. Every Tool entry carries a **mandatory**
`argument_schema` key whose value is either the string `"unchecked"` or an
inline JSON Schema object. The key is always present, so there is no
absent-means-unchecked reading, and the two values mean different things rather
than being two spellings of one. The cross-Tool `$defs` indirection is deleted;
reuse inside one schema object uses JSON Schema's own `$defs`, and two Tools
repeating a definition is the accepted price of having one spelling.

**Everything declarative is inlined.** `tool_catalog` and
`tool_precondition_schema` cease to be file paths. The catalog becomes an inline
`tools` array; the precondition schema member is deleted outright rather than
moved, because argument shape now lives in each Tool entry. Identity schemas
inline as `{name, schema}` entries. One document is the whole declaration, so
it has one identity, one digest and one admission decision.

`resources` survives untouched, as file paths. A resource is an asset, not
declarative material: it is the opaque supply the framework delivers to a
program that has no filesystem, and the framework never reads its meaning. That
is already conforming, and inlining assets would be a different and worse thing.

`timeout_policy.on_timeout` survives as a Project configurable. That it is
published and unenforced remains a behaviour debt, repaid immediately after the
flip. Behaviour does not touch the declaration, so repaying it late costs the
consumer no second migration.

### The deployment's members, exactly

The document's top level is unchanged: `schema`, `runtime_artifact`,
`policy_artifact` (optional), `primary_deployment`, `deployments`,
`template_cuts`. The `schema` const moves to `umbraflow-project/v3`, as a
courtesy to whoever has to speak about the two shapes. Nothing in the code
depends on that name having moved; the shape itself is the authority, and the
newly staged binary is the oracle that judges a document.

| Member | Status |
|---|---|
| `$comment` | optional, unchanged |
| `name` | required |
| `plugin_id` | required |
| `plugin_authoring` | required |
| `plugin_justification` | optional, unchanged |
| `tool_closure` | required — the only closure |
| `tool_bindings` | required |
| `tools` | required array, may be empty. Entry keys, all mandatory: `name`, `version`, `mutability`, `surface`, `idempotency`, `required_capabilities`, `ui_action_bounds`, `effect_bounds`, `timeout_policy`, `workflow_limits`, `argument_schema` |
| `observed_instance_identity_schemas` | required array, may be empty; entries `{name, schema}`, inline |
| `resources` | required array, may be empty, unchanged |

Deleted from the deployment: `reducer_closure`, `baseline_event_type`,
`project_state_schema`, `tool_precondition_schema`, `tool_catalog`,
`journal_event_schema_manifest`, `journal_payload_schemas`,
`effect_payload_schemas`. Deleted from each Tool entry:
`tool_precondition_sha256`, `effect_payload_sha256s`.

`baseline_event_type` dies because it is the Journal family's birth certificate
and has nothing to name once the Journal is gone. Typed effect payloads die for
the same reason typed Journal events do: with no fold, parsing that meaning buys
the framework no guarantee of its own, and it still records an effect's bytes,
digest and coordinates.

An empty array is a written abstention, in the same way `tools: []` is. It is
required to be present precisely so that abstaining is stated.

### The floor

A hello-world Project authors `umbraflow-project.json` — with three empty arrays
— and one Luau source. It writes no JSON Schema at all. A Project that binds
Tools and declines argument validation writes none either. Zero schema is a
consequence of the capabilities a Project chooses, not an unconditional promise:
a Project that wants semantic identity comparison does author schema, because it
asked the framework to enforce something.

## Sequencing

One cut, because the declaration's shape must be final in a single commit or the
blocked consumer migrates twice. It deletes the Journal event schemas, the
reducer contract, revision and the CAS; it deletes the `reducer_closure` slot;
it reshapes `umbraflow-project.json` into the final inlined single-document
form; it migrates every in-repo fixture and the consumer's declaration; and it
migrates the one production byte-set on disk — the sequence-0 baseline row,
which is deleted with its table.

Separable, and following immediately rather than riding along: removal of the
pure program type if nothing else compiles on it (verified on this date: the
only production consumer is the reducer at
`modules/operator/source/operator/project-generation.cpp:438`), deletion of the
target-wide mutation barrier, and enforcement of `on_timeout`. None of these
touch the declaration, so none of them can cause a second migration.

Recording that an outcome is unconfirmed survives as a recorded outcome state.
Freezing a target until something reconciles it does not: that is a decision
about what may happen next, and it is the Project's.
