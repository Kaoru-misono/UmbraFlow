# Project Kit is not production project admission

## Symptom

A consumer can run all of the following successfully:

```text
project init
project build
project check
project freeze
project run
```

and still have `umbra-flow open --project <root>` refuse the same project. A
second misleading symptom is that changing a declared project file or a
RuntimeArtifact file does not necessarily move the Project Kit frozen-release
id, which can look like an incomplete release closure.

## Root cause

Three different identities and validators are involved:

1. The Project Kit frozen release identifies generated Project Kit artifacts
   and the inputs relevant to those artifacts. `project run` verifies that
   immutable release; it is not a product runtime entry point.
2. The production project loader confinement-opens the project declaration, its
   modules and its resources, compiles every schema the declaration states
   inline, validates their semantic joins, and derives the
   `ProjectRegistration` identity.
3. The Host independently verifies the RuntimeArtifact manifest/file closure.
   Its root is pinned separately by `SessionManifest`.

The kit reads the same published `umbraflow-project.json` shape the loader
reads, so no document the kit accepts is one the loader refuses on shape.

Amended 2026-08-23: the kit now also applies the two joins that used to live
only in the loader, through the loader's own functions rather than a second
implementation of them — `operator_runtime::validateToolNameOwnership` for a
Tool name against the namespace its deployment's `plugin_id` owns, and
`operator_runtime::validateProjectToolBindings` for the binding table against
the declared Tool names and the closure's stated exports. Both were found the
hard way: a real migration passed `project check` and then met
`Tool name chaos.get_current_event is outside the namespace chaos.dream its
registrant owns` from `umbra-flow open`.

Amended 2026-08-25: `project check` and `project build` also run the trusted
RuntimeModel parse over the artifact the declaration names, through
`task::parseRuntimeArtifact` — the framework's one embedded `model.luau`,
reached by `project.model_semantics()`, which is the boot path minus the
binding. This too was found the hard way: a downstream project's
`runtime/artifact/runtime-model.toml` was missing two members of its `slots`
block, and `project check`, `project build` and `umbra-flow open` all exited 0
against it. All three refuse it now, in the parser's own words. The kit's
declaration therefore names an artifact that must be THERE and must PARSE; a
source tree that declares `runtime_artifact` and writes none is refused.

What the kit still does **not** do is compile the project's inline schemas,
derive the registration root, require every declared Tool to be bound, or
compile the closure and compare its actual exports against what the declaration
states — those are the loader's, and a build can faithfully record a
declaration whose remaining semantic joins do not hold. It also stops short of
the two joins a RuntimeModel binding makes — the parser's asset closure against
the artifact manifest, and the parser's format number against the artifact's —
because both live behind `finalizeRuntimeModel`, which only a sealed generation
reaches.

The unbound-Tool rule is the one deliberately left out rather than merely
unimplemented. The framework's own `project init --plugin generated` scaffold
declares a Tool and binds none, because the declarative generator has no
renderer for a bound entry; adding the rule to `project check` would refuse the
starter this repository ships. Fix the generator and the scaffold before moving
the rule.

Consequently, unchanged Project Kit release identity after a declaration or
RuntimeArtifact change is not by itself evidence of a closure defect. A
declaration change is expected to move or invalidate `ProjectRegistration`; a
RuntimeArtifact change is expected to move or invalidate its own root.

## Fix

Run both layers of the consumer gate with executables from the same UmbraFlow
release:

```text
project upgrade --source <root>
project build --source <root> --build <build>
project check --source <root> --build <build>
project freeze --source <root> --build <build> --release <release-root>
project run --release <release-directory>
umbra-flow open --project <root>
```

`project upgrade` is what puts a matched set of executables under
`<root>/umbraflow-bin`, and it ends by running the newly installed binary's own
`check`, so the first two lines above are one command when the release is being
changed. `project init` no longer acquires anything: it makes a directory a
project and nothing else.

Treat the reported Project Kit bundle root, production registration hash, and
RuntimeArtifact root as separate values. Do not fix this symptom by copying
project bytes or RuntimeArtifact bytes into the Project Kit release, and do not
treat `project run` as proof that the product can load.

## Regression check

A release-facing consumer test must include these independent cases:

1. The unmodified project passes `project check` and `umbra-flow open`, and open
   reports every production plugin registered.
2. After binding a Tool to a closure entry the closure does not export,
   `project check` must fail on the export join — as of 2026-08-23 it applies
   the loader's own function — and `umbra-flow open` must fail the same way.
   After declaring a Tool nothing binds, `project check` still passes and
   `umbra-flow open` must fail.
3. After changing a RuntimeArtifact model or asset without regenerating its
   manifest, `umbra-flow open` must fail and name the mismatched file or row.
   After changing the model to one the trusted parser refuses — and
   regenerating the manifest so the closure is intact — `project check`,
   `project build` and `umbra-flow open` must all fail with the SAME sentence,
   which is the parser's.
4. After valid regeneration, a registration member change moves the production
   registration hash, and a RuntimeArtifact change moves its root hash. Neither
   assertion should require the Project Kit bundle root to move unless that
   bundle's own declared inputs or generated artifacts changed.
