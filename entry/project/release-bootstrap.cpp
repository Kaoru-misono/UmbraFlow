#include "release-bootstrap.hpp"

#include "platform/curl-download.hpp"

#include <project/project-kit.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/value.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::project_entry
{
    namespace
    {
        constexpr auto k_kitConfigName = std::string_view{
            "umbraflow-kit.json"
        };
        constexpr auto k_releaseManifestSchema = std::string_view{
            "umbraflow-release/v1"
        };
        constexpr auto k_bundleDirectory = std::string_view{
            "umbraflow-bin"
        };
        constexpr auto k_stagingDirectory = std::string_view{
            "project-init-release"
        };
        constexpr auto k_maximumManifestBytes = std::uintmax_t{1U << 20U};
        constexpr auto k_maximumArtifactBytes = std::uintmax_t{128U << 20U};
        constexpr auto k_maximumBundleBytes = std::uintmax_t{1U} << 30U;
        constexpr auto k_maximumArtifactCount = std::size_t{128};

        struct KitConfig final
        {
            std::string host{};
            std::string manifest{};
        };

        struct ReleaseArtifact final
        {
            std::string           name{};
            std::string           platform{};
            std::string           arch{};
            std::filesystem::path path{};
            std::string           asset{};
            ContentHash           digest;
        };

        struct ReleaseManifest final
        {
            std::string                  release{};
            std::vector<std::string>     contractVersions{};
            std::vector<ReleaseArtifact> artifacts{};
        };

        [[nodiscard]]
        auto readFile(
            std::filesystem::path const& path,
            std::uintmax_t maximumBytes,
            std::string_view role
        ) -> Result<std::string>
        {
            auto error      = std::error_code{};
            auto const size = std::filesystem::file_size(path, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect {} \"{}\": {}",
                        role,
                        path.string(),
                        error.message()
                    )
                );
            }
            if (size > maximumBytes)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} \"{}\" is too large: {} bytes exceeds {}",
                        role,
                        path.string(),
                        size,
                        maximumBytes
                    )
                );
            }
            auto const stringSize = checkedCast<std::size_t>(size);
            auto const streamSize = checkedCast<std::streamsize>(size);
            if (!stringSize || !streamSize)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} \"{}\" cannot be represented in memory",
                        role,
                        path.string()
                    )
                );
            }
            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot open {} \"{}\"", role, path.string())
                );
            }
            auto bytes = std::string(*stringSize, '\0');
            stream.read(bytes.data(), *streamSize);
            if (!stream || stream.gcount() != *streamSize)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot read {} \"{}\"", role, path.string())
                );
            }
            return bytes;
        }

        [[nodiscard]]
        auto stringMember(
            json::Value const& object,
            std::string_view name,
            std::string_view role
        ) -> Result<std::string>
        {
            auto const* const p_value = object.find(name);
            if (
                p_value == nullptr
                || p_value->kind() != json::ValueKind::String
                || p_value->string().empty()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} must carry a non-empty \"{}\"", role, name)
                );
            }
            return std::string{p_value->string()};
        }

        template <std::size_t Size>
        [[nodiscard]]
        auto hasExactMembers(
            json::Value const& object,
            std::array<std::string_view, Size> const& names
        ) -> bool
        {
            if (
                object.kind() != json::ValueKind::Object
                || object.members().size() != names.size()
            )
            {
                return false;
            }
            return std::ranges::all_of(
                names,
                [&object](std::string_view name)
                {
                    return object.find(name) != nullptr;
                }
            );
        }

        [[nodiscard]]
        auto resolvedPath(
            std::filesystem::path const& path,
            std::string_view role
        ) -> Result<std::filesystem::path>
        {
            auto error          = std::error_code{};
            auto const absolute = std::filesystem::absolute(path, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot resolve {} path \"{}\": {}",
                        role,
                        path.string(),
                        error.message()
                    )
                );
            }
            error                = std::error_code{};
            auto const canonical = std::filesystem::weakly_canonical(
                absolute,
                error
            );
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot canonicalize {} path \"{}\": {}",
                        role,
                        path.string(),
                        error.message()
                    )
                );
            }
            return canonical;
        }

        [[nodiscard]]
        auto isWithinOrEqual(
            std::filesystem::path const& candidate,
            std::filesystem::path const& root
        ) -> bool
        {
            auto candidatePart = candidate.begin();
            for (auto const& rootPart : root)
            {
                if (
                    candidatePart == candidate.end()
                    || *candidatePart != rootPart
                )
                {
                    return false;
                }
                ++candidatePart;
            }
            return true;
        }

        [[nodiscard]]
        auto isUrlSegment(std::string_view value) -> bool
        {
            return (
                !value.empty()
                && value != "."
                && value != ".."
                && std::ranges::all_of(
                    value,
                    [](char character)
                    {
                        auto const byte = static_cast<unsigned char>(character);
                        return (
                            std::isalnum(byte) != 0
                            || character == '.'
                            || character == '_'
                            || character == '-'
                        );
                    }
                )
            );
        }

        [[nodiscard]]
        auto canonicalArtifactPath(std::string_view value)
            -> Result<std::filesystem::path>
        {
            if (
                value.empty()
                || value.size() > 512U
                || value.starts_with('/')
                || value.ends_with('/')
                || value.contains('\\')
                || value.contains(':')
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release artifact path is not canonical: \"{}\"",
                        value
                    )
                );
            }
            auto remaining = value;
            while (!remaining.empty())
            {
                auto const separator = remaining.find('/');
                auto const component = remaining.substr(0, separator);
                if (component.empty() || component == "." || component == "..")
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "release artifact path is not canonical: \"{}\"",
                            value
                        )
                    );
                }
                if (separator == std::string_view::npos)
                    break;
                remaining.remove_prefix(separator + 1U);
            }
            return std::filesystem::path{value};
        }

        [[nodiscard]]
        auto parseKitConfig(
            std::filesystem::path const& sourceDirectory
        ) -> Result<std::optional<KitConfig>>
        {
            auto const path = sourceDirectory / k_kitConfigName;
            auto error      = std::error_code{};
            auto const status = std::filesystem::status(path, error);
            if (
                error == std::errc::no_such_file_or_directory
                || status.type() == std::filesystem::file_type::not_found
            )
            {
                return std::optional<KitConfig>{};
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect {} at \"{}\": {}",
                        k_kitConfigName,
                        path.string(),
                        error.message()
                    )
                );
            }
            if (!std::filesystem::is_regular_file(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} is not a regular file", k_kitConfigName)
                );
            }
            UF_TRY_VALUE(bytes, readFile(path, k_maximumManifestBytes, "kit config"));
            UF_TRY_VALUE(document, json::parse(bytes));
            constexpr auto members = std::array<std::string_view, 2>{
                "host",
                "manifest",
            };
            if (!hasExactMembers(document, members))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} has the wrong member set", k_kitConfigName)
                );
            }
            UF_TRY_VALUE(host, stringMember(document, "host", k_kitConfigName));
            UF_TRY_VALUE(
                manifest,
                stringMember(document, "manifest", k_kitConfigName)
            );
            if (
                !host.starts_with("https://")
                && !host.starts_with("file://")
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} host must be an https:// or file:// URL",
                        k_kitConfigName
                    )
                );
            }
            if (host.ends_with('/'))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} host must not end with '/'",
                        k_kitConfigName
                    )
                );
            }
            if (!isUrlSegment(manifest))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} manifest must be one canonical asset name",
                        k_kitConfigName
                    )
                );
            }
            return std::optional<KitConfig>{
                std::in_place,
                KitConfig{
                    .host     = std::move(host),
                    .manifest = std::move(manifest),
                }
            };
        }

        [[nodiscard]]
        auto parseReleaseManifest(std::string_view bytes)
            -> Result<ReleaseManifest>
        {
            UF_TRY_CONTEXT(
                json::requireExactCanonical(bytes),
                "reading the release manifest"
            );
            UF_TRY_VALUE(document, json::parse(bytes));
            constexpr auto rootMembers = std::array<std::string_view, 4>{
                "schema",
                "release",
                "contract_versions",
                "artifacts",
            };
            if (!hasExactMembers(document, rootMembers))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "release manifest has the wrong top-level member set"
                );
            }
            UF_TRY_VALUE(schema, stringMember(document, "schema", "release manifest"));
            if (schema != k_releaseManifestSchema)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release manifest schema is not {}",
                        k_releaseManifestSchema
                    )
                );
            }
            UF_TRY_VALUE(
                release,
                stringMember(document, "release", "release manifest")
            );
            if (!isUrlSegment(release))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "release manifest carries a non-canonical release name"
                );
            }

            auto const* const p_versions = document.find("contract_versions");
            auto const* const p_artifacts = document.find("artifacts");
            if (
                p_versions == nullptr
                || p_versions->kind() != json::ValueKind::Array
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "release manifest contract_versions must be a string array"
                );
            }
            auto versions = std::vector<std::string>{};
            versions.reserve(p_versions->items().size());
            for (auto const& value : p_versions->items())
            {
                if (
                    value.kind() != json::ValueKind::String
                    || value.string().empty()
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "release manifest contract_versions must be a string array"
                    );
                }
                versions.emplace_back(value.string());
            }
            if (
                !std::ranges::contains(
                    versions,
                    project::k_projectContractVersion
                )
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release does not carry required contract {}",
                        project::k_projectContractVersion
                    )
                );
            }
            if (
                p_artifacts == nullptr
                || p_artifacts->kind() != json::ValueKind::Array
                || p_artifacts->items().empty()
                || p_artifacts->items().size() > k_maximumArtifactCount
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "release manifest must carry 1..128 artifacts"
                );
            }

            constexpr auto artifactMembers = std::array<std::string_view, 6>{
                "name",
                "platform",
                "arch",
                "path",
                "asset",
                "sha256",
            };
            auto artifacts = std::vector<ReleaseArtifact>{};
            artifacts.reserve(p_artifacts->items().size());
            auto identities = std::set<std::string>{};
            for (auto const& row : p_artifacts->items())
            {
                if (!hasExactMembers(row, artifactMembers))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "release artifact has the wrong member set"
                    );
                }
                UF_TRY_VALUE(name, stringMember(row, "name", "release artifact"));
                UF_TRY_VALUE(
                    platform,
                    stringMember(row, "platform", "release artifact")
                );
                UF_TRY_VALUE(arch, stringMember(row, "arch", "release artifact"));
                UF_TRY_VALUE(path, stringMember(row, "path", "release artifact"));
                UF_TRY_VALUE(asset, stringMember(row, "asset", "release artifact"));
                UF_TRY_VALUE(digest, stringMember(row, "sha256", "release artifact"));
                UF_TRY_VALUE(canonicalPath, canonicalArtifactPath(path));
                if (!isUrlSegment(asset))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "release artifact {} has a non-canonical asset name",
                            name
                        )
                    );
                }
                UF_TRY_VALUE(
                    contentHash,
                    ContentHash::parse("sha256:" + digest)
                );
                auto const identity = platform + "\n" + arch + "\n" + path;
                if (!identities.emplace(identity).second)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "release artifact path appears more than once: {}",
                            path
                        )
                    );
                }
                artifacts.emplace_back(ReleaseArtifact{
                    .name     = std::move(name),
                    .platform = std::move(platform),
                    .arch     = std::move(arch),
                    .path     = std::move(canonicalPath),
                    .asset    = std::move(asset),
                    .digest   = contentHash,
                });
            }
            return ReleaseManifest{
                .release          = std::move(release),
                .contractVersions = std::move(versions),
                .artifacts        = std::move(artifacts),
            };
        }

        [[nodiscard]]
        constexpr auto hostPlatform() noexcept -> std::string_view
        {
#if defined(_WIN32)
            return "windows";
#elif defined(__APPLE__)
            return "macos";
#elif defined(__linux__)
            return "linux";
#else
#error Unsupported Project Kit release platform
#endif
        }

        [[nodiscard]]
        constexpr auto hostArch() noexcept -> std::string_view
        {
#if defined(_M_X64) || defined(__x86_64__)
            return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
            return "arm64";
#else
#error Unsupported Project Kit release architecture
#endif
        }

        [[nodiscard]]
        auto selectedArtifacts(ReleaseManifest const& manifest)
            -> Result<std::vector<ReleaseArtifact>>
        {
            auto selected = std::vector<ReleaseArtifact>{};
            for (auto const& artifact : manifest.artifacts)
            {
                if (
                    artifact.platform == hostPlatform()
                    && artifact.arch == hostArch()
                )
                {
                    selected.emplace_back(artifact);
                }
            }
            constexpr auto required = std::array<std::string_view, 3>{
                "project",
                "umbra-flow",
                "umbra-flow-conformance",
            };
            for (auto const name : required)
            {
                auto const count = std::ranges::count(
                    selected,
                    name,
                    &ReleaseArtifact::name
                );
                if (count != 1)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "release must carry exactly one {} artifact for {}/{}",
                            name,
                            hostPlatform(),
                            hostArch()
                        )
                    );
                }
            }
            return selected;
        }

        [[nodiscard]]
        auto verifyArtifact(
            std::filesystem::path const& bundleRoot,
            ReleaseArtifact const& artifact
        ) -> Result<std::uintmax_t>
        {
            auto const path = bundleRoot / artifact.path;
            UF_TRY_VALUE(resolvedRoot, resolvedPath(bundleRoot, "release bundle"));
            UF_TRY_VALUE(resolvedArtifact, resolvedPath(path, "release artifact"));
            if (!isWithinOrEqual(resolvedArtifact, resolvedRoot))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release artifact {} leaves the bundle",
                        artifact.name
                    )
                );
            }
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(path, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect release artifact {}: {}",
                        artifact.name,
                        error.message()
                    )
                );
            }
            if (!std::filesystem::is_regular_file(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release artifact {} is not a regular file",
                        artifact.name
                    )
                );
            }
            UF_TRY_VALUE(bytes, readFile(path, k_maximumArtifactBytes, "release artifact"));
            UF_TRY_VALUE(actual, sha256(std::as_bytes(std::span{bytes})));
            if (actual != artifact.digest)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release artifact {} has sha256 {}, not {}",
                        artifact.name,
                        actual.hex(),
                        artifact.digest.hex()
                    )
                );
            }
            return static_cast<std::uintmax_t>(bytes.size());
        }

        [[nodiscard]]
        auto addBundleBytes(
            std::uintmax_t& total,
            std::uintmax_t artifactBytes
        ) -> Status
        {
            if (
                total > k_maximumBundleBytes
                || artifactBytes > k_maximumBundleBytes - total
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release bundle exceeds {} bytes",
                        k_maximumBundleBytes
                    )
                );
            }
            total += artifactBytes;
            return ok();
        }

        [[nodiscard]]
        auto verifyBundle(
            std::filesystem::path const& bundleRoot,
            KitConfig const& config
        ) -> Status
        {
            auto const manifestPath = bundleRoot / config.manifest;
            auto error              = std::error_code{};
            auto const status = std::filesystem::symlink_status(
                manifestPath,
                error
            );
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect cached release manifest: {}",
                        error.message()
                    )
                );
            }
            if (!std::filesystem::is_regular_file(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "cached release manifest is not a regular file"
                );
            }
            UF_TRY_VALUE(
                manifestBytes,
                readFile(
                    manifestPath,
                    k_maximumManifestBytes,
                    "release manifest"
                )
            );
            UF_TRY_VALUE(manifest, parseReleaseManifest(manifestBytes));
            UF_TRY_VALUE(artifacts, selectedArtifacts(manifest));
            auto bundleBytes = static_cast<std::uintmax_t>(manifestBytes.size());
            for (auto const& artifact : artifacts)
            {
                if (artifact.path == std::filesystem::path{config.manifest})
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "release artifact path collides with its manifest"
                    );
                }
                UF_TRY_VALUE(
                    artifactBytes,
                    verifyArtifact(bundleRoot, artifact)
                );
                UF_TRY(addBundleBytes(bundleBytes, artifactBytes));
            }
            return ok();
        }

        [[nodiscard]]
        auto installBundle(
            std::filesystem::path const& sourceDirectory,
            KitConfig const& config
        ) -> Status
        {
            auto const bundleRoot = sourceDirectory / k_bundleDirectory;
            UF_TRY_VALUE(sourceRoot, resolvedPath(sourceDirectory, "project source"));
            UF_TRY_VALUE(resolvedBundle, resolvedPath(bundleRoot, "release bundle"));
            if (!isWithinOrEqual(resolvedBundle, sourceRoot))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "release bundle path leaves the project source tree"
                );
            }
            auto error            = std::error_code{};
            auto const bundleStatus = std::filesystem::symlink_status(
                bundleRoot,
                error
            );
            if (
                !error
                && bundleStatus.type() != std::filesystem::file_type::not_found
            )
            {
                if (!std::filesystem::is_directory(bundleStatus))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "release bundle path is not a directory: \"{}\"",
                            bundleRoot.string()
                        )
                    );
                }
                return verifyBundle(bundleRoot, config);
            }
            if (
                error
                && error != std::errc::no_such_file_or_directory
            )
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect release bundle \"{}\": {}",
                        bundleRoot.string(),
                        error.message()
                    )
                );
            }

            auto const stagingRoot = (
                sourceDirectory / "work" / k_stagingDirectory
            );
            UF_TRY_VALUE(
                resolvedStaging,
                resolvedPath(stagingRoot, "release staging")
            );
            if (!isWithinOrEqual(resolvedStaging, sourceRoot))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "release staging path leaves the project source tree"
                );
            }
            error = std::error_code{};
            auto const stagingStatus = std::filesystem::symlink_status(
                stagingRoot,
                error
            );
            if (
                !error
                && stagingStatus.type() != std::filesystem::file_type::not_found
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "release staging path already exists: \"{}\"",
                        stagingRoot.string()
                    )
                );
            }
            if (
                error
                && error != std::errc::no_such_file_or_directory
            )
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect release staging path \"{}\": {}",
                        stagingRoot.string(),
                        error.message()
                    )
                );
            }

            error = std::error_code{};
            std::filesystem::create_directories(stagingRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create release staging path \"{}\": {}",
                        stagingRoot.string(),
                        error.message()
                    )
                );
            }
            auto cleanup = scopeExit([&stagingRoot]() noexcept
            {
                auto cleanupError = std::error_code{};
                static_cast<void>(
                    std::filesystem::remove_all(stagingRoot, cleanupError)
                );
            });

            auto const manifestUrl = (
                config.host + "/releases/latest/download/" + config.manifest
            );
            auto const manifestPath = stagingRoot / config.manifest;
            UF_TRY(downloadFile(
                manifestUrl,
                manifestPath,
                k_maximumManifestBytes
            ));
            UF_TRY_VALUE(
                manifestBytes,
                readFile(
                    manifestPath,
                    k_maximumManifestBytes,
                    "release manifest"
                )
            );
            UF_TRY_VALUE(manifest, parseReleaseManifest(manifestBytes));
            UF_TRY_VALUE(artifacts, selectedArtifacts(manifest));
            auto bundleBytes = static_cast<std::uintmax_t>(manifestBytes.size());
            for (auto const& artifact : artifacts)
            {
                if (artifact.path == std::filesystem::path{config.manifest})
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "release artifact path collides with its manifest"
                    );
                }
                auto const target = stagingRoot / artifact.path;
                error             = std::error_code{};
                std::filesystem::create_directories(target.parent_path(), error);
                if (error)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        std::format(
                            "cannot create release artifact directory \"{}\": {}",
                            target.parent_path().string(),
                            error.message()
                        )
                    );
                }
                auto const url = (
                    config.host + "/releases/download/" + manifest.release
                    + "/" + artifact.asset
                );
                UF_TRY(downloadFile(url, target, k_maximumArtifactBytes));
                UF_TRY_VALUE(
                    artifactBytes,
                    verifyArtifact(stagingRoot, artifact)
                );
                UF_TRY(addBundleBytes(bundleBytes, artifactBytes));
            }

            error = std::error_code{};
            std::filesystem::rename(stagingRoot, bundleRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot install release bundle at \"{}\": {}",
                        bundleRoot.string(),
                        error.message()
                    )
                );
            }
            cleanup.release();
            return ok();
        }
    }

    auto prepareReleaseBundle(
        std::filesystem::path const& sourceDirectory
    ) -> Status
    {
        UF_TRY_VALUE(config, parseKitConfig(sourceDirectory));
        if (!config)
            return ok();
        return installBundle(sourceDirectory, *config);
    }
}
