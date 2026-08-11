#include <script/pure-data-program.hpp>

#include "allocator.hpp"
#include "cancellation.hpp"

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
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
        constexpr auto k_maximumSourceBytes        = std::size_t{256U} * 1024U;
        constexpr auto k_maximumBytecodeBytes      = std::size_t{1024U} * 1024U;
        constexpr auto k_maximumArtifactCount      = std::size_t{64U};
        constexpr auto k_maximumArtifactBytes      = std::size_t{4U} * 1024U * 1024U;
        constexpr auto k_maximumTotalArtifactBytes = std::size_t{16U} * 1024U * 1024U;
        constexpr auto k_maximumErrorBytes         = std::size_t{4096U};
        constexpr auto k_memoryQuotaBytes          = std::size_t{16U} * 1024U * 1024U;
        constexpr auto k_interruptBudgetTicks      = uint64{2'000'000U};
        constexpr auto k_maximumRuntime            = std::chrono::seconds{2};

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

        constexpr auto k_pureGlobals = std::array{
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

        // The two frozen tables published beside the whitelist, spelled once so
        // that what pushPureEnvironment publishes and what
        // pluginEnvironmentHash attests to cannot drift apart.
        constexpr auto k_artifactTable    = std::string_view{"artifact"};
        constexpr auto k_artifactRead     = std::string_view{"read"};
        constexpr auto k_canonTable       = std::string_view{"canon"};
        constexpr auto k_canonEmptyObject = std::string_view{"emptyObject"};
        constexpr auto k_canonNull        = std::string_view{"null"};

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
            MemoryQuota quota{.limitBytes = k_memoryQuotaBytes};
            InterruptState control{
                .budgetTicks = k_interruptBudgetTicks,
            };
            lua_State* state{nullptr};
        };

        // Everything one fresh VM publishes that is not a copied Luau global.
        // The three references are registry pins created once per VM, so the
        // bridge environment and the plugin environment share one canon table
        // and one identity for each sentinel; two environments each minting
        // their own would make a sentinel the plugin returned unrecognizable to
        // the host.
        struct PureEnvironment final
        {
            std::span<PureDataProgram::Artifact const> artifacts{};
            int nullReference{LUA_NOREF};
            int emptyObjectReference{LUA_NOREF};
            int canonReference{LUA_NOREF};
        };

        // What a value conversion has spent so far. Passed as a mutable
        // reference because accumulating into it across a recursive walk is the
        // whole of what it is for.
        struct ValueBudget final
        {
            std::size_t nodes{0};
            std::size_t textBytes{0};
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
        auto spendNode(ValueBudget& budget, std::size_t textBytes) -> Status
        {
            budget.nodes += 1U;
            budget.textBytes += textBytes;
            if (budget.nodes > k_maximumValueNodes)
            {
                return refuse("pure data value exceeds its fixed node ceiling");
            }
            if (budget.textBytes > k_maximumValueTextBytes)
            {
                return refuse("pure data value exceeds its fixed byte ceiling");
            }
            return ok();
        }

        auto readArtifact(lua_State* state) -> int
        {
            if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)
            {
                luaL_error(state, "artifact.read requires exactly one root name");
            }

            std::size_t nameLength = 0;
            char const* p_name = lua_tolstring(state, 1, &nameLength);
            // SAFETY: upvalue 1 is installed only by pushArtifactReader and points
            // to the PureEnvironment owned by runFresh. That object outlives
            // every callback and the VM is closed before it leaves scope.
            auto const* p_environment = static_cast<PureEnvironment const*>(
                lua_tolightuserdata(state, lua_upvalueindex(1)));
            if (p_environment == nullptr || p_name == nullptr || nameLength > 256U)
            {
                luaL_error(state, "artifact.read rejected an invalid root name");
            }

            auto const name = std::string_view{p_name, nameLength};
            auto const found = std::ranges::find(p_environment->artifacts,
                                                 name,
                                                 &PureDataProgram::Artifact::name);
            if (found == p_environment->artifacts.end())
            {
                luaL_error(state, "artifact.read rejected an unknown root");
            }

            // Luau strings are immutable. This copies only the requested blob
            // into the quota-bound fresh VM; no artifact is eagerly materialized.
            lua_pushlstring(state, found->bytes.data(), found->bytes.size());
            return 1;
        }

        auto pushArtifactReader(lua_State* state, PureEnvironment* p_environment) -> void
        {
            lua_createtable(state, 0, 1);
            lua_pushlightuserdata(state, p_environment);
            lua_pushcclosure(state, &readArtifact, "artifact.read", 1);
            rawSetField(state, -2, k_artifactRead);
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

        [[nodiscard]]
        auto pushPureEnvironment(lua_State* state, PureEnvironment* p_environment) -> Status
        {
            lua_newtable(state);
            int const environment = lua_gettop(state);
            for (auto const name : k_pureGlobals)
            {
                auto const key = std::string{name};
                lua_rawgetfield(state, LUA_GLOBALSINDEX, key.c_str());
                if (lua_isnil(state, -1))
                {
                    lua_pop(state, 2);
                    return fail(AutomationErrorKind::InternalInvariant,
                                "pure data whitelist names an absent Luau global: " + key);
                }
                lua_rawsetfield(state, environment, key.c_str());
            }

            pushArtifactReader(state, p_environment);
            rawSetField(state, environment, k_artifactTable);

            lua_getref(state, p_environment->canonReference);
            rawSetField(state, environment, k_canonTable);

            // No metatable is attached. In particular there is no __index path
            // to main globals, the registry, a host table, or another environment.
            return ok();
        }

        [[nodiscard]]
        auto pushValue(lua_State* state,
                       PureEnvironment const& environment,
                       json::Value const& value,
                       std::size_t depth,
                       ValueBudget& budget) -> Status
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
        auto readValue(lua_State* state,
                       PureEnvironment const& environment,
                       int index,
                       std::size_t depth,
                       ValueBudget& budget) -> Result<json::Value>;

        [[nodiscard]]
        auto readTable(lua_State* state,
                       PureEnvironment const& environment,
                       int index,
                       std::size_t depth,
                       ValueBudget& budget) -> Result<json::Value>
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
                    items.push_back(*std::move(item));
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

        auto readValue(lua_State* state,
                       PureEnvironment const& environment,
                       int index,
                       std::size_t depth,
                       ValueBudget& budget) -> Result<json::Value>
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
            options.optimizationLevel = 1;
            options.debugLevel        = 0;

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

            if (bytecodeSize == 0U || bytecodeSize > k_maximumBytecodeBytes)
            {
                return refuse("compiled pure data bytecode exceeds its fixed ceiling");
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
        auto resume(lua_State* thread, int argumentCount, InterruptState& control) -> Status
        {
            int const status = lua_resume(thread, nullptr, argumentCount);
            if (status == LUA_BREAK || control.broken())
            {
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
            return ok();
        }

        [[nodiscard]]
        auto loadModule(lua_State* mainState,
                        std::string_view bytecode,
                        std::string_view label,
                        PureEnvironment* p_environment,
                        InterruptState& control) -> Status
        {
            UF_TRY(pushPureEnvironment(mainState, p_environment));
            int const environment = lua_gettop(mainState);
            lua_State* thread = lua_newthread(mainState);

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
            UF_TRY(resume(thread, 0, control));
            if (lua_gettop(thread) != 1 || !lua_istable(thread, 1))
            {
                return refuse("pure data module must return exactly one table");
            }

            lua_xpush(thread, mainState, 1);
            lua_remove(mainState, environment + 1);
            lua_remove(mainState, environment);
            return ok();
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
        auto inspectModule(lua_State* mainState,
                           int bridge,
                           int plugin,
                           std::string_view moduleId,
                           std::span<std::string const> entryPoints,
                           InterruptState& control) -> Status
        {
            lua_State* thread = lua_newthread(mainState);
            lua_rawgetfield(mainState, bridge, "inspect");
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            lua_xpush(mainState, thread, plugin);
            lua_pushlstring(thread, moduleId.data(), moduleId.size());
            pushEntryPoints(thread, entryPoints);
            UF_TRY(resume(thread, 3, control));
            lua_pop(mainState, 1);
            return ok();
        }

        [[nodiscard]]
        auto invokeModule(lua_State* mainState,
                          int bridge,
                          int plugin,
                          std::string_view moduleId,
                          std::span<std::string const> entryPoints,
                          std::string_view entryPoint,
                          json::Value const& input,
                          PureEnvironment const& environment,
                          InterruptState& control) -> Result<json::Value>
        {
            lua_State* thread = lua_newthread(mainState);
            lua_rawgetfield(mainState, bridge, "invoke");
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            lua_xpush(mainState, thread, plugin);
            lua_pushlstring(thread, moduleId.data(), moduleId.size());
            pushEntryPoints(thread, entryPoints);
            lua_pushlstring(thread, entryPoint.data(), entryPoint.size());

            auto inputBudget = ValueBudget{};
            UF_TRY_CONTEXT(pushValue(thread, environment, input, 0U, inputBudget),
                           "pushing the decoded ProjectPlugin input");
            UF_TRY(resume(thread, 5, control));
            if (lua_gettop(thread) != 1)
            {
                return refuse("pure data program returned other than one value");
            }

            auto outputBudget = ValueBudget{};
            return readValue(thread, environment, 1, 0U, outputBudget);
        }

        [[nodiscard]]
        auto runFresh(std::string_view bridgeBytecode,
                      std::string_view pluginBytecode,
                      std::string_view moduleId,
                      std::span<std::string const> entryPoints,
                      std::span<PureDataProgram::Artifact const> artifacts,
                      std::string_view entryPoint,
                      json::Value const& input,
                      bool invoke) -> Result<json::Value>
        {
            auto environment = PureEnvironment{.artifacts = artifacts};
            auto vm  = VmRun{};
            vm.state = createStateWithQuota(&vm.quota);
            if (vm.state == nullptr)
            {
                return fail(AutomationErrorKind::InternalInvariant,
                            "pure data VM allocation failed");
            }
            auto stateGuard = scopeExit([&vm]() noexcept { lua_close(vm.state); });

            luaL_openlibs(vm.state);
            nilLibraryField(vm.state, "math", "random");
            nilLibraryField(vm.state, "math", "randomseed");
            nilLibraryField(vm.state, "string", "dump");
            installInterrupt(vm.state, &vm.control);
            luaL_sandbox(vm.state);
            UF_TRY(createFrozenSentinels(vm.state, environment));
            vm.control.beginUnitOfScript(k_maximumRuntime);

            UF_TRY(loadModule(vm.state,
                              bridgeBytecode,
                              "pure-data-bridge",
                              &environment,
                              vm.control));
            int const bridge = lua_gettop(vm.state);
            UF_TRY(loadModule(vm.state, pluginBytecode, moduleId, &environment, vm.control));
            int const plugin = lua_gettop(vm.state);
            UF_TRY(inspectModule(vm.state, bridge, plugin, moduleId, entryPoints, vm.control));
            if (!invoke)
                return json::Value{};
            return invokeModule(vm.state,
                                bridge,
                                plugin,
                                moduleId,
                                entryPoints,
                                entryPoint,
                                input,
                                environment,
                                vm.control);
        }

        [[nodiscard]]
        auto validIdentifier(std::string_view value) -> bool
        {
            if (value.empty() || value.size() > 256U || !isValidUtf8(value))
            {
                return false;
            }
            return std::ranges::none_of(value, [](char character) {
                return static_cast<unsigned char>(character) < 0x20U;
            });
        }

        // The exact bytes the environment's identity is taken over, as one
        // canonical JSON object: the bridge that wraps every call, the frozen
        // tables published beside the whitelist, and the whitelist itself.
        [[nodiscard]]
        auto pluginEnvironmentMaterial() -> std::string
        {
            auto output = std::string{"{\"bridge_source\":"};
            appendJsonString(output, k_bridgeSource);
            output += ",\"frozen_tables\":{";
            appendJsonString(output, k_artifactTable);
            output += ":[";
            appendJsonString(output, k_artifactRead);
            output += "],";
            appendJsonString(output, k_canonTable);
            output += ":[";
            appendJsonString(output, k_canonEmptyObject);
            output += ',';
            appendJsonString(output, k_canonNull);
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
            output += "]}";
            return output;
        }
    } // namespace

    class PureDataProgram::State final
    {
    public:
        std::string              moduleId{};
        std::vector<std::string> entryPoints{};
        std::vector<Artifact>    artifacts{};
        std::string              bridgeBytecode{};
        std::string              pluginBytecode{};
    };

    PureDataProgram::PureDataProgram(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto PureDataProgram::compile(std::string_view moduleId,
                                  std::string_view source,
                                  std::span<std::string_view const> entryPoints,
                                  std::vector<Artifact> artifacts) -> Result<PureDataProgram>
    {
        if (!validIdentifier(moduleId))
        {
            return refuse("pure data module id is invalid");
        }
        if (source.empty() || source.size() > k_maximumSourceBytes || !isValidUtf8(source))
        {
            return refuse("pure data source must be non-empty bounded UTF-8");
        }
        if (entryPoints.empty() || entryPoints.size() > 32U)
        {
            return refuse("pure data module requires a bounded entry-point set");
        }
        if (artifacts.size() > k_maximumArtifactCount)
        {
            return refuse("pure data artifact count exceeds its fixed ceiling");
        }

        auto totalArtifactBytes = std::size_t{0};
        for (auto const& artifact : artifacts)
        {
            if (!validIdentifier(artifact.name))
            {
                return refuse("pure data artifact root name is invalid");
            }
            if (artifact.bytes.size() > k_maximumArtifactBytes)
            {
                return refuse("pure data artifact exceeds its fixed byte ceiling");
            }
            if (totalArtifactBytes > k_maximumTotalArtifactBytes - artifact.bytes.size())
            {
                return refuse("pure data artifacts exceed their total byte ceiling");
            }
            totalArtifactBytes += artifact.bytes.size();
        }
        std::ranges::sort(artifacts, {}, &Artifact::name);
        if (std::ranges::adjacent_find(artifacts, {}, &Artifact::name) != artifacts.end())
        {
            return refuse("pure data artifact root names must be unique");
        }

        auto ownedEntryPoints = std::vector<std::string>{};
        ownedEntryPoints.reserve(entryPoints.size());
        for (auto const entryPoint : entryPoints)
        {
            if (!validIdentifier(entryPoint) || entryPoint.size() > 64U)
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
        UF_TRY_VALUE(pluginBytecode, compileBytecode(source, moduleId));
        UF_TRY(runFresh(bridgeBytecode,
                        pluginBytecode,
                        moduleId,
                        ownedEntryPoints,
                        artifacts,
                        {},
                        json::Value{},
                        false));

        auto state = std::make_shared<State>(State{
            .moduleId       = std::string{moduleId},
            .entryPoints    = std::move(ownedEntryPoints),
            .artifacts      = std::move(artifacts),
            .bridgeBytecode = std::move(bridgeBytecode),
            .pluginBytecode = std::move(pluginBytecode),
        });
        return PureDataProgram{std::shared_ptr<State const>{std::move(state)}};
    }

    auto PureDataProgram::invoke(std::string_view entryPoint,
                                 json::Value const& immutableInput) const -> Result<json::Value>
    {
        if (!std::ranges::binary_search(m_state->entryPoints, entryPoint))
        {
            return refuse("pure data entry point is not registered");
        }
        return runFresh(m_state->bridgeBytecode,
                        m_state->pluginBytecode,
                        m_state->moduleId,
                        m_state->entryPoints,
                        m_state->artifacts,
                        entryPoint,
                        immutableInput,
                        true);
    }

    auto pureEnvironmentGlobals() -> std::span<std::string_view const>
    {
        return k_pureGlobals;
    }

    auto pluginEnvironmentHash() -> Result<ContentHash>
    {
        auto const material = pluginEnvironmentMaterial();
        return sha256(std::as_bytes(std::span{material}));
    }
} // namespace uf::script
