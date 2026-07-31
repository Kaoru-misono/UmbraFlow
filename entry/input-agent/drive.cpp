#include "drive.hpp"

#include "error-text.hpp"
#include "target-setup.hpp"

#include <controller/capture.hpp>
#include <controller/detail/input-revalidation.hpp>
#include <controller/discovery.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <domain/detection.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::input_agent
{
    namespace
    {
        // The freshness and coordinate fences a pointer action passes before the
        // target is revalidated, so a point the caller got wrong is answered as
        // a rejected action rather than as a target that vanished. It hands back
        // the lease the post then runs under, so the observation is proved fresh
        // once rather than once per fence.
        [[nodiscard]]
        auto openPointerDelivery(
            DeliveryTarget const& delivery,
            Frame const& observation,
            Point<ClientSpace> point
        ) -> Result<ObservationLease>
        {
            UF_TRY_VALUE(
                lease,
                ObservationLease::forFrame(
                    observation,
                    k_defaultMaxActionFrameAge
                )
            );
            UF_TRY(
                validateInputAgentPointerAction(
                    delivery,
                    lease,
                    point,
                    MonotonicInstant::now()
                )
            );
            return lease;
        }

        auto addReleaseFailures(
            std::string_view operation,
            Error& error,
            std::vector<ReleaseOutcome> const& releases
        ) -> void
        {
            for (auto const& release : releases)
            {
                if (!release.result)
                {
                    error.addContext(
                        std::format(
                            "input-agent {} compensation failed: {}",
                            operation,
                            formatAutomationError(release.result.error())
                        )
                    );
                }
            }
        }
    }

    auto clearInputAgentCommandAudit(AuditLog& audit) noexcept -> void
    {
        audit = AuditLog{};
    }

    auto validateInputAgentPointerAction(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        MonotonicInstant now
    ) -> Status
    {
        auto validated = controller_detail::checkPointerPreconditions(
            lease,
            target.sessionId(),
            target.generation(),
            now,
            point,
            target.clientWidth(),
            target.clientHeight()
        );
        if (!validated)
        {
            return std::unexpected{std::move(validated).error()};
        }
        return ok();
    }

    auto DriveOutcome::delivered() const noexcept -> bool
    {
        return !error.has_value();
    }

    WindowInputAgentDrive::WindowInputAgentDrive(
        ResolvedTarget resolved,
        WgcCaptureSession session,
        DeliveryTarget delivery,
        ClientSize client
    )
        : m_resolved{std::move(resolved)}
        , m_session{std::move(session)}
        , m_delivery{delivery}
        , m_client{client}
    {
    }

    auto WindowInputAgentDrive::requireSameTarget() -> Status
    {
        UF_TRY_VALUE(revalidated, m_resolved.revalidate());
        UF_TRY(requireUnchangedTarget(revalidated));
        return m_session.validateTargetInstance();
    }

    auto WindowInputAgentDrive::compensate(
        std::string_view operation,
        Error failure
    ) -> DriveOutcome
    {
        auto error                = std::move(failure);
        auto cleanupTarget        = m_delivery;
        auto instanceAfterFailure = m_session.validateTargetInstance();
        auto const targetReplaced = !instanceAfterFailure;
        if (!instanceAfterFailure)
        {
            // Posting to a replaced target would reach a window that is no
            // longer the observed one, so the release is aimed at a deliberately
            // rejected identity: HeldInputs then drops the holds and no message
            // leaves the process.
            error.addContext(
                std::format(
                    "input-agent {} compensation blocked because the capture target instance changed: {}",
                    operation,
                    formatAutomationError(instanceAfterFailure.error())
                )
            );
            auto rejectedTarget = DeliveryTarget::create(
                m_delivery.windowHandle(),
                CaptureSessionId{~m_delivery.sessionId().value()},
                m_delivery.generation(),
                m_delivery.clientWidth(),
                m_delivery.clientHeight()
            );
            UF_CHECK(rejectedTarget.has_value());
            cleanupTarget = *rejectedTarget;
        }
        auto const releases = releaseHeld(cleanupTarget, m_held, m_audit);
        addReleaseFailures(operation, error, releases);
        return DriveOutcome{
            .error          = std::move(error),
            .targetReplaced = targetReplaced,
        };
    }

    auto WindowInputAgentDrive::clientSize() const noexcept -> ClientSize
    {
        return m_client;
    }

    auto WindowInputAgentDrive::capture() -> Result<Frame>
    {
        return m_session.capture();
    }

    auto WindowInputAgentDrive::click(
        Frame const& observation,
        Point<ClientSpace> point
    ) -> DriveOutcome
    {
        auto lease = openPointerDelivery(m_delivery, observation, point);
        if (!lease)
        {
            return DriveOutcome{.error = std::move(lease).error()};
        }

        auto current = requireSameTarget();
        if (!current)
        {
            return DriveOutcome{
                .error          = std::move(current).error(),
                .targetReplaced = true,
            };
        }

        auto clicked = uf::click(
            m_delivery,
            *lease,
            point,
            m_held,
            m_audit
        );
        if (!clicked)
        {
            return compensate("click", std::move(clicked).error());
        }
        return DriveOutcome{};
    }

    auto WindowInputAgentDrive::scroll(
        Frame const& observation,
        Point<ClientSpace> point,
        WheelDelta delta
    ) -> DriveOutcome
    {
        auto lease = openPointerDelivery(m_delivery, observation, point);
        if (!lease)
        {
            return DriveOutcome{.error = std::move(lease).error()};
        }

        auto current = requireSameTarget();
        if (!current)
        {
            return DriveOutcome{
                .error          = std::move(current).error(),
                .targetReplaced = true,
            };
        }

        auto scrolled = uf::scroll(
            m_delivery,
            *lease,
            point,
            delta,
            m_held,
            m_audit
        );
        if (!scrolled)
        {
            return compensate("scroll", std::move(scrolled).error());
        }
        return DriveOutcome{};
    }

    auto WindowInputAgentDrive::key(
        Frame const& observation,
        KeyInput key
    ) -> DriveOutcome
    {
        // No lease freshness fence here. A keystroke names no position, so an
        // older observation cannot make it land in the wrong place; the only
        // staleness that matters is the target having been replaced, which the
        // generation carried below and requireSameTarget cover.
        auto current = requireSameTarget();
        if (!current)
        {
            return DriveOutcome{
                .error          = std::move(current).error(),
                .targetReplaced = true,
            };
        }

        // keyPress posts WM_KEYDOWN then WM_KEYUP with nothing in between, which
        // is the same zero hold click uses. A hold between DOWN and UP is
        // exactly what made a hand-rolled pointer sequence read as a drag and
        // activate nothing against this target
        // (docs/pitfalls/capture-and-target-selection.md), so no wait belongs
        // inside the keystroke; the settle the layer above applies waits after
        // it.
        auto pressed = keyPress(
            m_delivery,
            observation.targetGeneration(),
            key,
            m_held,
            m_audit
        );
        if (!pressed)
        {
            return compensate("key", std::move(pressed).error());
        }
        return DriveOutcome{};
    }

    auto WindowInputAgentDrive::clearAudit() noexcept -> void
    {
        clearInputAgentCommandAudit(m_audit);
    }

    auto WindowInputAgentDrive::close() -> Status
    {
        return m_session.close();
    }
}
