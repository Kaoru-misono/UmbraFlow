#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// Luau's opaque VM handle, forward-declared so this public header never pulls in
// the Luau C headers (they compile only behind the pragma-wrapped includes in
// the ffi layer). Declared at global scope to match Luau's own typedef, so the
// name resolves to the same type once <lua.h> is visible in a .cpp.
struct lua_State;

namespace uf::script
{
    // Registers host-facing global tables on the VM's main state just before the
    // sandbox freezes it, and is responsible for deep-freezing whatever it
    // registers. The script module ships no installer of its own; modules/task
    // supplies the umbra.* capability tables through this seam. Luau types never
    // cross this boundary: the installer receives only the opaque lua_State the
    // ffi layer knows how to drive. An empty installer registers nothing.
    //
    // It returns Status because a host table the installer cannot build is a
    // reason for the whole VM generation to fail. A void installer left only two
    // ways out: longjmp through the C++ boot with luaL_error, or register
    // nothing and let the generation come up silently crippled. Engine::create
    // now propagates the installer's own error unchanged and closes the VM it
    // had already allocated.
    using HostTableInstaller = std::function<Status(lua_State* state)>;

    // Builds the PRIVATE capability surface and leaves it, and nothing else, on
    // the stack top. The boot hands that one table to every framework module as
    // its chunk argument and then drops it, so the primitives live in a trusted
    // closure's upvalue and are never a key of any table a project script can
    // name -- which is the whole difference between this seam and the one above.
    //
    // Contract, enforced rather than assumed: a successful call grows the stack
    // by exactly one and leaves a table on top. Anything else fails the
    // generation with InternalInvariant instead of silently booting a framework
    // whose capability argument is a stray value.
    using PrivateCapabilityInstaller = std::function<Status(lua_State* state)>;

    // Recursively marks the table at stack `index`, every table reachable from
    // its values, and every metatable on the way, read-only, and enforces the
    // two structural rules a project-visible host object must satisfy. Freezing
    // runs metatable-first: a still-writable metatable would let a script
    // rewrite __index/__newindex and monkey-patch around the frozen table it
    // guards. Cycle-safe: each table is visited once.
    //
    // The rules, and why each is a rule rather than a convention:
    //
    //   - Every metatable carries a __metatable field. table.clone refuses a
    //     table whose metatable is protected (ltablib.cpp tclone); without the
    //     field it returns a MUTABLE copy carrying the SAME metatable, so any
    //     table that proves its identity by its metatable could be forged.
    //   - __index is a table, never a function. A function __index cannot yield,
    //     so it would silently become an unyieldable hole if the primitive layer
    //     ever moves to a yield protocol, and it is one more place a host object
    //     could run script-reachable code.
    //
    // A violation is InternalInvariant: the host builds these objects, so it is
    // a broken host rather than bad user input. The stack is restored on every
    // path, including a failing one.
    [[nodiscard]]
    auto deepFreeze(lua_State* state, int index) -> Status;

    // Deep-freezes the table at stack `index` AS a metatable: it is checked
    // against the two rules above in its own right, then frozen exactly as
    // deepFreeze would freeze it.
    //
    // It exists because a metatable is usually built and frozen before it is
    // attached to anything -- the umbra handle kinds register theirs in the VM
    // registry and hang it on each handle later -- and deepFreeze can only check
    // a metatable it reaches through an object already wearing it. Checking at
    // construction is what makes the rules hold for every object that metatable
    // will ever guard, including ones minted mid-run.
    [[nodiscard]]
    auto deepFreezeMetatable(lua_State* state, int index) -> Status;

    // One module of the trusted Luau framework, loaded under the framework
    // environment while the VM boots.
    //
    // Lifetime contract: both views must outlive the Engine::create call that
    // consumes them. modules/task satisfies this with string literals in the
    // generated bundle translation unit, which live for the whole process.
    struct FrameworkModule final
    {
        // The Luau module name: a bare identifier, never a path. It is the key
        // the module's frozen exports are bound under in the framework
        // environment, so a later module can reach an earlier one.
        std::string_view name{};

        // The module's UTF-8 source text.
        std::string_view source{};
    };

    // Tunables for one task VM generation. Every field is live: the cancellation
    // source and the instruction/time budgets drive the interrupt callback, and
    // the memory ceiling drives the accounting allocator that backs the VM. The
    // defaults are conservative placeholders to be calibrated against the first
    // real task. Luau types never appear here.
    struct EngineConfig final
    {
        // External stop source for hard cancellation. A default-constructed
        // token never requests a stop. The interrupt callback (later wave) reads
        // it; a watchdog thread may only set the atomic behind it and must never
        // touch the VM.
        std::stop_token cancellation{};

