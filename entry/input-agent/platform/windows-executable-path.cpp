#include "windows-executable-path.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <Windows.h>

#include <cstddef>
#include <format>
#include <string>

namespace uf::input_agent::platform
{
    namespace
    {
        // The Windows extended path limit. It bounds the growth loop below, so a
        // GetModuleFileNameW that kept reporting truncation could not spin.
        constexpr auto k_maximumExecutablePathLength = std::size_t{32768};
    }

    auto executableDirectory() -> Result<std::filesystem::path>
    {
        auto buffer = std::wstring(MAX_PATH, L'\0');
        while (true)
        {
            auto const capacity = checkedCast<uint32>(buffer.size());
            if (!capacity)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "the running executable's path buffer is too large to address"
                );
            }

            SetLastError(ERROR_SUCCESS);
            // SAFETY: buffer owns the writable storage this fills, and its exact
            // element count is passed as the bound the call must not exceed. The
            // contents are read only after the reported length is confirmed to
            // be strictly inside that bound, which is what distinguishes a
            // complete path from a truncated one.
            auto const length = GetModuleFileNameW(
                nullptr,
                buffer.data(),
                *capacity
            );
            if (length == 0U)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "GetModuleFileNameW failed with Win32 error {}",
                        GetLastError()
                    )
                );
            }
            if (length < *capacity)
            {
                buffer.resize(length);
                return std::filesystem::path{buffer}.parent_path();
            }
            if (buffer.size() >= k_maximumExecutablePathLength)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    "the running executable's path exceeds the Windows path limit"
                );
            }
            buffer.resize(buffer.size() * 2U);
        }
    }
}
