#include "explore.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <task/task-host.hpp>

namespace uf::cli
{
    // The composition binds a live Windows target through the controller module,
    // which only builds on Windows. Other hosts fail before opening resources.
    auto exploreProduct(ExploreArgs const&) -> Result<task::TaskRunReport>
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "umbra-flow explore is unsupported on this host"
        );
    }

    auto exploreCancellationRequested() noexcept -> bool
    {
        return false;
    }
}
