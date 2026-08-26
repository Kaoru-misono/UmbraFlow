# 2026-08-26 — A Tool is a flat call over an explicit reference

The Tool surface grew a shape nobody asked for. It has tagged arms, bodies that
re-enter the VM, measuring Tools that bind to "the innermost open frame" by call
position, and results that must be canonical JSON because they are written
inline into a durable row. Each piece was reasoned; together they are a protocol
a caller has to be taught rather than one a caller can read.

This ruling replaces that shape with the one the owner named, and it supersedes
ten earlier rulings in the clauses listed at the end.

## The shape, in the owner's terms

Every Tool is called the way a modern agent harness calls one:

```json
{ "name": "Bash",
  "input": { "command": "git status --short", "timeout": 120000 } }
```

- A Tool is a **name plus one flat JSON Schema object**. Optional properties
  carry defaults; every property carries a description.
- **No arms.** Different behaviours are different Tools.
- **No body, and no nesting.** A caller never invokes a Tool inside another
  Tool's callback. Sequencing is the caller's, and so is the state carried
  between calls.
- **No implicit scope.** Nothing binds to "the innermost open something".
- **A failure is the result the caller reads**, not a value they must
  interrogate before knowing whether anything happened.

The flow that follows is the one the owner described: capture a screenshot,
resolve what is in it, then act on what was resolved.

```text
screen.capture              -> screenshot receipt
screen.observe(screenshot)  -> surfaces, ui_targets, bindings, readings
ui.click(observation, ...)  -> acted
```

## What the code invented, and was never given

**Every Tool call's payload is canonical JSON stored inline in a durable row.**
Nobody ruled that. It is the reason there is no `framework.screen.capture` and
no crop Tool — a result carrying pixels would put pixels inside a hashed record
— and it is stated as such at the constants it forbids
(`modules/operator/source/operator/tool-invocation.cpp`).

`docs/decisions/2026-08-23-the-framework-enforces-limits-it-was-given.md` draws
the line this falls on: the framework records, verifies and enforces limits it
was **given**, and never limits it **invented**. This one it invented.

The record is not even one row. Canonical arguments and their hash live in
`tool_call_positions`; a terminal outcome payload and its hash live in
`tool_call_history`. Two tables, two hashes, and a call in flight has no result
at all.

## Z1 — The durable record stays; the payload leaves it

A Tool call still leaves a durable record, because durable coordinates and
outcomes are what buy replay, nondeterminism detection, attribution,
authorization evidence and protection against duplicated effects.

Traceability needs the record to identify the Tool, the caller, the provider,
the run and the call coordinate; the canonical input and its hash; the admission
authority; the terminal classification; and **the returned artifact's content
hash, size, media type and provenance** — frame identity and rectangle where
there is one. It does not need the artifact's bytes to sit in a SQLite text
column.

**Small semantic results stay inline. Binary and large results travel as a
content-addressed artifact, and only their receipt enters the ledger.**

This is not a new mechanism. `2026-08-20-project-execution-identity-is-closure-plus-environment.md`
already rules exactly this pattern for external capability results: a
content-addressed evidence store, a canonical receipt, **the receipt committed
only after the blob is durable**, an evidence closure containing exactly the
receipt-named blobs, host readers that expose bytes by digest and cannot select
a blob the explicit input did not name, and replay bundles that carry and
re-verify the same blobs. That ruling stands; this one puts screenshots under it.

What moving pixels out costs, stated plainly: the ledger file stops being
self-contained, a deleted blob makes its payload unavailable, and replay from
the SQLite file alone stops being possible. What it does not cost: integrity,
provenance or tamper detection. A missing blob is detectable and a corrupted one
fails its hash.

## Z2 — A screenshot is a value, so the frame stops being a scope

`screen.capture` answers with an **immutable screenshot artifact identified by
its content hash**. Every measuring Tool takes that hash explicitly.

Passing the same hash into two calls is stronger evidence that they measured one
screenshot than lexical nesting ever was: the dependency is in the canonical
arguments, where it can be read, replayed and checked, rather than in the shape
of the source. Calls can then be reordered, retried and inspected like any
other.

**The invariant that forced the current shape is not load-bearing.** "A handle
could be stored and a frame may not" protects a live, borrowed engine frame from
escaping its cycle. Turning that frame into an **owned immutable blob** removes
the thing being protected: once the bytes are owned and content-addressed,
storing a reference to them is correct rather than forbidden.

