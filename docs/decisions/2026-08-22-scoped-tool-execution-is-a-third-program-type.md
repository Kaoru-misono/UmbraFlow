# 2026-08-22 — Scoped Tool execution is a third program type

## Decision

This ruling refines, and does not reopen,
[`tools are the shared game-driving boundary`](2026-08-21-tools-are-the-shared-game-driving-boundary.md).
It fixes the five questions that decision left to implementation: which program
type runs scoped code, what shape the native call has, how the scoped modules
are authored, what the replay key is, and how `@umbraflow/audit` reaches
durability.

### A third program type, `ScopedToolProgram`

`script::PureDataProgram` is not extended. It keeps its stated invariant that no
host installer or native capability seam is part of its API, and the
five-function ProjectPlugin path and the Journal reducer keep running on it
unchanged.

`script::ScopedToolProgram` is a third program type. It shares
`PureDataProgram`'s closed-graph compilation, host-owned `require` resolver,
per-invoke fresh quota-bound VM, resource closure, and deep-freeze discipline,
and it adds exactly one native seam: the Tool Runtime call. Both Project
automation scripts and Project Tool handlers run on it. The shared closed-graph
compiler, resolver, and deep-freeze machinery is factored into shared internals
used by both types rather than copied.

`ScopedToolProgram`'s environment-hash preimage includes the scoped module
catalog and the Tool Runtime facade contract — arity, result shape, and failure
behaviour — and is distinct from `pluginEnvironmentMaterial()`.

### A synchronous blocking native call, with no yield protocol

The scoped Tool call is a straight synchronous C function that runs
`ToolRuntimeExecutor::invoke*` to completion and returns the decoded result.
There is no yield protocol, no coroutine suspension, and no async completion
callback in the VM.

Cancellation is teardown, not unwind. A cooperative stop is delivered as the
recorded outcome of a Tool call — `framework.workflow.wait` returning a stopped
classification the script branches on. A hard stop signals a stop token into the
provider and then destroys the VM without resuming the script; it is
deliberately indistinguishable from a crash, and the existing durable machinery
already covers it. There is no third path that returns a cancellation error into
script code.

Providers that wait or deliver native input take a stop token, and the Luau
interrupt hook is armed with the same token, because a script can spin in pure
computation without ever reaching a Tool call. The run executes on a structured
worker owned by the run context; no detached threads.

The VM memory quota meters VM allocations only. During a blocked native call the
VM allocates nothing, and provider buffers are host-owned C++ values under
Operator budgets; the quota is charged at result-decode on return. Tool results
are therefore references, not payloads, on the template of the existing
`framework.screen.observe` opaque snapshot handle.

The `deepFreeze` rule that `__index` must be a table and never a function
stands. Its rationale mentions a future yield protocol; that phrase is option
preservation, not an unpaid debt, and a function `__index` is script-reachable
host code regardless.

### Luau-authored facades over one native primitive

`@umbraflow/tools`, `@umbraflow/screen`, `@umbraflow/workflow` and
`@umbraflow/audit` are Luau sources in the Framework bundle, exactly like the
pure modules. Boot builds one private capability table holding the small native
surface — essentially a single `invoke` C closure and nothing derivable from it
— hands it to each scoped Framework module as its chunk argument, and drops it.
This is the pattern `script::Engine`'s `PrivateCapabilityInstaller` already
proves. Primitives become upvalues of trusted closures, and the returned facades
are deep-frozen plain tables of functions.

Every ergonomic — `screen` wrappers, `workflow` bounded wait, delivery
classification, and recovery helpers with no blind retry, and the result helpers
— is pure Luau over `invoke`. Tool discovery and description in
`@umbraflow/tools` is a frozen data table baked at program construction from the
pinned catalog bytes, not a native call.

Snapshot references and other handles stay plain JSON data wrapped by frozen
helper functions. There is no userdata and no metatable machinery, so the
`__index`-must-be-a-table rule is satisfied by never needing `__index` at all,
and replay equality is byte equality of recorded JSON.

"Cannot be retained into another run" is structural, and must not be backed by a
runtime generation-token check. Three facts already close every channel: each
run or handler invocation gets a fresh VM and the per-VM module cache never
crosses VMs; the only egress is one decoded JSON value, and closures carrying
upvalues are not JSON-representable; and the capability table is dropped after
module loading and is never a key project code can name. A generation token
would be a branch whose failure path can never be exercised.

The `invoke` upvalue borrows run-scoped C++ state — controller binding, lease,
root identity, and the per-context child counter. The run context owns the VM as
a member, so the lifetime contract is one line at the declaration: the VM is
strictly outlived by its run context, and the primitive's pointer to the run
context is a borrow backed by that ownership. Member order enforces it.

### A per-parent child counter, and the ordinal coordinate as the replay key

The call sequence is a monotone child index per issuing context — the root
script's context, and one per live handler invocation — assigned and incremented
exclusively by the C++ seam. Scripts never see or influence it.

The replay lookup key is the ordinal coordinate (root, parent position, child
index) alone. Tool name, tool version, canonical argument bytes — JCS-
canonicalised before comparison — execution identity, and observation references
are stored attributes compared field by field at that coordinate, and the first
mismatch stops the run with that field named in the diagnostic.

A restarted script that terminates leaving recorded calls unconsumed for some
context is divergence and stops the run. A call arriving under a different
parent is divergence too, and manifests as a coordinate mismatch reported as a
changed parent.

