# 2026-08-24 — A collection's layout has a bounded extent, and slots round per slot

`[[collection]]` could not express a card game's hand. This ruling widens the
one rule rather than adding a second one.

## What could not be said

`slots` is required, carries `origin`, `pitch` and `tolerance`, and declares a
centred layout by `slot_k = origin - (n-1) * pitch / 2 + k * pitch`. It assumes
**a fixed pitch, and therefore an extent that grows with cardinality**.

A hand of cards fans out from centre with a **bounded total width**, so the
spacing shrinks as the hand grows. Measured on a real target at 1600x900,
anchored on each card's type line:

```
n     item origins                                          pitch   span
2     632, 866                                              234.0    234
3     528, 750, 971                                         221.5    443
4     417, 640, 859, 1080                                   221.0    663
5     389, 571, 754, 932, 1112                              180.8    723
6     376, 527, 679, 830, 982, 1130                         150.8    754
8     351, 469, 581, 697, 814, 926, 1040, 1151              114.3    800
9     364, 463, 563, 660, 751, 851, 946, 1042, 1139          96.9    775
10    353, 440, 527, 618, 705, 794, 881, 975, 1062, 1153     88.9    800
```

with `n=1` at `750`. The span saturates near 800.

An exhaustive search over every even pitch: **no single `(origin, pitch,
tolerance)` expresses this.** The best single pitch is 122 and its worst slot
error is 152 px — a tolerance of 125% of the pitch, so slots would overlap by
more than a whole pitch and the resolver would refuse the assignment. This is
not a near miss to be tuned. It also does not degrade gracefully: a declared
layout that does not match reality resolves to `unknown` and carries no items.

## The decision

One rule, one spelling, with the pitch taken from whichever bound binds:

> **`p_n = min(pitch * (n-1), extent) / (n-1)`**

`extent` becomes a **required** member of `slots` for every collection. An
optional `extent` whose absence meant "unbounded" would be the
absent-means-the-old-behaviour reading this repository forbids — and "unbounded"
is a falsehood anyway: a Surface is finite, so every centred layout already has
a finite extent. Requiring it makes an existing fact visible rather than adding
a new burden, and it is one integer obtained in the same measuring session as
`origin` and `pitch`: the span of the widest layout observed.

### Integer pixels move from `pitch` to the slot

There is no constraint on `extent` equivalent to "pitch must be even" — that
would require `extent` to be divisible by every `2(n-1)`, which is impossible.
The integer guarantee therefore moves to the slot itself, and once it moves, the
even-pitch constraint is a residue and is **deleted**. `pitch` becomes any
positive integer.

One rule, pure integer arithmetic, half-up:

- `n = 1`: `slot = origin`
- `n >= 2`:
  - `m = min(pitch * (n-1), extent)`
  - `a = 2 * origin * (n-1) + (2k - (n-1)) * m`
  - `d = 2 * (n-1)`
  - `slot_k = floor((2a + d) / (2d))`

### The overlap check becomes a runtime check

With `extent` in play, overlap cannot be decided statically at declaration time.
It is replaced by a per-cardinality runtime check: for the detected `n`, compute
the rounded slots and refuse the assignment when the smallest adjacent gap
`g_min < 2 * tolerance + 1`. The refusal names the cardinality, `g_min` and the
tolerance — a specific signal, as always.

## Verified, not asserted

With `origin = 750, pitch = 208, extent = 764, tolerance = 28`, checked against
every one of the nine measured cardinalities above:

```
n     g_min   worst error
1     -       0
2     208     14
3     208     14
4     208     21
5     191     21
6     152      8
8     109     19
9      95      7
10     84     21
```

Worst error 21 against a tolerance of 28, and the smallest gap anywhere is 84
against the ambiguity floor of `2*28+1 = 57`. Every cardinality passes with
margin at both ends.

**The existing event collection does not move.** With `origin = 629,
pitch = 418, extent = 836`, the new formula yields `629`, `420, 838` and
`211, 629, 1047` — byte-identical to what the current rule yields, and within
two pixels of its measurements. The migration is behaviour-preserving for the
one collection that already exists.

## Why 21 px is accepted when 6.9 is reachable

A per-cardinality pitch table reaches a worst error of 6.9 px, which is the floor
imposed by the fan's own arc. It buys nothing. **The only consumer of a slot
coordinate is assignment to an index**; click geometry always comes from the
measured rectangle, never from the slot. Exceeding tolerance produces a refused
resolution, not a wrong click. Across the whole interval between 21 and 6.9 every
rule behaves identically on all measured data, so the extra precision is
decoration bought with authored data proportional to the cardinality range —
plus a hole for every cardinality never observed.

Tolerance is ruled at 28: seven above the worst fit error, to absorb the
within-cardinality noise the fan's own arc produces, and thirteen below the
ambiguity line.

## An unobserved cardinality may be covered by the rule

`n = 7` has never occurred on this target. A closed rule covers it anyway, and
that is **legitimate declaration, not invented data**.

The framework's line is that it enforces what it was given. A Project states a
rule; the framework verifies every detection against it and, when the rule is
wrong, fails closed — refusing the assignment and reporting `unknown` — never
producing a wrong action. Requiring that every cardinality be observed before a
collection may be declared would have the framework audit what a Project is
allowed to believe, which is the wrong side of that line. Every declaration
generalises: the event collection's is equally a belief about frames this session
has not seen. The first appearance of `n = 7` is the day it is verified, and a
refusal is the correction signal.

## Migration

1. `slots` gains a required `extent` — a positive integer, the upper bound on the
   distance between the first and last slot on the order axis.
2. The slot formula is replaced by the rational-plus-half-up rule above, with
   `n = 1` as `origin`.
3. The "pitch must be even" constraint and its schema comment are deleted; pitch
   is any positive integer.
4. The schema comment is rewritten around `min(pitch * (n-1), extent)`. The old
   text describes fixed pitch only, and leaving it would be documentation drift.
5. The overlap refusal moves from a static declaration-time property to the
   per-cardinality runtime check, with the specific signal above.
6. Every collection declaration already on disk gains `extent` **in the same
   change**: the event collection gets `extent = 836`. No default, no absent
   reading.
7. The hand collection is declared `origin = 750, pitch = 208, extent = 764,
   tolerance = 28`.
8. **Breaks**: a declaration without `extent` fails validation; every resolver
   path reading `slots` implements the new formula; the declaration-time overlap
   check's call site is deleted.

## Deliberately left unresolved

- The systematic residue in the fan — outer slots pulled wide at small
  cardinalities, compressed by the arc at large ones — is not modelled and is
  absorbed by tolerance. Revisit only if a real target needs a tolerance
  approaching half the pitch.
- The small drift of the measured centre across cardinalities (749–753) is not
  modelled, and is absorbed the same way.
- `n = 7`'s real layout awaits its first observation. The rule already predicts
  it; a refusal is the correction signal, and no data is manufactured ahead of
  time.
