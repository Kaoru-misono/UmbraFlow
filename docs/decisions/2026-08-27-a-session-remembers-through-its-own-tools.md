# 2026-08-27 — A session remembers through its own Tools

This continues
[`2026-08-25-interactive-code-is-a-scoped-tool-program.md`](2026-08-25-interactive-code-is-a-scoped-tool-program.md),
which fixed what an interactive chunk *is*, and answers what that left a caller
unable to do: keep anything.

## The measurement

Driven against a live session on this date, one chunk at a time:

- `_G` does not exist, and neither does `getfenv`.
- Assigning any global raises `attempt to modify a readonly table`.
- `require("<declared module>")` works for a Project module the deployment's
  `tool_closure.modules` declares, and two requires of one name inside ONE chunk
  return the same instance, which is mutable.
- Across chunks that mutation is gone. Each `evaluate()` builds a fresh VM and a
  fresh module graph, so the module is re-evaluated and the table is a new one.
- The only channel that survived a chunk boundary was
  `framework.project.read_text` / `framework.project.write_text`.

The consequence in the consuming project was concrete: an entire multi-turn
battle had to be written as one enormous chunk, because a second chunk would
begin knowing nothing. That is not a property anybody chose. It is what falls
out of "one chunk is one bracket" when nothing else in the session is allowed to
hold a value.

## The ruling

**A session keeps state through three ordinary Framework Tools**, and through
nothing else:

| Tool | Takes | Confirmed result |
| --- | --- | --- |
| `framework.session.set` | `name`, `value` | `name`, `stored_bytes` |
| `framework.session.get` | `name` | `name`, `present`, `value` when present |
| `framework.session.list` | nothing | `names`, `stored_bytes`, `maximum_names`, `maximum_bytes` |

The Tool face renders them from the pinned catalog with no per-Tool source
anywhere, so a chunk writes:

```lua
local session = require("@umbraflow/session")
session.set{ name = "battle", value = { turn = 3, cards = { "strike", "guard" } } }
-- ...a later chunk of the same session...
local held = session.get{ name = "battle" }
if held.present then
    act(held.value.turn)
end
```

`value` is any JSON but null, stored and returned byte for byte. Null is left
out because `present` already spells absence once, and a stored null would be
the same answer read two ways.

The store is **read-only with no effect bound**, on the terms
`framework.audit.record` already set: read-only means no external-world effect
requiring plan authority and approval grants, and a map that dies with the
process reaches no world at all. So a session under the deny-all artifact can
still remember what it is doing — which is the whole point, since an annotator
forced to re-derive its state every chunk is no better off than one holding it
in a single enormous chunk. It is Semantic, because a name and a value are the
caller's vocabulary and never the machine's.

### The lifetime contract

**"Session" means the Operator session** — the one `service::ProductLifecycle`
mints `session-<hash>` for and reports as `session_id` from
`framework.workflow.status`. Not the chunk, not the VM, not the process's whole
life, and not the project directory.

The store is a member of the object that mints that id. It is born with the
session, it is memory and never a file, and it dies when the lifecycle shuts
down. A later session over the same Operator root, the same project and the same
installed generation starts **empty**, and its `framework.session.list` says so.
Something that must outlive a run is written with `framework.project.write_text`
into the project's own authoring store, which is what that Tool is for.

Two ceilings bound it — 256 names, and 1 MiB across names and values together —
and a `set` past either is refused naming what was exceeded and what the limit
was. No single value can cross the byte ceiling on its own, because exact
canonical JSON is already bounded at a megabyte before it reaches the store; the
ceiling is there for accumulation, which is the only way a session's memory
could grow without bound while it holds a target.

### Replay determinism

`ToolRuntimeExecutor::invoke` calls `replayToolCall` **before** the provider, so
a call position that already has a durable outcome answers from the ledger and
the provider never runs. That is what makes `framework.workflow.now` honest, and
it is the property this design had to survive.

It splits cleanly:

- `get` and `list` read something only this process knows. Their answers become
  terminal outcomes, so a replayed position answers the reading that was
  recorded rather than a fresh one — exactly `now`'s property, for exactly
  `now`'s reason.
- `set` is the one that could have gone wrong. A replayed `set` returns the
  recorded outcome without storing anything, so a store that had lost the entry
  would then disagree with a record saying it exists.

**It cannot, on the door this exists for.** An interactive call's root request
key is `interactive-<sessionId>-<n>`. `sessionId` is a hash over the session
manifest, the controller, the target, a steady-clock reading and a per-process
counter, so no two lifecycles mint the same one; `n` comes from a counter on the
same object and rises on every interactive call. No interactive root key
therefore repeats, within a process or across processes, so no interactive
position ever finds a durable row and the provider always runs. The store lives
on that same object, which is what makes this structural rather than two
counters that happen not to collide: the only keys that could address a row
describing this store are keys this object minted, it mints each one once, and
it dies with the state those rows described.

