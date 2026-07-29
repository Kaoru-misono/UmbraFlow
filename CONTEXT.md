# UmbraFlow

A strictly-background personal game automation app: a C++ host observes a game
window, recognizes annotated pages, and delivers provable background actions;
task scripts written in Luau drive the loop through a minimal capability API.

## Language

### Scripting

**Capability namespace (`uf`)**:
The single read-only global root through which a project task script reaches
the host: `uf.pages`, `uf.recognizers`, `uf.task`, and `uf.errors`. A script
sees nothing of the host outside this root, and effects reach it only through
the `ctx` object passed into a task's `run`. `uf` is the same product
abbreviation as the C++ `uf::` namespace, used deliberately in both languages.
_Avoid_: `umbra` (the 2026-07-27 spelling of this root; renamed to `uf` on
2026-07-29, see `docs/plans/2026-07-29-three-layer-task-system.md` §6 and §18 —
the rename touches only this script root, never the product names `UmbraFlow`
and `umbra-flow` or the schema ids `umbraflow-authoring/v2`,
`umbraflow-annotations/v1`, and the merged trace schema `umbraflow-trace/v1`,
which shipped on 2026-07-29 in `modules/trace`), `bot` (superseded draft
wording in the grill decisions and the S0 annotation design)

**Private capability surface**:
The one host-built table of primitives that only the trusted Luau framework can
reach — the observation cycle, the bounded waits, `raise`, `emit`, `terminal`
and `random`. The primitives ARE keys of that table; what makes it private is
that the table itself has no name in either environment: the host hands it to
the framework bundle as its chunk argument, drops its own reference, and the
only way to reach it afterwards is the closure it was handed to. A project task
can neither name nor reach it; it sees only the `uf` capability namespace and
`ctx`. The table also carries one non-capability field, `error_tag`, the Tier B
label `ctx:try` compares against.
The roster and its four invariants are defined in
`docs/plans/2026-07-29-three-layer-task-system.md` §5 (2026-07-29).
_Avoid_: native driver, private native surface, raw verbs, "never a key of any
table" (the 2026-07-29 draft wording; §5's own tightening note replaced it,
because the primitives are exactly the keys of the private table — read
literally it says the code violates its own design)

**Framework environment**:
The writable globals table the trusted Luau framework's modules load under, and
the only environment whose metatable chains `__index` to the VM's main globals.
Environment isolation in Luau is **per closure, not per thread** — `luau_load`
takes the env a chunk closes over, and a new thread copies its parent's globals
— so this is a real table in the VM registry, never the `luaL_sandboxthread`
proxy shape, which is the `_G` escape shape the design exists to exclude.
_Avoid_: framework sandbox, trusted thread, framework `_G`

**Project environment**:
The globals table one project task run executes under: an explicit whitelist
with **no metatable at all**, so there is no `__index` chain to the framework
environment or the main globals. It is registered as a frozen prototype and
shallow-copied per run, so globals a run writes die with that copy. The absence
of the metatable is the isolation property itself; the denial list is a
consequence of it, not the mechanism.
_Avoid_: script sandbox, task environment, per-run `_G`

**Tier B error carrier**:
The value a recoverable automation failure is raised as: a userdata the host
mints under its own Luau tag, wearing a protected metatable labelled
`uf.error`. C++ decides what it is by the tag alone; the label is what tells it
apart from the other host userdata a script can hold (a page, a hit) and is
what `ctx:try` compares. A project script can mint no tagged userdata at all —
`setmetatable` and `table.clone` take tables, and `newproxy` is absent from
both environments — so the carrier cannot be forged however exactly a table
copies its fields.
_Avoid_: error table (the shape before 2026-07-29 `c37ee5b`, where identity
rested on a metatable a `table.clone` could carry along), exception, error
object

**Task**:
One automation flow (such as a game's daily routine) authored as a Luau
script and executed by the host in its own isolated VM against one target
window. A task always belongs to exactly one project and is addressed by its
name within that project, never by a loose file path.
_Avoid_: script (the source artifact a task is written in, not the unit of
execution), job, macro

**Project**:
Everything the host needs to automate one game target: the authoring
document, source screenshots, the generated runtime recognition assets, and
the tasks written against them.
_Avoid_: workspace, profile

### Runtime

**Engine session (`engine::EngineSession`)**:
The stateful engine capability scope that owns one loaded recognition runtime,
one frame source, one action sink, and one trace sink. It vends observations
and authorizes actions for one bound target. The name describes the lifetime
of that capability bundle; it does not imply that the object has a
`CaptureSessionId`.
_Avoid_: engine run (would conflate the capability scope with execution
identity and `EngineRunId`), capture session

**Capture session identity (`CaptureSessionId`)**:
The outer component of frame identity. Together with `TargetGeneration` and
`FrameId`, it prevents evidence from distinct capture sessions from
colliding. It is supplied by the composition root and belongs to captured
frames, not to `engine::EngineSession`.
_Avoid_: `SessionId` (too generic), engine session id, task session id

**Observation cycle**:
The explicit open/close scope around exactly one capture, inside which page
resolution, recognizer lookup, and a single click all read the same frame.
Opening it costs one capture; closing it releases the frame deterministically
and is idempotent. It replaces GC-driven frame lifetime, and because a page and
a hit from one cycle are same-frame by construction, cross-frame evidence
mismatch stops being checkable state and becomes structurally impossible.
Defined in `docs/plans/2026-07-29-three-layer-task-system.md` §4 (2026-07-29).
_Avoid_: frame box, observation lease (the freshness contract on a delivered
action, not this scope), capture scope

**Ticket**:
The script-side handle to one open observation cycle. C++ owns the ticket
ledger and re-checks the ticket on every use; when the cycle closes, the ticket
is dead and every later operation on it fails. The ticket is the unit the host
invalidates after an action, and it carries no data the script can read.
It names its cycle by two numbers, both re-checked: the ledger's process-wide
generation stamp and the cycle's ordinal within that ledger. The stamp is what
makes a ticket left over from a spent generation get rejected rather than
collide with a live ordinal in the next one.
Defined in `docs/plans/2026-07-29-three-layer-task-system.md` §4 (2026-07-29);
implemented in `modules/task/source/task/cycle-ledger.hpp`.
_Avoid_: frame handle, cycle object, token (reserved for `std::stop_token`)
