# Umbraflow Runtime Annotation and Agent Model

Status: design proposal

Date: 2026-08-09

## 1. Decision

Umbraflow's annotation system is an offline model-building and compilation
system. It is not a database of screenshots that the runtime carries with it.

The deployment boundary is:

~~~text
offline exploration, screenshots, OCR observations, and review
        |
        | Agent analysis, human decisions, validation, compilation
        v
page-model.toml + runtime locator assets
        |
        v
Umbraflow runtime
~~~

page-model.toml remains the runtime contract file name, but its schema is
redesigned from scratch. The old Page, Element, Reference, CapabilitySet,
holding, exercised, screen, and expect semantics are not retained through a
compatibility layer.

An offline one-time data extraction tool may reuse existing project material,
but the runtime and authoring system do not support the old schema.

## 2. Goals

The new system must:

1. Describe the runtime information needed to recognize a UI and authorize an
   action without deploying annotation screenshots.
2. Represent a layered UI state: a business context plus an ordered stack of
   visible surfaces.
3. Reject ambiguous and unknown states before an action can receive a receipt.
4. Make identity evidence, reading, placement, and action authorization
   explicit instead of encoding them as interacting boolean fields.
5. Let an Agent perform the mechanical majority of annotation work.
6. Leave humans only the semantic and high-risk decisions that pixels cannot
   determine reliably.
7. Keep the C++ host responsible for capture, input, ticket guarantees, and
   model identity, while the trusted Luau framework owns model semantics.
8. Give the annotation tool, model compiler, runtime, check, regress, and
   replay one schema authority.

## 3. Non-goals

This design does not:

- make screenshots runtime dependencies;
- turn the runtime into a general-purpose visual-learning system;
- let an Agent silently promote unknown or ambiguous states into known pages;
- preserve the old page-model schema or old runtime API;
- make project scripts responsible for global page recognition order;
- treat observed navigation as identical to declared navigation policy.

## 4. What is retained from the current Umbraflow design

### 4.1 Observation cycles

Capture once and use the same frame for resolution, reading, finding, and
clicking. A ticket identifies the observation cycle. This prevents a script
from resolving one frame and acting on a later frame.

### 4.2 Trusted evidence ledgers

The framework-minted hit and receipt ledgers in
modules/task/runtime/evidence.luau are the right security pattern. A project
must not be able to forge a hit or a receipt by constructing a table with the
same fields.

### 4.3 C++/Luau ownership

C++ should own:

- target/window identity;
- frame capture;
- raw vision and OCR primitives;
- input delivery;
- ticket and lease guarantees;
- model hash and geometry fingerprint checks.

The trusted Luau framework should own:

- the semantic model;
- candidate evaluation;
- surface-stack resolution;
- binding lookup;
- action authorization;
- transitions and replay semantics.

C++ should not become a second implementation of page or surface semantics.

### 4.4 Content-addressed runtime assets

Runtime template assets may be produced from annotation screenshots, but a
runtime template crop is not the annotation corpus. The full source screenshots,
their review history, and their assertions remain offline.

## 5. Runtime domain model

The runtime model has seven primary concepts.

### 5.1 Context

A Context is a business/runtime environment, not a screenshot and not a
locator. Examples:

~~~text
camp
chaos
chaos_battle
settlement
~~~

A context can constrain which surfaces are candidates and which transitions
are legal. A context may be carried by the task state machine or inferred as
part of a surface-stack resolution.

### 5.2 Surface

A Surface is one visible layer with an identity contract and bindings.

Its layer kind is one of:

~~~text
scene       replaces the current visible scene
overlay     covers a compatible lower surface
interrupt   may appear independently of the current scene
~~~

A surface is not the entire runtime state. The runtime state is:

~~~text
RuntimeState = context + ordered surface stack
~~~

For example:

~~~text
context: chaos_battle
surfaces:
  - battle
  - enemy_inspect
~~~

The old overlay, interrupt, catch_all, state, and over fields are replaced by:

- kind = scene | overlay | interrupt;
- an explicit context compatibility relation;
- an explicit covers relation where a surface is an overlay;
- resolver rules derived from layer kind and evidence strength.

There is no authored catch-all surface. UnknownResolution is a runtime result,
not a model object and never grants an action.

### 5.3 Target

A Target is a reusable semantic object, such as:

~~~text
confirm_button
leave_button
enemy_title
item_cards
~~~

A target owns stable mechanics:

~~~text
Target
  id
  geometry
  locators
  reader?
  supported actions
