# Annotation System Parallel Work Breakdown

Status: execution plan

Date: 2026-08-09

Related design:

- 2026-08-09-runtime-annotation-and-agent-model.md

## 1. Purpose

This document breaks the annotation-system redesign into work packages that
can be assigned to multiple Agents in parallel.

The objective is not to maximize the number of Agents. The objective is to
maximize independent progress without allowing different Agents to invent
incompatible models or edit the same files.

The execution rule is:

~~~text
freeze the contract first
        ↓
parallel work on disjoint write sets
        ↓
integration gate
        ↓
parallel project migration
        ↓
final deletion and hardening
~~~

## 2. Non-negotiable coordination rules

1. No Agent edits the shared integration worktree.
2. Every Agent gets a separate worktree and branch.
3. No Agent edits files outside its declared write set.
4. No Agent adds a compatibility adapter for the old Page/Element/Reference
   schema.
5. No Agent changes the canonical model contract without an explicit contract
   review.
6. No Agent changes uf-chaos data before the runtime contract integration gate.
7. Every Agent must return:
   - files changed;
   - tests added or run;
   - assumptions made;
   - unresolved contract questions;
   - known follow-up work.
8. Agents may read any relevant source, but parallel writes must be disjoint.
9. Documentation integration is owned by one Agent after the implementation
   contracts have stabilized.
10. The parent branch is only merged after its checks pass; no manual cherry
   picking of partial, unverified patches.

## 3. Dependency graph

~~~mermaid
flowchart TD
    C["P0 Contract freeze"] --> M["P1 Runtime model"]
    C --> H["P2 C++ envelope"]
    C --> R["P3 Resolver and receipt"]
    C --> N["P4 Navigation and offline checks"]
    C --> B["P5 Agent backend"]
    C --> U["P6 Annotator UI"]
    C --> T["P7 Fixtures and tests"]

    M --> R
    M --> N
    M --> B
    M --> U
    H --> I["P8 Integration gate"]
    R --> I
    N --> I
    B --> I
    U --> I
    T --> I

    I --> D1["P9 uf-chaos model migration"]
    I --> D2["P10 uf-chaos task migration"]
    I --> D3["P11 project plugin and evidence migration"]
    I --> D4["P12 docs cleanup"]
~~~

P1 through P7 can start from the same contract-freeze commit, but not all of
them can finish independently. The dependency is expressed through the
contract, not through shared file edits.

## 4. P0: contract freeze

Owner: lead architecture Agent

This is the only intentionally serial design package. It must finish before
parallel implementation starts.

### Deliverables

1. Canonical runtime object definitions:

~~~text
Context
Surface
RuntimeState
Target
Locator
LocatorVariant
Reader
Binding
Action
Transition
Resolution
Receipt
~~~

2. Canonical result types:

~~~text
Present
Absent
Unknown
Resolved
Ambiguous
UnknownResolution
~~~

3. Runtime runtime-model.toml schema.
4. Offline CandidateModel schema.
5. Annotation backend/UI API contract.
6. C++ pre-VM envelope contract.
7. Receipt and action-authorization invariants.
8. Exact file ownership for P1 through P7.

### Acceptance gate

The contract is frozen only when:

- no object has two responsibilities;
- no old Page/Element/Reference field is required by the new model;
- runtime data and offline evidence are explicitly separated;
- the UI can display a CandidateModel without knowing runtime internals;
- C++ can consume the envelope without interpreting surface semantics;
- all Agents can build against the same field and enum definitions.

The freeze should be represented by one commit in the integration branch.

## 5. P1: runtime model and compiler

Owner: Runtime Model Agent

### Scope

Implement the new trusted Luau model and compile the new runtime-model.toml.

### Write set

~~~text
modules/task/runtime/model.luau
modules/task/runtime/project.luau
modules/task/runtime/schema.luau
modules/task/runtime/model-compiler.luau
schema/
tests/task/test-runtime-model-v2.luau
~~~

The exact new filenames may be adjusted during P0, but this Agent owns the
model constructors, validation, parsing, and compiled indexes.

### Responsibilities

- Context, Surface, Target, Locator, Reader, Binding, Action, Transition;
- immutable framework-minted objects;
- semantic validation;
- locator-asset existence and hash validation;
- surface-layer and context constraints;
- binding action constraints;
- compiled lookup indexes;
- runtime model content hash;
- removal of the old model constructors.

### Must not modify

- observe, recognition, evidence, navigation, C++ host;
- annotation UI;
- uf-chaos;
- shared architecture documents.

### Acceptance

