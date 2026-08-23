# 2026-08-23 — The framework enforces limits it was given, and does not choose them

## Decision

The framework's contract with a Project is:

- **The framework guarantees that what happened in a run is traceable and
  verifiable**, and it is responsible for the safety and verification of the
  flow — who may act, what they may reach, what was actually done, and whether
  the record of it can be reproduced.
- **What to do is the Project's decision.** The framework does not define how a
  Project's own state evolves, what a Project should do when an outcome is
  unknown, or what a Project's limits should be.
- **The framework exposes configurables and enforces what the Project chose.**
  A ceiling the framework picked and the Project cannot see is the framework
  making the Project's decision.
- A capability the Project needs is the framework's to *provide*, not to
  *interpret*.

The one-line test for any new facility: **does it record and enforce, or does it
decide?** Recording and enforcing a declared limit is the framework's job.
Choosing the limit, or choosing what the Project does next, is not.

This is not a licence to be passive. Refusal is squarely inside "safety and
verification of the flow" — an unauthorised actor, a Tool a registration never
bound, a document whose identity does not match, a budget the Project declared
and then exceeded, are all the framework's to refuse. The distinction is that
the framework enforces limits it was **given** — by the registration, by the
operator, by the published contract — and does not invent them.

## What conforms today

The durable Tool call history and its coordinate-keyed replay; admission, with
its surface, capability, delegation and approval refusals; the trace recorder;
every identity and digest comparison; the two program types and the capability
contracts that separate them; exact canonical bytes. All of these record what
happened or enforce something the Project or the operator declared.

## What does not conform, and is now known

**Ceilings are framework-chosen and invisible to a Project.**
`EngineConfig::memoryQuotaBytes` defaults to 64 MiB, `interruptBudgetTicks` to
100,000,000, and the wall-clock ceiling is a framework constant. No schema
member anywhere lets a Project state any of them. A Project that legitimately
needs a larger working set cannot ask for one, and a Project that wants to be
held to a tighter one cannot say so. These belong in the registration, with the
framework enforcing the declared value.

The signal is also generic rather than specific. The accounting allocator
refuses over-quota growth and Luau surfaces it as a catchable out-of-memory
error, so the host is never dragged down — that part is right. But the Project
learns "out of memory", not "you asked for more than the ceiling you declared,
which is N bytes". Specific warning is part of traceability; a Project cannot
act on a signal that does not say what was exceeded.

**`timeout_policy` is published and unenforced.** Recorded already as a finding.
Under this ruling its shape is settled: `on_timeout` is a **Project
configurable**, and the framework's job is to enforce what the Project chose. A
published policy nothing reads is the worst of both — the framework neither
records nor enforces, and the Project's choice has no effect.

**The Journal, the reducer, the revision and the final CAS define how a
Project's state evolves.** That is the Project's decision, taken by the
framework. Under this ruling the framework owes a Project a durable, opaque,
Project-owned channel — the Project cannot open one itself, because a scoped
program has no filesystem by construction and giving it one would dissolve the
boundary that makes any of this verifiable. What the framework does not owe is
the interpretation: no fold, no revision semantics, no compare-and-swap over
meaning it cannot read.

**The uncertain-delivery machinery decides on the Project's behalf.** `possible`,
reconciliation and the target-wide mutation barrier exist because a mutating
input may have landed before the process died. Recording that the outcome is
unknown is traceability and is the framework's. Freezing the target until
something reconciles it is a decision about what may happen next, and it is the
Project's. The sharper objection is that the uncertainty is usually resolvable
by the thing this kind of automation does every round anyway — observing the
screen again — so the framework is carrying a large apparatus to avoid an act
the Project performs regardless.

## Consequences and sequencing

Nothing here is scheduled by this ruling. The non-conforming machinery is built,
green and falsified, and removing it costs more than leaving it while a
downstream consumer has not yet run. What this ruling fixes is the **judgement**
applied from here: a new facility that decides on a Project's behalf does not
land, and each existing one above is now a named debt rather than an
unexamined feature.

The cheapest conforming work, and the only one that removes nothing, is making
the ceilings declarable and their refusals specific.

A Project-side debugger is out of scope here and is recorded as intended future
work: under this contract it is squarely the framework's, because it is
traceability handed to the person who has to act on it.
