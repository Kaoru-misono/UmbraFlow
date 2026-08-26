#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uf::operator_runtime
{
    inline constexpr auto k_screenshotSha256Member =
        std::string_view{"screenshot_sha256"};
    inline constexpr auto k_fileSha256Member =
        std::string_view{"file_sha256"};

    // The receipt committed into one Tool outcome after its blob is durable.
    // Hashes and frame identity have no default state, so every construction
    // site must name the exact bytes and capture this receipt describes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct EvidenceArtifactReceipt final
    {
        ContentHash              contentHash;
        uint64                   byteCount{};
        std::string              mediaType{};
        uint32                   width{};
        uint32                   height{};
        FrameIdentity            frameIdentity;
        std::optional<PixelRect> rectangle{};
        uint64                   createdAtUnixMillis{};
    };

    // Description consumed by the Operator-owned evidence store. The byte span
    // is a synchronous borrow and is never retained after publish returns.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct EvidenceArtifactSpec final
    {
        std::span<std::byte const> bytes{};
        std::string                mediaType{};
        uint32                     width{};
        uint32                     height{};
        FrameIdentity              frameIdentity;
        std::optional<PixelRect>   rectangle{};
    };

    struct ReclaimedEvidenceArtifacts final
    {
        uint64 blobs{};
        uint64 stagings{};
    };

    class EvidenceArtifactStore final
    {
        std::filesystem::path m_blobRoot;
        std::filesystem::path m_stagingRoot;

        EvidenceArtifactStore(
            std::filesystem::path blobRoot,
            std::filesystem::path stagingRoot
        );

        [[nodiscard]]
        auto blobPath(ContentHash const& hash) const -> std::filesystem::path;

    public:
        [[nodiscard]]
        static auto open(std::filesystem::path const& operatorRoot)
            -> Result<EvidenceArtifactStore>;

        [[nodiscard]]
        auto publish(EvidenceArtifactSpec const& spec)
            -> Result<EvidenceArtifactReceipt>;

        [[nodiscard]]
        auto read(ContentHash const& hash, std::size_t maximumBytes) const
            -> Result<std::vector<std::byte>>;

        [[nodiscard]]
        auto reclaim(std::span<ContentHash const> retained)
            -> Result<ReclaimedEvidenceArtifacts>;

    };
}
