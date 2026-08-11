#include "candidate-selection.hpp"

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <format>

namespace uf::cli
{
    auto selectCandidate(
        std::span<TargetCandidate const> candidates,
        WindowHandle handle
    ) -> Result<TargetCandidate>
    {
        auto const printable = static_cast<uintptr>(handle.value());
        auto const found     = std::ranges::find_if(
            candidates,
            [handle](TargetCandidate const& candidate)
            {
                return candidate.handle() == handle;
            }
        );

        if (found == candidates.end())
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "no window on this desktop has handle {:#x}; a handle dies "
                    "with its window, so re-read it with `umbra-flow targets`",
                    printable
                )
            );
        }

        // Left in the enumeration by the desktop, and excluded here rather than
        // earlier so the operator is told the window is a decoy instead of being
        // told the handle does not exist.
        if (!found->isVisible())
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "window {:#x} (\"{}\") is not visible and cannot be captured",
                    printable,
                    found->title()
                )
            );
        }

        if (found->isIconic())
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "window {:#x} (\"{}\") is minimized, and a minimized window "
                    "composites no frames at all",
                    printable,
                    found->title()
                )
            );
        }

        return *found;
    }
}
