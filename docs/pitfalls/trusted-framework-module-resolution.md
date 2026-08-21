# Trusted Framework module resolution

## A pure Framework module boots in ProjectPlugin but fails in Engine::create

### Symptom

A release-owned Luau module that uses an exact `@umbraflow/...` dependency works
through `PureDataProgram`, while a full trusted Framework VM fails during
`Engine::create` with a nil `require` at the dependency's source line.

### Root cause

`PureDataProgram` and the full Framework loader are separate execution paths.
Adding an exact resolver to the pure path does not give the trusted loader the
same capability. The trusted loader historically published module exports only
as bare Framework globals in input order, so a module source using reserved
dependency names had no resolver at all.

### Fix

Each `FrameworkModule` may declare one canonical reserved resolver name. The
trusted loader validates all declarations before running source, then gives each
module a temporary `require` over a frozen snapshot of earlier aliases. Task's
bundle adapter supplies a topological order for release-owned dependencies.
Unknown and forward names, cycles, and duplicate aliases therefore refuse the
Framework generation instead of falling through to a host package mechanism.
The loader removes `require` before the Project environment prototype is built.

### Regression check

Build `test-script` and `test-task`, then run:

```powershell
build/x64-debug/bin/test-script.exe --test-case="Trusted Framework require*"
build/x64-debug/bin/test-task.exe --test-case="business framework publication is fail closed"
```
