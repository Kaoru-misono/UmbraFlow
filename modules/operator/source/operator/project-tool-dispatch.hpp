#pragma once

#include "ledger.hpp"
#include "project-generation.hpp"
#include "snapshot-reference.hpp"
#include "tool-admission-request.hpp"
#include "tool-executor.hpp"
#include "tool-runtime.hpp"

#include <core/error/result.hpp>

#include <memory>
#include <stop_token>

namespace uf::operator_runtime
{
    // Runs a Project handler and its recorded child calls under one outer VM
    // budget. Every child reaches the same admission and replay boundary.
    // The coordinator must outlive this dispatcher and every dispatch.
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
            ToolProvider const& frameworkProvider,
            SnapshotObservationAuthority const& observations,
            std::stop_token cancellation
        ) -> Result<ToolCallReplay>;
    };
}
