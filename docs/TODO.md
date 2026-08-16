# Umbraflow upstream execution checklist

> **The record of corrections to the boxes below moved to
> [the correction record](archive/plans/2026-08-12-todo-correction-record.md) on
> 2026-08-12.** It was 118 lines of dated amendments — none of them opening work
> — standing between this heading and the first thing still owed. Read it for
> how a tick came to be true, or was corrected, or was corrected back.
>
> **Three facts from it qualify every tick below and are therefore restated
> here, not moved:**
>
> - The boxes were ticked against a build and a test run on 2026-08-10, the
>   first this tree ever had. **Every G2/G4 tick predates later Operator DDL
>   changes** and has not been re-run against them. Schema identity lives only
>   in `modules/operator/source/operator/ledger.cpp`. Exact-pair migrations now
>   preserve recognised older audit chains; unregistered identities are
>   refused without replacement. The current ruling is below.
> - **The `linux-analysis` CI job does not compile**, under the project's own
>   `-Wunsafe-buffer-usage` and under clang-tidy with `WarningsAsErrors: '*'`.
>   That is now tracked as O-002 in the consumer repository's
>   `docs/architecture/parallel-implementation-plan.md` §2.1 (gate in §7)
>   and it blocks merging this branch regardless of anything below. The current
>   local head is ahead of its remote branch, so the unpublished commits have not
>   been seen by CI.
> - **Both independent reviews returned FAIL, twice**, and the third adversarial
>   round returned FAIL with 17 findings. Every finding is now closed or accepted
>   with a stated reason —
>   [the review outcome](reviews/2026-08-10-runtime-hardening-review.md) and
>   [the third round](archive/reviews/2026-08-10-third-round-review.md) — but no
>   PASS verdict exists, which is what the two unticked review boxes in G0 wait
>   on. W9, the round over W2-W7, has never run.
>
> The requirement-by-requirement state is not here and never was: it is
> [the archived next-block record](archive/plans/2026-08-10-next-block.md) §2,
> where all 42
> `REQUIRED_CORE` requirements are closed. What the W-series specifications
> still owe is tracked by O-101 through O-122, absorbed into workflows U2, U3
> and U8-U12 in §3 of the consumer repository's
> `docs/architecture/parallel-implementation-plan.md`.

## The registration chain must be proven to have teeth

Opened 2026-08-11 by the independent review of the Q3 ruling in
[the boundary correction](archive/plans/2026-08-11-project-as-data.md) §7.0. Under that
ruling the loader computes every digest, so **every comparison inside a single
load is between two quantities the loader itself produced** — structurally
incapable of failing, deliberately, and named as such. Exactly one check on this
chain compares values from two different times, and the whole ruling rests on
it.

