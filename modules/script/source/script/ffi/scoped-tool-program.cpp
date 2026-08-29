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
    class ProjectToolBudget::State final
    {
    public:
        MemoryQuota                   quota{.limitBytes = PureDataProgram::k_memoryQuotaBytes};
        uint64                        calls{};
        std::vector<MonotonicInstant> deadlines{};
    };

    ProjectToolBudget::ProjectToolBudget(MonotonicInstant::Duration maximumRuntime)
        : m_state{std::make_unique<State>()}
    {
        auto const now = MonotonicInstant::now();
        m_state->deadlines.push_back(
            maximumRuntime <= MonotonicInstant::Duration::zero()
                ? now
                : now.checkedAdd(maximumRuntime).value_or(k_maximumInstant)
        );
    }

    ProjectToolBudget::~ProjectToolBudget() = default;

    auto ProjectToolBudget::deadline() const noexcept -> MonotonicInstant
    {
        return m_state->deadlines.back();
    }

    auto ProjectToolBudget::chargeToolCall() -> Status
    {
        if (m_state->calls == ScopedToolProgram::k_maximumProjectToolCalls)
        {
            return detail::refuse(
                "one outer Project Tool invocation exceeded its fixed "
                "descendant Tool call ceiling"
            );
        }
        ++m_state->calls;
        return ok();
    }

    namespace
    {
        // The two static scoped modules, each spelled once and referred to by
        // its constant everywhere else: the same name appears in the scoped
        // catalog, in a visibility list, in the capability list and inside the
        // source of every generated module, and four spellings of one name is
        // four places to drift.
        constexpr auto k_catalogModule = std::string_view{"@umbraflow/catalog"};
        constexpr auto k_renderModule =
            std::string_view{"@umbraflow/internal/render"};

        // The scoped Framework catalog, in JCS order because the environment
        // identity states it in this order. A name moved here moves that digest
        // and nothing else in the repository can move it back.
        //
        // These two are the STATIC half. The rest of a scoped closure's Tool
        // surface is GENERATED from the pinned catalog below, one module per
        // Tool namespace, and is therefore a property of the run rather than of
        // the release.
        constexpr auto k_scopedModules = std::array{
            k_catalogModule,
            k_renderModule,
        };

        // Which of the static two Project code may resolve. The renderer is not
        // one of them: it holds the private capability table, so the ONLY route
        // from Project source to the Tool Runtime is a generated module the host
        // built from the pinned catalog, and a chunk cannot reach past it to
        // call a name the catalog does not carry.
        constexpr auto k_projectVisibleScopedModules = std::array{
            k_catalogModule,
        };

        // Which module is handed the private capability table. Exactly one is:
        // the renderer. `@umbraflow/catalog` reads the same pinned bytes as data
        // and holds no capability at all, so a reader asking "what here can act"
        // has one answer rather than a list to audit.
        constexpr auto k_capabilityBoundModules = std::array{
            k_renderModule,
        };

        // The read-only JSON resource the host bakes this run's pinned Tool
        // catalog into. It is named HERE, by the program type that renders it,
        // rather than by the bundle: the module set a scoped closure carries is
        // derived from these bytes, so the type that builds the closure has to
        // be able to read them.
        constexpr auto k_toolCatalogResource =
            std::string_view{"umbraflow.tool-catalog"};

        // Where a generated module sits in the declared topology: one above the
        // renderer it asks to render it. Metadata on this path -- the closure
        // resolves by name and loads on demand -- but it is what the bundle's
        // own depth discipline says, stated rather than left at zero.
        constexpr auto k_generatedModuleDepth = std::size_t{5};

        // The single field of the private capability table, and the whole of the
        // native surface. Everything else -- the generated Tool face, delivery
        // classification, discovery -- is Luau over this one closure, so there
        // is exactly one native primitive to reason about and exactly one path
        // an effect can take.
        constexpr auto k_capabilityInvokeField = std::string_view{"invoke"};
        constexpr auto k_toolInvokeArity       = 2;

        // Observable contracts. These move the scoped environment identity
        // whenever a script can distinguish the old runtime from the new one.
        //
        // A Tool's top-level arguments are one JSON OBJECT by contract, so the
        // seam admits only a table there and reads an EMPTY one as `{}` rather
        // than as the empty array a Luau table would otherwise decode to. That
        // is what retires the `canon.emptyObject` spelling at this boundary: a
        // caller writes `screen.capture{}` and means the object.
        constexpr auto k_toolArgumentContract = std::string_view{
            "tool_name_string_plus_one_argument_object_empty_table_is_object_v4"
        };

        // How a Tool name becomes a module a chunk can require, stated as one
        // literal because it is the whole of what a caller must know to address
        // any Tool at all. It is in the environment identity for the same
        // reason the argument shape is: a build that renamed the modules under
        // a chunk would otherwise move nothing.
        constexpr auto k_generatedModuleContract = std::string_view{
            "tool_namespace_is_the_module_path_under_@umbraflow_framework_elided_v1"
        };
        constexpr auto k_toolResultContract = std::string_view{
            "one_frozen_decoded_json_value_no_userdata_no_metatable_v1"
        };
        constexpr auto k_toolFailureContract = std::string_view{
            "runtime_refusal_is_terminal_uncatchable_vm_teardown_v1"
        };
        constexpr auto k_capabilityTableContract = std::string_view{
            "single_invoke_closure_chunk_argument_to_the_generated_tool_face_v2"
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

        // The framework's own Tool namespace. The module path elides it
        // because `@umbraflow/` already IS it, which is what makes
        // `framework.screen.capture` reachable as `screen.capture` after
        // requiring `@umbraflow/screen`. A registration inside `framework.` is
        // refused, so no Project namespace can be elided by this rule.
        constexpr auto k_frameworkToolNamespace = std::string_view{"framework."};
        constexpr auto k_bareFrameworkNamespace = std::string_view{"framework"};

        // The module a Tool is reached through, and the namespace that module
        // asks the renderer to render.
        struct ToolPlacement final
        {
            std::string moduleName{};
            std::string toolNamespace{};
        };

        // The one rule, applied to one Tool name. Everything before the last
        // dot is the namespace and becomes the module path; the last segment is
        // the member. A name whose namespace is only `framework` has no module
        // segment left after the elision and is refused by name -- the only
        // catalog shape this spelling cannot render, and no Tool has it.
        //
        // Nothing here restricts what a member may be called. A member that is
        // not a Luau identifier is reached as `alpha["observe-1"]{...}`, which
        // is the language's own index sugar for one expression rather than a
        // second spelling, so a legal Tool name never has to be refused for it.
        [[nodiscard]]
        auto placementOf(std::string_view toolName) -> Result<ToolPlacement>
        {
            auto const lastDot = toolName.rfind('.');
            if (lastDot == std::string_view::npos)
            {
                return detail::refuse(
                    "the pinned Tool catalog names " + std::string{toolName}
                    + ", which has no namespace to become a module"
                );
            }
            auto const toolNamespace = toolName.substr(0U, lastDot);
            if (toolNamespace == k_bareFrameworkNamespace)
            {
                return detail::refuse(
                    "the pinned Tool catalog names " + std::string{toolName}
                    + ", whose namespace is the framework's own and leaves no "
                      "module segment behind"
                );
            }
            auto path = std::string{
                toolNamespace.starts_with(k_frameworkToolNamespace)
                    ? toolNamespace.substr(k_frameworkToolNamespace.size())
                    : toolNamespace
            };
            std::ranges::replace(path, '.', '/');
            return ToolPlacement{
                .moduleName    = std::string{detail::k_frameworkModulePrefix} + path,
                .toolNamespace = std::string{toolNamespace},
            };
        }

        // The Tool names this run pinned, read out of the catalog resource the
        // host baked. It is read here rather than passed in because the scoped
        // program type is the thing that renders it: a caller that could supply
        // a different list than the one `@umbraflow/catalog` answers from could
        // publish a module for a Tool the run never pinned.
        [[nodiscard]]
        auto pinnedToolNames(
            std::span<PureDataProgram::Resource const> frameworkResources
        ) -> Result<std::vector<std::string>>
        {
            auto const found = std::ranges::find(
                frameworkResources,
                k_toolCatalogResource,
                &PureDataProgram::Resource::name
            );
            if (found == frameworkResources.end())
            {
                return detail::refuse(
                    "a scoped tool program requires the pinned Tool catalog "
                    "resource "
                    + std::string{k_toolCatalogResource}
                );
            }
            UF_TRY_VALUE(document, json::parse(found->bytes));
            auto const* const p_tools = document.find("tools");
            if (
                document.kind() != json::ValueKind::Object || p_tools == nullptr
                || p_tools->kind() != json::ValueKind::Array
            )
            {
                return detail::refuse(
                    "the pinned Tool catalog resource carries no tools list"
                );
            }
            auto names = std::vector<std::string>{};
            names.reserve(p_tools->items().size());
            for (auto const& entry : p_tools->items())
            {
                auto const* const p_name = entry.find("name");
                if (
                    entry.kind() != json::ValueKind::Object || p_name == nullptr
                    || p_name->kind() != json::ValueKind::String
                    || !validToolName(p_name->string())
                )
                {
                    return detail::refuse(
                        "the pinned Tool catalog carries a descriptor that does "
                        "not name a canonical tool"
                    );
                }
                names.emplace_back(p_name->string());
            }
            return names;
        }

        // The Tool face, as modules. One per Tool namespace the catalog names,
        // each a single line asking the renderer for that namespace, so there is
        // no per-Tool source anywhere and the catalog moving moves the whole
        // surface with it.
        [[nodiscard]]
        auto generatedToolModules(
            std::span<PureDataProgram::Resource const> frameworkResources,
            std::span<FrameworkModule const> frameworkModules
        ) -> Result<std::vector<PureDataProgram::Module>>
        {
            UF_TRY_VALUE(names, pinnedToolNames(frameworkResources));
            auto generated = std::vector<PureDataProgram::Module>{};
            for (auto const& toolName : names)
            {
                UF_TRY_VALUE(placement, placementOf(toolName));
                if (std::ranges::contains(
                        generated,
                        placement.moduleName,
                        &PureDataProgram::Module::name
                    ))
                {
                    continue;
                }
                if (std::ranges::contains(
                        frameworkModules,
                        placement.moduleName,
                        &FrameworkModule::name
                    ))
                {
                    return detail::refuse(
                        "the pinned Tool catalog renders " + toolName
                        + " into the module " + placement.moduleName
                        + ", which the Framework closure already carries"
                    );
                }
                generated.emplace_back(PureDataProgram::Module{
                    .name   = placement.moduleName,
                    .source = "return require(\"" + std::string{k_renderModule}
                        + "\").module(\"" + placement.toolNamespace + "\")\n",
                });
            }
            return generated;
        }

        // The static Framework list plus the generated one, as the view list a
        // closure is compiled from. The generated sources must outlive the
        // returned vector, which is why every caller keeps them in a local that
        // is not moved from until the closure is compiled.
        [[nodiscard]]
        auto withGeneratedModules(
            std::span<FrameworkModule const> frameworkModules,
            std::span<PureDataProgram::Module const> generated
        ) -> std::vector<FrameworkModule>
        {
            auto combined = std::vector<FrameworkModule>{};
            combined.reserve(frameworkModules.size() + generated.size());
            combined.assign(frameworkModules.begin(), frameworkModules.end());
            for (auto const& module : generated)
            {
                combined.emplace_back(FrameworkModule{
                    .name            = module.name,
                    .source          = module.source,
                    .dependencyDepth = k_generatedModuleDepth,
                    .projectVisible  = true,
                });
            }
            return combined;
        }

        // The run context the ONE VM primitive borrows. Registered handlers
        // reach their child issuing door; interactive chunks reach their root.
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

        // One registered Project Tool run. The VM is declared last so it
        // dies before every object its native closure can reach.
        class ScopedToolRun final : public ScopedVmRun
        {
            ContentHash                        m_budgetPosition;
            std::string                        m_budgetOwner;
            uint64                             m_maximumElapsedMillis;
            std::shared_ptr<ProjectToolBudget> m_budget;

            // Borrowed only by invoke()'s stack-local run; its caller owns the
            // callback until the run and its VM are destroyed.
            ToolRuntimeInvoke& m_invokeTool;

            detail::QuotaBoundVm m_vm;

        public:
            ScopedToolRun(
                ScopedRunRequest const& request,
                MonotonicInstant::Duration runtimeCeiling,
                MemoryQuota& sharedQuota,
                ToolRuntimeInvoke& invokeTool
            )
                : m_budgetPosition{request.callIdentity}
                , m_budgetOwner{request.budgetOwner}
                , m_maximumElapsedMillis{request.maximumElapsedMillis}
                , m_budget{request.budget}
                , m_invokeTool{invokeTool}
                , m_vm{
                      runtimeCeiling,
                      request.cancellation,
                      sharedQuota,
                      request.budget->deadline(),
                  }
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
                json::Value const& arguments
            ) -> Result<json::Value> override
            {
                UF_TRY(checkBudget());
                UF_TRY(m_budget->chargeToolCall());
                auto result = m_invokeTool(toolName, arguments);
                UF_TRY(checkBudget());
                return result;
            }

            [[nodiscard]] auto checkBudget() -> Status
            {
                if (m_vm.control().cancellation.stop_requested())
                {
                    return fail(
                        AutomationErrorKind::Cancelled,
                        "Project Tool " + m_budgetOwner + " was cancelled"
                    );
                }
                if (MonotonicInstant::now() >= m_budget->deadline())
                {
                    return fail(
                        AutomationErrorKind::Cancelled,
                        "Project Tool " + m_budgetOwner
                            + " exceeded its shared ancestor elapsed deadline"
                    );
                }
                if (m_vm.memoryCeilingRefused())
                {
                    return detail::refuse(
                        "Project Tool " + m_budgetOwner
                            + " exhausted its shared Luau memory ceiling"
                    );
                }
                return ok();
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
                || lua_type(state, 2) != LUA_TTABLE
            )
            {
                luaL_error(
                    state,
                    "the Tool Runtime primitive takes a tool name and one "
                    "argument object"
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
                if (
                    arguments.has_value()
                    && arguments->kind() == json::ValueKind::Array
                    && arguments->items().empty()
                )
                {
                    // A Tool's top-level arguments are an object by contract, so
                    // the one table shape Luau cannot tell apart from an empty
                    // array is read as the object it must be. This is the whole
                    // reason a caller no longer needs an empty-object sentinel
                    // to spell `capture{}`.
                    *arguments = json::Value::ofObject({});
                }
                if (!arguments.has_value())
                {
                    refusalText = detail::boundedText(arguments.error().message());
                    refused     = true;
                }
                else if (arguments->kind() != json::ValueKind::Object)
                {
                    refusalText = detail::boundedText(
                        "a Tool call's arguments must be one JSON object"
                    );
                    refused = true;
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
                // Visibility is asserted in both directions. The discovery
                // module must be reachable from Project source or a chunk
                // cannot ask what exists; the renderer must NOT be, or a chunk
                // could hold the capability table's one primitive directly and
                // address a Tool the pinned catalog never named.
                auto const shouldBeVisible = std::ranges::contains(
                    k_projectVisibleScopedModules,
                    scopedName
                );
                if (found->projectVisible != shouldBeVisible)
                {
                    return detail::refuse(
                        std::string{"scoped tool program requires "}
                        + std::string{scopedName}
                        + (shouldBeVisible
                               ? " to be project-visible"
                               : " to be hidden from Project source")
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

        // One chunk plus the generation's own module closure, compiled as the
        // one Project half of a session closure. The chunk is the entry module
        // and the Project modules sit beside it under their registered names,
        // so a chunk requires `strategy/battle` exactly as a registered handler
        // in the same generation does -- one graph, resolved by the same host
        // resolver, and no second visibility rule anywhere.
        [[nodiscard]]
        auto compileSessionClosure(
            std::string_view source,
            std::span<FrameworkModule const> frameworkModules,
            std::span<PureDataProgram::Module const> generated,
            std::span<PureDataProgram::Module const> projectModules,
            std::vector<PureDataProgram::Resource> projectResources,
            std::vector<PureDataProgram::Resource> frameworkResources
        ) -> Result<detail::ProgramClosure>
        {
            auto modules = std::vector<PureDataProgram::Module>{};
            modules.reserve(projectModules.size() + 1U);
            modules.emplace_back(PureDataProgram::Module{
                .name   = std::string{k_sessionModuleName},
                .source = sessionModuleSource(source),
            });
            modules.insert(
                modules.end(),
                projectModules.begin(),
                projectModules.end()
            );
            auto const rendered =
                withGeneratedModules(frameworkModules, generated);
            return detail::compileClosure(
                detail::ClosureSpec{
                    .pluginId               = k_sessionPluginId,
                    .entryModule            = k_sessionModuleName,
                    .entryPoints            = k_sessionEntryPoints,
                    .frameworkModules       = rendered,
                    .capabilityBoundModules = k_capabilityBoundModules,
                },
                std::move(modules),
                std::move(projectResources),
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

    auto ScopedToolProgram::projectVisibleScopedModuleNames()
        -> std::span<std::string_view const>
    {
        return k_projectVisibleScopedModules;
    }

    auto scopedToolCatalogResourceName() noexcept -> std::string_view
    {
        return k_toolCatalogResource;
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
        UF_TRY_VALUE(
            generated,
            generatedToolModules(frameworkResources, frameworkModules)
        );
        auto const rendered = withGeneratedModules(frameworkModules, generated);

        auto const spec = detail::ClosureSpec{
            .pluginId               = pluginId,
            .entryModule            = entryModule,
            .entryPoints            = entryPoints,
            .frameworkModules       = rendered,
            .capabilityBoundModules = k_capabilityBoundModules,
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
        ScopedRunRequest const& request,
        ToolRuntimeInvoke& invokeTool
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
        if (!request.budget || !invokeTool)
        {
            return detail::refuse(
                "a scoped run requires its shared Project budget and child issuing door"
            );
        }
        auto const representable = std::chrono::duration_cast<std::chrono::milliseconds>(
            MonotonicInstant::Duration::max()
        );
        if (
            request.maximumElapsedMillis == 0U
            || std::cmp_greater(request.maximumElapsedMillis, representable.count())
        )
        {
            return detail::refuse(
                "a scoped run must carry a positive representable "
                "registration-declared maximum_elapsed_ms"
            );
        }

        // The initial deadline belongs to the budget itself. Every additional
        // entry is one active handler; reject before constructing another VM.
        if (request.budget->m_state->deadlines.size() > k_maximumProjectToolDepth)
        {
            return detail::refuse("Project Tool call depth exceeds 16 active handlers");
        }
        auto const runtimeCeiling = std::chrono::milliseconds{
            static_cast<int64>(request.maximumElapsedMillis)
        };
        auto const localDeadline = (
            MonotonicInstant::now().checkedAdd(runtimeCeiling)
                .value_or(k_maximumInstant)
        );
        request.budget->m_state->deadlines.push_back(
            std::min(localDeadline, request.budget->deadline())
        );
        auto restoreDeadline = ScopeExit{
            [budget = request.budget]() noexcept
            {
                budget->m_state->deadlines.pop_back();
            }
        };
        auto run = ScopedToolRun{
            request, runtimeCeiling, request.budget->m_state->quota, invokeTool,
        };
        UF_TRY(run.checkBudget());
        auto result = detail::invokeClosure(
            run.vm(),
            m_state->closure,
            capabilityInstaller(run),
            entryPoint,
            immutableInput
        );
        UF_TRY(run.checkBudget());
        return result;
    }

    ScopedToolSession::ScopedToolSession(
        std::vector<FrameworkModule> frameworkModules,
        std::vector<PureDataProgram::Module> generatedModules,
        std::vector<PureDataProgram::Resource> frameworkResources,
        std::vector<PureDataProgram::Module> projectModules,
        std::vector<PureDataProgram::Resource> projectResources,
        ToolRuntimeInvoke invokeTool,
        std::stop_token cancellation,
        MonotonicInstant::Duration maximumRuntime,
        std::size_t memoryQuotaBytes
    ) noexcept
        : m_frameworkModules{std::move(frameworkModules)}
        , m_generatedModules{std::move(generatedModules)}
        , m_frameworkResources{std::move(frameworkResources)}
        , m_projectModules{std::move(projectModules)}
        , m_projectResources{std::move(projectResources)}
        , m_invokeTool{std::move(invokeTool)}
        , m_cancellation{std::move(cancellation)}
        , m_maximumRuntime{maximumRuntime}
        , m_memoryQuotaBytes{memoryQuotaBytes}
    {
    }

    auto ScopedToolSession::create(
        std::vector<FrameworkModule> frameworkModules,
        std::vector<PureDataProgram::Resource> frameworkResources,
        std::vector<PureDataProgram::Module> projectModules,
        std::vector<PureDataProgram::Resource> projectResources,
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

        // The one name a Project closure may not carry into a session. The
        // compiler beneath would refuse the pair as a duplicate and name
        // neither side of it, and a Project author reading "module names must
        // be unique" about a module they declared once has nothing to act on.
        for (auto const& module : projectModules)
        {
            if (module.name == k_sessionModuleName)
            {
                return detail::refuse(
                    "a Project closure module may not be named "
                    + std::string{k_sessionModuleName}
                    + ": that name is the interactive session's own root module"
                );
            }
        }

        UF_TRY(validateScopedCatalog(frameworkModules));
        UF_TRY_VALUE(
            generated,
            generatedToolModules(frameworkResources, frameworkModules)
        );
        auto const convertedMemoryQuota = checkedCast<std::size_t>(memoryQuotaBytes);
        if (!convertedMemoryQuota.has_value())
        {
            return detail::refuse(
                "a scoped Tool session's memory ceiling is not representable"
            );
        }

        // Validate and boot the exact Framework closure, catalog and Project
        // closure before the caller receives a session. The probe module has no
        // user source and calls no Tool; the admission capability makes that
        // structural.
        //
        // Every Project module is COMPILED here, so a Project module that does
        // not parse refuses the session and names the module. None of them is
        // EXECUTED here: the closure is resolved on demand, so a module's
        // top-level code first runs in the chunk that requires it. That is
        // exactly what ScopedToolProgram::compile does for a registered handler
        // over the same graph, and matching it is the point -- a session and a
        // handler must not disagree about when a Project module runs.
        UF_TRY_VALUE(
            probe,
            compileSessionClosure(
                "return nil",
                frameworkModules,
                generated,
                projectModules,
                projectResources,
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
                        "the scoped Tool session's Framework and Project closure "
                        "exhausted its session-supplied Luau memory ceiling of {} "
                        "bytes during admission",
                        *convertedMemoryQuota
                    )
                );
            }
            return std::unexpected{std::move(admitted).error()};
        }

        return ScopedToolSession{
            std::move(frameworkModules),
            std::move(generated),
            std::move(frameworkResources),
            std::move(projectModules),
            std::move(projectResources),
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
                m_generatedModules,
                m_projectModules,
                m_projectResources,
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
        output += ",\"project_tool_calls\":";
        output += std::to_string(ScopedToolProgram::k_maximumProjectToolCalls);
        output += ",\"project_tool_depth\":";
        output += std::to_string(ScopedToolProgram::k_maximumProjectToolDepth);
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
        output += ",\"capability_modules\":[";
        auto boundSeparated = false;
        for (auto const name : k_capabilityBoundModules)
        {
            if (boundSeparated)
            {
                output += ',';
            }
            boundSeparated = true;
            appendJsonString(output, name);
        }
        output += "],\"capability_table\":";
        appendJsonString(output, k_capabilityTableContract);
        output += ",\"catalog_resource\":";
        appendJsonString(output, k_toolCatalogResource);
        output += ",\"failure_behaviour\":";
        appendJsonString(output, k_toolFailureContract);
        output += ",\"generated_module_contract\":";
        appendJsonString(output, k_generatedModuleContract);
        output += ",\"generated_module_depth\":";
        output += std::to_string(k_generatedModuleDepth);
        output += ",\"handler_composition\":";
        appendJsonString(
            output,
            "durable_children_shared_live_vm_quota_ancestor_deadlines_v1"
        );
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
