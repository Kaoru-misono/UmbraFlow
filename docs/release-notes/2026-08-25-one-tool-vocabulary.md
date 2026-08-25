# 2026-08-25 — One Tool vocabulary

For a project author holding a directory that runs on `m1-project-v3`. This note
says what **broke** and what each removed member *was*. The reasoning lives in
`docs/decisions/`, dated by ruling; the published surface is
[`docs/PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).

The release is `m2-one-tool-vocabulary`. Thirty-one commits, twelve of them
breaking.

## What this release is

There is no annotation phase and no runtime phase. Everything is a Tool call,
and the only difference between a session that annotates and a session that runs
the product is which Tools its Operator policy admits. Everything below follows
from that one sentence: the private authoring natives are gone, the exploration
Luau module is gone, the generation kind that separated the two is gone, and the
surface that used to be reachable only while annotating is now the same six-arm
input Tool a Project handler calls.

## The loop is unchanged

```text
project upgrade            # new binaries in place, plus the whole work list
<edit umbraflow-project.json, your Luau, and your RuntimeModel>
project check              # what is still wrong, offline, as often as you like
project build              # materialise, then check again
umbra-flow open --project .
```

One warning about that loop, measured on the only existing consumer while
writing this note: **`project check` does not parse your RuntimeModel.** Two of
the breaks below are in `runtime-model.toml`, and `check`, `build` and
`umbra-flow open` all pass with a model the trusted resolver will refuse the
first time a collection resolves. Read the RuntimeModel section and edit the
file; nothing offline will tell you to.

## Every Tool declaration gains a required `body`

`umbraflow-project.json`, every entry of every deployment's `tools`:

```json
{
  "name": "chaos.dream.get_current_event",
  "version": "1.0.0",
  "body": false,
  "mutability": "read_only",
  ...
}
```

`body` is **required and pinned to `false`**. A Project Tool is an entry point
that takes its arguments and nothing else; it cannot receive a caller-supplied
body. Framework built-ins declare their own body shape separately, per Tool or
per tagged arm, and that declaration is part of the Framework Tool Catalog
rather than of any project document.

The member is required rather than defaulted for the usual reason: an absent
`body` would have to mean `false`, which is one more thing a reader has to know
is not written down.

## A RuntimeModel collection's `slots` gains `extent` and `maximum_slots`

Both are **required**, and a `slots` object without them is refused when the
model is parsed.

```toml
slots = { origin = 629, pitch = 418, extent = 836, tolerance = 6, maximum_slots = 3 }
```

- **`extent`** is the upper bound, in pixels, on the distance between the first
  and last slot. A layout of `n` items spans `min(pitch * (n - 1), extent)`, so
  spacing is `pitch` until `extent` binds and then shrinks as the layout grows.
  It is required because a Surface is finite: every centred fan already has an
  extent, so a declaration without one was hiding a fact rather than describing
  an unbounded fan. Before this release the layout was `origin` plus `pitch`
  alone, which no measured fan of more than a few items actually obeys.
- **`maximum_slots`** is the largest number of slots the collection can hold,
  and it bounds the search for a larger layout with missing slots. It is stated
  rather than derived: `origin`, `pitch`, `extent` and `tolerance` are all
  pixels, so a slot count computed from them would be a cardinality bound the
  framework invented over a declaration that never mentioned cardinality. A
  detected item count the search cannot place inside the bound is refused by
  name, reporting the measured count and the declared maximum.

Each slot is rounded half up, so `pitch` carries no parity constraint. Whether
one cardinality's slots are far enough apart to assign items to them is a
property of that cardinality, so it is checked when the collection resolves
rather than when it is declared.

## A Binding's action vocabulary is six verbs, and `long_press` is gone

`binding_action` carried `click`, `key` and `drag`. It now carries `click`,
`key`, `drag`, `hold`, `scroll` and `move`.

**`long_press` is deleted as a name, not aliased.** A declaration using it is
refused; so is every symbol that carried it, down to the trace events.

- `hold_action` carries `hold_ms`, bounded by the delivery layer's own ceiling,
  so a press no Host can perform is refused at the artifact boundary rather than
  at delivery.
- `scroll_action` carries `notches` and no coordinate.
- `move_action` carries only a `proof_locator`.
- `action_point`'s rule widens from "all of them are `key`" to "all of them are
  `key` or `scroll`", because those two are the verbs that do not aim.

## `explore` is no longer a Luau global, a module, or a capability table

The whole `explore` surface is deleted, along with the sixteen `explore_*`
private natives underneath it. There is no compatibility spelling.

An interactive chunk is now a **root scoped Tool program**: it imports the same
release-owned scoped modules a registered Project handler imports, and calls
Tools by name. `explore` survives as the name of the CLI and its queue protocol
and nothing else.

```lua
local screen = require("@umbraflow/screen")
local tools  = require("@umbraflow/tools")
```

The four reserved scoped modules are `@umbraflow/audit`, `@umbraflow/screen`,
`@umbraflow/tools` and `@umbraflow/workflow`. A verb without a facade is written
as a `tools.call` naming the Tool.

### What each removed member was, and what replaces it

| Was (`m1-project-v3`) | Is now |
| --- | --- |
| `explore.cycle(fn)` | `screen.observe(fn)` |
| `view:read_lines(...)` | `tools.call("framework.screen.read_lines", {...})` **inside** the observe body |
| `view:census_grid(...)` | `tools.call("framework.screen.census_grid", {...})` inside the body |
| `explore.probe(...)` | `tools.call("framework.screen.probe", {...})` inside the body |
| `view:crop(...)` | **deleted; nothing returns pixels** — see below |
| `view:click_point(x, y)` | `tools.call("framework.input.deliver", { action = "click", x = x, y = y })` |
| `view:long_press(x, y, ms)` | `tools.call("framework.input.deliver", { action = "hold", x = x, y = y }, fn)` |
| `view:drag(...)` | `{ action = "drag", x, y, to_x, to_y, travel_ms }` |
| `view:move_pointer(x, y)` | `{ action = "move", x, y }` |
| `view:scroll(n)` | `{ action = "scroll", notches = n }` |
| `view:key(name)` | `{ action = "key", key = name }` |
| `explore.read(path)` | `tools.call("framework.project.read", { path = path })` |
| `explore.write(path, text)` | `tools.call("framework.project.write", { action = "text", path = path, content = text })` |
| `explore.settle(ms)` | `tools.call("framework.workflow.wait", { duration_ms = ms })` |
| `explore.terminal()` | **deleted** — whether the generation was spent is the session's outcome, not a verb |

### The structural change, and it is the one that will break your chunks

Under `explore.cycle` the acting verbs lived **inside** the callback, on the
cycle view. They do not any more.

**An observation's body holds read-only measurements only.** Admission matches a
child's declared effects against its parent's declaration, and an observation
declares none, so no mutating Tool can be a child of one. An input or a project
write is issued as an **independent root Tool call, outside any body**:

```lua
-- measure inside the body
local answer = screen.observe(function()
    tools.call("framework.screen.read_lines", { x = 0, y = 640, width = 1600, height = 60 })
end)

-- act outside it
tools.call("framework.input.deliver", { action = "key", key = "1" })
```

The one exception is the direction that is actually safe: a **read-only child
under a mutating parent**. That is what the `hold` arm's body is.

## Input is one Tool over a closed six-verb contract

`framework.input.deliver` takes exactly one of six tagged arms — `click`,
`drag`, `hold`, `key`, `move`, `scroll` — and only `hold` accepts a body.

**The `hold` arm has no duration.** It carries `action`, `x` and `y`, and the
body's extent *is* how long the press lasts: engage, run the body, release on
whichever way the body ended. Observe some frames inside it and **the frames are
the clock**, which is strictly better than the deleted native's blind wait — the
framework records what was on screen while the input was held, so the run is
verifiable, whereas an 800 ms hold records nothing.

An **empty** `hold` body is refused by name:

```text
a hold body is empty; press and release is the `click` arm.
```

Note that a *Binding*'s `hold` action is a different thing and does still carry
`hold_ms`: that is project-owned measured behaviour of one declared control,
while the Tool arm is a caller-driven press whose body says when it ends.

## There is deliberately no crop, and no Tool answers with pixels

`view:crop` is gone and nothing replaces it. A Tool result is canonical JSON
inside a durable ledger row, so a cropping Tool would put pixels inside a hashed
record.

Keeping a rectangle of the screen is an **authoring write** and is spelled as
one, on `framework.project.write`'s `capture` arm:

```lua
tools.call("framework.project.write", {
    action = "capture", path = "assets/templates/card.png",
    x = 351, y = 640, width = 376, height = 32,
})
```

The host encodes the PNG and the call answers with the file's content hash,
never its pixels.

## What a session may do is its Operator's policy, not its kind

`GenerationKind` is deleted. There is one session type, one manifest, one
admission authority and one dispatch path. An ordinary session can annotate.

The capability difference now lives where an operator can read it: the
**PolicyArtifact at `<runtime>/policy-artifact.json`**, whose shape is
[`schema/umbraflow-policy-v1.schema.json`](../../schema/umbraflow-policy-v1.schema.json).
It lists every Tool permitted on the Privileged surface, and admission refuses a
top-level call of a privileged-surfaced Tool it does not name, whichever actor
presents it.

**An absent artifact is deny-all**, and that is a working configuration rather
than a broken one: read-only screen observation still runs, and an input
injection is refused naming the grant the artifact does not make. A project that
wants a session to deliver input or write its own authoring directory needs an
artifact granting `framework.input.deliver` and `framework.project.write`.

A project declaring `surface` about its own Tools grants nothing.

## Identities that moved

Every one of these enters a recorded call identity, so a durable row from an
older release does not compare equal to a new one.

| Identity | Value |
| --- | --- |
| Framework Tool Catalog | `a6fd2afbf07ceeb53bdc8dbaf4073a94f25a4f91f6e8619b3c1da59225397c8b` |
| Scoped Tool Environment | `e15fe8ed81a8edf041ccd45e14f953670f8337d420ea2f7e6fd9f99ccaecfc77` |
| `@umbraflow/screen` source | `cf8f228a71108300ac4f2421c9cbaf631e931e6af6a8daba902c8aa47c7fd718` |
| `@umbraflow/tools` source | `c64a623869a21f16f50a35505b52ec66570f08c89b5d88dd5947c6a969869ecd` |

## `timeout_policy` is enforced, and `reconcile` is deleted from it

`timeout_policy` was published and unenforced: a Project stated a ceiling and an
action, the framework carried both in the descriptor, and neither did anything.
It is enforced now, at the one seam every caller adapter passes through. An
overrunning call gets a durable outcome that **is** the timeout whatever the
provider went on to answer, reported specifically — which `maximum_elapsed_ms`
was exceeded and what the call actually took — rather than as a generic failure.

Enforcement is deliberately post-hoc. A provider runs synchronously on the
calling thread, so the ceiling decides the call's outcome and whether the run
continues, not whether the provider is interrupted.

**`on_timeout` no longer accepts `reconcile`.** A declaration using it is
refused: the value is gone with its wire name, its parse entry, the deployment
reader's array, both schema enums and the two Framework descriptors that carried
it. It goes because no framework behaviour could tell it from the other two —
reconciliation is a transition only an already-`possible` call may take and
needs a trusted external query that has no production producer, while a
timed-out mutating leaf is classified `possible` whatever `on_timeout` says. A
Project choosing `reconcile` was silently given something else, which is worse
than an absent value. The two surviving values do what they say: `stop` ends the
run, and `reobserve` keeps it alive with the timeout as the recorded answer so
the Project looks again itself. **No observation is minted on your behalf** —
that would perform your next action for you and would make the shape of the
recorded call tree depend on a timeout rather than on what you asked for.

Enforcing the ceiling also exposed a collision: `framework.workflow.wait`'s own
timeout ceiling equalled the longest wait it accepts, while discovery published
that same number as the `maximum_duration_ms` bounding `wait()`. The Tool would
have failed at exactly the value it advertised as allowed. The two numbers are
separated and the Tool's ceiling gains headroom.

Alongside it, one new invariant that is flow safety rather than a Project's
decision: **no input state survives the end of its Tool call.** On timeout, on
error, on abort, every held input is released unconditionally. A hung call still
holding a mouse button down is a worse failure than a hung call. The `hold` arm
above rests entirely on this.

## A Replay Bundle is the record it actually is

The bundle still required `baseline_event_id`, `journal_prefix` and
`operation_rows`. The Journal was deleted in the previous release and
`operation_rows` was already naming nothing, so every bundle signed since then
carried members referencing entities that do not exist. All three are gone. A
reader written against the old required set is refused; a replay needs the
session's own ordered observations and the identity pins to reproduce them, and
has both already.

Two published schemas also stopped requiring a revision member that no longer
exists, so a document written against the old requirement is refused.

## What this release still does not give a project author

`umbra-flow explore`, `observe` and `invoke` all require `--runtime DIR`, an
Operator production root holding an installed generation, and refuse a root that
holds none rather than bootstrapping one. The verb that installs one is
`umbra-flow upgrade --handoff DIR`, and the handoff it requires — a directory
holding exactly `release.manifest.json` and `runtime-artifact/` — is **described
nowhere in `docs/PUBLIC-CONTRACT.md` and produced by no shipped verb.** A
project author who has never had an Operator root therefore cannot make one
without reading this repository's own tests.

That is a gap in this release, stated rather than omitted. It is not new here,
and nothing in this release made it worse; it is written down because it is the
one thing standing between a correctly adapted project and a session that runs.
