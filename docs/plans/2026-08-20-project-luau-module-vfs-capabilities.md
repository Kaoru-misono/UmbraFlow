# Project Luau modules, resources, locked packages, and explicit capabilities

Date: 2026-08-20
Scope: `script`, `deployment`, `operator`, `project`, `service`, `cli`,
conformance, schemas, examples, generated public contract, release publication,
and the later project-template/consumer migration
Status: **independent review passed; R1-A implemented and locally validated;
R1-B pending**

## Outcome

Project authors write multi-file Luau. UmbraFlow executes a pinned module
closure with real resolver, module-return, cache, cycle, and failure semantics;
it never concatenates sources. The same public cut adds a content-addressed,
read-only Project resource closure for Python-built runtime corpora and other
declared bytes. The registration pins both code bytes and the complete execution
environment contract.

Locked packages then reuse the resolver under verified `@alias/...` names.
Filesystem and network authority arrive later through separately registered,
policy-scoped Luau capability programs whose durable results become explicit
evidence for pure calls.

The governing ruling is
[`2026-08-20-project-execution-identity-is-closure-plus-environment.md`](../decisions/2026-08-20-project-execution-identity-is-closure-plus-environment.md).

## Non-negotiable invariants

1. `derive`, `plan`, `nextStep`, `reconcile`, and `reduce` remain pure decoded
   JSON-in/JSON-out calls in a fresh quota-bound VM.
2. A registration pins both the module closure and the observable execution
   environment. Changing resolver, cache, cycle/error, bridge, Luau, compiler,
   globals, or host API semantics moves the registration identity.
3. Runtime resolution observes only logical names and exact bytes in the
   verified registration. It never sees a host path, search path, package cache,
   registry, network, environment variable, or current working directory.
4. Authors declare paths; trusted code derives hashes. No author types a module
   or resource digest in `umbraflow-project.json`.
5. Module names and resource names are canonical before hashing. Authored array
   order has no identity; names, selected entry, kinds, sizes, and exact bytes
   do.
6. Format-2 persisted Operator history is retained byte-for-byte and becomes
   audit-only. It is not reinterpreted or executed under format 3.
7. Python remains an offline build dependency for JCS, artifact compilation,
   database extraction/transforms, and packaging. Project runtime behavior is
   shipped as Luau plus immutable resources.
8. External capabilities are deny-by-default, manifest-declared, policy-scoped,
   optionally user-approved, quota-bound, and audited. They never run inside a
   pure ProjectPlugin VM.

## Exact contracts for release R1

R1 is one atomic supported-contract boundary. Its internal work packages may be
developed and tested in sequence, but no commit, release, public-contract update,
or consumer migration may present only a subset as supported.

### Authored project directory generation 2

The active directory schema becomes `umbraflow-project/v2`; the old generation
is removed from production loading. A deployment declares:

```json
{
  "plugin": {
    "entry": "main",
    "modules": [
      {"name": "main", "path": "plugin/main.luau"},
      {"name": "recognition", "path": "plugin/recognition.luau"},
      {"name": "state/reducer", "path": "plugin/state/reducer.luau"}
    ]
  },
  "resources": [
    {"kind": "json", "name": "runtime.corpus", "path": "runtime/corpus.json"}
  ]
}
```

Module logical names and paths are unique. The entry equals exactly one logical
name. Resource names, names within one deployment, and resource paths are
unique. The loader reads every path through `ConfinedRoot` and retains no path
in runtime identity.

`plugin_authoring` still distinguishes a generated adapter from hand-written
code. Generated output is a valid one-module closure; hand-written code may use
any number of admitted modules. `plugin_justification` judges the behavior tier,
not whether code happens to fit in one file.

### Canonical module manifest

The loader sorts by UTF-8 logical-name bytes and derives exact RFC 8785 JCS:

```json
{
  "entry": "main",
  "modules": [
    {"name": "main", "sha256": "..."},
    {"name": "recognition", "sha256": "..."},
    {"name": "state/reducer", "sha256": "..."}
  ]
}
```