- [x] A mutation case in CI: against a stored session naming
      `project_registration_hash` `H`, flip one byte of a pinned file in the
      project directory, reload and resume, and require the refusal to fire and
      to print both hashes. **If it does not go red, there is no real check
      anywhere on this chain** and the ruling is unfounded rather than merely
      untested. Lands with step 3 of §5, not after it.

      Landed with step 3, in `tests/deployment/test-project-directory.cpp`, "a
      project whose bytes moved under a stored session is refused". It goes
      red: removing the comparison in `commitmentFor` leaves the case green,
      measured on a mirrored tree. The flipped byte is in the plugin, whose
      bytes reach `project_registration_hash` through `plugin_hash` and nothing
      else, and the case reloads the flipped directory a third time with no
      commitment and requires that load to succeed — so no second mechanism can
      stand in for the check. **The ruling is founded.**

      One half is owed by a module this step did not own. No refusal in
      `modules/operator` prints either hash — `manifest.cpp:306-309`,
      `ledger.cpp:2815-2820` and `ledger.cpp:2891-2894` are bare literals — so
      the loader's own refusal is the only one on the chain that says what
      moved. Whoever owns `modules/operator` should decide whether those three
      grow the two hashes they already hold.

      Closed 2026-08-11 by the owner of `modules/operator`. Two of the three
      already held both values at the point they refuse, the same shape as the
      loader's check, and now print them: `manifest.cpp:304-315`
      (`verifyExact`) names `expected` and `computed`; `ledger.cpp:2815-2826`
      (`pinSession`'s registration-agreement check) names what the manifest
      and the pin each name. The third, `ledger.cpp:2892-2912`, is not that
      shape: it is an existence query on `(project_registration_hash,
      project_instance_key)`, not a same-identity comparison, so there is no
      second, "actual" hash already in hand to print beside the first — the
      table's natural key also needs `plugin_id`, which `SessionPin` does not
      carry, so fetching one would be a lookup this refusal does not otherwise
      owe. It now prints the pair it searched for instead of staying a bare
      literal, without pretending that pair is a before/after comparison.
      Covered by three cases in `tests/operator/test-manifest.cpp` and
      `tests/operator/test-ledger.cpp` asserting on message content, each
      falsified by reverting its message to the old bare literal and
      confirming the message-content assertions — and only those — go red.
- [x] At every `verifyExact` call site, say in a sentence whether
      `expectedRootHash` came from another time or another source — the ledger,
      or a signature — or is the value just computed. The second case is legal
      and empty, and the reader must not have to work out which one they are
      looking at.

      Done. The loader's call site
      (`modules/deployment/source/deployment/project-directory.cpp`) says it is
      the empty case and why it is always the empty case: the recorded hash is
      compared one line earlier, where both values can be named. Every other
      call site in the tree is a test fixture or a conformance provider that
      passes `hashOf` of the bytes it just assembled, and those die with step 6.

## Build-system shape — closed 2026-08-12

Raised by the owner on 2026-08-11: a manifest is how a target is declared, so
everything carrying one belongs under `modules/`, examples belong in a directory
of their own, and `cli` was never made a module at all. All three still stand
after [the boundary correction](archive/plans/2026-08-11-project-as-data.md); that
document makes them simpler rather than moot, because a `sources` module kind, a
nameable doctest target, `EXCLUDE_FROM_ALL` for consumers and a portable
`UF_FRAMEWORK_ROOT` default were all symptoms of a consumer compiling this
source tree, and no consumer does that any more.

Amended 2026-08-11: the `cli` row and the uncommitted-cmake row below are
closed. `conformance/` is the only manifest left outside `modules/`, so the
`DECLARED_SOURCE_TREES` row still waits on the boundary rather than on this
work. One new row was opened by the falsification the `cli` move was asked for.

Amended 2026-08-12: the `conformance` row and the `DECLARED_SOURCE_TREES` row
are closed together, because the second existed only for the first. Nothing
carries a manifest outside `modules/` now, and the two shipped binaries have one
shape: `entry/<name>/main.cpp` plus `modules/<name>`, `type = static`.

Closed 2026-08-12, later the same day: the last open row, `conformance/exemplars/*`
becoming project directories, was found already done in the tree. **Every row in
this section is ticked**, and the section is kept because six of the seven carry
a falsification whose result is not obvious from the change — most sharply the
`[sources.other]` row, whose restoration half is enforced by no gate this host
can run. Nothing here is owed.

- [x] `entry/cli/` becomes `modules/cli/`, `type = static`. Done 2026-08-11.
      `entry/` keeps `main.cpp`, the generated `application-info.hpp` and the OCR
      payload staging, and links `uf::cli`; there is no alias for
      `${PROJECT_NAME}_cli_support`. The `"../candidate-selection.hpp"`
      traversal and the `<args.hpp>` that only resolved because `entry/cli` was
      published as an include root are both gone. Includes follow the documented
      standard like every other module: `cpp-coding` §Includes rule 2, quotes
      for a header of one's own module, angle brackets from outside it — so
      `entry/cli/main.cpp` and `tests/cli` reach `cli` as `<cli/…>` while the
      module's own sources use quotes. *(Corrected 2026-08-11: this row first
      claimed every first-party include became `<cli/…>` including inside the
      module. That was an instruction of mine and it was wrong — it made `cli`
      the only module of twelve spelling its own headers with angle brackets,
      which is the second spelling this repository forbids, and amending the
      standard instead would have meant editing eleven modules to save one.
      Reverted across 19 files and 33 include lines, gate re-run.)*
      **The invisibility claim was checked rather than assumed**:
      on manifest-only replicas of the graph, giving `engine` a dependency back
      on `cli` leaves `scripts/check_modules.py` green without the module and
      turns it red with it — `module dependency cycle: cli -> engine -> cli`.
      The move also carried `tests/test-runtime-surface.py`, whose
      `RETIRED_PATHS`, `REQUIRED_SAFE_PATHS` and executable-text scan all named
      `entry/cli/`; the scan now covers `entry/` and `modules/cli/` both, so a
      retired source cannot come back by being written to the other one.
