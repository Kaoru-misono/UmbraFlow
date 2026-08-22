# 2026-08-22 — Caller independence is structural, and the fixture is its witness

## Decision

This ruling refines, and does not reopen,
[`tools are the shared game-driving boundary`](2026-08-21-tools-are-the-shared-game-driving-boundary.md),
which made Agent, human clients and Project automation first-class callers of one
Tool Runtime. It fixes what enforces that "one".

"The same Project Tool behaves the same and passes the same authority checks
whether its caller is Agent, human client, another Tool, or a Project run" is a
**structural** property of the runtime, not a test result. Both the structure and
one semantic fixture are required, and neither substitutes for the other.

### The structure

There is one internal admission-request value — actor identity, target tool name,
canonical arguments, issuing coordinate context — consumed by exactly one
admission function. Every executable artifact downstream of that function is
constructible only inside the runtime: the position identity, the delegation
grant, and the capability to dispatch a call. The capability to execute is a
move-only value only the runtime mints.

The pattern already exists and is only being extended:
`ToolCallPositionIdentity::create` is private to the issuing context, so no
caller can hand the seam an ordinal. Admitted-call construction and grant
construction become private the same way.

An adapter is then *definitionally* a translator from transport specifics into
the request value. It cannot bypass authority, because nothing it is able to
construct is executable.

### Where the boundary sits

Below every adapter, above all authority. An adapter resolves actor identity and
canonicalises arguments, and is then out of the frame. Policy, approvals,
envelope intersection, session and target authority, and budgets evaluate once,
inside, on the request value.

There are four producers, not three: the Agent adapter, the human-client adapter,
the producer that admits an actor's start at the top of a run, and Tool calling
Tool. The fourth already obeys this, because a child call has never had anywhere
else to go. The three root producers are additional producers feeding the same
funnel, symmetric with that native seam.

### What the fixture is for

Structure cannot see semantic equivalence of *inputs*. An adapter could
canonicalise arguments differently, or map its transport principal to a subtly
different actor identity, and still be structurally incapable of bypassing
anything. One semantic fixture compares, across all four producers: canonical
argument bytes, admission outcome, durable row attributes, and result, modulo
actor identity.

Structure guarantees that one evaluation path exists. The fixture guarantees that
the producers feed it equivalently. A plan that promises only the fixture has
promised half of this ruling.

## Context

The earlier statement of this property was a test obligation: "the same Project
Tool must have the same behavior and authority checks whether its caller is
Agent, human, another Project Tool, or automation script. Tests compare those
actor paths against one semantic fixture."

A test-only guarantee is a promise re-made by every future adapter author. The
adapter set is an extension point — a new transport, a new client, a new
embedding — and a property that must hold for every future extension must be
unbypassable by construction, because the author who breaks it is by definition
not the author who read the fixture. Four adapters each assembling their own
path to the Tool Runtime is also four spellings of "issue a call", which is the
shape the house rules ban independently of whether any one of the four is
currently wrong.

The counter-argument considered was that the funnel is a large refactor for a
property tests already cover. It is not large: the funnel is one value and one
function, and the privacy that makes it unbypassable is already demonstrated by
`ToolCallPositionIdentity::create`. What the refactor buys is that the failure
mode changes kind — from "an adapter evaluated authority differently and the
fixture did not cover that case" to "the adapter does not compile".

The fixture is not thereby redundant, because the two mechanisms fail
differently. Structure is blind to what an adapter puts *into* the request; a
mistranslated principal or a differently canonicalised argument passes every
private constructor and produces a legitimately admitted call with the wrong
meaning. Only a semantic comparison across producers catches that, and only
structure catches an adapter that skips admission altogether. Dropping either one
leaves a whole class uncovered.

## Consequences

- An adapter may not construct an admitted call, a delegation grant, a position
  identity, or a dispatch capability. If a new adapter needs one, the request
  value is what is missing, not the constructor's access level.
- Authority evaluates exactly once, inside. An adapter that pre-checks policy or
  budget so that it can return a nicer error is duplicating the authority
  decision and is refused; a refusal from inside is what an adapter renders.
- Adding a fifth caller is adding a translator. If a proposed caller cannot be
  expressed as a translation into the request value, that is a finding about the
  design and not a reason to add a second path.
- The four-way semantic fixture lands with the second producer, not after the
  last one, and grows as each further producer is added. A fixture written once
  against a finished adapter set would be written after the divergence it exists
  to catch.
- "Same behaviour regardless of caller" may not be cited as satisfied on the
  strength of the fixture alone, nor on the strength of the funnel alone.
