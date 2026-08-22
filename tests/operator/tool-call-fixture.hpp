#pragma once

#include <operator/tool-invocation.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <utility>

namespace uf::operator_runtime::test_support
{
    // One position at a chosen ordinal, reached the only way a position can be
    // reached: by walking an issuing context to it. The seam assigns the child
    // index, so a case that wants the third call of a context issues three,
    // exactly as the run it stands for would.
    //
    // parent is an optional non-owning observation of the handler call whose
    // context issues this one, and nullptr for a root-positioned call -- one
    // the run's own context issues under the root request itself. Nothing here
    // retains it.
    [[nodiscard]]
    inline auto toolCallAt(
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const* parent,
        uint32 ordinal,
        ToolExecutionIdentity const& executionIdentity,
        ValidatedToolInvocation const& invocation
    ) -> Result<ToolCallPositionIdentity>
    {
        REQUIRE(ordinal > 0U);
        auto context = parent != nullptr
            ? ToolCallIssuingContext::forHandler(*parent)
            : ToolCallIssuingContext::forRoot(root, executionIdentity);
        auto position = context.issue(invocation);
        for (auto index = uint32{1}; index < ordinal; ++index)
        {
            position = context.issue(invocation);
        }
        return position;
    }
}