- synthetic models compile;
- invalid identity/action combinations fail with useful diagnostics;
- an authored fallback surface is rejected;
- no old field is accepted accidentally;
- downstream Agents can import the frozen model contract.

## 6. P2: C++ pre-VM model envelope

Owner: Host Boundary Agent

### Scope

Update the C++ pre-VM reader to understand only the new model envelope.

### Write set

~~~text
modules/task/source/task/runtime-model-file.hpp
modules/task/source/task/runtime-model-file.cpp
modules/task/source/task/task-host.cpp
tests/task/test-runtime-model-file.cpp
tests/task/test-script-validator.cpp
~~~

### Responsibilities

- schema version;
- model content hash;
- geometry fingerprint;
- target names;
- surface names;
- pre-VM script literal validation.

The parser must not validate Binding identity, locator scoring, surface
selection, or transition semantics. Those belong to Luau.

### Must not modify

- Luau model semantics;
- annotation backend or UI;
- project data;
- old schema compatibility code.

### Acceptance

- a new runtime-model.toml envelope is accepted;
- duplicate target/surface names fail before VM startup;
- model hash remains stable over the exact file bytes;
- C++ never needs annotation screenshots;
- the C++ tests prove it does not become a second semantic parser.

P2 can work in parallel with P1 because it consumes only the P0 envelope
contract.

## 7. P3: resolver, observation, and action safety

Owner: Resolver Agent

### Scope

Implement runtime candidate evaluation, Surface stack resolution, and safe
action authorization.

### Write set

~~~text
modules/task/runtime/recognition.luau
modules/task/runtime/observe.luau
modules/task/runtime/evidence.luau
modules/task/runtime/resolution.luau
tests/task/test-resolution-v2.luau
tests/task/test-receipt-authorization-v2.luau
~~~

### Responsibilities

- Present/Absent/Unknown evidence;
- Resolved/Ambiguous/UnknownResolution;
- scene, overlay, and interrupt candidate ordering;
- context compatibility;
- Surface stack construction;
- evidence conflict reporting;
- receipt fields;
- same-ticket and same-frame authorization;
- explicit action grants and preconditions;
- rejection of unknown and ambiguous clicks.

### Must not modify

- model constructors or TOML schema;
- navigation/replay;
- annotation UI;
- C++ envelope;
- uf-chaos task policies.

### Acceptance

The test suite must include:

- two candidates that both match and produce Ambiguous;
- insufficient OCR confidence producing Unknown;
- an overlay resolving above a scene;
- an interrupt resolving above any scene;
- a catch-all-like frame producing UnknownResolution with no receipt;
- stale receipts being rejected;
- a hit from one ticket being rejected by another ticket;
- a resolved state authorizing only the Binding's declared action.

P3 depends on the P0 contract and compiles against P1's model types. It may
start before P1 finishes by using contract stubs, but it cannot merge before
P1.

## 8. P4: navigation, replay, and offline validation

Owner: State Graph Agent

### Scope

Separate declared transitions, observed transitions, and offline assertions.

### Write set

~~~text
modules/task/runtime/navigation.luau
modules/task/runtime/replay.luau
modules/task/runtime/regress.luau
modules/task/runtime/oracle.luau
modules/task/runtime/offline/
tests/task/test-navigation-v2.luau
tests/task/test-replay-v2.luau
tests/task/test-regress-v2.luau
~~~

### Responsibilities

- declared Transition model;
- observed transition records;
- replay against Surface stacks;
- offline frame/assertion matrix;
- coverage, ambiguity, and missing-evidence findings;
- distinction between unreviewed and absent evidence;
- no screenshot loading in the normal runtime path.

### Must not modify

- runtime resolver semantics;
- annotation UI;
- project task scripts;
- C++ code.

### Acceptance

- declared and observed transitions are different types;
- replay reports an unknown or ambiguous Surface explicitly;
- regress does not treat an absent assertion as negative evidence;
- offline modules can be excluded from a normal run;
- shared Target placement can be tested at multiple rectangles.

P4 can proceed in parallel with P3 after the contract freeze, but its
integration tests depend on the final Resolution shape.

## 9. P5: Agent backend and CandidateModel

Owner: Annotation Intelligence Agent

### Scope

Build the offline data and Agent-facing backend. This package does not own the
browser UI.

### Write set

~~~text
tools/annotate/model_file.py
tools/annotate/serve.py
tools/annotate/candidate_model.py
tools/annotate/agent_pipeline.py
tools/annotate/patches.py
tools/annotate/evidence_store.py
tools/annotate/tests/
~~~

### Responsibilities

