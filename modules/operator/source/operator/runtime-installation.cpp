#include "runtime-installation.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/text/utf8.hpp>

#include <task/platform/confined-file.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::operator_runtime::detail
{
    namespace
    {
        constexpr auto k_releaseManifestName = std::string_view{"release.manifest.json"};
        constexpr auto k_runtimeDirectoryName = std::string_view{"runtime-artifact"};
        constexpr auto k_maximumReleaseManifestBytes = std::size_t{64U} * 1024U;
        constexpr auto k_maximumExactJsonInteger = uint64{9'007'199'254'740'991U};

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto ioFailure(
            std::string_view action,
            std::filesystem::path const& path,
            std::error_code error = {}
        ) -> std::unexpected<Error>
        {
            auto message = std::format(
                "cannot {} production RuntimeArtifact path {}",
                action,
                path.string()
            );
            if (error)
            {
                message += std::format(": {}", error.message());
            }
            return fail(AutomationErrorKind::IoFailure, std::move(message));
        }

        [[nodiscard]]
        auto isLowerHex(char value) noexcept -> bool
        {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        }

        class ReleaseReader final
        {
            std::string_view m_source;
            std::size_t      m_offset{};

            [[nodiscard]]
            auto failure(std::string_view expected) const -> std::unexpected<Error>
            {
                return refuse(
                    std::format(
                        "release manifest is not exact canonical JSON at byte {}: expected {}",
                        m_offset,
                        expected
                    )
                );
            }

        public:
            explicit ReleaseReader(std::string_view source) noexcept
                : m_source{source}
            {
            }

            [[nodiscard]] auto atEnd() const noexcept -> bool
            {
                return m_offset == m_source.size();
            }

            [[nodiscard]] auto startsWith(std::string_view literal) const noexcept -> bool
            {
                return m_source.substr(m_offset).starts_with(literal);
            }

            [[nodiscard]] auto consume(std::string_view literal) -> Status
            {
                if (!m_source.substr(m_offset).starts_with(literal))
                {
                    return failure(literal);
                }
                m_offset += literal.size();
                return ok();
            }

            [[nodiscard]] auto hash() -> Result<ContentHash>
            {
                UF_TRY(consume("\""));
                if (m_source.size() - m_offset < 65U)
                {
                    return failure("a lowercase SHA-256 string");
                }
                auto const text = m_source.substr(m_offset, 64U);
                if (!std::ranges::all_of(text, isLowerHex))
                {
                    return failure("a lowercase SHA-256 string");
                }
                m_offset += text.size();
                UF_TRY(consume("\""));
                auto encoded = std::string{"sha256:"};
                encoded += text;
                return ContentHash::parse(encoded);
            }

            [[nodiscard]] auto unsignedInteger() -> Result<uint64>
            {
                auto const first = m_offset;
                while (
                    m_offset < m_source.size()
                    && m_source[m_offset] >= '0'
                    && m_source[m_offset] <= '9'
                )
                {
                    ++m_offset;
                }
                auto const digits = m_source.substr(first, m_offset - first);
                if (
                    digits.empty()
                    || (digits.size() > 1U && digits.front() == '0')
                )
                {
                    return failure("a canonical positive integer");
                }
                auto value = uint64{};
                auto const* const begin = std::to_address(digits.begin());
                auto const* const end   = std::to_address(digits.end());
                auto const parsed       = std::from_chars(begin, end, value);
                if (
                    parsed.ec != std::errc{}
                    || parsed.ptr != end
                    || value == 0U
                    || value > k_maximumExactJsonInteger
                )
                {
                    return failure("a positive I-JSON exact integer");
                }
                return value;
            }

            [[nodiscard]] auto nonEmptyString() -> Status
            {
                UF_TRY(consume("\""));
                auto decoded = std::string{};
                while (m_offset < m_source.size() && m_source[m_offset] != '"')
                {
                    auto const next = m_source[m_offset++];
                    auto const byte = static_cast<unsigned char>(next);
                    if (byte < 0x20U)
                    {
                        return failure("a canonical JSON string byte");
                    }
                    if (next != '\\')
                    {
                        decoded.push_back(next);
                        continue;
                    }
                    if (m_offset == m_source.size())
                    {
                        return failure("a canonical JSON escape");
                    }
                    auto const escape = m_source[m_offset++];
                    switch (escape)
                    {
                    case '"': decoded.push_back('"'); break;
                    case '\\': decoded.push_back('\\'); break;
                    case 'b': decoded.push_back('\b'); break;
                    case 't': decoded.push_back('\t'); break;
                    case 'n': decoded.push_back('\n'); break;
                    case 'f': decoded.push_back('\f'); break;
                    case 'r': decoded.push_back('\r'); break;
                    case 'u':
                    {
                        if (
                            m_source.size() - m_offset < 4U
                            || m_source.substr(m_offset, 2U) != "00"
                            || !isLowerHex(m_source[m_offset + 2U])
                            || !isLowerHex(m_source[m_offset + 3U])
                        )
                        {
                            return failure("a canonical control-character escape");
                        }
                        auto const digits = m_source.substr(m_offset + 2U, 2U);
                        auto value = 0U;
                        auto const* const begin = std::to_address(digits.begin());
                        auto const* const end   = std::to_address(digits.end());
                        auto const parsed = std::from_chars(begin, end, value, 16);
                        if (
                            parsed.ec != std::errc{}
                            || value >= 0x20U
                            || value == 0x08U
                            || value == 0x09U
                            || value == 0x0AU
                            || value == 0x0CU
                            || value == 0x0DU
                        )
                        {
                            return failure("the shortest canonical control-character escape");
                        }
                        decoded.push_back(static_cast<char>(value));
                        m_offset += 4U;
                        break;
                    }
                    default: return failure("a canonical JSON escape");
                    }
                }
                UF_TRY(consume("\""));
                if (decoded.empty() || !isValidUtf8(decoded))
                {
                    return failure("a non-empty Unicode scalar string");
                }
                return ok();
            }
        };

        struct ReleaseManifest final
        {
            ContentHash artifactRootHash;
        };

        [[nodiscard]]
        auto parseReleaseManifest(std::string_view source) -> Result<ReleaseManifest>
        {
            auto reader = ReleaseReader{source};
            UF_TRY(reader.consume("{\"annotation_workspace_schema_hash\":"));
            UF_TRY_VALUE(annotationSchemaHash, reader.hash());
            auto encodedSchemaHash = std::string{"sha256:"};
            encodedSchemaHash += k_annotationWorkspaceSchemaHash;
            UF_TRY_VALUE(expectedAnnotationSchemaHash, ContentHash::parse(encodedSchemaHash));
            if (annotationSchemaHash != expectedAnnotationSchemaHash)
            {
                return refuse("release manifest uses an unsupported annotation schema");
            }
            UF_TRY(reader.consume(",\"candidate_id\":"));
            UF_TRY(reader.nonEmptyString());
            UF_TRY(reader.consume(",\"candidate_revision\":"));
            UF_TRY(reader.unsignedInteger());
            UF_TRY(reader.consume(",\"generation\":"));
            UF_TRY(reader.unsignedInteger());
            UF_TRY(reader.consume(",\"predecessor_publication_id\":"));
            // The predecessor is authoring provenance, not production CAS
            // authority. Its exact value is nevertheless schema-validated.
            if (reader.startsWith("null"))
            {
                UF_TRY(reader.consume("null"));
            }
            else
            {
                UF_TRY(reader.hash());
            }
            UF_TRY(reader.consume(",\"replay_gate_hash\":"));
            UF_TRY(reader.hash());
            UF_TRY(reader.consume(",\"runtime_artifact_root_hash\":"));
            UF_TRY_VALUE(artifactRootHash, reader.hash());
            UF_TRY(reader.consume(",\"workspace_sqlite_schema_hash\":"));
            UF_TRY_VALUE(workspaceSchemaHash, reader.hash());
            auto encodedWorkspaceHash = std::string{"sha256:"};
            encodedWorkspaceHash += k_workspaceSqliteSchemaHash;
            UF_TRY_VALUE(
                expectedWorkspaceSchemaHash,
                ContentHash::parse(encodedWorkspaceHash)
            );
            if (workspaceSchemaHash != expectedWorkspaceSchemaHash)
            {
                return refuse("release manifest uses an unsupported workspace schema");
            }
            UF_TRY(reader.consume("}"));
            if (!reader.atEnd())
            {
                return refuse("release manifest has trailing bytes");
            }
            return ReleaseManifest{.artifactRootHash = artifactRootHash};
        }

        [[nodiscard]]
        auto readPlainFile(
            std::filesystem::path const& path,
            std::size_t maximumBytes
        ) -> Result<std::vector<std::byte>>
        {
            auto error = std::error_code{};
            auto const status = std::filesystem::symlink_status(path, error);
            if (error)
            {
                return ioFailure("inspect", path, error);
            }
            if (status.type() != std::filesystem::file_type::regular)
            {
                return refuse(std::format("{} must be a plain file", path.string()));
            }
            auto const size = std::filesystem::file_size(path, error);
            if (error)
            {
                return ioFailure("measure", path, error);
            }
            auto const checked = checkedCast<std::size_t>(size);
            if (!checked || *checked > maximumBytes)
            {
                return refuse(std::format("{} exceeds its byte ceiling", path.string()));
            }
            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream)
            {
                return ioFailure("open", path);
            }
            auto text = std::string(*checked, '\0');
            if (!text.empty())
            {
                stream.read(
                    text.data(),
                    static_cast<std::streamsize>(text.size())
                );
            }
            if (!stream || stream.gcount() != static_cast<std::streamsize>(text.size()))
            {
                return ioFailure("read", path);
            }
            auto bytes = std::vector<std::byte>{};
            bytes.reserve(text.size());
            for (auto const value : text)
            {
                bytes.emplace_back(
                    static_cast<std::byte>(static_cast<unsigned char>(value))
                );
            }
            return bytes;
        }

        [[nodiscard]]
        auto asString(std::span<std::byte const> bytes) -> std::string
        {
            auto text = std::string{};
            text.reserve(bytes.size());
            for (auto const value : bytes)
            {
                text.push_back(
                    static_cast<char>(std::to_integer<unsigned char>(value))
                );
            }
            return text;
        }

        [[nodiscard]]
        auto requirePlainDirectory(std::filesystem::path const& path) -> Status
        {
            auto error = std::error_code{};
            auto const status = std::filesystem::symlink_status(path, error);
            if (
                error
                || !std::filesystem::is_directory(status)
                || std::filesystem::is_symlink(status)
            )
            {
                return refuse(std::format("{} must be a plain directory", path.string()));
            }
            return ok();
        }

        [[nodiscard]]
        auto isWithin(
            std::filesystem::path const& path,
            std::filesystem::path const& root
        ) -> bool
        {
            auto component = path.begin();
            for (auto const& expected : root)
            {
                if (component == path.end() || *component != expected)
                {
                    return false;
                }
                ++component;
            }
            return true;
        }

        [[nodiscard]]
        auto verifyHandoffTopLevel(std::filesystem::path const& handoffRoot) -> Status
        {
            UF_TRY(requirePlainDirectory(handoffRoot));
            auto actual = std::set<std::string>{};
            auto error  = std::error_code{};
            for (auto iterator = std::filesystem::directory_iterator{handoffRoot, error};
                 !error && iterator != std::filesystem::directory_iterator{};
                 iterator.increment(error))
            {
                auto const status = iterator->symlink_status(error);
                if (error || std::filesystem::is_symlink(status))
                {
                    return refuse("release handoff contains an unreadable path or link");
                }
                actual.emplace(iterator->path().filename().generic_string());
            }
            if (error)
            {
                return ioFailure("enumerate", handoffRoot, error);
            }
            auto const expected = std::set<std::string>{
                std::string{k_releaseManifestName},
                std::string{k_runtimeDirectoryName},
            };
            if (actual != expected)
            {
                return refuse("release handoff must contain exactly its manifest and RuntimeArtifact");
            }
            return ok();
        }

        class StagingDirectory final
        {
            std::filesystem::path m_path;
            bool                  m_active{true};

        public:
            explicit StagingDirectory(std::filesystem::path path)
                : m_path{std::move(path)}
            {
            }

            // The guard owns one directory for the length of one scope and
            // never hands it on, so neither copying nor moving it is defined.
            StagingDirectory(StagingDirectory const&) = delete;
            StagingDirectory(StagingDirectory&&) = delete;
            auto operator=(StagingDirectory const&) -> StagingDirectory& = delete;
            auto operator=(StagingDirectory&&) -> StagingDirectory& = delete;

            ~StagingDirectory()
            {
                if (m_active)
                {
                    auto ignored = std::error_code{};
                    std::filesystem::remove_all(m_path, ignored);
                }
            }

            auto release() noexcept -> void { m_active = false; }
        };

        [[nodiscard]]
        auto materialize(
            std::filesystem::path const& productionRoot,
            task::RuntimeArtifactHandle const& source,
            std::string_view stagingToken
        ) -> Result<std::filesystem::path>
        {
            auto destination = productionRoot / source.rootHash().hex();
            auto error       = std::error_code{};
            auto status      = std::filesystem::symlink_status(destination, error);
            if (!error && std::filesystem::exists(status))
            {
                if (
                    !std::filesystem::is_directory(status)
                    || std::filesystem::is_symlink(status)
                )
                {
                    return refuse("production RuntimeArtifact object path is not a directory");
                }
                return destination;
            }
            if (error && error != std::errc::no_such_file_or_directory)
            {
                return ioFailure("inspect", destination, error);
            }

            // The staging root belongs to the production layout, which
            // OperatorCoordinator::open creates and verifies; creating it here
            // as well would leave two owners of one directory.
            auto const stagingRoot = productionRoot / k_stagingDirectoryName;
            UF_TRY(requirePlainDirectory(stagingRoot));
            auto const staging = stagingRoot / std::string{stagingToken};
            if (!std::filesystem::create_directory(staging, error) || error)
            {
                return refuse("production RuntimeArtifact staging token is already in use");
            }
            auto cleanup = StagingDirectory{staging};

            // Staging writes go through the same confinement the loader reads
            // through. The leaf name is 32 CSPRNG bytes and so cannot be
            // pre-created, but the directories underneath it are ours to make,
            // and a link planted at one of those between the create and the
            // write would otherwise redirect a deployment write out of the
            // production root.
            //
            // The confined root is scoped to the writes: its handles are held
            // without delete sharing, which is what stops the prefix moving --
            // and would equally stop the rename below.
            {
                UF_TRY_VALUE(confinedStaging, task_platform::ConfinedRoot::open(staging));
                UF_TRY(confinedStaging.writeNewFile(
                    task::k_runtimeArtifactManifestFileName,
                    source.manifestBytes()
                ));
                UF_TRY(confinedStaging.writeNewFile(
                    task::k_runtimeModelFileName,
                    source.modelBytes()
                ));
                for (auto const& relative : source.assetPaths())
                {
                    UF_TRY_VALUE(bytes, source.fileBytes(relative));
                    UF_TRY(confinedStaging.writeNewFile(relative, bytes));
                }
            }
            UF_TRY(task::loadRuntimeArtifact(staging, source.rootHash()));

            std::filesystem::rename(staging, destination, error);
            if (error)
            {
                auto destinationStatus = std::filesystem::symlink_status(destination, error);
                if (
                    error
                    || !std::filesystem::is_directory(destinationStatus)
                    || std::filesystem::is_symlink(destinationStatus)
                )
                {
                    return ioFailure("publish", destination, error);
                }
            }
            else
            {
                cleanup.release();
            }
            return destination;
        }
    }

    auto readRuntimeRelease(
        std::filesystem::path const& productionRoot,
        std::filesystem::path const& handoffRoot,
        ContentHash const& expectedReleaseManifestHash
    ) -> Result<VerifiedRuntimeRelease>
    {
        UF_TRY(requirePlainDirectory(productionRoot));
        UF_TRY(verifyHandoffTopLevel(handoffRoot));
        auto error = std::error_code{};
        auto const canonicalProductionRoot = std::filesystem::canonical(
            productionRoot,
            error
        );
        if (error)
        {
            return ioFailure("canonicalize", productionRoot, error);
        }
        auto const canonicalHandoffRoot = std::filesystem::canonical(
            handoffRoot,
            error
        );
        if (error)
        {
            return ioFailure("canonicalize", handoffRoot, error);
        }
        if (
            isWithin(canonicalHandoffRoot, canonicalProductionRoot)
            || isWithin(canonicalProductionRoot, canonicalHandoffRoot)
        )
        {
            return refuse(
                "release handoff and production RuntimeArtifact roots must be disjoint"
            );
        }
        UF_TRY_VALUE(
            releaseBytes,
            readPlainFile(
                handoffRoot / k_releaseManifestName,
                k_maximumReleaseManifestBytes
            )
        );
        UF_TRY_VALUE(releaseHash, sha256(releaseBytes));
        if (releaseHash != expectedReleaseManifestHash)
        {
            return refuse("release manifest does not match trusted deployment metadata");
        }
        UF_TRY_VALUE(release, parseReleaseManifest(asString(releaseBytes)));
        UF_TRY_VALUE(
            source,
            task::loadRuntimeArtifact(
                handoffRoot / k_runtimeDirectoryName,
                release.artifactRootHash
            )
        );
        return VerifiedRuntimeRelease{
            .handoffArtifact     = std::move(source),
            .releaseManifestHash = releaseHash,
            .artifactRootHash    = release.artifactRootHash,
        };
    }

    auto publishRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        VerifiedRuntimeRelease const& release,
        std::string_view stagingToken
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>
    {
        UF_TRY_VALUE(
            productionPath,
            materialize(productionRoot, release.handoffArtifact, stagingToken)
        );
        UF_TRY_VALUE(
            installed,
            task::loadRuntimeArtifact(productionPath, release.artifactRootHash)
        );
        return std::make_shared<task::RuntimeArtifactHandle const>(
            std::move(installed)
        );
    }

    auto openProductionRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        ContentHash const& artifactRootHash
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>
    {
        UF_TRY_VALUE(
            artifact,
            task::loadRuntimeArtifact(
                productionRoot / artifactRootHash.hex(),
                artifactRootHash
            )
        );
        return std::make_shared<task::RuntimeArtifactHandle const>(
            std::move(artifact)
        );
    }
}
