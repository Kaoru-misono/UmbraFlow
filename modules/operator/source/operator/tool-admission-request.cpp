#include "tool-admission-request.hpp"

namespace uf::operator_runtime
{
    auto ToolAdmissionRequest::requiredMutability() const noexcept
        -> ToolMutability
    {
        return call.descriptor().mutability;
    }

    auto ToolAdmissionRequest::isRootPositioned() const -> bool
    {
        return call.parentIdentity() == root.identity();
    }
}