The frozen ruling's changed-field list includes `sequence`. Under this ruling
sequence *is* the address, so a changed sequence can only ever surface as a
changed-parent or changed-ordinal divergence, never as a changed attribute at a
matched coordinate.

"Root call" in `@umbraflow/tools` means a parentless position under the run's
existing `ToolRootRequestIdentity`; "child call" means a parented position under
the same root. Scripts never mint root request identities.

### `@umbraflow/audit` is a real Tool

`@umbraflow/audit` is Luau sugar over a genuine `framework.audit.record` Tool
with a descriptor in the Framework Tool Catalog, invoked through
`ToolRuntimeExecutor::invokeReadOnly`. The project-semantic record, auto-
attributed by the seam-supplied root and position, is the call's own durable
outcome. The provider performs no external work.

Read-only is the correct classification. "Read-only" means no external-world
effect requiring plan authority and approval grants, not "writes no rows" —
`framework.screen.observe` is read-only yet persists durable snapshot
references. Audit therefore needs no `OperatorPlanAuthority` and no
`ProposedEffect`. It does spend Tool-call budget. If chatty telemetry later
proves too expensive, the lever is a per-descriptor budget weight in the
catalog — policy inside the one path — never an exemption from the path. Scoped
call budget weight is per-descriptor policy, and that policy lives inside the
single path.

Audit records are not attached as evidence on the current call: a handler may
emit records not aligned to any call boundary, and attaching them would give
audit distinct semantics from every other scoped operation.

## Context

The frozen 2026-08-21 ruling settled that every scoped operation crosses one
Tool Runtime, but not what executes the scoped Luau. Two shapes were live. One
extended `PureDataProgram` with a capability-set constructor flag; the other
added a separate program type. The required experiment E7 decides between them:
a reducer's attempt to load a scoped module must fail *by module name*. That is
a load-time resolver property, so the scoped module set must be a property of
the program type, not of a VM boot or a constructor flag. A capability-set-
parameterised single type is the same defect wearing a different hat — purity
becomes a value rather than a type, and `PureDataProgram` in a signature stops
telling the reader whether the thing in hand can reach the world. That is the
"absent means the old behaviour" double reading `CLAUDE.md` forbids. Two
genuinely different contracts get two types; the no-shim rule forbids two
spellings of one thing, not one spelling each for two things.

A yield protocol exists to preserve a stack that must survive, or to multiplex
scripts on one thread. The frozen ruling already destroys the first — the stack
is never serialised and restart-replay *is* the durability mechanism — and the
second is an unrequested scheduling optimisation that would create a second
suspension mechanism beside deterministic restart, leaving no way for a reader
to tell which one carries the durability contract. Blocking is bounded by
construction because every waiting primitive is a Tool with a validated bounded
duration.

Per-parent numbering rather than a run-global counter follows from replay
short-circuiting with no provider execution: a replayed parent's handler never
runs and its descendants are never re-issued. Under a global counter the next
new call after a replayed subtree would need one more than the subtree maximum,
making the counter a function of recorded history. Under per-parent numbering a
replayed child costs its parent exactly one increment regardless of subtree
size, so each context's numbering depends only on that context's own behaviour.

Coordinate-as-key follows from what the alternative does on a changed argument.
If the position identity folded name and arguments into an opaque fingerprint, a
changed argument would produce a lookup *miss*, indistinguishable from a first
new call — which would then execute, silently converting mandated stop-the-run
nondeterminism into nondeterministic re-execution. That is a violation of the
frozen ruling, not merely a worse design. Script-influenced sequence is
forbidden outright for the same family of reason: it would let a script alias
another call's position and inherit its recorded outcome without authorisation.

Audit had one apparent alternative, an Operator-side scoped append. "Expose no
native primitive except by making a normal Tool call" leaves no channel for it:
a scoped Operator-side append is definitionally a second native primitive with
its own delivery and durability semantics, the second execution path the frozen
ruling forbids by name. Replay idempotence closes it independently — on restart
the script re-executes all of its logic and only Tool calls short-circuit, so an
append outside the Tool boundary would re-append on every restart. The durable
suspension boundary is the only dedup mechanism in the architecture, and an
audit append is an effect.

## Consequences

- `PureDataProgram`'s no-native-seam invariant survives intact, and
  `2026-08-20-project-execution-identity-is-closure-plus-environment.md` is not
  reopened by scoped execution.
- There are two environment identities to derive and pin, not one:
  `pluginEnvironmentMaterial()` for the pure program and a distinct
  `ScopedToolProgram` preimage covering the scoped catalog and facade contract.
- The native surface reachable from Project Luau is one `invoke` closure. Every
  scoped module is reviewable as Luau source in the Framework bundle, and moving
  any of those bytes moves the scoped environment identity.
- Non-retention is asserted by structure and reviewed by argument. Adding a
  runtime generation token to "prove" it would add a branch nothing can
  exercise.
- The Operator seam owns the call sequence. Any envelope that accepts a
  caller-supplied sequence for a scoped call contradicts this ruling.
- Replay divergence must name the field that diverged. A single undifferentiated
  refusal message does not satisfy it.
- The Framework Tool Catalog gains `framework.audit.record`, and audit spends
  Tool-call budget like every other scoped operation.