Its sha256 is `plugin_module_manifest_hash`. Changing any source byte, logical
name, or entry changes the hash. Reordering declarations does not. A duplicate
logical name/path, missing entry, invalid UTF-8 source, empty source, or limit
violation is refused before registration.

### Module-name and request grammar

Definitions are byte grammars, not Unicode-normalization rules:

```text
Segment            = [a-z][a-z0-9_-]{0,63}
ProjectModuleName  = Segment ("/" Segment){0,15}
RootRequest        = ProjectModuleName
RelativeRequest    = "./" ProjectModuleName
                   | ("../")+ ProjectModuleName
```

`ProjectModuleName`, the request string, and the resolved name are each at most
256 ASCII bytes. A root request starts at the project root. `./` appends to the
caller's directory. Each complete `../` removes one caller-directory segment;
removing beyond root refuses. No other normalization occurs.

Consequently `./a/../b`, `a/./b`, a bare dot segment, repeated `/`, leading or
trailing `/`, backslash, control/non-ASCII byte, extension fallback, `init`
fallback, and search-path spelling are invalid rather than normalized. Legal
requests that resolve to the same canonical name share one cache key. The
package grammar is absent in R1; `@` is refused until R2 moves the environment
identity.

Resource names use a separate exact byte grammar:

```text
ResourceSegment = [a-z][a-z0-9_-]{0,63}
ResourceName    = ResourceSegment ("." ResourceSegment){0,15}
```

A ResourceName is at most 128 ASCII bytes. Empty segments, leading/trailing
dots, repeated dots, controls, non-ASCII bytes, case folding, and Unicode
normalization are all absent. Each resource reader accepts exactly one Luau
string already satisfying this grammar and performs exact byte comparison.
Empty, non-string, extra-argument, overlong, or otherwise non-canonical calls
refuse at the reader; nothing is normalized.

### Runtime module state

- A host-created module environment publishes a caller-bound frozen `require`.
  The argument is exactly one bounded string and may be computed.
- Modules are precompiled at admission. Loading executes exact bytecode only
  from the sorted closure and returns exactly one non-`nil` Luau value.
- The entry value alone is passed to the bridge and must be the exact plain
  `plugin_id` plus five-function table. Dependency values have no such authority.
- State is keyed by resolved canonical name and is `unloaded`, `loading`,
  `loaded`, or `failed`. Repeated loaded requests return the same Luau identity.
- Self-require and indirect cycles use one bounded stable cycle refusal. Every
  active module unwound by that load becomes failed.
- Deterministic name/resolution/top-level Luau failures cache a stable refusal
  class and bounded message with pointer/traceback addresses removed. Repeating
  the request in plugin `pcall` observes the same failure.
- VM memory exhaustion, instruction/wall-time exhaustion, cancellation, and
  host invariants terminate the whole invocation. They are not dependency
  failures and cannot become recoverable cache entries.
- All modules share the fresh VM's memory, instruction, wall-time, value, and
  cancellation budgets. R1 sets and publishes limits for module count,
  per-source/total-source bytes, and per-bytecode/total-bytecode bytes.
- Admission compiles every module and executes the entry's reached dependencies.
  A later computed name can reach a syntactically admitted but previously
  unexecuted module and safely refuse then; R1 claims no impossible whole-input
  top-level proof.

### Execution-environment identity

`plugin_environment_hash` is the sha256 of exact canonical material containing:

- pinned Luau implementation and compiler options;
- bridge source;
- published globals and frozen tables;
- module-name/request grammar version;
- resolver, cache, cycle, terminal-failure, and cached-failure contract versions;
- every resource-reader name and observable return/error/cache contract; and
- the existing deterministic value-conversion and `tostring` contracts;
- every admission/runtime quota and name, request, value, stack, and error-copy
  ceiling listed below, by numeric value.

`pluginEnvironmentMaterial()` remains the single preimage writer and
`pluginEnvironmentHash()` its sole hash. Production project loading derives the
hash from this build; ProjectRegistration format 3 contains it, and registration
verification requires exact equality with the running implementation before a
plugin can register. SessionManifest pins the registration root and needs no
duplicate environment field.

