# 2026-08-23 — The Project-side upgrade path

For a project author holding a directory that runs on an older framework
release. This note says what **broke**, what replaced it, and what the new loop
is. The reasoning is in
[the ruling](../decisions/2026-08-23-project-upgrade-owns-the-bundle.md); the
published surface is [`docs/PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).

## The loop

```text
project upgrade            # new binaries in place, plus the whole work list
<edit umbraflow-project.json and your Luau>
project check              # what is still wrong, offline, as often as you like
project build              # materialise, then check again
umbra-flow open --project .
```

## Reaching this release costs one manual step, once

`project upgrade` did not exist before this release, so the `project` executable
already in your `umbraflow-bin/` cannot run it:

```text
> umbraflow-bin/project upgrade --source .
unknown project action "upgrade"
```

Download `project` from this release's assets by hand and run `upgrade` from
wherever you put it, once. It installs the whole bundle, and the `project` it
installs knows the verb, so every later upgrade is `project upgrade` from
`umbraflow-bin/` and nothing else. Measured on the only existing consumer: the
hand-run binary installed the bundle, and the installed one answered `upgrade`
in its own usage immediately afterwards.

This is the last release that will cost that step, and it is the reason
`umbraflow-kit.json` also needs `release` added by hand before the first
upgrade — the member is new here, and the parser requires it.

## `project upgrade` is a new verb, and `project init` no longer acquires

`project init` used to install the release `umbraflow-kit.json` names. It does
not any more: it makes a directory a project — writes a starter when
`umbraflow-project.json` is absent, judges the declaration, creates the build
directory — and touches no network and no `umbraflow-bin/`.

Installing binaries is `project upgrade [--source PATH] [--release NAME]`. It
stages under `work/release-staging/`, downloads every artifact the manifest
selects for your host, verifies each against its declared sha256, swaps it into
`umbraflow-bin/`, and then runs the newly installed binary's own
`project check` and hands you its exit code. So one command yields new binaries
plus the whole remaining work list.

**It installs whether or not your declaration matches the release.** That is
deliberate. The binary it installs is the only thing that can tell you whether
your adaptation is right, so refusing to install would leave you editing blind.

### The leftover bundle is not a rollback

After an upgrade you will see `umbraflow-bin.<previous release>` beside
`umbraflow-bin`. A directory holding a running executable can be renamed on
Windows and cannot be deleted, and `project upgrade` is normally running out of
the bundle it replaces — so it renames and stops. **The next `project` command
deletes it**, as its first act.

Do not treat it as a rollback and do not run anything out of it. To go back, pin
`release` in `umbraflow-kit.json` to the older name and upgrade again; a bundle
is fully reconstructible from an immutable release.

If a machine dies between the two renames you will have no `umbraflow-bin/` and
one `umbraflow-bin.*`. The next `project` command names that state, puts the
bundle back, and tells you to upgrade again.

## `umbraflow-kit.json` gains a required `release`

```json
{
  "host": "https://github.com/OWNER/REPO",
  "manifest": "umbraflow-release-v1.json",
  "release": "latest"
}
```

`release` is **required and has no default**. Its value is the literal string
`"latest"` — follow the newest release — or a release name, which pins. A
document without it is refused:

```text
umbraflow-kit.json must carry exactly "host", "manifest" and "release"
```

There is no lock file. Pinning `release` is how a project records which
framework build it was authored against.

`--release NAME` overrides the document for one run, and a pinned name is
checked against the manifest that arrives, so a host answering a pin with
another release is refused rather than silently installed.

## `--input` and `work/build/project-kit.inputs` are gone

`project init --input RELATIVE_PATH` no longer exists, and neither does the
ledger it wrote. Passing it is refused by name:

```text
unknown project argument "--input"
```

The set of source files a project has is now derived from
`umbraflow-project.json` on every run — the document itself, every module of
every deployment's closure, the declaration each **generated** module is
rendered from, and every resource. Nothing is cached, so nothing goes stale.

Two things this fixes, both hit in a real migration: naming a new module in the
declaration used to make `project build` refuse the module it had just been told
about, and a tree with no `work/build/` could not be built at all.

**One thing it takes away**: a file no member of the declaration names is not an
input. If you relied on `--input` to pin an extra authoring source by digest,
declare it — as a resource, or as the declaration a generated module is rendered
from — or drop it. In particular a declarative-tools declaration is now reached
only through a deployment whose `plugin_authoring` is `generated` and whose
closure names `generated/adapters/PLUGIN_ID/NAME/tool.luau`.

## Downloads retry

`--retry 5 --retry-all-errors --retry-delay 2`. A single TLS blip used to fail
the whole operation and throw away every artifact already fetched. A consequence
to expect: a URL the host genuinely cannot serve now takes about ten seconds to
fail.

## `project check` reports the whole declaration problem, not the first one

A refused `umbraflow-project.json` now prints the complete member-set arithmetic
at the location the schema refused, with each missing member's own `$comment`
from the schema:

```text
umbraflow-project.json: schema/umbraflow-project-v3.schema.json refused
deployments[0]: additionalProperties carries 'plugin', which this closed object
does not declare

schema/umbraflow-project-v3.schema.json refused deployments[0]. Every member
problem at that one location follows, so the whole edit can be made at once.

deployments[0] must carry 4 member(s) it does not:

  observed_instance_identity_schemas
    The identity schema documents this deployment supplies for the observed
    instances it can publish, inline and named. ...

  ...

deployments[0] carries 5 member(s) this closed object does not declare:

  plugin
  reducer_closure
  tool_catalog
  project_state_schema
  baseline_event_type
```

It is an account of one refusal and never a verdict: validation still stops at
the first refusal, so another location may be refused once these are fixed.

## `project check` now applies two rules that used to wait for `umbra-flow open`

Both were previously load-time only, and both cost a real migration a second
round trip:

- **Every Tool name must sit under its deployment's `plugin_id`.** With
  `plugin_id` `chaos.dream`, a Tool named `chaos.get_current_event` is refused —
  `Tool name chaos.get_current_event is outside the namespace chaos.dream its
  registrant owns` — and must be `chaos.dream.get_current_event`.
- **The binding join.** Every `tool_bindings` row must name a Tool that `tools`
  declares and an entry that `tool_closure.exported_entry_points` states, and
  every stated export must be bound.

One rule is still load-time only: **a declared Tool with no binding**. `project
check` accepts it and `umbra-flow open` refuses it. Run
`umbra-flow open --project DIR` after `project check`.
