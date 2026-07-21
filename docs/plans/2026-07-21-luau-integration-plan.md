# Luau 0.730 integration plan (P0-B foundation)

> Status: draft for review, 2026-07-21 (autonomous overnight work, NOT committed).
> The two load-bearing claims and the integration mechanics were **empirically
> verified** on this machine (MSVC 14.44, Luau 0.730) with a throwaway spike;
> results below are measured, not assumed. This plan is schema-independent P0-B
> groundwork — it does NOT touch the deferred annotation design (S0 schema).
> Authorities: [`2026-07-21-product-form-and-roadmap.md`](2026-07-21-product-form-and-roadmap.md) §五,
> [`2026-07-21-lua-task-model-grill-decisions.md`](2026-07-21-lua-task-model-grill-decisions.md) D5/D9,
> [`2026-07-21-p0b-luau-hardening-ledger.md`](2026-07-21-p0b-luau-hardening-ledger.md).

## 1. Verified on our toolchain (spike)

A throwaway spike (`add_subdirectory(luau)` @ tag 0.730, link `Luau.VM Luau.Compiler`,
C++23 host TU, MSVC/Ninja/Release) proved:

- **Veto #1 — cancellation (THE hard red line): PASS.** `interrupt` callback →
  `lua_break` hard-stops `while true do end`, `pcall(function() while true do end end)`,
  and `while true do pcall(function() error('x') end) end` within the budget, and a
  finite task is not false-tripped. **`lua_break` is uncatchable by `pcall`** (sets
  `L->status = LUA_BREAK`, no `luaD_throw`). Contrast measured: the SAME pcall loop with
  `luaL_error` in the interrupt returns `LUA_OK` — the script **swallows** the cancel.
  So the primitive MUST be `lua_break` (or `lua_yield`), never `luaL_error`.