- [x] `conformance/` becomes `modules/conformance/`, `type = static`, its
      `include/` collapsed into `source/conformance/` like every other module.
      Done 2026-08-12. **The reason in the row this replaced was wrong by the
      time it landed.** It said the move was owed because `conformance/` was an
      exported source library a consumer compiled; the boundary correction
      killed that — nothing links it and no consumer compiles it. The reason it
      moved is that it is the logic half of a shipped binary, and this
      repository has one shape for those: `umbra-flow` is `entry/cli/main.cpp`
      plus `modules/cli`, so `umbra-flow-conformance` is `entry/conformance/main.cpp`
      plus `modules/conformance`. §4.3 of
      [the plan](archive/plans/2026-08-11-project-as-data.md) had said
      `entry/conformance/main.cpp` and step 6 placed it elsewhere for scope; this
      closes that gap.
      What moved with it: `runSuite` left the anonymous namespace for
      `conformance/suite-run.hpp` so `entry/` has only `main`; the suite's five
      `<conformance/…>` self-includes became quoted, because the autoloader
      publishes `source/` and `source/conformance/` and a module spells its own
      headers with quotes; `cmake/doctest-gate.cmake` is now included from the
      root before the autoloader, because the manifest names `uf::doctest` and a
      manifest dependency must be a target by the link pass; and the two runs
      moved from the deleted `conformance/CMakeLists.txt` into the root's
      `PROJECT_IS_TOP_LEVEL` guard, reading their declared cases off the module
      target's `SOURCES` rather than a hand-written list the glob could
      contradict.
      **`type = static` needs `WHOLE_ARCHIVE` at the executable, and this was
      measured rather than assumed.** A `TEST_CASE` registers from the dynamic
      initializer of an object nothing names, so the linker pulls no member for
      it: with a plain `uf::conformance` on the link line the binary links,
      runs, reports `test cases: 0`, and **exits 0**. `uf_require_executed_assertions`
      catches it — `conformance-umbraflow` goes red on `assertions: 0` — but the
      binary alone does not say so.
- [x] The three C++ fixtures move to `tests/support/`. Done 2026-08-12.
      `project-fixture.hpp` and both `project-schemas.hpp` are included by
      `tests/operator` and `tests/deployment` and by nothing in the suite, so
      they were never the suite's; they are now `tests/support/umbraflow/` and
      `tests/support/arcana-expedition/`, which leaves every
      `#include "project-fixture.hpp"` and `#include "umbraflow/project-schemas.hpp"`
      spelled the same way. This is what §4.2 of
      [the plan](archive/plans/2026-08-11-project-as-data.md) recorded as owed. Their
      deletion is still Q5's, unchanged.
