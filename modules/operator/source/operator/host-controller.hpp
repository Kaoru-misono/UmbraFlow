#pragma once

#include "ledger.hpp"

#include <task/task-host.hpp>

#include <core/error/result.hpp>

#include <memory>
#include <string>
#include <variant>

namespace uf::operator_runtime
{
    // The production join between the Operator ledger and Host delivery for
    // one controlled target. Lease acquire/release/takeover and a complete
    // Tool-call input delivery share m_targetSerialization inside the
    // implementation, so a displaced fence cannot enter a reservation after
    // takeover returns or move between a successful reservation and
    // TaskHost::deliver.
    //
    // A flat hold keeps the serialization token until it releases. Its one
    // optional capture/observation is internal to the same Tool call, so no
    // child call needs to enter while the target is pressed.
    class OperatorTaskHost final
    {
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        explicit OperatorTaskHost(std::unique_ptr<Impl> implementation);

        [[nodiscard]]
        auto requireControlledTarget(std::string const& controlledTargetId) const
            -> Status;

    public:
        class ToolCallHold final
        {
            friend class OperatorTaskHost;

            struct Impl;
            std::unique_ptr<Impl> m_impl;

            explicit ToolCallHold(std::unique_ptr<Impl> implementation);

        public:
            ToolCallHold(ToolCallHold const&) = delete;
            ToolCallHold(ToolCallHold&&) noexcept;
            auto operator=(ToolCallHold const&) -> ToolCallHold& = delete;
            auto operator=(ToolCallHold&&) noexcept -> ToolCallHold&;
            ~ToolCallHold();

            [[nodiscard]] auto duration() const noexcept
                -> MonotonicInstant::Duration;
        };

        using ToolCallHoldStart =
            std::variant<ToolCallHold, task::HostDeliveryReport>;

        // What one Tool-call-native input asks the Host to deliver: the model
        // target the call's own resolved observation named, and the UI action
        // it asks for on that target.
        //
        // The two travel as one value because they are one statement and are
        // read together at every boundary they cross -- the ledger pins the
        // target into the authority, the Host judges both against the model it
        // parsed, and the resolver authorizes the action on the Binding the
        // target resolved.
        struct ToolCallInputIntent final
        {
            std::string uiTarget{};
            std::string binding{};
            std::string action{};
            std::string expectedKind{};
        };

        [[nodiscard]]
        static auto create(
            OperatorCoordinator coordinator,
            std::string controlledTargetId
        ) -> Result<OperatorTaskHost>;

        OperatorTaskHost(OperatorTaskHost&&) noexcept;
        auto operator=(OperatorTaskHost&&) noexcept -> OperatorTaskHost&;
        OperatorTaskHost(OperatorTaskHost const&) = delete;
        auto operator=(OperatorTaskHost const&) -> OperatorTaskHost& = delete;
        ~OperatorTaskHost();

        [[nodiscard]]
        auto controlledTargetId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        // Non-control Coordinator work remains available through the authority
        // that owns it. Production control changes and delivery use the joined
        // operations below; direct Coordinator control calls are test/setup
        // surface and do not update this Host's fence.
        [[nodiscard]] auto coordinator() noexcept -> OperatorCoordinator&;
        [[nodiscard]] auto host() noexcept -> task::TaskHost&;

        [[nodiscard]]
        auto acquireLease(
            ControllerBinding const& controller
        ) -> Result<ControlLease>;

        [[nodiscard]]
        auto releaseLease(ControlLease const& lease) -> Status;

        [[nodiscard]]
        auto takeoverLease(
            ControllerBinding const& controller,
            std::string const& reason
        ) -> Result<ControlTakeover>;

        // The one mint of Host delivery authority. It shares
        // m_targetSerialization with lease acquire, release and takeover,
        // because a fence displaced between the reservation and the Host call
        // would let a Host act under authority the ledger has already
        // superseded.
        //
        // It returns the report alone. There is no ledger write to pair with
        // it here, because the outcome a Tool call records is written by
        // completeToolCallDispatch under the dispatch token the executor holds,
        // and the classification that report earns is
        // toolCallCompletionFor's -- never the caller's.
        [[nodiscard]]
        auto deliverToolCallInput(
            ToolCallPositionIdentity const& call,
            ControlLease const& lease,
            GenerationId runtimeGeneration,
            ToolCallInputIntent const& intent,
            task::TaskContext& context
        ) -> Result<task::HostDeliveryReport>;

        [[nodiscard]]
        auto engageToolCallHold(
            ToolCallPositionIdentity const& call,
            ControlLease const& lease,
            GenerationId runtimeGeneration,
            ToolCallInputIntent const& intent,
            task::TaskContext& context
        ) -> Result<ToolCallHoldStart>;

        [[nodiscard]]
        auto finishToolCallHold(
            ToolCallHold hold,
            task::TaskContext& context
        ) -> task::HostDeliveryReport;
    };
}
