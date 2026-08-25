#pragma once

#include "allocator.hpp"
#include "cancellation.hpp"

#include <script/engine.hpp>
#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The closed-graph program runtime: the compilation, host-owned require
// resolver, fresh quota-bound VM, resource closure and deep-freeze discipline
// that every program type in this module runs on. It exists because there is
// more than one program type -- a pure one and a scoped one -- and the property
// that distinguishes them is which modules their resolver admits and whether a
// native capability surface is built at all. Everything else is one machine, so
// spelling it twice would let two environments claiming one identity drift.
namespace uf::script::detail
{
    constexpr auto k_maximumErrorBytes           = std::size_t{4096U};
    constexpr auto k_maximumCachedFailureBytes   = std::size_t{1024U};
    constexpr auto k_maximumReaderErrorBytes     = std::size_t{256U};
    constexpr auto k_maximumModuleNameBytes      = std::size_t{256U};
    constexpr auto k_maximumModuleSegments       = std::size_t{16U};
    constexpr auto k_maximumModuleSegmentBytes   = std::size_t{64U};
    constexpr auto k_maximumResourceNameBytes    = std::size_t{128U};
    constexpr auto k_maximumResourceSegments     = std::size_t{16U};
    constexpr auto k_maximumResourceSegmentBytes = std::size_t{64U};
    constexpr auto k_maximumPluginIdBytes        = std::size_t{256U};
    constexpr auto k_maximumEntryPointBytes      = std::size_t{64U};
    constexpr auto k_maximumEntryPointCount      = std::size_t{32U};
    constexpr auto k_frameworkModulePrefix       = std::string_view{"@umbraflow/"};

    // The reserved resource-name namespace, and the resource analogue of
    // k_frameworkModulePrefix. One byte string read in opposite directions by
    // the two admissions: a Framework-supplied resource name must begin with it
    // and a Project-authored one must not. The two name sets are therefore
    // disjoint by construction, so nothing has to carry a trust flag saying
    // which side a resource arrived from, and a Project cannot occupy a name a
    // Framework module resolves -- not the pinned Tool catalog, and not any
    // Framework resource added later, because what is reserved is the class and
    // not one literal. They are counted apart from Project resources for the
    // same reason Framework modules are: the host's own closure must not spend
    // the ceiling a Project was promised.
    constexpr auto k_frameworkResourcePrefix       = std::string_view{"umbraflow."};
    constexpr auto k_maximumFrameworkResourceCount = std::size_t{16U};

    // The two leading markers that make a require request caller-relative, read
    // by resolveModuleRequest and published in the environment material. They
    // are the resolver's own bytes rather than a sentence about it, so
    // respelling either one moves the digest of every program type that runs on
    // this resolver.
    constexpr auto k_sameLevelMarker             = std::string_view{"./"};
    constexpr auto k_parentLevelMarker           = std::string_view{"../"};
    constexpr auto k_maximumFrameworkModuleCount = std::size_t{16U};
    constexpr auto k_interruptBudgetTicks        = uint64{2'000'000U};
    constexpr auto k_compileOptimizationLevel    = 1;
    constexpr auto k_compileDebugLevel           = 0;

    // The data boundary is a value, so its ceilings are the value's:
    // nesting, node count, and the bytes its strings and member names
    // occupy. The depth matches json::parse's own bound, so a document the
    // host accepted cannot be one this refuses to push. The node ceiling is
    // what a 1 MiB canonical document can spell, since the cheapest node an
    // array can hold costs two bytes; the text ceiling is that same 1 MiB
    // applied to the only part of a value whose size a plugin controls
    // without also spending nodes.
    constexpr auto k_maximumValueDepth     = std::size_t{64U};
    constexpr auto k_maximumValueNodes     = std::size_t{512U} * 1024U;
    constexpr auto k_maximumValueTextBytes = std::size_t{1024U} * 1024U;
    constexpr auto k_valueStackSlots = static_cast<int>(
        k_maximumValueDepth * 4U + 32U
    );

