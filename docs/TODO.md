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
>   first this tree ever had. **Every G2/G4 tick predates seven Operator DDL
>   fingerprint changes** and has not been re-run against them. The fingerprint
>   is now `sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`
>   over 23 tables; an `operator-runtime.sqlite` from any earlier date is refused
>   at open and deleted, never migrated.
> - **The `linux-analysis` CI job does not compile**, under the project's own
>   `-Wunsafe-buffer-usage` and under clang-tidy with `WarningsAsErrors: '*'`.
>   That is W11 in [the next block](plans/2026-08-10-next-block.md) and it blocks
>   merging this branch regardless of anything below. This branch has also never
>   been pushed, so no tick below has been seen by CI at all.
> - **Both independent reviews returned FAIL, twice**, and the third adversarial
>   round returned FAIL with 17 findings. Every finding is now closed or accepted
>   with a stated reason —
>   [the review outcome](reviews/2026-08-10-runtime-hardening-review.md) and
>   [the third round](archive/reviews/2026-08-10-third-round-review.md) — but no
>   PASS verdict exists, which is what the two unticked review boxes in G0 wait
>   on. W9, the round over W2-W7, has never run.
>
> The requirement-by-requirement state is not here and never was: it is
> [the next block](plans/2026-08-10-next-block.md) §2, where all 42
> `REQUIRED_CORE` requirements are closed. What the W-series specifications
> still owe is [the carried debt ledger](plans/2026-08-12-carried-debt-ledger.md).

## The registration chain must be proven to have teeth

Opened 2026-08-11 by the independent review of the Q3 ruling in
[the boundary correction](plans/2026-08-11-project-as-data.md) §7.0. Under that
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
after [the boundary correction](plans/2026-08-11-project-as-data.md); that
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
      [the plan](plans/2026-08-11-project-as-data.md) had said
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
      [the plan](plans/2026-08-11-project-as-data.md) recorded as owed. Their
      deletion is still Q5's, unchanged.
- [x] `conformance/exemplars/*` become project **directories** under `examples/`.
      Data a project author copies, not C++ a consumer links.
      **Closed 2026-08-12, verified against the tree rather than against a
      commit message.** `conformance/exemplars/` does not exist, no `.cpp` or
      `.hpp` anywhere names `provideProject` or `ProvidedProject`, and the root
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
      are project directories in `docs/plans/2026-08-11-project-as-data.md`
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

## Delete-on-open has a deadline, and the deadline is C3

Opened 2026-08-12. [The correction record](archive/plans/2026-08-12-todo-correction-record.md)
— the amendments that stood at the top of this file until 2026-08-12 — records
the Operator DDL fingerprint moving repeatedly and states, in passing, that "an
operator database from any earlier date is refused at open and deleted, never
migrated".
That is history, and it is also a live decision with an expiry date that nothing
was tracking. This section is where the expiry lives; it owns nothing else.

- [ ] Before a mutation is delivered to a real target on a user's behalf, an
      Operator database written by an earlier build must stop being something a
      person deletes in order to get moving.

      **What is deleted today.** `OperatorCoordinator::open`
      (`modules/operator/source/operator/ledger.cpp`) refuses any database
      whose table set is not the 23 v1 tables, or whose canonicalized DDL text
      does not hash to `k_exactSchemaV1Fingerprint`, currently
      `sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`.
      The code refuses; it neither migrates nor deletes. Deleting the file is
      what a developer then does by hand, and what goes with it is not a cache:
      `journal_events`, `ledger_events`, `operations`, `operation_plans`,
      `operation_steps`, `dispatches`, `approvals`, `authority_decisions`,
      `control_transitions` and `reconciliations` are the record of what was
      proposed, authorized, delivered and reconciled — an Agent's journal, its
      operations and its audit chain.

      **Why that is acceptable now.** Nothing built here has acted on a real
      target on anyone's behalf. Every Operator database in existence was
      written by a test, a fixture, or a developer's own exploration, so the
      chain being discarded is an account nobody is owed. Under exactly that
      condition, refusing at the fingerprint is the better trade: a schema
      break is loud and immediate, instead of buying a migration path for
      records that have no subject.

      **When it stops being acceptable: C3.** On the consuming project's phase
      axis — `uf-chaos` §11, C0 extraction and projection, C1 content slice, C2
      read-only observation and state, **C3 the first mutation**, C4 expansion
      and Agent — C0 through C2 change nothing outside the project, so a
      database deleted at those levels still records nothing owed. C3 is the
      level at which a plan is approved, an action is delivered to a real
      client, and a Journal records that it happened on someone's behalf. From
      the first C3 run onward the audit chain is the account of what was done to
      a real target, and a product may not delete that on open — not to accept a
      schema change, and not for anything else. The upstream half of the same
      boundary is G4 First Mutation; see
      [the consumer attestation](plans/2026-08-11-consumer-attestation.md) §6.

      **This row does not design the answer, and does not assume it is
      migration.** Exporting the chain before refusing, a versioned read path, a
      refusal that leaves the file untouched and names where the records went —
      all are open. What this row fixes is the deadline: whoever takes a project
      into C3 owes a decision here first, and reaching C3 with delete-on-open
      still in force is the outcome this row exists to prevent.

## G0 — contract and inherited baseline

- [x] Pin the four-document consumer bundle at root
      `c8e559a1ee6618246778ac465842976b7445fbe10a20a2edaf77ca047ec6e5f0`
      (v1.13). Was `c4760bb5…bfb6a966` (v1.9), stale from v1.10 on 2026-08-12;
      v1.11 was never written down anywhere, and v1.12 preceded the final
      product/conformance archive correction.
- [x] Re-pin the gate at v1.12. Done 2026-08-12. `scripts/check_spec_bundle.py`'s
      `FROZEN_BUNDLE` and the five digest lines it cross-checks in
      [the hardening rewrite](plans/2026-08-09-runtime-hardening-rewrite.md):10-18
      both read v1.12 root `b3306dde…de51cda5`, and
      `python scripts/check_spec_bundle.py --pins-only` reports 5 pins matched
      with 6/6 self-test controls. Before this the two copies stated v1.10 root
      `adb7f29f…51049f` and agreed with each other, so `check-spec-pins` stayed
      green while asserting a bundle two versions behind. **Agreement between the
      two copies is still not freshness** — the gate detects an edited pin, not a
      moved bundle — which is recorded at
      [checks that cannot fail](pitfalls/checks-that-cannot-fail.md).
- [x] Re-pin the gate at v1.13. Done 2026-08-12 with the consumer archive move:
      the hardening rewrite and `FROZEN_BUNDLE` both read root
      `c8e559a1…ec6e5f0`, and the full bundle gate reads the consumer directory.
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
      [the next block](plans/2026-08-10-next-block.md), which has never run. This
      is why that review is the one document in `docs/reviews/` that stayed live
      through the 2026-08-12 archive pass: a live box turns on its verdicts.
- [x] Make active documentation point to this authority without copying old
      Context/Page/Target semantics. Done 2026-08-12, and what was checked is
      worth stating because "active documentation" is now a smaller set than it
      was. The five documents a reader meets first —
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
