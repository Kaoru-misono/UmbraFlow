# `modules/script` Architecture Knowledge

> **DIRTY (2026-07-29)**: This document was written against a `modules/script` that
> was only a VM wrapper. Since then the module gained the sandbox, the accounting
> allocator, interrupt cancellation, and — in `67e7e63` / `e89bc53` — the two-environment
> split (`environment.{hpp,cpp}`). The claims flagged below have been corrected in place,
> but the per-step narratives and the tests inventory have not been re-derived. Trust the
> code and `docs/plans/2026-07-29-three-layer-task-system.md` until resynced.

`modules/script` is UmbraFlow's Luau substrate. A C++23 host runs Luau pinned to 0.730 behind a
sandbox, an accounting allocator, and interrupt-driven cancellation, and boots each VM with **two
environments**: a trusted framework environment and a project environment that has no `__index`
chain reaching it. What the module still does not own is task policy — waiting, retry, steps,
interrupts — which belongs to the Luau framework in `modules/task/runtime/`.

## Current Capabilities and Limits

The module currently owns the following responsibilities:

- It exposes `uf::script::Engine` as the only public type, managing the complete lifetime of one
  embedded Luau `lua_State`. The declaration lives in
  `modules/script/source/script/engine.hpp`, and the implementation in
  `modules/script/source/script/ffi/engine.cpp`.
- It accepts Luau source and a chunk name, generates bytecode with the compiler, loads it into a new
  coroutine, and executes synchronously.
- It converts VM creation failures, compile/load failures, and script run failures into the
  repository's unified `Result<T>`/`Error` channel, rather than exposing Luau status codes or C API
  types to the caller.
- It confines the third-party C API, C-allocated memory, and `lua_State*` to the
  `modules/script/source/script/ffi/` boundary; the public header includes no Luau header at all.
- It wraps the vendored Luau build under `modules/script/external/`;
  `modules/script/external/CMakeLists.txt` enables only the libraries the host needs.

It deliberately does not own the following responsibilities:

- It does not own observation, page resolution, recognition, action, trace, or lease; these
  capabilities live in modules such as `modules/annotation`, `modules/engine`, and
  `modules/controller`, and `script` currently connects to none of them.
- It does not own Windows capture or input. `modules/script/manifest.txt` has no
  `platforms = windows`, so it is itself a cross-platform module; strict-background delivery remains
  the responsibility of `controller` and the composition root.
- It does not provide task-file loading, `require`, multi-file subtasks, task queues, hot reloading,
  persistent state, or host binding; the public surface accepts only the `std::string_view` source
  the caller already holds.
- It does no Luau offline analysis, JIT, CLI, Web, or Luau's own tests. The build links only
  `Luau.VM` and `Luau.Compiler`; `Luau.CodeGen`, `Luau.Analysis`, `Luau.Config`, and `Luau.Require`
  are all absent from the module's dependencies.
- It does not own task policy. Waiting, retry, step nesting, and the interrupt registry live in the
  trusted Luau framework under `modules/task/runtime/`, not here. `script` supplies the substrate
  those policies run on: environments, freezing, budgets, and the seam through which a host installs
  its own tables.

What it *does* own, and did not when this page was first written, is the safety substrate:

- `installSandbox` (`modules/script/source/script/ffi/sandbox.cpp`) strips the dangerous globals
  `luaL_sandbox` leaves behind (`getfenv`, `setfenv`, `newproxy`, `gcinfo`, `coroutine`, `debug`,
  and `_G`) plus the residual clock and RNG entry points (`os.time`, `os.clock`, `os.date`,
  `math.random`, `math.randomseed`), then runs `luaL_sandbox()`.
- `createStateWithQuota` (`ffi/allocator.hpp`) installs an accounting allocator enforcing a
  per-task hard memory ceiling; over-quota growth surfaces as a catchable `LUA_ERRMEM` rather than
  dragging the host down.
- `ffi/cancellation.cpp` arms an interrupt callback driven by a stop token, an instruction budget,
  and a `maxRuntime` deadline. Its GC-context guard (`if (gc >= 0) return;`) is the first statement
  in the callback, per the hardening ledger's hard red line.
- `ffi/environment.{hpp,cpp}` builds the two environments described below.

`docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` and
`docs/plans/2026-07-21-luau-integration-plan.md` are the historical records of this work. Both
predate the two-environment design and still teach `luaL_sandboxthread` for script-global isolation;
see "Two Environments" below for why that mechanism was rejected.

The module is in the product link closure. `modules/task/manifest.txt` declares `script` a public
dependency, and `entry/CMakeLists.txt` links `${PROJECT_NAME}_task` into the `umbra-flow` CLI
support library, so the substrate reaches the shipped executable through `task`. Inbound includes of
`script/engine.hpp` come from `modules/task` (`capability-surface.hpp`, `framework-bundle.{hpp,cpp}`,
`task-host.cpp`) and from `tests/script/` and `tests/task/`.

### Two Environments

Environment isolation in Luau is **per-closure, not per-thread**: `luau_load` takes the env table a
chunk's closure carries, and a new thread's globals table is copied from its parent. So
`luaL_sandboxthread`'s proxy shape cannot separate two trust levels on one VM — that proxy shape is
exactly the `_G` escape the design rules out. It is a **rejected** mechanism here, not a pending one.
Two explicit env tables can do what it cannot:

- The **framework environment** is a writable table whose frozen metatable chains `__index` to the
  main globals. Trusted framework modules load under it and keep their own globals off the main
  table.
- The **project environment** is an explicit whitelist with **no metatable at all**. That absence is
  the isolation property: with no `__index` there is no chain to the framework environment or to the
  main globals, so the denial list holds structurally rather than by enumeration. A whitelisted name
  missing from its source table fails the generation instead of silently producing a thinner
  environment.

Both live only in the VM registry, so neither is reachable from any global table. The project
environment is registered as a **frozen prototype**, and `pushProjectEnvironment` gives each run a
fresh writable shallow copy — globals a run writes die with that copy, while the values it shares
with the prototype stay frozen.

## Execution Flow

### Public Surface

The public surface in `modules/script/source/script/engine.hpp` is `uf::script::Engine` plus the
configuration and freezing vocabulary it needs:

- `Engine::create(EngineConfig const& = {}) -> Result<Engine>` is the named factory; VM allocation
  may fail, so there is no public constructor. `EngineConfig` carries the stop token, the memory and
  instruction budgets, `maxRuntime`, the `frameworkModules` bundle, the **three** host seams
  (`HostTableInstaller` and `PrivateCapabilityInstaller`, both returning `Status`, plus
  `RaisedErrorClassifier`, added on 2026-07-29 in `c37ee5b`), and the two whitelists
  `projectGlobals` and `frameworkProjectGlobals`.
- `RaisedErrorClassifier` reads the automation kind out of a value a run raised and nobody caught,
  deciding by the carrier's host userdata tag alone. The script module owns no error vocabulary of
  its own, so without it every uncaught host error reaches the boundary as `InvalidResource` — a
  task that timed out and did not catch it would be reported, and traced in `run.finished`, as a
  malformed script. `modules/task` supplies it through
  `CapabilitySurface::raisedErrorClassifier()`. A hard cancel is classified before it runs and never
  reaches it, so a classifier cannot downgrade a cancel into a catchable kind.
- `deepFreeze(lua_State*, int index) -> Status` recursively marks a table, everything reachable from
  its values, and every metatable on the way read-only, metatable-first. It also enforces the two
  structural rules a project-visible host object must satisfy: every metatable carries a
  `__metatable` field (without it `table.clone` returns a **mutable** copy carrying the **same**
  metatable, so identity-by-metatable could be forged), and `__index` is a table, never a function.
- `deepFreezeMetatable(lua_State*, int index) -> Status` checks and freezes a table *as* a
  metatable, for the common case where a metatable is built and registered before it is attached to
  anything.
- `Engine::runNumber(std::string_view source, std::string_view chunkName)
  -> Result<double>` is the only execution entry point.
- `Engine(Engine&&)`, move assignment, and the destructor are public; `std::unique_ptr<Impl>` makes
  copy unavailable.
- Both `Engine::Impl` and the constructor that accepts `std::unique_ptr<Impl>` are private, so an
  `Engine` without a valid VM cannot be constructed.