    // The same arithmetic applied to the resource byte ceiling, because a
    // resource is not a value whose size a plugin controls: the
    // host registers it, and the ceiling above exists for the 1 MiB
    // document a plugin does control. Like the depth bound, neither of
    // these can refuse a value json::parse produced from bytes within
    // PureDataProgram::k_maximumResourceBytes. They are kept, and stated
    // unfalsifiable, because they are what would notice a value reaching
    // pushValue from anywhere but that parse. What actually binds an
    // admitted resource is the VM memory quota, and its reader fails there.
    constexpr auto k_maximumResourceValueNodes =
        PureDataProgram::k_maximumResourceBytes / 2U;
    constexpr auto k_maximumResourceValueTextBytes =
        PureDataProgram::k_maximumResourceBytes;

    constexpr auto k_copiedGlobals = std::array{
        std::string_view{"assert"},       std::string_view{"error"},
        std::string_view{"getmetatable"}, std::string_view{"ipairs"},
        std::string_view{"next"},         std::string_view{"pairs"},
        std::string_view{"pcall"},        std::string_view{"rawequal"},
        std::string_view{"rawget"},       std::string_view{"rawlen"},
        std::string_view{"rawset"},       std::string_view{"select"},
        std::string_view{"tonumber"},     std::string_view{"tostring"},
        std::string_view{"type"},         std::string_view{"typeof"},
        std::string_view{"unpack"},       std::string_view{"xpcall"},
        std::string_view{"bit32"},        std::string_view{"math"},
        std::string_view{"string"},       std::string_view{"table"},
        std::string_view{"utf8"},
    };

    constexpr auto k_pureGlobals = std::array{
        std::string_view{"assert"},       std::string_view{"error"},
        std::string_view{"getmetatable"}, std::string_view{"ipairs"},
        std::string_view{"next"},         std::string_view{"pairs"},
        std::string_view{"pcall"},        std::string_view{"rawequal"},
        std::string_view{"rawget"},       std::string_view{"rawlen"},
        std::string_view{"rawset"},       std::string_view{"require"},
        std::string_view{"select"},       std::string_view{"tonumber"},
        std::string_view{"tostring"},     std::string_view{"type"},
        std::string_view{"typeof"},       std::string_view{"unpack"},
        std::string_view{"xpcall"},       std::string_view{"bit32"},
        std::string_view{"math"},         std::string_view{"string"},
        std::string_view{"table"},        std::string_view{"utf8"},
    };

    // The frozen tables published beside the whitelist, spelled once so
    // that what pushProgramEnvironment publishes and what an environment
    // digest attests to cannot drift apart.
    constexpr auto k_requireGlobal     = std::string_view{"require"};
    constexpr auto k_resourceTable     = std::string_view{"resource"};
    constexpr auto k_resourceReadJson  = std::string_view{"readJson"};
    constexpr auto k_resourceReadText  = std::string_view{"readText"};
    constexpr auto k_resourceReadBytes = std::string_view{"readBytes"};
    constexpr auto k_canonTable        = std::string_view{"canon"};
    constexpr auto k_canonEmptyObject  = std::string_view{"emptyObject"};
    constexpr auto k_canonNull         = std::string_view{"null"};

    // Observable contracts, not implementation layouts. These strings and
    // the numeric material below move the environment identity whenever a
    // project can distinguish the old runtime from the new one.
    constexpr auto k_requireContract = std::string_view{
        "closed_project_relative_plus_reserved_framework_cached_value_v2"
    };
    constexpr auto k_resourceReadJsonContract =
        std::string_view{"exact_name_kind_checked_cached_frozen_json_value_v1"};
    constexpr auto k_resourceReadTextContract =
        std::string_view{"exact_name_kind_checked_cached_utf8_string_v1"};
    constexpr auto k_resourceReadBytesContract =
        std::string_view{"exact_name_kind_checked_cached_byte_string_v1"};
    constexpr auto k_tostringContract =
        std::string_view{"json_scalar_or_type_name_v1"};
    constexpr auto k_moduleGrammarContract =
        std::string_view{
            "ascii_slash_segments_relative_prefix_reserved_umbraflow_v2"
        };
    constexpr auto k_resourceGrammarContract =
        std::string_view{"ascii_dotted_segments_reserved_umbraflow_v2"};
    constexpr auto k_moduleFailureContract = std::string_view{
        "canonical_cache_cycle_cached_script_terminal_vm_v1"
    };
    constexpr auto k_interruptContract = std::string_view{
        "non_gc_loop_backedge_call_return_safepoints_v1"
    };

