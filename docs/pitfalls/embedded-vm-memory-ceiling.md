# Embedded VM memory ceilings

What a hard memory ceiling on an embedded VM actually measures, and why the two
obvious tests for it prove nothing. Recorded on 2026-08-02 after an exploration
session died with a bare `not enough memory` and took three chunks of an agent's
session to diagnose.

The instance is Luau behind `EngineConfig::memoryQuotaBytes`, but nothing here is
about Luau specifically: it applies to any runtime we embed under an accounting
allocator that can refuse.

## A ceiling on a runtime with no emergency GC measures live bytes plus garbage

### Symptom

A long-running VM works, then stops: **every** allocation fails, including ones
far smaller than any that succeeded a moment earlier. The error names no figure
and no culprit — the whole of it is `not enough memory`. Closing the VM and
opening a fresh one recovers immediately, which reads exactly like a leak.

Measured: an exploration session cropping full frames in a loop
(`explore.crop(0,0,1600,900)`, ~1.8 MB of PNG per call) died after some tens of
crops; afterwards even a single 200x200 crop plus a probe failed.

### Root cause

Nothing leaked. Two facts compose:

1. The accounting allocator refuses any growth past the ceiling and returns null
   (`modules/script/source/script/ffi/allocator.cpp`), which is the point of
   having one — an over-quota task must fail without dragging the host down.
2. Luau throws `LUA_ERRMEM` **the instant** `frealloc` returns null
   (`VM/src/lmem.cpp:248`, `:505`, `:545`). Unlike PUC Lua's `luaM_realloc_`,
   there is **no emergency full collection and retry**.

So the ceiling is not measured against the live set. It is measured against the
live set **plus everything the incremental collector has not reached yet**, and
an allocation rate of megabytes per call outruns the collector easily. A fresh VM
recovers because it is fresh, not because the old one was holding anything.

The trap in diagnosis is that "restarting fixes it" is the signature of a leak
*and* of this, and only this one has a fix that is not a hunt.

### Fix

The seam cannot be the allocator: adding a retry there means re-entering the
collector from inside the callback the collector itself runs under. Put the
collection where an ordinary call frame exists, at both scales:

- **At the natural boundary.** For a chunk-fed session that is the chunk: nothing
  is meant to survive one except files on disk, so a full collection there
  reclaims garbage and cannot reclaim anything the next chunk needs.
- **Under pressure, before a large allocation.** The boundary alone does not save
  a *single* iteration-heavy call, which is the shape that actually failed. Check
  free headroom against the payload immediately before minting it, and collect
  first if it is tight. `cycle_crop` uses four times the payload: one for the
  payload, one for what the push costs besides the bytes, and two as hysteresis —
  collecting at exactly one payload leaves the next call against the ceiling
  again, since the object just minted is live, so the session would pay a full
  sweep per call from then on.

And make the wall visible before it is hit: report used-against-ceiling on every
result line, and when a failure happens with the ledger near the ceiling, say so
with both figures while keeping the runtime's own sentence verbatim in front of
them. That sentence is the difference between a three-chunk hunt and one line.

### Regression check

Under a small ceiling, a loop that allocates and **drops** large objects must
complete. Remove the pressure check and it must fail against the ceiling — not
merely fail, but fail with the ledger at the wall, which is worth asserting
separately because a badly built test fails for other reasons (see both entries
below).

## A test that allocates the same bytes twice allocates once

### Symptom

A memory test passes with the fix removed. The loop is right, the sizes are
right, and the peak never moves.

### Root cause

Luau interns strings, long ones included, so N iterations producing identical
bytes produce **one** object. The first version of the test above cropped one
rectangle forty times and therefore allocated once. The same trap sank two other
cases where the payload was `string.rep` of a constant.

### Fix

Vary the payload per iteration — slide the rectangle, concatenate the index — so
each iteration genuinely allocates. Then confirm the variation is load-bearing by
removing the production change and watching the test go red.

