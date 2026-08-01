#include "controller-action-sink.hpp"

#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/space.hpp>

#include <string>
#include <utility>

namespace uf::cli::platform
{
    auto ControllerActionSink::click(
        Point<ClientSpace> point,
        ObservationLease const& lease
    ) -> Status
    {
        auto delivered = uf::click(m_target, lease, point, m_held, m_audit);
        if (delivered)
        {
            return ok();
        }

        // The click failed after possibly leaving a pointer button held. Drain any
        // residual held input so the target is not stranded mid-press. HeldInputs
        // and AuditLog are cheap owned members, so this compensation is always
        // affordable here. The original click failure remains the reported error;
        // a compensation release that itself fails only adds context.
        auto error    = std::move(delivered).error();
        auto releases = releaseHeld(m_target, m_held, m_audit);
        for (auto const& release : releases)
        {
            if (!release.result)
            {
                error.addContext(
                    "input compensation after failed click also failed: "
                        + std::string{release.result.error().message()}
                );
            }
        }
        return std::unexpected{std::move(error)};
    }

    auto ControllerActionSink::pressKey(
        KeyName key,
        TargetGeneration actionGeneration
    ) -> Status
    {
        auto delivered = uf::keyPress(
            m_target,
            actionGeneration,
            KeyInput::fromKeyName(key),
            m_held,
            m_audit
        );
        if (delivered)
        {
            return ok();
        }

        // The press may have landed while the release did not, leaving the key held
        // down in the target. Drain any residual held input so the target is not
        // stranded mid-press -- the same compensation click() performs, and for the
        // same reason. The original failure remains the reported error; a compensation
        // release that itself fails only adds context.
        auto error    = std::move(delivered).error();
        auto releases = releaseHeld(m_target, m_held, m_audit);
        for (auto const& release : releases)
        {
            if (!release.result)
            {
                error.addContext(
                    "input compensation after a failed key also failed: "
                        + std::string{release.result.error().message()}
                );
            }
        }
        return std::unexpected{std::move(error)};
    }

    auto ControllerActionSink::scroll(
        int32 notches,
        ObservationLease const& lease
    ) -> Status
    {
        UF_TRY_VALUE(delta, WheelDelta::create(notches));

        // WHERE THE WHEEL IS AIMED, and why the choice is made here rather than
        // above. WM_MOUSEWHEEL carries a position the target hit-tests to decide
        // which control scrolls, so a position has to exist even though the verb
        // named none -- and only this layer knows the live client rectangle.
        //
        // The centre of the bound target's client area is the "no anchor was
        // named" answer: it addresses the window itself and no annotated region,
        // which leaves open question 5 of
        // docs/plans/2026-08-01-three-layers-and-agent-operator.md open instead of
        // quietly answering it with whichever position happened to be convenient.
        // It is always inside the rectangle, because DeliveryTarget::create
        // refuses to build an empty one.
        auto const centre = Point<ClientSpace>{
            static_cast<float>(m_target.clientWidth()) / 2.0F,
            static_cast<float>(m_target.clientHeight()) / 2.0F,
        };

        // No compensation drain follows a failure here, and none is owed: a wheel
        // is one posted message that holds nothing down, so there is no half-press
        // for a failed scroll to strand in the target.
        return uf::scroll(m_target, lease, centre, delta, m_held, m_audit);
    }
}
