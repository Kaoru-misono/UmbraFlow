# Luau source standard

The embedded runtime is every `.luau` file under `modules/task/runtime/`. This
document names only rules with an executable enforcer. A preference with no
reader check and no gate is a wish, not a coding standard.

## Reader-enforced rules

The build-time reader is `scripts/embed_luau.py::collect_sources`, invoked by
`cpp_embed_luau_sources` for every recursive `*.luau` input. It enforces these
rules before generating the embedded bundle:

- Source bytes are valid UTF-8. Invalid UTF-8 makes the embed command exit 1
  with `not valid UTF-8`.
- Source bytes contain neither NUL nor carriage return. Either byte makes the
  embed command exit 1 with the matching refusal. LF is the only line ending.
- The file stem is the module's publication name and matches
  `[A-Za-z_][A-Za-z0-9_-]*`. An invalid stem makes the embed command exit 1.
- File stems are unique across the recursive runtime tree. A duplicate stem in
  another directory makes the embed command exit 1 and names both paths.
- Bundle order is normalized relative-path order, and the generated entry name
  is exactly the file stem.

The publication name keys the module's frozen exports in the loader's module
registry and is the spelling a host whitelist projects by. It is not a name any
Luau source can write. A module's reserved resolver name and its dependency
depth are declared in C++, in `modules/task/source/task/framework-bundle.cpp`,
and appear neither in the `.luau` file nor in its path.

The reader does not enforce file size, file splitting, helper extraction,
maximum function length, or a one-table-per-file source shape. Those preferences
are deliberately not standards here. In particular, the consumer convention
to split files and extract helpers where possible remains review advice unless
and until an executable rule can state the boundary without pretending that a
line-count threshold measures design.

## Loader-enforced rules

One embedded source can be admitted into more than one closure, and which
closure admits it is a C++ declaration rather than a property of the file.
`modules/task/source/task/framework-bundle.cpp` emits three lists over the same
bundle: `frameworkScriptModules()` for the trusted RuntimeModel Engine,
`pureFrameworkScriptModules()` for `script::PureDataProgram`, and
`scopedFrameworkScriptModules()` for `script::ScopedToolProgram` and
`script::ScopedToolSession`. The four scoped facades — `audit.luau`,
`screen.luau`, `tools.luau`, `workflow.luau` — are in the recursive tree the
reader collects but are admitted by the third list alone, so neither the trusted
Engine nor a pure program can name one, and the refusal names the module. Do not read
"every `.luau` file under `modules/task/runtime/`" as "every module every VM
loads".

`loadFrameworkModules` in `modules/script/source/script/ffi/environment.cpp`
runs the bundle in the order `EngineConfig::frameworkModules` arrives in, which
is not the reader's path order: `frameworkScriptModules()` re-emits the entries
it admits in non-decreasing declared dependency depth, stable within one depth.

- Every module declares one canonical reserved resolver name and one dependency
  depth; a case in `tests/task/test-framework-bundle.cpp` fails if an embedded
  module declares neither, because a module nothing can require is a silent way
  to get the load order wrong. Before any source runs, the loader refuses a
  non-canonical `@umbraflow/` spelling, a duplicate publication name, a
  duplicate resolver name, and a list that does not arrive in non-decreasing
  depth. A depth reordering is therefore
  a boot refusal, not a silently different dependency graph.
- While one module runs, its `require` resolves over a frozen snapshot holding
  the reserved names of earlier modules only. An unknown name, a forward name —
  and so any cycle — and a non-string argument each refuse the whole Framework
  generation. `require` is unbound again as soon as that module returns, so no
  later module inherits it and no project script can ever see it.
- A module's first return value is its exports: deep-frozen when it is a table,
  and bound in a VM registry table keyed by the publication name. Loading a
  module binds no name in any environment. The registry is frozen once loading
  ends, and no name in the framework or project environment can reach it.
- **A Framework module reaches another Framework module by `require` of that
  module's reserved name, and by nothing else.** A bare identifier is the
  trusted VM's own globals — the Luau standard library plus the host installer's
  tables — on both execution paths, and never another Framework module. There is
  no load-time-versus-call-time distinction left to reason about, because no
  module is ever bound in that namespace at either moment.
- The one route from a framework module to project-authored source is
  `EngineConfig::frameworkProjectGlobals`: a curated list of publication names
  copied out of the registry into the project environment. That projection is a
  separate mechanism from resolution — it publishes one frozen value under a
  bare name and opens no chain, a name no module bound fails the generation, and
  a projected name spelled like a standard-library global would replace that
  library for every project script (see
  [`docs/pitfalls/trusted-framework-module-resolution.md`](../pitfalls/trusted-framework-module-resolution.md)).

## Existing executable coverage

The embedded-bundle contract test reparses every entry with the pinned Luau
parser, checks source hashes, checks sorted unique entry names, and recomputes
the bundle hash. VM creation then validates every module declaration, loads
every framework module in declared-depth order, and refuses a syntax or load
failure, a module that returns no value, and every resolution refusal listed
above. A further case asserts that the trusted VM's global set is unchanged by
Framework loading, and that no name the three release whitelists project
collides with `script::projectStandardGlobals()`. These checks cover syntax,
loadability, linkage and namespace isolation; they are not a static type
checker.

## `check_luau` ruling

No separate `scripts/check_luau` subset is needed.

Every exact source-file property worth enforcing in the current design already
has a refusal in the reader or the embedded-bundle contract test. Rechecking
UTF-8, line endings, names, uniqueness, order, syntax, or hashes in another
script would create two spellings of one rule. The remaining linkage rules —
reserved names, dependency depth, earlier-only resolution — are declared in C++
and refused at boot, so a Luau-source linter could not see them in the first
place. Checking only that a file starts with `--!strict` would not type-check it
and would enforce a label rather than a property. The remaining preferences
cannot be made falsifiable without inventing arbitrary style thresholds.

Consequently no `check_luau` script is added and `scripts/ci-local.*` is
unchanged.

## `luau-analyze` ruling

`luau-analyze` is not a required gate.

The repository pins the Luau VM and compiler but forces `LUAU_BUILD_CLI=OFF`
and does not link `Luau.Analysis`; `luau-analyze` is not produced by the project
build, and it was not on `PATH` when this was measured on 2026-08-13. More
importantly, the framework resolves its dependencies through a host-installed
`require` that is not Luau's own resolver: it admits only the reserved
`@umbraflow/` names the C++ bundle adapter declares, resolves earlier modules
only, and disappears again when the requiring module returns. A stock per-file
analyzer models neither that linkage nor the private native surface each module
receives as its chunk argument.

Making analysis required would first need a maintained definition environment
and resolver that reproduce those runtime bindings, plus a falsification proving
that a real type error goes red for the intended reason. That facility does not
exist today. Until it does, `--!strict` is authoring intent rather than an
enforced repository standard; this document does not claim otherwise.
