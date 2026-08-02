#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
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
    // supplies the uf.* capability tables through this seam. Luau types never
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

    // What the host minted a raised value as: the kind the failure is reported
    // under, and the sentence it was raised with.
    //
    // The message travels with the kind because it is lost otherwise. A host
    // carrier is a userdata, and `lua_tostring` does not run a metamethod, so the
    // runner's fallback renders it "(non-string error value)" -- which is what an
    // agent's result line said when the host refused a project write, naming
    // neither the path nor the reason.
    struct RaisedError final
    {
        AutomationErrorKind kind;

        std::string message{};
    };

    // Reads the raised value at stack `index` of the thread that failed, or
    // nullopt when that value is not one the host minted.
    //
    // The script module cannot answer that question itself: it owns no error
    // vocabulary of its own, and the carrier is minted by modules/task, which
    // sits above it. Without this seam an uncaught Tier B failure reaches the
    // host as InvalidResource with the message "(non-string error value)", so a
    // task that timed out and did not catch it would be reported -- and traced
    // in run.finished -- as a malformed script rather than as a timeout.
    //
    // The classifier must decide by the carrier's host tag, never by reading
    // fields off the value: a project script can build a table with any fields
    // it likes, and duck-typing here would let it choose the kind its own
    // failure is reported under. Once the tag has answered, the message may be
    // read off the carrier, because by then the value is one the host built.
    //
    // It runs on the thread that failed, so it must leave that thread's stack as
    // it found it, and it must reach nothing that can raise -- there is no
    // protected frame left to catch one. The runner grows the stack before the
    // call, because a thread unwound by an error has no spare slots of its own.
    using RaisedErrorClassifier =
        std::function<std::optional<RaisedError>(lua_State* state, int index)>;

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
    // attached to anything -- the uf handle kinds register theirs in the VM
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

    // The value one chunk returned, restricted to what a text protocol can carry
    // back to whoever asked for the chunk to run.
    //
    // FOUR SHAPES AND NOT MORE. A Luau chunk may return a table, a function or a
    // host handle, and none of those has an honest rendering as one line of
    // JSON: a table would have to be walked (and could be cyclic, or hold a
    // handle), a function has no value at all, and a handle names something that
    // dies with the run. So the runner REFUSES them by type and says so, rather
    // than serialising a plausible-looking approximation. A caller that wants
    // more out of a chunk formats it into a string inside the chunk, where the
    // formatting is a decision the script layer made and can be read.
    //
    // Absent is a chunk that returned nothing, which is different from one that
    // returned false or an empty string; a caller reporting a result line needs
    // to be able to say so.
    class ScriptValue final
    {
        std::variant<std::monostate, bool, double, std::string> m_value{};

    public:
        ScriptValue() noexcept = default;

        explicit ScriptValue(bool value) noexcept;
        explicit ScriptValue(double value) noexcept;
        explicit ScriptValue(std::string value) noexcept;

        [[nodiscard]] auto absent() const noexcept -> bool;
        [[nodiscard]] auto boolean() const noexcept -> std::optional<bool>;
        [[nodiscard]] auto number() const noexcept -> std::optional<double>;

        // The borrow lasts as long as this value does.
        [[nodiscard]]
        auto text() const noexcept UF_LIFETIME_BOUND -> std::string const*;
    };

    // What the accounting allocator's ledger says about one VM's heap.
    //
    // THE THREE FIGURES ANSWER DIFFERENT QUESTIONS AND NONE OF THEM DERIVES THE
    // OTHERS. `usedBytes` is what the NEXT allocation is measured against, and
    // it counts garbage the incremental collector has not reached as well as
    // reachable objects -- which is the whole reason it is worth reporting.
    // `ceilingBytes` is what it is measured against, and `peakBytes` is how
    // close the VM has ever come, which a run that recovered would otherwise
    // hide.
    //
    // A readout is a SNAPSHOT and not a borrow: it is copied out of the ledger
    // and the next allocation invalidates nothing, it merely makes it stale.
    struct HeapUsage final
    {
        uint64 usedBytes{0};

        // Zero means the VM was built with no ceiling at all, which is a
        // different fact from a ceiling with no room left.
        uint64 ceilingBytes{0};

        uint64 peakBytes{0};

        // Bytes still available under the ceiling; the widest representable
        // value when there is no ceiling.
        //
        // It is a member rather than each caller's arithmetic because the zero
        // convention above is a trap: `ceilingBytes - usedBytes` on an
        // unlimited VM wraps to an enormous number by accident, which happens
        // to be right, and a caller who instead guards `ceilingBytes != 0` and
        // forgets the else branch gets a VM that reclaims on every call.
        [[nodiscard]] auto headroomBytes() const noexcept -> uint64;
    };

    // Reads the ledger behind `state` without allocating or running any Lua.
    //
    // A VM this module did not create through its accounting allocator reports
    // an all-zero readout. That is deliberate: the figures are the ALLOCATOR's,
    // and Luau's own totalbytes could stand in for `usedBytes` but would have to
    // invent a ceiling, which is the one figure a caller acts on.
    [[nodiscard]]
    auto heapUsage(lua_State* state) noexcept -> HeapUsage;

    // Runs a FULL collection on `state` and reports the ledger afterwards.
    //
    // WHY THE HOST HAS TO ASK, rather than the allocator retrying on its own.
    // Luau calls luaD_throw(L, LUA_ERRMEM) the moment frealloc returns null
    // (VM/src/lmem.cpp:248, :505, :545); unlike PUC Lua's luaM_realloc_ it has
    // no emergency collection to run before giving up, and adding one is not
    // available to us either -- re-entering the collector from inside the
    // allocator callback is not sound, because the callback runs during a
    // collection as well. So the ceiling is measured against live bytes PLUS
    // whatever the incremental collector has not reached yet, and the only
    // place that can be fixed is a point where no allocation is in flight: a
    // host call, or the boundary between two units of script.
    //
    // Full rather than a step, because one step is worth a fixed two kilobytes
    // of marking (LUAI_GCSTEPSIZE * LUAI_GCSTEPMUL) however large the allocation
    // that triggered it was -- which is exactly what a loop minting
    // hundred-kilobyte strings outruns, measured.
    auto collectGarbage(lua_State* state) -> HeapUsage;

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
        // uf.* data tables here -- elements, pages and error kinds, all of
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

        // Optional decoder for a value a run raised and nobody caught. Empty by
        // default, which reports every uncaught raise as InvalidResource --
        // correct for a VM with no host error carrier, which is every VM the
        // script module boots on its own. modules/task supplies the Tier B
        // decoder here.
        //
        // A cancellation is classified before this runs and never reaches it, so
        // a classifier cannot downgrade a hard cancel into a catchable kind.
        RaisedErrorClassifier classifyRaisedError{};
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

        // The same run, reporting what the chunk returned rather than coercing
        // it to a number. It exists for a front-end that hands a chunk back an
        // answer -- `umbra-flow explore` writes one result line per queued chunk
        // -- where "the script returned the string 'home'" and "the script
        // returned 0" are different answers and runNumber renders both as 0.0.
        //
        // A returned value this cannot carry is a FAILURE naming the Luau type,
        // not a silent absent: see ScriptValue for why the set is closed.
        [[nodiscard]]
        auto runValue(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<ScriptValue>;

        // Run a full collection over this VM and report the ledger afterwards.
        //
        // It is the host's only reclamation lever, and it exists because the
        // ceiling this Engine enforces is measured against uncollected garbage
        // as well as live data -- see the free collectGarbage above for why the
        // allocator cannot do it for us. Callers run it where no allocation is
        // in flight and nothing of theirs is expected to survive: between two
        // units of script, or in a host call that is about to mint something
        // large.
        //
        // The return value may be ignored; the ledger is readable at any time
        // through heapUsage().
        auto collectGarbage() -> HeapUsage;

        // The ledger as it stands, which is what a caller watches to see itself
        // approach the ceiling instead of meeting it.
        [[nodiscard]]
        auto heapUsage() const noexcept -> HeapUsage;
    };
}
