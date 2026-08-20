#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace uf::project
{
    enum class ProjectExitCode : uint8
    {
        Success = 0,
        Failure = 1,
    };

    // The two directories the `project` executable's composition wiring opens:
    // the source tree init may acquire into and the deployment declaration
    // names its files in, plus the build tree whose record build/check own.
    // Parsed under the action's own flag rules, so the executable does not
    // spell the command line a second time.
    struct ProjectDirectories final
    {
        std::filesystem::path sourceDirectory{};
        std::filesystem::path buildDirectory{};
    };

    [[nodiscard]]
    auto parseProjectDirectories(
        std::span<std::string const> raw,
        std::string_view action
    ) -> Result<ProjectDirectories>;

    [[nodiscard]]
    auto runProjectCommand(
        std::span<std::string const> raw
    ) -> ProjectExitCode;

    [[nodiscard]] auto projectUsageText() noexcept -> std::string_view;
}
