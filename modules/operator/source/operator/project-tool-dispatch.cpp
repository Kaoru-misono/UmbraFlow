#include "project-tool-dispatch.hpp"

#include "tool-root-producer.hpp"

#include <json/value.hpp>

#include <core/numeric/checked-cast.hpp>

#include <domain/error.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace uf::operator_runtime
{
    class ProjectToolDispatcher::State final
        : public std::enable_shared_from_this<State>
    {
        // Shared only by synchronous callbacks within dispatchCall. The frame
        // cannot escape the invocation that owns the provider and authority.
        struct ChildCalls final
        {
            ToolCallIssuingContext issuing;
            std::optional<Error>   failure{};

            explicit ChildCalls(ToolCallPositionIdentity const& call)
                : issuing{ToolCallIssuingContext::forHandler(call)}
            {
            }
        };

        OperatorCoordinator& m_coordinator;

    public:
        explicit State(OperatorCoordinator& coordinator)
            : m_coordinator{coordinator}
        {
        }

        [[nodiscard]]
        auto dispatchCall(
            ProjectGenerationHandle const& program,
            ToolAdmissionRequest const& request,
            ToolProvider const& frameworkProvider,
            SnapshotObservationAuthority const& observations,
            script::ScopedRunRequest const& budgetRequest
        ) -> Result<ToolCallReplay>
        {
            UF_TRY_VALUE(catalog, ToolStartCatalog::create(program.catalog()));
            auto const p_children = std::make_shared<ChildCalls>(request.call);
            auto const p_state = shared_from_this();
            // These two borrows are synchronous: ToolRuntimeExecutor never
            // retains its provider, and the VM is destroyed before we return.
            auto const* const p_observations = &observations;
            auto invokeChild = script::ToolRuntimeInvoke{
                [
                    p_state, p_children, p_observations, program, request,
                    frameworkProvider, budgetRequest, catalog
                ](
                    std::string_view toolName,
                    json::Value const& arguments
                ) -> Result<json::Value>
                {
                    if (p_children->failure)
                    {
                        return std::unexpected{p_children->failure->clone()};
                    }
                    auto result = [&]() -> Result<json::Value>
                    {
                        UF_TRY_VALUE(
                            canonical,
                            CanonicalJson::parseExact(json::canonicalBytes(arguments))
                        );
                        UF_TRY_VALUE(
                            invocation,
                            catalog.validate(std::string{toolName}, std::move(canonical))
                        );
                        UF_TRY_VALUE(
                            child,
                            p_state->m_coordinator.issueToolChild(
                                p_children->issuing, invocation, *p_observations
                            )
                        );
                        auto ancestors = request.ancestors;
                        ancestors.emplace_back(request.call);
                        auto childRequest = ToolAdmissionRequest{
                            .controller      = request.controller,
                            .lease           = request.lease,
                            .root            = request.root,
                            .call            = child,
                            .policyAuthority = request.policyAuthority,
                            .mutation        = proposedToolMutation(
                                invocation, request.controller.controlledTargetId()
                            ),
                            .ancestors = std::move(ancestors),
                        };
                        auto replay = (
                            std::holds_alternative<ProjectToolProvider>(child.provider())
                                ? p_state->dispatchCall(
                                      program, childRequest, frameworkProvider,
                                      *p_observations, budgetRequest
                                  )
                                : ToolRuntimeExecutor{p_state->m_coordinator}.invoke(
                                      childRequest, frameworkProvider
                                  )
                        );
                        UF_TRY_VALUE(answer, std::move(replay));
                        if (
                            answer.state == ToolCallState::Possible
                            || answer.state == ToolCallState::TerminallyUnresolved
                            || !toolCallStateHasOutcome(answer.state)
                        )
                        {
                            return fail(
                                AutomationErrorKind::ActionRejected,
                                "Project Tool is blocked by unresolved child "
                                    + child.identity().hex()
                            );
                        }
                        return toolCallAnswer(child.identity(), answer);
                    }();
                    if (!result)
                    {
                        p_children->failure.emplace(result.error().clone());
                    }
                    return result;
                }
            };
            auto const runHandler = [
                &program, &budgetRequest, &invokeChild, &request, p_children, p_state
            ](ToolCallPositionIdentity const& call) -> Result<ToolCallCompletion>
            {
                UF_TRY_VALUE(arguments, json::parse(call.canonicalArgs()));
                auto runRequest                 = budgetRequest;
                runRequest.maximumElapsedMillis = call.descriptor().timeout.maximumElapsedMillis;
                UF_TRY_VALUE(
                    answer,
                    program.invokeBoundTool(
                        call.toolName(),
                        arguments,
                        runRequest,
                        invokeChild
                    )
                );
                if (p_children->failure)
                {
                    return std::unexpected{p_children->failure->clone()};
                }
                UF_TRY(p_state->m_coordinator.validateToolCallChildren(
                    request.root, call, p_children->issuing.issuedCalls()
                ));
                UF_TRY_VALUE(
                    result,
                    CanonicalJson::parseExact(json::canonicalBytes(answer))
                );
                return ToolCallCompletion::confirmed(std::move(result));
            };
            return ToolRuntimeExecutor{m_coordinator}.invoke(request, runHandler);
        }
    };

    ProjectToolDispatcher::ProjectToolDispatcher(
        std::shared_ptr<State> p_state
    ) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto ProjectToolDispatcher::create(OperatorCoordinator& coordinator)
        -> Result<ProjectToolDispatcher>
    {
        return ProjectToolDispatcher{std::make_shared<State>(coordinator)};
    }

    auto ProjectToolDispatcher::dispatch(
        ProjectGenerationHandle const& program,
        ToolAdmissionRequest const& request,
        ToolProvider const& frameworkProvider,
        SnapshotObservationAuthority const& observations,
        std::stop_token cancellation
    ) -> Result<ToolCallReplay>
    {
        if (!frameworkProvider)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Project Tool dispatch requires a Framework provider"
            );
        }
        auto const maximumElapsedMillis = request.call.descriptor().timeout.maximumElapsedMillis;
        auto const elapsed = checkedCast<std::chrono::milliseconds::rep>(maximumElapsedMillis);
        auto const representable = std::chrono::duration_cast<std::chrono::milliseconds>(
            MonotonicInstant::Duration::max()
        );
        if (!elapsed || *elapsed <= 0 || *elapsed > representable.count())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Project Tool requires a positive representable maximum_elapsed_ms"
            );
        }
        auto const budget = std::make_shared<script::ProjectToolBudget>(
            std::chrono::milliseconds{*elapsed}
        );
        return m_state->dispatchCall(
            program, request, frameworkProvider, observations,
            script::ScopedRunRequest{
                .callIdentity         = request.call.identity(),
                .budgetOwner          = request.call.toolName(),
                .maximumElapsedMillis = maximumElapsedMillis,
                .budget               = budget,
                .cancellation         = cancellation,
            }
        );
    }
}