The costs are real and are accepted: a screenshot must be retained for a defined
lifetime, storage becomes explicit, every consumer passes the reference, and a
missing or expired artifact becomes an ordinary refusal that names what expired.

### Where the blob lives

**On disk, content-addressed, in the Operator root, under its own store —
not in `runtime-artifacts/`.** Same discipline as the artifact store — staged,
then renamed, so a half-written blob is never mistaken for a whole one — but a
separate store, because the two have different lifetimes: an installed release
artifact is rare, immutable and long-lived, while a screenshot is frequent,
session-scoped and disposable. One `reclaim` should not have to serve two
retention rules.

Memory is a cache below this and never the contract. A hash outlives the process
that minted it: it is passed between separate Tool calls, and those calls may be
separated by a restart or by a replay. **A hash pointing at nothing is not
traceability.**

**How long a blob is kept is the operator's decision, not the framework's.** The
framework exposes the ceiling and enforces what was chosen, and `reclaim` sweeps
what no retained run still references — the rule it already applies to
unreferenced artifact directories.

## Z3 — Six input verbs are six Tools

`framework.input.deliver`'s six-arm tagged union becomes six Tools, and
`framework.project.write`'s two arms become two.

Splitting loses one closed enumeration in one descriptor, one catalog entry and
one policy grant. It loses **no delivery capability**; the shared delivery
implementation stays internal.

It buys a straightforward schema per Tool, no meaningless `action`
discriminator, per-verb timeout, risk and idempotency, and — the one that
matters — **least privilege**: a policy can admit `move` without admitting
`click` or `key`, which the tagged union cannot express.

The RuntimeModel may keep a closed action vocabulary of its own. That is a
Project's declaration about its own screens and is not a reason to repeat the
union at the Tool boundary.

`framework.project.write`'s `capture` arm does not survive under another name.
It fuses two acts — take pixels, write a file — and it exists only because the
architecture refused to return pixels. With `screen.capture` and
`screen.crop` answering with artifacts, the flat equivalent is two calls, and a
failure between them is visible: the screenshot exists and the project write did
not happen.

## Z4 — A hold carries a duration again, and returns what it saw

`framework.input.hold` presses, waits its declared `duration_ms`, optionally
captures or captures-and-observes **while still pressed**, releases on every
exit path, and answers with what it saw.

`return_screen` is `none`, `capture` or `observe`, defaulting to `none`.

This is the capability the body was built for — evidence of what was on screen
while an input was engaged — and it delivers it without a caller callback, which
means **a held button cannot leak**: press and release are one Tool's business
rather than a scope the caller could exit in a way nobody anticipated. The
unconditional-release invariant that
`2026-08-24-on-timeout-is-enforced-and-no-input-outlives-its-call.md` added
survives and gets easier to keep.

What is given up: arbitrary caller computation and arbitrary child Tool calls
while the button is down. An interaction that genuinely needs "observe, decide,
then deliver another input while still held" would need its own named atomic
Tool, and none is known to be needed.

## Z5 — A failure is the result

The distinction is right and the caller-facing shape is not. Delivery
uncertainty must stay data: `possible` cannot become an exception, because an
exception discards whether an input may have landed.

So: provider and domain failure stay a durable result; protocol, admission and
replay violations keep raising. What goes is **the success-shaped answer plus
the `tools.state` / `tools.result` / `tools.evidence` interrogation protocol.**
A call answers directly:

```json
{ "ok": false, "call_identity": "...", "delivery": "terminal_failure",
  "error": { "code": "io_failure", "message": "…", "retryable": false } }
```

`possible`, `proven_absent` and `terminally_unresolved` stay explicit delivery
values. `proposed`, `admitted` and `dispatching` are internal ledger states and
stop being advertised as results of a synchronous call.

The evidence is on the record. On 2026-08-25 every observation of a live session
failed, the body never ran, and the chunk reported success — three probes were
read as "the screen has no text on it" when no observation had happened at all.
The failure reason had reached the caller all three times and the SDK wrapper
replaced it with a constant sentence.

## Z6 — A Project Tool is a leaf

A Project Tool publishes a name, a description and a flat input schema. No body,
no child-effect declaration, and **its handler issues no Tool calls**. It is a
transform over explicit inputs; composition is the caller's.

The sole consumer shows the cost of the current shape: each of its two read-only
Tools carries a large hand-authored descriptor, declares `framework.screen.observe`
as a child, and observes inside its own handler. Under this ruling the caller
writes three ordinary calls instead, and the Project Tool needs neither child
bounds nor an observation budget.

