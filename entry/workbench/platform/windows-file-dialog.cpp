#include "windows-file-dialog.hpp"

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#pragma warning(push, 0)
#include <Windows.h>

#include <commdlg.h>
#pragma warning(pop)

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace uf::workbench::platform
{
    namespace
    {
        // The Win32 filter is a run of null-terminated label/pattern pairs closed
        // by a final empty string. It is kept as a raw array rather than a
        // string_view, whose pointer constructor would stop at the first embedded
        // null and truncate the filter to its label.
        constexpr wchar_t k_pngFilter[] =
            L"PNG images (*.png)\0*.png\0All files (*.*)\0*.*\0";
        constexpr auto k_pathBufferLength = std::size_t{1024};
    }

    auto openPngFileDialog() -> Result<std::optional<std::filesystem::path>>
    {
        auto pathBuffer = std::array<wchar_t, k_pathBufferLength>{};

        auto dialog        = OPENFILENAMEW{};
        dialog.lStructSize = sizeof(OPENFILENAMEW);
        dialog.lpstrFilter = k_pngFilter;
        dialog.lpstrFile   = pathBuffer.data();
        dialog.nMaxFile    = static_cast<DWORD>(pathBuffer.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        // SAFETY: dialog and pathBuffer stay live across this synchronous call;
        // GetOpenFileNameW writes only into pathBuffer and retains no pointer.
        if (GetOpenFileNameW(&dialog) == FALSE)
        {
            auto const extendedError = CommDlgExtendedError();
            if (extendedError == 0U)
            {
                return std::optional<std::filesystem::path>{};
            }
            return fail(
                AutomationErrorKind::UnsupportedCapability,
                "workbench failed to open the PNG file dialog",
                systemErrorCode(static_cast<DWORD>(extendedError))
            );
        }

        return std::optional<std::filesystem::path>{
            std::filesystem::path{std::wstring{pathBuffer.data()}}
        };
    }
}