~~~

A target does not own page identity. A surface binding decides whether a target
is used as identity evidence.

### 5.4 Locator and LocatorVariant

A Locator describes how a target can be found:

~~~text
template
ocr/text
fixed geometry
repeated geometry
relative geometry
~~~

A LocatorVariant represents a visual state of one locator, for example:

~~~text
confirm_button.enabled
confirm_button.disabled
confirm_button.highlighted
~~~

The old Appearance concept is replaced by this name because the runtime needs
a locating strategy, not an abstract record of a screenshot appearance.

Locator resources may point to cropped template assets:

~~~toml
[[locator]]
id = "confirm_button.enabled"
target = "confirm_button"
kind = "template"
source = "templates/confirm_enabled.png"
threshold = 0.92
~~~

### 5.5 Reader

A Reader describes how a target region is decoded:

~~~text
line
block
items
presence
~~~

The old read_mode, read_floor, and item_spacing fields become typed reader or
geometry properties rather than ungrouped element fields.

### 5.6 Binding

A Binding is the relation between one surface and one target. It carries the
surface-specific facts:

~~~text
placement
identity evidence
reading usage
action grants and preconditions
~~~

Identity is a binding usage, not a target capability. Reading is a reader
usage, not a page capability. An action is an explicit typed contract, not a
generic interact = true.

Illustrative shape:

~~~toml
[[binding]]
surface = "training_confirm"
target = "confirm_button"
placement = { kind = "default" }

[binding.identity]
locator = "confirm_button.enabled"
text = "确认"
polarity = "required"

[binding.actions.click]
locator = "confirm_button.enabled"
requires = "confirm_button.enabled"
~~~

The old holding field is removed. Shared targets are represented naturally by
multiple bindings. Editorial ownership, if needed by the tool, belongs in
offline review metadata and is not a runtime property.

### 5.7 Transition

A declared transition describes a permitted or expected business movement:

~~~text
from runtime state
trigger action
expected resulting state
verification policy
~~~

The model stores declared transitions. Observed transitions and test
assertions are offline records and must not be confused with the declared
runtime policy.

## 6. Runtime model file

page-model.toml contains only deployment information:

~~~text
schema version
geometry fingerprint
contexts
targets
locators and readers
surfaces
bindings
declared transitions
runtime asset references
~~~

It does not contain:

~~~text
annotation screenshots
raw OCR observations
Agent reasoning
review history
regression matrix
notes
observed transitions
~~~

An illustrative complete fragment is:

~~~toml
schema_version = 1
base_resolution = [1920, 1080]
base_dpi = [96, 96]

[[context]]
id = "chaos_battle"

[[target]]
id = "confirm_button"
kind = "control"
geometry = { kind = "fixed", rect = [100, 200, 180, 64] }
reader = { kind = "ocr", mode = "line", floor = 0.80 }
actions = ["click"]

[[locator]]
id = "confirm_button.enabled"
target = "confirm_button"
kind = "template"
source = "templates/confirm_enabled.png"
threshold = 0.92

[[surface]]
id = "training_confirm"
kind = "overlay"
contexts = ["chaos_battle"]
covers = ["camp"]

[[binding]]
surface = "training_confirm"
target = "confirm_button"

[binding.identity]
locator = "confirm_button.enabled"
text = "确认"
polarity = "required"

[binding.actions.click]
locator = "confirm_button.enabled"
requires = "confirm_button.enabled"

[[transition]]
from = { context = "camp", surfaces = [] }
trigger = { target = "training_button", action = "click" }
to = { surfaces = ["training_confirm"] }
~~~

The exact TOML layout is subject to implementation, but the semantic grouping
is mandatory.

## 7. Offline annotation and Agent model

The offline workspace is separate from the runtime package:

~~~text
annotation/
  frames/
  observations/
  candidates/
  decisions/
  regressions/
  review/
~~~

### 7.1 Frame

A frame is an offline source artifact:

~~~text
image hash
resolution
source trace
capture metadata
~~~

The runtime may also hold a temporary live frame during an observation cycle,
but it does not retain the offline frame corpus.

### 7.2 Observation

An observation is an offline measurement made against a frame:

~~~text
target or region
measurement kind
raw OCR/template result
confidence
coordinates
source frame
~~~

Observations are evidence for model construction. They are not themselves
runtime contracts.

### 7.3 Assertion

An assertion is a reviewed statement about evidence:

~~~text
frame
subject
predicate
expected result
provenance
review status
~~~

