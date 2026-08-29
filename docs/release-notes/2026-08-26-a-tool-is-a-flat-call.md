# 2026-08-26 — A Tool is a flat call over an explicit reference

For a project author holding a directory that runs on
`2026-08-25-one-tool-vocabulary`. This note says what **broke** and what each
removed member *was*. The reasoning lives in
[`docs/decisions/2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md`](../decisions/2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md);
the published surface is [`docs/PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).

> **Later upgrade:** [Project handlers may call Tools](2026-08-29-project-handler-tool-calls.md)
> supersedes this release's leaf-handler restriction. The removed `body`,
> `child_effects` and `workflow_limits` members stay removed.

The release is `2026-08-26-a-tool-is-a-flat-call`. The name is a date and what
the build did, not a milestone tier: this project declares no milestone until
the first game is stably automated end to end.

## What this release is

The previous release made everything a Tool. This one makes every Tool a **flat
call over an explicit reference**: a name, one flat argument object, one visible
result. No Tool takes a body, no Tool runs caller code, and nothing reaches a
call through an ambient scope. Where a call needs a screenshot, it names that
screenshot's digest as an argument.

A screenshot became a value you can hold. `framework.screen.capture` answers
with a receipt; `framework.screen.observe`, `read_lines`, `census_grid`, `probe`
and `crop` each take a `screenshot_sha256` and so each says which frame it
measured. The result of one call is the argument of the next, which is what the
old `explore.cycle` callback was simulating by keeping a borrowed view alive.

## The loop is unchanged

```text
project upgrade            # new binaries in place, plus the whole work list
<edit umbraflow-project.json, your Luau, and your RuntimeModel>
project check              # what is still wrong, offline, as often as you like
project build              # materialise, then check again
umbra-flow open --project .
```

**Your Operator root keeps working. Open it and it migrates itself.** Two
identities moved under it, and this release carries a migration for each.

The ledger schema identity moved, and an existing root moves with it on open: it
drops the nested-Tool-call schema, adds the `genesis_transitions` audit table,
records the transition in `schema_identity_transitions`, and verifies the
resulting schema against the new identity, all in one transaction. Rows written
before this release stay readable; they simply do not compare equal to rows
written after it, because a recorded call identity now covers a different
catalog.

The **genesis RuntimeArtifact** digest moved too. A root records its origin as
generation 0 of `runtime_installations`, pinned to the digest of the framework's
own three-line empty model, whose first line is its `schema_version`. This
release cuts that format from 3 to 4, so those bytes changed and so did their
digest:

| | |
| --- | --- |
| genesis RuntimeArtifact root | `4b1174c77b10e313df176819057d7bf0e65c4332cd57697dac0f9bad77ff22a9` → `bf3c0e8ffbf87253b3f46ee14fb62cd0d135c4beef5857ad694d856f672e7866` |

Generation 0 is **layout, not history**: it is the framework's own constant,
materialised into a root the same way the staging directory and the empty
database are, and never the output of a run. So when the constant moves, the
root's copy of it is migrated to follow, in one transaction — every
`runtime_installations` generation holding the old digest, every `sessions` row
pinned to one of them, and the active pin in `runtime_state`. The superseded
artifact stops being referenced and the ordinary reclamation sweep collects it.
The transition is recorded in `genesis_transitions`, on the same terms
`schema_identity_transitions` records a schema move. The pure event logs —
`runtime_upgrade_failures` and `release_capability_approvals` — keep their bytes
exactly: they record what happened rather than what will be loaded.

Only a digest this framework actually published is recognised. The superseded
ones are registered in `k_formerGenesisArtifactRootHashes`
(`modules/task/source/task/runtime-model-file.hpp`), and anything else at
generation 0 is still refused by name, with the database left intact:

```text
Operator root pins generation 0 to RuntimeArtifact root sha256:…, which is
neither the genesis RuntimeArtifact sha256:bf3c0e8f… nor a genesis this
framework superseded
```

**If you are performing a format cut yourself**, that registry is what your
change owes: append the pre-change `genesisArtifactRootHash()` in the same
change, with a fixture that reproduces a root pinning it.

## A Project Tool declaration loses three members and gains one

`umbraflow-project.json`, every entry of every deployment's `tools`:

```json
{
  "name": "chaos.dream.get_current_event",
  "description": "Reports which encounter the observation shows.",
  "version": "1.0.0",
  "mutability": "read_only",
  ...
}
```

`body`, `child_effects` and `workflow_limits` are **deleted rather than
defaulted**, and a document still carrying any of them is refused as an unknown
member. `Tool` closes over exactly eleven required members plus `$comment`.

- `body` was pinned to `false` and said a Project Tool takes no caller-supplied
  body. There are no bodies, so there is nothing to pin.
- `child_effects` declared which Tools a handler could issue while it ran. A
  handler is now a **leaf transform** over explicit arguments and receives no
  Tool invocation capability at all, so the ceiling has nothing to bound. The
  whole `ChildEffects` definition is gone from the schema.
- `workflow_limits` bounded a handler's steps, dispatches, observations and
  waits. A leaf transform takes none of those; the `WorkflowLimits` definition
  is gone from `umbraflow-operator-v1.schema.json` as well.

**`description` is new and required** — a string of 1 to 1024 characters. A Tool
publishes a name, a description and one flat argument schema, and the
description is what a caller reads to decide whether to call it. A declaration
without one is refused as a missing member, so this is the break that hits every
project whether or not it used bodies.

**`argument_schema` no longer accepts the string `"unchecked"`.** It was
`oneOf: [{"const": "unchecked"}, {"type": "object"}]` and is now `{"type":
"object"}`: every Tool states a real schema and the framework enforces it. An
opting-out spelling is exactly the second reading this repository does not keep.

If a handler of yours called another Tool to do its work, that composition moves
to the caller. This is a real rewrite and it is the point of the release: a Tool
whose handler can call Tools has a result that does not say what it did. Where a
handler observed for itself, the caller now captures, observes, and passes the
resolved observation in as an argument.

## Every verb, was and is

| Was (`2026-08-25-one-tool-vocabulary`) | Is now |
| --- | --- |
| `screen.observe(fn)` | `screen.capture()` then `screen.observe(screenshot_sha256)` |
| `framework.screen.read_lines` inside the body | `{ screenshot_sha256, x, y, width, height }`, its own root call |
| `framework.screen.census_grid` inside the body | `{ screenshot_sha256, ... }`, its own root call |
| `framework.screen.probe` inside the body | `{ screenshot_sha256, ... }`, its own root call |
| `framework.project.write` `capture` arm | `screen.crop(...)` then `framework.project.write_file` |
| `framework.input.deliver` `{ action = "click", ... }` | `framework.input.click` |
| `{ action = "drag", ... }` | `framework.input.drag` |
| `{ action = "hold", ... }` plus a body | `framework.input.hold` with `duration_ms` |
| `{ action = "key", ... }` | `framework.input.key` |
| `{ action = "move", ... }` | `framework.input.move` |
| `{ action = "scroll", ... }` | `framework.input.scroll` |
| `framework.project.read` | `framework.project.read_text` |
| `framework.project.write` `text` arm | `framework.project.write_text` |

The `action` discriminator and both arm tables are deleted. A document or a
chunk still naming `framework.input.deliver`, `framework.project.read` or
`framework.project.write` is refused as an unknown Tool.

## The structural change, and it is the one that will break your chunks

Under `screen.observe(fn)` the measuring verbs lived **inside** a callback, on a
borrowed view, and the frame they measured was whichever one the scope happened
to hold. There is no callback and no implicit scope now:

```lua
local captured = screen.capture()
local screenshot = rawget(rawget(captured, "result"), "screenshot_sha256")

local observed = screen.observe(screenshot)
local lines = tools.call("framework.screen.read_lines", {
    screenshot_sha256 = screenshot,
    x = 0, y = 640, width = 1600, height = 60,
})
tools.call("framework.input.key", {
    screenshot_sha256 = screenshot,
    key = "1",
})
```

Every call has its own visible result and its own visible failure. A failure is
the result, not an unwind: a call that refuses answers with the refusal, and the
chunk decides what to do about it. Composition belongs to the caller.

## Input is twelve flat Tools on two surfaces

Raw input is six Tools under `framework.input.*` — `click`, `drag`, `hold`,
`key`, `move`, `scroll` — each naming a point or a key directly. Semantic input
is the same six verbs under `framework.ui.*`, taking an observation reference
and a resolved target instead of coordinates.

**`hold` requires `duration_ms` again.** The previous release made the body's
extent the duration; with bodies gone the duration is a number. Its optional
`return_screen` is `"none"`, `"capture"` or `"observe"` — the capture happens
while the input is engaged, and release runs on every exit path including
refusal and timeout. Arbitrary work while an input is held is gone, and
deliberately so: it was the last place a caller could run code inside a Tool
call.

A *Binding*'s `hold` action still carries `hold_ms`, unchanged. That is
project-owned measured behaviour of one declared control, and a different thing
from the Tool argument.

## Screenshots are durable artifact values

`framework.screen.capture` and `framework.screen.crop` publish PNG bytes to the
**Operator evidence store** — content-addressed, under `evidence/` in the
Operator root — and answer with a receipt. The durable call row carries the
digest, the byte count and the provenance; it never carries pixels. The blob is
made durable before the receipt is committed, so a hash in the ledger always
points at bytes that exist.

Keeping a rectangle of the screen is now two calls, and that is the point: a
failure between measuring and writing stays visible instead of being swallowed
inside one compound verb.

```lua
local cropped = screen.crop(screenshot, 351, 640, 376, 32)
tools.call("framework.project.write_file", {
    path = "assets/templates/card.png",
    file_sha256 = rawget(rawget(cropped, "result"), "screenshot_sha256"),
})
```

## The Project store is three Tools, on two surfaces

`framework.project.read_text`, `framework.project.write_text` and
`framework.project.write_file` are the whole Project-store surface. Both writes
declare the same authoring effect — a change to the project's own unsealed
generation's authoring store — but they sit on **different Tool surfaces**:
`write_text` is Semantic and `write_file` is Privileged. A policy that admits
one does not thereby admit the other, so grant the exact names you use.

## The clock is a Tool, and it is the only one

`framework.workflow.now` is new. It takes no arguments and answers
`read_at_unix_ms`: the instant the Operator's wall clock was read, in Unix
milliseconds since 1970-01-01T00:00:00Z, as a decimal string — the same clock,
epoch and rendering as a `framework.screen.capture` receipt's
`created_at_unix_ms`. It is Semantic and read-only, so an absent policy artifact
still admits it.

It exists because a chunk had no way to ask the time. `os.time`, `os.clock` and
`os.date` are nilled in the sandbox and stay nilled, because a pure-data VM must
carry no nondeterministic source; the only reachable instant was
`created_at_unix_ms` inside a capture receipt, so learning the time meant taking
a screenshot. A Tool call is what makes a wall-clock reading safe here: the
instant enters the call's durable outcome, so **a replay of that call position
answers the recorded instant rather than a fresh one**. A clock reached through
a VM global would tick again on every restart and leave no record of what it
first said.

It costs a Tool call like everything else, and how often you want one is yours.
A project that logs a timestamp per line pays a call per line; one that stamps
per turn pays a call per turn. The framework states the reading and never how
often to want it.

## What a session may do is still its Operator's policy

Unchanged in shape, changed in spelling: an artifact that granted
`framework.input.deliver` and `framework.project.write` now grants nothing,
because neither name exists any more. Grant the flat names your chunks call.

An absent artifact is still deny-all, but be precise about what that leaves you:
`framework.screen.capture` and `framework.screen.observe` are Semantic and still
run, while `read_lines`, `census_grid`, `probe` and `crop` are **Privileged** and
do not. Under deny-all you can take a frame and resolve a Surface on it; you
cannot measure anything inside it.

One widening: `privileged_surface_tools` used to judge only a call **at the top
of a run**, because a delegated child's surface was judged against its parent's
`maximum_child_surface` instead. There are no delegated children, so the list
now judges every call of a privileged-surfaced Tool. If you use
`framework.project.write_file`, it must appear there.

## Identities that moved

Every one of these enters a recorded call identity, so a durable row from an
older release does not compare equal to a new one.

| Identity | Before | After |
| --- | --- | --- |
| Operator ledger schema | `045925eefabef97b964f6a21db0da81cdc6a2c293c21e7f495011fe3d1b9277f` | `181f202e9d7516dff603a006dfabf4fef372a0413710e2bb23ad9cb75dc1bc12` |
| Operator protocol schema | `cb21ba6a001fa0a955584269b1f5850dd19bbf3a7e7dc774483563a07baf572e` | `137f1e7101310736172425e8282e2df479759cdf04d3a2962eb9e74568547b07` |
| Framework Tool Catalog | `a6fd2afbf07ceeb53bdc8dbaf4073a94f25a4f91f6e8619b3c1da59225397c8b` | `2a3629d6534c4795e94ec256c889f8227d56a725846899e608acb5562ee34aac` |
| Tool Runtime Protocol | `c51ec2229b00c3c62e56fb14993ca3b820b6c77a905433d3e38904377212c25c` | `d943bd81b033b7cfe5cd2fab59e8f3ccaacd6b6f2fdd513f75cf06fd1650fd01` |
| Scoped Tool Environment | `e15fe8ed81a8edf041ccd45e14f953670f8337d420ea2f7e6fd9f99ccaecfc77` | `f2acab246b56b6348344048cbd440ce4598eab92fcc47ff1ea06ca6961a5058a` |

The four `@umbraflow/*` module source rows this table used to carry are gone
along with the modules. `screen`, `tools`, `workflow` and `audit` were
hand-written Luau; the face is generated from the catalog now, so there is no
per-module source to digest and nothing for a consumer to compare. What replaces
them as the thing to watch is the catalog hash above: it moves when any Tool's
name, description or schema moves, and the module face moves with it.

## The work list, shortest first

1. Delete `body`, `child_effects` and `workflow_limits` from every Tool entry in
   `umbraflow-project.json`, and give every Tool a `description`. If any Tool
   declared `"argument_schema": "unchecked"`, write the real schema.
2. Rename `framework.project.read` and `framework.project.write` to `read_text`
   and `write_text` in your chunks and in your Operator policy artifact, and
   move any `capture` arm to `screen.crop` plus `write_file`.
3. Replace every `framework.input.deliver` call with the flat verb its `action`
   named, and give every `hold` a `duration_ms`.
4. Replace `screen.observe(fn)` with `screen.capture()` plus
   `screen.observe(digest)`, and lift every measuring call out of the callback
   into its own root call naming that digest.
5. If any Project Tool handler called another Tool, move that composition into
   the chunk that calls the handler.

`project check` finds items 1 through 4 offline. Item 5 shows up at compile: a
handler that calls `tools.call` no longer has one to call.
