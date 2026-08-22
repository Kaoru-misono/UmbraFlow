#include <script/scoped-tool-program.hpp>

#include "program-runtime.hpp"

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/text/json-text.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
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
        // The scoped Framework catalog, in JCS order because the environment
        // identity states it in this order. A name moved here moves that digest
        // and nothing else in the repository can move it back.
        constexpr auto k_scopedModules = std::array{
            std::string_view{"@umbraflow/audit"},
            std::string_view{"@umbraflow/screen"},
            std::string_view{"@umbraflow/tools"},
            std::string_view{"@umbraflow/workflow"},
        };

        // The single field of the private capability table, and the whole of the
        // native surface. Everything else -- screen wrappers, bounded waits,
        // delivery classification, result helpers, Tool discovery -- is Luau over
        // this one closure, so there is exactly one native primitive to reason
        // about and exactly one path an effect can take.
        constexpr auto k_capabilityInvokeField = std::string_view{"invoke"};
        constexpr auto k_toolInvokeArity       = 2;

        // Observable contracts. These move the scoped environment identity
        // whenever a script can distinguish the old runtime from the new one.
        constexpr auto k_toolArgumentContract = std::string_view{
            "tool_name_string_plus_one_decoded_json_value_v1"
        };
        constexpr auto k_toolResultContract = std::string_view{
            "one_frozen_decoded_json_value_no_userdata_no_metatable_v1"
        };
        constexpr auto k_toolFailureContract = std::string_view{
            "runtime_refusal_is_terminal_uncatchable_vm_teardown_v1"
        };
        constexpr auto k_capabilityTableContract = std::string_view{
            "single_invoke_closure_chunk_argument_to_catalog_modules_dropped_v1"
        };

        [[nodiscard]]
        auto validToolName(std::string_view value) -> bool
        {
            return detail::validSegmentedAsciiName(
                value,
                '.',
                ScopedToolProgram::k_maximumToolNameBytes,
                ScopedToolProgram::k_maximumToolNameSegments,
                ScopedToolProgram::k_maximumToolNameSegmentBytes
            );
        }

        // One scoped run: the fresh VM it executes in, and the run-scoped state
        // the single native primitive borrows.
        //
        // Lifetime contract: `m_vm` is declared LAST, so it is destroyed FIRST
        // and every member the invoke primitive reaches through its
        // light-userdata upvalue strictly outlives the VM that can reach them.
        // Member order, not discipline, is what enforces it. The borrowed
        // ToolRuntimeInvoke belongs to the ScopedToolProgram that started this
        // run and outlives the run by construction.
        class ScopedToolRun final
        {
            ToolRuntimeInvoke const& m_invokeTool;
            uint64          m_parentPosition;
            std::stop_token m_cancellation;
            uint64          m_childIndex{0};

            detail::QuotaBoundVm m_vm;

        public:
            ScopedToolRun(
                ToolRuntimeInvoke const& invokeTool,
                ScopedRunRequest const& request,
                MonotonicInstant::Duration runtimeCeiling
            )
                : m_invokeTool{invokeTool}
                , m_parentPosition{request.parentPosition}
                , m_cancellation{request.cancellation}
                , m_vm{runtimeCeiling, request.cancellation}
            {
            }

            [[nodiscard]]
            auto vm() noexcept UF_LIFETIME_BOUND -> detail::QuotaBoundVm&
            {
                return m_vm;
            }

            // Number this call under the issuing context and run it to
            // completion. The index is assigned here and nowhere else, so no
            // script can reach a position it was not given.
            [[nodiscard]]
            auto callTool(
                std::string_view toolName,
                json::Value const& arguments
            ) -> Result<json::Value>
            {
                if (m_childIndex == ScopedToolProgram::k_maximumToolCallsPerContext)
                {
                    return detail::refuse(
                        "one issuing context exceeded its fixed scoped Tool call ceiling"
                    );
                }
                ++m_childIndex;
                auto const coordinate = ToolCallCoordinate{
                    .parentPosition = m_parentPosition,
                    .childIndex     = m_childIndex,
                };
                return m_invokeTool(toolName, arguments, coordinate, m_cancellation);
            }
        };

        auto invokeToolPrimitive(lua_State* state) -> int
        {
            if (
                lua_gettop(state) != k_toolInvokeArity
                || lua_type(state, 1) != LUA_TSTRING
            )
            {
                luaL_error(
                    state,
                    "the Tool Runtime primitive takes a tool name and one argument value"
                );
            }

            // SAFETY: both upvalues are installed only by pushCapabilityTable.
            // The ScopedToolRun owns the VM as its last member, so it and the
            // ProgramEnvironment that runs inside that VM both outlive every
            // callback this VM can make.
            auto* p_run = static_cast<ScopedToolRun*>(
                lua_tolightuserdata(state, lua_upvalueindex(1))
            );
            auto* p_environment = static_cast<detail::ProgramEnvironment*>(
                lua_tolightuserdata(state, lua_upvalueindex(2))
            );
            std::size_t nameLength = 0;
            char const* p_name = lua_tolstring(state, 1, &nameLength);
            if (p_run == nullptr || p_environment == nullptr || p_name == nullptr)
            {
                luaL_error(state, "the Tool Runtime primitive has an invalid host context");
            }
            auto const toolName = std::string_view{p_name, nameLength};
            if (!validToolName(toolName))
            {
                luaL_error(state, "the Tool Runtime primitive rejected a non-canonical tool name");
            }

            auto refusalText = std::array<char, detail::k_maximumReaderErrorBytes>{};
            auto refused  = false;
            auto terminal = false;
            {
                // A malformed argument is the script's own error and stays
                // catchable, exactly as an entry point's malformed return value
                // does. A Tool Runtime refusal is not: it is a replay or budget
                // fact about the run, and a script that could catch it could
                // convert a mandated stop into ordinary control flow.
                auto argumentBudget = detail::ValueBudget{};
                auto arguments =
                    detail::readValue(state, *p_environment, 2, 0U, argumentBudget);
                if (!arguments.has_value())
                {
                    refusalText = detail::boundedText(arguments.error().message());
                    refused     = true;
                }
                else
                {
                    auto answered = p_run->callTool(toolName, *arguments);
                    if (!answered.has_value())
                    {
                        detail::recordTerminalFailure(*p_environment, answered.error());
                        terminal = true;
                    }
                    else
                    {
                        auto resultBudget = detail::ValueBudget{};
                        auto pushed = detail::pushValue(
                            state,
                            *p_environment,
                            *answered,
                            0U,
                            resultBudget
                        );
                        if (!pushed.has_value())
                        {
                            detail::recordTerminalFailure(*p_environment, pushed.error());
                            terminal = true;
                        }
                    }
                }
            }
            if (terminal)
            {
                return lua_break(state);
            }
            if (refused)
            {
                luaL_error(state, "%s", refusalText.data());
            }
            return 1;
        }

        auto pushCapabilityTable(
            lua_State* state,
            ScopedToolRun& run,
            detail::ProgramEnvironment& environment
        ) -> void
        {
            lua_createtable(state, 0, 1);
            lua_pushlightuserdata(state, &run);
            lua_pushlightuserdata(state, &environment);
            lua_pushcclosure(state, &invokeToolPrimitive, "tools.invoke", 2);
            auto const field = std::string{k_capabilityInvokeField};
            lua_rawsetfield(state, -2, field.c_str());
        }

        [[nodiscard]]
        auto capabilityInstaller(ScopedToolRun& run) -> detail::CapabilityInstaller
        {
            // A synchronous, non-escaping callback: the shared boot runs it once
            // and this installer dies with the invoke() frame that owns `run`.
            return [&run](
                       lua_State* state,
                       detail::ProgramEnvironment& environment
                   ) -> Status {
                pushCapabilityTable(state, run, environment);
                return ok();
            };
        }

        // A closure graph is admitted with a Tool Runtime that refuses every
        // call. Admission proves the graph loads and its resources materialize;
        // a Tool call issued while a module is still loading has no issuing
        // context to be numbered under, so it is a refusal rather than a call.
        [[nodiscard]]
        auto admissionToolRuntime() -> ToolRuntimeInvoke
        {
            return [](
                       std::string_view toolName,
                       json::Value const&,
                       ToolCallCoordinate const&,
                       std::stop_token
                   ) -> Result<json::Value> {
                return detail::refuse(
                    "a scoped tool program may not call a Tool while it is admitted: "
                    + std::string{toolName}
                );
            };
        }

        [[nodiscard]]
        auto validateScopedCatalog(
            std::span<FrameworkModule const> frameworkModules
        ) -> Status
        {
            for (auto const scopedName : k_scopedModules)
            {
                auto const found = std::ranges::find(
                    frameworkModules,
                    scopedName,
                    &FrameworkModule::name
                );
                if (found == frameworkModules.end())
                {
                    return detail::refuse(
                        "scoped tool program requires the Framework module "
                        + std::string{scopedName}
                    );
                }
                if (!found->projectVisible)
                {
                    return detail::refuse(
                        "scoped tool program requires a project-visible Framework module: "
                        + std::string{scopedName}
                    );
                }
            }
            return ok();
        }
    } // namespace

    class ScopedToolProgram::State final
    {
    public:
        detail::ProgramClosure closure{};
        ToolRuntimeInvoke      invokeTool{};
    };

    ScopedToolProgram::ScopedToolProgram(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ScopedToolProgram::scopedModuleNames() -> std::span<std::string_view const>
    {
        return k_scopedModules;
    }

    auto ScopedToolProgram::compile(
        std::string_view pluginId,
        std::string_view entryModule,
        std::vector<PureDataProgram::Module> modules,
        std::span<std::string_view const> entryPoints,
        std::vector<PureDataProgram::Resource> resources,
        std::span<FrameworkModule const> frameworkModules,
        std::vector<PureDataProgram::Resource> frameworkResources,
        ToolRuntimeInvoke invokeTool
    ) -> Result<ScopedToolProgram>
    {
        if (!invokeTool)
        {
            return detail::refuse("scoped tool program requires a Tool Runtime");
        }
        UF_TRY(validateScopedCatalog(frameworkModules));

        auto const spec = detail::ClosureSpec{
            .pluginId               = pluginId,
            .entryModule            = entryModule,
            .entryPoints            = entryPoints,
            .frameworkModules       = frameworkModules,
            .capabilityBoundModules = k_scopedModules,
        };
        UF_TRY_VALUE(
            closure,
            detail::compileClosure(
                spec,
                std::move(modules),
                std::move(resources),
                std::move(frameworkResources)
            )
        );

        auto const admissionRuntime = admissionToolRuntime();
        auto admissionRun = ScopedToolRun{
            admissionRuntime,
            ScopedRunRequest{},
            k_defaultMaxRuntime,
        };
        UF_TRY(
            detail::admitClosure(
                admissionRun.vm(),
                closure,
                capabilityInstaller(admissionRun)
            )
        );

        auto state = std::make_shared<State>(State{
            .closure    = std::move(closure),
            .invokeTool = std::move(invokeTool),
        });
        return ScopedToolProgram{std::shared_ptr<State const>{std::move(state)}};
    }

    auto ScopedToolProgram::invoke(
        std::string_view entryPoint,
        json::Value const& immutableInput,
        ScopedRunRequest const& request
    ) const -> Result<json::Value>
    {
        if (!std::ranges::binary_search(m_state->closure.entryPoints, entryPoint))
        {
            return detail::refuse("scoped tool entry point is not registered");
        }

        auto run = ScopedToolRun{m_state->invokeTool, request, k_defaultMaxRuntime};
        return detail::invokeClosure(
            run.vm(),
            m_state->closure,
            capabilityInstaller(run),
            entryPoint,
            immutableInput
        );
    }

    // Everything the pure environment attests to, continued in JCS order with
    // the members only this program type has: the scoped catalog, its own
    // ceilings, and the Tool Runtime facade contract. A digest over published
    // NAMES alone would leave a build that changed what invoke ANSWERS WITH
    // unmoved, which is the upgrade this pin exists to catch.
    auto scopedToolEnvironmentMaterial() -> std::string
    {
        auto output = detail::sharedEnvironmentMaterial(k_defaultMaxRuntime);

        output += ",\"scoped_limits\":{\"tool_calls_per_context\":";
        output += std::to_string(ScopedToolProgram::k_maximumToolCallsPerContext);
        output += ",\"tool_name_bytes\":";
        output += std::to_string(ScopedToolProgram::k_maximumToolNameBytes);
        output += ",\"tool_name_segment_bytes\":";
        output += std::to_string(ScopedToolProgram::k_maximumToolNameSegmentBytes);
        output += ",\"tool_name_segments\":";
        output += std::to_string(ScopedToolProgram::k_maximumToolNameSegments);

        output += "},\"scoped_modules\":[";
        auto separated = false;
        for (auto const name : k_scopedModules)
        {
            if (separated)
            {
                output += ',';
            }
            separated = true;
            appendJsonString(output, name);
        }

        output += "],\"tool_runtime\":{\"argument_shape\":";
        appendJsonString(output, k_toolArgumentContract);
        output += ",\"capability_field\":";
        appendJsonString(output, k_capabilityInvokeField);
        output += ",\"capability_table\":";
        appendJsonString(output, k_capabilityTableContract);
        output += ",\"failure_behaviour\":";
        appendJsonString(output, k_toolFailureContract);
        output += ",\"invoke_arity\":";
        output += std::to_string(k_toolInvokeArity);
        output += ",\"result_shape\":";
        appendJsonString(output, k_toolResultContract);
        output += "}}";
        return output;
    }

    auto scopedToolEnvironmentHash() -> Result<ContentHash>
    {
        auto const material = scopedToolEnvironmentMaterial();
        return sha256(std::as_bytes(std::span{material}));
    }
} // namespace uf::script