    // Luau has no public implementation-version constant. The pinned
    // submodule revision is therefore part of the environment material
    // explicitly: bytecode or VM behavior moving beneath an unchanged
    // bridge and whitelist must still move every session pin.
    constexpr auto k_luauImplementation = std::string_view{
        "luau-0.730+5bc7f4b23756f69f4669b419fa9034f117ccd6fe"
    };

    constexpr auto k_bridgeSource = std::string_view{R"LUAU(
local safe_type = type
local safe_error = error
local safe_pairs = pairs
local safe_ipairs = ipairs
local safe_rawget = rawget
local safe_getmetatable = getmetatable
local safe_table_freeze = table.freeze

local canonical = {
    accept = function(value)
        local kind = safe_type(value)
        if kind ~= "table" and kind ~= "string"
            and kind ~= "number" and kind ~= "boolean" then
            safe_error("pure data function must exchange decoded JSON values", 0)
        end
        return value
    end,
}
safe_table_freeze(canonical)
local safe_canonical = canonical

local function inspect(plugin, expected_id, entry_points)
    if safe_type(plugin) ~= "table" or safe_getmetatable(plugin) ~= nil then
        safe_error("pure data module must return a plain table", 0)
    end
    if safe_rawget(plugin, "plugin_id") ~= expected_id then
        safe_error("pure data module identity does not match its verified registration", 0)
    end

    local allowed = { plugin_id = true }
    for _, name in safe_ipairs(entry_points) do
        allowed[name] = true
        if safe_type(safe_rawget(plugin, name)) ~= "function" then
            safe_error("pure data module is missing an entry point", 0)
        end
    end
    for key in safe_pairs(plugin) do
        if safe_type(key) ~= "string" or not allowed[key] then
            safe_error("pure data module exported an undeclared field", 0)
        end
    end
end

local function invoke(plugin, expected_id, entry_points, entry_point, input)
    inspect(plugin, expected_id, entry_points)
    local selected = nil
    for _, name in safe_ipairs(entry_points) do
        if name == entry_point then
            selected = safe_rawget(plugin, name)
        end
    end
    if selected == nil then
        safe_error("pure data entry point is not registered", 0)
    end
    return safe_canonical.accept(selected(safe_canonical.accept(input)))
end

return {
    inspect = inspect,
    invoke = invoke,
}
)LUAU"};

    // One admitted module, compiled once and shared by every run of its
    // program.
    struct CompiledModule final
    {
        std::string name{};
        std::string bytecode{};
        bool        frameworkOwned{false};
        bool        projectVisible{true};

        // Whether this trusted module is handed the private capability table as
        // its single chunk argument. Only a program type that publishes a native
        // seam sets it, and only for the exact module names its own catalog
        // states, so a Project-authored module can never be handed the table.
        bool capabilityBound{false};
    };

    struct DecodedResource final
    {
        PureDataProgram::ResourceKind kind{PureDataProgram::ResourceKind::Json};
        std::string                   name{};
        std::variant<json::Value, std::string> value{};
    };

    // Everything one admitted closure needs to run, with nothing left to
    // validate or compile. A program type owns its own public wrapper around
    // this and adds only what its own contract adds.
    struct ProgramClosure final
    {
        std::string                  pluginId{};
        std::size_t                  entryModuleIndex{0};
        std::vector<std::string>     entryPoints{};
        std::vector<CompiledModule>  modules{};
        std::vector<DecodedResource> resources{};
        std::string                  bridgeBytecode{};
    };

    // What one program type hands the shared compiler about the graph it is
    // admitting. Every view is call-scoped: compileClosure consumes them before
    // it returns and stores nothing that points into them.
    struct ClosureSpec final
    {
        std::string_view                  pluginId{};
        std::string_view                  entryModule{};
        std::span<std::string_view const> entryPoints{};
        std::span<FrameworkModule const>  frameworkModules{};

        // The exact Framework module names this program type hands its private
        // capability table to. Empty for a program type that publishes no native
        // seam, which is what keeps the scoped catalog a property of the program
        // type rather than of a boot or a constructor flag.
        std::span<std::string_view const> capabilityBoundModules{};
    };

    // The run-scoped state a VM's host callbacks read. Opaque here: a program
    // type reaches it only through the primitives below, so no program type can
    // reach into a field of a run it did not build.
    struct ProgramEnvironment;

