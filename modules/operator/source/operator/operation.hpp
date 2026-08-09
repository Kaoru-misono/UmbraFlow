#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <string_view>

namespace uf::operator_runtime
{
    enum class OperationState : uint8
    {
        Proposed,
        AwaitingApproval,
        Ready,
        NeedsRevalidation,
        Running,
        Reconciling,
        Confirmed,
        Rejected,
        Ambiguous,
        Invalid,
        Denied,
        Cancelled,
        Expired,
        Diverged,
    };

    enum class OperationEvent : uint8
    {
        ApprovalRequired,
        ApprovalObtained,
        ReadyWithoutApproval,
        ReadCompleted,
        DecisionInputsChanged,
        Revalidated,
        Invalidated,
        Denied,
        Cancelled,
        DeadlineExpired,
        DispatchStarted,
        HostOutcomeObserved,
        NextStepApprovalRequired,
        NextStepReady,
        ReconciliationContinued,
        ReconciliationConfirmed,
        ReconciliationRejected,
        ReconciliationAmbiguous,
        NewEvidence,
        CorrectionCommitted,
        PostDispatchAbort,
    };

    [[nodiscard]]
    auto operationStateWireName(
        OperationState state
    ) noexcept -> std::string_view;

    [[nodiscard]]
    auto parseOperationState(
        std::string_view value
    ) -> Result<OperationState>;

    class OperationMachine final
    {
        OperationState m_state{OperationState::Proposed};
        bool           m_planFrozen{};
        bool           m_hasDispatched{};

    public:
        [[nodiscard]]
        static auto restore(
            OperationState state,
            bool planFrozen,
            bool hasDispatched
        ) -> Result<OperationMachine>;

        [[nodiscard]] auto state() const noexcept -> OperationState;
        [[nodiscard]] auto planFrozen() const noexcept -> bool;
        [[nodiscard]] auto hasDispatched() const noexcept -> bool;
        [[nodiscard]] auto mutationLocked() const noexcept -> bool;
        [[nodiscard]] auto terminal() const noexcept -> bool;

        [[nodiscard]]
        auto transition(
            OperationEvent event
        ) -> Result<OperationState>;
    };
}