- [x] `conformance/exemplars/*` become project **directories** under `examples/`.
      Data a project author copies, not C++ a consumer links.
      **Closed 2026-08-12, verified against the tree rather than against a
      commit message.** `conformance/exemplars/` does not exist, the retired
      provider seam is absent from C++, and the root
      `CMakeLists.txt` registers both runs by directory —
      `uf_add_conformance_run(PROJECT umbraflow DIRECTORY .../examples/umbraflow)`
      and the same for `arcana-expedition`, the second one being the whole of
      what a consuming repository writes. That is §5 step 6, which the row below
      said was still owed. The three C++ fixtures that were mixed in with the
      exemplars are not part of this and moved separately, to `tests/support/`.
      What follows is the pre-close record and is left as written.
      **The data half landed 2026-08-11.** `examples/umbraflow/` (deployments
      `alpha` and `foreign`, no artifact root) and `examples/arcana-expedition/`
      (deployments `expedition` and `rival`, one artifact root over one blob)
      are project directories in `docs/archive/plans/2026-08-11-project-as-data.md`
      §2's format, and `deployment::loadProject` accepts both. Every value in
      them was written out of the exemplar C++ that assembles the same values
      today; nothing reads them, `conformance/exemplars/` is untouched, and §5
      step 6 is the change that switches the suite over and deletes the C++.
      What the writing measured, and what the switch therefore owes:
    - §2.2's `effect_payload_sha256s` **landed 2026-08-11**, one file larger than
      §2.2 had counted: `k_toolCatalogSchema`
      (`modules/deployment/source/deployment/project-deployment.cpp`) is a
      closed object, so it had to declare the member and list it in `required`
      in the same change as `ProjectDeployment::create`'s set check, the four
      catalogs under `examples/*/schema/*/tool-catalog-v1.json`, the two
      generators at `conformance/exemplars/*/project-schemas.hpp`, and
      uf-chaos's two authored catalogs at
      `schema/{dream,archive}/umbraflow-tool-catalog-v1.json`. An effect payload
      schema's bytes now reach `tool_catalog_hash` and so
      `project_registration_hash`; both of uf-chaos's registration hashes moved
      (`d5d2246…` to `1bee72c…`, `59cf758…` to `9e7f828…`) and no stored
      session recorded either.
    - **Both example artifact roots now carry their
      `runtime-artifact.manifest.json`**, and the collision that kept them out
      was ruled rather than worked around on 2026-08-11: `fix_format.py` does
      not own files whose bytes a digest pins, implemented by the marker — it
      excludes any directory holding `umbraflow-project.json` at its root, and
      everything under it (§2.5). Its file count went 524 to 484, which is
      exactly the 40 files the two example project directories contributed.
      `task::loadRuntimeArtifact` accepts each manifest and still refuses the
      same bytes with one trailing newline, which is the byte the normalizer
      would have added.
    - Every digest a project-authored document states about a file beside it is
      the sha256 of the **file**, and the exemplar C++ computes the same digest
      over a string constant that carries no trailing newline. The three
      manifests and the plugin's own `payload_schema_hash` are the C++'s bytes
      with exactly those digests restated; `plugin_id`, `baseline_event_type`
      and the artifact roots are byte-identical to the C++ side.
- [x] `scripts/check_modules.py` drops `DECLARED_SOURCE_TREES` and returns to a
      single root once nothing carries a manifest outside `modules/`. Done
      2026-08-12 with the row above, which is what put the last one inside. The
      `autoloaded` flag on `Module` went with it, and the two layouts it
      selected between became one: every manifest now owes `source/<directory>`.
      The graph did not shrink — it reads `15 modules under modules/` where it
      read `15 manifests: 14 modules under modules/, 1 outside it`, and the node
      is load-bearing either way: on a manifest-only replica, giving `operator` a
      dependency back on `conformance` is green with the node removed (14
      modules) and red with it present — `module dependency cycle: deployment ->
      operator -> conformance -> deployment`.
      Amended 2026-08-13: the graph now reads 17 modules. The post-measurement
      [offline Project Kit](../modules/project/source/project/project-kit.hpp)
      and
      [generated framework schema catalog](../modules/schema/source/schema/framework-schema-catalog.hpp)
      are separate modules; the 15-module statement above remains the
      measurement of the 2026-08-12 move.
      `scripts/check_cpp_format.py` and `scripts/check_safety.py` dropped
      `"conformance"` from `SOURCE_ROOTS` in the same change, for the same
      reason: it named a directory that no longer exists. Coverage rose rather
      than fell — 302 files to 304, which is exactly the two files the move
      added.
- [x] Decide the fate of the uncommitted `cmake/manifest.cmake` and
      `cmake/build.cmake` work. Settled 2026-08-11. The `[sources.*]` platform
      grammar stays and the `cli` row above is its first user; the promotion of
      an unknown module type from `message(WARNING)` to `FATAL_ERROR` stays; the
      `sources` module kind is gone, along with the INTERFACE-target branches in
      `cpp_define_module` and `cpp_link_module` that only it reached.
      `conformance/manifest.txt` drops `[module].type` entirely rather than
      naming another kind: nothing about that file reaches CMake, so a type
      there is a declaration nothing can honour. `scripts/check_modules.py`
      passes over it unchanged, because it never read the field.
