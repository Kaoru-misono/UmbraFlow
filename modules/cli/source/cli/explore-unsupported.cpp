#include "explore.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <task/task-host.hpp>

namespace uf::cli
{
    // The composition binds a live Windows target through the controller module,
    // which only builds on Windows. Other hosts fail before opening resources.
    //
    // exploreProject above is deliberately not split, on observeProject's terms:
    // it takes its ports as arguments, so every host compiles the production
    // door itself and only the binding of a live desktop is missing here.
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
