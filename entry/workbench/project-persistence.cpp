#include "project-persistence.hpp"

#include "platform/windows-file-publication.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        auto invalidProject(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto ioFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error,
            std::source_location location = std::source_location::current()
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "workbench failed to {} {}: {}",
                    operation,
                    path.string(),
                    error.message()
                ),
                error,
                location
            );
        }

        [[nodiscard]]
        auto ensureDirectory(std::filesystem::path const& directory) -> Status
        {
            auto error = std::error_code{};
            static_cast<void>(std::filesystem::create_directories(directory, error));
            if (error)
            {
                return ioFailure("create directory", directory, error);
            }

            auto const isDirectory = std::filesystem::is_directory(directory, error);
            if (error)
            {
                return ioFailure("inspect directory", directory, error);
            }
            if (!isDirectory)
            {
                return invalidProject(
                    std::format(
                        "workbench output path {} is not a directory",
                        directory.string()
                    )
                );
            }
            return ok();
        }
    }

    auto saveAndGenerateAuthoringProject(
        std::filesystem::path const& projectRoot,
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets
    ) -> Status
    {
        if (projectRoot.empty())
        {
            return invalidProject("workbench project root must not be empty");
        }
        UF_TRY_VALUE(
            compiled,
            annotation::compileAuthoringDocument(document, sourceAssets)
        );

        auto filesystemError = std::error_code{};
        auto const absoluteRoot = std::filesystem::absolute(
            projectRoot,
            filesystemError
        ).lexically_normal();
        if (filesystemError)
        {
            return ioFailure(
                "resolve project root",
                projectRoot,
                filesystemError
            );
        }

        UF_TRY(ensureDirectory(absoluteRoot));
        UF_TRY(ensureDirectory(absoluteRoot / "assets" / "sources"));
        UF_TRY(ensureDirectory(absoluteRoot / "assets" / "templates"));
        UF_TRY(ensureDirectory(absoluteRoot / "generated"));

        auto assetOrder = std::vector<std::size_t>{};
        assetOrder.reserve(sourceAssets.size());
        for (auto index = std::size_t{0}; index < sourceAssets.size(); ++index)
        {
            assetOrder.emplace_back(index);
        }
        std::ranges::sort(
            assetOrder,
            {},
            [&sourceAssets](std::size_t index) -> annotation::ResourceId
            {
                return checkedAt(sourceAssets, index).m_id.value();
            }
        );

        auto const sources = document.sources();
        UF_CHECK(sources.size() == assetOrder.size());
        for (auto index = std::size_t{0}; index < sources.size(); ++index)
        {
            auto const& source = checkedAt(sources, index);
            auto const& asset = checkedAt(
                sourceAssets,
                checkedAt(assetOrder, index)
            );
            UF_CHECK(source.id() == asset.m_id);
            UF_TRY(
                platform::publishImmutableFile(
                    absoluteRoot / source.relativePath(),
                    asset.m_pngBytes
                )
            );
        }

        for (auto const& asset : compiled.m_templateAssets)
        {
            UF_TRY(
                platform::publishImmutableFile(
                    absoluteRoot / asset.m_relativePath,
                    asset.m_pngBytes
                )
            );
        }

        auto const authoringToml  = annotation::serializeAuthoringDocument(document);
        auto const authoringBytes = std::as_bytes(std::span{authoringToml});
        UF_TRY(
            platform::replaceFileAtomically(
                absoluteRoot / "annotations.toml",
                authoringBytes
            )
        );

        auto const runtimeBytes = std::as_bytes(
            std::span{compiled.m_runtimeManifestToml}
        );
        UF_TRY(
            platform::replaceFileAtomically(
                absoluteRoot / "generated" / "annotations.runtime.toml",
                runtimeBytes
            )
        );
        return ok();
    }
}