## What is deleted

`framework.input.deliver`, `framework.input.semantic_target`,
`framework.project.read`, `framework.project.write`, the `action` tag and both
arm tables, and the project-write `capture` branch.

`ToolBodyArm`, `ToolBodyDeclaration`, the body argument on the VM-facing invoke
primitive, `ToolBodyRun`, `ToolBodyProvider`, `ToolBodyPostcondition`,
`attachToolBodyScope`, `runToolBody`, `runChildToolBody`, the dispatcher's
body-scope close state and its `ObservationFrame` state, and the call-position
lookup of the innermost open frame.

`ChildEffectDeclaration`, `child_effects`, `workflow.child_flow`, nested
Project-handler Tool issuance, `screen.observe(callback)`, and the
`tools.state` / `tools.result` / `tools.evidence` double-accessor protocol.

And the rule that an outcome payload must be stored inline because the call
record is canonical JSON.

**What is kept**: namespaced Tool ownership, the single admission funnel, policy
enforcement, durable replay coordinates, content hashes, and every held input
released on every exit.

## What this supersedes

These files keep their bytes. Each is superseded only in the clauses named.

1. `2026-08-24-an-observation-frame-is-the-scope-of-its-call.md` — call-position
   binding, and the prohibition on a storable frame reference.
2. `2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md` — V2's held
   observation and V3's one tagged input Tool. **V1's policy axis stands**, and
   so do its effect-ownership rulings.
3. `2026-08-25-a-body-is-one-shape-and-hold-takes-one.md` — the body mechanism
   entire. `hold` regains an explicit duration and answers with `return_screen`.
4. `2026-08-21-tools-are-the-shared-game-driving-boundary.md` — the clause
   admitting calls from inside a Project Tool handler, and the nested-delegation
   model. **One Tool Runtime and the durable call boundary stand.**
5. `2026-08-22-handler-and-automation-script-are-one-mechanism.md` —
   handler-as-caller, `ChildEffectDeclaration`, and body-shaped dispatch.
6. `2026-08-22-re-entry-is-keyed-on-recorded-children.md` — its safety argument
   assumed a handler's only effects are durable child calls. With leaf handlers,
   re-entry must be ruled from a handler's own purity and idempotency contract.
7. `2026-08-22-scoped-tool-execution-is-a-third-program-type.md` — structured
   body re-entry and per-parent body contexts. **The synchronous call, and
   results as references rather than large payloads, stand.**
8. `2026-08-25-interactive-code-is-a-scoped-tool-program.md` — the body-taking
   exception. **Each interactive top-level call stays an independent root call.**
9. `2026-08-23-what-the-framework-publishes-for-the-tool-runtime.md` — the
   published answer envelope, per Z5.
10. `2026-08-22-a-join-needs-two-independent-sources.md` — only the exact
    binding-table carrier, and only if Project closure exports become exact Tool
    names. **The independent-source principle stands.**

Not touched, and not in question: there is no annotation phase and no runtime
phase; policy is the Operator's; a Tool name is owned by its namespace; one
admission funnel serves every caller; no input outlives its Tool call; and the
framework enforces limits it was given rather than limits it invented.

## Deliberately left unresolved

- **Retention and export.** How long an evidence blob is kept, how replay
  bundles carry them, and the exact atomicity of blob-before-receipt. The
  operator chooses the ceiling; the framework enforces it and says specifically
  what expired.
- **Semantic versus raw coverage.** Both surfaces are kept — `framework.ui.*`
  under observation authority and `framework.input.*` under privileged policy —
  because both are expressible today. Narrowing that is a ruling about which
  combinations are unwanted, and nobody has asked for one.
- **When a hold captures.** One capture after the declared dwell and before
  release is the recommendation. Repeated captures would justify a separately
  named Tool with a bounded count, never a callback.
- **Whether `screen.crop` survives.** The authoring flow needs rectangle PNGs
  and `cycleCrop` already produces them. If the caller's own harness can crop an
  image it holds, the Tool is unnecessary. What must not return is
  capture-plus-project-write as one act.
- **`screen.observe`'s exact answer.** Its observation authority currently
  carries the RuntimeModel's declared UI-target vocabulary rather than the
  targets resolved in that frame, and the code names that as debt. The result
  schema for surfaces, targets, bindings and action identities settles the
  semantic Tool schemas above.
