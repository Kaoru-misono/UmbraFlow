# 2026-08-22 — A Tool name is owned by its whole namespace

## Decision

This ruling refines, and does not reopen,
[`tools are the shared game-driving boundary`](2026-08-21-tools-are-the-shared-game-driving-boundary.md),
which assigns every Tool an owner **via its namespace**. It fixes the grammar
that makes that assignment checkable, the check itself, and the one deletion that
follows from the check.

### The name is namespaced, and the dot is required

The Tool Catalog wire schema's `name` is a **namespaced identifier**: a dot is
required, not optional. `tool_name` on a plan, each entry of a child-effect
declaration's `child_tool_names`, each delegation grant name, and the
registration binding table's `tool_name` all validate as that same type — one
spelling of "tool name" in every document that carries one. The binding
validator's separate contains-a-dot check is subsumed by it rather than kept
beside it.

Every fixture, shipped example, and recorded name in a test or a development
journal is renamed in the same change that tightens the schema.

### The ownership rule is enforced positively, at catalog admission

A Tool's namespace must equal the registrant's owned namespace, or be
Framework-owned. This is checked where a catalog is admitted, by one function, so
that the authoring tier and the Operator apply the same rule rather than two
readings of it.

### What "the namespace" means

**The namespace is the owner's whole registered namespace, and the local name is
everything after the dot that ends it.**

- The Framework's owned namespace is `framework`, one segment. Its Tools are
  `framework.screen.observe`, `framework.audit.record`, and so on: namespace
  `framework`, local name `screen.observe`.
- A Project's owned namespace is its entire `plugin_id`, which is itself required
  to be namespaced and is therefore at least two segments. Its Tools are
  `arcana.expedition.move`, `fixture.control.command-1`: namespace
  `arcana.expedition`, local name `move`.

The check is consequently a prefix test at a dot boundary — the name begins with
the owned namespace, the next byte is a dot, and at least one byte follows it —
and **not** a split at the first dot. A local name may itself contain dots; the
owner's namespace is what fixes where the boundary is, and nothing else does.

**This reading resolves a conflict inside the ruling as it was first written.**
That wording said both that the first dot separates the namespace from the local
name *and* that the namespace must equal the registrant's owned namespace,
normally its `plugin_id`. The two sentences cannot both be obeyed literally,
because a `plugin_id` is itself required to carry a namespace: under a first-dot
split, `arcana.expedition.move` has namespace `arcana`, which is not
`arcana.expedition`, so **every** Project Tool would be refused and every
Project's owned namespace would be unreachable by construction. Only the
whole-registered-namespace reading satisfies both sentences at once, and it is
the ruling's meaning. The first-dot sentence was describing the Framework case,
where the namespace happens to be one segment, and generalised wrongly from it.

A later reader must not re-derive the literal reading from the shorter sentence.
Under it the positive ownership check refuses the entire Project tier, which is a
refusal no Project can avoid and therefore not a rule but a prohibition on
Project Tools existing.

### The negative refusal is deleted, and what that rests on

The old negative half — a check refusing a non-Framework registrant that names a
Tool under `framework.` — becomes unreachable behind the positive check and is
deleted in the same change, rather than left as a check no test can make fail.

That deletion rests on a premise that must be stated where it can be found,
because the deletion is unsound without it: **a `plugin_id` inside `framework.`
is refused at the registration.** Without that refusal the positive rule has a
hole exactly the width of the deleted check — a project calling itself
`framework.impostor` would own `framework.impostor.*` legitimately, and nothing
downstream would object, because the positive check would find the name inside
the namespace the registrant registered.

The Framework's ownership of `framework.*` is therefore stated **once**, at
registration-claim validation, rather than re-stated at every catalog row. That
is the whole content of the negative half, moved to the one place it is
enforceable, and it is why deleting the row-level check subtracts nothing.

## Context

**Why enforcement forces the dot rather than tolerating undotted names.** The
frozen ruling assigns each Tool an owner via its namespace. An undotted name has
no namespace, so there is nothing for the ownership check to check. Enforcing the
positive rule over an optional-dot schema therefore has exactly two outcomes:
exempt undotted names, which is a hole that guts the rule for anyone willing to
drop a dot; or refuse them all, which is the tightening reached by a longer
route. There is no third outcome, so the tightening is not a separate decision
that could have gone the other way — it is what enforcing the rule at all means.

**Why the alternative — relaxing the binding validator — is worse than it
looks.** Admitting undotted names into bindings requires giving an undotted name
a meaning, and the only meaning available is "the caller's own namespace". That
is an absent value carrying a meaning, which the house rules ban outright, and it
simultaneously makes one Tool addressable under two spellings, bare and
qualified — two violations in one move. It also does not survive contact with the
feature set: delegation grants and child calls cross project boundaries by
construction, so undotted names collide the moment two catalogs declare the same
bare word, and the collision surfaces as an authority question rather than as a
name error.

**Why the deleted check was not merely redundant but harmful to keep.** A
refusal that nothing can reach is a green name with no failure mode behind it —
the class
[`checks that cannot fail`](../pitfalls/checks-that-cannot-fail.md) exists to
catch. Keeping it would also leave two answers to "who owns `framework.*`" in the
tree, one at the registration and one per catalog row, and a later reader
weakening either would have no way to see that the other was load-bearing.

**Why this was live drift rather than a future decision.** Before the change, the
shipped examples and the shared test fixture declared Tool names that no
authoring tier could bind, because the binding validator already required a dot
while the catalog schema did not. The tree shipped Tools its own authoring path
could not use. The rename of fixtures, examples and recorded names is therefore
part of the same change and not follow-up work: the schema tightening is what
makes those documents describable at all.

## Consequences

- Catalog `name`, plan `tool_name`, `child_tool_names` entries, grant names and
  binding `tool_name` are one type with one grammar. A document that admits a
  spelling another document refuses declares a Tool no authoring tier can bind,
  and is a defect in whichever of the two drifted.
- A Project cannot declare a Tool in another Project's namespace or in the
  Framework's, and the refusal names the namespace its registrant owns. The
  Framework's Tools are `framework.<local>` by construction rather than by a
  runtime ownership check on a compile-time catalog.
- The reserved-prefix refusal must not be reintroduced "for defence in depth".
  Its premise — the registration-level refusal of a `plugin_id` inside
  `framework.` — is where the rule lives; a second copy at the row level would be
  a check nothing can make fail, and a reader would not be able to tell which of
  the two carried the rule.
- Weakening the registration-level `plugin_id` refusal reopens the hole this
  deletion closed. Any change to it must be read as a change to Tool-name
  ownership, whatever else it is about.
- A future owner with a one-segment namespace is possible — `framework` already
  is one — and the whole-namespace reading handles it without a special case. A
  future proposal to key the split on segment *count* rather than on the owner's
  registered namespace is refused: it re-creates the conflict this ruling
  resolves.
