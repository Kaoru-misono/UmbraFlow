#include "project-tool-dispatch.hpp"

#include <json/value.hpp>

#include <domain/error.hpp>

#include <memory>
#include <string>
#include <utility>

namespace uf::operator_runtime
{
    class ProjectToolDispatcher::State final
    {
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
            std::stop_token const& cancellation
        ) -> Result<ToolCallReplay>
        {
            auto const runHandler = [&program, &cancellation](
                                        ToolCallPositionIdentity const& call
                                    ) -> Result<ToolCallCompletion>
            {
                UF_TRY_VALUE(arguments, json::parse(call.canonicalArgs()));
                UF_TRY_VALUE(
                    answer,
                    program.invokeBoundTool(
                        call.toolName(),
                        arguments,
                        script::ScopedRunRequest{
                            .callIdentity = call.identity(),
                            .budgetOwner  = std::string{call.toolName()},
                            .maximumElapsedMillis = call.descriptor()
                                                        .timeout
                                                        .maximumElapsedMillis,
                            .cancellation = cancellation,
                        }
                    )
                );
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
        std::stop_token cancellation
    ) -> Result<ToolCallReplay>
    {
        return m_state->dispatchCall(program, request, cancellation);
    }
}
