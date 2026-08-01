#include "runtime-project.hpp"

#include <core/error/result.hpp>

#include "content-hash.hpp"
#include "recognition-runtime.hpp"
#include "runtime-manifest.hpp"

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

namespace uf::annotation
{
    namespace
    {
        // Mirrors image::k_maximumPngFileBytes (64 MiB) from <image/png.hpp>.
        // image is a private dependency of this module, so a public header
        // cannot name that constant; the value is duplicated here deliberately
        // and must track the image module's encoded-size quota. Decoding runs
        // under that same quota, so a template accepted by this read still faces
        // the authoritative bound.
        inline constexpr auto k_maximumTemplateFileBytes = std::size_t{64} * 1024U * 1024U;

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
    ) -> Result<RecognitionRuntime>
    {
        auto const manifestPath = projectRoot / "generated" / "annotations.runtime.toml";
        UF_TRY_VALUE(
            manifestToml,
            readCappedFile(manifestPath, k_maximumRuntimeManifestBytes)
        );
        UF_TRY_VALUE(manifest, parseRuntimeManifest(manifestToml));

        auto encodedTemplates = std::vector<EncodedRuntimeTemplate>{};
        auto loadedHashes     = std::vector<ContentHash>{};
        for (auto const& asset : manifest.assets())
        {
            if (std::ranges::contains(loadedHashes, asset.templateHash))
            {
                continue;
            }
            loadedHashes.emplace_back(asset.templateHash);

            auto const templatePath = projectRoot / asset.templatePath;
            UF_TRY_VALUE(
                templateText,
                readCappedFile(templatePath, k_maximumTemplateFileBytes)
            );
            auto const view = std::as_bytes(std::span{templateText});
            encodedTemplates.emplace_back(
                EncodedRuntimeTemplate{
                    .hash     = asset.templateHash,
                    .pngBytes = std::vector<std::byte>{view.begin(), view.end()},
                }
            );
        }

        return RecognitionRuntime::create(
            std::move(manifest),
            std::move(encodedTemplates)
        );
    }
}