- [x] `[sources.other]` cannot fail on the platform that has a section of its
      own. Found 2026-08-11 while falsifying the grammar. Deleting
      `[sources.other]` from `modules/cli/manifest.txt` does put
      `explore-unsupported.cpp` and `targets-unsupported.cpp` back into the
      module's compiled file set — the target's source list shows both — and the
      Windows build stays **green**, links, and behaves identically, because a
      static library only contributes the members needed to resolve a symbol and
      the duplicate definitions sit in members nothing pulls. Which definition
      wins is the linker's archive search order, not something the manifest
      states. The mirror-image mutation is red: reassigning
      `targets-windows.cpp` to `[sources.linux]` removes it from the Windows
      build and `umbra-flow` fails to link on `unresolved external symbol
      uf::cli::targetsProduct`. So the removal half of the grammar is enforced
      and the restoration half is not, on any platform that has its own section.
      Ruled 2026-08-11: the property under test is "this translation unit
      compiles only on that platform", and the only honest verification of that
      is compiling it on that platform, so the Linux and macOS CI jobs are the
      gate — nothing on the Windows side is built to detect this mutation.
      Those jobs are currently blocked by the repository's CI billing state, so
      the gate exists but has not run.

## Operator schema migration

> Amended 2026-08-13: the 2026-08-12 delete-on-open deadline is closed by
> `O-007`. `OperatorCoordinator::open` now applies only migrations registered
> for an exact source/target schema-identity pair, verifies the exact target
> before commit, records the transition, and otherwise refuses without a
> fallback or replacement. The read-only door never migrates. The sole
> authoritative identity value remains in
> `modules/operator/source/operator/ledger.cpp`; this document intentionally
> carries no copied fingerprint or table count.
>
> The falsification evidence remains in `tests/operator/test-ledger.cpp`: a
> registered source migrates and preserves audit rows, an unregistered source
> is refused unchanged, the read-only door refuses rather than migrating, and a
> committed migration reopens under the exact target identity. The historical
> deadline and its earlier refusal-only rationale remain in the dated
> [correction record](archive/plans/2026-08-12-todo-correction-record.md).

- [x] Replace manual delete-and-recreate handling before the first real C3
      mutation. Done 2026-08-13 by exact-pair, audit-preserving migration with
      fail-closed refusal for every unregistered source.

## G0 — contract and inherited baseline

- [x] Pin and verify the committed five-document consumer bundle. Revalidated
      2026-08-13 at v1.18; the implementation plan remains deliberately outside
      the bundle. Earlier pin history is retained in the archived
      cross-repository audit.

      **Superseded 2026-08-16: the exact-byte root pin is removed and this box
      no longer describes a live mechanism.** It stays ticked because the work
      it names was done; what replaced it is a semantic contract version plus
      the consumer's own interface-lock manifest over the bytes code actually
      consumes. The row's own gate, `check-spec-bundle`, carried the `CI` label
      and located the bundle by parsing an absolute path out of a sentence, so
      this repository's `GATE: PASS` depended on a second repository existing at
      one path on one machine. Ruling in the
      [rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md).
- [x] Record base commit, rejected stash and the 101-entry dirty baseline
      manifest.
- [x] Assign exactly one disposition to all 101 inherited dirty paths
      (67 REWRITE, 34 DELETE).
- [x] Resolve post-dispatch approval recovery, exact registration/session root
      bytes, Operator-owned policy and consumer-example scope in the upstream
      authority.
- [x] Map every P/D/U/S/C/A requirement to an owner, schema location and exact
      CTest ID.
- [ ] Obtain final state/persistence and plugin-boundary review PASS on the
      upstream execution profile. Both ran on 2026-08-10, returned FAIL, and
      every finding is now closed or accepted with a reason; see
      [the review outcome](reviews/2026-08-10-runtime-hardening-review.md).
      The box turns on the re-review verdicts. **No PASS verdict exists anywhere
      in the tree**, and the round that would produce one over W2-W7 is W9 in
      O-003 in the consumer repository's
      `docs/architecture/parallel-implementation-plan.md` §2.1 (gate in §7),
      which has never run. This
      is why that review is the one document in `docs/reviews/` that stayed live
      through the 2026-08-12 archive pass: a live box turns on its verdicts.