The minimum result domain is:

~~~text
present
absent
unknown
~~~

The absence of an assertion means unreviewed, not absent. Runtime recognition
must not use an unreviewed cell as negative evidence.

### 7.4 Candidate Model

Agent output is not written directly into the accepted runtime model. It first
enters a CandidateModel containing:

~~~text
frame clusters
target candidates
surface candidates
locator candidates
binding candidates
transition candidates
supporting evidence
confidence
conflicts
proposed patches
~~~

Each proposal is a semantic patch, for example:

~~~text
create surface "training_confirm"
set surface kind = overlay
bind target "confirm_button"
add required text identity "确认"
grant click action using locator "enabled"
~~~

The user reviews the semantic patch, not a large raw TOML diff.

## 8. Agent pipeline

### Capture Agent

- imports exploration trajectories;
- extracts relevant frames;
- deduplicates frames;
- groups frames by target, window, and rough context.

### Perception Agent

- runs OCR;
- proposes regions;
- crops locator assets;
- detects repeated collections;
- estimates geometry and thresholds.

### Structure Agent

- clusters frames into candidate surfaces;
- identifies scene/overlay relationships;
- detects shared targets;
- proposes context membership;
- finds distinguishing evidence.

### Contract Agent

- proposes targets;
- proposes locators and readers;
- proposes bindings;
- proposes action contracts;
- proposes declared transitions.

### Verification Agent

- compiles the candidate model;
- evaluates all candidate surfaces against all available frames;
- finds no-match frames;
- finds multi-match frames;
- finds weak or missing identity evidence;
- checks shared-target ambiguity;
- checks action authorization;
- calibrates thresholds.

### Repair Agent

Converts verification findings into additional semantic patches:

~~~text
shop and equip_page share insufficient identity evidence
training_confirm has no unique positive evidence
enemy_inspect conflicts with battle and needs overlay scope
~~~

## 9. Human decision boundary

The Agent may automatically accept low-risk mechanical facts inside the
candidate workspace when they have sufficient support and no conflicts.

The user must approve:

- semantic names;
- whether two visual clusters are the same business surface;
- context and overlay scope;
- action permission;
- transition meaning;
- high-impact merges and splits.

The Agent must never silently perform these promotions:

~~~text
unknown -> known surface
ambiguous -> arbitrary surface
not observed -> absent
low-confidence target -> actionable target
shared target -> arbitrary page ownership
~~~

## 10. Runtime resolution

Every resolution uses one observation cycle:

~~~text
open cycle
  -> capture frame
  -> evaluate candidate surfaces
  -> combine context and surface constraints
  -> resolve the ordered surface stack
  -> return Resolution
close cycle
~~~

Each evidence evaluation returns:

~~~text
Present(value, confidence, proof)
Absent(proof)
Unknown(reason, confidence)
~~~

The complete resolution result is one of:

~~~text
Resolved(runtime state, evidence)
Ambiguous(candidates, conflicts, evidence)
Unknown(diagnostics, evidence)
~~~

Only Resolved can mint a receipt.

There is no first-match page order. Candidate ordering is deterministic:

1. interrupt surfaces;
2. overlays compatible with the current context;
3. scene surfaces;
4. unknown.

Within one layer, evidence strength and specificity may rank candidates, but a
tie or insufficient margin produces Ambiguous, not an arbitrary choice.

## 11. Receipt and action authorization

The existing evidence identity ledger remains the foundation. A new receipt
must contain or securely reference:

~~~text
frame/ticket identity
resolved runtime state
surface stack
binding
action contract
evidence used
authorization scope
~~~

observe.click must require:

1. a framework-minted receipt from the current ticket;
2. a binding that grants the requested action;
3. a fresh hit or locator proof from the same ticket;
4. any binding action preconditions;
5. a non-ambiguous resolved state.

Unknown and ambiguous states never produce a clickable receipt.

## 12. Schema authority and compilation

There must be one canonical schema and one semantic validator. The current
independent definitions in Luau, Python, TypeScript, and the annotation
backend must be removed.

The intended arrangement is:

~~~text
canonical model schema
        ↓
model compiler and semantic validator
        ↓
Luau runtime model
Python annotation backend
TypeScript UI types
C++ pre-VM envelope metadata
~~~

The C++ layer only needs the pre-VM envelope:

~~~text
schema version
model content hash
geometry fingerprint
target names
surface names
~~~

It must not implement a second interpretation of bindings, identity, or
surface selection.

