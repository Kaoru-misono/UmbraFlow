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

    // The two directories the `project` executable's declared-file wiring
    // opens: the source tree the deployment declaration names its files in,
    // and the build tree whose record of them the build wrote. Parsed under
    // the same flag rules as the command itself, so the executable wires the
    // loader without spelling the command line a second time.
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
