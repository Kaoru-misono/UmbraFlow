#include <script/pure-data-program.hpp>

#include "allocator.hpp"
#include "cancellation.hpp"
#include "pure-data-admission.hpp"

#include <json/value.hpp>

#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Luau's C headers do not build clean under the project's warning profile.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::script
{
    namespace
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
        constexpr auto k_maximumFrameworkModuleCount = std::size_t{16U};
        constexpr auto k_interruptBudgetTicks        = uint64{2'000'000U};
        constexpr auto k_maximumRuntime               = std::chrono::seconds{2};
        constexpr auto k_compileOptimizationLevel     = 1;
        constexpr auto k_compileDebugLevel            = 0;

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
        // that what pushPureEnvironment publishes and what
        // pluginEnvironmentHash attests to cannot drift apart.
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
            std::string_view{"ascii_dotted_segments_v1"};
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

        struct VmRun final
        {
            MemoryQuota quota{.limitBytes = PureDataProgram::k_memoryQuotaBytes};
            InterruptState control{
                .budgetTicks = k_interruptBudgetTicks,
            };
            lua_State* state{nullptr};
        };

        struct CompiledModule final
        {
            std::string name{};
            std::string bytecode{};
            bool        frameworkOwned{false};
        };

        struct DecodedResource final
        {
            PureDataProgram::ResourceKind kind{PureDataProgram::ResourceKind::Json};
            std::string                   name{};
            std::variant<json::Value, std::string> value{};
        };

        enum class ModuleLoadState : uint8
        {
            Unloaded,
            Loading,
            Loaded,
            Failed,
        };

        struct ModuleRuntime final
        {
            ModuleLoadState state{ModuleLoadState::Unloaded};
            int             reference{LUA_NOREF};
            std::string     failure{};
        };

        struct PureEnvironment final
        {
            std::span<CompiledModule const>  modules{};
            std::span<DecodedResource const> resources{};
            std::vector<ModuleRuntime>       moduleRuntime{};
            std::vector<int>                 materializedResources{};
            InterruptState*                   pControl{nullptr};

            std::optional<AutomationErrorKind> terminalKind{};
            std::string                        terminalMessage{};

            int nullReference{LUA_NOREF};
            int emptyObjectReference{LUA_NOREF};
            int canonReference{LUA_NOREF};
        };

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

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        auto nilLibraryField(lua_State* state, char const* library, char const* field) -> void
        {
            lua_getglobal(state, library);
            if (lua_istable(state, -1))
            {
                lua_pushnil(state);
                lua_setfield(state, -2, field);
            }
            lua_pop(state, 1);
        }

        auto rawSetField(lua_State* state, int index, std::string_view name) -> void
        {
            auto const key = std::string{name};
            lua_rawsetfield(state, index, key.c_str());
        }

        [[nodiscard]]
        auto validSegmentedAsciiName(
            std::string_view value,
            char separator,
            std::size_t maximumBytes,
            std::size_t maximumSegments,
            std::size_t maximumSegmentBytes
        ) -> bool
        {
            if (value.empty() || value.size() > maximumBytes)
            {
                return false;
            }

            auto segmentBytes = std::size_t{0};
            auto segments     = std::size_t{1};
            for (auto const character : value)
            {
                if (character == separator)
                {
                    if (segmentBytes == 0U || segments == maximumSegments)
                    {
                        return false;
                    }
                    segmentBytes = 0U;
                    ++segments;
                    continue;
                }

                auto const first = segmentBytes == 0U;
                auto const lower = character >= 'a' && character <= 'z';
                auto const digit = character >= '0' && character <= '9';
                if (
                    (!lower && (first || (!digit && character != '_' && character != '-')))
                    || segmentBytes == maximumSegmentBytes
                )
                {
                    return false;
                }
                ++segmentBytes;
            }
            return segmentBytes != 0U;
        }

        [[nodiscard]]
        auto validProjectModuleName(std::string_view value) -> bool
        {
            return validSegmentedAsciiName(
                value,
                '/',
                k_maximumModuleNameBytes,
                k_maximumModuleSegments,
                k_maximumModuleSegmentBytes
            );
        }

        [[nodiscard]]
        auto validFrameworkModuleName(std::string_view value) -> bool
        {
            if (!value.starts_with(k_frameworkModulePrefix))
            {
                return false;
            }
            auto const suffix = value.substr(k_frameworkModulePrefix.size());
            return value.size() <= k_maximumModuleNameBytes
                && validProjectModuleName(suffix);
        }

        [[nodiscard]]
        auto validResourceName(std::string_view value) -> bool
        {
            return validSegmentedAsciiName(
                value,
                '.',
                k_maximumResourceNameBytes,
                k_maximumResourceSegments,
                k_maximumResourceSegmentBytes
            );
        }

        [[nodiscard]]
        auto resolveModuleRequest(
            std::string_view caller,
            std::string_view request
        ) -> Result<std::string>
        {
            if (request.empty() || request.size() > k_maximumModuleNameBytes)
            {
                return refuse("pure data require request is not a bounded module name");
            }
            if (request.starts_with('@'))
            {
                if (!validFrameworkModuleName(request))
                {
                    return refuse(
                        "pure data require request is not a canonical Framework module"
                    );
                }
                return std::string{request};
            }
            if (validFrameworkModuleName(caller))
            {
                return refuse(
                    "Framework modules may require only reserved Framework modules"
                );
            }
            if (validProjectModuleName(request))
            {
                return std::string{request};
            }

            auto prefixCount = std::size_t{0};
            auto suffix      = request;
            auto sameLevel   = false;
            if (suffix.starts_with("./"))
            {
                sameLevel = true;
                suffix.remove_prefix(2U);
            }
            else
            {
                while (suffix.starts_with("../"))
                {
                    ++prefixCount;
                    suffix.remove_prefix(3U);
                }
            }
            if ((!sameLevel && prefixCount == 0U) || !validProjectModuleName(suffix))
            {
                return refuse("pure data require request is not canonical");
            }

            auto const separator = caller.rfind('/');
            auto directory = separator == std::string_view::npos
                           ? std::string{}
                           : std::string{caller.substr(0U, separator)};
            while (prefixCount != 0U)
            {
                if (directory.empty())
                {
                    return refuse("pure data require request traverses above its logical root");
                }
                auto const parent = directory.rfind('/');
                directory = parent == std::string::npos
                          ? std::string{}
                          : directory.substr(0U, parent);
                --prefixCount;
            }

            auto resolved = directory.empty()
                          ? std::string{suffix}
                          : directory + '/' + std::string{suffix};
            if (!validProjectModuleName(resolved))
            {
                return refuse("pure data require request resolves outside its bounded grammar");
            }
            return resolved;
        }

        // Native Luau tostring includes an encoded object pointer for tables,
        // functions and other reference types. A pure data result must not vary
        // with process layout, so the published function keeps scalar spelling
        // and maps every other value to its stable Luau type name.
        auto deterministicToString(lua_State* state) -> int
        {
            luaL_checkany(state, 1);
            switch (lua_type(state, 1))
            {
            case LUA_TNIL: lua_pushliteral(state, "nil"); break;
            case LUA_TBOOLEAN:
                lua_pushstring(state, lua_toboolean(state, 1) != 0 ? "true" : "false");
                break;
            case LUA_TNUMBER:
            case LUA_TINTEGER:
                lua_pushvalue(state, 1);
                static_cast<void>(lua_tolstring(state, -1, nullptr));
                break;
            case LUA_TSTRING: lua_pushvalue(state, 1); break;
            default: lua_pushstring(state, lua_typename(state, lua_type(state, 1))); break;
            }
            return 1;
        }

        [[nodiscard]]
        auto spendNode(ValueBudget& budget, std::size_t textBytes) -> Status
        {
            budget.nodes += 1U;
            budget.textBytes += textBytes;
            if (budget.nodes > budget.nodeCeiling)
            {
                return refuse("pure data value exceeds its fixed node ceiling");
            }
            if (budget.textBytes > budget.textCeiling)
            {
                return refuse("pure data value exceeds its fixed byte ceiling");
            }
            return ok();
        }

        [[nodiscard]]
        auto pushValue(
            lua_State* state,
            PureEnvironment const& environment,
            json::Value const& value,
            std::size_t depth,
            ValueBudget& budget
        ) -> Status;

        // A bounded copy of a refusal, in storage nothing has to destroy.
        // luaL_error leaves its frame without returning to it, so the message
        // a host reader reports cannot be held in anything that owns memory.
        [[nodiscard]]
        auto boundedText(std::string_view text) -> std::array<char, k_maximumReaderErrorBytes>
        {
            auto bounded      = std::array<char, k_maximumReaderErrorBytes>{};
            auto const length = std::min(text.size(), bounded.size() - 1U);
            std::ranges::copy(text.substr(0U, length), bounded.begin());
            return bounded;
        }

        [[nodiscard]]
        auto materializeResource(
            lua_State* state,
            PureEnvironment& environment,
            std::size_t index
        )
            -> Status
        {
            if (environment.materializedResources[index] != LUA_NOREF)
            {
                lua_getref(state, environment.materializedResources[index]);
                return ok();
            }

            auto const& resource = environment.resources[index];
            if (resource.kind == PureDataProgram::ResourceKind::Json)
            {
                auto budget = ValueBudget{
                    .nodeCeiling = k_maximumResourceValueNodes,
                    .textCeiling = k_maximumResourceValueTextBytes,
                };
                UF_TRY(
                    pushValue(
                        state,
                        environment,
                        std::get<json::Value>(resource.value),
                        0U,
                        budget
                    )
                );
            }
            else
            {
                auto const& bytes = std::get<std::string>(resource.value);
                lua_pushlstring(state, bytes.data(), bytes.size());
            }

            int const reference = lua_ref(state, -1);
            if (reference == LUA_NOREF)
            {
                return fail(AutomationErrorKind::InternalInvariant,
                            "pure data VM could not pin a materialized resource");
            }
            environment.materializedResources[index] = reference;
            return ok();
        }

        auto readResource(lua_State* state) -> int
        {
            if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)
            {
                luaL_error(state, "resource reader requires exactly one resource name");
            }

            std::size_t nameLength = 0;
            char const* p_name = lua_tolstring(state, 1, &nameLength);
            // SAFETY: upvalue 1 is installed only by pushResourceReaders and points
            // to the PureEnvironment owned by runFresh. That object outlives
            // every callback and the VM is closed before it leaves scope.
            auto* p_environment = static_cast<PureEnvironment*>(
                lua_tolightuserdata(state, lua_upvalueindex(1)));
            auto const expectedKind = static_cast<PureDataProgram::ResourceKind>(
                lua_tointeger(state, lua_upvalueindex(2))
            );
            if (
                p_environment == nullptr
                || p_name == nullptr
                || !validResourceName(std::string_view{p_name, nameLength})
            )
            {
                luaL_error(state, "resource reader rejected a non-canonical name");
            }

            auto const name = std::string_view{p_name, nameLength};
            auto const found = std::ranges::find(
                p_environment->resources,
                name,
                &DecodedResource::name
            );
            if (found == p_environment->resources.end())
            {
                luaL_error(state, "resource reader rejected an unknown resource");
            }
            if (found->kind != expectedKind)
            {
                luaL_error(state, "resource reader rejected a resource kind mismatch");
            }

            auto refusalText = std::array<char, k_maximumReaderErrorBytes>{};
            auto refused     = false;
            {
                auto const index = static_cast<std::size_t>(
                    found - p_environment->resources.begin()
                );
                auto const materialized = materializeResource(
                    state,
                    *p_environment,
                    index
                );
                refused = !materialized.has_value();
                if (refused)
                {
                    refusalText = boundedText(materialized.error().message());
                }
            }
            if (refused)
            {
                luaL_error(
                    state,
                    "resource reader could not materialize its value: %s",
                    refusalText.data()
                );
            }
            return 1;
        }

        auto pushResourceReader(
            lua_State* state,
            PureEnvironment* p_environment,
            PureDataProgram::ResourceKind kind,
            std::string_view callbackName
        ) -> void
        {
            lua_pushlightuserdata(state, p_environment);
            lua_pushinteger(state, static_cast<int>(kind));
            auto const name = std::string{callbackName};
            lua_pushcclosure(state, &readResource, name.c_str(), 2);
        }

        auto pushResourceReaders(lua_State* state, PureEnvironment* p_environment) -> void
        {
            lua_createtable(state, 0, 3);
            pushResourceReader(
                state,
                p_environment,
                PureDataProgram::ResourceKind::Json,
                "resource.readJson"
            );
            rawSetField(state, -2, k_resourceReadJson);
            pushResourceReader(
                state,
                p_environment,
                PureDataProgram::ResourceKind::Utf8,
                "resource.readText"
            );
            rawSetField(state, -2, k_resourceReadText);
            pushResourceReader(
                state,
                p_environment,
                PureDataProgram::ResourceKind::Bytes,
                "resource.readBytes"
            );
            rawSetField(state, -2, k_resourceReadBytes);
            lua_setreadonly(state, -1, 1);
        }

        // The two JSON values a Lua table cannot spell for itself: nil is not a
        // storable member, and an empty table is indistinguishable from an
        // empty array. Both directions of the boundary compare against these
        // exact objects, so the round trip is total rather than approximate.
        [[nodiscard]]
        auto createFrozenSentinels(lua_State* state, PureEnvironment& environment) -> Status
        {
            if (lua_checkstack(state, 4) == 0)
            {
                return fail(AutomationErrorKind::InternalInvariant,
                            "pure data VM cannot reserve stack for its frozen sentinels");
            }

            lua_createtable(state, 0, 0);
            lua_setreadonly(state, -1, 1);
            environment.nullReference = lua_ref(state, -1);
            lua_pop(state, 1);

            lua_createtable(state, 0, 0);
            lua_setreadonly(state, -1, 1);
            environment.emptyObjectReference = lua_ref(state, -1);
            lua_pop(state, 1);

            lua_createtable(state, 0, 2);
            lua_getref(state, environment.emptyObjectReference);
            rawSetField(state, -2, k_canonEmptyObject);
            lua_getref(state, environment.nullReference);
            rawSetField(state, -2, k_canonNull);
            lua_setreadonly(state, -1, 1);
            environment.canonReference = lua_ref(state, -1);
            lua_pop(state, 1);

            if (
                environment.nullReference == LUA_NOREF
                || environment.emptyObjectReference == LUA_NOREF
                || environment.canonReference == LUA_NOREF
            )
            {
                return fail(AutomationErrorKind::InternalInvariant,
                            "pure data VM could not pin its frozen sentinels");
            }
            return ok();
        }

        auto requireModule(lua_State* state) -> int;

        [[nodiscard]]
        auto pushPureEnvironment(
            lua_State* state,
            PureEnvironment* p_environment,
            std::optional<std::size_t> callerModule
        ) -> Status
        {
            lua_newtable(state);
            int const environment = lua_gettop(state);
            for (auto const name : k_copiedGlobals)
            {
                auto const key = std::string{name};
                if (name == "tostring")
                {
                    lua_pushcfunction(state, &deterministicToString, "tostring");
                }
                else
                {
                    lua_rawgetfield(state, LUA_GLOBALSINDEX, key.c_str());
                }
                if (lua_isnil(state, -1))
                {
                    lua_pop(state, 2);
                    return fail(AutomationErrorKind::InternalInvariant,
                                "pure data whitelist names an absent Luau global: " + key);
                }
                lua_rawsetfield(state, environment, key.c_str());
            }

            if (callerModule.has_value())
            {
                lua_pushlightuserdata(state, p_environment);
                lua_pushinteger(state, static_cast<int>(*callerModule));
                lua_pushcclosure(state, &requireModule, "require", 2);
                rawSetField(state, environment, k_requireGlobal);
            }

            pushResourceReaders(state, p_environment);
            rawSetField(state, environment, k_resourceTable);

            lua_getref(state, p_environment->canonReference);
            rawSetField(state, environment, k_canonTable);

            // No metatable is attached. In particular there is no __index path
            // to main globals, the registry, a host table, or another environment.
            lua_setreadonly(state, environment, 1);
            return ok();
        }

        [[nodiscard]]
        auto pushValue(
            lua_State* state,
            PureEnvironment const& environment,
            json::Value const& value,
            std::size_t depth,
            ValueBudget& budget
        ) -> Status
        {
            if (depth > k_maximumValueDepth)
            {
                return refuse("pure data value exceeds its fixed nesting ceiling");
            }
            if (lua_checkstack(state, 4) == 0)
            {
                return refuse("pure data value exceeds the VM stack it must be pushed onto");
            }

            switch (value.kind())
            {
            case json::ValueKind::Null:
                UF_TRY(spendNode(budget, 0U));
                lua_getref(state, environment.nullReference);
                return ok();
            case json::ValueKind::Boolean:
                UF_TRY(spendNode(budget, 0U));
                lua_pushboolean(state, value.boolean() ? 1 : 0);
                return ok();
            case json::ValueKind::Number:
                UF_TRY(spendNode(budget, 0U));
                lua_pushnumber(state, value.number());
                return ok();
            case json::ValueKind::String:
            {
                auto const text = value.string();
                UF_TRY(spendNode(budget, text.size()));
                lua_pushlstring(state, text.data(), text.size());
                return ok();
            }
            case json::ValueKind::Array:
            {
                UF_TRY(spendNode(budget, 0U));
                auto const items = value.items();
                lua_createtable(state, static_cast<int>(items.size()), 0);
                int index = 1;
                for (auto const& item : items)
                {
                    UF_TRY(pushValue(state, environment, item, depth + 1U, budget));
                    lua_rawseti(state, -2, index);
                    ++index;
                }
                lua_setreadonly(state, -1, 1);
                return ok();
            }
            case json::ValueKind::Object:
            {
                UF_TRY(spendNode(budget, 0U));
                auto const members = value.members();
                if (members.empty())
                {
                    lua_getref(state, environment.emptyObjectReference);
                    return ok();
                }
                lua_createtable(state, 0, static_cast<int>(members.size()));
                for (auto const& member : members)
                {
                    UF_TRY(spendNode(budget, member.first.size()));
                    UF_TRY(pushValue(state, environment, member.second, depth + 1U, budget));
                    rawSetField(state, -2, member.first);
                }
                lua_setreadonly(state, -1, 1);
                return ok();
            }
            }

            return fail(AutomationErrorKind::InternalInvariant, "unknown json::ValueKind value");
        }

        [[nodiscard]]
        auto isSentinel(lua_State* state, int index, int reference) -> bool
        {
            lua_getref(state, reference);
            bool const same = lua_rawequal(state, index, -1) != 0;
            lua_pop(state, 1);
            return same;
        }

        // What one Lua table's keys turn out to be. A JSON value is an array or
        // an object and never both, so the keys are counted before any of them
        // is converted: a table carrying one of each is a shape no JSON
        // document has, and saying so needs the whole key set.
        struct TableKeys final
        {
            std::size_t sequenceCount{0};
            std::size_t highestSequenceKey{0};
            std::size_t memberCount{0};
        };

        [[nodiscard]]
        auto readTableKeys(lua_State* state, int index) -> Result<TableKeys>
        {
            auto keys = TableKeys{};
            for (int iterator = lua_rawiter(state, index, 0);
                 iterator >= 0;
                 iterator = lua_rawiter(state, index, iterator))
            {
                int const keyType = lua_type(state, -2);
                if (keyType == LUA_TNUMBER)
                {
                    auto const key = lua_tonumber(state, -2);
                    if (
                        !(key >= 1.0)
                        || key > static_cast<double>(k_maximumValueNodes)
                        || std::floor(key) != key
                    )
                    {
                        lua_pop(state, 2);
                        return refuse("pure data value holds a table index no JSON array has");
                    }
                    keys.sequenceCount += 1U;
                    keys.highestSequenceKey =
                        std::max(keys.highestSequenceKey, static_cast<std::size_t>(key));
                }
                else if (keyType == LUA_TSTRING)
                {
                    keys.memberCount += 1U;
                }
                else
                {
                    lua_pop(state, 2);
                    return refuse("pure data value holds a table key no JSON member name has");
                }
                lua_pop(state, 2);
            }

            if (keys.sequenceCount != 0U && keys.memberCount != 0U)
            {
                return refuse("pure data value holds a table that is neither array nor object");
            }
            if (keys.sequenceCount != keys.highestSequenceKey)
            {
                return refuse("pure data value holds a sparse table no JSON array has");
            }
            return keys;
        }

        [[nodiscard]]
        auto readValue(
            lua_State* state,
            PureEnvironment const& environment,
            int index,
            std::size_t depth,
            ValueBudget& budget
        ) -> Result<json::Value>;

        [[nodiscard]]
        auto readTable(
            lua_State* state,
            PureEnvironment const& environment,
            int index,
            std::size_t depth,
            ValueBudget& budget
        ) -> Result<json::Value>
        {
            if (isSentinel(state, index, environment.nullReference))
            {
                return json::Value{};
            }
            if (isSentinel(state, index, environment.emptyObjectReference))
            {
                return json::Value::ofObject({});
            }

            UF_TRY_VALUE(keys, readTableKeys(state, index));
            if (keys.memberCount == 0U)
            {
                auto items = std::vector<json::Value>{};
                items.reserve(keys.sequenceCount);
                for (auto position = std::size_t{0}; position < keys.sequenceCount; ++position)
                {
                    lua_rawgeti(state, index, static_cast<int>(position) + 1);
                    auto item = readValue(state, environment, lua_gettop(state), depth + 1U, budget);
                    lua_pop(state, 1);
                    if (!item.has_value())
                    {
                        return std::unexpected{std::move(item).error()};
                    }
                    items.emplace_back(*std::move(item));
                }
                return json::Value::ofArray(std::move(items));
            }

            auto members = std::vector<json::Member>{};
            members.reserve(keys.memberCount);
            for (int iterator = lua_rawiter(state, index, 0);
                 iterator >= 0;
                 iterator = lua_rawiter(state, index, iterator))
            {
                std::size_t nameLength = 0;
                // SAFETY: the key was counted as LUA_TSTRING by readTableKeys
                // over this same table, so lua_tolstring returns the string's
                // own bytes and converts nothing in place. The bytes are copied
                // before the next iteration step can move the stack.
                char const* p_name = lua_tolstring(state, -2, &nameLength);
                auto const name = p_name == nullptr
                                      ? std::string_view{}
                                      : std::string_view{p_name, nameLength};
                if (p_name == nullptr || !isValidUtf8(name))
                {
                    lua_pop(state, 2);
                    return refuse("pure data value holds a member name that is not UTF-8");
                }
                auto memberName = std::string{name};
                auto nameCost   = spendNode(budget, nameLength);
                if (!nameCost.has_value())
                {
                    lua_pop(state, 2);
                    return std::unexpected{std::move(nameCost).error()};
                }

                auto member = readValue(state, environment, lua_gettop(state), depth + 1U, budget);
                lua_pop(state, 2);
                if (!member.has_value())
                {
                    return std::unexpected{std::move(member).error()};
                }
                members.emplace_back(std::move(memberName), *std::move(member));
            }
            return json::Value::ofObject(std::move(members));
        }

        auto readValue(
            lua_State* state,
            PureEnvironment const& environment,
            int index,
            std::size_t depth,
            ValueBudget& budget
        ) -> Result<json::Value>
        {
            if (depth > k_maximumValueDepth)
            {
                return refuse("pure data value exceeds its fixed nesting ceiling");
            }
            if (lua_checkstack(state, 4) == 0)
            {
                return refuse("pure data value exceeds the VM stack it must be read from");
            }

            switch (lua_type(state, index))
            {
            case LUA_TBOOLEAN:
                UF_TRY(spendNode(budget, 0U));
                return json::Value::ofBoolean(lua_toboolean(state, index) != 0);
            case LUA_TNUMBER:
            {
                UF_TRY(spendNode(budget, 0U));
                auto const number = lua_tonumber(state, index);
                if (!std::isfinite(number))
                {
                    return refuse("pure data value holds a number no JSON document can spell");
                }
                return json::Value::ofNumber(number);
            }
            case LUA_TSTRING:
            {
                std::size_t length = 0;
                // SAFETY: the value was checked as a string, so lua_tolstring
                // returns its own bytes without converting anything in place,
                // and they are copied before the fresh VM is closed.
                char const* p_text = lua_tolstring(state, index, &length);
                if (p_text == nullptr)
                {
                    return refuse("pure data value holds an unreadable string");
                }
                auto const text = std::string_view{p_text, length};
                UF_TRY(spendNode(budget, length));
                if (!isValidUtf8(text))
                {
                    return refuse("pure data value holds a string that is not UTF-8");
                }
                return json::Value::ofString(std::string{text});
            }
            case LUA_TTABLE:
                UF_TRY(spendNode(budget, 0U));
                return readTable(state, environment, index, depth, budget);
            default: return refuse("pure data value holds a type no JSON document has");
            }
        }

        [[nodiscard]]
        auto compileBytecode(std::string_view source, std::string_view label) -> Result<std::string>
        {
            auto options              = lua_CompileOptions{};
            options.optimizationLevel = k_compileOptimizationLevel;
            options.debugLevel        = k_compileDebugLevel;

            std::size_t bytecodeSize = 0;
            // SAFETY: luau_compile returns a malloc-owned buffer of exactly
            // bytecodeSize bytes. It is copied before the paired free below.
            char* p_bytecode = luau_compile(source.data(), source.size(), &options, &bytecodeSize);
            if (p_bytecode == nullptr)
            {
                return fail(AutomationErrorKind::InternalInvariant,
                            "luau_compile allocation failed for " + std::string{label});
            }
            auto bytecodeGuard = scopeExit([p_bytecode]() noexcept {
                // SAFETY: pairs the malloc performed by luau_compile.
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
                std::free(p_bytecode);
            });

            if (bytecodeSize == 0U)
            {
                return refuse("compiled pure data bytecode is empty");
            }
            if (static_cast<unsigned char>(p_bytecode[0]) == 0U)
            {
                auto message =
                    "pure data source failed to compile: " + std::string{label} + ':';
                auto const detailBytes = std::min(
                    bytecodeSize - 1U,
                    k_maximumErrorBytes - message.size()
                );
                message.append(p_bytecode + 1, detailBytes);
                return refuse(std::move(message));
            }
            return std::string{p_bytecode, bytecodeSize};
        }

        [[nodiscard]]
        auto topError(lua_State* thread) -> std::string
        {
            std::size_t length = 0;
            char const* p_text = lua_tolstring(thread, -1, &length);
            if (p_text == nullptr)
                return "(non-string error value)";
            if (length <= k_maximumErrorBytes)
                return std::string{p_text, length};
            return std::string{p_text, k_maximumErrorBytes} + "... (truncated)";
        }

        [[nodiscard]]
        auto resume(
            lua_State* thread,
            int argumentCount,
            InterruptState& control,
            PureEnvironment* p_environment = nullptr
        ) -> Status
        {
            int const status = lua_resume(thread, nullptr, argumentCount);
            auto const* p_quota = quotaFor(thread);
            if (
                p_environment != nullptr
                && p_quota != nullptr
                && p_quota->ceilingRefused
            )
            {
                p_environment->terminalKind = AutomationErrorKind::InvalidResource;
                p_environment->terminalMessage =
                    "pure data VM exhausted its fixed memory quota";
                return refuse("pure data VM exhausted its fixed memory quota");
            }
            if (status == LUA_ERRMEM)
            {
                if (p_environment != nullptr)
                {
                    p_environment->terminalKind = AutomationErrorKind::InvalidResource;
                    p_environment->terminalMessage =
                        "pure data VM exhausted its fixed memory quota";
                }
                return refuse("pure data VM exhausted its fixed memory quota");
            }
            if (status == LUA_BREAK || control.broken())
            {
                if (
                    p_environment != nullptr
                    && p_environment->terminalKind.has_value()
                )
                {
                    return fail(
                        *p_environment->terminalKind,
                        p_environment->terminalMessage
                    );
                }
                return fail(AutomationErrorKind::Cancelled,
                            "pure data program hard-cancelled [" + describeBreak(control) + "]");
            }
            if (status == LUA_YIELD)
            {
                return refuse("pure data program yielded");
            }
            if (status != LUA_OK)
            {
                return refuse("pure data program failed: " + topError(thread));
            }
            if (
                p_environment != nullptr
                && p_environment->terminalKind.has_value()
            )
            {
                return fail(
                    *p_environment->terminalKind,
                    p_environment->terminalMessage
                );
            }
            return ok();
        }

        struct StackReservation final
        {
            int  slots{0};
            bool succeeded{false};
        };

        auto reserveStackProtected(lua_State* state) -> int
        {
            auto* p_reservation = static_cast<StackReservation*>(
                lua_tolightuserdata(state, 1)
            );
            if (p_reservation == nullptr)
            {
                luaL_error(state, "pure data stack reservation context is invalid");
            }
            p_reservation->succeeded = lua_checkstack(state, p_reservation->slots) != 0;
            return 0;
        }

        [[nodiscard]]
        auto reserveStack(
            lua_State* state,
            int slots,
            PureEnvironment* p_environment = nullptr
        ) -> Status
        {
            auto reservation = StackReservation{.slots = slots};
            int const status = lua_cpcall(state, &reserveStackProtected, &reservation);
            if (status == LUA_ERRMEM)
            {
                if (p_environment != nullptr)
                {
                    p_environment->terminalKind = AutomationErrorKind::InvalidResource;
                    p_environment->terminalMessage =
                        "pure data VM exhausted its fixed memory quota reserving a stack";
                }
                return refuse("pure data VM exhausted its fixed memory quota reserving a stack");
            }
            if (status != LUA_OK)
            {
                return refuse("pure data VM could not reserve a protected stack: " + topError(state));
            }
            if (!reservation.succeeded)
            {
                return refuse("pure data value exceeds the VM stack it must use");
            }
            return ok();
        }

        [[nodiscard]]
        auto executeModule(
            lua_State* mainState,
            std::string_view bytecode,
            std::string_view label,
            PureEnvironment* p_environment,
            InterruptState& control,
            std::optional<std::size_t> callerModule,
            bool requireTable
        ) -> Status
        {
            auto const initialTop = lua_gettop(mainState);
            auto resetStack = scopeExit(
                [mainState, initialTop]() noexcept { lua_settop(mainState, initialTop); }
            );

            UF_TRY(pushPureEnvironment(mainState, p_environment, callerModule));
            int const environment = lua_gettop(mainState);
            lua_State* thread = lua_newthread(mainState);
            UF_TRY(reserveStack(thread, 4, p_environment));

            lua_xpush(mainState, thread, environment);
            lua_pushthread(thread);
            lua_pushvalue(thread, 1);
            lua_setfenv(thread, -2);
            lua_pop(thread, 1);

            auto const name = std::string{label};
            int const loadStatus =
                luau_load(thread, name.c_str(), bytecode.data(), bytecode.size(), 1);
            if (loadStatus != LUA_OK)
            {
                return refuse("pure data bytecode failed to load: " + topError(thread));
            }
            lua_remove(thread, 1);
            UF_TRY(resume(thread, 0, control, p_environment));
            if (
                lua_gettop(thread) != 1
                || lua_isnil(thread, 1)
                || (requireTable && !lua_istable(thread, 1))
            )
            {
                return refuse(
                    requireTable
                        ? "pure data entry module must return exactly one table"
                        : "pure data dependency must return exactly one non-nil value"
                );
            }

            lua_xpush(thread, mainState, 1);
            lua_remove(mainState, environment + 1);
            lua_remove(mainState, environment);
            resetStack.release();
            return ok();
        }

        [[nodiscard]]
        auto boundedFailure(std::string_view message) -> std::string
        {
            if (message.size() <= k_maximumCachedFailureBytes)
            {
                return std::string{message};
            }
            return std::string{message.substr(0U, k_maximumCachedFailureBytes)};
        }

        [[nodiscard]]
        auto loadRegisteredModule(
            lua_State* mainState,
            PureEnvironment& environment,
            std::size_t index,
            bool requireTable
        ) -> Status
        {
            auto& runtime = environment.moduleRuntime[index];
            auto const& module = environment.modules[index];
            switch (runtime.state)
            {
            case ModuleLoadState::Loaded:
                lua_getref(mainState, runtime.reference);
                if (requireTable && !lua_istable(mainState, -1))
                {
                    lua_pop(mainState, 1);
                    return refuse("pure data entry module must return exactly one table");
                }
                return ok();
            case ModuleLoadState::Loading:
                return refuse("pure data module dependency cycle: " + module.name);
            case ModuleLoadState::Failed: return refuse(runtime.failure);
            case ModuleLoadState::Unloaded: break;
            }

            runtime.state = ModuleLoadState::Loading;
            auto executed = executeModule(
                mainState,
                module.bytecode,
                module.name,
                &environment,
                *environment.pControl,
                index,
                requireTable
            );
            if (!executed.has_value())
            {
                auto const kind = automationErrorKind(executed.error());
                if (
                    environment.terminalKind.has_value()
                    || kind == AutomationErrorKind::Cancelled
                    || kind == AutomationErrorKind::InternalInvariant
                )
                {
                    runtime.state = ModuleLoadState::Unloaded;
                    return std::unexpected{std::move(executed).error()};
                }
                runtime.state   = ModuleLoadState::Failed;
                runtime.failure = boundedFailure(executed.error().message());
                return std::unexpected{std::move(executed).error()};
            }

            if (module.frameworkOwned && lua_istable(mainState, -1))
            {
                auto frozen = deepFreeze(mainState, -1);
                if (!frozen.has_value())
                {
                    runtime.state = ModuleLoadState::Unloaded;
                    auto error    = std::move(frozen).error();
                    error.addContext(
                        "freezing trusted Framework module " + module.name
                    );
                    return std::unexpected{std::move(error)};
                }
            }

            runtime.reference = lua_ref(mainState, -1);
            if (runtime.reference == LUA_NOREF)
            {
                runtime.state   = ModuleLoadState::Failed;
                runtime.failure = "pure data VM could not pin a loaded module";
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    runtime.failure
                );
            }
            runtime.state = ModuleLoadState::Loaded;
            return ok();
        }

        auto requireModule(lua_State* state) -> int
        {
            if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)
            {
                luaL_error(state, "require expects exactly one module-name string");
            }

            // SAFETY: both upvalues are installed only by pushPureEnvironment.
            // The environment and its fixed module vectors outlive this fresh
            // VM, and the integer is checked before it selects either vector.
            auto* p_environment = static_cast<PureEnvironment*>(
                lua_tolightuserdata(state, lua_upvalueindex(1))
            );
            auto const caller = lua_tointeger(state, lua_upvalueindex(2));
            if (
                p_environment == nullptr
                || caller < 0
                || static_cast<std::size_t>(caller) >= p_environment->modules.size()
            )
            {
                luaL_error(state, "require has an invalid host module context");
            }

            std::size_t requestLength = 0;
            char const* p_request = lua_tolstring(state, 1, &requestLength);
            auto refusalText = std::array<char, k_maximumReaderErrorBytes>{};
            auto moduleIndex = std::optional<std::size_t>{};
            {
                auto const resolved = resolveModuleRequest(
                    p_environment->modules[static_cast<std::size_t>(caller)].name,
                    std::string_view{p_request, requestLength}
                );
                if (!resolved.has_value())
                {
                    refusalText = boundedText(resolved.error().message());
                }
                else
                {
                    auto const found = std::ranges::lower_bound(
                        p_environment->modules,
                        *resolved,
                        {},
                        &CompiledModule::name
                    );
                    if (
                        found == p_environment->modules.end()
                        || found->name != *resolved
                    )
                    {
                        refusalText = boundedText("pure data require rejected an unknown module");
                    }
                    else
                    {
                        moduleIndex = static_cast<std::size_t>(
                            found - p_environment->modules.begin()
                        );
                    }
                }
            }
            if (!moduleIndex.has_value())
            {
                luaL_error(state, "%s", refusalText.data());
            }

            auto terminalFailure = false;
            {
                auto const loaded = loadRegisteredModule(
                    lua_mainthread(state),
                    *p_environment,
                    *moduleIndex,
                    false
                );
                if (!loaded.has_value())
                {
                    refusalText = boundedText(loaded.error().message());
                    if (
                        p_environment->terminalKind.has_value()
                        || automationErrorKind(loaded.error())
                            == AutomationErrorKind::Cancelled
                        || automationErrorKind(loaded.error())
                            == AutomationErrorKind::InternalInvariant
                    )
                    {
                        if (!p_environment->terminalKind.has_value())
                        {
                            p_environment->terminalKind = automationErrorKind(loaded.error());
                            p_environment->terminalMessage = std::string{loaded.error().message()};
                        }
                        terminalFailure = true;
                    }
                }
                else
                {
                    auto* mainState = lua_mainthread(state);
                    lua_xpush(mainState, state, -1);
                    lua_pop(mainState, 1);
                    return 1;
                }
            }
            if (terminalFailure)
            {
                return lua_break(state);
            }
            luaL_error(state, "%s", refusalText.data());
        }

        auto pushEntryPoints(lua_State* thread, std::span<std::string const> entryPoints) -> void
        {
            lua_createtable(thread, static_cast<int>(entryPoints.size()), 0);
            int index = 1;
            for (auto const& entryPoint : entryPoints)
            {
                lua_pushlstring(thread, entryPoint.data(), entryPoint.size());
                lua_rawseti(thread, -2, index);
                ++index;
            }
            lua_setreadonly(thread, -1, 1);
        }

        [[nodiscard]]
        auto inspectModule(
            lua_State* mainState,
            int bridge,
            int plugin,
            std::string_view pluginId,
            std::span<std::string const> entryPoints,
            PureEnvironment& environment,
            InterruptState& control
        ) -> Status
        {
            lua_State* thread = lua_newthread(mainState);
            UF_TRY(reserveStack(thread, 4, &environment));
            lua_rawgetfield(mainState, bridge, "inspect");
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            lua_xpush(mainState, thread, plugin);
            lua_pushlstring(mainState, pluginId.data(), pluginId.size());
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            pushEntryPoints(mainState, entryPoints);
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            UF_TRY(resume(thread, 3, control, &environment));
            lua_pop(mainState, 1);
            return ok();
        }

        [[nodiscard]]
        auto invokeModule(
            lua_State* mainState,
            int bridge,
            int plugin,
            std::string_view pluginId,
            std::span<std::string const> entryPoints,
            std::string_view entryPoint,
            json::Value const& input,
            PureEnvironment& environment,
            InterruptState& control
        ) -> Result<json::Value>
        {
            lua_State* thread = lua_newthread(mainState);
            UF_TRY(reserveStack(thread, k_valueStackSlots, &environment));
            lua_rawgetfield(mainState, bridge, "invoke");
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            lua_xpush(mainState, thread, plugin);
            lua_pushlstring(mainState, pluginId.data(), pluginId.size());
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            pushEntryPoints(mainState, entryPoints);
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            lua_pushlstring(mainState, entryPoint.data(), entryPoint.size());
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);

            auto inputBudget = ValueBudget{};
            UF_TRY_CONTEXT(pushValue(mainState, environment, input, 0U, inputBudget),
                           "pushing the decoded ProjectPlugin input");
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            UF_TRY(resume(thread, 5, control, &environment));
            if (lua_gettop(thread) != 1)
            {
                return refuse("pure data program returned other than one value");
            }

            auto outputBudget = ValueBudget{};
            return readValue(thread, environment, 1, 0U, outputBudget);
        }

        [[nodiscard]]
        auto proveResourcesMaterialize(
            lua_State* state,
            PureEnvironment& environment
        ) -> Status
        {
            for (auto index = std::size_t{0}; index < environment.resources.size(); ++index)
            {
                if (environment.materializedResources[index] != LUA_NOREF)
                {
                    continue;
                }
                if (lua_checkstack(state, 3) == 0)
                {
                    return fail(AutomationErrorKind::InternalInvariant,
                                "pure data VM cannot reserve stack to admit its resources");
                }

                auto const& resource = environment.resources[index];
                lua_pushlightuserdata(state, &environment);
                lua_pushinteger(state, static_cast<int>(resource.kind));
                lua_pushcclosure(state, &readResource, "resource admission", 2);
                lua_pushlstring(state, resource.name.data(), resource.name.size());
                if (lua_pcall(state, 1, 1, 0) != LUA_OK)
                {
                    auto message =
                        "pure data resource cannot be materialized inside its VM quota: "
                        + resource.name + ": " + topError(state);
                    lua_pop(state, 1);
                    return refuse(std::move(message));
                }

                lua_pop(state, 1);
                lua_unref(state, environment.materializedResources[index]);
                environment.materializedResources[index] = LUA_NOREF;
                lua_gc(state, LUA_GCCOLLECT, 0);
            }
            return ok();
        }

        struct FreshRunContext final
        {
            VmRun*           pVm{nullptr};
            PureEnvironment* pEnvironment{nullptr};

            std::string_view             bridgeBytecode{};
            std::string_view             pluginId{};
            std::size_t                  entryModuleIndex{};
            std::span<std::string const> entryPoints{};
            std::string_view             entryPoint{};
            json::Value const*           pInput{nullptr};
            bool                         invoke{false};

            std::optional<Result<json::Value>> result{};
        };

        [[nodiscard]]
        auto runInsideProtectedCall(FreshRunContext& context) -> Result<json::Value>
        {
            auto& vm          = *context.pVm;
            auto& environment = *context.pEnvironment;

            luaL_openlibs(vm.state);
            nilLibraryField(vm.state, "math", "random");
            nilLibraryField(vm.state, "math", "randomseed");
            nilLibraryField(vm.state, "string", "dump");
            installInterrupt(vm.state, &vm.control);
            luaL_sandbox(vm.state);
            UF_TRY(createFrozenSentinels(vm.state, environment));
            vm.control.beginUnitOfScript(k_maximumRuntime);

            UF_TRY(
                executeModule(
                    vm.state,
                    context.bridgeBytecode,
                    "pure-data-bridge",
                    &environment,
                    vm.control,
                    std::nullopt,
                    true
                )
            );
            int const bridge = lua_gettop(vm.state);
            UF_TRY(
                loadRegisteredModule(
                    vm.state,
                    environment,
                    context.entryModuleIndex,
                    true
                )
            );
            int const plugin = lua_gettop(vm.state);
            UF_TRY(
                inspectModule(
                    vm.state,
                    bridge,
                    plugin,
                    context.pluginId,
                    context.entryPoints,
                    environment,
                    vm.control
                )
            );
            if (!context.invoke)
            {
                UF_TRY(proveResourcesMaterialize(vm.state, environment));
                return json::Value{};
            }
            return invokeModule(
                vm.state,
                bridge,
                plugin,
                context.pluginId,
                context.entryPoints,
                context.entryPoint,
                *context.pInput,
                environment,
                vm.control
            );
        }

        // lua_cpcall supplies the context as its sole light-userdata argument.
        // This callback is deliberately the outermost Luau frame: every Luau
        // allocation after state creation, including host-side argument pushes,
        // must unwind to it when the quota allocator refuses growth.
        auto runProtected(lua_State* state) -> int
        {
            auto* p_context = static_cast<FreshRunContext*>(lua_tolightuserdata(state, 1));
            if (p_context == nullptr || p_context->pVm == nullptr
                || p_context->pEnvironment == nullptr || p_context->pInput == nullptr)
            {
                luaL_error(state, "pure data protected-call context is invalid");
            }
            lua_settop(state, 0);
            p_context->result.emplace(runInsideProtectedCall(*p_context));
            return 0;
        }

        [[nodiscard]]
        auto runFresh(
            std::string_view bridgeBytecode,
            std::string_view pluginId,
            std::size_t entryModuleIndex,
            std::span<CompiledModule const> modules,
            std::span<std::string const> entryPoints,
            std::span<DecodedResource const> resources,
            std::string_view entryPoint,
            json::Value const& input,
            bool invoke
        ) -> Result<json::Value>
        {
            auto vm  = VmRun{};
            vm.state = createStateWithQuota(&vm.quota);
            if (vm.state == nullptr)
            {
                return fail(AutomationErrorKind::InternalInvariant,
                            "pure data VM allocation failed");
            }
            auto stateGuard = scopeExit([&vm]() noexcept { lua_close(vm.state); });

            auto environment = PureEnvironment{
                .modules       = modules,
                .resources     = resources,
                .moduleRuntime = std::vector<ModuleRuntime>(modules.size()),
                .materializedResources = std::vector<int>(resources.size(), LUA_NOREF),
                .pControl        = &vm.control,
                .terminalKind    = {},
                .terminalMessage = {},
            };

            auto context = FreshRunContext{
                .pVm              = &vm,
                .pEnvironment     = &environment,
                .bridgeBytecode   = bridgeBytecode,
                .pluginId         = pluginId,
                .entryModuleIndex = entryModuleIndex,
                .entryPoints      = entryPoints,
                .entryPoint       = entryPoint,
                .pInput           = &input,
                .invoke           = invoke,
                .result           = {},
            };
            int const status = lua_cpcall(vm.state, &runProtected, &context);
            if (status != LUA_OK)
            {
                auto const detail = topError(vm.state);
                if (status == LUA_ERRMEM)
                {
                    return refuse(
                        "pure data VM exhausted its fixed memory quota: " + detail
                    );
                }
                return refuse("pure data VM protected call failed: " + detail);
            }
            if (!context.result.has_value())
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "pure data VM protected call returned without a result"
                );
            }
            return *std::move(context.result);
        }

        [[nodiscard]]
        auto validIdentifier(std::string_view value) -> bool
        {
            if (
                value.empty()
                || value.size() > k_maximumPluginIdBytes
                || !isValidUtf8(value)
            )
            {
                return false;
            }
            return std::ranges::none_of(value, [](char character) {
                return static_cast<unsigned char>(character) < 0x20U;
            });
        }

    } // namespace

    class PureDataProgram::State final
    {
    public:
        std::string                  pluginId{};
        std::size_t                  entryModuleIndex{};
        std::vector<std::string>     entryPoints{};
        std::vector<CompiledModule>  modules{};
        std::vector<DecodedResource> resources{};
        std::string                  bridgeBytecode{};
    };

    PureDataProgram::PureDataProgram(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto PureDataProgram::validateModuleClosure(
        std::string_view entryModule,
        std::span<Module const> modules
    ) -> Status
    {
        if (!validProjectModuleName(entryModule))
        {
            return refuse("pure data entry module name is not canonical");
        }
        if (modules.empty() || modules.size() > k_maximumModuleCount)
        {
            return refuse("pure data module count exceeds its fixed ceiling");
        }

        auto totalSourceBytes = std::size_t{0};
        auto names            = std::vector<std::string_view>{};
        names.reserve(modules.size());
        for (auto const& module : modules)
        {
            if (!validProjectModuleName(module.name))
            {
                return refuse("pure data module name is not canonical");
            }
            if (
                module.source.empty()
                || module.source.size() > k_maximumModuleSourceBytes
                || !isValidUtf8(module.source)
            )
            {
                return refuse("pure data module source must be non-empty bounded UTF-8");
            }
            if (
                totalSourceBytes
                > k_maximumModuleClosureSourceBytes - module.source.size()
            )
            {
                return refuse("pure data module sources exceed their total byte ceiling");
            }
            totalSourceBytes += module.source.size();
            names.emplace_back(module.name);
        }
        std::ranges::sort(names);
        if (std::ranges::adjacent_find(names) != names.end())
        {
            return refuse("pure data module names must be unique");
        }
        if (!std::ranges::binary_search(names, entryModule))
        {
            return refuse("pure data entry module is absent from its closure");
        }
        return ok();
    }

    auto PureDataProgram::validateResourceClosure(
        std::span<Resource const> resources
    ) -> Status
    {
        if (resources.size() > k_maximumResourceCount)
        {
            return refuse("pure data resource count exceeds its fixed ceiling");
        }

        auto totalResourceBytes = std::size_t{0};
        auto names              = std::vector<std::string_view>{};
        names.reserve(resources.size());
        for (auto const& resource : resources)
        {
            if (!validResourceName(resource.name))
            {
                return refuse("pure data resource name is not canonical");
            }
            if (resource.bytes.size() > k_maximumResourceBytes)
            {
                return refuse("pure data resource exceeds its fixed byte ceiling");
            }
            if (
                totalResourceBytes
                > k_maximumResourceClosureBytes - resource.bytes.size()
            )
            {
                return refuse("pure data resources exceed their total byte ceiling");
            }
            totalResourceBytes += resource.bytes.size();
            names.emplace_back(resource.name);

            switch (resource.kind)
            {
            case ResourceKind::Json:
                if (!json::parse(resource.bytes).has_value())
                    return refuse("pure data JSON resource is invalid: " + resource.name);
                break;
            case ResourceKind::Utf8:
                if (!isValidUtf8(resource.bytes))
                    return refuse("pure data UTF-8 resource is invalid: " + resource.name);
                break;
            case ResourceKind::Bytes: break;
            default: return refuse("pure data resource kind is invalid");
            }
        }
        std::ranges::sort(names);
        if (std::ranges::adjacent_find(names) != names.end())
        {
            return refuse("pure data resource names must be unique");
        }
        return ok();
    }

    auto PureDataProgram::compile(
        std::string_view pluginId,
        std::string_view entryModule,
        std::vector<Module> modules,
        std::span<std::string_view const> entryPoints,
        std::vector<Resource> resources,
        std::span<FrameworkModule const> frameworkModules
    ) -> Result<PureDataProgram>
    {
        if (!validIdentifier(pluginId))
        {
            return refuse("pure data plugin id is invalid");
        }
        UF_TRY(validateModuleClosure(entryModule, modules));
        if (
            entryPoints.empty()
            || entryPoints.size() > k_maximumEntryPointCount
        )
        {
            return refuse("pure data module requires a bounded entry-point set");
        }
        std::ranges::sort(modules, {}, &Module::name);
        auto const entry = std::ranges::lower_bound(
            modules,
            entryModule,
            {},
            &Module::name
        );
        if (entry == modules.end() || entry->name != entryModule)
        {
            return refuse("pure data entry module is absent from its closure");
        }
        auto const projectEntryModuleIndex = static_cast<std::size_t>(
            entry - modules.begin()
        );

        UF_TRY(validateResourceClosure(resources));
        std::ranges::sort(resources, {}, &Resource::name);

        auto decodedResources = std::vector<DecodedResource>{};
        decodedResources.reserve(resources.size());
        for (auto& resource : resources)
        {
            switch (resource.kind)
            {
            case ResourceKind::Json:
            {
                auto parsed = json::parse(resource.bytes);
                if (!parsed.has_value())
                {
                    return refuse(
                        "pure data JSON resource is invalid: " + resource.name + ": "
                        + std::string{parsed.error().message()}
                    );
                }
                decodedResources.emplace_back(DecodedResource{
                    .kind  = resource.kind,
                    .name  = std::move(resource.name),
                    .value = *std::move(parsed),
                });
                break;
            }
            case ResourceKind::Utf8:
                if (!isValidUtf8(resource.bytes))
                {
                    return refuse(
                        "pure data UTF-8 resource is invalid: " + resource.name
                    );
                }
                decodedResources.emplace_back(DecodedResource{
                    .kind  = resource.kind,
                    .name  = std::move(resource.name),
                    .value = std::move(resource.bytes),
                });
                break;
            case ResourceKind::Bytes:
                decodedResources.emplace_back(DecodedResource{
                    .kind  = resource.kind,
                    .name  = std::move(resource.name),
                    .value = std::move(resource.bytes),
                });
                break;
            default: return refuse("pure data resource kind is invalid");
            }
        }

        auto ownedEntryPoints = std::vector<std::string>{};
        ownedEntryPoints.reserve(entryPoints.size());
        for (auto const entryPoint : entryPoints)
        {
            if (!validIdentifier(entryPoint) || entryPoint.size() > k_maximumEntryPointBytes)
            {
                return refuse("pure data entry point is invalid");
            }
            ownedEntryPoints.emplace_back(entryPoint);
        }
        std::ranges::sort(ownedEntryPoints);
        if (std::ranges::adjacent_find(ownedEntryPoints) != ownedEntryPoints.end())
        {
            return refuse("pure data entry points must be unique");
        }

        UF_TRY_VALUE(bridgeBytecode, compileBytecode(k_bridgeSource, "bridge"));
        if (
            detail::classifyModuleBytecode(
                bridgeBytecode.size(),
                0U,
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleBytecodeBytes
            )
            != detail::ModuleBytecodeAdmission::Accepted
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "trusted pure data bridge bytecode exceeds its fixed ceiling"
            );
        }
        if (frameworkModules.size() > k_maximumFrameworkModuleCount)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Framework pure module count exceeds its fixed ceiling"
            );
        }
        auto orderedFrameworkModules = std::vector<FrameworkModule>{
            frameworkModules.begin(),
            frameworkModules.end(),
        };
        std::ranges::sort(orderedFrameworkModules, {}, &FrameworkModule::name);
        if (
            std::ranges::adjacent_find(
                orderedFrameworkModules,
                {},
                &FrameworkModule::name
            ) != orderedFrameworkModules.end()
        )
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Framework pure module names must be unique"
            );
        }

        auto compiledModules = std::vector<CompiledModule>{};
        compiledModules.reserve(modules.size() + orderedFrameworkModules.size());
        auto frameworkSourceBytes   = std::size_t{};
        auto frameworkBytecodeBytes = std::size_t{};
        for (auto const& module : orderedFrameworkModules)
        {
            if (
                !validFrameworkModuleName(module.name)
                || module.source.empty()
                || module.source.size() > k_maximumModuleSourceBytes
                || !isValidUtf8(module.source)
                || frameworkSourceBytes
                    > k_maximumModuleClosureSourceBytes - module.source.size()
            )
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Framework pure module is not canonical bounded UTF-8: "
                        + std::string{module.name}
                );
            }
            frameworkSourceBytes += module.source.size();
            UF_TRY_VALUE(bytecode, compileBytecode(module.source, module.name));
            switch (detail::classifyModuleBytecode(
                bytecode.size(),
                frameworkBytecodeBytes,
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes
            ))
            {
            case detail::ModuleBytecodeAdmission::Accepted: break;
            case detail::ModuleBytecodeAdmission::Empty:
            case detail::ModuleBytecodeAdmission::ModuleCeiling:
            case detail::ModuleBytecodeAdmission::ClosureCeiling:
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Framework pure module bytecode exceeds its fixed ceiling: "
                        + std::string{module.name}
                );
            }
            frameworkBytecodeBytes += bytecode.size();
            compiledModules.emplace_back(CompiledModule{
                .name           = std::string{module.name},
                .bytecode       = std::move(bytecode),
                .frameworkOwned = true,
            });
        }
        auto const entryModuleIndex = orderedFrameworkModules.size()
            + projectEntryModuleIndex;
        auto totalBytecodeBytes = std::size_t{0};
        for (auto& module : modules)
        {
            UF_TRY_VALUE(bytecode, compileBytecode(module.source, module.name));
            switch (detail::classifyModuleBytecode(
                bytecode.size(),
                totalBytecodeBytes,
                PureDataProgram::k_maximumModuleBytecodeBytes,
                PureDataProgram::k_maximumModuleClosureBytecodeBytes
            ))
            {
            case detail::ModuleBytecodeAdmission::Empty:
                return refuse("compiled pure data bytecode is empty");
            case detail::ModuleBytecodeAdmission::ModuleCeiling:
                return refuse("compiled pure data bytecode exceeds its fixed ceiling");
            case detail::ModuleBytecodeAdmission::ClosureCeiling:
                return refuse("pure data module bytecode exceeds its total byte ceiling");
            case detail::ModuleBytecodeAdmission::Accepted: break;
            }
            totalBytecodeBytes += bytecode.size();
            compiledModules.emplace_back(CompiledModule{
                .name           = std::move(module.name),
                .bytecode       = std::move(bytecode),
                .frameworkOwned = false,
            });
        }
        UF_TRY(
            runFresh(
                bridgeBytecode,
                pluginId,
                entryModuleIndex,
                compiledModules,
                ownedEntryPoints,
                decodedResources,
                {},
                json::Value{},
                false
            )
        );

        auto state = std::make_shared<State>(State{
            .pluginId         = std::string{pluginId},
            .entryModuleIndex = entryModuleIndex,
            .entryPoints      = std::move(ownedEntryPoints),
            .modules          = std::move(compiledModules),
            .resources        = std::move(decodedResources),
            .bridgeBytecode   = std::move(bridgeBytecode),
        });
        return PureDataProgram{std::shared_ptr<State const>{std::move(state)}};
    }

    auto PureDataProgram::invoke(
        std::string_view entryPoint,
        json::Value const& immutableInput
    ) const -> Result<json::Value>
    {
        if (!std::ranges::binary_search(m_state->entryPoints, entryPoint))
        {
            return refuse("pure data entry point is not registered");
        }
        return runFresh(
            m_state->bridgeBytecode,
            m_state->pluginId,
            m_state->entryModuleIndex,
            m_state->modules,
            m_state->entryPoints,
            m_state->resources,
            entryPoint,
            immutableInput,
            true
        );
    }

    auto pureEnvironmentGlobals() -> std::span<std::string_view const>
    {
        return k_pureGlobals;
    }

    // The exact bytes the environment's identity is taken over, as one
    // canonical JSON object: the bridge that wraps every call, the versioned
    // contract each published function answers to, the frozen tables published
    // beside the whitelist, and the whitelist itself. Members are in JCS order,
    // so the object is canonical as written.
    auto pluginEnvironmentMaterial() -> std::string
    {
        auto const resourceReadBytesName =
            std::string{k_resourceTable} + '.' + std::string{k_resourceReadBytes};
        auto const resourceReadJsonName =
            std::string{k_resourceTable} + '.' + std::string{k_resourceReadJson};
        auto const resourceReadTextName =
            std::string{k_resourceTable} + '.' + std::string{k_resourceReadText};

        auto output = std::string{"{\"bridge_source\":"};
        appendJsonString(output, k_bridgeSource);
        output += ",\"compiler\":{\"debug_level\":";
        output += std::to_string(k_compileDebugLevel);
        output += ",\"optimization_level\":";
        output += std::to_string(k_compileOptimizationLevel);
        output += ",\"remaining_options\":\"default_zero_v1\"}";
        output += ",\"contracts\":{";
        appendJsonString(output, k_requireGlobal);
        output += ':';
        appendJsonString(output, k_requireContract);
        output += ',';
        appendJsonString(output, resourceReadBytesName);
        output += ':';
        appendJsonString(output, k_resourceReadBytesContract);
        output += ',';
        appendJsonString(output, resourceReadJsonName);
        output += ':';
        appendJsonString(output, k_resourceReadJsonContract);
        output += ',';
        appendJsonString(output, resourceReadTextName);
        output += ':';
        appendJsonString(output, k_resourceReadTextContract);
        output += ',';
        appendJsonString(output, "tostring");
        output += ':';
        appendJsonString(output, k_tostringContract);
        output += "},\"frozen_tables\":{";
        appendJsonString(output, k_canonTable);
        output += ":[";
        appendJsonString(output, k_canonEmptyObject);
        output += ',';
        appendJsonString(output, k_canonNull);
        output += "],";
        appendJsonString(output, k_resourceTable);
        output += ":[";
        appendJsonString(output, k_resourceReadBytes);
        output += ',';
        appendJsonString(output, k_resourceReadJson);
        output += ',';
        appendJsonString(output, k_resourceReadText);
        output += "]},\"globals\":[";
        auto separated = false;
        for (auto const name : k_pureGlobals)
        {
            if (separated)
            {
                output += ',';
            }
            separated = true;
            appendJsonString(output, name);
        }
        output += "],\"grammars\":{\"module\":";
        appendJsonString(output, k_moduleGrammarContract);
        output += ",\"resource\":";
        appendJsonString(output, k_resourceGrammarContract);
        output += "},\"interrupt_contract\":";
        appendJsonString(output, k_interruptContract);
        output += ",\"limits\":{";

        auto appendLimit = [&output](std::string_view name, auto value) -> void {
            appendJsonString(output, name);
            output += ':';
            output += std::to_string(value);
        };
        appendLimit("cached_failure_bytes", k_maximumCachedFailureBytes);
        output += ',';
        appendLimit("entry_point_count", k_maximumEntryPointCount);
        output += ',';
        appendLimit("entry_point_name_bytes", k_maximumEntryPointBytes);
        output += ',';
        appendLimit("framework_module_count", k_maximumFrameworkModuleCount);
        output += ',';
        appendLimit("host_error_bytes", k_maximumErrorBytes);
        output += ',';
        appendLimit("instruction_budget_ticks", k_interruptBudgetTicks);
        output += ',';
        appendLimit(
            "module_bytecode_bytes",
            PureDataProgram::k_maximumModuleBytecodeBytes
        );
        output += ',';
        appendLimit(
            "module_bytecode_total_bytes",
            PureDataProgram::k_maximumModuleClosureBytecodeBytes
        );
        output += ',';
        appendLimit("module_count", PureDataProgram::k_maximumModuleCount);
        output += ',';
        appendLimit("module_name_bytes", k_maximumModuleNameBytes);
        output += ',';
        appendLimit("module_request_bytes", k_maximumModuleNameBytes);
        output += ',';
        appendLimit("module_resolved_name_bytes", k_maximumModuleNameBytes);
        output += ',';
        appendLimit("module_segment_bytes", k_maximumModuleSegmentBytes);
        output += ',';
        appendLimit("module_segments", k_maximumModuleSegments);
        output += ',';
        appendLimit(
            "module_source_bytes",
            PureDataProgram::k_maximumModuleSourceBytes
        );
        output += ',';
        appendLimit(
            "module_source_total_bytes",
            PureDataProgram::k_maximumModuleClosureSourceBytes
        );
        output += ',';
        appendLimit("plugin_id_bytes", k_maximumPluginIdBytes);
        output += ',';
        appendLimit("reader_error_bytes", k_maximumReaderErrorBytes);
        output += ',';
        appendLimit("resource_bytes", PureDataProgram::k_maximumResourceBytes);
        output += ',';
        appendLimit("resource_count", PureDataProgram::k_maximumResourceCount);
        output += ',';
        appendLimit("resource_json_depth", k_maximumValueDepth);
        output += ',';
        appendLimit("resource_json_nodes", k_maximumResourceValueNodes);
        output += ',';
        appendLimit("resource_json_text_bytes", k_maximumResourceValueTextBytes);
        output += ',';
        appendLimit("resource_name_bytes", k_maximumResourceNameBytes);
        output += ',';
        appendLimit("resource_request_bytes", k_maximumResourceNameBytes);
        output += ',';
        appendLimit("resource_segment_bytes", k_maximumResourceSegmentBytes);
        output += ',';
        appendLimit("resource_segments", k_maximumResourceSegments);
        output += ',';
        appendLimit(
            "resource_total_bytes",
            PureDataProgram::k_maximumResourceClosureBytes
        );
        output += ',';
        appendLimit(
            "trusted_bridge_bytecode_bytes",
            PureDataProgram::k_maximumModuleBytecodeBytes
        );
        output += ',';
        appendLimit("value_depth", k_maximumValueDepth);
        output += ',';
        appendLimit("value_nodes", k_maximumValueNodes);
        output += ',';
        appendLimit("value_stack_slots", k_valueStackSlots);
        output += ',';
        appendLimit("value_text_bytes", k_maximumValueTextBytes);
        output += ',';
        appendLimit("vm_memory_bytes", PureDataProgram::k_memoryQuotaBytes);
        output += ',';
        appendLimit(
            "wall_time_milliseconds",
            std::chrono::duration_cast<std::chrono::milliseconds>(k_maximumRuntime).count()
        );

        output += "},\"luau_implementation\":";
        appendJsonString(output, k_luauImplementation);
        output += ",\"module_failure_contract\":";
        appendJsonString(output, k_moduleFailureContract);
        output += '}';
        return output;
    }

    auto pluginEnvironmentHash() -> Result<ContentHash>
    {
        auto const material = pluginEnvironmentMaterial();
        return sha256(std::as_bytes(std::span{material}));
    }
} // namespace uf::script
