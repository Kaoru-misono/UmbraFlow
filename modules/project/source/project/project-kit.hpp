#pragma once

#include "tool-catalog.hpp"

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <json/value.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
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

    // The one document a project directory holds at its root, and the one
    // statement of its shape. The kit cannot link the runtime loader --
    // uf::deployment reaches uf::task, and the `project` executable links
    // uf::project and uf::core alone -- so the shape lives in neither of
    // them: it is published under schema/ and reaches both through the
    // framework schema catalog this module already embeds. There is no
    // second, narrower reading of this document here.
    inline constexpr auto k_projectManifestName = std::string_view{
        "umbraflow-project.json"
    };
    inline constexpr auto k_projectSchemaPath = std::string_view{
        "schema/umbraflow-project-v2.schema.json"
    };

    // The bytes behind one content hash, obtained however the caller obtains
    // them. Naming a source by content is what lets a project declare a
    // template cut without referencing a screenshot path, and that only holds
    // while this stays the caller's job: the kit hands out a hash and takes
    // bytes back, so it never learns what a directory, a store or a corpus is.
    // The `project` command line is the caller in production and reads a
    // directory named by --frames-root; a test is the caller in tests and reads
    // its own memory. Neither shape reaches this module.
    //
    // The resolver does not have to verify what it returns. generatedTemplates
    // re-hashes every answer and refuses one that does not hash to what it
    // asked for, so a store whose file names lie is caught in one place instead
    // of in each caller.
    using TemplateSourceResolver = std::function<
        Result<std::vector<std::byte>>(ContentHash const&)
    >;

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

    struct ProjectFreezeSpec final
    {
        ProjectBuildSpec      candidate{};
        std::filesystem::path releaseRoot{};
    };

    // The project's root document, read unconditionally and judged by the one
    // published statement of its shape. The kit's own build reads it again
    // through this function for template_cuts; the `project` command line
    // reads it for the deployment declarations the kit cannot see. Every
    // reader compiles the same published schema bytes out of the framework
    // schema catalog, so no reader can accept a document another refuses --
    // and no caller is allowed to pull members out of the document before
    // this function has let the schema judge them.
    [[nodiscard]]
    auto readProjectRootDocument(
        std::filesystem::path const& sourceDirectory
    ) -> Result<json::Value>;

    [[nodiscard]]
    auto initProject(ProjectInitSpec const& spec) -> Status;

    [[nodiscard]]
    auto buildProject(
        ProjectBuildSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Status;

    [[nodiscard]]
    auto checkProject(
        ProjectBuildSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Status;

    [[nodiscard]]
    auto freezeProject(
        ProjectFreezeSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Result<std::filesystem::path>;

    [[nodiscard]]
    auto loadProjectRelease(
        std::filesystem::path const& releaseDirectory
    ) -> Status;
}