        // Per-task hard memory ceiling the accounting allocator enforces by
        // refusing any over-quota growth (Luau surfaces the refusal as a
        // catchable out-of-memory error, so the host is never dragged down).
        // Zero disables the ceiling. Conservative placeholder to be calibrated
        // against the first real task.
        uint64 memoryQuotaBytes{uint64{64} * 1024 * 1024};

        // Instruction budget counted by the interrupt callback (later wave).
        // Conservative placeholder pending calibration.
        uint64 interruptBudgetTicks{uint64{100'000'000}};

        // Wall-clock ceiling measured on steady_clock by the interrupt callback
        // (later wave). Placeholder default.
        std::chrono::steady_clock::duration maxRuntime{std::chrono::minutes{30}};

        // The trusted Luau framework, loaded in order under the framework
        // environment during create(). Empty by default, which boots a VM whose
        // framework environment exists but holds no modules. A module that fails
        // to compile or raises while running fails the whole generation.
        std::vector<FrameworkModule> frameworkModules{};

        // Optional host-table installer invoked once during create(), after the
        // framework bundle has loaded and before the sandbox freezes the globals
        // (this is installSandbox's order). Empty by default, which yields a
        // bare sandboxed VM with no host capabilities. modules/task supplies the
        // umbra.* data tables here -- recognizers, pages and error kinds, all of
        // which are data a project script may name.
        HostTableInstaller installHostTables{};

        // Optional private capability installer invoked once during create(),
        // BEFORE the framework bundle loads, because the table it leaves on the
        // stack is what each framework module is handed as its chunk argument.
        // Empty by default, which boots a framework whose argument is absent.
        // modules/task supplies the observation-cycle primitives here.
        PrivateCapabilityInstaller installPrivateCapabilities{};

        // The global names `installHostTables` registers that a project script
        // must see. They are copied by name into the project environment, which
        // is otherwise an explicit whitelist of the deterministic standard
        // library and has no __index chain to reach anything else.
        //
        // The host names them because create() cannot ask an opaque
        // std::function what it registered. The pairing is not left to
        // discipline: a name listed here that the installer did not register
        // fails the generation, so the two cannot drift apart silently.
        std::vector<std::string> projectGlobals{};

        // The framework module names whose frozen exports the project
        // environment publishes, under the same name, as project globals. This
        // is the ONLY route by which anything the framework built becomes
        // nameable from a project script, and it publishes a value rather than
        // opening a chain: the project environment still has no metatable, so
        // naming `ctx` here exposes that one frozen table and nothing else the
        // framework environment holds.
        //
        // A name listed here that no framework module bound fails the
        // generation, exactly as an unregistered projectGlobals name does.
        std::vector<std::string> frameworkProjectGlobals{};
    };

    // Owns one embedded Luau VM (lua_State) for a single task generation: create
    // one per task, run it, then destroy it. A lua_State is never reused across
    // tasks, so mutable globals cannot leak between tasks. RAII; Luau types are
    // confined to the implementation and never appear in this header. NOT
    // thread-safe: every call runs on the owning thread. An external watchdog
    // may only set the atomic behind EngineConfig::cancellation, never the VM.
    class Engine final
    {
        class Impl;
        std::unique_ptr<Impl> m_impl;

        explicit Engine(std::unique_ptr<Impl> p_impl) noexcept;

    public:
        Engine(Engine&&) noexcept;
        auto operator=(Engine&&) noexcept -> Engine&;
        ~Engine();

        // Create a task VM and boot its two environments: open the admitted base
        // libraries, remove the dangerous survivors luaL_sandbox leaves
        // (getfenv/setfenv/newproxy/gcinfo/coroutine/debug/_G) together with the
        // residual clock and RNG entry points, build the framework environment,
        // install the private capability surface and load the framework bundle
        // under that environment with the surface as its chunk argument, install
        // the host tables, freeze the base libraries, and finally build the
        // project environment as an explicit whitelist with no __index chain
        // back to the framework environment or the main globals.
        //
        // Fails if the VM cannot be allocated, if a framework module does not
        // load or run, if an installer reports a failure (its error propagates
        // unchanged), or if a whitelisted global is missing. Every failure path
        // closes the VM it had already allocated.
        [[nodiscard]]
        static auto create(EngineConfig const& config = {}) -> Result<Engine>;

        // Compile, load, and run `source` under a fresh project environment on
        // its own task thread; return the script's sole numeric result (the LAST
        // value if it returns several; 0.0 if it returns nothing numeric). The
        // environment is rebuilt per call from the frozen prototype, so globals
        // a run writes never reach the next one. A compile, load, or runtime
        // error is a recoverable failure. Retained as the substrate's test entry
        // point.
        [[nodiscard]]
        auto runNumber(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<double>;
    };
}