On the three actor doors the request key is the **caller's**, and a caller that
re-presents one is asking for the earlier run's recorded outcome — that is what
a root request key means, and it is why `umbra-flow invoke` can store into a
session that ends when its process does. Nothing about that is specific to this
store: it is the same property `framework.screen.observe` has, measured on
2026-08-25 and fixed for the interactive door in exactly this way. Determinism
is intact either way, because a replay answers what was recorded. What a replay
never does is invent an entry: an entry exists because the first attempt made
it.

Nothing is lost that the ledger does not already hold, either. A `set`'s
canonical arguments ARE the name and the value, and they are durable. What dies
with the session is only the framework's willingness to answer from them, which
is deliberate: folding its own ledger rows back into a Project's state is the
interpretation
[`2026-08-23-the-framework-stops-interpreting-project-state.md`](2026-08-23-the-framework-stops-interpreting-project-state.md)
removed.

## What was rejected

### A writable per-session environment table

The obvious answer — make the global table writable, or hand each chunk a
mutable `session` table — fails twice.

It does not work. The table would have to live in the VM, and the VM is built
for one chunk and destroyed after it, so a writable environment would be a
writable environment that still forgets. Keeping one Luau table alive across VMs
is not possible without serialising it, at which point it is a store with a
worse interface.

And if it did work it would be wrong. `docs/ARCHITECTURE.md` records that no
ambient authority reaches project code, and that a project's boundary is one
closure whose single native seam is a synchronous Tool call. A per-session table
is state with no name in any catalog, no argument schema, no admission decision
and no ledger row — the exact ambient channel that absence exists to forbid. The
readonly environment is not an obstacle that had to be worked around; it is the
property that makes "everything a chunk can reach is a Tool" true, and a store
reached as a Tool keeps it true.

### Reusing `framework.project.write_text`

It is what the consumer had, and it is the wrong tool for the job in three ways.
It is Mutating with an effect bound, so scratch state needs the same Operator
authoring grant that real annotation output needs. It is durable, so state whose
whole meaning is "for the next few minutes" accumulates in the project directory
forever. And it carries text, so every value is encoded and decoded by hand.
Keeping it as the answer would also have made `write_text` mean two things.

It stays exactly what it was, and the session store's own description points at
it for anything that must outlive the run.

### Making the store durable, in `operator-runtime.sqlite`

This is the serious alternative, and its one advantage is real: a durable row
written in the same store as the ledger cannot disagree with the outcome that
records it, which closes the caller-supplied-request-key case above completely.

It was rejected because it buys that by breaking the contract it is
implementing. State that survives the session is state with no owner and no end:
nothing retires an Operator session's rows, so the store would need a reaper, an
eviction rule, and a decision about what a *later* session may see of an earlier
one — three questions this change does not have to answer and would answer
badly. It also costs a schema identity move and an exact-pair migration, for a
hazard that only appears when a caller deliberately asserts, across a restart,
that its new call is a retry of an old run.

Session-scoped state that lives exactly as long as its session is the smaller
and more honest thing.

### The durable Project channel frozen on 2026-08-23

[`2026-08-23-the-framework-stops-interpreting-project-state.md`](2026-08-23-the-framework-stops-interpreting-project-state.md)
froze the contract for a durable, opaque, Project-owned append channel — a
per-Project gap-free sequence with chained digests — and deliberately left its
implementation off the critical path.

This is not that, does not implement it, and does not pre-empt it. That channel
is an append-only log whose whole point is that it survives and replays; this is
a map that forgets. A Project that wants the log still gets the log, and a
Project that only wants to remember which turn it is on no longer has to wait
for one.

### A fourth verb

No `delete`. The store dies with the session, a name can be overwritten, and the
ceilings are sized for a scratchpad rather than a database. A verb that existed
only to make room inside a bound nothing is expected to reach would be
speculative surface on a catalog every model has to read.

## Consequences

- The Framework Tool catalog declares twenty-eight Tools, and
  `tool_catalog_hash` moves. Nothing outside this repository pins that value.
- Nothing is removed and no spelling changes, so no project author has to
  migrate anything and there is no release note to write.
- `SessionStateStore` is `operator`'s, beside the rest of the Tool Runtime, and
  is where the ceilings and their refusals are spelled once. `service` performs
  the three Tools as it performs every other Framework Tool, so the declaration
  stays on the side that declares and the performance on the side that can
  touch the world — the split
  [`docs/plans/2026-08-27-split-the-tool-provider-file.md`](../plans/2026-08-27-split-the-tool-provider-file.md)
  says must not move.
