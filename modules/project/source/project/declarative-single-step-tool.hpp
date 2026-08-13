#pragma once

#include <core/error/result.hpp>

#include <string>
#include <string_view>

namespace uf::project
{
    // Produces one ordinary ProjectPlugin module with the sole runtime SPI:
    // derive, plan, next_step, reconcile and reduce. The declaration grants no
    // script, closure, coordinate, fallback action or Host capability.
    [[nodiscard]]
    auto generateDeclarativeSingleStepAdapter(
        std::string_view pluginId,
        std::string_view declarationBytes
    ) -> Result<std::string>;
}