The actual definition of `Result<T>` is in `modules/core/source/core/error/result.hpp` and is
`std::expected<T, Error>`. The `AutomationErrorKind`, `fail(...)`, and `automationErrorKind(...)` used
by the implementation live in `modules/domain/source/domain/error.hpp`.
`modules/script/manifest.txt` in fact declares both `core domain` as public; the current
implementation classifies errors with domain, and the tests then check for failures by that
classification.

### Creation Path

The synchronous data flow of `Engine::create()` is:

1. `luaL_newstate()` creates the main `lua_State`.
2. When it returns null, it returns `AutomationErrorKind::InternalInvariant` with the message
   `luaL_newstate returned null`; no half-initialized `Engine` is produced.
3. On success, `luaL_openlibs(state)` opens the full standard library.
4. `state` is handed to `std::make_unique<Engine::Impl>`.
5. The private constructor moves sole ownership into `Engine::m_impl`.

`Engine::Impl` is defined in `modules/script/source/script/ffi/engine.cpp`. It holds
`lua_State* m_state`, deletes copy/move, and calls `lua_close()` on a non-null state at destruction;
the raw pointer never crosses the FFI implementation file.

This pImpl layer serves two purposes. First, the public header depends only on `core`, `<memory>`, and
`<string_view>`, so the Luau ABI does not become `script`'s public ABI. Second, the C API's ownership
proofs and compiler warning suppression are concentrated in a single auditable file.

### Single-Execution Path

The data flow of `Engine::runNumber()`, in code order, is as follows:

1. It obtains the main state from `m_impl->m_state`.
2. It constructs `lua_CompileOptions`, explicitly pinning `optimizationLevel = 1` and
   `debugLevel = 1`.
3. It calls `luau_compile(source.data(), source.size(), &options,
   &bytecodeSize)`. The source is passed by pointer + size and does not require null termination.
4. The `char*` returned by `luau_compile()` is owned by the caller; null is mapped to
   `AutomationErrorKind::InternalInvariant`.
5. `scopeExit(...)` guarantees that all return paths call `std::free()`; its implementation lives in
   `modules/core/source/core/utility/scope-exit.hpp`.
6. It records `stackBase` with `lua_gettop(state)`, then creates and pushes a coroutine with
   `lua_newthread(state)` so it stays reachable.
7. A second `scopeExit(...)` always runs `lua_settop(state, stackBase)`, popping the coroutine and
   restoring the main stack.
8. `chunkName` is copied into a `std::string` to provide `luau_load()` with a stable null-terminated
   name.
9. `luau_load(thread, name.c_str(), bytecode, bytecodeSize, 0)` loads the bytecode; a non-`LUA_OK`
   result returns `AutomationErrorKind::InvalidResource`.
10. `lua_resume(thread, nullptr, 0)` runs the script synchronously. `LUA_YIELD` is explicitly rejected
    because the current host has no resume protocol; other non-`LUA_OK` statuses also return
    `AutomationErrorKind::InvalidResource`.
11. On success, if the stack is non-empty it converts the last return value with
    `lua_tonumber(..., -1)`; an empty stack returns `0.0`.

The error text is read from the top of the stack by the in-file helper `topError(lua_State*)`. If
`lua_tostring(..., -1)` returns null, it uses the fixed text `(non-string error value)`, so that error
reporting does not itself rely on the assumption that the value is necessarily a string.

"A new coroutine per run" is not "a new VM per run": multiple `runNumber()` calls on the same
`Engine` share the main `lua_State`. But globals no longer leak between runs. `runNumber` goes
through `runNumberInProjectEnvironment`, which builds a fresh project environment for that call from
the frozen prototype and discards it afterwards, and binds it to the run's thread with `lua_setfenv`
before `luau_load`. Binding the thread is the C++-side setfenv path the sandbox needs — Lua's own
`setfenv` was removed — and it matters twice: without it `LUA_GLOBALSINDEX` on that thread would
reach the main globals, and `luau_load` would pre-resolve import constants against the frozen (and
therefore `safeenv`-marked) main globals.

## Constraints That Must Remain True

### Fail-Closed Behavior That Already Holds Today

For the existing narrow API, a failure does not masquerade as a valid number:

- A VM or compiler buffer allocation failure returns `InternalInvariant`.
- A syntax error is encoded into error bytecode by Luau and becomes `InvalidResource` at the
  `luau_load()` stage.
