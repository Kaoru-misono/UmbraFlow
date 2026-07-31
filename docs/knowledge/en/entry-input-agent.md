# entry/input-agent: the annotation front-end

`entry/input-agent` is the `umbra-input-agent` program. It serves one authoring session's command
queue against one raw window: read a line, deliver the input it names, capture the frames the
decision was made from, and answer on a results file. It is the third `trace::FrontEnd` — neither
the task a trusted Luau framework runs nor the operator driving a loaded project — and it says so on
every line it writes.

It left `entry/m0-demo` on 2026-07-31. Until then it was the `input-agent` subcommand of the frozen
M0 demo, and the directory name was the only thing about it that was still a demo: in one day it had
gained a durable queue cursor, an extracted serving loop with mutation-proven tests, a drive layer
under an annotation layer, and the front-end stamp. The old spelling `m0-demo input-agent` now
prints where the program went and exits with failure.

The directory also owns what the two programs still share — the frame PNG writer, the path
confinement rules, target selection and capture-session setup, the JSON string escape, the error
text, and the command-line primitives. The direction is deliberate: `m0_demo_support` links
`input_agent_support`, never the reverse, so retiring the frozen demo is a delete rather than
another extraction.

## What It Is For

An annotation session measures a bare window. It has no project, no runtime manifest, no
`ResolvedPage` and no annotated element, because measuring the screen is what *produces* the
annotations. The agent therefore authorizes nothing beyond the target and the observation: it is the
same delivery path the product uses, exposed one command at a time.

The unelevated side of a session — `umbra-authoring`, a script, or a person — appends JSON lines to
a queue file and reads the results file. The agent is launched once, elevated if the target's
integrity requires it, and stays resident. That keeps the UAC boundary at one long-lived process
instead of one prompt per click.

```text
umbra-input-agent --hwnd 0xHWND --queue q.jsonl --results r.jsonl --output-dir DIR
```

Two operational rules follow from the confinement checks below: the queue and results files must be
distinct and must live **outside** `--output-dir`, and the agent is long-running, so it is launched
detached.

## Layering

One serving loop over two ports, each concern testable without the ones below it.

- `loop.{hpp,cpp}` — `runInputAgentQueueLoop`, the serving loop and the only place the ways a run
  ends are decided, over the `IInputAgentSession` port. `InputAgentResultWriter` lives here too, and
  it is where the front-end stamp goes on.
- `annotation.{hpp,cpp}` — `AnnotationSession`, the annotation layer: output-path confinement, the
  before/after framing, PNG encoding, the rect fence, and the results-line shape. This is where a
  verb an authoring session grows belongs, and `read` is the first one that did.
- `drive.{hpp,cpp}` — `IInputAgentDrive` and `WindowInputAgentDrive`: delivering one input to a
  window against one observation and getting a frame back. It names no file, encodes no image and
  parses no command, so nothing an authoring session invents reaches it.
- `text-reader.{hpp,cpp}` — `IInputAgentTextReader`, the second port the annotation layer stands on:
  a rectangle of one observation in, `ocr::TextLine`s out. It is a sibling of the drive rather than
  a verb on it, because a drive knows only a window, an input and a frame, while what a rectangle of
  that frame *says* is what an author is measuring.
- `ocr-text-reader.{hpp,cpp}` — `OcrTextReader`, the live half: PP-OCRv6_small over ONNX Runtime.

`IInputAgentDrive` is deliberately not `engine::IActionSink`, for the same reason
`IInputAgentSession` is not: that port speaks the engine's vocabulary of one already-authorized
action against an *annotated element*, and lives in a module this executable does not link.

`agent.{hpp,cpp}` is the composition root — `runInputAgent` plus the `InputAgentQueueReader` that
feeds it — and `main.cpp` is the process boundary above it.

