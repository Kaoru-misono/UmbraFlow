#pragma once

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

    struct ProjectDirectories final
    {
        std::filesystem::path sourceDirectory{};
        std::filesystem::path buildDirectory{};
    };

    [[nodiscard]]
    auto initProject(ProjectInitSpec const& spec) -> Status;

    [[nodiscard]]
    auto buildProject(ProjectDirectories const& directories) -> Status;

    [[nodiscard]]
    auto checkProject(ProjectDirectories const& directories) -> Status;
}
