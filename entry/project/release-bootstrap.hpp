#pragma once

#include <core/error/result.hpp>

#include <filesystem>

namespace uf::project_entry
{
    [[nodiscard]]
    auto prepareReleaseBundle(
        std::filesystem::path const& sourceDirectory
    ) -> Status;
}
