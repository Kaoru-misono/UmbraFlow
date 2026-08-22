#pragma once

#include "project-plugin.hpp"

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace uf::operator_runtime
{
    class OperatorCoordinator;

    enum class ToolCallState : uint8
    {
        Proposed,
        Admitted,
        Dispatching,
        Confirmed,
        ProvenAbsent,
        Possible,
        Rejected,
        TerminalFailure,
        TerminallyUnresolved,
    };

    [[nodiscard]]
    auto toolCallStateWireName(ToolCallState state) noexcept -> std::string_view;

    [[nodiscard]]
    auto parseToolCallState(std::string_view value) -> Result<ToolCallState>;

    enum class ToolCallCompletionKind : uint8
    {
        Confirmed,
        ProvenAbsent,
        Possible,
        TerminalFailure,
    };

    // One provider conclusion, expressed as exact canonical bytes. The named
    // constructors are the only way to form it, so an absent result/evidence
    // combination cannot be stored under a terminal classification.
    class ToolCallCompletion final
    {
        ToolCallCompletionKind       m_kind;
        CanonicalJson                m_payload;
        std::optional<CanonicalJson> m_evidence;

        ToolCallCompletion(
            ToolCallCompletionKind kind,
            CanonicalJson payload,
            std::optional<CanonicalJson> evidence
        );

    public:
        [[nodiscard]]
        static auto confirmed(
            CanonicalJson result,
            std::optional<CanonicalJson> evidence = std::nullopt
        ) -> ToolCallCompletion;

        [[nodiscard]]
        static auto provenAbsent(
            CanonicalJson explanation,
            CanonicalJson evidence
        ) -> ToolCallCompletion;

        [[nodiscard]]
        static auto possible(
            CanonicalJson explanation,
            std::optional<CanonicalJson> evidence = std::nullopt
        ) -> ToolCallCompletion;

        [[nodiscard]]
        static auto terminalFailure(
            CanonicalJson error,
            std::optional<CanonicalJson> evidence = std::nullopt
        ) -> ToolCallCompletion;

        [[nodiscard]] auto kind() const noexcept -> ToolCallCompletionKind;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;

        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND
            -> std::optional<CanonicalJson> const&;
    };

    enum class ToolCallReconciliationKind : uint8
    {
        Confirmed,
        ProvenAbsent,
        TerminallyUnresolved,
    };

    // A Framework-owned conclusion over a previously possible mutating call.
    // Evidence is mandatory for every classification: reconciliation is the
    // act of replacing uncertainty with a claim about the target, and without
    // fresh evidence no such claim has been earned. TerminallyUnresolved ends
    // this run but intentionally leaves the target-wide mutation barrier set.
    class ToolCallReconciliation final
    {
        ToolCallReconciliationKind m_kind;
        CanonicalJson              m_payload;
        CanonicalJson              m_evidence;

        ToolCallReconciliation(
            ToolCallReconciliationKind kind,
            CanonicalJson payload,
            CanonicalJson evidence
        );

    public:
        [[nodiscard]]
        static auto confirmed(CanonicalJson result, CanonicalJson evidence)
            -> ToolCallReconciliation;

        [[nodiscard]]
        static auto provenAbsent(
            CanonicalJson explanation,
            CanonicalJson evidence
        ) -> ToolCallReconciliation;

        [[nodiscard]]
        static auto terminallyUnresolved(
            CanonicalJson explanation,
            CanonicalJson evidence
        ) -> ToolCallReconciliation;

        [[nodiscard]]
        auto kind() const noexcept -> ToolCallReconciliationKind;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;

        [[nodiscard]]
        auto evidence() const noexcept UF_LIFETIME_BOUND -> CanonicalJson const&;
    };

    // An unforgeable handle to one durable admission row. Only the
    // Coordinator can mint it after re-reading live authority.
    class ToolCallAdmission final
    {
        friend class OperatorCoordinator;

        ContentHash m_callIdentity;
        uint64      m_attemptNumber;
        uint64      m_historyRevision;

        ToolCallAdmission(
            ContentHash callIdentity,
            uint64 attemptNumber,
            uint64 historyRevision
        );

    public:
        [[nodiscard]] auto callIdentity() const -> ContentHash;
        [[nodiscard]] auto attemptNumber() const noexcept -> uint64;
        [[nodiscard]] auto historyRevision() const noexcept -> uint64;
    };

    // The token returned only after dispatching is durable. Provider code must
    // hold this token before it executes, then return it with its conclusion.
    class ToolCallDispatch final
    {
        friend class OperatorCoordinator;

        ContentHash m_callIdentity;
        uint64      m_attemptNumber;
        uint64      m_historyRevision;

        ToolCallDispatch(
            ContentHash callIdentity,
            uint64 attemptNumber,
            uint64 historyRevision
        );

    public:
        [[nodiscard]] auto callIdentity() const -> ContentHash;
        [[nodiscard]] auto attemptNumber() const noexcept -> uint64;
        [[nodiscard]] auto historyRevision() const noexcept -> uint64;
    };

    // The authority one live handler invocation holds to issue child calls.
    //
    // It is unforgeable for the reason ToolCallAdmission is: only the
    // Coordinator can mint one, and only over a call whose durable state is
    // already dispatching. That is what makes "a child call belongs to a
    // handler that is actually running" a fact about the ledger rather than a
    // claim a caller makes, and it is why a call arriving under a parent that
    // is not dispatching is refused as a changed parent.
    //
    // The grant deliberately carries no effect bound of its own. What the
    // parent may delegate is the parent descriptor's registered child-effect
    // declaration, which the grant row records at issue time; re-stating it
    // here would be a second copy of one catalog statement.
    class ToolDelegationGrant final
    {
        friend class OperatorCoordinator;

        std::string m_grantId;
        ContentHash m_rootIdentity;
        ContentHash m_parentCallIdentity;
        uint64      m_parentAttemptNumber;

        // The principal that executes the children, recorded separately from
        // the run's origin actor. Section 3.3 requires the handler execution
        // principal never to substitute its own profile for the origin's
        // admitted objective, so the two travel as two values and are written
        // to two column pairs.
        std::string m_executionPrincipalId;

        ToolDelegationGrant(
            std::string grantId,
            ContentHash rootIdentity,
            ContentHash parentCallIdentity,
            uint64 parentAttemptNumber,
            std::string executionPrincipalId
        );

    public:
        [[nodiscard]]
        auto grantId() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]] auto rootIdentity() const -> ContentHash;
        [[nodiscard]] auto parentCallIdentity() const -> ContentHash;
        [[nodiscard]] auto parentAttemptNumber() const noexcept -> uint64;

        [[nodiscard]]
        auto executionPrincipalId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;
    };

    enum class ToolOutcomeLookup : uint8
    {
        Created,
        Existing,
    };

    struct StoredToolCallOutcome final
    {
        ToolCallState     state{ToolCallState::TerminalFailure};
        ToolOutcomeLookup lookup{ToolOutcomeLookup::Created};
        uint64            revision{};
    };

    struct ToolCallReplay final
    {
        ToolCallState                state{ToolCallState::Proposed};
        uint64                       revision{};
        uint64                       activeAdmissionAttempt{};
        std::optional<CanonicalJson> payload{};
        std::optional<CanonicalJson> evidence{};
    };
}
