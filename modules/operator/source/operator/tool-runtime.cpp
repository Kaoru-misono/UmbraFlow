#include "tool-runtime.hpp"

#include <core/error/contracts.hpp>

#include <array>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        struct ToolCallStateName final
        {
            ToolCallState    state;
            std::string_view name;
        };

        constexpr auto k_toolCallStateNames = std::array{
            ToolCallStateName{ToolCallState::Proposed, "proposed"},
            ToolCallStateName{ToolCallState::Admitted, "admitted"},
            ToolCallStateName{ToolCallState::Dispatching, "dispatching"},
            ToolCallStateName{ToolCallState::Confirmed, "confirmed"},
            ToolCallStateName{ToolCallState::ProvenAbsent, "proven_absent"},
            ToolCallStateName{ToolCallState::Possible, "possible"},
            ToolCallStateName{ToolCallState::Rejected, "rejected"},
            ToolCallStateName{ToolCallState::TerminalFailure, "terminal_failure"},
            ToolCallStateName{
                ToolCallState::TerminallyUnresolved,
                "terminally_unresolved",
            },
        };
    }

    auto toolCallStateWireName(ToolCallState state) noexcept -> std::string_view
    {
        for (auto const& candidate : k_toolCallStateNames)
        {
            if (candidate.state == state)
            {
                return candidate.name;
            }
        }
        UF_UNREACHABLE_MSG("Unknown Tool call state");
    }

    auto parseToolCallState(std::string_view value) -> Result<ToolCallState>
    {
        for (auto const& candidate : k_toolCallStateNames)
        {
            if (candidate.name == value)
            {
                return candidate.state;
            }
        }
        return fail(
            AutomationErrorKind::InvalidResource,
            "Unknown stored Tool call state"
        );
    }

    ToolCallCompletion::ToolCallCompletion(
        ToolCallCompletionKind kind,
        CanonicalJson payload,
        std::optional<CanonicalJson> evidence
    )
        : m_kind{kind}
        , m_payload{std::move(payload)}
        , m_evidence{std::move(evidence)}
    {
    }

    auto ToolCallCompletion::confirmed(
        CanonicalJson result,
        std::optional<CanonicalJson> evidence
    ) -> ToolCallCompletion
    {
        return ToolCallCompletion{
            ToolCallCompletionKind::Confirmed,
            std::move(result),
            std::move(evidence),
        };
    }

    auto ToolCallCompletion::provenAbsent(
        CanonicalJson explanation,
        CanonicalJson evidence
    ) -> ToolCallCompletion
    {
        return ToolCallCompletion{
            ToolCallCompletionKind::ProvenAbsent,
            std::move(explanation),
            std::move(evidence),
        };
    }

    auto ToolCallCompletion::possible(
        CanonicalJson explanation,
        std::optional<CanonicalJson> evidence
    ) -> ToolCallCompletion
    {
        return ToolCallCompletion{
            ToolCallCompletionKind::Possible,
            std::move(explanation),
            std::move(evidence),
        };
    }

    auto ToolCallCompletion::terminalFailure(
        CanonicalJson error,
        std::optional<CanonicalJson> evidence
    ) -> ToolCallCompletion
    {
        return ToolCallCompletion{
            ToolCallCompletionKind::TerminalFailure,
            std::move(error),
            std::move(evidence),
        };
    }

    auto ToolCallCompletion::kind() const noexcept -> ToolCallCompletionKind
    {
        return m_kind;
    }

    auto ToolCallCompletion::payload() const noexcept -> CanonicalJson const&
    {
        return m_payload;
    }

    auto ToolCallCompletion::evidence() const noexcept
        -> std::optional<CanonicalJson> const&
    {
        return m_evidence;
    }

    ToolCallAdmission::ToolCallAdmission(
        ContentHash callIdentity,
        uint64 attemptNumber,
        uint64 historyRevision
    )
        : m_callIdentity{callIdentity}
        , m_attemptNumber{attemptNumber}
        , m_historyRevision{historyRevision}
    {
    }

    auto ToolCallAdmission::callIdentity() const -> ContentHash
    {
        return m_callIdentity;
    }

    auto ToolCallAdmission::attemptNumber() const noexcept -> uint64
    {
        return m_attemptNumber;
    }

    auto ToolCallAdmission::historyRevision() const noexcept -> uint64
    {
        return m_historyRevision;
    }

    ToolCallDispatch::ToolCallDispatch(
        ContentHash callIdentity,
        uint64 attemptNumber,
        uint64 historyRevision
    )
        : m_callIdentity{callIdentity}
        , m_attemptNumber{attemptNumber}
        , m_historyRevision{historyRevision}
    {
    }

    auto ToolCallDispatch::callIdentity() const -> ContentHash
    {
        return m_callIdentity;
    }

    auto ToolCallDispatch::attemptNumber() const noexcept -> uint64
    {
        return m_attemptNumber;
    }

    auto ToolCallDispatch::historyRevision() const noexcept -> uint64
    {
        return m_historyRevision;
    }
}
