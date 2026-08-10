#include "host-delivery.hpp"

#include <core/error/contracts.hpp>

#include <utility>

namespace uf::task
{
    HostDeliveryReport::HostDeliveryReport(
        DispatchAuthority authority,
        DeliveryOutcome outcome,
        std::string reason,
        uint64 receiptId,
        std::optional<engine::ActReceipt> act
    )
        : m_authority{std::move(authority)}
        , m_outcome{outcome}
        , m_reason{std::move(reason)}
        , m_receiptId{receiptId}
        , m_act{act}
    {
        // The schema admits no fourth shape: delivered carries an act and no
        // reason, and the other two carry a reason and no act. Checked here
        // rather than at each producing site, because there is exactly one
        // producer and this is the value the ledger reads.
        auto const delivered = m_outcome == DeliveryOutcome::Delivered;
        UF_CHECK(delivered == m_reason.empty());
        UF_CHECK(delivered == m_act.has_value());
    }

    auto HostDeliveryReport::authority() const noexcept -> DispatchAuthority const&
    {
        return m_authority;
    }

    auto HostDeliveryReport::outcome() const noexcept -> DeliveryOutcome
    {
        return m_outcome;
    }

    auto HostDeliveryReport::reason() const noexcept -> std::string_view
    {
        return m_reason;
    }

    auto HostDeliveryReport::receiptId() const noexcept -> uint64
    {
        return m_receiptId;
    }

    auto HostDeliveryReport::act() const noexcept
        -> std::optional<engine::ActReceipt> const&
    {
        return m_act;
    }
}