    // What a value conversion has spent so far, and what it may spend.
    // Passed as a mutable reference because accumulating into it across a
    // recursive walk is the whole of what it is for. The ceilings are
    // members rather than constants because the two things pushed into a VM
    // are bounded by different facts: a call's input and output are the
    // 1 MiB document a plugin controls, while a resource is host-
    // registered and bounded by PureDataProgram::k_maximumResourceBytes at
    // admission.
    struct ValueBudget final
    {
        std::size_t nodes{0};
        std::size_t textBytes{0};
        std::size_t nodeCeiling{k_maximumValueNodes};
        std::size_t textCeiling{k_maximumValueTextBytes};
    };

    // Builds the private capability surface for one run and leaves it, and
    // nothing else, on the stack top: the boot deep-freezes that one table and
    // hands it to every capability-bound Framework module as its chunk argument,
    // so the primitives are upvalues of trusted closures rather than keys of a
    // table Project-authored source can name. It is the closed-graph analogue of
    // script::PrivateCapabilityInstaller and takes the run environment as well,
    // because the native primitives it installs convert between decoded JSON and
    // VM values through it. An empty installer builds no surface at all, which
    // is what a program type with no native seam passes.
    using CapabilityInstaller =
        std::function<Status(lua_State* state, ProgramEnvironment& environment)>;

    // One fresh quota-bound VM, closed when this object dies. Every run of a
    // closed-graph program builds one and destroys it, so nothing one run wrote
    // can reach the next; that is the whole of the isolation contract, and it is
    // why "cannot be retained into another run" needs no runtime check.
    //
    // Neither copyable nor movable: the accounting allocator holds the address
    // of `m_quota` for the life of the VM and reads it again during lua_close,
    // and the interrupt callback holds the address of `m_control`, so both
    // ledgers must stay put.
    class QuotaBoundVm final
    {
        MonotonicInstant::Duration m_runtimeCeiling;
        MemoryQuota                m_quota;
        InterruptState             m_control;
        lua_State*                 m_state;

    public:
        // `runtimeCeiling` is the wall-clock ceiling on this one run and
        // `cancellation` the external stop source the interrupt polls at every
        // safepoint; a default-constructed token never requests a stop. A VM the
        // host allocator could not build leaves state() null and the run refuses
        // on it.
        explicit QuotaBoundVm(
            MonotonicInstant::Duration runtimeCeiling,
            std::stop_token cancellation,
            std::size_t memoryQuotaBytes = PureDataProgram::k_memoryQuotaBytes,
            uint64 interruptBudgetTicks = k_interruptBudgetTicks
        );

        QuotaBoundVm(QuotaBoundVm const&)                    = delete;
        QuotaBoundVm(QuotaBoundVm&&)                         = delete;
        auto operator=(QuotaBoundVm const&) -> QuotaBoundVm& = delete;
        auto operator=(QuotaBoundVm&&) -> QuotaBoundVm&      = delete;
        ~QuotaBoundVm();

        // A borrow of the VM this object owns, valid until it is destroyed.
        [[nodiscard]] auto state() const noexcept UF_LIFETIME_BOUND -> lua_State*;

        // The interrupt ledger, mutated by the run that owns this VM.
        [[nodiscard]] auto control() noexcept UF_LIFETIME_BOUND -> InterruptState&;

        // Whether this VM's accounting allocator refused a growth because it
        // crossed the configured ceiling. A program front end uses this to name
        // its own owning budget instead of leaking this shared runtime's generic
        // diagnostic.
        [[nodiscard]] auto memoryCeilingRefused() const noexcept -> bool;

        // Anchor this run's wall-clock window. Called once, after the interrupt
        // callback is installed and before any script executes.
        auto beginUnitOfScript() noexcept -> void;
    };

    [[nodiscard]]
    auto refuse(std::string message) -> std::unexpected<Error>;

    // A bounded copy of a refusal, in storage nothing has to destroy.
    // luaL_error leaves its frame without returning to it, so the message
    // a host reader reports cannot be held in anything that owns memory.
    [[nodiscard]]
    auto boundedText(std::string_view text) -> std::array<char, k_maximumReaderErrorBytes>;

    [[nodiscard]]
    auto validSegmentedAsciiName(
        std::string_view value,
        char separator,
        std::size_t maximumBytes,
        std::size_t maximumSegments,
        std::size_t maximumSegmentBytes
    ) -> bool;

