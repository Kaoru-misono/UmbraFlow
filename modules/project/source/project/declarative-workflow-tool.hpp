#pragma once

#include <core/error/result.hpp>

#include <string>
#include <string_view>

namespace uf::project
{
    // The two closures one declared workflow tool is generated into: the
    // reducer module the pure program type is compiled from, and the tool
    // module the scoped program type is compiled from. A deployment needs
    // both, so the generator answers with both or with a refusal; there is no
    // spelling of "one closure was generated".
    struct DeclarativeWorkflowAdapter final
    {
        std::string reducerModule{};
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
