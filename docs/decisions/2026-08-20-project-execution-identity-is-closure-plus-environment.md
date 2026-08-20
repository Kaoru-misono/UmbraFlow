# 2026-08-20 — Project execution identity is closure plus environment

## Decision

Project code is a multi-file, non-concatenated Luau module closure. This ruling
defines the complete identity, resolver, persistence, resource, package, and
effect-result contract selected by independent review.

A ProjectPlugin execution identity is the pair of:

1. `plugin_module_manifest_hash`, the sha256 of exact JCS containing the entry
   logical name and every logical module name plus exact source sha256; and
2. `plugin_environment_hash`, the sha256 of exact material that describes the
   Luau implementation, compiler options, bridge source, global and frozen-table
   surface, resource APIs, module resolver grammar, cache/cycle/error semantics,
   and the observable contract version of each host function.

ProjectRegistration format **3** pins both values. SessionManifest already pins
the complete project registration hash and therefore transitively pins the code
closure and execution environment. The current format value is 2 despite the
registration schema's `v1` filename; schema generation and document format are
separate numbers and neither is reused.

The public project-directory generation changes to `umbraflow-project/v2`, the
registration schema changes to `project-registration-v2`, and the attestation
set changes to schema/set version 2. Attestation v2 pins the complete project
registration hash rather than copying a subset of plugin identity fields. The
old generations are not accepted for new loading or execution.

## Module names and `require`

One project segment is ASCII `[a-z][a-z0-9_-]{0,63}`. A canonical project
module name is one through sixteen segments separated by one `/`, at most 256
bytes in total. It has no leading or trailing slash, empty segment, dot segment,
backslash, control byte, non-ASCII byte, or filename extension convention.

Before package support, `require` accepts exactly one of:

- a canonical module name, resolved from the project root;
- `./` followed by a canonical module name, appended to the caller module's
  directory; or
- one or more complete `../` prefixes followed by a canonical module name,
  removing that many caller-directory segments before appending the name.

The request itself and the resolved name are each bounded to 256 bytes.
`./a/../b`, `a/./b`, repeated slashes, a leading `/`, a bare `.`/`..`, traversal
above the logical root, and every spelling outside the grammar are refused; they
are never normalized into acceptance. Different legal requests that resolve to
the same canonical name select one cache key and one module value.

Package support later adds `@` plus one segment alias and zero through fifteen
`/segment` suffixes. That grammar and its contract version enter
`plugin_environment_hash`, so enabling it moves every affected registration
even when source bytes do not.

Each fresh VM owns module states keyed only by resolved canonical logical name.
A module executes at most once and returns exactly one non-`nil` Luau value.
Self-require and indirect cycles have one stable cycle refusal. Deterministic
resolution and module-execution errors are cached as a stable refusal class plus
a bounded message with no pointer or traceback address. VM memory exhaustion,
instruction/wall-time exhaustion, cancellation, and host invariants terminate
the whole invocation; plugin `pcall` cannot turn them into a cached dependency
result. The entry module alone is presented to the ProjectPlugin bridge and must
return the exact five-function export table; dependency values carry no
ProjectPlugin authority.

Runtime module loading observes no filesystem, working directory, search path,
registry, network, or environment variable. Computed require strings are
allowed because successful resolution remains inside the pinned closure.

## Execution environment material

`plugin_environment_hash` binds every admission or runtime limit that can
change whether the same registration succeeds or refuses. One named C++
constant supplies each enforced value and the numeric value emitted into
`pluginEnvironmentMaterial()`; the implementation does not enforce one literal
and attest to another. This includes VM memory, interrupt and wall-time budgets,
module/resource counts and byte ceilings, source/bytecode totals, module and
resource name/request bounds, entry-point bounds, JSON value depth/node/text
bounds, stack reservation derived from those bounds, and every bounded error
copy visible to Luau or the host. Moving any value moves the environment hash.

The pinned Luau revision is not an unchecked prose string. A repository parity
gate reads the `modules/script/external/luau` gitlink and compares it with the
exact revision embedded in the environment material. Compiler options are
emitted from the same constants passed to `luau_compile`. The pinned Luau
revision also owns interrupt safepoint cadence; the material states the
observable non-GC safepoint contract beside that revision.

## Project resources

The first module-capable public generation also replaces `artifact_blobs` and
`project_artifact_roots`; this is not deferred to a second consumer break. A
registration contains canonical `project_resources` rows sorted by UTF-8 name:

```json
{"kind":"json|utf8|bytes","name":"logical.name","sha256":"...","size":123}
```

