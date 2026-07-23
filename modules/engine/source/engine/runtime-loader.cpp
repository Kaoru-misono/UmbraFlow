#include "runtime-loader.hpp"

#include <core/error/result.hpp>

#include <annotation/content-hash.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/runtime-manifest.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::engine
{
    namespace
    {
        // Mirrors image::g_maximumPngFileBytes (64 MiB) from <image/png.hpp>.
        // image is a private dependency of annotation, so engine cannot include
        // that header without adding image to its own dependency set; the value
        // is duplicated here deliberately and must track the image module's
        // encoded-size quota. RecognitionRuntime::create decodes each template
        // under that same quota, so a template accepted by this read still faces
        // the authoritative bound.
        inline constexpr auto g_maximumTemplateFileBytes = std::size_t{64} * 1024U * 1024U;

        [[nodiscard]]
        auto ioFailure(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::IoFailure, std::move(message));
        }

        [[nodiscard]]
        auto oversizedResource(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // Reads a whole file, rejecting anything larger than a cap so a malformed
        // or hostile project cannot force an unbounded read. The cap decision
        // rests on the bytes the stream actually yields, not on a pre-read stat:
        // the stream is read to at most cap+1 bytes, and the presence of that
        // extra byte proves the file exceeds the cap even if it grew after any
        // stat (a time-of-check/time-of-use race). The stat survives only as a
        // fast-path early rejection that avoids opening an already-oversized file.
        [[nodiscard]]
        auto readCappedFile(
            std::filesystem::path const& path,
            std::size_t maximumBytes
        ) -> Result<std::string>
        {
            auto sizeError       = std::error_code{};
            auto const fileBytes = std::filesystem::file_size(path, sizeError);
            if (!sizeError && fileBytes > maximumBytes)
            {
                return oversizedResource(
                    std::format(
                        "'{}' is {} bytes, exceeding the {}-byte cap",
                        path.string(),
                        fileBytes,
                        maximumBytes
                    )
                );
            }

            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return ioFailure(std::format("cannot open '{}'", path.string()));
            }

            // Read one chunk at a time, growing the buffer only as far as the file
            // requires, and stop as soon as the accumulated size passes the cap.
            // A file within the cap is read in full; an oversized one is refused
            // after reading no more than cap plus a single trailing chunk.
            constexpr auto chunkBytes = std::size_t{64} * 1024U;
            auto contents             = std::string{};
            for (;;)
            {
                auto const oldSize = contents.size();
                contents.resize(oldSize + chunkBytes);
                stream.read(
                    contents.data() + oldSize,
                    static_cast<std::streamsize>(chunkBytes)
                );
                contents.resize(oldSize + static_cast<std::size_t>(stream.gcount()));

                if (stream.bad())
                {
                    return ioFailure(std::format("cannot read '{}'", path.string()));
                }
                if (contents.size() > maximumBytes)
                {
                    return oversizedResource(
                        std::format(
                            "'{}' exceeds the {}-byte cap",
                            path.string(),
                            maximumBytes
                        )
                    );
                }
                if (stream.eof())
                {
                    break;
                }
            }
            return contents;
        }
    }

    auto loadRuntimeProject(
        std::filesystem::path const& projectRoot
    ) -> Result<LoadedRuntime>
    {
        auto const manifestPath = projectRoot / "generated" / "annotations.runtime.toml";
        UF_TRY_VALUE(
            manifestToml,
            readCappedFile(manifestPath, g_maximumRuntimeManifestBytes)
        );
        UF_TRY_VALUE(manifest, annotation::parseRuntimeManifest(manifestToml));

        auto encodedTemplates = std::vector<annotation::EncodedRuntimeTemplate>{};
        auto loadedHashes     = std::vector<annotation::ContentHash>{};
        for (auto const& asset : manifest.assets())
        {
            if (std::ranges::contains(loadedHashes, asset.m_templateHash))
            {
                continue;
            }
            loadedHashes.emplace_back(asset.m_templateHash);

            auto const templatePath = projectRoot / asset.m_templatePath;
            UF_TRY_VALUE(
                templateText,
                readCappedFile(templatePath, g_maximumTemplateFileBytes)
            );
            auto const view = std::as_bytes(std::span{templateText});
            encodedTemplates.emplace_back(
                annotation::EncodedRuntimeTemplate{
                    .m_hash     = asset.m_templateHash,
                    .m_pngBytes = std::vector<std::byte>{view.begin(), view.end()},
                }
            );
        }

        UF_TRY_VALUE(
            runtime,
            annotation::RecognitionRuntime::create(
                std::move(manifest),
                std::move(encodedTemplates)
            )
        );
        return LoadedRuntime{.m_runtime = std::move(runtime)};
    }
}
