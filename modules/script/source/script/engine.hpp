#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
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

// Luau's opaque VM handle, forward-declared to keep the Luau C headers out of
// this header; global scope matches Luau's own typedef.
struct lua_State;

namespace uf::script
{
    // Registers host-facing global tables on the VM's main state just before the
    // sandbox freezes it, and must deep-freeze whatever it registers.
    // modules/task supplies the uf.* capability tables through this seam; a
    // table it cannot build fails the whole VM generation, hence Status.
    using HostTableInstaller = std::function<Status(lua_State* state)>;

    // Builds the private capability surface and leaves it, and nothing else, on
    // the stack top: the boot hands that one table to every framework module as
    // its chunk argument and then drops it, so the primitives are upvalues of
    // trusted closures rather than keys of a table a project script can name.
    // Anything but exactly one table fails the generation with
    // InternalInvariant.
    using PrivateCapabilityInstaller = std::function<Status(lua_State* state)>;

    // What the host minted a raised value as: the kind the failure is reported
    // under, and the sentence it was raised with. The message travels with the
    // kind because a host carrier is a userdata and `lua_tostring` runs no
    // metamethod, so the runner's fallback would render it
    // "(non-string error value)".
    struct RaisedError final
    {
        AutomationErrorKind kind;

        std::string message{};
    };

    // Reads the raised value at stack `index` of the thread that failed, or
    // nullopt when that value is not one the host minted. The script module owns
    // no error vocabulary of its own, so without this seam an uncaught Tier B
    // failure -- a task that timed out, say -- reaches the host as
    // InvalidResource "(non-string error value)" and is traced in run.finished
    // as a malformed script.
    //
    // It must decide by the carrier's host tag, never by fields read off the
    // value, or a project script could choose the kind its own failure is
    // reported under. It runs on the thread that failed, so it must leave that
    // thread's stack as found and reach nothing that can raise -- no protected
    // frame is left to catch one.
    using RaisedErrorClassifier =
        std::function<std::optional<RaisedError>(lua_State* state, int index)>;

    // Recursively marks the table at stack `index`, every table reachable as a
    // key or value, and every metatable on the way, read-only, enforcing the
    // two rules a project-visible host object must satisfy. Metatable-first,
    // because a still-writable metatable would let a script rewrite
    // __index/__newindex and monkey-patch around the frozen table it guards.
    // Cycle-safe.
    //
    //   - Every metatable carries a __metatable field. table.clone refuses a
    //     table whose metatable is protected (ltablib.cpp tclone); without it
    //     the clone is MUTABLE and carries the SAME metatable, so an identity
    //     proved by a metatable could be forged.
    //   - __index is a table, never a function. A function __index cannot yield,
    //     so it would be an unyieldable hole under a future yield protocol and
    //     one more place a host object runs script-reachable code.
    //
    // A violation is InternalInvariant: the host builds these objects. The stack
    // is restored on every path, including a failing one.
    [[nodiscard]]
    auto deepFreeze(lua_State* state, int index) -> Status;

    // Deep-freezes the table at stack `index` AS a metatable: checked against
    // the two rules above in its own right, then frozen as deepFreeze would.
    // deepFreeze can only check a metatable it reaches through an object already
    // wearing it, so checking a registry-held metatable at construction is what
    // makes the rules hold for every object it will ever guard, mid-run ones
    // included.
    [[nodiscard]]
    auto deepFreezeMetatable(lua_State* state, int index) -> Status;

    // One module of the trusted Luau framework, loaded under the framework
    // environment while the VM boots.
    //
    // Lifetime contract: both views must outlive the create/compile call that
    // consumes them. modules/task satisfies this with string literals in the
    // generated bundle translation unit, which live for the whole process.
    struct FrameworkModule final
    {
        // A consumer-owned canonical logical name. Engine's private framework
        // environment admits bare identifiers; PureDataProgram admits only the
        // reserved @umbraflow/ grammar. It is never a filesystem path.
        std::string_view name{};

        // The module's UTF-8 source text.
        std::string_view source{};

        // An optional exact reserved name by which a later trusted Framework
        // module may require this module. The loader admits only @umbraflow/
        // names, rejects duplicates before running any source, and resolves
        // against earlier modules only. This is separate from `name`, which
        // remains the Framework global and Project publication spelling.
        std::string_view resolverName{};

        // Whether Project-authored PureDataProgram modules may resolve `name`.
        // Framework-owned pure modules may always resolve it, so release-owned
        // data can remain an implementation detail while still passing through
        // the same source, bytecode, memory, and identity boundaries. The full
        // trusted loader uses `resolverName` instead and never publishes its
        // require function to Project source.
        bool projectVisible{true};
    };

    // The value one chunk returned, restricted to what a text protocol can carry
    // back: absent, boolean, number or string, where absent -- a chunk that
    // returned nothing -- is a different answer from false and "".
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