- Load errors, a yield unsupported by the host, and — **when no `RaisedErrorClassifier` is
  installed** — every runtime error return `InvalidResource`. With a classifier installed (which
  the product path does, from `modules/task`), an uncaught host Tier B carrier is reported under its
  own kind, and only a value the classifier does not recognize — a string or table the script raised
  itself — stays `InvalidResource`. A hard cancel is classified as `Cancelled` before this branch
  and never reaches the classifier.
- Only when `lua_resume()` returns `LUA_OK` is the result read.

The fail-closed behavior here covers only "whether a single `runNumber()` can complete." It is not
equivalent to the product's action safety gate, because the current API has no capture, recognition,
or input capability at all.

### Ownership and Lifetime

- `Engine` uniquely owns the VM through `std::unique_ptr<Impl>`; move transfers ownership, and copy is
  inexpressible at the type level.
- `Impl::~Impl()` is paired with `lua_close()`, guaranteeing that leaving scope normally reclaims the
  entire VM.
- The C allocation from `luau_compile()` is paired with `std::free()` by the first scope guard;
  whether the code after `luau_load()` succeeds, fails, or a subsequent string allocation throws, the
  buffer is released.
- The coroutine is kept reachable temporarily by the main state stack; the second scope guard restores
  the original stack top, so repeated calls do not leave the coroutine permanently on the main stack.
- `source` and `chunkName` are call-scoped views and are not stored in `Engine`. `chunkName` is copied
  first when a C string is needed; the source is only borrowed during the synchronous compile call.

These mechanisms explain why the dangerous operations live in
`modules/script/source/script/ffi/engine.cpp`: `std::free()` and external handle management require
local `// SAFETY:` proofs, while outside the boundary only RAII values and `Result<T>` are visible.

### Threading Constraints

`Engine`'s header explicitly declares it NOT thread-safe: all VM calls must occur on the owning thread.
The existing implementation has no mutex; the planned watchdog can only set an atomic flag and cannot
call the Luau API from another thread, and the interrupt callback is not yet in place.

Therefore the currently verifiable invariant is thread confinement, not thread safety. A caller that
concurrently calls the same `Engine` is already outside the contract.

### Current State of Determinism

The compiler's optimization/debug level is pinned, and the determinism floor is now enforced rather
than planned:

- The residual real-time and randomness entry points are removed — `os.time`, `os.clock`,
  `os.date`, `math.random`, `math.randomseed` — so a script's only randomness is the host's seeded
  RNG and it can read no clock at all;
- Globals no longer leak across `runNumber()` calls: each run gets a fresh project environment;
- A host-controlled seeded RNG exists in `modules/task`; the seed is injected per run and recorded
  in `run.started`. Per `docs/plans/2026-07-29-three-layer-task-system.md` §10 the logical clock has
  been **deleted** (stage 3a, `f146329`): `DeterministicClock`, `ctx:now`, and that primitive are
  all gone. Three host facilities replace it — `ctx:deadline(ms)` mints an absolute instant a script
  cannot read back, `ctx:wait(deadline, interval)` pauses one turn against it and reports whether
  budget remains, and `ctx:settle(ms)` is a declarative bounded pause. A script therefore still
  reads **no clock at all**, but "wait a while" changed from counting loop iterations into actually
  waiting.
- Nothing yet restricts dictionary iteration results from entering decisions or serialization; that
  remains a convention.

So "running 1000 times with the same observation trace + seed yields fully identical results" is
still a veto gate rather than a discharged guarantee, but the substrate no longer contradicts it.
See section five of
`docs/plans/2026-07-21-product-form-and-roadmap.md` for the specific gate, and
`docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` for determinism-hardening details.

### Where strict-background Sits

The product's `background_only` is a fail-closed contract: when incompatible it must fail and must not
switch to foreground activation, global injection, or real cursor movement. That invariant is
currently carried by the automation/controller path, not implemented by `script`. `script` has no
input API and does not depend on the Windows-only `controller`, so it can neither violate nor prove
strict-background.

When host binding is wired in the future, Luau may obtain only a restricted capability and must not
bypass the observation lease, target generation, or controller compatibility gate; the roadmap also
requires validating backend capability and target compatibility before the VM state is created.

