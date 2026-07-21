#pragma once

#include <core/error/result.hpp>

#include <filesystem>
#include <string_view>

namespace uf::m0_demo
{
    [[nodiscard]]
    auto canonicalizePathForComparison(
        std::filesystem::path const& path,
        std::string_view role
    ) -> Result<std::filesystem::path>;

    [[nodiscard]]
    auto canonicalizeOutputDirectory(
        std::filesystem::path const& path
    ) -> Result<std::filesystem::path>;

    [[nodiscard]]
    auto isPathWithinDirectory(
        std::filesystem::path const& canonicalPath,
        std::filesystem::path const& canonicalDirectory
    ) -> bool;

    [[nodiscard]]
    auto resolveConfinedOutputPath(
        std::filesystem::path const& canonicalOutputDirectory,
        std::filesystem::path const& output,
        std::string_view role
    ) -> Result<std::filesystem::path>;

    [[nodiscard]]
    auto canonicalPathsAlias(
        std::filesystem::path const& left,
        std::filesystem::path const& right
    ) -> Result<bool>;
}
