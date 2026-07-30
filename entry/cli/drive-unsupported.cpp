#include "drive.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <task/task-host.hpp>

namespace uf::cli
{
    // The composition binds a live Windows target through the controller module,
    // which only builds on Windows. Every other host keeps the binary buildable and
    // reports the drive path as unsupported rather than failing to link, exactly as
    // the run path does.
    auto driveProduct(DriveArgs const&) -> Result<task::TaskRunReport>
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "umbra-flow drive is unsupported on this host"
        );
    }
}
