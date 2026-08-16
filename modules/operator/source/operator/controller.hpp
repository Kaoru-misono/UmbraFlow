#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <array>
#include <string>
#include <string_view>

namespace uf::operator_runtime
{
    class OperatorCoordinator;

    // The three operators the system has. It is closed: the offline Annotation
    // Agent is not a fourth value, because it holds no production session,
    // lease or ledger row at all, and a controller that holds none of those has
    // nothing on this path to vary.
    enum class ControllerKind : uint8
    {
        Script,
        Agent,
        Human,
    };

    // The only per-kind variation the Operator recognises. Everything else --
    // the fingerprint material, the mutation chain, the fence, the epoch check,
    // the snapshot freshness rule, the state machine, the Journal -- is one
    // path for all three kinds. Approval and takeover eligibility are
    // deliberately absent: they are ControllerCapability entries, and
    // expressing them here as well would be a second spelling.
    struct ControllerProfile final
    {
        ControllerKind kind{ControllerKind::Agent};

        // An online Agent is the only controller whose intent the Operator
        // cannot verify before the fact, so it is offered and accepted only on
        // the surface where every argument is a name the project's model
        // already defines.
        bool semanticToolsOnly{true};

        // For the same reason, an online Agent is the only controller whose
        // stopping condition the Operator has to hold for it: a Script stops
        // when its program ends and a Human stops when the human does. The
        // ceilings come from the AgentProfile the session manifest pins, so
        // pinSession requires one for exactly the kinds this is true of and
        // refuses one for the kinds it is not.
        bool budgetsRequired{true};

        // A Script asserting "a human typed" would be fabricating evidence
        // about a third party, so only the human surface may record a finding.
        bool mayReportExternalInput{false};
    };

    // Dispatch is this table, never a chain of tests on the kind. It is indexed
    // by the enumerator's own value and controllerProfile checks that it is, so
    // a reordered or extended enum fails at the first lookup rather than
    // silently answering with the wrong row.
    inline constexpr auto k_controllerProfiles = std::array{
        ControllerProfile{ControllerKind::Script, false, false, false},
        ControllerProfile{ControllerKind::Agent, true, true, false},
        ControllerProfile{ControllerKind::Human, false, false, true},
    };

    [[nodiscard]]
    auto controllerProfile(ControllerKind kind) -> ControllerProfile;

    [[nodiscard]]
    auto controllerKindWireName(ControllerKind kind) noexcept -> std::string_view;

    [[nodiscard]]
    auto parseControllerKind(std::string_view value) -> Result<ControllerKind>;

    // An authenticated controller bound to one pinned session for one process
    // epoch. It is a value, not a handle: it stores no reference to the
    // coordinator, so it cannot outlive one and cannot be a stored borrow.
    // Every member is copied out of the pinned sessions row, so binding adds no
    // field a caller can choose -- least of all the kind, which is what the
    // whole p03 ceiling hangs from.
    //
    // Only OperatorCoordinator::bindController can mint one. A binding is
    // evidence of who the caller is at the moment it was minted and not a
    // capability: every entry point re-reads the session row and refuses a
    // binding whose epoch, kind or activity no longer matches.
    class ControllerBinding final
    {
        friend class OperatorCoordinator;

        std::string    m_sessionId;
        std::string    m_controllerId;
        std::string    m_controlledTargetId;
        ContentHash    m_capabilityProfileHash;
        uint64         m_sessionEpoch;
        ControllerKind m_kind;

        ControllerBinding(
            std::string sessionId,
            std::string controllerId,
            std::string controlledTargetId,
            ContentHash capabilityProfileHash,
            uint64 sessionEpoch,
            ControllerKind kind
        );

    public:
        [[nodiscard]]
        auto sessionId() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto controllerId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto controlledTargetId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto capabilityProfileHash() const -> ContentHash;
        [[nodiscard]] auto sessionEpoch() const noexcept -> uint64;
        [[nodiscard]] auto kind() const noexcept -> ControllerKind;
        [[nodiscard]] auto profile() const -> ControllerProfile;
    };
}
