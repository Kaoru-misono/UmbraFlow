#pragma once

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>

#include <engine/session.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace uf::task
{
    class TaskHost;

    // The control fence one ledger currently holds over one target. Plain data:
    // raising a Host's fence only ever withdraws authority, so a forged value
    // costs its forger and nobody else, and a lower one is refused outright by
    // the monotonicity rule at TaskHost::adoptControlFence.
    struct ControlFence final
    {
        std::string controlledTargetId{};
        uint64      sessionEpoch{};
        uint64      fencingToken{};
    };

    // GenerationId and ContentHash have no default state, which is why the two
    // members below carry no in-class initializer and every construction site
    // supplies them. Proved rather than asserted: if either type ever gains a
    // default, the suppression stops standing for a real absence.
    static_assert(!std::is_default_constructible_v<GenerationId>);
    static_assert(!std::is_default_constructible_v<ContentHash>);

    // One dispatch the ledger reserved, as the ledger recorded it. The Host
    // checks the four fields it can know -- target, runtime generation, epoch
    // and fence -- and carries the rest back untouched so the ledger can
    // recognise its own reservation. Nothing here is proof; the proof is that a
    // HostDeliveryReport exists at all, which is why this stays an aggregate
    // anyone can build.
    //
    // A forged authority is not harmless, though. One naming the fence a Host
    // currently holds makes that Host act, for as long as a Receipt minted
    // under the same fence is pending -- and a takeover refreshes no Host's
    // fence, so the displaced one keeps both. What forbids it is TaskHost::
    // deliver being private, not the shape of this type. A production delivery
    // path must therefore mint every authority from
    // OperatorCoordinator::reserveDispatch, which refuses a lease a takeover
    // has superseded, and accept none from a caller.
    //
    // The two generations are deliberately separate quantities under separate
    // names. runtimeGeneration is the Host's own GenerationId, so the Host can
    // and does compare it; targetGeneration is the domain TargetGeneration the
    // ledger records, which the Host cannot see and therefore only carries.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct DispatchAuthority final
    {
        std::string      controlledTargetId{};
        std::string      leaseId{};
        std::string      operationId{};
        std::string      authorityDecisionId{};
        ContentHash      frozenPlanHash;
        GenerationId     runtimeGeneration;
        TargetGeneration targetGeneration{};
        uint64           sessionEpoch{};
        uint64           fencingToken{};
        uint64           dispatchSequence{};
    };

    enum class DeliveryOutcome : uint8
    {
        // The Host consumed the authorization and posted nothing. The only
        // value that proves an external effect ABSENT.
        NotDelivered,
        Delivered,
        // The Host called into the delivery path and cannot say whether input
        // reached the target. EngineSession::clickPoint fails before the sink,
        // at the sink, and after the click has already landed, and its Result
        // cannot separate the three, so every click-path error is this and
        // never NotDelivered. Under-claiming is the safe direction: only
        // NotDelivered unlocks a Rejected disposition in reconciliation.
        //
        // The keystroke path is the same claim for its own reasons, and it is
        // not weaker. EngineSession::pressKey refuses before the sink on a
        // requested stop, a foreign or spent observation and a target instance
        // that no longer matches; it can fail at the sink; and it can fail
        // after the key has already gone down, because
        // ControllerActionSink::pressKey drains a press whose release did not
        // land. One Result covers all three there too, so every key-path error
        // is TransportUnknown as well -- a keystroke that reached the target is
        // exactly as unprovable-absent as a click that did.
        TransportUnknown,
    };

    // What one delivery posted, as the engine described it. A sum type because
    // a Receipt authorizes one input of one kind and the two carry different
    // evidence: an ActReceipt names the client-space point, a KeyReceipt names
    // the key and no point, which is the whole reason engine keeps them apart
    // rather than inventing a coordinate for a keystroke.
    using DeliveredInput = std::variant<engine::ActReceipt, engine::KeyReceipt>;

    // What one TaskHost::deliver did with one Receipt. Constructible only by
    // TaskHost, so a ledger that demands one cannot be told about a delivery
    // that never ran. The test harness is deliberately not a friend: it reaches
    // TaskHost's privates and can therefore call deliver, but a harness able to
    // fabricate what deliver returns would make every test over this value
    // unfalsifiable. Copyable because the Operator stores it by value.
    class HostDeliveryReport final
    {
        friend class TaskHost;

        DispatchAuthority             m_authority;
        DeliveryOutcome               m_outcome;
        std::string                   m_reason;
        uint64                        m_receiptId;
        std::optional<DeliveredInput> m_posted;

        HostDeliveryReport(
            DispatchAuthority authority,
            DeliveryOutcome outcome,
            std::string reason,
            uint64 receiptId,
            std::optional<DeliveredInput> posted
        );

    public:
        HostDeliveryReport(HostDeliveryReport const&) = default;
        HostDeliveryReport(HostDeliveryReport&&) noexcept = default;
        auto operator=(HostDeliveryReport const&) -> HostDeliveryReport& = default;
        auto operator=(HostDeliveryReport&&) noexcept -> HostDeliveryReport& = default;
        ~HostDeliveryReport() = default;

        // The reservation exactly as it was presented. The ledger matches every
        // field against its own rows, so a report carried across a takeover is
        // refused by the rows rather than by anything the Host remembers.
        [[nodiscard]]
        auto authority() const noexcept UF_LIFETIME_BOUND -> DispatchAuthority const&;

        [[nodiscard]] auto outcome() const noexcept -> DeliveryOutcome;

        // Empty exactly when outcome() is Delivered, which is the schema's
        // DeliveryOutcome rule: reason is required for not_delivered and
        // transport_unknown and forbidden for delivered.
        [[nodiscard]] auto reason() const noexcept UF_LIFETIME_BOUND -> std::string_view;

        // The opaque ordinal of the one Receipt this report consumed. It names
        // Host-private storage and is meaningful only as an identity.
        [[nodiscard]] auto receiptId() const noexcept -> uint64;

        // Engaged exactly when outcome() is Delivered, and then holding the
        // alternative matching the kind of action the Receipt authorized.
        //
        // TODO(cpp-debt): nothing reads this back today -- ceiling: the value
        // is unfalsifiable while it has no reader, upgrade: the reconciliation
        // path that will join a delivered input to its trace line.
        [[nodiscard]]
        auto posted() const noexcept UF_LIFETIME_BOUND
            -> std::optional<DeliveredInput> const&;
    };
}