## 13. Umbraflow versus project responsibility

### Umbraflow upstream owns

- runtime schema;
- model compiler;
- Surface stack resolver;
- tri-state evidence;
- receipts and action authorization;
- transition primitives;
- offline Candidate Model;
- Agent pipeline primitives;
- annotation decision queue;
- check, regress, and replay infrastructure;
- schema generation and validation;
- C++/Luau model envelope.

### uf-chaos owns

- Context and Surface names;
- game-specific target semantics;
- screenshots and exploration traces;
- project-specific naming hints;
- action policy decisions;
- project transitions;
- accepted page-model.toml;
- project task scripts;
- domain-specific Agent prompts and labels.

uf-chaos should consume the model language, not implement the resolver, receipt
security, or annotation compiler.

## 14. Concrete source changes

### Runtime

Rewrite:

- modules/task/runtime/model.luau
- modules/task/runtime/project.luau
- modules/task/runtime/recognition.luau
- modules/task/runtime/observe.luau
- modules/task/runtime/evidence.luau
- modules/task/runtime/navigation.luau

Split or relocate offline-only behavior from:

- modules/task/runtime/oracle.luau
- modules/task/runtime/regress.luau
- modules/task/runtime/replay.luau
- modules/task/runtime/scribe.luau

### C++ host

Update:

- modules/task/source/task/page-model-file.hpp
- modules/task/source/task/page-model-file.cpp
- modules/task/source/task/task-host.cpp
- related task model tests

The pre-VM names become target/surface names. The C++ parser remains an
envelope reader, not a semantic page-model parser.

### Annotation tool

Replace the old field-oriented model in:

- tools/annotate/model_file.py
- tools/annotate/serve.py
- tools/annotate/annotator.ts
- tools/annotate/annotator.js

The new UI is organized around:

~~~text
agent proposals
conflicts
low-confidence observations
missing identity evidence
unsafe actions
uncovered frames
model compilation results
~~~

### Documentation

Update:

- docs/ARCHITECTURE.md
- the current annotation plans;
- the annotator specifications;
- runtime evidence and replay documentation.

Retire documents that describe Page/Reference/Capability as the final model.

### Project migration

Rebuild uf-chaos from its offline corpus. Do not mechanically preserve all old
rows. Use existing screenshots, templates, traces, and names as Agent input,
then validate the new model against the known failure cases:

- training confirmation overlay;
- training reward;
- loot details;
- reroll confirmation;
- shop/equip shared anchors;
- settlement tail pages;
- enemy inspection overlay;
- unknown dark modal.

## 15. Implementation phases

### Phase 0: contract

Produce:

- schema;
- object definitions;
- runtime/offline boundary;
- tri-state evidence;
- receipt invariants;
- resolution semantics.

### Phase 1: runtime model

Implement:

- new constructors and validators;
- new TOML parser;
- compiled lookup indexes;
- model hash and C++ envelope;
- synthetic model tests.

### Phase 2: resolver safety

Implement:

- Surface candidates;
- Surface stack resolution;
- Resolved/Ambiguous/Unknown;
- diagnostic evidence;
- receipt authorization;
- no action from unknown or ambiguous state.

### Phase 3: offline Agent pipeline

Implement:

- frame ingest;
- observation store;
- Candidate Model;
- semantic patches;
- automatic validation;
- conflict and coverage reports.

### Phase 4: annotation decision queue

Implement:

- proposal review;
- evidence display;
- semantic patch approval;
- model compile;
- replay preview;
- risk-based human review.

### Phase 5: project rebuild

Start with a small acceptance corpus covering the known uf-chaos failures.
Then rebuild the complete model and remove manual page ordering.

### Phase 6: deletion and hardening

Delete the old schema, old model writers, duplicate validators, obsolete
documentation, and old project-specific workarounds.

## 16. Acceptance criteria

The design is complete only when:

- runtime packages contain no annotation screenshots;
- page-model.toml contains only deployment data;
- Surface resolution is not first-match;
- unknown and ambiguous states cannot receive an actionable receipt;
- overlay stacks are represented explicitly;
- identity, reading, placement, and actions are separate concepts;
- shared targets do not require runtime ownership flags;
- declared, observed, and expected transitions are separate;
- Agent output is reviewable as semantic patches;
- users do not manually annotate every rectangle and threshold;
- the compiler and all tools use one schema authority;
- runtime fields cannot be silently dropped by one writer;
- the known uf-chaos misclassification cases become automated regression cases.
