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
The twelve host primitives (`cycle_open`, `cycle_close`, `cycle_page`,
`cycle_find`, `cycle_click`, `deadline`, `wait`, `settle`, `raise`, `emit`,
`terminal`, `random`) that only the trusted Luau framework can reach, held as
closure upvalues and never as a key of any table. A project task can neither
name nor reach them; it sees only the `uf` capability namespace and `ctx`.
Defined in `docs/plans/2026-07-29-three-layer-task-system.md` §5 (2026-07-29).
_Avoid_: native driver, private native surface, raw verbs

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