- **Veto #3 — sandbox: PASS, with a precise finding.** After `luaL_sandbox` +
  per-thread `luaL_sandboxthread`, the filesystem/loader escape vectors
  `io / package / require / loadstring / load / dofile / loadfile` (and
  `collectgarbage`) are **absent**, and `os.execute/getenv/remove/exit` are removed
  while `os.time` remains. Nested host tables need recursive deep-freeze
  (`luaL_sandbox`'s readonly is shallow) — verified: a deep-frozen `uf.cfg` rejects
  `uf.cfg.x = 999`.
  - **⚠ `luaL_sandbox` does NOT remove `getfenv`, `setfenv`, `newproxy`, `coroutine`,
    `debug`.** The host must nil these explicitly. Critical ones: `coroutine` (a script
    that spawns its own coroutine escapes the host's interrupt-driven cancel — D5
    depends on this), `debug` (can uninstall hooks / read outside the sandbox — D9),
    `setfenv`/`getfenv` (environment escape). This empirically confirms and sharpens
    the decision-package Q9 list.

Spike lives at `<scratchpad>/luau-spike/` (throwaway, not for the repo).

## 2. Integration approach (validated at the toolchain level)

- **Vendor (IMPLEMENTED):** git submodule **confined to the module** at
  `modules/script/external/luau`, pinned to 0.730 (SHA
  `5bc7f4b23756f69f4669b419fa9034f117ccd6fe`; a bump is the trigger to re-run the veto
  spikes + regressions). Per the April2 rule that a single-module library stays inside
  its own module, nothing sits at the top level. Contributors/CI need
  `git clone --recursive` / `submodule update --init`. Submodule over FetchContent:
  exact auditable pin, offline after clone, no history bloat.
- **Build wiring (IMPLEMENTED):** the module owns
  `modules/script/external/CMakeLists.txt`, which sets `LUAU_BUILD_CLI/TESTS/WEB=OFF`
  and `add_subdirectory(luau EXCLUDE_FROM_ALL)`. The **AutoLoader was extended**
  (`cmake/build.cmake` `cpp_define_module`): in Pass 1 (Define) it `add_subdirectory()`s
  any module's `external/` that has a `CMakeLists.txt`, so the `Luau.*` targets exist
  before Pass 2 (Link). Generic — any future module gets the same by dropping an
  `external/CMakeLists.txt`. `scripts/fix_format.py` excludes any `external/` dir.
- **Targets:** link **`Luau.VM` + `Luau.Compiler`** only; `Luau.Ast` / `Luau.Bytecode`
  / `Luau.Common` come transitively. Do NOT pull `Luau.CodeGen` (JIT), `Luau.Analysis`
  (offline type-checker), or `Luau.Config`/`Luau.Require` (filesystem `require()` —
  actively unwanted in a sandboxed host).
- **Build options** in `modules/script/external/CMakeLists.txt`: `LUAU_BUILD_CLI=OFF`,
  `LUAU_BUILD_TESTS=OFF`, `LUAU_BUILD_WEB=OFF`, `EXCLUDE_FROM_ALL`. Leave
  `LUAU_EXTERN_C=OFF` (C++ host), `LUAU_WERROR=OFF` (default). Luau targets are NOT
  given `cpp_apply_safety_profile`, so `/W4 /WX` never hits Luau's own compilation.
- **MSVC `/W4 /WX` friction — one spot:** the host `.cpp` that includes `lua.h` /
  `lualib.h` / `luacode.h` under `/WX`. Luau's include dirs propagate as non-SYSTEM
  PUBLIC dirs, and a manifest-driven module has no `CMakeLists.txt` to mark them
  `SYSTEM`. Use the repo's established idiom: **pragma-wrap the Luau includes** in the
  `.cpp` (`#pragma warning(push, 0)` / clang/gcc equivalents — exactly as
  `entry/m0-demo/ffi/png-decoder.cpp:29-50`). Verified in the spike's `main.cpp`.
  Keep Luau types OUT of `uf::script` public headers (opaque/pImpl) so the wrapped
  includes live only in `.cpp` and Luau stays a PRIVATE dep.

## 3. New module `modules/script` (namespace `uf::script`)

Cross-platform (Luau builds on Win/Linux/macOS — no `platforms=` restriction),
static lib, manifest-driven. `modules/script/manifest.txt`:

```ini
[module]
name = script
type = static
version = 0.1.0

[build]
unity_build = false

[dependencies]
public = core domain
private = Luau.VM Luau.Compiler
```

`public = core domain` → `Result<T>`/`Status`/`fail(...)`/`UF_*` + `AutomationErrorKind`.
`private = Luau.VM Luau.Compiler` resolves through `cpp_resolve_dependency`'s
`TARGET Luau.VM` branch (the dot survives tokenization); PRIVATE because Luau types
never appear in public headers.

Source layout (auto-globbed; no per-module CMakeLists):
```
modules/script/source/script/
    engine.hpp/.cpp        # uf::script::Engine — RAII lua_State (pImpl), named factory
                           #   (creation can fail), unique_ptr<lua_State, LuaClose>
    sandbox.hpp/.cpp       # luaL_sandbox + nil {getfenv,setfenv,newproxy,coroutine,debug}
                           #   + recursive deepFreeze of host tables
    cancellation.hpp/.cpp  # interrupt cb (gc>=0 guard) + atomic cancel token, lua_break
    host-api.hpp/.cpp      # (later) uf.* bindings, deep-frozen
```

**No top-level edit** (April2 rule: a single-module library stays inside its module).
`modules/script/external/` holds the `luau` submodule plus a small `CMakeLists.txt`
wrapper, and the AutoLoader `add_subdirectory()`s that `external/` in Pass 1 so the
`Luau.*` targets exist before Pass 2 links the module:

```cmake
# cmake/build.cmake, cpp_define_module (Pass 1: Define) — generic, any module
if(EXISTS "${MODULE_PATH}/external/CMakeLists.txt")
    add_subdirectory("${MODULE_PATH}/external"
                     "${CMAKE_BINARY_DIR}/modules/${DIR_NAME}/external")
endif()

# modules/script/external/CMakeLists.txt — Luau-specific, confined to the module
set(LUAU_BUILD_CLI   OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB   OFF CACHE BOOL "" FORCE)
add_subdirectory(luau EXCLUDE_FROM_ALL)
```

Luau-calling code lives under `source/script/ffi/` — the `unsafe`/`platform`/`ffi`
boundary check_safety requires for the C `free` that pairs `luau_compile`'s malloc.
Tests wire in via `tests/CMakeLists.txt` (`test-script`, mirroring `test-core`).

## 4. Sandbox + cancellation recipe (from the verified spike)

**Sandbox setup order** (per task VM/thread):
1. `luaL_openlibs(L)`.
2. Register host API tables (`uf.*`), then **recursively deep-freeze** them
   (`lua_setreadonly` walks nested tables — shallow otherwise).
3. **Nil the survivors:** `getfenv`, `setfenv`, `newproxy`, `coroutine`, `debug`
   (host uses `debug`/`coroutine` internally in C, never exposes them to script).
4. `luaL_sandbox(L)` (freezes base libs, sets `safeenv`) — AFTER step 2/3 so global
   mutation assumptions hold.
5. Run each task on its own `lua_newthread` + `luaL_sandboxthread`.
6. Only accept source (`luau_compile` → `luau_load`); never script-provided bytecode
   (already unreachable — no `load`/`loadstring` under sandbox).
7. Determinism policy: pin `lua_CompileOptions` `optimizationLevel`/`debugLevel`; nil
   or wrap residual `os.time/clock/date` and `math.random` behind host-controlled
   logical clock / seeded RNG (D9 determinism floor).

**Cancellation:** task runs on a coroutine via `lua_resume`. A watchdog / Ctrl-C
thread sets an **atomic** cancel flag ONLY (no Lua API off-thread). The VM thread's
`interrupt` callback: `if (gc >= 0) return;` then, on cancel-or-deadline, call
`lua_break(L)`. `lua_resume` returns `LUA_BREAK`; the host **abandons** the thread
(drops the ref, never resumes). Never `luaL_error` for the final cancel.

## 5. Known limitation to test once bindings exist (Risk #1)

`lua_break`/`lua_yield` raise a **catchable** "break/yield across C-call boundary"
error if the interrupt fires while the script is inside a non-yieldable host C
function. So **every `uf::script::host-api` binding must return promptly** (capture,
recognition, wait, input must honor a deadline/`stop_token` and poll the cancel flag —
must not block indefinitely in C). Add a veto sub-case that hangs INSIDE a registered
C function once the first binding lands. This is the same failure class as the
`table.sort`-comparator / `string.gsub`-callback loop noted in the hardening ledger.

## 6. Concrete P0-B first steps (schema-independent)

1. ✅ **DONE.** `modules/script/external/luau` submodule pinned to 0.730; the
   module-confined `external/CMakeLists.txt` + AutoLoader convention build it; a clean
   project build passes the full CI gate (9/9, incl. `test-script`).
2. ✅ **DONE.** `modules/script` with `Engine` (RAII `lua_State` via pImpl, named
   `create()` factory) + `runNumber(source, chunk)` that compiles + loads + runs on a
   fresh coroutine; pragma-wrapped includes under `source/script/ffi/`.
3. Port the sandbox + cancellation recipe (§4) into `sandbox.*` / `cancellation.*`.
4. Land the veto suite as real `tests/script/` doctest cases (the spike's 10 checks +
   the C-boundary case from §5) wired into CI. This is the P0 "6 一票否决" gate made
   executable and regression-guarded on every Luau bump.
5. THEN (blocked on the annotation design) the recognizer/page host handles and the
   observe/act/wait engine loop.

## Open items / for the user
- ✅ Resolved: submodule (not FetchContent), confined to `modules/script/external/luau`
  (0.730 SHA `5bc7f4b23756f69f4669b419fa9034f117ccd6fe`).
- The recognizer/page binding, manifest schema, and ROI coordinate space remain in the
  **deferred annotation design doc** — §6 step 5 waits on it.
- Submodule name in `.gitmodules` is still the original `external/luau` (internal id);
  the path is correct (`modules/script/external/luau`). Cosmetic; leave unless it bugs you.
