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