**Every results line is stamped with the front-end that produced it**: it opens with
`"front_end":"annotation"`, from `trace::FrontEnd::Annotation` under `trace::frontEndWireName`'s
spelling. `input_agent_support` links `modules/trace` for that one type. The agent writes no
`umbraflow-trace/v2` line, because every line of that schema carries a `runId` and a `generationId`
and an annotation session — which reaches no project — has neither; the results file is its whole
evidence stream, and the stamp is applied by the writer rather than by each serializer for the
reason `trace::TraceRecorder` rather than each emitter owns that stamp.

## Protocol

`entry/input-agent/protocol.hpp` defines six variants:

- `InputAgentCaptureCommand{output}`;
- `InputAgentClickCommand{x, y, outputBefore, outputAfter, settle}`;
- `InputAgentKeyCommand{key, outputBefore, outputAfter, settle}`;
- `InputAgentScrollCommand{x, y, delta, outputBefore, outputAfter, settle}`;
- `InputAgentReadCommand{rect}`;
- `InputAgentQuitCommand`.

The corresponding JSON objects accept only `op=capture|click|key|scroll|read|quit` and each one's
exact field set; the frame fields are spelled `out`, `out_before`, `out_after`, and `settle_ms`, and
the rect fields `rect_x`, `rect_y`, `rect_width`, `rect_height`.
`parseInputAgentCommand()` rejects anything over 64 KiB, invalid UTF-8, duplicate/unknown fields,
malformed JSON numbers, empty paths, NUL, non-finite coordinates, and a settle over 5000 ms. The
default action settle is 400 ms.

`delta` is a whole number of wheel notches rather than raw `WHEEL_DELTA` units, and the parser
resolves it through `WheelDelta::create`, so a zero, a fraction, and a count whose raw form would
not fit `wParam`'s signed 16-bit word are all refused before any command is queued — the same way
`key` is resolved through `KeyInput::fromName` and the rect through `PixelRect::create`.

## The `read` verb

```text
{"op":"read","rect_x":262,"rect_y":14,"rect_width":138,"rect_height":36}
{"front_end":"annotation","op":"read","ok":true,"frame_id":7,"reader_ready":true,
 "lines":[{"text":"621/922","confidence_bp":9871,
           "bounds":{"x":262,"y":14,"width":138,"height":36}}],"error":null}
```

(The results line is one physical line; it is wrapped here only to fit the page.)

The rectangle is in the **captured frame's own pixel space** — what an author measures on a capture
PNG — and not the client space `click` and `scroll` take. `capture` reports `frame_size`,
`client_size` and their `delta`, which is what tells an author whether the two differ on a target.

The verb names no output path, and that absence is the point: measuring text during an annotation
session otherwise means capturing a PNG and reading it with human eyes.

`rect_*` are whole non-negative pixels, so a fraction or a negative is refused at parse time rather
than truncated onto a grid that has no fractions. `PixelRect::create` refuses an empty or
overflowing rectangle, and `PixelRect::ensureWithinExtent` refuses one that does not fit the
observation — the second fence is the annotation layer's rather than the engine's, so a rectangle
that was never going to be readable cannot be what brings a 20 MB model up.

**One line, not a block.** The rect is asserted by the caller to hold exactly one line, which skips
detection and is both the cheap path and the one immune to a detector that splits a label in two. A
rect that in fact holds several lines reads as one run of nonsense rather than failing; that is
`modules/ocr`'s documented behaviour and nothing here can detect it. `TextLayout::Block` needs the
detection model, which the adapter does not run yet, so no field selects a layout — the day
detection lands is the day this verb grows one.

**Lines rather than one joined string**, each with the model's own confidence in basis points: an
author measuring a region needs to know both what was read and how much of it to believe, and a
concatenation throws the second away. `bounds` travels too, because a block read will one day return
boxes the caller did not supply.

### Three answers, kept apart

| condition | `ok` | `reader_ready` | `lines` |
|---|---|---|---|
| the model would not load | `false` | `false` | `null` |
| this rect or this frame was refused | `false` | `true` | `null` |
| the region was read and holds no text | `true` | `true` | `[]` |