### Regression check

This is the general form of the repo's falsification rule and the reason it
exists: a memory test you have not personally watched fail is not evidence that
anything is being reclaimed. Neutralize, watch it go red, restore.

## Loop shape decides whether the incremental collector keeps up

### Symptom

The pressure path is correct and unreachable: with the fix removed the test still
passes, because the collector kept pace on its own.

### Root cause

An incremental collector does work proportional to allocation *and* to
interpreted work. A loop that does enough of the latter per megabyte — opening an
observation cycle per iteration, for instance — gives the assist collector room
to keep up, and the ceiling is never approached. The failure needs the shape that
actually occurred: a bare loop allocating over one already-open cycle.

### Fix

Write the test in the shape the real caller has, and say so in a comment, because
the "simpler" version of such a test is the version that proves nothing and looks
tidier. The annotation loop that hit this cropped repeatedly inside one cycle;
that is what the test does.

### Regression check

Any test asserting that a memory guard fires must be shown to fail without the
guard. If it does not, the loop is doing the collector's work for it and the
guard is untested whatever the assertions say.

## A registry that records identity outlives everything it names

Added 2026-08-03, after the ceiling above was reached a second time by a
different route.

### Symptom

`project.load_project` left roughly 700 KB behind on every call, monotonic, and
an exploration session walked into the ceiling after some 150 loads. Reading and
parsing the file cost nothing lasting; only the phase that built objects did.

### Root cause

Eleven module-level tables recorded every object a constructor minted, keyed by
the object, so a look-alike could be refused later. Nothing ever removed an
entry. The entries are not the cost — a boolean against a key is nothing — but
the key is a strong reference, so one page kept its references, which kept their
elements, and a model that no caller could still name stayed whole.

This is the shape to recognise: **a table whose purpose is to remember that
something exists will, by default, also make it exist.** Provenance registries,
interning caches, "have I seen this" sets and debug ledgers are all of this
kind.

### Fix

Weak keys, `setmetatable({}, { __mode = "k" })`. It costs the registry nothing
it was doing: an entry can only vanish once the object is unreachable, and an
object nobody can reach is one nobody can present to the predicate. Where the
value is not a boolean, check the value cannot reach the key — a value that can
is an ephemeron the collector must still keep.

Fix all of them at once, or measure after each. Weakening only the element
registry changed nothing measurable, because the pages were still held strongly
and the pages held the elements.

### Regression check

Assert on the heap reading, not on the registry: mint a fixed number of objects
per chunk for several chunks and require the reading not to climb. Restore one
`__mode` to strong and watch that one test go red -- measured at +1.87 MB over
2000 elements, against a bound of 256 KB.

## An empty result rules nothing out until the experiment can produce one

### Symptom

The registries above had already been investigated and excluded. The recorded
evidence was that weakening all ten (there are eleven) and re-measuring gave
"ten readings byte-identical to the ten before". That was read as *the change
had no effect* and the hypothesis was dropped, with the change reverted so as
not to leave an ineffective patch. It was the correct hypothesis.

### Root cause

Byte-identical readings across a real change are not a weak result, they are an
implausible one. A live system re-measured after a rebuild moves by at least
allocator granularity. The reading was evidence about the *experiment* -- that
the binary under test was not the binary that was changed, or the path was never
exercised -- and it was spent on the hypothesis instead.

### Fix

Before a negative result is allowed to exclude anything, show the experiment can
produce a positive one. Here that meant a control the leak could not hide in: 500
minted elements per chunk against 500 plain frozen tables per chunk. The plain
tables moved the reading zero; the minted ones moved it half a megabyte every
chunk. Only then is "weakening the keys removes the growth" a result about the
keys.

### Regression check

Not a test -- a reading habit. When an experiment returns exactly what "no
effect" would return, ask what a *detected* effect would have looked like and
whether this run could have shown it. If nothing distinguishes the two, the run
measured nothing.