### Resource and Cancellation Invariants

These mechanisms now exist in the code (this section previously listed them as absent):

- an accounting allocator and a per-task hard memory quota (`ffi/allocator.{hpp,cpp}`);
- an instruction budget and a `maxRuntime` time budget, both read by the interrupt callback;
- an atomic cancel flag, the interrupt callback, `lua_break()`, and the abandon protocol
  (`ffi/cancellation.{hpp,cpp}`);
- the GC guard forbidding interrupts when `gc >= 0`, as the callback's first statement;
- `luaL_sandbox()` and recursive readonly host tables via `deepFreeze`;
- explicit removal of `getfenv`, `setfenv`, `newproxy`, `gcinfo`, `coroutine`, `debug`, and `_G`,
  plus the residual clock and RNG entry points.

`luaL_sandboxthread()` is **not** on that list and never will be: see "Two Environments" above.

One subtlety the code encodes and a reader should not lose: `lua_break()` raised while
`nCcalls > baseCcalls` surfaces as an **ordinary catchable error**, not `LUA_BREAK`. So
`InterruptState::broken` — not the resume status — is the truth about whether a hard cancel
happened, and `resumeChunkOnThread` checks both. Classifying that degraded break as a script error
would misreport a host control signal as a recoverable failure.

The hardening ledger's hard red line requires that cancellation abandon the coroutine after using
`lua_break()` in the interrupt, and never use `luaL_error()`, which `pcall` can swallow. It also
requires long-running C++ bindings to honor their own deadline/stop token; a VM interrupt cannot
preempt a stuck C++ call. Together the two form a "500ms total exit" rather than a single VM trick.
The per-binding half of that (veto 6, artificially blocking each primitive) **landed on 2026-07-29
in `1fb41a7` and now runs in CI**: `tests/task/test-veto-blocking.cpp`, which lives outside this
module because what gets blocked is the task layer's own primitives. Eight blockable primitives are
blocked in turn and all eight exit inside a 2 s budget; `deadline`, `random`, `terminal`, and
`raise` are exempt with stated reasons — `raise` mints its carrier out of VM allocations and
longjmps, so it reaches no host call at all. The roster and the reasoning are in
`docs/plans/2026-07-29-three-layer-task-system.md` §8.

The `key` primitive added on 2026-07-30 (`ed38124`) is blockable and has **no case of its own**: the
suite's fake sink blocks in `pressKey` exactly as it blocks in `click`, so what the blocked-click
case proves holds for a blocked key — same port, same gate. The count above is therefore still eight
cases, over a private surface that is now thirteen primitives rather than twelve.

## Relationship to the Product Runtime

### Current Callers

The inbound edges are `modules/task` and the tests. `modules/task` is the only non-test consumer: it
includes `<script/engine.hpp>` from `capability-surface.hpp`, `framework-bundle.{hpp,cpp}`, and
`task-host.cpp`, supplying the `uf` data tables through `HostTableInstaller`, the observation-cycle
primitives through `PrivateCapabilityInstaller`, and the `.luau` bundle through `frameworkModules`.

No `entry/`, `engine`, `annotation`, or `controller` source file includes `script/engine.hpp`
directly — `script` reaches the CLI transitively through `task`. This is a boundary that can be
verified directly by a repository reference search.

### Current Dependencies

`modules/script/manifest.txt` declares:

- public `core`: public functions return `Result<T>`;
- public `domain`: the implementation classifies automation failures with `AutomationErrorKind`, and
  the tests also consume that classification;
- private `Luau.VM Luau.Compiler`: Luau types do not appear in the public header.

The `cpp_define_module(...)` in `cmake/build.cmake` checks `${MODULE_PATH}/external/CMakeLists.txt`
before defining the module target and does `add_subdirectory(... EXCLUDE_FROM_ALL)`; Pass 1
establishes the `Luau.*` targets, and only Pass 2 resolves the private dependencies.

`modules/script/external/CMakeLists.txt` forcibly disables `LUAU_BUILD_CLI`, `LUAU_BUILD_TESTS`, and
`LUAU_BUILD_WEB`, then adds the fixed-location `modules/script/external/luau`. `.gitmodules` registers
that path as the `https://github.com/luau-lang/luau.git` submodule; the exact baseline recorded by the
integration plan is tag 0.730, commit `5bc7f4b23756f69f4669b419fa9034f117ccd6fe`.