R1 uses these exact observable limits:

| Limit | Value |
|---|---:|
| VM live memory | 16 MiB |
| instruction budget | 2,000,000 non-GC Luau interrupt safepoint ticks |
| wall-time budget | 2 seconds per admission/invocation unit |
| module count | 64 |
| one module source / all module sources | 256 KiB / 4 MiB |
| one module bytecode / all module bytecode | 1 MiB / 16 MiB |
| trusted bridge bytecode | 1 MiB, outside the project-bytecode total |
| resource count | 64 |
| one resource / all resources | 4 MiB / 16 MiB |
| module segment / segments / request / resolved name | 64 bytes / 16 / 256 bytes / 256 bytes |
| resource segment / segments / request | 64 bytes / 16 / 128 bytes |
| plugin id / entry-point name / entry-point count | 256 bytes / 64 bytes / 32 |
| JSON input/output depth / nodes / text | 64 / 524,288 / 1 MiB |
| JSON resource depth / nodes / text | 64 / 2,097,152 / 4 MiB |
| value-conversion stack reservation | 288 slots (`64 * 4 + 32`) |
| host error / cached module failure / reader callback error | 4,096 / 1,024 / 256 bytes |

One named C++ constant supplies each enforced value and its emitted material;
derived limits are emitted from the same arithmetic. Compiler constants supply
both `luau_compile` options (`optimizationLevel = 1`, `debugLevel = 0`, all
remaining options default-zero) and their material. A parity gate reads the
`modules/script/external/luau` gitlink and verifies that its exact revision is
the one in the material. The revision plus a contract string for non-GC loop
back-edge/call/return safepoints binds the interrupt cadence; no manually synced
version prose is accepted as the only evidence.

### Project resources

ProjectRegistration format 3 replaces `project_artifact_roots` with a sorted
`project_resources` array. Each exact row is:

```json
{"kind":"json","name":"runtime.corpus","sha256":"...","size":123}
```

`kind` is exactly `json`, `utf8`, or `bytes`; `size` is the exact byte count as a
bounded JSON integer. The full array is the canonical resource manifest inside
registration JCS, so name, kind, byte hash, and admission-relevant size are all
identity. The registrar recomputes rows from exact supplied blobs and refuses
missing, duplicate, extra, wrong-kind, wrong-size, and wrong-hash data.

The pure environment replaces `artifact.read` with:

- `resource.readJson(name)`: decoded, deeply frozen JSON with one identity per VM;
- `resource.readText(name)`: an admission-verified UTF-8 Luau string; and
- `resource.readBytes(name)`: an exact Luau byte string, which may contain
  arbitrary bytes even though the final ProjectPlugin output remains JSON.

Readers require the declared kind, never coerce or fall back, share the VM
quota, accept exactly one canonical ResourceName string, and can reach only
registration rows. Empty/non-string/extra/invalid reader calls refuse before a
lookup. The Python database-to-corpus build step emits a declared `json`
resource; no Python enters runtime.

### Registration, attestation, and persistence generations

- The active registration schema is `project-registration-v2`, but its document
  field is `project_registration_format: 3`; current code already uses format 2.
- Required registration identity fields include
  `plugin_module_manifest_hash`, `plugin_environment_hash`, and
  `project_resources`. `plugin_hash` and `project_artifact_roots` disappear.
- The attestation schema and `set_version` both become 2. Each attestation pins
  `project_registration_hash`; it does not restate plugin subset hashes.
- In-memory names, diagnostics, observations, conformance fixtures, and comments
  use the new terms in the same cut.

The Operator DDL changes through one registered exact-source/exact-target
audit-preserving migration. `project_registrations` becomes generation-neutral:

```text
registration_hash, registration_format, plugin_id,
plugin_identity_kind, plugin_identity_hash, canonical_manifest
```

Existing rows retain their primary key and canonical bytes, are marked format 2
and `single_source`, and keep all foreign keys/session/journal/evidence hashes
unchanged. New rows are format 3 and `module_manifest`. Read-only audit/export
may return historical bytes; every production path that would register, create,
resume, plan, or execute under format 2 refuses `legacy registration is
audit-only`. No old VM or format-2 execution fallback is retained.

