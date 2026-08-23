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
reads, so no document the kit accepts is one the loader refuses on shape. What
the kit does **not** do is compile the project's inline schemas, join a Tool
name against the closure entry that answers it, or derive the registration root
— all three are the loader's, and a build can faithfully record a declaration
whose semantic joins do not hold.

Consequently, unchanged Project Kit release identity after a declaration or
RuntimeArtifact change is not by itself evidence of a closure defect. A
declaration change is expected to move or invalidate `ProjectRegistration`; a
RuntimeArtifact change is expected to move or invalidate its own root.

## Fix

Run both layers of the consumer gate with executables from the same UmbraFlow
release:

```text
project init --source <root> --build <build>
project build --source <root> --build <build>
project check --source <root> --build <build>
project freeze --source <root> --build <build> --release <release-root>
project run --release <release-directory>
umbra-flow open --project <root>
```

Treat the reported Project Kit bundle root, production registration hash, and
RuntimeArtifact root as separate values. Do not fix this symptom by copying
project bytes or RuntimeArtifact bytes into the Project Kit release, and do not
treat `project run` as proof that the product can load.

## Regression check

A release-facing consumer test must include these independent cases:

1. The unmodified project passes `project check` and `umbra-flow open`, and open
   reports every production plugin registered.
2. After binding a Tool to a closure entry the closure does not export,
   `project check` may pass but `umbra-flow open` must fail on the export join.
3. After changing a RuntimeArtifact model or asset without regenerating its
   manifest, `umbra-flow open` must fail and name the mismatched file or row.
4. After valid regeneration, a registration member change moves the production
   registration hash, and a RuntimeArtifact change moves its root hash. Neither
   assertion should require the Project Kit bundle root to move unless that
   bundle's own declared inputs or generated artifacts changed.