The array itself is the canonical resource manifest inside the exact
registration JCS. Name, kind, exact-byte hash, and byte size therefore all enter
the registration identity. The registrar recomputes every row from the supplied
bytes and refuses missing, duplicate, extra, wrong-kind, wrong-size, or
wrong-hash resources.

A resource segment is ASCII `[a-z][a-z0-9_-]{0,63}`. A ResourceName is one
through sixteen such segments separated by one `.`, at most 128 bytes in total.
It has no empty segment, leading or trailing dot, control byte, non-ASCII byte,
case folding, or Unicode normalization.

The pure VM publishes `resource.readJson`, `resource.readText`, and
`resource.readBytes`. JSON is decoded and deeply frozen, UTF-8 text is verified
at admission, and bytes are a Luau byte string. All readers are name- and
kind-exact, bounded, cached per VM, and backed only by the registered resource
closure. Python remains the offline owner of JCS, database extraction,
database-to-corpus transforms, and packaging; the shipped runtime consumes its
pinned outputs and contains no Python interpreter.

Each reader accepts exactly one Luau string whose bytes already satisfy
ResourceName and compares it byte-for-byte against the sorted registered
resource closure. Empty, non-string, extra-argument, non-ASCII,
control-containing, overlong, and otherwise non-canonical names refuse before
lookup; nothing is normalized. The resource-name grammar version, 128-byte
request limit, reader arity/type contract, return kind, failure behavior, and
per-VM cache semantics are part of `plugin_environment_hash`.

## Persisted Operator history

Existing Operator databases are not discarded and historical hashes are not
rewritten. The exact-pair schema migration rebuilds the registration index into
generation-neutral columns: registration hash, registration format, plugin id,
plugin identity kind, plugin identity hash, and canonical registration bytes.
Existing format-2 rows are classified as `single_source`; format-3 rows are
classified as `module_manifest`. All foreign keys, session hashes, journal rows,
and audit evidence retain their exact historical values.

Format-2 registrations and sessions are audit-only after migration. Read-only
inspection/export may retain their canonical bytes, but production registration,
session creation, resumption, planning, or effect execution refuses them with a
specific legacy-generation diagnostic. There is no legacy VM or execution
fallback. This preserves accepted persisted evidence without pretending it ran
under the new module environment.

## Locked packages and immutable releases

A package lock is an exact-JCS build input that pins package identity, immutable
source/archive sha256, export map, provenance, and license metadata. Resolution
may use the network; verify/build/freeze do not. Verified exported source bytes
are materialized under `@alias/...` and enter the normal module manifest.

An immutable release carries the exact lock document and every materialized
module byte. Its artifact manifest pins the lock sha256, module-manifest sha256,
and paths/digests of the shipped sources. Thus runtime semantics are bound by
the plugin registration while provenance, license, and export-map facts are
also bound by the release root. Registry and signing selection is a required
decision before package support is implemented.

## External capability results

Filesystem and network authority belongs to separately registered Luau
capability programs, never the five pure functions. Before dispatch, the
Operator durably records the effect request and the exact policy/user grant.
The effect host receives only scoped capability handles.

Response bodies are first written atomically into an Operator-owned immutable
content-addressed evidence store. A canonical receipt binds request id, program
and grant hashes, terminal outcome, body kind, exact sha256, and byte size. The
receipt is committed only after the blob is durable. A subsequent pure call is
given the schema-validated receipt in its JSON input and a verified per-call
evidence closure containing exactly the receipt-named blobs. Host readers expose
those bytes by digest; they cannot select any blob the explicit input did not
name. Replay bundles carry and re-verify the same blobs.

Recovery never blindly repeats an effect whose delivery is not proven absent.
It reuses the existing delivery-outcome discipline: a provably undispatched
effect may run, a committed receipt is reused, and an unknown transport outcome
becomes explicit reconciliation input. Partial writes never publish a receipt;
grant revocation prevents undispatched work but does not erase the grant hash
under which an already dispatched result was obtained.

## Consequences

- The module VM, resource VM, project-directory generation, registration format
  3, loader/registrar, ledger migration, Project Kit, examples, conformance, and
  generated public contract form one release boundary. Internal work may be
  staged, but no subset is committed as a supported contract or released.
- Existing data remains auditable but cannot silently resume under changed
  semantics.
- A future resolver/API semantic change moves `plugin_environment_hash`; a
  registration member-set change also increments the registration format.
- A vendored Luau revision or observable limit cannot move without moving and
  verifying the environment identity.
- Resource declarations and reader requests share one exact grammar; schema and
  runtime validators cannot introduce alternate spellings.
- Consumer migration occurs only after the complete module-and-resource release
  passes the full gate.
- Package and external-capability releases have their own entry decisions and
  security gates; neither is implied by merely shipping the module kernel.
