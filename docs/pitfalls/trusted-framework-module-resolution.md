# Trusted Framework module resolution

Release-owned Luau runs under two *loaders* — the closed-graph loader in
`ffi/program-runtime.cpp`, shared by the `PureDataProgram` and
`ScopedToolProgram` program types, and the trusted Engine loader in
`ffi/environment.cpp` — and they are two implementations of one contract. Both
entries below come from that shape: a capability proven on one path is not
thereby present on the other, and anything a project script can name across the
trust boundary is a projection somebody wrote by hand, never a chain it can
follow.

## A pure Framework module boots in ProjectPlugin but fails in Engine::create

### Symptom

A release-owned Luau module that uses an exact `@umbraflow/...` dependency works
through `PureDataProgram`, while a full trusted Framework VM fails during
`Engine::create` with a nil `require` at the dependency's source line.

### Root cause

`PureDataProgram` and the full Framework loader are separate execution paths.
Adding an exact resolver to the pure path does not give the trusted loader the
same capability. The trusted loader had no resolver of its own: it bound each
module's exports as a bare field of the shared framework environment, which is
the globals table of every module chunk it runs afterwards, so an
intra-Framework dependency was a bare identifier and `require` was nil.

### Fix

Each `FrameworkModule` declares one canonical reserved resolver name and one
dependency depth. The trusted loader validates every declaration before running
any source — canonical `@umbraflow/` spelling, no duplicate publication name, no
duplicate resolver name, and arrival in non-decreasing depth — then gives each
module a temporary `require` over a frozen snapshot holding the aliases of
earlier modules only, and unbinds it again as soon as that module returns, so it
cannot survive into the Project environment prototype. Unknown and forward
names, cycles, duplicate aliases, and a depth reordering therefore refuse the
Framework generation instead of falling through to a host package mechanism.
Task's bundle adapter emits release-owned modules in non-decreasing declared
depth, which is the topological order the loader then insists on.

Loading a module binds no name in any environment. A module's frozen exports go
into a VM registry table that nothing in either environment can name, itself
frozen once loading ends. `require` of a reserved name is the only route from
one Framework module to another, and a bare identifier inside a Framework module
means the trusted VM's own globals — the Luau standard library plus the host
installer's tables — on both execution paths.

### Regression check

Build `test-script` and `test-task`, then run:

```powershell
build/x64-debug/bin/test-script.exe --test-case="Trusted Framework require*"
build/x64-debug/bin/test-task.exe --test-case="business framework publication is fail closed"
```

## A projected framework name overwrites the standard library a project script sees

### Symptom

A project script calls `utf8.char`, `table.sort` or `os.date` and gets "attempt
to call a nil value" from a table with the right name and the wrong contents.
Nothing failed at boot, and the framework module the value actually came from is
named nowhere in the project source.

### Root cause

A project environment is built by copying values in by name.
`installProjectEnvironmentPrototype` in
`modules/script/source/script/ffi/environment.cpp` copies
`projectStandardGlobals()` out of the sandboxed globals, then the host
installer's `projectGlobals`, then `EngineConfig::frameworkProjectGlobals` out
of the loader's module registry — all three with `lua_rawsetfield` into one flat
table, in that order, and nothing compares a name against the names already
written. The prototype deliberately carries no metatable, which is what makes
the project surface a whitelist rather than a chain; it also means there is
nothing behind an entry, so the last writer of a name owns it outright.

A framework projection is spelled with the module's publication name, which is
its `.luau` file stem. A module whose stem is `utf8`, `string`, `table`, `math`,
`os`, `coroutine`, `buffer`, `bit32` or `vector` therefore replaces that library
for every project script the moment the stem is whitelisted — silently, and
invisibly at the call site that breaks.

Inside the Framework this can no longer happen: the trusted loader binds no
globals at all (see the first entry), so a bare `utf8` in a framework module is
the standard library on both paths. The curated projection across the trust
boundary is the one place left that publishes a framework-built value under a
bare name.

### Fix

Never give a projected name the spelling of a standard-library global. The
authoritative list is `script::projectStandardGlobals()`, declared in
`modules/script/source/script/engine.hpp`; rename the projection rather than the
capture in the project script, because the script is reading the name the
language gave it.

### Regression check

`tests/task/test-framework-bundle.cpp`, case `loading the framework publishes no
trusted global`, checks every entry of `frameworkProjectGlobals()` and
`runtimeProjectGlobals()` against
`script::projectStandardGlobals()`, and in the same case asserts that the
trusted VM's global set is byte-identical before and after Framework loading —
which is the property that keeps this hazard confined to the projection:

```powershell
build/x64-debug/bin/test-task.exe --test-case="loading the framework publishes no trusted global"
```

A whitelist that case does not name is not covered. When a new one is added,
extend the array of whitelists there instead of checking the names by eye.