## R1 work packages

### R1-A — Script kernel

Owner: `modules/script`

Replace the old `compile(moduleId, source, ...)` API with a nested owned
`Module { name, source }` closure API; retain no overload. Implement the exact
resolver/state contract and typed resource readers. Expand the environment
material before exposing the behavior.

Permanent focused tests cover root/relative/computed resolution, two legal
spellings selecting one cache object, top-level once per VM and again in a new
VM, self/indirect cycles, cached deterministic failure, terminal quota/cancel,
all invalid grammar classes, missing/duplicate modules, source/bytecode limits,
entry/dependency return contracts, and absence of filesystem/network/search
fallbacks. Resource tests cover all kinds, cache identity, UTF-8, wrong kind,
unknown name, byte fidelity, mutation, size, and VM quota.

R1-A completion evidence: the old overload is absent; the focused script suite
exercises the closed resolver, module state machine, terminal failures, complete
grammar boundary, typed resources, admission ceilings, environment preimage,
and Luau gitlink parity. The 2026-08-20 Windows `x64-debug` local CI gate passed
all 101 CI tests. This is an internal work-package boundary only: production
support remains unavailable until R1-B and R1-C complete the atomic release.

### R1-B — Schema, loader, identity, and Operator

Owners: `schema`, `deployment`, `operator`, `service`, `cli`

Land project directory v2, registration format 3/schema v2, attestation set v2,
module/resource loading, canonical derivation, registrar verification,
environment equality, handle/observation/session terminology, and every caller.
Add the exact-pair ledger migration and explicit audit-only production refusals.

Identity falsification must prove byte/name/entry/environment/kind/size changes
move the registration, authored declaration reorder does not, a forged running
environment refuses, and historical database hashes remain unchanged while
execution paths refuse them.

### R1-C — Project Kit, examples, publication, and consumer boundary

Owners: `modules/project`, examples, conformance, scripts/publication

- `project init` writes `plugin/main.luau` plus a real dependency module.
- Generated declarative adapters become one-module closures under the same API.
- `project build`, `check`, and `freeze` record every exact module/resource byte,
  module-manifest identity, and environment identity.
- The example projects exercise at least three modules and a Python-built JSON
  resource.
- `scripts/generate_public_contract.py` publishes the authored shape, grammar,
  limits, environment/resource APIs, identity preimages, and refusals from
  authoritative bytes. `docs/PUBLIC-CONTRACT.md` remains generated.
- Only after R1 passes and is released does the project template pin it and the
  current consumer split its plugin. That migration is not part of an upstream
  partially green working tree.

### R1-D — Gates

Run the repository `build-project` and `post-change-validation` workflows, the
full local gate, both example conformance runs, a database migration/reopen audit
test, and a clean-machine Project Kit bootstrap. Treat tests as retained only
when they protect a public contract or the reproduced identity/persistence risk.

## Release R2 — Locked Luau packages

Entry gate: a focused decision must choose registry/provenance/signing policy
and exact lock schema before implementation.

Owners: `script`, `schema`, `deployment`, `operator` registration verification,
`project`, publication/release tooling, conformance, and public contract.

Add request grammar:

```text
PackageRequest = "@" Segment ("/" Segment){0,15}
```

The same 256-byte request/resolution bound applies. Package aliases and exports
resolve directly to canonical closure keys; no runtime registry/cache path is
visible. Adding this grammar changes the resolver contract in
`plugin_environment_hash`, so affected registrations move even if their source
bytes do not.

`umbraflow-luau.lock.json` is exact JCS and pins package id/version, immutable
archive/source sha256, export map, provenance, and license metadata. `project
deps resolve` is the sole networked path. `deps verify`, build, check, and freeze
operate offline over verified materialized bytes and reject archive traversal,
alias/export collision, mutable selectors, missing provenance/license, missing
bytes, and hash mismatch.

The project authoring schema receives an explicit new generation if it gains a
lock-path member; there is no silent extension of v2. Registration format stays
3 only if its member set and semantics are unchanged: package exports already
collapse into the module manifest and the moved environment hash. Otherwise it
increments again.