    // What the accounting allocator's ledger says about one VM's heap. None of
    // the three figures derives the others: `usedBytes` is what the next
    // allocation is measured against and counts unreached garbage as well as
    // reachable objects, `ceilingBytes` is what it is measured against, and
    // `peakBytes` is how close the VM has ever come, which a run that recovered
    // would hide. A readout is a snapshot: the next allocation makes it stale.
    struct HeapUsage final
    {
        uint64 usedBytes{0};

        // Zero means the VM was built with no ceiling at all, which is a
        // different fact from a ceiling with no room left.
        uint64 ceilingBytes{0};

        uint64 peakBytes{0};

        // Bytes still available under the ceiling, and the widest representable
        // value when there is no ceiling, because the subtraction wraps on a VM
        // whose ceilingBytes is zero.
        [[nodiscard]] auto headroomBytes() const noexcept -> uint64;
    };

    // Reads the ledger behind `state` without allocating or running any Lua; a
    // VM this module did not create through its accounting allocator reports an
    // all-zero readout.
    [[nodiscard]]
    auto heapUsage(lua_State* state) noexcept -> HeapUsage;

    // Runs a FULL collection on `state` and reports the ledger afterwards.
    //
    // The host has to ask because Luau throws LUA_ERRMEM the moment frealloc
    // returns null, with no emergency collection, and a retry inside the
    // allocator callback would re-enter the collector that callback itself runs
    // under. The only place to reclaim is one where no allocation is in flight:
    // a host call, or the boundary between two units of script
    // (docs/pitfalls/embedded-vm-memory-ceiling.md).
    //
    // Full rather than a step: one step marks a fixed two kilobytes
    // (LUAI_GCSTEPSIZE * LUAI_GCSTEPMUL) however large the allocation that
    // triggered it was.
    auto collectGarbage(lua_State* state) -> HeapUsage;

    // The default wall-clock ceiling on one unit of script. It is named because a
    // host restates it in its own run config, and two spellings of thirty minutes
    // would be free to drift; see EngineConfig::maxRuntime for what one unit is.
    inline constexpr auto k_defaultMaxRuntime =
        MonotonicInstant::Duration{std::chrono::minutes{30}};

    // Tunables for one VM generation. Every field is live, the numeric defaults
    // are conservative placeholders to be calibrated against the first real
    // task, and Luau types never appear here.
    struct EngineConfig final
    {
        // External stop source for hard cancellation. A default-constructed
        // token never requests a stop. The interrupt callback polls it at each
        // safepoint; a watchdog thread may only set the atomic behind it and
        // must never touch the VM.
        std::stop_token cancellation{};

        // Hard memory ceiling the accounting allocator enforces by refusing any
        // over-quota growth; Luau surfaces the refusal as a catchable
        // out-of-memory error, so the host is never dragged down. Zero disables
        // the ceiling.
        uint64 memoryQuotaBytes{uint64{64} * 1024 * 1024};

        // Instruction budget counted by the interrupt callback, cumulatively
        // over the whole VM generation rather than per unit of script. Zero
        // disables it. Cumulative is deliberate: it counts interrupts, so it
        // advances only while Luau is executing and charges an idle VM nothing.
        uint64 interruptBudgetTicks{uint64{100'000'000}};

        // Wall-clock ceiling on ONE unit of script -- one runNumber or runValue
        // call -- measured on the monotonic clock by the interrupt callback. The
        // framework boot runs under its own window of the same length.
        //
        // Per unit of script and NOT per VM: an exploration session answers an
        // agent chunk by chunk with the agent's own thinking time in between,
        // and a chunk that will not finish is still stopped by this clock,
        // whether it is the VM's first or its fortieth.
        //
        // A ceiling the clock cannot represent saturates to the farthest instant
        // it can name rather than wrapping into the past; one below zero expires
        // at once.
        MonotonicInstant::Duration maxRuntime{k_defaultMaxRuntime};

        // The trusted Luau framework, loaded in order under the framework
        // environment during create(). Empty boots a VM whose framework
        // environment exists but holds no modules.
        std::vector<FrameworkModule> frameworkModules{};

        // Optional host-table installer invoked once during create(), after the
        // framework bundle has loaded and before the sandbox freezes the globals
        // (this is installSandbox's order). Empty yields a bare sandboxed VM;
        // modules/task supplies the uf.* data tables -- elements, pages and
        // error kinds, all data a project script may name.
        HostTableInstaller installHostTables{};

        // Optional private capability installer invoked once during create(),
        // BEFORE the framework bundle loads, because the table it leaves on the
        // stack is what each framework module is handed as its chunk argument.
        // Empty boots a framework whose argument is absent; modules/task
        // supplies the observation-cycle primitives here.
        PrivateCapabilityInstaller installPrivateCapabilities{};

