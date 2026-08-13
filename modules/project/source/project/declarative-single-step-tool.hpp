#pragma once

#include <core/error/result.hpp>

#include <string>
#include <string_view>

namespace uf::project
{
    // These are the exact interface-lock v1.18 schema bytes. Publishing the
    // bytes, rather than a second C++ description of them, lets Project Kit
    // consumers pin and compare the same contract the validator applies.
    [[nodiscard]]
    auto declarativeSingleStepToolSchemaBytes() noexcept -> std::string_view;

    // Produces one ordinary ProjectPlugin module with the sole runtime SPI:
    // derive, plan, next_step, reconcile and reduce. The declaration grants no
    // script, closure, coordinate, fallback action or Host capability.
    [[nodiscard]]
    auto generateDeclarativeSingleStepAdapter(
        std::string_view pluginId,
        std::string_view declarationBytes
    ) -> Result<std::string>;
}