`reader_ready` is the only fact on the line that is about the run rather than about this command:
false means no read will succeed until the payload is fixed, true means the rectangle is what to
look at. An empty `lines` array is an **answer**, not a failure — a region with no text is an
ordinary fact about the screen, and folding it into the failure side would make "the popup is not
showing" and "the model would not load" the same line.

A reader that cannot come up does **not** end the run. Every other verb is unaffected, and the queue
behind the command may hold nothing but captures and clicks.

### The engine's lifetime and its payload

`OcrTextReader` builds the ONNX Runtime session on the **first read**, never at startup. Two things
follow: a session that only captures pays nothing for a model it never uses, and an agent whose
payload is missing still starts and still serves every other verb. A creation *failure* is not
cached, so a payload restored beside a running agent is picked up by the next read; only a success
is.

The payload is resolved **relative to the executable** — `<exe dir>/models/ppocr-v6-small-rec/` —
with no flag pointing elsewhere, because the agent is launched detached and its working directory is
not something it may depend on. `entry/CMakeLists.txt` stages that directory and the ONNX Runtime
DLLs into the runtime output directory, and a release ships them the same way. The detection model
is deliberately not staged: nothing loads it.

Linking `modules/ocr` gives `umbra-input-agent` a load-time import of `onnxruntime.dll`. `m0-demo`
links the same support library but references nothing in the ocr module, so the linker drops it and
the frozen demo gained no runtime dependency.

`InputAgentQueueReader` reads the append-only queue incrementally by offset, accepts LF/CRLF, and
retains incomplete lines; it fails closed when the queue is truncated or a pending command exceeds
1 MiB. `InputAgentResultWriter` calls `flushDurably()` after each JSONL result it writes.

## The Queue Cursor

`cursor.{hpp,cpp}` records how far the queue has been consumed in a `<queue>.cursor` file beside it,
so a restarted agent resumes rather than walking a live target through every command the queue
already holds. The cursor path is derived from the canonical queue path, and it must not alias
either IPC file — which a hard link could otherwise arrange.

`--queue-start refuse|beginning|end` is read **only** when no cursor exists yet and the queue is
already non-empty. `Refuse` is the zero value, so the unstated policy is the one that asks rather
than the one that replays. A cursor always wins over the flag.

The loop advances the cursor *after* the results line is written. A hard kill in that gap replays
one command; the reverse order would silently skip an action whose delivery nobody can still
observe.

## Path Confinement

The agent's file-permission surface is part of the protocol: the queue and results must be different
and both outside the output directory; screenshot paths must be constrained inside the output
directory by a confinement check and must not alias the IPC files.
`platform::FileWriter::createExclusive()` performs a relative `NtCreateFile(FILE_CREATE)` through a
chain of already-verified, kept-open directory handles, rejecting overwrite, reparse escape,
alternate data streams, and directory-rename races.

## The observe -> act hot path

`executeClick()` runs this, and `executeScroll()` runs the same one with `scroll` in place of
`click`:

```text
reserve fresh before/after outputs
-> capture immutable before Frame
-> ObservationLease::forFrame
-> validateInputAgentPointerAction
-> ResolvedTarget::revalidate
-> requireUnchangedTarget
-> WgcCaptureSession::validateTargetInstance
-> click
-> encode/write before PNG
-> settle
-> capture/encode/write after PNG
```

The encoding and durable flush of the before PNG are deliberately moved to after the click. On real
hardware it was found that placing the 1600×900 BGRA encode, disk write, and `FlushFileBuffers`
between capture and click consumed the 750 ms lease budget and produced unexpected expiry. After the
move, the same immutable pre-click `Frame` is still retained, but forensic I/O no longer lengthens
observe -> act.

A keystroke takes no lease freshness fence: it names no position, so an older observation cannot
make it land in the wrong place; only target replacement matters, which the generation and
`requireUnchangedTarget` already cover.

