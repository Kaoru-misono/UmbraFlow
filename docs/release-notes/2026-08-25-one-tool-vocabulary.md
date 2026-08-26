# 2026-08-25 — One Tool vocabulary

For a project author holding a directory that runs on `m1-project-v3`. This note
says what **broke** and what each removed member *was*. The reasoning lives in
`docs/decisions/`, dated by ruling; the published surface is
[`docs/PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).

The release is `2026-08-25-one-tool-vocabulary`. Thirty-nine commits, twenty of
them breaking. The name is a date and what the build did, not a milestone tier:
this project declares no milestone until the first game is stably automated end
to end.

> **Superseded in part.** This note describes the `2026-08-25-one-tool-vocabulary`
> boundary as it was published. The 2026-08-26 flat-call ruling then deleted Tool
> bodies, the `framework.input.deliver` arm table and the `framework.project.write`
> capture branch, and made every screen Tool name an explicit screenshot digest.
> Read [`2026-08-26-a-tool-is-a-flat-call.md`](2026-08-26-a-tool-is-a-flat-call.md)
> after this one; where the two disagree, the later note holds.

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

That loop is now worth more than it was, because **`check` parses your
RuntimeModel**. Until this release nothing offline did: the parse ran only in
the trusted VM a live session boots, so a model the parser refuses passed
`check`, `build` and `open` alike and failed only once you had a window handle,
an Operator root and an elevated target. Two of the breaks below are in
`runtime-model.toml`, and before this release neither was visible until then.
The last section of this note says what changed and what it will find in your
project.

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

## The install door takes the artifact itself

`umbra-flow explore`, `observe` and `invoke` all require `--runtime DIR`, an
Operator production root holding an installed generation, and refuse a root that
holds none rather than bootstrapping one. The verb that installs one is
`umbra-flow upgrade`, and until this release it demanded a *release handoff*: a
directory holding exactly `release.manifest.json` and `runtime-artifact/`, where
that manifest was a document from the retired annotation publication pipeline —
`candidate_id`, `candidate_revision`, `replay_gate_hash`,
`predecessor_publication_id`, an annotation workspace format and a workspace
SQLite revision. Eight members were parsed byte for byte and **one** was used:
the artifact root hash. The other seven were shape-checked and thrown away.

That document is the annotation phase surviving as a required file. Production
could not install an artifact without a receipt from a pipeline no shipped verb
runs, and the framework's own fixtures forged one to get past the door.

**The handoff and `release.manifest.json` are deleted.** `umbra-flow upgrade`
now takes the RuntimeArtifact directory itself:

```
umbra-flow upgrade --project DIR --runtime DIR --artifact DIR \
                   --artifact-root-hash sha256:... [--capability NAME]...
```

`--artifact-root-hash` was already required, so nothing new is asked of the
caller — the manifest was a courier for a value the operator states anyway.
There is one read now: the install holds the directory's own
`runtime-artifact.manifest.json` against that hash through
`task::loadRuntimeArtifact`, and the session the upgrade pins binds its
`SessionManifest` to the same hash. The ledger proves the two uses agree by
refusing to pin a session whose manifest names a root that was not installed.

`--handoff` and `--release-manifest-hash` are gone. Both were required, so a
caller written against them is refused by name rather than silently ignored.

What a project supplies is the directory it already declares as
`runtime_artifact` in `umbraflow-project.json` — `runtime/artifact` in both
shipped exemplars — and the sha256 of the `runtime-artifact.manifest.json`
inside it. The `--artifact` directory must be disjoint from `--runtime`: the
production root is content-addressed storage the installer owns, and a source
nested in it would make the copy read and write one tree.

Still not closed: `umbra-flow upgrade` remains absent from
`docs/PUBLIC-CONTRACT.md`, so its usage text is read from `--help` rather than
from the outward document. That is now a documentation gap rather than a
missing producer.

## Generation 0 is the genesis generation, not "nothing is installed"

`H_genesis` is the content hash of the empty RuntimeModel — `base_resolution
= [1, 1]`, no Bindings, no Locators, no Readers. The framework has always called
it "sealed by construction: a constant of the framework, not the output of a
run", and `project init` has always scaffolded it into a project directory. **No
Operator code had ever heard of it.** A freshly created root pinned no
generation, so a first session had nothing to bind, so `init` then `explore
--runtime DIR` — which a ruling of 2026-08-24 states as the path from nothing to
a first session — could not work for a project with no artifact of its own.

Every Operator root now materialises genesis as part of its layout, beside the
directories, the staging root and the database that `OperatorCoordinator::open`
already creates. **Generation 0 stops meaning "nothing is installed" and starts
meaning the genesis generation**: it names a real artifact root, so a first
session binds `H_genesis` at generation 0. A root whose generation 0 pins any
other artifact root is refused by name.

What does not change: a first real installation still lands at **generation 1**
and still compares against the same number it always did, and the
capability-expansion rule is untouched — a first install carrying
`--capability` is not refused as an expansion.

**A root written before this release gains genesis the next time anything opens
it**, through a ledger migration rather than a branch, without disturbing what
it already holds. Measured on the only existing root: it gained genesis, its
installed generation 1 was untouched, and `reclaim` counted zero artifact
directories to sweep — genesis is layout, so nothing sweeps it.

Nothing is asked of a project author here. Genesis grants nothing: an empty
model can do nothing under the deny-all an absent policy artifact means, which
is why pinning it decides nothing on anyone's behalf.

## `project check` and `project build` parse your RuntimeModel

The warning at the top of this note described this release's own defect, and it
is fixed in the same release. Until now the RuntimeModel was parsed in exactly
one place — the trusted VM a live session boots — so `project check`, `project
build` and `umbra-flow open` all exited 0 against a model the parser refuses,
and the refusal arrived only once you had a window handle, an Operator root and
an elevated target.

`check` and `build` now run **that same parser**, not a second one. There is no
C++ TOML reader and no schema over the model: the kit executes the one
interpreter and relays its refusal verbatim, which is falsified by changing the
wording inside `model.luau` and watching what `project check` prints change with
it. `freeze` inherits the check.

`umbra-flow open` runs it too, and its report gains a line:

```text
model semantics 9eb7dcd7…  (parsed by this binary)
```

The old `model format 3 (accepted by this binary)` line remains and still means
what it always meant — the *manifest's* declared format number. It is the new
line that says the model itself was read.

**What this will do to you.** Two things, and both are the defect surfacing
rather than a new rule:

- A model that was already invalid now fails at `project check` instead of at
  your first session. The only existing consumer had two such defects; one had
  gone unnoticed for days because nothing offline read the file.
- `runtime_artifact` is **required** by the project schema, so the parse is
  unconditional and a declared-but-absent RuntimeArtifact is refused rather than
  ignored. If your project declares one it never wrote, it will say so now.

The refusal names the member, in the parser's own words:

```text
script error: [string "model"]:56:
RuntimeModel.collections[1].placement.slots.maximum_slots must be a finite number
```

## Two smaller repairs

- **The install no longer fails on a long path it could live at.** A staged
  file sat under `runtime-artifacts\.staging\<64 hex>\` while its final home was
  `runtime-artifacts\<64 hex>\`, so staging was nine characters longer than the
  destination and a 144-character Operator root failed partway through an
  install. The staging leaf is 54 characters now, one shorter than the
  destination, so the destination is always the binding constraint: whatever
  root the artifact can live in, it can be installed into.
- **A confined-file failure says which one, where and why.** All twenty-eight
  reported one line number and none carried the operating system's error. They
  now carry the platform's own sentence and code, the real call site, and — for
  a path — the path and its character count. Windows reports "cannot find the
  path specified" for a path over `MAX_PATH`, so the count is what tells the
  truth: `(273 characters)` against a 260 limit.