### What Crosses the FFI Boundary

What crosses the Luau boundary is currently only:

- inbound: source bytes, length, compile options, chunk name;
- intermediate: the caller-owned bytecode buffer;
- VM control: `lua_State*`, load/resume status;
- outbound: a single `double` or the repository `Error`.

No C++ domain object, raw screenshot, controller handle, or callback crosses the boundary. This
extremely narrow surface makes the current proof easy to audit, and also leaves room to later turn
host objects into opaque handles.

Third-party headers are included only in `modules/script/source/script/ffi/engine.cpp`, and locally
suppress third-party warnings with the corresponding MSVC/Clang/GCC pragmas. The project's own targets
still apply the strict safety profile; the warning suppression does not spread to the whole module.

### Planned Integration

The product plan requires the C++ host to hold the screenshot, recognition, input, keypress ledger,
and trace; Luau consumes only a minimal, read-only, cancellable capability. This means that what
crosses the boundary in the future should be validated handles and serializable results, rather than
giving the script the controller, the file system, or a C++ raw pointer.

When wiring, the repository module graph must also remain acyclic. Currently `script -> core, domain`
and `engine -> core, domain, annotation` are independent of each other; any new dependency must be
decided by an actual composition design, not merely a convenience of letting `script` and `engine`
depend on each other.

## Tests

Host tests live in four files: `tests/script/test-script.cpp` (the substrate),
`tests/script/test-environments.cpp` (the two-environment split),
`tests/script/test-veto-suite.cpp` (the roadmap vetoes), and
`tests/script/test-adversarial-substrate.cpp` (the substrate-side adversarial suite, added
2026-07-29 in `2ebcf0c`). The `test-script` target in
`tests/CMakeLists.txt` links `${PROJECT_NAME}_script`, registers only when the module exists, and
inherits the 60-second timeout and the `CI` label.

`test-adversarial-substrate.cpp` pins that a `lua_break` landing in any non-yieldable C frame still
ends the run `Cancelled`: seven forms (`table.sort` comparator, `string.gsub` callback, generic-for
iterator, `__index`, `__newindex`, `__tostring`, and the `xpcall` error handler), each paired with a
control that every form runs to `mark()` when nothing breaks. The `xpcall` form is the one that
historically failed **silently**: Luau folds a failure inside an error handler into `LUA_ERRERR` and
hands the caller a plain `(false, "error in error handling")`. It is still silent here, but it is
contained — `resumeChunkOnThread` decides terminality on `runStatus == LUA_BREAK ||
control->broken`, and removing the `broken` disjunct turns six of the seven forms from `Cancelled`
into `InvalidResource`. The primitive- and script-surface side of the suite is
`tests/task/test-adversarial-surface.cpp`.

`test-environments.cpp` pins the isolation claims this page rests on: that the project environment
holds no name on the denial list, that it cannot reach the framework environment, that a failing
host-table installer fails `create` with its own error, and — asserted **from inside the framework
environment** — that a framework module cannot capture a dangerous global at load time. That last
case is why `installSandbox` strips those globals *before* the bundle loads rather than after.

`test-script.cpp`'s original six doctest cases pin the following behavior:

- `Engine runs a Luau script and returns its numeric result`: verifies that `create()` succeeds, the
  compile/load/resume happy path, and `1 + 2 -> 3.0`.
- `Engine reports a compile/load error as a recoverable failure`: verifies that erroneous source
  returns a failure and is classified as `AutomationErrorKind::InvalidResource`.
- `Engine reports a runtime error as a recoverable failure`: verifies that the script `error('boom')`
  does not escape the C++ boundary and likewise becomes `InvalidResource`.
- `Engine returns zero when there is no numeric result`: two subcases pin, respectively, that a missing
  return value and a string return value both yield `0.0`.
- `Engine does not accumulate state across repeated runs`: the same VM executes 500 times and remains
  stable; this proves only that the main stack does not keep accumulating, not that globals are
  isolated.
- `Engine is move-only and usable after a move`: verifies that after ownership moves, the target
  `Engine` can still execute a script.

