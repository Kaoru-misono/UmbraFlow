#pragma once

#include "tool-catalog.hpp"

#include <core/error/result.hpp>

#include <filesystem>
#include <string_view>
#include <vector>

namespace uf::project
{
    inline constexpr auto k_inputManifestName = std::string_view{
        "project-kit.inputs"
    };
    inline constexpr auto k_buildReceiptName = std::string_view{
        "project-kit.build"
    };

    struct ProjectInitSpec final
    {
        std::filesystem::path              sourceDirectory{};
        std::filesystem::path              buildDirectory{};
        std::vector<std::filesystem::path> inputs{};
    };

    struct ProjectBuildSpec final
    {
        std::filesystem::path               sourceDirectory{};
        std::filesystem::path               buildDirectory{};
        std::vector<ToolCatalogDeclaration> toolCatalogs{};
    };

    [[nodiscard]]
    auto initProject(ProjectInitSpec const& spec) -> Status;

    [[nodiscard]]
    auto buildProject(ProjectBuildSpec const& spec) -> Status;

    [[nodiscard]]
    auto checkProject(ProjectBuildSpec const& spec) -> Status;
}
