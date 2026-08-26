#include "evidence-store.hpp"

#include "platform/durable-file.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_evidenceDirectory = std::string_view{"evidence"};
        constexpr auto k_blobDirectory     = std::string_view{"blobs"};
        constexpr auto k_sha256Directory   = std::string_view{"sha256"};
        constexpr auto k_stagingDirectory  = std::string_view{".staging"};

        [[nodiscard]]
        auto evidenceFailure(
            AutomationErrorKind kind,
            std::string message,
            std::error_code error = {}
        ) -> std::unexpected<Error>
        {
            return fail(kind, std::move(message), error);
        }

        [[nodiscard]]
        auto requirePlainDirectory(
            std::filesystem::path const& path,
            std::string_view label
        ) -> Status
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(path, error);
            if (
                error
                || !std::filesystem::is_directory(status)
                || std::filesystem::is_symlink(status)
            )
            {
                return evidenceFailure(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} must be a plain directory", label),
                    error
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto ensureDirectory(
            std::filesystem::path const& path,
            std::string_view label
        ) -> Status
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(path, error);
            if (error)
            {
                return evidenceFailure(
                    AutomationErrorKind::IoFailure,
                    std::format("Could not create {}", label),
                    error
                );
            }
            return requirePlainDirectory(path, label);
        }

        [[nodiscard]] auto unixMillisNow() -> uint64
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            return elapsed <= 0 ? uint64{0U} : static_cast<uint64>(elapsed);
        }

        [[nodiscard]] auto stagingToken() -> std::string
        {
            auto random = std::random_device{};
            return std::format(
                "{:08x}{:08x}{:08x}{:08x}",
                random(),
                random(),
                random(),
                random()
            );
        }

        [[nodiscard]] auto hashNamed(std::string_view name) -> Result<ContentHash>
        {
            if (name.size() != 64U)
            {
                return evidenceFailure(
                    AutomationErrorKind::InvalidResource,
                    "Operator evidence store contains a blob with a malformed sha256 name"
                );
            }
            return ContentHash::parse(std::format("sha256:{}", name));
        }
    } // namespace

    EvidenceArtifactStore::EvidenceArtifactStore(
        std::filesystem::path blobRoot,
        std::filesystem::path stagingRoot
    )
        : m_blobRoot{std::move(blobRoot)}
        , m_stagingRoot{std::move(stagingRoot)}
    {
    }

    auto EvidenceArtifactStore::blobPath(ContentHash const& hash) const
        -> std::filesystem::path
    {
        auto const hex = hash.hex();
        return m_blobRoot / hex.substr(0U, 2U) / hex;
    }

    auto EvidenceArtifactStore::open(
        std::filesystem::path const& operatorRoot
    ) -> Result<EvidenceArtifactStore>
    {
        auto const root = operatorRoot / k_evidenceDirectory;
        auto const blobRoot = root / k_blobDirectory / k_sha256Directory;
        auto const stagingRoot = root / k_stagingDirectory;
        UF_TRY(ensureDirectory(root, "Operator evidence root"));
        UF_TRY(ensureDirectory(blobRoot, "Operator evidence sha256 blob root"));
        UF_TRY(ensureDirectory(stagingRoot, "Operator evidence staging root"));
        return EvidenceArtifactStore{blobRoot, stagingRoot};
    }

    auto EvidenceArtifactStore::publish(EvidenceArtifactSpec const& spec)
        -> Result<EvidenceArtifactReceipt>
    {
        if (spec.bytes.empty())
        {
            return evidenceFailure(
                AutomationErrorKind::InvalidResource,
                "Operator evidence artifact bytes must not be empty"
            );
        }
        if (spec.mediaType.empty() || spec.width == 0U || spec.height == 0U)
        {
            return evidenceFailure(
                AutomationErrorKind::InvalidResource,
                "Operator evidence artifact requires media type, width and height"
            );
        }
        UF_TRY_VALUE(contentHash, sha256(spec.bytes));
        auto const byteCount = checkedCast<uint64>(spec.bytes.size());
        if (!byteCount.has_value())
        {
            return evidenceFailure(
                AutomationErrorKind::InvalidResource,
                "Operator evidence artifact byte count exceeds uint64"
            );
        }

        auto const destination = blobPath(contentHash);
        UF_TRY(ensureDirectory(
            destination.parent_path(),
            "Operator evidence digest prefix"
        ));

        auto const staged = m_stagingRoot / stagingToken();
        UF_TRY(platform::writeDurableNewFile(staged, spec.bytes));
        auto cleanup = std::filesystem::path{staged};
        auto const removeStaging = [&cleanup]() noexcept
        {
            if (cleanup.empty())
            {
                return;
            }
            auto ignored = std::error_code{};
            std::filesystem::remove(cleanup, ignored);
        };

        auto published = platform::publishDurableNewFile(staged, destination);
        if (!published)
        {
            removeStaging();
            return std::unexpected{std::move(published).error()};
        }
        if (*published == platform::DurablePublishResult::Published)
        {
            cleanup.clear();
        }
        else
        {
            removeStaging();
        }

        UF_TRY_VALUE(verified, read(contentHash, spec.bytes.size()));
        if (verified.size() != spec.bytes.size())
        {
            return evidenceFailure(
                AutomationErrorKind::InvalidResource,
                "Published Operator evidence artifact byte count changed"
            );
        }
        return EvidenceArtifactReceipt{
            .contentHash         = contentHash,
            .byteCount           = *byteCount,
            .mediaType           = spec.mediaType,
            .width               = spec.width,
            .height              = spec.height,
            .frameIdentity       = spec.frameIdentity,
            .rectangle           = spec.rectangle,
            .createdAtUnixMillis = unixMillisNow(),
        };
    }

    auto EvidenceArtifactStore::read(
        ContentHash const& hash,
        std::size_t maximumBytes
    ) const -> Result<std::vector<std::byte>>
    {
        auto const path = blobPath(hash);
        auto error      = std::error_code{};
        auto const status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory)
        {
            return evidenceFailure(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "screenshot_sha256 {} is missing or expired: no committed "
                    "screenshot receipt names a retained evidence blob",
                    hash.hex()
                )
            );
        }
        if (
            error
            || !std::filesystem::is_regular_file(status)
            || std::filesystem::is_symlink(status)
        )
        {
            return evidenceFailure(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "Operator evidence artifact sha256:{} is not a plain file",
                    hash.hex()
                ),
                error
            );
        }
        auto const fileBytes = std::filesystem::file_size(path, error);
        if (error || fileBytes > maximumBytes)
        {
            return evidenceFailure(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "Operator evidence artifact sha256:{} exceeds the caller's byte ceiling of {}",
                    hash.hex(),
                    maximumBytes
                ),
                error
            );
        }

        auto stream = std::ifstream{path, std::ios::binary};
        if (!stream.good())
        {
            return evidenceFailure(
                AutomationErrorKind::IoFailure,
                std::format(
                    "Could not open Operator evidence artifact sha256:{}",
                    hash.hex()
                )
            );
        }
        auto const text = std::string{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{},
        };
        auto bytes = std::vector<std::byte>{};
        bytes.reserve(text.size());
        for (auto const value : text)
        {
            bytes.emplace_back(
                static_cast<std::byte>(static_cast<unsigned char>(value))
            );
        }
        UF_TRY_VALUE(actualHash, sha256(bytes));
        if (actualHash != hash)
        {
            return evidenceFailure(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "Operator evidence artifact sha256:{} failed content verification",
                    hash.hex()
                )
            );
        }
        return bytes;
    }

    auto EvidenceArtifactStore::reclaim(std::span<ContentHash const> retained)
        -> Result<ReclaimedEvidenceArtifacts>
    {
        auto retainedHex = std::vector<std::string>{};
        retainedHex.reserve(retained.size());
        for (auto const& hash : retained)
        {
            retainedHex.emplace_back(hash.hex());
        }
        std::ranges::sort(retainedHex);

        auto reclaimedBlobs = uint64{};
        for (auto const& prefix : std::filesystem::directory_iterator{m_blobRoot})
        {
            auto const prefixStatus = prefix.symlink_status();
            if (
                !prefix.is_directory()
                || std::filesystem::is_symlink(prefixStatus)
            )
            {
                return evidenceFailure(
                    AutomationErrorKind::InvalidResource,
                    "Operator evidence sha256 root contains a non-directory prefix"
                );
            }
            for (auto const& blob : std::filesystem::directory_iterator{prefix.path()})
            {
                auto const blobStatus = blob.symlink_status();
                if (
                    !blob.is_regular_file()
                    || std::filesystem::is_symlink(blobStatus)
                )
                {
                    return evidenceFailure(
                        AutomationErrorKind::InvalidResource,
                        "Operator evidence digest prefix contains a non-file blob"
                    );
                }
                auto const name = blob.path().filename().string();
                UF_TRY_VALUE(hash, hashNamed(name));
                if (blob.path().parent_path().filename().string() != name.substr(0U, 2U))
                {
                    return evidenceFailure(
                        AutomationErrorKind::InvalidResource,
                        "Operator evidence blob is filed under the wrong sha256 prefix"
                    );
                }
                if (std::ranges::binary_search(retainedHex, hash.hex()))
                {
                    continue;
                }
                auto error = std::error_code{};
                if (!std::filesystem::remove(blob.path(), error) || error)
                {
                    return evidenceFailure(
                        AutomationErrorKind::IoFailure,
                        "Could not remove unreferenced Operator evidence blob",
                        error
                    );
                }
                ++reclaimedBlobs;
            }
        }

        auto reclaimedStagings = uint64{};
        for (auto const& staging : std::filesystem::directory_iterator{m_stagingRoot})
        {
            auto error = std::error_code{};
            std::filesystem::remove_all(staging.path(), error);
            if (error)
            {
                return evidenceFailure(
                    AutomationErrorKind::IoFailure,
                    "Could not remove interrupted Operator evidence staging entry",
                    error
                );
            }
            ++reclaimedStagings;
        }
        return ReclaimedEvidenceArtifacts{
            .blobs    = reclaimedBlobs,
            .stagings = reclaimedStagings,
        };
    }

}
