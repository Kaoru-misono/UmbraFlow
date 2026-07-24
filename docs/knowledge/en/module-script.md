# `modules/script` Architecture Knowledge

`modules/script` is UmbraFlow's current minimal Luau embedding layer. A C++23 host can run Luau
pinned to 0.730, but the existing code only creates a VM, executes source synchronously, and returns
errors. It does not yet have the sandboxing, cancellation, or resource quotas required for
unattended execution.

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
- Most critically, it currently provides no security sandbox, accounting allocator, interrupt
  cancellation, instruction budget, or time budget. `Engine::create()` calls `luaL_openlibs()` and
  does not call `luaL_sandbox()` or `luaL_sandboxthread()`.

This boundary is an intentional staging split, not a security promise. The authoritative status is
recorded in `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md`: the unchecked items there must be
completed before the module enters the product execution path.
`docs/plans/2026-07-21-luau-integration-plan.md` likewise marks the existing implementation as steps
1-2 complete, while sandbox/cancellation, the veto suite, and observe/act/wait host handles remain
open.

The module enters no executable today. Its only external include is `tests/script/test-script.cpp`.
The three executable link closures in `entry/CMakeLists.txt` are:

- `umbra-flow` links `engine` through `${PROJECT_NAME}_cli_support`, and additionally links
  `controller` on Windows;
- `m0-demo` links `controller`, `vision`, and `image` through the support library;
- `umbra-workbench` links the workbench support, `engine`, `controller`, `image`, and Dear ImGui.

None of the three links `${PROJECT_NAME}_script`, and `modules/engine/manifest.txt` does not depend on
`script` either. As a result, the static library today is instantiated only in `test-script`. This
preserves the already-validated embedding foundation while avoiding wiring a VM that is explicitly
"unsandboxed, non-cancellable, and quota-free" into a strict-background unattended product.

## Execution Flow

### Public Surface

The only public surface that actually exists in `modules/script/source/script/engine.hpp` is
`uf::script::Engine`:

- `Engine::create() -> Result<Engine>` is the named factory; VM allocation may fail, so there is no
  public constructor.
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

"A new coroutine per run" is not "a new VM per run." Multiple `runNumber()` calls on the same `Engine`
share the main `lua_State` and the global table; global modifications opened by the current
`luaL_openlibs()` can persist across calls. The new coroutine currently isolates only the execution
stack, and works together with the stack guard to prevent thread objects from accumulating on the main
stack. True per-task environment isolation is not yet implemented.

## Constraints That Must Remain True

### Fail-Closed Behavior That Already Holds Today

For the existing narrow API, a failure does not masquerade as a valid number:

- A VM or compiler buffer allocation failure returns `InternalInvariant`.
- A syntax error is encoded into error bytecode by Luau and becomes `InvalidResource` at the
  `luau_load()` stage.
- Load errors, runtime errors, and a yield unsupported by the host all return `InvalidResource`.
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

Currently only the compiler's optimization/debug level is pinned, and a single coroutine is executed
synchronously. This is not enough to form the determinism the roadmap requires:

- The full standard library remains open, and real time and default randomness capabilities have not
  yet been replaced;
- Globals of the same `Engine` can leak across `runNumber()` calls;
- There is no host-controlled logical clock, fixed-algorithm RNG, state hash, or action trace;
- There is nothing restricting dictionary iteration results from entering decisions or serialization.

So "running 1000 times with the same observation trace + seed yields fully identical results" is
currently a veto gate, not an existing guarantee. See section five of
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

### Resource and Cancellation Invariants That Do Not Yet Hold

The following mechanisms do not exist in the code and cannot be inferred from `Engine`'s current form:

- an accounting allocator and a per-task hard memory quota;
- an instruction budget and a `max_runtime` time budget;
- an atomic cancel flag, an interrupt callback, `lua_break()`, and an abandon protocol;
- a GC guard forbidding interrupts when `gc >= 0`;
- `luaL_sandbox()`, `luaL_sandboxthread()`, and recursive readonly host tables;
- explicit removal of `getfenv`, `setfenv`, `newproxy`, `coroutine`, and `debug`.

The hardening ledger's hard red line requires that cancellation ultimately abandon the coroutine after
using `lua_break()` in the interrupt, and must never use `luaL_error()`, which can be swallowed by
`pcall`. It also requires long-running C++ bindings to honor their own deadline/stop token; a VM
interrupt cannot preempt a stuck C++ call. Together the two form a "500ms total exit" rather than a
single VM trick.

## Relationship to the Product Runtime

### Current Callers

The only actual inbound edge today is `tests/script/test-script.cpp`:

- it creates an `Engine` through `<script/engine.hpp>`;
- it passes in-memory source and a chunk name;
- it consumes `Result<double>`;
- it checks the domain error classification via `automationErrorKind(...)`.

No entry, `engine`, `annotation`, or `controller` source file includes `script/engine.hpp`. This is a
boundary that can be verified directly by a repository reference search.

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

The current host tests are concentrated in `tests/script/test-script.cpp`; the `test-script` target in
`tests/CMakeLists.txt` links `${PROJECT_NAME}_script`, registers only when the module exists, and
inherits the 60-second timeout and the `CI` label.

The existing six doctest cases pin the following behavior:

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

The current tests do not pin sandbox, memory quota, cancellation, budget, logical clock/RNG, globals
isolation, yield resumption, host binding, or strict-background behavior. Luau upstream tests also do
not enter the project CI because of `LUAU_BUILD_TESTS=OFF`.

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

1. Add sandbox setup inside `modules/script/source/script/ffi/`: register minimal host tables,
   recursively freeze, remove the five residual globals, then run `luaL_sandbox()`; each task coroutine
   runs `luaL_sandboxthread()` and accepts only source.
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
