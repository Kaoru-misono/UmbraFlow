#pragma once

#include <core/error/result.hpp>

#include <string>
#include <string_view>

namespace uf::project
{
    // The closure one declared workflow tool is generated into: the tool
    // module the scoped program type is compiled from. A deployment ships one
    // closure, so the generator answers with one or with a refusal.
    struct DeclarativeWorkflowAdapter final
    {
        std::string toolModule{};
    };

    // Produces a declared workflow tool's two ordinary Project modules. The
    // declaration grants no script, closure, coordinate or Host capability.
    [[nodiscard]]
    auto generateDeclarativeWorkflowAdapter(
        std::string_view pluginId,
        std::string_view declarationBytes
    ) -> Result<DeclarativeWorkflowAdapter>;
}