- frame and observation storage;
- CandidateModel;
- semantic model patches;
- proposal provenance;
- confidence and conflict reports;
- accept/reject decisions;
- model compilation requests;
- schema validation through the canonical compiler;
- Agent stages: capture, perception, structure, contract, verification,
  repair.

### Must not modify

- annotator TypeScript/JavaScript;
- runtime resolver;
- C++ host;
- uf-chaos;
- old model schema as a compatibility path.

### API contract

The backend must expose typed operations equivalent to:

~~~text
list_candidates()
get_candidate(id)
accept_patch(id)
reject_patch(id)
compile_candidate(id)
run_validation(id)
get_conflicts(id)
get_provenance(id)
~~~

The backend returns semantic objects, not raw TOML formatting instructions.

### Acceptance

- an Agent can produce a CandidateModel without touching accepted runtime data;
- every proposal carries supporting evidence and provenance;
- accepting a patch is explicit;
- ambiguous candidates remain unresolved;
- the backend can compile and validate a small synthetic candidate.

P5 depends on the frozen schema and compiler API, but can work in parallel
with P3 and P4.

## 10. P6: Annotation decision-queue UI

Owner: Annotation UI Agent

### Scope

Replace the field-oriented annotator with a CandidateModel and decision queue
interface.

### Write set

~~~text
tools/annotate/annotator.ts
tools/annotate/annotator.js
tools/annotate/index.html
tools/annotate/style.css
tools/annotate/ui/
tools/annotate/ui-tests/
~~~

### Responsibilities

The primary navigation is:

~~~text
Agent proposals
Conflicts
Low-confidence observations
Missing identity evidence
Unsafe actions
Uncovered frames
Compile and replay
Review history
~~~

The UI must show:

- supporting frames;
- competing Surface candidates;
- the semantic patch;
- expected runtime effect;
- validation status;
- blast radius;
- accept/reject/edit actions.

### Must not modify

- Python backend protocol;
- runtime model;
- project plugin;
- C++ code.

The backend API must be frozen by P0/P5 before this Agent merges.

### Acceptance

- a user can approve a semantic patch without editing raw TOML;
- a user can inspect why a Surface is ambiguous;
- action grants are clearly marked as high impact;
- unknown and low-confidence items cannot be bulk-approved accidentally;
- the UI does not expose old Page/Element/Reference terminology.

## 11. P7: fixtures and cross-layer tests

> **2026-08-11:** this package ran and its output was later removed. The write
> set below was created on 2026-08-09; `tests/fixtures/annotation-v2/` and
> `tests/annotation/` never gained a test target, nothing referenced them, and
> they were deleted on 2026-08-11. `tests/task/fixtures/` was never created. The
> eleven scenarios listed here survive as T01-T11 in
> [the test matrix](2026-08-09-annotation-v2-test-matrix.md), which also records
> the fixture format and which contracts a live gate covers today. Do not
> recreate the deleted files from this section.

Owner: Verification Agent

### Scope

Create synthetic and small real-world fixtures that all implementation Agents
can use without sharing production files.

### Write set

~~~text
tests/fixtures/annotation-v2/
tests/task/fixtures/
tests/annotation/
docs/plans/2026-08-09-annotation-v2-test-matrix.md
~~~

### Fixture scenarios

At minimum:

- ordinary scene;
- overlay above a scene;
- interrupt above any scene;
- two indistinguishable surfaces;
- OCR unknown;
- shared leave button;
- shared equip button;
- strip/collection target;
- target with page-specific placement;
- stale receipt;
- unknown dark modal;
- a declared transition that was not observed.

### Acceptance

Every runtime and tool Agent can run the fixtures independently. The fixtures
must not depend on uf-chaos's large model.

P7 can start immediately after P0 and should be merged before P3/P4
integration.

## 12. P8: integration gate

Owner: Integration Agent

This is serial. It is not a normal feature package.

### Merge order

1. P0 contract freeze;
2. P1 runtime model;
3. P2 C++ envelope;
4. P7 fixtures;
5. P3 resolver;
6. P4 navigation and offline validation;
7. P5 backend;
8. P6 UI.

P2 can be merged before P1 if it only uses the frozen envelope. P3/P4/P5/P6
must not be accepted as complete until their contract-level integration tests
pass.

### Integration checks

- static C++ checks;
- Luau module checks;
- model compile;
- runtime resolution tests;
- receipt safety tests;
- offline regression tests;
- backend API tests;
- UI type/build checks;
- no old schema names in runtime or UI;
- no duplicate schema declarations;
- no screenshot assets in the runtime model package.

