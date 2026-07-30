#include "controller-action-sink.hpp"

#include <controller/input.hpp>

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
}