    [[nodiscard]]
    auto validateModuleClosure(
        std::string_view entryModule,
        std::span<PureDataProgram::Module const> modules
    ) -> Status;

    // The Project half of the resource closure: canonical names outside the
    // reserved Framework namespace, admitted bytes, and the fixed count and byte
    // ceilings. A name inside the reserved namespace is refused here by name,
    // which is what stops a Project shadowing a resource a Framework module
    // reads.
    [[nodiscard]]
    auto validateResourceClosure(
        std::span<PureDataProgram::Resource const> resources
    ) -> Status;

    // Admit and compile one closed module graph. It validates in a fixed order,
    // compiles the shared bridge and every module, and marks the Framework
    // modules named by ClosureSpec::capabilityBoundModules. It runs no VM: a
    // program type boots the returned closure itself, because only it knows
    // which capability surface that boot installs.
    //
    // The two resource closures are separate parameters rather than one list
    // with a per-entry origin, because they are admitted by opposite name rules:
    // `frameworkResources` must all be inside the reserved namespace and
    // `resources` must all be outside it. Neither list can hold a value
    // the other would accept, so which side supplied a resource is a fact about
    // its name and not about a flag travelling beside it. Framework resources
    // arrive owned rather than as a span because the host computes their bytes,
    // unlike the static Framework module sources ClosureSpec borrows.
    [[nodiscard]]
    auto compileClosure(
        ClosureSpec const& spec,
        std::vector<PureDataProgram::Module> modules,
        std::vector<PureDataProgram::Resource> resources,
        std::vector<PureDataProgram::Resource> frameworkResources
    ) -> Result<ProgramClosure>;

    // Boot `closure` in `vm`, inspect the entry module's exports against the
    // recorded identity and entry points, and prove every resource materializes
    // inside the VM quota. It calls no entry point. `vm` is mutable because
    // driving it is the whole operation.
    [[nodiscard]]
    auto admitClosure(
        QuotaBoundVm& vm,
        ProgramClosure const& closure,
        CapabilityInstaller const& installCapabilities
    ) -> Status;

    // Boot `closure` in `vm` and call one registered entry point with one
    // decoded JSON value, returning the one decoded JSON value it produced.
    // `vm` is mutable for the same reason.
    [[nodiscard]]
    auto invokeClosure(
        QuotaBoundVm& vm,
        ProgramClosure const& closure,
        CapabilityInstaller const& installCapabilities,
        std::string_view entryPoint,
        json::Value const& input
    ) -> Result<json::Value>;

    // Push one decoded JSON value onto `state` as a frozen VM value, and read
    // one back. A native primitive a program type installs exchanges its
    // arguments and its answer through these, so the boundary a Tool call
    // crosses is the same boundary an entry point's input and output cross.
    [[nodiscard]]
    auto pushValue(
        lua_State* state,
        ProgramEnvironment const& environment,
        json::Value const& value,
        std::size_t depth,
        ValueBudget& budget
    ) -> Status;

    [[nodiscard]]
    auto readValue(
        lua_State* state,
        ProgramEnvironment const& environment,
        int index,
        std::size_t depth,
        ValueBudget& budget
    ) -> Result<json::Value>;

    // Record `error` as this run's terminal outcome. The caller then breaks the
    // VM, and the break is reported under the recorded kind and message rather
    // than as a cancellation. It is how a host refusal reaches the caller past a
    // script pcall: there is no catchable value, only a dead VM.
    auto recordTerminalFailure(ProgramEnvironment& environment, Error const& error) -> void;

    // The members every closed-graph environment identity shares, as one
    // canonical JSON object with its closing brace withheld: the bridge that
    // wraps every call, the versioned contract each published function answers
    // to, the frozen tables published beside the whitelist, the whitelist
    // itself, and the fixed ceilings. A program type appends its own members in
    // JCS order and closes the object, so two program types cannot claim one
    // identity and cannot drift apart on what they share.
    //
    // `runtimeCeiling` is a member rather than a constant because it is the one
    // ceiling a program type sets for itself: a pure call returns without
    // waiting, and a scoped run blocks in a Tool for as long as that Tool's own
    // validated duration allows.
    [[nodiscard]]
    auto sharedEnvironmentMaterial(
        MonotonicInstant::Duration runtimeCeiling,
        std::string_view runtimeCeilingSource = {}
    ) -> std::string;
}
