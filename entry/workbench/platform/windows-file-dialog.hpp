#pragma once

#include <core/error/result.hpp>

#include <filesystem>
#include <optional>

namespace uf::workbench::platform
{
    // Opens the operating-system file picker filtered to PNG images and returns
    // the chosen path, or nullopt when the user cancels. Fails only when the
    // dialog itself reports an error. The call is modal and borrows no state.
    [[nodiscard]]
    auto openPngFileDialog() -> Result<std::optional<std::filesystem::path>>;
}
