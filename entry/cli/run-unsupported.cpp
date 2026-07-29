#include "run.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <task/task-host.hpp>

namespace uf::cli
{
    // The composition binds a live Windows target through the controller module,
    // which only builds on Windows. Every other host keeps the binary buildable
    // and reports the run path as unsupported rather than failing to link.
    auto runProduct(RunArgs const&) -> Result<task::TaskRunReport>
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "umbra-flow run is unsupported on this host"
        );
    }

    // No console cancellation handler is installed on hosts without the run
    // composition, so a stop can never have been requested here.
    auto runCancellationRequested() noexcept -> bool
    {
        return false;
    }
}
