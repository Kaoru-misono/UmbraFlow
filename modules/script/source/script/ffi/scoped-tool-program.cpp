#include <script/scoped-tool-program.hpp>

#include "program-runtime.hpp"

#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
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
        constexpr auto k_toolInvokeArity       = 2;

        // Observable contracts. These move the scoped environment identity
        // whenever a script can distinguish the old runtime from the new one.
        constexpr auto k_toolArgumentContract = std::string_view{
            "tool_name_string_plus_one_decoded_json_value_v3"
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
        constexpr auto k_runtimeCeilingSource = std::string_view{
            "outer_project_tool_registration.timeout.maximum_elapsed_ms"
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

        // The run context the ONE VM primitive borrows. Registered handlers
        // refuse every Tool name; interactive chunks reach their root caller.
        class ScopedVmRun
        {
        public:
            ScopedVmRun() = default;
            ScopedVmRun(ScopedVmRun const&) = delete;
            ScopedVmRun(ScopedVmRun&&) = delete;
            auto operator=(ScopedVmRun const&) -> ScopedVmRun& = delete;
            auto operator=(ScopedVmRun&&) -> ScopedVmRun& = delete;
            virtual ~ScopedVmRun() = default;

            [[nodiscard]]
            virtual auto vm() noexcept UF_LIFETIME_BOUND
                -> detail::QuotaBoundVm& = 0;

            [[nodiscard]]
            virtual auto callTool(
                std::string_view toolName,
                json::Value const& arguments
            ) -> Result<json::Value> = 0;
        };

        // One registered Project Tool leaf run. The VM is declared last so it
        // dies before every object its native closure can reach.
        class ScopedToolRun final : public ScopedVmRun
        {
            ContentHash m_budgetPosition;
            std::string m_budgetOwner;
            uint64      m_maximumElapsedMillis;

            detail::QuotaBoundVm m_vm;

        public:
            ScopedToolRun(
                ScopedRunRequest const& request,
                MonotonicInstant::Duration runtimeCeiling
            )
                : m_budgetPosition{request.callIdentity}
                , m_budgetOwner{request.budgetOwner}
                , m_maximumElapsedMillis{request.maximumElapsedMillis}
                , m_vm{runtimeCeiling, request.cancellation}
            {
            }

            [[nodiscard]]
            auto vm() noexcept UF_LIFETIME_BOUND
                -> detail::QuotaBoundVm& override
            {
                return m_vm;
            }

            [[nodiscard]]
            auto callTool(
                std::string_view toolName,
                json::Value const&
            ) -> Result<json::Value> override
            {
                return detail::refuse(
                    "Project Tool handler " + m_budgetOwner
                    + " may not issue Tool call " + std::string{toolName}
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

        };

        // One interactive chunk. The ToolRuntimeInvoke already represents the
        // session's root issuing door, so it assigns no synthetic coordinate.
        // It still counts calls in that issuing context.
        class ScopedSessionRun final : public ScopedVmRun
        {
            ToolRuntimeInvoke& m_invokeTool;
            std::string m_chunkName;
            int64       m_maximumElapsedMillis;
            std::size_t m_memoryQuotaBytes;
            uint64      m_interruptBudgetTicks;
            uint64      m_callIndex{};

            detail::QuotaBoundVm m_vm;

        public:
            ScopedSessionRun(
                ToolRuntimeInvoke& invokeTool,
                std::string_view chunkName,
                MonotonicInstant::Duration maximumRuntime,
                std::stop_token cancellation,
                std::size_t memoryQuotaBytes,
                uint64 interruptBudgetTicks
            )
                : m_invokeTool{invokeTool}
                , m_chunkName{chunkName}
                , m_maximumElapsedMillis{
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          maximumRuntime
                      )
                          .count()
                  }
                , m_memoryQuotaBytes{memoryQuotaBytes}
                , m_interruptBudgetTicks{interruptBudgetTicks}
                , m_vm{
                      maximumRuntime,
                      std::move(cancellation),
                      memoryQuotaBytes,
                      interruptBudgetTicks,
                  }
            {
            }

            [[nodiscard]]
            auto vm() noexcept UF_LIFETIME_BOUND
                -> detail::QuotaBoundVm& override
            {
                return m_vm;
            }

            [[nodiscard]] auto executionLimitFailure() -> Status
            {
                auto const& control = m_vm.control();
                switch (control.cause)
                {
                case InterruptState::BreakCause::Deadline:
                    return fail(
                        AutomationErrorKind::Cancelled,
                        std::format(
                            "interactive chunk {} exceeded its session-supplied "
                            "maximum_elapsed_ms={}",
                            m_chunkName,
                            m_maximumElapsedMillis
                        )
                    );
                case InterruptState::BreakCause::InstructionBudget:
                    return fail(
                        AutomationErrorKind::Cancelled,
                        std::format(
                            "interactive chunk {} exhausted its Framework-pinned "
                            "instruction budget of {} ticks [{}]",
                            m_chunkName,
                            m_interruptBudgetTicks,
                            describeBreak(control)
                        )
                    );
                case InterruptState::BreakCause::StopToken:
                    return fail(
                        AutomationErrorKind::Cancelled,
                        "interactive chunk " + m_chunkName + " was cancelled ["
                            + describeBreak(control) + "]"
                    );
                case InterruptState::BreakCause::None:
                    return fail(
                        AutomationErrorKind::InternalInvariant,
                        "an unbroken interactive VM was asked to report an execution limit"
                    );
                }
                UF_UNREACHABLE_MSG("Unknown interrupt break cause");
            }

            [[nodiscard]]
            auto callTool(
                std::string_view toolName,
                json::Value const& arguments
            ) -> Result<json::Value> override
            {
                if (m_callIndex == ScopedToolProgram::k_maximumInteractiveToolCalls)
                {
                    return detail::refuse(
                        "one issuing context exceeded its fixed scoped Tool call ceiling"
                    );
                }
                ++m_callIndex;
                return m_invokeTool(
                    toolName,
                    arguments
                );
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
                    "the Tool Runtime primitive takes a tool name and one "
                    "argument value"
                );
            }

            // SAFETY: both upvalues are installed only by pushCapabilityTable.
            // The ScopedVmRun owns the VM as its last member, so it and the
            // ProgramEnvironment that runs inside that VM both outlive every
            // callback this VM can make.
            auto* p_run = static_cast<ScopedVmRun*>(
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
            ScopedVmRun& run,
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
        auto capabilityInstaller(ScopedVmRun& run) -> detail::CapabilityInstaller
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

        constexpr auto k_sessionPluginId   = std::string_view{"framework.interactive"};
        constexpr auto k_sessionModuleName = std::string_view{"chunk"};
        constexpr auto k_sessionEntryPoint = std::string_view{"evaluate"};
        constexpr auto k_sessionEntryPoints = std::array{k_sessionEntryPoint};

        [[nodiscard]]
        auto sessionModuleSource(std::string_view source) -> std::string
        {
            auto wrapped = std::string{};
            constexpr auto k_prefix = std::string_view{
                "return table.freeze({\n"
                "    plugin_id = \"framework.interactive\",\n"
                "    evaluate = function(_)\n"
            };
            constexpr auto k_suffix = std::string_view{"\n    end,\n})\n"};
            wrapped.reserve(k_prefix.size() + source.size() + k_suffix.size());
            wrapped += k_prefix;
            wrapped += source;
            wrapped += k_suffix;
            return wrapped;
        }

        [[nodiscard]]
        auto compileSessionClosure(
            std::string_view source,
            std::span<FrameworkModule const> frameworkModules,
            std::vector<PureDataProgram::Resource> frameworkResources
        ) -> Result<detail::ProgramClosure>
        {
            auto modules = std::vector<PureDataProgram::Module>{};
            modules.emplace_back(PureDataProgram::Module{
                .name   = std::string{k_sessionModuleName},
                .source = sessionModuleSource(source),
            });
            return detail::compileClosure(
                detail::ClosureSpec{
                    .pluginId               = k_sessionPluginId,
                    .entryModule            = k_sessionModuleName,
                    .entryPoints            = k_sessionEntryPoints,
                    .frameworkModules       = frameworkModules,
                    .capabilityBoundModules = k_scopedModules,
                },
                std::move(modules),
                {},
                std::move(frameworkResources)
            );
        }
    } // namespace

    class ScopedToolProgram::State final
    {
    public:
        detail::ProgramClosure closure{};
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
        std::vector<PureDataProgram::Resource> frameworkResources
    ) -> Result<ScopedToolProgram>
    {
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
            .closure = std::move(closure),
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
        auto run = ScopedToolRun{request, runtimeCeiling};
        return detail::invokeClosure(
            run.vm(),
            m_state->closure,
            capabilityInstaller(run),
            entryPoint,
            immutableInput
        );
    }

    ScopedToolSession::ScopedToolSession(
        std::vector<FrameworkModule> frameworkModules,
        std::vector<PureDataProgram::Resource> frameworkResources,
        ToolRuntimeInvoke invokeTool,
        std::stop_token cancellation,
        MonotonicInstant::Duration maximumRuntime,
        std::size_t memoryQuotaBytes
    ) noexcept
        : m_frameworkModules{std::move(frameworkModules)}
        , m_frameworkResources{std::move(frameworkResources)}
        , m_invokeTool{std::move(invokeTool)}
        , m_cancellation{std::move(cancellation)}
        , m_maximumRuntime{maximumRuntime}
        , m_memoryQuotaBytes{memoryQuotaBytes}
    {
    }

    auto ScopedToolSession::create(
        std::vector<FrameworkModule> frameworkModules,
        std::vector<PureDataProgram::Resource> frameworkResources,
        ToolRuntimeInvoke invokeTool,
        std::stop_token cancellation,
        MonotonicInstant::Duration maximumRuntime,
        uint64 memoryQuotaBytes
    ) -> Result<ScopedToolSession>
    {
        if (!invokeTool)
        {
            return detail::refuse("a scoped Tool session requires a Tool Runtime");
        }
        UF_TRY(validateScopedCatalog(frameworkModules));
        auto const convertedMemoryQuota = checkedCast<std::size_t>(memoryQuotaBytes);
        if (!convertedMemoryQuota.has_value())
        {
            return detail::refuse(
                "a scoped Tool session's memory ceiling is not representable"
            );
        }

        // Validate and boot the exact Framework closure and catalog before the
        // caller receives a session. The probe module has no user source and
        // calls no Tool; the admission capability makes that structural.
        UF_TRY_VALUE(
            probe,
            compileSessionClosure(
                "return nil",
                frameworkModules,
                frameworkResources
            )
        );
        auto admissionVm = detail::QuotaBoundVm{
            maximumRuntime,
            cancellation,
            *convertedMemoryQuota,
            EngineConfig{}.interruptBudgetTicks,
        };
        auto admitted = detail::admitClosure(
            admissionVm,
            probe,
            admissionCapabilityInstaller()
        );
        if (!admitted)
        {
            if (admissionVm.memoryCeilingRefused())
            {
                return detail::refuse(
                    std::format(
                        "the scoped Tool session's Framework closure exhausted its "
                        "session-supplied Luau memory ceiling of {} bytes during admission",
                        *convertedMemoryQuota
                    )
                );
            }
            return std::unexpected{std::move(admitted).error()};
        }

        return ScopedToolSession{
            std::move(frameworkModules),
            std::move(frameworkResources),
            std::move(invokeTool),
            std::move(cancellation),
            maximumRuntime,
            *convertedMemoryQuota,
        };
    }

    auto ScopedToolSession::evaluate(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<ScriptValue>
    {
        if (m_generationSpent)
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "the scoped Tool session was already spent by cancellation or "
                "a script execution limit"
            );
        }

        UF_TRY_VALUE(
            closure,
            compileSessionClosure(
                source,
                m_frameworkModules,
                m_frameworkResources
            )
        );

        auto const displayName = chunkName.empty()
            ? std::string_view{"<unnamed>"}
            : chunkName;
        auto run = ScopedSessionRun{
            m_invokeTool,
            displayName,
            m_maximumRuntime,
            m_cancellation,
            m_memoryQuotaBytes,
            EngineConfig{}.interruptBudgetTicks,
        };
        auto result = detail::invokeClosure(
            run.vm(),
            closure,
            capabilityInstaller(run),
            k_sessionEntryPoint,
            json::Value{}
        );

        m_outcomeHeapUsage = script::heapUsage(run.vm().state());
        m_generationSpent  = run.vm().control().broken();
        m_heapUsage        = script::collectGarbage(run.vm().state());

        if (!result)
        {
            if (run.vm().memoryCeilingRefused())
            {
                return detail::refuse(
                    std::format(
                        "interactive chunk {} exhausted its session-supplied Luau "
                        "memory ceiling of {} bytes",
                        displayName,
                        m_memoryQuotaBytes
                    )
                );
            }
            if (run.vm().control().broken())
            {
                auto limit = run.executionLimitFailure();
                return std::unexpected{std::move(limit).error()};
            }
            return std::unexpected{std::move(result).error()};
        }
        switch (result->kind())
        {
        case json::ValueKind::Null: return ScriptValue{};
        case json::ValueKind::Boolean:
            return ScriptValue{result->boolean()};
        case json::ValueKind::Number: return ScriptValue{result->number()};
        case json::ValueKind::String:
            return ScriptValue{std::string{result->string()}};
        case json::ValueKind::Array:
            return detail::refuse(
                "an interactive chunk returned an array; result transport only "
                "carries absent, boolean, number or string"
            );
        case json::ValueKind::Object:
            return detail::refuse(
                "an interactive chunk returned an object; result transport only "
                "carries absent, boolean, number or string"
            );
        }
        UF_UNREACHABLE_MSG("Unknown JSON value kind");
    }

    auto ScopedToolSession::outcomeHeapUsage() const noexcept -> HeapUsage
    {
        return m_outcomeHeapUsage;
    }

    auto ScopedToolSession::heapUsage() const noexcept -> HeapUsage
    {
        return m_heapUsage;
    }

    auto ScopedToolSession::generationSpent() const noexcept -> bool
    {
        return m_generationSpent;
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

        output += ",\"scoped_limits\":{\"interactive_tool_calls\":";
        output += std::to_string(ScopedToolProgram::k_maximumInteractiveToolCalls);
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
