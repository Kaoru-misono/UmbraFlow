#pragma once

#include "ledger.hpp"
#include "project-generation.hpp"
#include "tool-admission-request.hpp"
#include "tool-executor.hpp"
#include "tool-runtime.hpp"

#include <core/error/result.hpp>

#include <memory>
#include <stop_token>

namespace uf::operator_runtime
{
    // Runs one admitted Project Tool leaf handler. A terminal row replays
    // without invoking the handler; every nonterminal entry starts the pure
    // handler again from its immutable input. The handler's Tool primitive is
    // a terminal named refusal, so there is no child-call admission path.
    class ProjectToolDispatcher final
    {
        class State;

        std::shared_ptr<State> m_state;

        explicit ProjectToolDispatcher(std::shared_ptr<State> p_state) noexcept;

    public:
        [[nodiscard]]
        static auto create(OperatorCoordinator& coordinator)
            -> Result<ProjectToolDispatcher>;

        ProjectToolDispatcher(ProjectToolDispatcher const&) noexcept = default;
        ProjectToolDispatcher(ProjectToolDispatcher&&) noexcept      = default;
        auto operator=(ProjectToolDispatcher const&) noexcept
            -> ProjectToolDispatcher& = default;
        auto operator=(ProjectToolDispatcher&&) noexcept
            -> ProjectToolDispatcher& = default;
        ~ProjectToolDispatcher() = default;

        [[nodiscard]]
        auto dispatch(
            ProjectGenerationHandle const& program,
            ToolAdmissionRequest const& request,
            std::stop_token cancellation
        ) -> Result<ToolCallReplay>;
    };
}
