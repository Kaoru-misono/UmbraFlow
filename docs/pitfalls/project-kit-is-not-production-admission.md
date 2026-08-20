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
second misleading symptom is that changing a declared project schema or a
RuntimeArtifact file does not necessarily move the Project Kit frozen-release
id, which can look like an incomplete release closure.

One concrete case changed only comments in journal payload schemas. `project
build` recorded the changed schema bytes and the unchanged journal schema
manifest, and `project check` accepted that record. Production open correctly
refused because the manifest still named the old payload-schema SHA-256.

## Root cause

Three different identities and validators are involved:

1. The Project Kit frozen release identifies generated Project Kit artifacts
   and the inputs relevant to those artifacts. `project run` verifies that
   immutable release; it is not a product runtime entry point.
2. The production project loader confinement-opens the project declaration,
   schemas, schema manifests, modules, and resources, validates their semantic
   joins, and derives the `ProjectRegistration` identity.
3. The Host independently verifies the RuntimeArtifact manifest/file closure.
   Its root is pinned separately by `SessionManifest`.

The declared-file record written by `project build` lets `project check` detect
that a recorded source file moved after the build. It does not turn Project Kit
checking into the production loader, and it cannot prove relationships such as
a journal manifest's payload digest matching the schema bytes it names. A
fresh build can faithfully record two mutually inconsistent authored files.

Consequently, unchanged Project Kit release identity after a schema or
RuntimeArtifact change is not by itself evidence of a closure defect. A schema
change is expected to move or invalidate `ProjectRegistration`; a
RuntimeArtifact change is expected to move or invalidate its own root.

## Fix

Regenerate every authored manifest that pins changed exact bytes through its
owning writer, then run both layers of the consumer gate with executables from
the same UmbraFlow release:

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
project schemas or RuntimeArtifact bytes into the Project Kit release, and do
not treat `project run` as proof that the product can load.

## Regression check

A release-facing consumer test must include these independent cases:

1. The unmodified project passes `project check` and `umbra-flow open`, and open
   reports every production plugin registered.
2. After changing a journal payload schema and rebuilding without regenerating
   its journal schema manifest, `project check` may pass but `umbra-flow open`
   must fail on the schema-manifest digest join.
3. After changing a RuntimeArtifact model or asset without regenerating its
   manifest, `umbra-flow open` must fail and name the mismatched file or row.
4. After valid regeneration, a registration member change moves the production
   registration hash, and a RuntimeArtifact change moves its root hash. Neither
   assertion should require the Project Kit bundle root to move unless that
   bundle's own declared inputs or generated artifacts changed.