A command parse error writes a failure result and then continues; a target revalidation/instance
failure writes a result and stops the agent. After each command completes, the audit is cleared so
that a resident agent's records do not grow without bound.

## Which Build

The agent must be a **release** build. Debug recognition takes roughly 1030 ms against a 750 ms
lease, and every action then fails `StaleObservation` — see
`docs/pitfalls/capture-and-target-selection.md`. `umbra-authoring` is fine in debug: it only reads
frames already captured.

## Tests

`tests/CMakeLists.txt` composes `tests/input-agent/` into `test-input-agent`, linking
`${PROJECT_NAME}_input_agent_support`.

- `test-agent.cpp` pins the strict JSON command grammar, UTF-8, the settle cap, path confinement,
  incremental line framing, queue truncation/size limit, handle-relative exclusive output,
  per-command audit clearing, client bounds, stale-generation rejection, and the cursor's
  resume/refuse behaviour.
- `test-loop.cpp` pins the ways a run ends — a stopping command, an unparseable line, the idle
  timeout, whether an answer restarts the countdown — and that every answer carries the front-end
  stamp, including the two the loop itself authors.
- `test-annotation.cpp` pins the seam the drive/annotation split bought: an unconfined output is
  refused before the drive is asked to observe, the before-frame is encoded only after delivery so
  the observe->act window holds nothing slow, a replaced window is the one failure that ends the
  run, and `delivered` follows the drive's answer rather than a flag the session sets. It also pins
  the read verb over a scripted `IInputAgentTextReader`: the three answers stay apart, a rect outside
  the observation is refused with the reader untouched, and an absent payload is reported as the
  reader rather than as the rect. One case uses the real `OcrTextReader` against the payload staged
  beside the test binary, which is the only automated proof that the shipped resolution path finds a
  model at all; what it reads is `tests/ocr/test-ocr-real.cpp`'s subject, not this file's.
- `test-args.cpp` pins the required file-IPC paths, the defaults, the uncursored-queue policy, and
  the rejection boundaries.
- `test-target-setup.cpp` pins target revalidation and the empty-client-area refusal.
- `test-error-text.cpp` pins the error kind, context frames, and native origin in the one line a
  failure leaves the process as.

These are behavioural and boundary tests. What they cannot cover — that the delivery actually
reaches a high-integrity target, the before/after image change, the lease under hardware latency —
comes from on-hardware sessions.

## Relationship to Product Code

Inbound: an existing append-only queue, a results path, and a confined output directory.

Outbound toward `controller`: discovery (`enumerateCandidates`, `resolveTarget`,
`ResolvedTarget::revalidate`), DPI (`ensurePerMonitorAwareV2`), capture (`WgcCaptureSession`,
`Frame`), and input (`DeliveryTarget`, `ObservationLease`, `click`, `scroll`, `keyPress`,
`releaseHeld`, `AuditLog`). Toward `image`: PNG encoding of a captured frame. Toward `trace`: the
front-end value and its wire name, and nothing else. Toward `ocr`: the `IOcrEngine` port, its
`TextLine` vocabulary, and `createOnnxEngine` — this program is that module's composition root and,
as of 2026-07-31, its only caller. Toward `vision`: `BgraImage`, the form a `Frame` has to be
viewed as before it can be recognised.

`modules/annotation` and `modules/engine` have no link edge with this program, and the reason is not
layering hygiene but the domain: an annotation session has nothing annotated to authorize against.
`entry/cli`'s `drive` subcommand deliberately re-implements this protocol's semantics — bounded line
size, fresh results file, output confinement — at the runner adapter rather than linking here.

## Related

- [`entry-m0-demo.md`](entry-m0-demo.md) — the frozen demo this program grew out of and still shares
  a substrate with.
- [`entry-cli.md`](entry-cli.md) — the product runner, and its `drive` protocol's debt to this one.
- `docs/pitfalls/capture-and-target-selection.md` — why input goes through this program rather than
  a hand-rolled `PostMessageW`.
