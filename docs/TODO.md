# Umbraflow upstream execution checklist

> Amended 2026-08-09: this checklist is derived from the
> [v1.7 breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md)
> and replaces the former consumer-project queue. Work in this repository does
> not modify a game project.
>
> Amended 2026-08-10: boxes below are ticked against a build and a test run
> that actually happened -- the tree had never been compiled before this date.
> Both independent reviews returned FAIL; their closed and open findings are in
> [the review outcome](reviews/2026-08-10-runtime-hardening-review.md), which is
> what the two unticked review boxes now wait on.
>
> Amended 2026-08-10, later the same day: this checklist covers the rewrite, not
> what follows it. The remaining work is enumerated per requirement in
> [the next block](plans/2026-08-10-next-block.md), of which W1 (behavioural
> cases for `c02 c04 s03 s06 a04 a06`), W5 (Replay Bundle and both publication
> gates), W8 (runtime-artifact reclamation by database refcount, which closed
> A-F8) and the exported conformance suite landed on this date. Two consequences
> reach the boxes below: the Operator ledger DDL fingerprint changed, so an
> operator database written before this date is refused at open and recreated
> rather than migrated; and `ctest -N` now also lists
> `conformance-umbraflow` and `conformance-arcana`. Every tick below
> predates those changes and has not been re-run against them.
>
> The same day, W0's merge-readiness run found that the ticked Windows gate is
> not the whole picture. The three sanitizer presets pass 62/62 with the
> sanitizers proved active, but the required `linux-analysis` CI job does not
> compile: `-Wunsafe-buffer-usage` and clang-tidy under `WarningsAsErrors: '*'`
> produce well over a hundred fatal diagnostics. That is tracked as W11 in
> [the next block](plans/2026-08-10-next-block.md) and blocks merging this
> branch regardless of anything below.
>
> Amended 2026-08-11: W3 (`4b955de`) and W2 (`848e390`) landed, closing `s01`,
> `s02`, `s04`, `c05` and `c08`. Three consequences reach the boxes below. The
> gate map moved again — 52 gates over 42 requirements, 33 `contract-*` and 19
> `schema-*` — and the migration report drifted a second time for the same
> reason as R3-F3, because both landings registered their cases before updating
> it; it has been repaired. The Operator DDL fingerprint moved twice more, to
> `sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4`
> over 20 tables, and `848e390` deleted the `runtime_publications` table that
> R3-F2 found unobservable, so the G2/G4 ticks predate a third schema break.
> And the block's mutations were executed for the first time: three of W2's
> stay green and are unresolved defects in the tests, carried in
> [the next block](plans/2026-08-10-next-block.md) §2 and in
> [checks that cannot fail](pitfalls/checks-that-cannot-fail.md).
>
> Amended 2026-08-11, after the block closed. W4 (`e64c143`, `25f57f9`), W6
> (`93698b4`) and W7 (`c23efd3`) landed, closing `c03`, `p01`, `p02`,
> `p03`, `a01` and `a02` — every `REQUIRED_CORE` requirement is now implemented.
> Four consequences reach the boxes below. The gate map is **59 gates over 42
> requirements, 40 `contract-*` and 19 `schema-*`**, and a green run is 83
> registered tests. `a03` and `a05` are the only requirements without a
> per-requirement behavioural ID; both are implemented and both run under the
> aggregate `test-annotate-backend`, so that is a naming gap and not a coverage
> gap. The Operator DDL fingerprint moved four more times and is now
> `sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`
> over 23 tables, so every G2/G4 tick predates seven schema breaks; an operator
> database from any earlier date is refused at open and deleted, never migrated.
> And the migration report drifted a third, fourth and fifth time for the same
> structural reason — no gate reads it — which is now recorded in the report
> itself.
>
> Corrected 2026-08-11 (`07abc3e`), on two counts this paragraph got wrong.
> **`a07` was listed as closed by W4 and it is not**; it is reopened. Its
> acceptance text has two clauses, `contract-agent-a07` proves the second, and
> the first — that a human takeover and Host delivery share one target
> serialization — is implemented by nothing: the takeover transaction and
> `TaskHost`'s fence have no call edge between them in production or in test.
> **39 of 42 requirements are closed by a behavioural gate, not 40.** The gate
> map above is unchanged and correct: 40 `contract-*` gates are registered and
> green, and one of them closes half a requirement. The reopened row and the two
> ways it closes are in [the next block](plans/2026-08-10-next-block.md) §2. The
> fingerprint above also read `bda31e4b18…` and was a seventh break out of date:
> `07abc3e` renamed eight DDL columns to `controlled_target_id`, the only
> spelling now, which changes the canonicalized DDL text without changing a table
> name — hence the same 23 tables.
>
> Corrected 2026-08-11, once more: the fingerprint above also read
> `be80aca714…` before this block. Four DDL columns were renamed so that
> `journal_events` and `project_state` — both journal records — carry
> `$defs.JournalEvent` and `$defs.ProjectState`'s member names:
> `canonical_event` to `opaque_project_payload`, `canonical_provenance` to
> `provenance`, `state_schema_hash` to `project_state_schema_hash` and
> `canonical_state` to `canonical_opaque_payload`. No table added or dropped,
> hence the same 23 tables. See
> [journal record binding](plans/2026-08-11-journal-record-binding.md).
>
> Amended 2026-08-10, after the third adversarial round. It returned FAIL with
> 17 findings — [the record](reviews/2026-08-10-third-round-review.md) — so the
> two unticked review boxes below wait on it too. Three corrections to the
> paragraphs above. The G0 tick "map every requirement to an exact CTest ID"
> holds again only as of today's repair: `dcc43b5` renamed 14 gate IDs to
> `schema-*` without updating the migration report, which then named CTests
> `ctest -N` cannot produce; the report now carries all 47 gates — 28
> `contract-*` and 19 `schema-*` — over 42 requirements. `ctest -N` gained four
> `CONFORMANCE` aggregates rather than two: `test-contract-operator` and
> `test-contract-runtime` as well as `conformance-umbraflow` and
> `conformance-arcana`. And W11 is a scope rather than a count — "well over a
> hundred" was W0's reading before `603b0b0`, `cec8898` and `6f8d3a8` cleared
> everything outside `modules/operator`, `conformance` and `tests/operator`.

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

## Build-system shape — owed, ordered

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
- [ ] `conformance/` becomes `modules/conformance/`, `type = static`, its
      `include/` collapsed into `source/conformance/` like every other module.
      Waits on the boundary, which decides whether it is a library or a binary;
      moving first means moving twice.
- [ ] `conformance/exemplars/*` become project **directories** under `examples/`.
      Data a project author copies, not C++ a consumer links.
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
- [ ] `scripts/check_modules.py` drops `DECLARED_SOURCE_TREES` and returns to a
      single root once nothing carries a manifest outside `modules/`.
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

## G0 — contract and inherited baseline

- [x] Pin the four-document consumer bundle at root
      `c4760bb59e7df28e13a676446a4cfbb4a62b067741420ecf13f4b939bfb6a966`.
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
      The box turns on the re-review verdicts.
- [ ] Make active documentation point to this authority without copying old
      Context/Page/Target semantics.

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
