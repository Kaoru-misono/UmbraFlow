#include "project-persistence.hpp"

#include "platform/windows-file-publication.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>

#include <annotation/content-hash.hpp>

#include <domain/error.hpp>

#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
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

        constexpr auto k_maximumAuthoringTomlBytes = std::size_t{16} * 1024U * 1024U;

        // Reads a whole file as raw bytes held in a std::string, rejecting a
        // file larger than maximumBytes and a file that does not exist. The
        // string channel avoids reinterpreting the storage; callers view it as
        // text or convert it to bytes.
        [[nodiscard]]
        auto readCappedFile(
            std::filesystem::path const& path,
            std::size_t maximumBytes,
            std::string_view resourceKind
        ) -> Result<std::string>
        {
            auto error         = std::error_code{};
            auto const present = std::filesystem::is_regular_file(path, error);
            // A missing file is reported through the return value, but some
            // platforms also set error to no_such_file_or_directory; that is the
            // "missing" outcome below, not a genuine IO failure to surface here.
            if (error && error != std::errc::no_such_file_or_directory)
            {
                return ioFailure("inspect", path, error);
            }
            if (!present)
            {
                return invalidProject(
                    std::format(
                        "workbench {} {} is missing",
                        resourceKind,
                        path.string()
                    )
                );
            }

            auto const rawSize = std::filesystem::file_size(path, error);
            if (error)
            {
                return ioFailure("measure", path, error);
            }
            auto const size       = checkedCast<std::size_t>(rawSize);
            auto const streamSize = checkedCast<std::streamsize>(rawSize);
            if (!size || !streamSize || *size > maximumBytes)
            {
                return invalidProject(
                    std::format(
                        "workbench {} {} exceeds its {}-byte cap",
                        resourceKind,
                        path.string(),
                        maximumBytes
                    )
                );
            }

            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return ioFailure(
                    "open",
                    path,
                    std::make_error_code(std::io_errc::stream)
                );
            }

            auto contents = std::string(*size, '\0');
            if (!contents.empty())
            {
                stream.read(contents.data(), *streamSize);
                if (!stream.good())
                {
                    return ioFailure(
                        "read",
                        path,
                        std::make_error_code(std::io_errc::stream)
                    );
                }
            }
            return contents;
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

    auto loadAuthoringProject(
        std::filesystem::path const& projectRoot
    ) -> Result<LoadedAuthoringProject>
    {
        if (projectRoot.empty())
        {
            return invalidProject("workbench project root must not be empty");
        }

        auto filesystemError    = std::error_code{};
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

        UF_TRY_VALUE(
            authoringToml,
            readCappedFile(
                absoluteRoot / "annotations.toml",
                k_maximumAuthoringTomlBytes,
                "annotations document"
            )
        );
        UF_TRY_VALUE(
            document,
            annotation::parseAuthoringDocument(authoringToml)
        );

        auto const sources = document.sources();
        auto sourceAssets  = std::vector<annotation::AuthoringSourceAsset>{};
        sourceAssets.reserve(sources.size());
        for (auto const& source : sources)
        {
            UF_TRY_VALUE(
                pngText,
                readCappedFile(
                    absoluteRoot / source.relativePath(),
                    image::k_maximumPngFileBytes,
                    "source image"
                )
            );
            auto const pngView = std::as_bytes(std::span{pngText});
            auto pngBytes      = std::vector<std::byte>{
                pngView.begin(),
                pngView.end()
            };

            UF_TRY_VALUE_CONTEXT(
                actualHash,
                annotation::sha256(pngBytes),
                std::format("hashing workbench source {}", source.relativePath())
            );
            if (actualHash != source.contentHash())
            {
                return invalidProject(
                    std::format(
                        "workbench source {} content hash does not match its document record",
                        source.relativePath()
                    )
                );
            }

            UF_TRY_VALUE_CONTEXT(
                decoded,
                image::decodePng(pngBytes, source.relativePath()),
                std::format("decoding workbench source {}", source.relativePath())
            );
            auto const fingerprint = source.fingerprint();
            if (
                decoded.m_width != fingerprint.width()
                || decoded.m_height != fingerprint.height()
            )
            {
                return invalidProject(
                    std::format(
                        "workbench source {} decoded as {}x{}, expected {}x{}",
                        source.relativePath(),
                        decoded.m_width,
                        decoded.m_height,
                        fingerprint.width(),
                        fingerprint.height()
                    )
                );
            }

            sourceAssets.emplace_back(
                annotation::AuthoringSourceAsset{
                    .m_id       = source.id(),
                    .m_pngBytes = std::move(pngBytes),
                }
            );
        }

        return LoadedAuthoringProject{
            .m_document = std::move(document),
            .m_sources  = std::move(sourceAssets),
        };
    }
}
