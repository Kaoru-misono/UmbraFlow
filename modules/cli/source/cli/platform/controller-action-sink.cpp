#include "controller-action-sink.hpp"

#include <controller/discovery.hpp>
#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/space.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace uf::cli::platform
{
    auto ControllerActionSink::releaseHeldInputs() -> Status
    {
        auto releases = releaseHeld(m_target, m_held, m_audit);
        for (auto& release : releases)
        {
            if (!release.result)
            {
                return std::unexpected{std::move(release.result).error()};
            }
        }
        return ok();
    }

    auto ControllerActionSink::refreshTargetCallback(std::string_view what)
        -> std::move_only_function<Result<DeliveryTarget>()>
    {
        // The verb name is COPIED rather than borrowed: the callback outlives this
        // call by design -- it is handed to a controller verb that invokes it after
        // a pause -- and a stored string_view would be a borrow with no stated
        // owner.
        return [this, what = std::string{what}]() -> Result<DeliveryTarget>
        {
            UF_TRY_VALUE(candidates, enumerateCandidates());
            auto const found = std::ranges::find_if(
                candidates,
                [this](TargetCandidate const& candidate)
                {
                    return candidate.handle() == m_target.windowHandle();
                }
            );
            if (found == candidates.end())
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    std::format(
                        "the {} target {:#x} is gone from the desktop",
                        what,
                        static_cast<uintptr>(m_target.windowHandle().value())
                    )
                );
            }

            auto const client = found->clientSize();
            return DeliveryTarget::create(
                m_target.windowHandle(),
                m_target.sessionId(),
                m_target.generation(),
                client.width(),
                client.height()
            );
        };
    }

    auto ControllerActionSink::click(
        Point<ClientSpace> point,
        ObservationLease const& lease
    ) -> Status
    {
        return uf::click(m_target, lease, point, m_held, m_audit);
    }

    auto ControllerActionSink::pressKey(
        KeyName key,
        TargetGeneration actionGeneration
    ) -> Status
    {
        return uf::keyPress(
            m_target,
            actionGeneration,
            KeyInput::fromKeyName(key),
            m_held,
            m_audit
        );
    }

    auto ControllerActionSink::scroll(
        int32 notches,
        ObservationLease const& lease
    ) -> Status
    {
        UF_TRY_VALUE(delta, WheelDelta::create(notches));

        // WM_MOUSEWHEEL carries a position the target hit-tests to decide which
        // control scrolls, so a position has to exist even though the verb named
        // none, and only this layer knows the live client rectangle. The centre is
        // the "no anchor was named" answer -- it addresses the window itself and no
        // annotated region, leaving open question 5 of
        // docs/archive/plans/2026-08-01-three-layers-and-agent-operator.md open -- and it is
        // always inside the rectangle, since DeliveryTarget::create refuses an empty
        // one.
        auto const centre = Point<ClientSpace>{
            static_cast<float>(m_target.clientWidth()) / 2.0F,
            static_cast<float>(m_target.clientHeight()) / 2.0F,
        };

        return uf::scroll(m_target, lease, centre, delta, m_held, m_audit);
    }

    auto ControllerActionSink::hold(
        Point<ClientSpace> point,
        MonotonicInstant::Duration duration,
        ObservationLease const& lease
    ) -> Status
    {
        // controller::hold asks for the delivery target again after the hold and
        // refuses to post the release if its identity moved. This composition holds a
        // snapshot and re-resolves nothing, so that comparison is a no-op here until
        // a composition root re-resolves a target mid-run -- the seam the callback
        // exists for. What it does do here is FAIL: the live enumeration is re-read
        // across the hold, so a window gone by the time the button should come up is
        // reported rather than posted to.
        auto refreshTarget = refreshTargetCallback("hold");

        // A hold can leave a button that WENT down and did not come up,
        // since the refresh across the hold can refuse the release. Putting it
        // back up is releaseHeldInputs's, which the engine calls after this
        // returns however it returned.
        return uf::hold(
            m_target,
            lease,
            point,
            duration,
            m_held,
            m_audit,
            std::move(refreshTarget)
        );
    }

    auto ControllerActionSink::movePointer(
        Point<ClientSpace> point,
        ObservationLease const& lease
    ) -> Status
    {
        // controller::movePointer reads the held inputs to decide whether the
        // message is a plain move or a drag; nothing this port exposes leaves a
        // button held ACROSS deliveries, because the engine releases what one
        // left held before the next begins, so the plain move is what it picks
        // here. The held moves inside drag() are the other branch.
        return uf::movePointer(m_target, lease, point, m_held, m_audit);
    }

    auto ControllerActionSink::drag(
        Point<ClientSpace> start,
        Point<ClientSpace> end,
        MonotonicInstant::Duration travel,
        ObservationLease const& lease
    ) -> Status
    {
        // The hold's clause, and the reason it matters more here: a drag
        // can fail at any of its held moves as well as at the refresh, so "the
        // button went down and did not come up" is its ordinary failure rather
        // than its unlucky one. It is still not this verb's to compensate for.
        return uf::drag(
            m_target,
            lease,
            start,
            end,
            travel,
            m_held,
            m_audit,
            refreshTargetCallback("drag")
        );
    }
}
