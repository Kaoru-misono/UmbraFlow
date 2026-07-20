#include "input.hpp"

#include <domain/error.hpp>

#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uf
{
    auto HeldInputs::identity(DeliveryTarget const& target) noexcept -> DeliveryIdentity
    {
        return DeliveryIdentity{
            .m_window = static_cast<std::uintptr_t>(target.windowHandle().value()),
            .m_sessionId = target.sessionId(),
            .m_generation = target.generation(),
        };
    }

    auto HeldInputs::describeIdentity(DeliveryIdentity const& identity) -> std::string
    {
        return std::format(
            "DeliveryIdentity {{ hwnd: {}, session_id: SessionId({}), generation: TargetGeneration({}) }}",
            identity.m_window,
            identity.m_sessionId.value(),
            identity.m_generation.value()
        );
    }

    auto HeldInputs::describeBinding(
        std::optional<DeliveryIdentity> const& binding
    ) -> std::string
    {
        if (!binding)
        {
            return "None";
        }
        return "Some(" + describeIdentity(*binding) + ")";
    }

    auto HeldInputs::ensureTarget(DeliveryTarget const& target) const -> Status
    {
        auto const current = identity(target);
        if (m_binding && *m_binding != current)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "held inputs belong to {}, not current target {}",
                    describeIdentity(*m_binding),
                    describeIdentity(current)
                )
            );
        }
        return ok();
    }

    auto HeldInputs::bind(DeliveryTarget const& target) -> Status
    {
        UF_TRY(ensureTarget(target));
        if (!m_binding)
        {
            m_binding = identity(target);
        }
        return ok();
    }

    auto HeldInputs::onKeyDown(
        DeliveryTarget const& target,
        KeyInput key
    ) -> Result<bool>
    {
        UF_TRY(bind(target));
        return m_keys.emplace(key).second;
    }

    auto HeldInputs::onKeyUp(
        DeliveryTarget const& target,
        KeyInput key
    ) -> Result<bool>
    {
        UF_TRY(ensureTarget(target));
        auto const removed = m_keys.erase(key) != 0U;
        clearBindingIfEmpty();
        return removed;
    }

    auto HeldInputs::onPointerDown(
        DeliveryTarget const& target,
        PointerButton button,
        ClientPixel pixel
    ) -> Status
    {
        UF_TRY(bind(target));
        m_pointers.insert_or_assign(button, pixel);
        return ok();
    }

    auto HeldInputs::onPointerUp(
        DeliveryTarget const& target,
        PointerButton button
    ) -> Result<bool>
    {
        UF_TRY(ensureTarget(target));
        auto const removed = m_pointers.erase(button) != 0U;
        clearBindingIfEmpty();
        return removed;
    }

    auto HeldInputs::empty() const noexcept -> bool
    {
        return m_keys.empty() && m_pointers.empty();
    }

    auto HeldInputs::size() const noexcept -> std::size_t
    {
        return m_keys.size() + m_pointers.size();
    }

    auto HeldInputs::holdsKey(KeyInput key) const -> bool
    {
        return m_keys.contains(key);
    }

    auto HeldInputs::holdsPointer(PointerButton button) const -> bool
    {
        return m_pointers.contains(button);
    }

    auto HeldInputs::releaseAll(
        DeliveryTarget const& target,
        std::move_only_function<Status(HeldInput)> postUp
    ) -> std::vector<ReleaseOutcome>
    {
        auto const binding = std::exchange(m_binding, std::nullopt);
        auto inputs = std::vector<HeldInput>{};
        inputs.reserve(size());
        for (auto const key : m_keys)
        {
            inputs.emplace_back(key);
        }
        for (auto const& [button, pixel] : m_pointers)
        {
            inputs.emplace_back(HeldPointerInput{button, pixel});
        }
        m_keys.clear();
        m_pointers.clear();

        if (inputs.empty())
        {
            return {};
        }

        auto outcomes = std::vector<ReleaseOutcome>{};
        outcomes.reserve(inputs.size());
        auto const current = identity(target);
        if (binding != std::optional{current})
        {
            for (auto const& input : inputs)
            {
                auto result = Status{
                    fail(
                        AutomationErrorKind::ActionRejected,
                        std::format(
                            "refusing to release input bound to {} through current target {}",
                            describeBinding(binding),
                            describeIdentity(current)
                        )
                    )
                };
                outcomes.emplace_back(ReleaseOutcome{input, std::move(result)});
            }
            return outcomes;
        }

        for (auto const& input : inputs)
        {
            auto result = postUp(input);
            outcomes.emplace_back(ReleaseOutcome{input, std::move(result)});
        }
        return outcomes;
    }

    auto HeldInputs::clearBindingIfEmpty() noexcept -> void
    {
        if (empty())
        {
            m_binding.reset();
        }
    }
}
