# 2026-08-17 — Tightening the identity predicate union is a ruling, not an edit

## Decision

A text predicate may not decide Surface identity. Enforcing that by narrowing the
predicate union in the schema is **a ruling to be made, not an edit to be
applied**, because a landed test proves a different closed requirement by using
exactly the shape the rule forbids.

## Context

The plan row said the tightening was pure: "measured, neither repository has a
caller". That sentence was **wrong**, and how it became wrong is the more useful
half of this file. It was copied from a framework commit's own self-description
and recorded without independent verification.

The first step of acting on it found a caller. A runtime contract fixture declared
a Binding whose detector was a text-equality predicate, and that Binding was the
*entire* identity list of a surface — a text predicate deciding Surface identity,
precisely the forbidden shape.

It was written that way on purpose. The fixture serves a closed requirement about
real OCR failures leaving Surface identity unresolved, and all three of its
sub-cases — a silent Reader, readable garbage, and a score below the floor —
prove themselves *through* that detector: OCR fails, the detector is unsatisfied,
identity does not resolve.

So two landed rulings contradicted each other. One says text never decides
identity; the other proves its property by letting text decide identity and then
fail. Narrowing the union deletes the mechanism the second one runs on.

The attempt was rolled back in full when its premise was falsified; nothing was
left in the tree.

## Consequences

- Never record another document's self-description as a measurement. A commit
  message, review summary or plan row claiming "no callers exist" is a claim to
  verify, not evidence to copy. This exact claim was copied once and was false.
- When a tightening would delete the mechanism a closed requirement proves itself
  with, stop and rule; do not edit. The two requirements must be reconciled
  explicitly, and the fixture rewritten to prove its property another way, before
  the union moves.
- The rule itself is enforceable in the current runtime model: a Binding detector
  naming a text-equality predicate fails to parse and the whole artifact is
  refused, with the contract case in
  `tests/task/test-runtime-v3-contract.cpp` pinning it. The distinction the rule
  rests on is that an element predicate asserts an element is present, while a
  text predicate asserts what text a rectangle holds.
