# Luau source standard

Measured 2026-08-13 against the current embedded runtime: seven `.luau` files,
3,125 lines and 119,155 bytes under `modules/task/runtime/`. This document names
only rules with an executable enforcer. A preference with no reader check and no
gate is a wish, not a coding standard.

## Reader-enforced rules

The build-time reader is `scripts/embed_luau.py::collect_sources`, invoked by
`cpp_embed_luau_sources` for every recursive `*.luau` input. It enforces these
rules before generating the embedded bundle:

- Source bytes are valid UTF-8. Invalid UTF-8 makes the embed command exit 1
  with `not valid UTF-8`.
- Source bytes contain neither NUL nor carriage return. Either byte makes the
  embed command exit 1 with the matching refusal. LF is the only line ending.
- The file stem is the module's global name and matches
  `[A-Za-z_][A-Za-z0-9_-]*`. An invalid stem makes the embed command exit 1.
- File stems are unique across the recursive runtime tree. A duplicate stem in
  another directory makes the embed command exit 1 and names both paths.
- Bundle order is normalized relative-path order. The generated entry name is
  exactly the file stem; there is no second declared module name.

The runtime loader consumes that order without re-sorting it. After a module
runs, its first returned value is frozen when it is a table and is bound in the
shared framework environment under the file stem. A module can capture an
earlier module while it loads; it cannot capture a later module then, because
that global is not bound yet. A global lookup deferred until an exported
function runs can see modules that were bound later. Ordering therefore defines
load-time visibility, not a general dependency checker.

The reader does not enforce file size, file splitting, helper extraction,
maximum function length, or a one-table-per-file source shape. Those preferences
are deliberately not standards here. In particular, the consumer convention
to split files and extract helpers where possible remains review advice unless
and until an executable rule can state the boundary without pretending that a
line-count threshold measures design.

## Existing executable coverage

The embedded-bundle contract test reparses every entry with the pinned Luau
parser, checks source hashes, checks sorted unique entry names, and recomputes
the bundle hash. VM creation then loads every framework module and refuses a
syntax/load failure or a module that returns no value. These checks cover syntax
and loadability; they are not a static type checker.

## `check_luau` ruling

No separate `scripts/check_luau` subset is needed.

Every exact source-file property worth enforcing in the current design already
has a refusal in the reader or the embedded-bundle contract test. Rechecking
UTF-8, line endings, names, uniqueness, order, syntax, or hashes in another
script would create two spellings of one rule. Checking only that a file starts
with `--!strict` would not type-check it and would enforce a label rather than a
property. The remaining preferences cannot be made falsifiable without
inventing arbitrary style thresholds.

Consequently no `check_luau` script is added and `scripts/ci-local.*` is
unchanged.

## `luau-analyze` ruling

`luau-analyze` is not a required gate.

The repository pins the Luau VM and compiler but forces `LUAU_BUILD_CLI=OFF`
and does not link `Luau.Analysis`; `luau-analyze` is not produced by the project
build and was not present on `PATH` in the measurement above. More importantly,
the framework uses an ordered shared environment: files capture earlier module
globals directly and may defer later-module global lookup until call time. It
does not use Luau's `require` resolver. A stock per-file analyzer therefore does
not model the runtime's module linkage or private native chunk argument.

Making analysis required would first need a maintained definition environment
and resolver that reproduce those runtime bindings, plus a falsification proving
that a real type error goes red for the intended reason. That facility does not
exist today. Until it does, `--!strict` is authoring intent rather than an
enforced repository standard; this document does not claim otherwise.