The immutable release contains the exact lock and materialized source files.
Its artifact manifest pins `luau_lock_hash`, `plugin_module_manifest_hash`, and
each shipped path/digest. Runtime registration proves execution bytes; release
identity separately proves provenance, license, and export-map facts. Consumer
migration/release notes explicitly name this second authoring generation rather
than pretending R1 had package support.

## Release R3 — Explicit filesystem and network capability programs

Entry gates: a capability-manifest decision, an effect-result/evidence decision,
and a security review must all pass before implementation.

Owners: `operator`, `project`, `script`, deployment/schema/public contract,
durable evidence storage, a separately isolated effect host, conformance, and
release publication.

A capability program is a separately pinned Luau module closure. An effect/tool
request names its program registration and exact declared scope. Before dispatch
the Operator durably records request identity plus policy/user grant. The
isolated host receives only those handles, initially confined filesystem
read/write roots and bounded HTTP origin/method/redirect/size/timeout access.

Effect body data flow is exact:

1. The host writes response bytes atomically to an Operator-owned immutable CAS.
2. A canonical receipt binds request id, capability-program hash, grant hash,
   terminal outcome, body kind, exact sha256, and size.
3. The receipt commits only after the blob is durable; a partial blob has no
   visible receipt.
4. The next pure call receives the schema-validated receipt in its exact JSON
   input and a per-call evidence closure containing exactly the receipt-named
   blobs. `evidence.read*` verifies digest/kind/size and cannot select any other
   object.
5. Replay bundles contain and re-verify those exact blobs, so a digest never
   becomes a hidden lookup whose bytes may disappear or change.

Recovery follows the existing delivery-outcome model: proven-undispatched work
may run; a committed receipt is reused; unknown delivery becomes explicit
reconciliation input and is never blindly replayed. Revocation prevents work
not yet dispatched and remains recorded beside already dispatched evidence.

Threat-model tests cover path/symlink escape, UNC/device paths, DNS rebinding,
redirects, local-address access, environment/secret leakage, response bombs,
timeouts, cancellation, partial writes, crash recovery, transport-unknown,
revocation, replay completeness, and audit completeness.

## Documentation closure

The accepted ruling is reflected immediately in current terminology and
architecture notes as an accepted-but-not-yet-implemented replacement. Generated
public contract, schemas, examples, and runtime API prose change only in the R1
atomic cut so documentation never claims that unreleased code exists. Historical
decisions and archived plans remain unchanged.

## Review findings and disposition

| Round-1 blocking finding | Revision |
|---|---|
| Environment semantics absent from identity; M1/M2 split | Added `plugin_environment_hash`; R1 is one atomic module/resource/identity release |
| “v2” conflicted with current format 2; DB data policy absent | Registration schema v2 carries format 3; exact-pair migration preserves history as audit-only |
| Resolver/cache/error semantics incomplete | Added complete ASCII grammar, bounds, canonical cache key, cycle, cached-error, and terminal-error rules |
| VFS preimage and second consumer break unclear | Defined exact embedded resource rows and moved resources into R1 before consumer migration |
| Package owners/release closure incomplete | Added all owners, entry decision, exact lock-to-module-to-release linkage, and explicit later authoring generation |
| Capability receipt body was a hidden/unavailable input | Added durable CAS, exact receipt binding, per-call evidence closure, replay blobs, and recovery semantics |
| Current docs deferred to final milestone | Current terminology/architecture are updated now; generated implementation contract waits for R1 |
| Observable limits absent from environment preimage | Fixed every R1 numeric ceiling, made constants the shared enforcement/material source, and added a vendored-Luau gitlink parity gate |
| Resource reader name/call grammar incomplete | Added exact dotted ASCII grammar, byte/segment bounds, one-string arity, exact comparison, and refusal rules |

## Review gate

The independent reviewer must re-check this revision. Any remaining blocking
finding is resolved before implementation begins. A passing verdict authorizes
starting R1-A in the working tree, but R1 is not complete or releasable until
R1-A through R1-D all pass together.