        // The global names `installHostTables` registers that a project script
        // must see, copied by name into the project environment, which is
        // otherwise a whitelist with no __index chain to reach anything else.
        // The host names them because create() cannot ask an opaque
        // std::function what it registered; a name listed here that the
        // installer did not register fails the generation, so the two cannot
        // drift apart silently.
        std::vector<std::string> projectGlobals{};

        // The framework module names whose frozen exports the project
        // environment publishes, under the same name, as project globals: the
        // ONLY route by which anything the framework built becomes nameable from
        // a project script. It publishes a value rather than opening a chain, so
        // naming `ctx` here exposes that one frozen table and nothing else the
        // framework environment holds. A name no framework module bound fails
        // the generation, as an unregistered projectGlobals name does.
        std::vector<std::string> frameworkProjectGlobals{};

        // Optional decoder for a value a run raised and nobody caught. Empty
        // reports every uncaught raise as InvalidResource, correct for a VM with
        // no host error carrier; modules/task supplies the Tier B decoder. A
        // cancellation is classified before this runs and never reaches it, so a
        // classifier cannot downgrade a hard cancel into a catchable kind.
        RaisedErrorClassifier classifyRaisedError{};
    };

    // Owns one embedded Luau VM (lua_State) for one generation. How many units
    // of script that generation runs is the front end's business: a task run
    // runs one script and destroys the VM, an exploration session feeds one VM
    // chunk after chunk (see EngineConfig::maxRuntime for why that distinction
    // is load-bearing). A lua_State is never reused across generations, and the
    // project environment is rebuilt per run, so globals one unit of script
    // writes never reach the next. RAII, no Luau types in this header, and NOT
    // thread-safe: every call runs on the owning thread, and an external
    // watchdog may only set the atomic behind EngineConfig::cancellation.
    class Engine final
    {
        class Impl;
        std::unique_ptr<Impl> m_impl;

        explicit Engine(std::unique_ptr<Impl> p_impl) noexcept;

    public:
        Engine(Engine const&) = delete;
        auto operator=(Engine const&) -> Engine& = delete;
        Engine(Engine&&) noexcept;
        auto operator=(Engine&&) noexcept -> Engine&;
        ~Engine();

        // Create a task VM and boot its two environments: the framework
        // environment the trusted bundle loads under, with the private
        // capability surface as its chunk argument, and the project environment,
        // an explicit whitelist with no __index chain back to the framework
        // environment or the main globals. installSandbox owns the ordered
        // sequence, including which globals are removed and when.
        //
        // Fails if the VM cannot be allocated, if a framework module does not
        // load or run, if an installer reports a failure (its error propagates
        // unchanged), or if a whitelisted global is missing. Every failure path
        // closes the VM already allocated.
        [[nodiscard]]
        static auto create(EngineConfig const& config = {}) -> Result<Engine>;

        // Compile, load, and run `source` under a fresh project environment on
        // its own task thread; return the script's sole numeric result (the LAST
        // value if it returns several; 0.0 if it returns nothing numeric). The
        // environment is rebuilt per call from the frozen prototype, so globals
        // a run writes never reach the next one. A compile, load, or runtime
        // error is a recoverable failure.
        [[nodiscard]]
        auto runNumber(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<double>;

        // The same run, reporting what the chunk returned rather than coercing
        // it to a number: `umbra-flow explore` writes one result line per queued
        // chunk, where "returned the string 'home'" and "returned 0" are
        // different answers that runNumber renders alike. A returned value this
        // cannot carry is a FAILURE naming the Luau type, not a silent absent.
        [[nodiscard]]
        auto runValue(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<ScriptValue>;

        // Run a full collection over this VM and report the ledger afterwards.
        // It is the host's only reclamation lever, because the ceiling is
        // measured against uncollected garbage as well as live data -- see the
        // free collectGarbage above. Callers run it where no allocation is in
        // flight and nothing of theirs is expected to survive: between two units
        // of script, or in a host call about to mint something large.
        auto collectGarbage() -> HeapUsage;

        // The ledger as it stands, which is what a caller watches to see itself
        // approach the ceiling instead of meeting it.
        [[nodiscard]]
        auto heapUsage() const noexcept -> HeapUsage;

        // Whether a break has spent this generation, after which runNumber and
        // runValue refuse without touching the VM.
        //
        // A front end that feeds one VM many units of script has to ask: the
        // interrupt's three triggers reach no host call, so a break by the wall
        // clock or the instruction budget latches HERE and nowhere the task
        // layer can see
        // (docs/archive/plans/2026-08-01-agent-front-end-and-exploration.md).
        [[nodiscard]]
        auto generationSpent() const noexcept -> bool;
    };
}
