#include "targets.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <vector>

namespace uf::cli
{
    // Enumerating the desktop goes through the controller module, which only
    // builds on Windows. Every other host keeps the binary buildable and reports
    // the listing as unsupported rather than failing to link, as `run` does.
    auto targetsProduct() -> Result<std::vector<TargetListing>>
    {
        return fail(
            AutomationErrorKind::UnsupportedCapability,
            "umbra-flow targets is unsupported on this host"
        );
    }
}
