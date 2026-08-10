#include <script/pure-data-program.hpp>

#include "allocator.hpp"
#include "cancellation.hpp"

#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <format>
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
        constexpr auto k_maximumDataBytes          = std::size_t{1024U} * 1024U;
        constexpr auto k_maximumArtifactCount      = std::size_t{64U};
        constexpr auto k_maximumArtifactBytes      = std::size_t{4U} * 1024U * 1024U;
        constexpr auto k_maximumTotalArtifactBytes = std::size_t{16U} * 1024U * 1024U;
        constexpr auto k_maximumErrorBytes         = std::size_t{4096U};
        constexpr auto k_memoryQuotaBytes          = std::size_t{16U} * 1024U * 1024U;
        constexpr auto k_interruptBudgetTicks      = uint64{2'000'000U};
        constexpr auto k_maximumRuntime            = std::chrono::seconds{2};

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

        constexpr auto k_bridgeSource = std::string_view{R"LUAU(
local safe_type = type
local safe_error = error
local safe_pairs = pairs
local safe_ipairs = ipairs
local safe_rawget = rawget
local safe_getmetatable = getmetatable
local safe_string_len = string.len
local safe_table_freeze = table.freeze

local canonical = {
    accept = function(value)
        if safe_type(value) ~= "string" then
            safe_error("pure data function must exchange canonical JSON bytes", 0)
        end
        if safe_string_len(value) == 0 or safe_string_len(value) > 1024 * 1024 then
            safe_error("canonical JSON bytes exceed the data boundary", 0)
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

        struct ArtifactReaderState final
        {
            std::span<PureDataProgram::Artifact const> artifacts{};
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

        auto readArtifact(lua_State* state) -> int
        {
            if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING)
            {
                luaL_error(state, "artifact.read requires exactly one root name");
            }

            std::size_t nameLength = 0;
            char const* p_name = lua_tolstring(state, 1, &nameLength);
            // SAFETY: upvalue 1 is installed only by pushArtifactReader and points
            // to the ArtifactReaderState owned by runFresh. That object outlives
            // every callback and the VM is closed before it leaves scope.
            auto const* reader = static_cast<ArtifactReaderState const*>(
                lua_tolightuserdata(state, lua_upvalueindex(1)));
            if (reader == nullptr || p_name == nullptr || nameLength > 256U)
            {
                luaL_error(state, "artifact.read rejected an invalid root name");
            }

            auto const name = std::string_view{p_name, nameLength};
            auto const found =
                std::ranges::find(reader->artifacts, name, &PureDataProgram::Artifact::name);
            if (found == reader->artifacts.end())
            {
                luaL_error(state, "artifact.read rejected an unknown root");
            }

            // Luau strings are immutable. This copies only the requested blob
            // into the quota-bound fresh VM; no artifact is eagerly materialized.
            lua_pushlstring(state, found->bytes.data(), found->bytes.size());
            return 1;
        }

        auto pushArtifactReader(lua_State* state, ArtifactReaderState* p_reader) -> void
        {
            lua_createtable(state, 0, 1);
            lua_pushlightuserdata(state, p_reader);
            lua_pushcclosure(state, &readArtifact, "artifact.read", 1);
            lua_rawsetfield(state, -2, "read");
            lua_setreadonly(state, -1, 1);
        }

        [[nodiscard]]
        auto pushPureEnvironment(lua_State* state, ArtifactReaderState* p_reader) -> Status
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

            pushArtifactReader(state, p_reader);
            lua_rawsetfield(state, environment, "artifact");

            // No metatable is attached. In particular there is no __index path
            // to main globals, the registry, a host table, or another environment.
            return ok();
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

        auto armRun(InterruptState& control) -> void
        {
            control.runStartedAt = std::chrono::steady_clock::now();
            control.deadline     = control.runStartedAt + k_maximumRuntime;
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
                        ArtifactReaderState* p_reader,
                        InterruptState& control) -> Status
        {
            UF_TRY(pushPureEnvironment(mainState, p_reader));
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
                          std::string_view input,
                          InterruptState& control) -> Result<std::string>
        {
            lua_State* thread = lua_newthread(mainState);
            lua_rawgetfield(mainState, bridge, "invoke");
            lua_xpush(mainState, thread, -1);
            lua_pop(mainState, 1);
            lua_xpush(mainState, thread, plugin);
            lua_pushlstring(thread, moduleId.data(), moduleId.size());
            pushEntryPoints(thread, entryPoints);
            lua_pushlstring(thread, entryPoint.data(), entryPoint.size());
            lua_pushlstring(thread, input.data(), input.size());
            UF_TRY(resume(thread, 5, control));
            if (lua_gettop(thread) != 1 || lua_isstring(thread, 1) == 0)
            {
                return refuse("pure data program returned a non-string value");
            }

            std::size_t length = 0;
            // SAFETY: the value was checked as a string and is copied before
            // the fresh VM is closed.
            char const* p_output = lua_tolstring(thread, 1, &length);
            if (length == 0U || length > k_maximumDataBytes)
            {
                return refuse("pure data output exceeds its fixed byte ceiling");
            }
            return std::string{p_output, length};
        }

        [[nodiscard]]
        auto runFresh(std::string_view bridgeBytecode,
                      std::string_view pluginBytecode,
                      std::string_view moduleId,
                      std::span<std::string const> entryPoints,
                      std::span<PureDataProgram::Artifact const> artifacts,
                      std::string_view entryPoint,
                      std::string_view input,
                      bool invoke) -> Result<std::string>
        {
            auto artifactReader = ArtifactReaderState{.artifacts = artifacts};
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
            armRun(vm.control);

            UF_TRY(loadModule(vm.state,
                              bridgeBytecode,
                              "pure-data-bridge",
                              &artifactReader,
                              vm.control));
            int const bridge = lua_gettop(vm.state);
            UF_TRY(loadModule(vm.state, pluginBytecode, moduleId, &artifactReader, vm.control));
            int const plugin = lua_gettop(vm.state);
            UF_TRY(inspectModule(vm.state, bridge, plugin, moduleId, entryPoints, vm.control));
            if (!invoke)
                return std::string{};
            return invokeModule(vm.state,
                                bridge,
                                plugin,
                                moduleId,
                                entryPoints,
                                entryPoint,
                                input,
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
                        {},
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

    auto PureDataProgram::invoke(std::string_view entryPoint, std::string_view immutableInput) const
        -> Result<std::string>
    {
        if (immutableInput.empty() || immutableInput.size() > k_maximumDataBytes ||
            !isValidUtf8(immutableInput))
        {
            return refuse("pure data input must be non-empty bounded UTF-8");
        }
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
} // namespace uf::script
