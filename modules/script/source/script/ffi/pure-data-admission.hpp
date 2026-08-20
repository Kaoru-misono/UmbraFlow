#pragma once

#include <cstddef>

namespace uf::script::detail
{
    enum class ModuleBytecodeAdmission
    {
        Accepted,
        Empty,
        ModuleCeiling,
        ClosureCeiling,
    };

    [[nodiscard]]
    constexpr auto classifyModuleBytecode(
        std::size_t bytecodeBytes,
        std::size_t accumulatedBytes,
        std::size_t maximumModuleBytes,
        std::size_t maximumClosureBytes
    ) noexcept -> ModuleBytecodeAdmission
    {
        if (bytecodeBytes == 0U)
        {
            return ModuleBytecodeAdmission::Empty;
        }
        if (bytecodeBytes > maximumModuleBytes)
        {
            return ModuleBytecodeAdmission::ModuleCeiling;
        }
        if (
            accumulatedBytes > maximumClosureBytes
            || bytecodeBytes > maximumClosureBytes - accumulatedBytes
        )
        {
            return ModuleBytecodeAdmission::ClosureCeiling;
        }
        return ModuleBytecodeAdmission::Accepted;
    }
}