The Integration Agent owns only integration glue, test wiring, and conflict
resolution. It must not redesign the schema during merge.

## 13. P9-P11: parallel uf-chaos migration

These packages start only after P8.

### P9: model data migration

Owner: Project Model Agent

Write set:

~~~text
E:/umbraflow-projects/uf-chaos/page-model.toml
E:/umbraflow-projects/uf-chaos/annotation/
~~~

Responsibilities:

- rebuild Context and Surface definitions;
- create Target and Locator definitions;
- rebuild Bindings;
- use existing screenshots and templates as Agent input;
- resolve known page conflicts;
- remove old model fields.

### P10: task-script migration

Owner: Project Runtime Agent

Write set:

~~~text
E:/umbraflow-projects/uf-chaos/tasks/
E:/umbraflow-projects/uf-chaos/scripts/
~~~

Responsibilities:

- replace page resolution calls;
- replace old element/reference access;
- remove manual page ORDER;
- express task actions through new Binding contracts;
- adapt retry and wait policy to Unknown/Ambiguous results.

This Agent must not change the upstream runtime.

### P11: project plugin and evidence migration

Owner: Project Annotation Agent

Write set:

~~~text
E:/umbraflow-projects/uf-chaos/annotate/
E:/umbraflow-projects/uf-chaos/evidence/
E:/umbraflow-projects/uf-chaos/*.md
~~~

Responsibilities:

- replace plugin mode terminology with project scopes;
- move screenshots, assertions, review notes, and provenance to the offline
  workspace;
- define project-specific Agent labels and naming hints;
- build the project regression corpus.

P9, P10, and P11 can run in parallel after P8 because their write sets are
disjoint, but they must agree on the accepted target and surface names.

The safest coordination mechanism is a short accepted-name manifest generated
by P9 and consumed by P10/P11. Changes to that manifest go through the
Integration Agent.

## 14. P12: documentation cleanup

Owner: Documentation Agent

Write set:

~~~text
docs/ARCHITECTURE.md
docs/plans/
docs/pitfalls/
tools/annotate/*.md
~~~

Responsibilities:

- update architecture ownership;
- remove obsolete Page/Reference/Capability descriptions;
- document runtime/offline boundaries;
- document Agent review and promotion rules;
- document Surface stack resolution;
- document the final project integration process.

This Agent starts after P8 so the documentation describes implemented APIs,
not guesses.

## 15. Parallelization summary

The practical maximum is:

~~~text
serial:
  P0 contract freeze

parallel wave 1:
  P1 runtime model
  P2 C++ envelope
  P4 navigation/offline validation
  P5 Agent backend
  P6 UI
  P7 fixtures

serial:
  P8 integration gate

parallel wave 2:
  P9 uf-chaos model
  P10 uf-chaos tasks
  P11 uf-chaos plugin/evidence
  P12 documentation cleanup
~~~

P3 resolver can start in wave 1 after P0, but should merge after P1 and P7.

## 16. Recommended Agent prompt template

Every dispatched Agent should receive:

~~~text
You own work package <ID>.

Read:
  <contract documents>
  <relevant source files>

Write only:
  <exact write set>

Do not modify:
  <other agents' write sets>
  old schema compatibility
  project data unless explicitly assigned

Deliver:
  implementation or document
  tests
  files changed
  commands run
  assumptions
  unresolved contract questions

Before finishing:
  run the package checks
  inspect the diff
  report any dependency that must be merged first
~~~

## 17. Worktree strategy

The current design worktree is the integration design branch:

~~~text
E:/github/umbraflow-cpp-annotation-design
branch: design/annotation-system-v2
~~~

After P0 is committed, create one child worktree per Agent:

~~~text
E:/github/umbraflow-cpp-wt-runtime-model
E:/github/umbraflow-cpp-wt-host-envelope
E:/github/umbraflow-cpp-wt-resolver
E:/github/umbraflow-cpp-wt-offline-validation
E:/github/umbraflow-cpp-wt-agent-backend
E:/github/umbraflow-cpp-wt-annotator-ui
E:/github/umbraflow-cpp-wt-fixtures
~~~

The parent integration worktree remains clean while Agents work. Each child
branch is merged only after its package-level checks pass.

## 18. Definition of parallel-ready

The redesign is ready to dispatch in parallel when:

- P0 has a committed contract;
- all write sets are disjoint;
- the backend/UI API is written down;
- the C++ envelope is written down;
- the fixture scenarios exist;
- each package has a single owner;
- no package requires another Agent's uncommitted implementation to begin.

Before that point, parallel coding would create schema drift rather than
useful parallelism.