Sandbox, memory quota, cancellation, budgets, globals isolation, and `deepFreeze` are all pinned now
— by `test-script.cpp`'s later cases and by `test-veto-suite.cpp`, which covers vetoes 1-5. Veto 6
(each long-running binding artificially blocked, total exit still within budget) is in CI too, but
**not in this module**: it is `tests/task/test-veto-blocking.cpp` (2026-07-29, `1fb41a7`), because
what has to be blocked is the task layer's primitives and the ports behind them. What is still
unpinned here is host binding and strict-background behavior, neither of which is this module's to
prove. Luau upstream tests do not enter the project CI because of `LUAU_BUILD_TESTS=OFF`.

The roadmap's six vetoes must be filled in as a host regression suite:

1. infinite loops in both plain and nested `pcall` are irrecoverably stopped within the 500ms total
   budget;
2. unbounded allocation, deep recursion, and heavy string operations terminate only the corresponding
   task;
3. files, networks, processes, environment variables, dynamic loading, and the real clock are
   inaccessible;
4. replaying the same observation trace and seed 1000 times yields fully identical action traces and
   final state hashes;
5. a new generation's compile/self-check failure does not affect the old generation, and a successful
   switch does not mix objects;
6. each long-running C++ binding still satisfies the cooperative-cancel total budget when artificially
   blocked.

The hardening ledger further requires covering nested host table freeze, the five globals that survive
sandboxing, the GC interrupt guard, non-yieldable contexts such as the `table.sort` comparator and the
`string.gsub` callback, and the end-to-end 500ms test of "binding drain + top-up." Only when these
tests land does the 0.730 spike conclusion get upgraded from one-off evidence to a continuous
repository guarantee.

## Work Required Before Product Integration

The extension order is governed by `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md`, not by the
convenience of the current `Engine`. It is recommended to understand the work after the existing
two-step foundation as the following seams:

1. ~~Add sandbox setup inside `modules/script/source/script/ffi/`~~ — **done**, but not in the shape
   this step described. Host tables are registered and recursively frozen, the residual globals are
   removed, and `luaL_sandbox()` runs; the per-task isolation is **two explicit environments**, not
   `luaL_sandboxthread()`, which was rejected for the reason given under "Two Environments".
2. Install an accounting allocator so that the memory quota becomes a task-owned policy. OOM itself is
   an ordinary error that the script can catch; a true halt must rely on the allocator's hard quota and
   the host's stop semantics, and must not mistake `LUA_ERRMEM` for a non-swallowable cancel.
3. Add cancellation/budget state: other threads only write the atomic state, the interrupt callback on
   the VM owning thread calls `lua_break()` in a non-GC context, and after the host receives
   `LUA_BREAK` it destroys and never resumes that coroutine.
4. First land the roadmap's six vetoes and the ledger's narrow-boundary cases as `tests/script/`
   regressions; re-run the interrupt, sandbox, and determinism suites on every Luau upgrade, which is
   also the reason for pinning 0.730 exactly.
5. Only after the vetoes pass, add the read-only opaque handles for recognition/page, along with the
   observe/act/wait host calls. Their schema and action evidence should obey
   `docs/plans/2026-07-22-annotation-design.md` and the existing engine contract, rather than being
   reinvented in the Luau binding.
6. Only at the end does the composition root connect task execution to the strict-background controller
   and trace. Complete the backend capability/target compatibility gate before VM creation; every long
   C++ binding must be bounded and cooperative-cancellable.

`docs/plans/2026-07-21-product-form-and-roadmap.md` is the product-level authority: it defines the six
vetoes, P0's 500ms Ctrl-C goal, determinism/trace, and `background_only`.
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` supplements the task semantics, especially
D5's coroutine + interrupt dual budget, D9's whitelist sandbox, and the P0 lifecycle of "a brand-new
VM per task."

If Luau cannot pass the sandbox, non-swallowable cancellation, or the 500ms stop goal, the roadmap
requires reopening an independent worker selection; the vetoes must not be bypassed by broadening the
"trusted script" assumption. Conversely, before these gates pass, the existing `Engine::runNumber()`
should continue to hold a minimal verification interface and should not be wrapped into a seemingly
complete product runtime.
