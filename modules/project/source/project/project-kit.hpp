#pragma once

#include "tool-catalog.hpp"

#include <core/error/result.hpp>

#include <filesystem>
#include <string>
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
    inline constexpr auto k_artifactManifestName = std::string_view{
        "project-kit.artifacts.json"
    };

    struct ProjectArtifactBlobSpec final
    {
        std::string           name{};
        std::filesystem::path sourceInput{};
    };

    struct ProjectRegistrationBuildSpec final
    {
        std::vector<std::string> artifactBlobNames{};
    };

    struct ProjectInitSpec final
    {
        std::filesystem::path              sourceDirectory{};
        std::filesystem::path              buildDirectory{};
        std::vector<std::filesystem::path> inputs{};
    };

    struct ProjectBuildSpec final
    {
        std::filesystem::path                sourceDirectory{};
        std::filesystem::path                buildDirectory{};
        std::vector<ToolCatalogDeclaration>  toolCatalogs{};
        std::vector<ProjectArtifactBlobSpec> artifactBlobs{};
        ProjectRegistrationBuildSpec         registration{};
    };

    struct ProjectFreezeSpec final
    {
        ProjectBuildSpec      candidate{};
        std::filesystem::path releaseRoot{};
    };

    [[nodiscard]]
    auto initProject(ProjectInitSpec const& spec) -> Status;

    [[nodiscard]]
    auto buildProject(ProjectBuildSpec const& spec) -> Status;

    [[nodiscard]]
    auto checkProject(ProjectBuildSpec const& spec) -> Status;

    [[nodiscard]]
    auto freezeProject(
        ProjectFreezeSpec const& spec
    ) -> Result<std::filesystem::path>;

    [[nodiscard]]
    auto loadProjectRelease(
        std::filesystem::path const& releaseDirectory
    ) -> Status;
}
