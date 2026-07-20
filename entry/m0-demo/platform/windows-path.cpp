#include "windows-path.hpp"

#include <Windows.h>

namespace uf::m0_demo::platform
{
    auto pathsEqualOrdinal(
        std::filesystem::path const& left,
        std::filesystem::path const& right
    ) noexcept -> bool
    {
        auto const& leftNative = left.native();
        auto const& rightNative = right.native();
        // SAFETY: both native path strings own null-terminated buffers for this
        // synchronous comparison. CompareStringOrdinal reads them only until
        // the terminator and retains neither pointer.
        return CompareStringOrdinal(
            leftNative.c_str(),
            -1,
            rightNative.c_str(),
            -1,
            TRUE
        ) == CSTR_EQUAL;
    }
}
