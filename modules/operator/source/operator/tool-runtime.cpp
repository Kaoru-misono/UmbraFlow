#include "tool-runtime.hpp"

#include <core/error/contracts.hpp>

#include <json/value.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        struct ToolCallStateName final
        {
            ToolCallState    state;
            std::string_view name;
        };

        // The two closed conclusion sets, listed once so the vocabulary
        // material walks the same set every switch above answers for. A kind
        // added to the enum and not to its array renders a material that
        // describes fewer conclusions than the protocol admits, so the arrays
        // are asserted against the enum sizes below them.
        constexpr auto k_toolCallCompletionKinds = std::array{
            ToolCallCompletionKind::Confirmed,
            ToolCallCompletionKind::ProvenAbsent,
            ToolCallCompletionKind::Possible,
            ToolCallCompletionKind::TerminalFailure,
        };

        constexpr auto k_toolCallReconciliationKinds = std::array{
            ToolCallReconciliationKind::Confirmed,
            ToolCallReconciliationKind::ProvenAbsent,
            ToolCallReconciliationKind::TerminallyUnresolved,
        };

        constexpr auto k_toolCallStateNames = std::array{
            ToolCallStateName{ToolCallState::Proposed, "proposed"},
            ToolCallStateName{ToolCallState::Admitted, "admitted"},
            ToolCallStateName{ToolCallState::Dispatching, "dispatching"},
            ToolCallStateName{ToolCallState::Confirmed, "confirmed"},
            ToolCallStateName{ToolCallState::ProvenAbsent, "proven_absent"},
            ToolCallStateName{ToolCallState::Possible, "possible"},
            ToolCallStateName{ToolCallState::TerminalFailure, "terminal_failure"},
            ToolCallStateName{
                ToolCallState::TerminallyUnresolved,
                "terminally_unresolved",
            },
        };
    }

    auto toolEffectComposition(std::string_view providerKind)
        -> ToolEffectComposition
    {
        auto const found = std::ranges::find(
            k_toolAnswerers,
            providerKind,
            &ToolAnswerer::providerKind
        );
        if (found == k_toolAnswerers.end())
        {
            // tool_call_positions.provider_kind is CHECK-constrained to the
            // table's own two values, so a third one is a corrupted database
            // rather than a case to classify.
            UF_UNREACHABLE_MSG("Unknown Tool provider kind");
        }
        return found->composition;
    }

    auto toolCallEffectMayBeUnrecorded(
        ToolEffectComposition composition,
        ToolMutability mutability
    ) noexcept -> bool
    {
        return composition == ToolEffectComposition::DirectLeaf
            && mutability == ToolMutability::Mutating;
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

    auto toolCallStateHasOutcome(ToolCallState state) noexcept -> bool
    {
        switch (state)
        {
        case ToolCallState::Proposed:
        case ToolCallState::Admitted:
        case ToolCallState::Dispatching:
            return false;
        case ToolCallState::Confirmed:
        case ToolCallState::ProvenAbsent:
        case ToolCallState::Possible:
        case ToolCallState::TerminalFailure:
        case ToolCallState::TerminallyUnresolved:
            return true;
        }
        UF_UNREACHABLE_MSG("Unknown Tool call state outcome relation");
    }

    auto toolCallStateFor(ToolCallCompletionKind kind) noexcept -> ToolCallState
    {
        switch (kind)
        {
        case ToolCallCompletionKind::Confirmed:
            return ToolCallState::Confirmed;
        case ToolCallCompletionKind::ProvenAbsent:
            return ToolCallState::ProvenAbsent;
        case ToolCallCompletionKind::Possible:
            return ToolCallState::Possible;
        case ToolCallCompletionKind::TerminalFailure:
            return ToolCallState::TerminalFailure;
        }
        UF_UNREACHABLE_MSG("Unknown Tool call completion kind");
    }

    auto toolCallStateFor(ToolCallReconciliationKind kind) noexcept -> ToolCallState
    {
        switch (kind)
        {
        case ToolCallReconciliationKind::Confirmed:
            return ToolCallState::Confirmed;
        case ToolCallReconciliationKind::ProvenAbsent:
            return ToolCallState::ProvenAbsent;
        case ToolCallReconciliationKind::TerminallyUnresolved:
            return ToolCallState::TerminallyUnresolved;
        }
        UF_UNREACHABLE_MSG("Unknown Tool call reconciliation kind");
    }

    auto toolCallVocabularyMaterial() -> std::string
    {
        auto stateRows = std::vector<json::Value>{};
        stateRows.reserve(k_toolCallStateNames.size());
        for (auto const& candidate : k_toolCallStateNames)
        {
            stateRows.emplace_back(json::Value::ofObject({
                {"carries_outcome",
                 json::Value::ofBoolean(toolCallStateHasOutcome(candidate.state))},
                {"name", json::Value::ofString(std::string{candidate.name})},
            }));
        }

        // Each conclusion is rendered by the state it writes rather than by an
        // enumerator name, because the enumerator is a C++ spelling and the
        // state is the protocol.
        auto completionRows = std::vector<json::Value>{};
        completionRows.reserve(k_toolCallCompletionKinds.size());
        for (auto const kind : k_toolCallCompletionKinds)
        {
            completionRows.emplace_back(json::Value::ofString(
                std::string{toolCallStateWireName(toolCallStateFor(kind))}
            ));
        }

        auto reconciliationRows = std::vector<json::Value>{};
        reconciliationRows.reserve(k_toolCallReconciliationKinds.size());
        for (auto const kind : k_toolCallReconciliationKinds)
        {
            reconciliationRows.emplace_back(json::Value::ofString(
                std::string{toolCallStateWireName(toolCallStateFor(kind))}
            ));
        }

        auto answererRows = std::vector<json::Value>{};
        answererRows.reserve(k_toolAnswerers.size());
        for (auto const& answerer : k_toolAnswerers)
        {
            answererRows.emplace_back(json::Value::ofObject({
                {"provider_kind",
                 json::Value::ofString(std::string{answerer.providerKind})},
                {"reaches_world_through",
                 json::Value::ofString(std::string{
                     answerer.composition == ToolEffectComposition::RecordedChildren
                         ? "recorded_children"
                         : "direct_leaf",
                 })},
            }));
        }

        return json::canonicalBytes(json::Value::ofObject({
            {"answerers", json::Value::ofArray(std::move(answererRows))},
            {"completion_states",
             json::Value::ofArray(std::move(completionRows))},
            {"reconciliation_states",
             json::Value::ofArray(std::move(reconciliationRows))},
            {"states", json::Value::ofArray(std::move(stateRows))},
        }));
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

    ToolCallReconciliation::ToolCallReconciliation(
        ToolCallReconciliationKind kind,
        CanonicalJson payload,
        CanonicalJson evidence
    )
        : m_kind{kind}
        , m_payload{std::move(payload)}
        , m_evidence{std::move(evidence)}
    {
    }

    auto ToolCallReconciliation::confirmed(
        CanonicalJson result,
        CanonicalJson evidence
    ) -> ToolCallReconciliation
    {
        return ToolCallReconciliation{
            ToolCallReconciliationKind::Confirmed,
            std::move(result),
            std::move(evidence),
        };
    }

    auto ToolCallReconciliation::provenAbsent(
        CanonicalJson explanation,
        CanonicalJson evidence
    ) -> ToolCallReconciliation
    {
        return ToolCallReconciliation{
            ToolCallReconciliationKind::ProvenAbsent,
            std::move(explanation),
            std::move(evidence),
        };
    }

    auto ToolCallReconciliation::terminallyUnresolved(
        CanonicalJson explanation,
        CanonicalJson evidence
    ) -> ToolCallReconciliation
    {
        return ToolCallReconciliation{
            ToolCallReconciliationKind::TerminallyUnresolved,
            std::move(explanation),
            std::move(evidence),
        };
    }

    auto ToolCallReconciliation::kind() const noexcept
        -> ToolCallReconciliationKind
    {
        return m_kind;
    }

    auto ToolCallReconciliation::payload() const noexcept
        -> CanonicalJson const&
    {
        return m_payload;
    }

    auto ToolCallReconciliation::evidence() const noexcept
        -> CanonicalJson const&
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

    ToolDelegationGrant::ToolDelegationGrant(
        std::string grantId,
        ContentHash rootIdentity,
        ContentHash parentCallIdentity,
        uint64 parentAttemptNumber,
        std::string executionPrincipalId
    )
        : m_grantId{std::move(grantId)}
        , m_rootIdentity{rootIdentity}
        , m_parentCallIdentity{parentCallIdentity}
        , m_parentAttemptNumber{parentAttemptNumber}
        , m_executionPrincipalId{std::move(executionPrincipalId)}
    {
    }

    auto ToolDelegationGrant::grantId() const noexcept -> std::string const&
    {
        return m_grantId;
    }

    auto ToolDelegationGrant::rootIdentity() const -> ContentHash
    {
        return m_rootIdentity;
    }

    auto ToolDelegationGrant::parentCallIdentity() const -> ContentHash
    {
        return m_parentCallIdentity;
    }

    auto ToolDelegationGrant::parentAttemptNumber() const noexcept -> uint64
    {
        return m_parentAttemptNumber;
    }

    auto ToolDelegationGrant::executionPrincipalId() const noexcept
        -> std::string const&
    {
        return m_executionPrincipalId;
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
