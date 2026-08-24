#include <script/scoped-tool-program.hpp>

#include "program-runtime.hpp"

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/text/json-text.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <limits>
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
        constexpr auto k_toolInvokeArity       = 3;

        // Observable contracts. These move the scoped environment identity
        // whenever a script can distinguish the old runtime from the new one.
        constexpr auto k_toolArgumentContract = std::string_view{
            "tool_name_string_plus_one_decoded_json_value_plus_optional_structured_body_v2"
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
        constexpr auto k_toolBodyContract = std::string_view{
            "synchronous_framework_owned_child_dispatch_no_yield_no_suspension_no_unstructured_callback_v1"
        };
        constexpr auto k_runtimeCeilingSource = std::string_view{
            "outer_project_tool_registration.timeout.maximum_elapsed_ms"
        };
        constexpr auto k_bodyYieldRefusal = std::string_view{
            "a Tool body may not yield or suspend a coroutine; only framework "
            "structured child dispatch may re-enter the VM"
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
        // ToolRuntimeDispatch belongs to the ScopedToolProgram that started this
        // run and outlives the run by construction.
        class ScopedToolRun final
        {
            ToolRuntimeDispatch const& m_dispatchTool;

            ContentHash     m_parentPosition;
            ContentHash     m_budgetPosition;
            std::string     m_budgetOwner;
            uint64          m_maximumElapsedMillis;
            std::stop_token m_cancellation;
            uint64          m_childIndex{0};

            detail::QuotaBoundVm m_vm;

        public:
            ScopedToolRun(
                ToolRuntimeDispatch const& dispatchTool,
                ScopedRunRequest const& request,
                MonotonicInstant::Duration runtimeCeiling
            )
                : m_dispatchTool{dispatchTool}
                , m_parentPosition{request.parentPosition}
                , m_budgetPosition{request.parentPosition}
                , m_budgetOwner{request.budgetOwner}
                , m_maximumElapsedMillis{request.maximumElapsedMillis}
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
                json::Value const& arguments,
                ToolCallBody body
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
                return m_dispatchTool(
                    toolName,
                    arguments,
                    coordinate,
                    m_cancellation,
                    std::move(body)
                );
            }

            [[nodiscard]] auto budgetOwner() const -> std::string_view
            {
                return m_budgetOwner;
            }

            [[nodiscard]] auto budgetPosition() const -> ContentHash const&
            {
                return m_budgetPosition;
            }

            [[nodiscard]] auto maximumElapsedMillis() const -> uint64
            {
                return m_maximumElapsedMillis;
            }

            // Structured re-entry changes the VM adapter's issuing context to
            // the body-taking call. The same VM, allocator, deadline and host
            // stack remain in force; only ledger parentage and its per-context
            // ordinal change, and both are restored for nested bodies.
            auto runBodyAt(ContentHash const& owningCall, ToolCallBody body) -> Status
            {
                auto outerParent = std::exchange(m_parentPosition, owningCall);
                auto outerIndex  = std::exchange(m_childIndex, uint64{0});
                auto const restore = scopeExit(
                    [this, outerParent = std::move(outerParent), outerIndex]() mutable
                        noexcept
                    {
                        m_parentPosition = std::move(outerParent);
                        m_childIndex     = outerIndex;
                    }
                );
                return body(owningCall);
            }
        };

        [[nodiscard]]
        auto bodyFailure(
            ScopedToolRun& run,
            int status,
            std::string_view raised
        ) -> Status
        {
            if (status == LUA_ERRMEM)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "a nested Tool body exhausted the registration-pinned "
                        "Luau memory ceiling of {} bytes owned by Project Tool "
                        "{} at {}",
                        PureDataProgram::k_memoryQuotaBytes,
                        run.budgetOwner(),
                        run.budgetPosition().hex()
                    )
                );
            }
            auto const& control = run.vm().control();
            if (control.broken())
            {
                switch (control.cause)
                {
                case InterruptState::BreakCause::Deadline:
                    return fail(
                        AutomationErrorKind::Cancelled,
                        std::format(
                            "a nested Tool body exceeded registration-declared "
                            "maximum_elapsed_ms={} owned by Project Tool {} at {}",
                            run.maximumElapsedMillis(),
                            run.budgetOwner(),
                            run.budgetPosition().hex()
                        )
                    );
                case InterruptState::BreakCause::InstructionBudget:
                    return fail(
                        AutomationErrorKind::Cancelled,
                        std::format(
                            "a nested Tool body exhausted the registration-pinned "
                            "instruction budget owned by Project Tool {} at {} [{}]",
                            run.budgetOwner(),
                            run.budgetPosition().hex(),
                            describeBreak(control)
                        )
                    );
                case InterruptState::BreakCause::StopToken:
                    return fail(
                        AutomationErrorKind::Cancelled,
                        "a nested Tool body was cancelled [" + describeBreak(control)
                            + "]"
                    );
                case InterruptState::BreakCause::None: break;
                }
            }
            if (status == LUA_YIELD || raised.contains("yield"))
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    std::string{k_bodyYieldRefusal}
                );
            }
            return fail(
                AutomationErrorKind::ActionRejected,
                raised.empty() ? "a Tool body failed"
                               : "a Tool body failed: " + std::string{raised}
            );
        }

        auto invokeToolPrimitive(lua_State* state) -> int
        {
            if (
                lua_gettop(state) != k_toolInvokeArity
                || lua_type(state, 1) != LUA_TSTRING
                || (!lua_isnil(state, 3) && lua_type(state, 3) != LUA_TFUNCTION)
            )
            {
                luaL_error(
                    state,
                    "the Tool Runtime primitive takes a tool name, one argument "
                    "value and an optional structured body"
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
                    auto body = ToolCallBody{};
                    if (lua_type(state, 3) == LUA_TFUNCTION)
                    {
                        auto const bodyIndex = lua_absindex(state, 3);
                        body = [state, bodyIndex, p_run](
                                   ContentHash const& owningCall
                               ) -> Status
                        {
                            auto runScriptBody = ToolCallBody{
                                [state, bodyIndex, p_run](ContentHash const&) -> Status
                                {
                                    lua_pushvalue(state, bodyIndex);
                                    auto const status = lua_pcall(state, 0, 0, 0);
                                    if (status == LUA_OK)
                                    {
                                        return ok();
                                    }
                                    auto raised = std::string{};
                                    if (lua_type(state, -1) == LUA_TSTRING)
                                    {
                                        std::size_t size{};
                                        char const* const text =
                                            lua_tolstring(state, -1, &size);
                                        if (text != nullptr)
                                        {
                                            raised.assign(text, size);
                                        }
                                    }
                                    lua_pop(state, 1);
                                    return bodyFailure(*p_run, status, raised);
                                }
                            };
                            return p_run->runBodyAt(
                                owningCall,
                                std::move(runScriptBody)
                            );
                        };
                    }
                    auto answered =
                        p_run->callTool(toolName, *arguments, std::move(body));
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

        // The capability table a closure graph is ADMITTED under.
        //
        // Admission proves the graph loads and its resources materialize. It is
        // not a run: no durable call is dispatching, so there is no position to
        // number a call under and no issuing context to number it in. That is
        // why this is a second primitive rather than a Tool Runtime that says
        // no. A call here cannot be refused on a run's behalf because there is
        // no run, and a request naming a placeholder position would be exactly
        // the "absent parent" reading ScopedRunRequest is shaped to forbid.
        //
        // The refusal is terminal for the same reason every Tool Runtime
        // refusal is: it is a fact about the program rather than a value, so no
        // pcall in a loading module can convert it into control flow.
        auto admissionInvokePrimitive(lua_State* state) -> int
        {
            auto requested = std::string{"an unnamed tool"};
            if (lua_type(state, 1) == LUA_TSTRING)
            {
                std::size_t nameLength = 0;
                char const* p_name     = lua_tolstring(state, 1, &nameLength);
                if (p_name != nullptr)
                {
                    requested = std::string{p_name, nameLength};
                }
            }

            // SAFETY: the single upvalue is installed only by
            // pushAdmissionCapabilityTable, and the ProgramEnvironment it names
            // belongs to the admitClosure frame that owns the VM making this
            // call, so it strictly outlives every callback the VM can make.
            auto* p_environment = static_cast<detail::ProgramEnvironment*>(
                lua_tolightuserdata(state, lua_upvalueindex(1))
            );
            if (p_environment == nullptr)
            {
                luaL_error(
                    state,
                    "the Tool Runtime primitive has an invalid host context"
                );
            }
            auto const refusal = detail::refuse(
                "a scoped tool program may not call a Tool while it is admitted: "
                + requested
            );
            detail::recordTerminalFailure(*p_environment, refusal.error());
            return lua_break(state);
        }

        auto pushAdmissionCapabilityTable(
            lua_State* state,
            detail::ProgramEnvironment& environment
        ) -> void
        {
            lua_createtable(state, 0, 1);
            lua_pushlightuserdata(state, &environment);
            lua_pushcclosure(state, &admissionInvokePrimitive, "tools.invoke", 1);
            auto const field = std::string{k_capabilityInvokeField};
            lua_rawsetfield(state, -2, field.c_str());
        }

        [[nodiscard]]
        auto admissionCapabilityInstaller() -> detail::CapabilityInstaller
        {
            // Synchronous and non-escaping, and it closes over nothing at all:
            // an admission has no run state to close over.
            return [](
                       lua_State* state,
                       detail::ProgramEnvironment& environment
                   ) -> Status {
                pushAdmissionCapabilityTable(state, environment);
                return ok();
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
        ToolRuntimeDispatch    dispatchTool{};
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
        ToolRuntimeDispatch dispatchTool
    ) -> Result<ScopedToolProgram>
    {
        if (!dispatchTool)
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

        auto admissionVm = detail::QuotaBoundVm{
            k_defaultMaxRuntime,
            std::stop_token{},
        };
        UF_TRY(
            detail::admitClosure(
                admissionVm,
                closure,
                admissionCapabilityInstaller()
            )
        );

        auto state = std::make_shared<State>(State{
            .closure      = std::move(closure),
            .dispatchTool = std::move(dispatchTool),
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

        if (request.budgetOwner.empty())
        {
            return detail::refuse("a scoped run must name its outer budget owner");
        }
        if (
            request.maximumElapsedMillis == 0U
            || request.maximumElapsedMillis
                > static_cast<uint64>(std::numeric_limits<int64>::max())
        )
        {
            return detail::refuse(
                "a scoped run must carry a positive representable "
                "registration-declared maximum_elapsed_ms"
            );
        }

        auto const runtimeCeiling = std::chrono::milliseconds{
            static_cast<int64>(request.maximumElapsedMillis)
        };
        auto run = ScopedToolRun{m_state->dispatchTool, request, runtimeCeiling};
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
        auto output = detail::sharedEnvironmentMaterial(
            MonotonicInstant::Duration{},
            k_runtimeCeilingSource
        );

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
        output += ",\"body\":";
        appendJsonString(output, k_toolBodyContract);
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
