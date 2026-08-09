#include "operation.hpp"

#include <core/error/contracts.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        enum class FrozenGuard : uint8
        {
            Frozen,
            Unfrozen,
        };

        struct TransitionRule final
        {
            OperationState source;
            OperationEvent event;
            FrozenGuard    guard;
            OperationState target;
        };

        // The four events that end an Operation before anything was dispatched.
        // They are not in k_rules because they apply from every non-terminal
        // source state, and enumerating that product would state the same fact
        // thirteen times.
        struct PreDispatchTerminal final
        {
            OperationEvent event;
            OperationState target;
        };

        constexpr auto k_preDispatchTerminals = std::array{
            PreDispatchTerminal{OperationEvent::Invalidated, OperationState::Invalid},
            PreDispatchTerminal{OperationEvent::Denied, OperationState::Denied},
            PreDispatchTerminal{OperationEvent::Cancelled, OperationState::Cancelled},
            PreDispatchTerminal{OperationEvent::DeadlineExpired, OperationState::Expired},
        };

        constexpr auto k_rules = std::array{
            TransitionRule{OperationState::Proposed, OperationEvent::ApprovalRequired, FrozenGuard::Unfrozen, OperationState::AwaitingApproval},
            TransitionRule{OperationState::Proposed, OperationEvent::ReadyWithoutApproval, FrozenGuard::Unfrozen, OperationState::Ready},
            TransitionRule{OperationState::Proposed, OperationEvent::ReadCompleted, FrozenGuard::Unfrozen, OperationState::Confirmed},
            TransitionRule{OperationState::Proposed, OperationEvent::DecisionInputsChanged, FrozenGuard::Unfrozen, OperationState::NeedsRevalidation},
            TransitionRule{OperationState::AwaitingApproval, OperationEvent::DecisionInputsChanged, FrozenGuard::Unfrozen, OperationState::NeedsRevalidation},
            TransitionRule{OperationState::Ready, OperationEvent::DecisionInputsChanged, FrozenGuard::Unfrozen, OperationState::NeedsRevalidation},
            TransitionRule{OperationState::NeedsRevalidation, OperationEvent::Revalidated, FrozenGuard::Unfrozen, OperationState::Proposed},
            TransitionRule{OperationState::AwaitingApproval, OperationEvent::ApprovalObtained, FrozenGuard::Unfrozen, OperationState::Ready},
            TransitionRule{OperationState::Ready, OperationEvent::DispatchStarted, FrozenGuard::Unfrozen, OperationState::Running},
            TransitionRule{OperationState::Running, OperationEvent::HostOutcomeObserved, FrozenGuard::Frozen, OperationState::Reconciling},
            TransitionRule{OperationState::Reconciling, OperationEvent::NextStepApprovalRequired, FrozenGuard::Frozen, OperationState::AwaitingApproval},
            TransitionRule{OperationState::AwaitingApproval, OperationEvent::ApprovalObtained, FrozenGuard::Frozen, OperationState::Running},
            TransitionRule{OperationState::Reconciling, OperationEvent::NextStepReady, FrozenGuard::Frozen, OperationState::Running},
            TransitionRule{OperationState::Reconciling, OperationEvent::ReconciliationContinued, FrozenGuard::Frozen, OperationState::Reconciling},
            TransitionRule{OperationState::Reconciling, OperationEvent::ReconciliationConfirmed, FrozenGuard::Frozen, OperationState::Confirmed},
            TransitionRule{OperationState::Reconciling, OperationEvent::ReconciliationRejected, FrozenGuard::Frozen, OperationState::Rejected},
            TransitionRule{OperationState::Reconciling, OperationEvent::ReconciliationAmbiguous, FrozenGuard::Frozen, OperationState::Ambiguous},
            TransitionRule{OperationState::Ambiguous, OperationEvent::NewEvidence, FrozenGuard::Frozen, OperationState::Reconciling},
            TransitionRule{OperationState::Reconciling, OperationEvent::CorrectionCommitted, FrozenGuard::Frozen, OperationState::Diverged},
            TransitionRule{OperationState::AwaitingApproval, OperationEvent::PostDispatchAbort, FrozenGuard::Frozen, OperationState::Reconciling},
            TransitionRule{OperationState::Running, OperationEvent::PostDispatchAbort, FrozenGuard::Frozen, OperationState::Reconciling},
        };

        [[nodiscard]]
        constexpr auto guardMatches(
            FrozenGuard guard,
            bool planFrozen
        ) noexcept -> bool
        {
            switch (guard)
            {
            case FrozenGuard::Frozen: return planFrozen;
            case FrozenGuard::Unfrozen: return !planFrozen;
            }

            UF_UNREACHABLE_MSG("Unknown FrozenGuard value");
        }

        [[nodiscard]]
        constexpr auto isTerminal(
            OperationState state
        ) noexcept -> bool
        {
            switch (state)
            {
            case OperationState::Confirmed:
            case OperationState::Rejected:
            case OperationState::Invalid:
            case OperationState::Denied:
            case OperationState::Cancelled:
            case OperationState::Expired:
            case OperationState::Diverged:
                return true;
            case OperationState::Proposed:
            case OperationState::AwaitingApproval:
            case OperationState::Ready:
            case OperationState::NeedsRevalidation:
            case OperationState::Running:
            case OperationState::Reconciling:
            case OperationState::Ambiguous:
                return false;
            }

            UF_UNREACHABLE_MSG("Unknown OperationState value");
        }
    }

    auto operationStateWireName(
        OperationState state
    ) noexcept -> std::string_view
    {
        switch (state)
        {
        case OperationState::Proposed: return "proposed";
        case OperationState::AwaitingApproval: return "awaiting_approval";
        case OperationState::Ready: return "ready";
        case OperationState::NeedsRevalidation: return "needs_revalidation";
        case OperationState::Running: return "running";
        case OperationState::Reconciling: return "reconciling";
        case OperationState::Confirmed: return "confirmed";
        case OperationState::Rejected: return "rejected";
        case OperationState::Ambiguous: return "ambiguous";
        case OperationState::Invalid: return "invalid";
        case OperationState::Denied: return "denied";
        case OperationState::Cancelled: return "cancelled";
        case OperationState::Expired: return "expired";
        case OperationState::Diverged: return "diverged";
        }

        UF_UNREACHABLE_MSG("Unknown OperationState value");
    }

    auto parseOperationState(
        std::string_view value
    ) -> Result<OperationState>
    {
        constexpr auto states = std::array{
            OperationState::Proposed,
            OperationState::AwaitingApproval,
            OperationState::Ready,
            OperationState::NeedsRevalidation,
            OperationState::Running,
            OperationState::Reconciling,
            OperationState::Confirmed,
            OperationState::Rejected,
            OperationState::Ambiguous,
            OperationState::Invalid,
            OperationState::Denied,
            OperationState::Cancelled,
            OperationState::Expired,
            OperationState::Diverged,
        };
        auto const match = std::ranges::find_if(
            states,
            [value](OperationState candidate)
            {
                return operationStateWireName(candidate) == value;
            }
        );
        if (match == states.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("Unknown Operation state: {}", value)
            );
        }
        return *match;
    }

    auto OperationMachine::restore(
        OperationState state,
        bool planFrozen,
        bool hasDispatched
    ) -> Result<OperationMachine>
    {
        if (planFrozen != hasDispatched)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operation persistence flags disagree"
            );
        }

        auto const validState = hasDispatched
            ? (
                state == OperationState::AwaitingApproval
                || state == OperationState::Running
                || state == OperationState::Reconciling
                || state == OperationState::Confirmed
                || state == OperationState::Rejected
                || state == OperationState::Ambiguous
                || state == OperationState::Diverged
            )
            : (
                state == OperationState::Proposed
                || state == OperationState::AwaitingApproval
                || state == OperationState::Ready
                || state == OperationState::NeedsRevalidation
                || state == OperationState::Confirmed
                || state == OperationState::Invalid
                || state == OperationState::Denied
                || state == OperationState::Cancelled
                || state == OperationState::Expired
            );
        if (!validState)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operation persistence state contradicts dispatch history"
            );
        }

        auto machine            = OperationMachine{};
        machine.m_state         = state;
        machine.m_planFrozen    = planFrozen;
        machine.m_hasDispatched = hasDispatched;
        return machine;
    }

    auto OperationMachine::state() const noexcept -> OperationState
    {
        return m_state;
    }

    auto OperationMachine::planFrozen() const noexcept -> bool
    {
        return m_planFrozen;
    }

    auto OperationMachine::hasDispatched() const noexcept -> bool
    {
        return m_hasDispatched;
    }

    auto OperationMachine::mutationLocked() const noexcept -> bool
    {
        return m_hasDispatched && !isTerminal(m_state);
    }

    auto OperationMachine::terminal() const noexcept -> bool
    {
        return isTerminal(m_state);
    }

    auto OperationMachine::transition(
        OperationEvent event
    ) -> Result<OperationState>
    {
        if (isTerminal(m_state))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "terminal Operation has no outgoing transition"
            );
        }

        auto const rule = std::ranges::find_if(
            k_rules,
            [this, event](TransitionRule const& candidate)
            {
                return candidate.source == m_state
                    && candidate.event == event
                    && guardMatches(candidate.guard, m_planFrozen);
            }
        );
        if (rule == k_rules.end())
        {
            auto const preDispatch = std::ranges::find_if(
                k_preDispatchTerminals,
                [event](PreDispatchTerminal const& candidate)
                {
                    return candidate.event == event;
                }
            );
            if (!m_hasDispatched && preDispatch != k_preDispatchTerminals.end())
            {
                m_state = preDispatch->target;
                return m_state;
            }

            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "Operation transition is not allowed from state {} with event {}",
                    std::to_underlying(m_state),
                    std::to_underlying(event)
                )
            );
        }

        if (event == OperationEvent::DispatchStarted)
        {
            m_planFrozen    = true;
            m_hasDispatched = true;
        }
        m_state = rule->target;
        return m_state;
    }
}
