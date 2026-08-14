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
        std::optional<DeliveredInput> posted
    )
        : m_authority{std::move(authority)}
        , m_outcome{outcome}
        , m_reason{std::move(reason)}
        , m_receiptId{receiptId}
        , m_posted{posted}
    {
        // The schema admits no fourth shape: delivered carries an input receipt
        // and no reason, and the other two carry a reason and no receipt.
        // Checked here rather than at each producing site, because there is
        // exactly one producer and this is the value the ledger reads.
        auto const isDelivered = m_outcome == DeliveryOutcome::Delivered;
        UF_CHECK(isDelivered == m_reason.empty());
        UF_CHECK(isDelivered == m_posted.has_value());
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
}
