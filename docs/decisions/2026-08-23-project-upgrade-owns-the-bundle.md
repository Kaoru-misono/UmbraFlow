# 2026-08-23 — `project upgrade` owns the bundle, and installs before it reports

This decision supersedes
[`2026-08-20-project-init-owns-bootstrap.md`](2026-08-20-project-init-owns-bootstrap.md)
on everything about release acquisition and the input ledger. The template half
of that decision is unchanged.

## The loop being built

A project author on an older framework release has to: **pull the new binaries,
read what the release says changed, fix their own code, then run and see what is
left.** Every ruling below serves that loop and nothing else.

## Decision

**A new verb, `project upgrade [--source PATH] [--release NAME]`.** Not a flag
on `init`. `init` means "make this directory a project", and an author reaching
for it is not asking for their installed binaries to be replaced; hiding a
destructive action inside a benign verb is how a tool becomes something people
are afraid to run.

**It installs before it reports, and never refuses on incompatibility.** An
earlier reading had it refuse a release whose shape the declaration does not
match. That is backwards: the newly installed binary is the only oracle that can
tell an author whether an adaptation is correct, so refusing to install forces
them to edit blind and re-download on every attempt. Install first, report
second. The report is the second half of the same command — `upgrade` runs the
installed `project check` and hands its exit code back — so one invocation
yields new binaries plus the whole remaining work list.

The report has to come from the INSTALLED binary rather than from the process
performing the upgrade, because the schema a declaration is judged against is
compiled into an executable. A process cannot speak for the release it just
installed.

**The swap is two renames and stops there.** Measured on Windows: a directory
holding a running executable can be renamed and cannot be deleted, and the
process performing an upgrade is normally running out of the bundle it is
replacing. So `umbraflow-bin` is renamed to `umbraflow-bin.<previous release>`,
the staged directory is renamed to `umbraflow-bin`, and nothing is deleted.

The leftover is **not a rollback feature and must not be described as one.** It
is residue of that platform constraint. Every `project` invocation removes any
`umbraflow-bin.*` as its first act, by which time nothing holds it. Rollback is
pinning `release` to the older name and upgrading again: a bundle is fully
reconstructible from an immutable release, so keeping a local copy buys nothing
and creates a second directory that can be mistaken for the live one.

A crash between the two renames leaves no live bundle and exactly one set aside.
That shape is recognised and finished — the bundle is put back and the operator
told to upgrade again. Crash-proof atomicity is not required; a recoverable,
nameable intermediate is.

**`umbraflow-kit.json` gains a required `release`, with no default.** Its value
is the literal `"latest"` or a release name. "Absent means follow latest" is the
forbidden absent-means-something reading, and this is the member by which a
project states whether it follows or pins. Pinning is also how a project gets a
durable record of which framework build it was authored against, which is why no
separate lock file is introduced.

**A pinned selector is verified against the manifest that arrives.** Without
that, a misconfigured host answering every pin with the same release makes the
pin silently stop pinning.

**Downloads retry.** `--retry 5 --retry-all-errors --retry-delay 2`. A single
TLS reset failed a whole upgrade twice against the same GitHub asset and the
same URL with these flags succeeded first try; without them one blip costs every
artifact already downloaded.

## What this deletes

**The Project Kit input ledger, and `--input`.** A build derived its file set
from `work/build/project-kit.inputs`, which only `project init` wrote. Naming a
new module in the declaration therefore made `project build` refuse the module
it had just been told about, and a tree with no build directory could not be
built at all — and the way to refresh the ledger was an `init` that reached the
network. One migration had to copy a whole source tree to a scratch directory to
verify anything.

The set of files a project has is arithmetic over its declaration. Arithmetic is
redone, not cached. `project build` and `project check` derive it, `project
init` writes nothing, and `--input` — whose whole purpose was adding a source
the declaration does not carry — is gone with the ledger it fed.

The consequence, stated so the next reader is not surprised: a generated
adapter is now produced because a **generated deployment names the module the
generator writes**, and never because a command line named a declaration.

**`project init`'s release acquisition, and the verification of an already
installed bundle that came with it.** `init` reached the network and a second
`init` re-verified the local bundle's digests. Both are gone: `upgrade` verifies
what it installs, and nothing re-verifies a bundle afterwards. A tampered local
bundle is no longer detected by any `project` verb. That is a real property
given up, and it is recorded here rather than quietly dropped.

## The iterate loop's own verb

`project check`. It reaches no network, writes nothing into `umbraflow-bin`, and
after the ledger's deletion needs no earlier command to have run, so it can be
re-run after every edit for as long as it takes to work through the answer.
`project build` applies the same judgements and additionally materialises, so
either verb yields the same problem list; `check` is the one named because it is
what an author reaches for and what `upgrade` runs on their behalf.

## The complete adaptation report

`json::Evaluator` short-circuits, which is right for a verdict and wrong for a
work list: a declaration with six problems reports one and the author iterates
six times. The evaluator is **not** changed — making it accumulate would touch
every schema use in the tree, and short-circuit is correct accept/refuse
semantics.

Instead `json::Schema::explainRefusal` runs **after** a refusal and does pure
set arithmetic at the one instance path the refusal names: `required` minus the
instance's members is what is missing, and for a closed object the instance's
members minus the `properties` keys is what is not declared. Each missing member
is printed with its own `$comment`, because this repository already writes the
explanation an author needs there.

**It never accepts anything.** The validator remains the sole authority; the
report runs only after a refusal and only explains it. A report that finds
nothing while the validator refuses is a bug in the report. It must not grow
into a second reader: no `$ref` resolution beyond the path it walks, no format
or pattern logic, no re-implementation of validation semantics.

## The joins the kit now applies

`project check` accepted declarations `umbra-flow open` refused. The owner's
loop is "run and see what is left", and it is not satisfied while a second,
later command can still refuse. Two of the loader's rules move to the kit —
through the loader's own functions, so there is one authority and not two
implementations:

- `operator_runtime::validateToolNameOwnership`, for a Tool name against the
  namespace its deployment's `plugin_id` owns.
- `operator_runtime::validateProjectToolBindings`, for the binding table against
  the declared Tool names and the closure's stated exports. It absorbed a rule
  that was already written twice in the Operator, in `joinToolDeclaration` and
  in `ProjectToolBindingTable::bind`.

The rule that every declared Tool is bound stays in the registrar and is
**deliberately** not applied by the kit. This repository's own
`project init --plugin generated` scaffold declares a Tool and binds none,
because the declarative generator has no renderer for a bound entry; applying
the rule at `check` would refuse the starter the framework ships. Fix the
generator and the scaffold before moving it.
