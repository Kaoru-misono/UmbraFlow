# 2026-08-23 — What the framework publishes for the Tool Runtime

## Decision

Three rulings, to land in this order, because regenerating the public contract
must not publish a surface that is about to be cut.

### 1. The residual Operation surface is cut entirely

`Operation`, `DispatchRecord`, `AuthorityDecision`, `ApprovalToken`,
`ReconcileProposal`, `MutationChain`, `CommandRecord`, `OperationState`,
`OperationTimestamps`, `PlanVersion`, `PlanProposal`, `ObservedOutcome`, the
`Delivery*` definitions, `DispatchOutcome`, `ReceiptRef`, `ProgressMarker` and
the two superseded roots leave `schema/umbraflow-operator-v1.schema.json`, in
the same change that deletes `submitCommand`, `transitionOperation`, the
`operations` table with its database identity migration, and the four production
queries against it. `OperatorSession`, `SessionManifest`, `ControlLease` and the
other live definitions stay.

**The cut must fill a hole rather than only remove.** `requireQuiescentSessionPin`
queries `operations` and nothing else, so today an upgrade pin performs no check
at all against an active Tool mutation. Retarget it onto the Tool tables in the
same change. A cut that silently removes a check is a negative with no named
replacement.

### 2. `tool_runtime_protocol_identity` is derived from protocol material

A `currentToolRuntimeProtocolMaterial()` renders the protocol's own constants
canonically — the preimage tags and their member order, the call-state wire
names, the completion kinds, the durable column set, the JCS contract, and the
embedded `operator/tool-catalog` identity bytes — and the identity is the
SHA-256 of that material. The generator publishes the material the way the
public contract already publishes the environment hash.

The two-source shape applies. The recording incarnation derives from the release
it is running and writes the value into the `tool_runs` row; a resuming or
replaying incarnation re-derives from its own release bytes, and the existing
equality at `ledger.cpp:10218` is that join. It differs from
`plugin_environment_hash` only in that both sides are framework-owned, so the
join is across incarnations rather than across publisher and registrar — the
durable row is independent of the current binary, which is what makes the two
sources genuinely two. The field stays an assertion inside the resuming reader
and is never a dispatch key.

### 3. What the public contract must gain

Read out of bytes already here: the nine call-state wire names and the terminal
split, from `k_toolCallStateNames` / `toolCallStateWireName`; the answer envelope
a script sees, from `checkedAnswer` in `modules/task/runtime/tools.luau`; the
root-idempotency relation, from `rootIdentityMaterial` and `relationTo`;
`tool_catalog_hash` as the SHA-256 of the catalog document's JCS bytes; the
reproducible half of the durable record — root request and call position — with
admission attempts published as *deliberately not reproducible* rather than
omitted; and the shipped `umbra-flow invoke` usage and refusal strings, which
today appear nowhere in the generated contract at all.

## Context

Work package 11 is "regenerate the public contract and publish one matching
release", and section 14.3 recorded it as blocked because "neither `schema/` nor
the generated public contract describes a Tool Runtime call state, envelope or
identity preimage". A downstream repository is blocked on this framework
publishing, and `docs/PUBLIC-CONTRACT.md` is the only document that repository
is permitted to read — so anything a consumer needs and cannot find there is a
fact it will transcribe from somewhere that rots.

## Why the protocol identity was the sharper finding

The plan recorded `tool_runtime_protocol_identity` as an opaque caller-supplied
hash that nothing derives from release bytes. That is stale: it *is* derived, at
`product-lifecycle.cpp:623`, from the framework Tool catalog hash. The defect is
worse than the one recorded, because it looks correct.

The framework catalog hash covers framework Tool descriptors and nothing else.
The state vocabulary, the preimage tags, the completion kinds, the durable
record split and the JCS contract can every one of them change without moving
it. So the equality at `ledger.cpp:10236` is named for a property it cannot
observe, and no test can redden it on that property —
[`checks that cannot fail`](../pitfalls/checks-that-cannot-fail.md) exactly.
Compounding it, the provider identity for a framework call is the *same* hash,
so the preimage carries a same-source duplicate that cannot mismatch.

A digest that is derived from the wrong bytes is harder to find than one that is
not derived at all, because the absent case is visible in a grep and this case
is only visible by asking what the material covers.

## Why the Operation surface could not be cut halfway

Three findings force the whole cut rather than a smaller one.

The barrier does not need it. `requireNoActiveToolMutation` is the live detector
and queries the three Tool tables; the `operations` queries beside it are a
second branch that is empty in production, because production has no writer for
that table. A branch with no possible input is a branch no test can redden.

The pieces are one connected chain. Deleting a published definition immediately
reddens the contract tests that assert on the published bytes; deleting those
tests leaves `submitCommand` and `transitionOperation` with no caller; that
leaves the table with no writer; that makes the four production queries dead
code. There is no ordering of these that leaves a coherent intermediate state.

And the superseded roots describe the previous generation's runtime. Publishing
them to the one document a consumer is permitted to read is not merely surplus —
it is a description of a runtime that no longer exists, offered as the contract.

## The one tension in this ruling, and how it resolved

Publishing all the call-state names sat uneasily beside section 14.4's finding
that `rejected` had no producer. Publishing the name of a state nothing writes
has the same shape as publishing the Operation surface, which this ruling cuts
for exactly that reason, so the ruling named two ways out — give `rejected` a
producer before regenerating, or publish the set as the storage vocabulary it is
— and foreclosed only silence.

It resolved by a third way on the same day: `rejected` was deleted. A durable
`rejected` would be a terminal row, but every admission refusal is a function of
live authority, so writing one would permanently freeze a call that a fresh
lease legitimately re-admits; section 5.3's `proposed` recovery is already the
correct recovery for a refused admission. The published set is therefore the
states that are written, and the tension does not arise.

What that leaves for WP11 is a standing obligation rather than a one-off check:
a state name reaches the published contract because something writes it. Any
future addition to the vocabulary that has no producer must be refused entry to
the contract on this ruling's own argument.