- [x] Make active documentation point to this authority without copying old
      Context/Page/Target semantics. Done 2026-08-12, and what was checked is
      worth stating because "active documentation" is now a smaller set than it
      was. The first-screen authority set —
      [`docs/INDEX.md`](INDEX.md), [`docs/ARCHITECTURE.md`](ARCHITECTURE.md),
      this file, [`docs/plans/README.md`](plans/README.md) and `CONTEXT.md` —
      each reach
      [the rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md) in
      their first screen and none teaches Context, Page, Element, Hit or UFR.
      The retained predecessor plans still use that vocabulary and are kept for
      it; every one of them carries a dated supersession note, and
      `docs/plans/README.md` says for each what survives and what does not.
      Nine documents that taught it as current left the live set the same day.

## G1 — remove unsafe entrances and restore primitives

- [x] Delete old check/run/replay production CLI and file-frame projection.
- [x] Delete v1/UFR/envelope schemas, old runtime globals and compatibility
      adapters.
- [x] Keep and revalidate IDs, capture, cycle ledger, target generation,
      controller input/DPI, VM limits, TaskHost lifecycle and generic Trace.
- [x] Restore reduced regression assertions and register every planned contract
      in CTest before claiming coverage.
- [x] Register `check-repository-surface` to reject retired files,
      commands, globals, parser duplication, consumer symbols and unconsumed
      Receipt proof fields.

## G2 — RuntimeArtifact and Runtime v2

- [x] Add exact RuntimeArtifact JCS schema, confinement-first verifier and
      generation-owned private finalize binding.
- [x] Implement one trusted Luau RuntimeModel parser.
- [x] Make UiTarget semantic-only and Binding the sole placement/variant owner.
- [x] Implement StateResolution then same-cycle BindingResolution.
- [x] Mint Host-owned expiring one-shot Receipt; keep production input closed.
- [x] Pass `contract-runtime-u01` through `contract-runtime-u08`.

## G3 — offline authoring

- [x] Implement minimal `annotation-workspace.sqlite` candidate, decision and
      replay ledger.
- [x] Publish immutable asset-complete releases; authoring publication is not
      production activation.
- [x] Prove production cannot read authoring DB/workspace/blob/replay roots.

## G4 — Operator and two plugin fixtures

- [x] Add minimal `modules/operator` and
      `operator-runtime.sqlite`.
- [x] Implement exact ProjectRegistrationManifest and SessionManifest JCS roots.
- [x] Implement lease/fence, opaque snapshots, idempotent commands, EffectivePlan,
      single-use approval, Operation transitions and reconciliation transaction.
- [x] Run one conformance suite over two structurally different fixture plugins;
      core must contain no game symbol or branch. Since 2026-08-10 that suite is
      also the one a consumer runs: `cmake/conformance-suite.cmake` exports
      `uf_add_conformance_suite()` and the two exemplars under
      `conformance/exemplars/` reach it exactly as an outside repository would.
- [x] Pass all local `contract-product-*`,
      `contract-state-*`, `contract-control-*` and `contract-agent-*`.

## Final gate

- [x] `ctest -N` lists every migration-report verification marked `CTEST`.
- [x] Focused security, crash, path-confinement and capability attacks pass.
      Path confinement is handle-based since 2026-08-10: reads and staging
      writes resolve once, ancestors are held, and a reparse point is refused
      by attribute rather than by tag.
- [x] The repository build skill completes `scripts/ci-local.ps1` with
      `GATE: PASS`.
- [x] Independent review finds no old reader, direct action path, consumer
      schema or fake-green unregistered test. None of the first three were
      found; seven fake-green tests were, and each replacement was falsified by
      removing the protection it names.

The real `uf-chaos + second game` attestation remains external and
`NOT_RUN`. It is not part of this upstream-only worktree and cannot be
replaced by fixtures; it gates consumer production mutation later.
