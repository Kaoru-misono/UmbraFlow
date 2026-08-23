#include "release-bundle.hpp"

#include "platform/process-run.hpp"

#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        constexpr auto k_releaseManifestSchema = std::string_view{
            "umbraflow-release/v1"
        };

        // Scratch, owned by this file and by nothing else. It is emptied at the
        // start of every upgrade rather than refused when present: a staged
        // bundle is reconstructible from an immutable release, so a leftover
        // staging tree carries no information an operator could lose, and
        // refusing on it would leave a failed download blocking every retry.
        constexpr auto k_stagingDirectory = std::string_view{
            "release-staging"
        };

        // The one release selector that is not a release name. A project
        // states it to say it follows the newest release rather than a fixed
        // one; every other value is a name and pins.
        constexpr auto k_latestRelease = std::string_view{"latest"};

        constexpr auto k_maximumManifestBytes = std::uintmax_t{1U << 20U};
        constexpr auto k_maximumArtifactBytes = std::uintmax_t{128U << 20U};
        constexpr auto k_maximumBundleBytes = std::uintmax_t{1U} << 30U;
        constexpr auto k_maximumArtifactCount = std::size_t{128};

        // The artifact whose bytes answer for a release afterwards. `project`
        // is singled out because the report an upgrade prints has to come from
        // the binary that was just installed.
        constexpr auto k_projectArtifactName = std::string_view{"project"};

        struct KitConfig final
        {
            std::string host{};
            std::string manifest{};
            std::string release{};
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

        // A release manifest, as read.
        //
        // contractVersions is parsed and shape-checked and then deliberately
        // NOT consulted. An upgrade does not gate on which project contract a
        // release targets: the binary it installs is the only thing that can
        // tell an author whether their declaration matches it, so refusing here
        // would leave them editing blind and re-downloading on every attempt.
        // The member stays because it is part of the document's shape and a
        // reader that silently ignored an unparsed member would accept a
        // malformed release.
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

        // One artifact, off the network and onto disk.
        //
        // The retry flags are not an optimisation. A single TLS reset failed
        // the whole upgrade twice against the same GitHub asset, and the same
        // URL with these flags succeeded first try; without them one blip
        // costs the operator every artifact already downloaded. --retry-all-errors
        // is needed beside --retry because a reset mid-transfer is not one of
        // the transient conditions --retry alone covers.
        [[nodiscard]]
        auto downloadFile(
            std::string_view url,
            std::filesystem::path const& target,
            std::uintmax_t maximumBytes
        ) -> Status
        {
            auto const commandLine = std::vector<std::string>{
                "curl",
                "--fail",
                "--location",
                "--silent",
                "--show-error",
                "--connect-timeout",
                "15",
                "--max-time",
                "600",
                "--retry",
                "5",
                "--retry-all-errors",
                "--retry-delay",
                "2",
                "--proto",
                "=https,file",
                "--proto-redir",
                "=https",
                "--header",
                "Accept:application/octet-stream",
                "--max-filesize",
                std::to_string(maximumBytes),
                "--output",
                target.string(),
                std::string{url},
            };
            UF_TRY_VALUE(status, runProcess(commandLine));
            if (status != 0)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "curl refused release URL \"{}\" with exit code {}",
                        url,
                        status
                    )
                );
            }
            return ok();
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

        // umbraflow-kit.json, judged whole.
        //
        // `release` is required and has no default. A project states whether it
        // follows the newest release or pins one, and "absent means follow
        // latest" would make the member that carries a project's most
        // consequential decision the one member it could forget to write. It is
        // also the whole of a project's record of which framework build it was
        // authored against, which is why no lock file is introduced beside it.
        [[nodiscard]]
        auto parseKitConfig(
            std::filesystem::path const& sourceDirectory
        ) -> Result<KitConfig>
        {
            auto const path = sourceDirectory / k_kitConfigName;
            auto error      = std::error_code{};
            auto const status = std::filesystem::status(path, error);
            if (
                error == std::errc::no_such_file_or_directory
                || status.type() == std::filesystem::file_type::not_found
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "a project needs {} at the root of its source directory "
                        "to upgrade, and \"{}\" holds none",
                        k_kitConfigName,
                        sourceDirectory.string()
                    )
                );
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
            constexpr auto members = std::array<std::string_view, 3>{
                "host",
                "manifest",
                "release",
            };
            if (!hasExactMembers(document, members))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} must carry exactly \"host\", \"manifest\" and "
                        "\"release\"",
                        k_kitConfigName
                    )
                );
            }
            UF_TRY_VALUE(host, stringMember(document, "host", k_kitConfigName));
            UF_TRY_VALUE(
                manifest,
                stringMember(document, "manifest", k_kitConfigName)
            );
            UF_TRY_VALUE(
                release,
                stringMember(document, "release", k_kitConfigName)
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
            if (!isUrlSegment(release))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} release must be \"{}\" or one canonical release "
                        "name",
                        k_kitConfigName,
                        k_latestRelease
                    )
                );
            }
            return KitConfig{
                .host     = std::move(host),
                .manifest = std::move(manifest),
                .release  = std::move(release),
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
        auto addedBundleBytes(
            std::uintmax_t total,
            std::uintmax_t artifactBytes
        ) -> Result<std::uintmax_t>
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
            return total + artifactBytes;
        }

        // The URL the release manifest is fetched from.
        //
        // The two spellings are the two things a project can state. `latest` is
        // a mutable pointer the host maintains, and a release name is an
        // immutable directory; there is no third form and no default, so the
        // selector always came from somewhere an operator can read.
        [[nodiscard]]
        auto manifestUrl(
            KitConfig const& config,
            std::string_view selector
        ) -> std::string
        {
            if (selector == k_latestRelease)
            {
                return (
                    config.host + "/releases/latest/download/" + config.manifest
                );
            }
            return (
                config.host + "/releases/download/" + std::string{selector}
                + "/" + config.manifest
            );
        }

        [[nodiscard]]
        auto confinedChild(
            std::filesystem::path const& sourceRoot,
            std::filesystem::path const& candidate,
            std::string_view role
        ) -> Status
        {
            UF_TRY_VALUE(resolved, resolvedPath(candidate, role));
            if (!isWithinOrEqual(resolved, sourceRoot))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} path leaves the project source tree: \"{}\"",
                        role,
                        candidate.string()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto isDirectory(std::filesystem::path const& path) -> Result<bool>
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(path, error);
            if (
                error == std::errc::no_such_file_or_directory
                || status.type() == std::filesystem::file_type::not_found
            )
            {
                return false;
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect \"{}\": {}",
                        path.string(),
                        error.message()
                    )
                );
            }
            if (!std::filesystem::is_directory(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" exists and is not a directory",
                        path.string()
                    )
                );
            }
            return true;
        }

        // Every umbraflow-bin.* beside the live bundle, sorted, so a refusal
        // that has to name them names them the same way twice running.
        [[nodiscard]]
        auto supersededBundles(
            std::filesystem::path const& sourceDirectory
        ) -> Result<std::vector<std::filesystem::path>>
        {
            auto const prefix = std::string{k_bundleDirectory} + ".";
            auto error        = std::error_code{};
            auto iterator     = std::filesystem::directory_iterator{
                sourceDirectory,
                std::filesystem::directory_options::skip_permission_denied,
                error
            };
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot list the project source directory \"{}\": {}",
                        sourceDirectory.string(),
                        error.message()
                    )
                );
            }
            auto found     = std::vector<std::filesystem::path>{};
            auto const end = std::filesystem::directory_iterator{};
            for (; iterator != end; iterator.increment(error))
            {
                if (error)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        std::format(
                            "cannot list the project source directory \"{}\": {}",
                            sourceDirectory.string(),
                            error.message()
                        )
                    );
                }
                auto const name = iterator->path().filename().string();
                if (!name.starts_with(prefix))
                {
                    continue;
                }
                auto const directory = iterator->is_directory(error);
                if (error || !directory)
                {
                    continue;
                }
                found.emplace_back(iterator->path());
            }
            std::ranges::sort(found);
            return found;
        }

        // The release the bundle already installed at this path carries.
        //
        // It is read out of the pinned manifest rather than derived from
        // anything, because it becomes the name the previous bundle is set
        // aside under and an operator has to recognise it. A bundle carrying no
        // readable manifest is refused by name: nothing this tool installed can
        // be in that state, so there is nothing to guess about.
        [[nodiscard]]
        auto installedRelease(
            std::filesystem::path const& bundleRoot,
            KitConfig const& config
        ) -> Result<std::string>
        {
            auto const path = bundleRoot / config.manifest;
            UF_TRY_VALUE_CONTEXT(
                bytes,
                readFile(path, k_maximumManifestBytes, "installed release manifest"),
                std::format(
                    "\"{}\" holds no readable release manifest, so the bundle "
                    "it holds cannot be named; move it aside and upgrade again",
                    bundleRoot.string()
                )
            );
            UF_TRY_VALUE(document, json::parse(bytes));
            UF_TRY_VALUE(
                release,
                stringMember(document, "release", "installed release manifest")
            );
            if (!isUrlSegment(release))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "the installed release manifest carries a non-canonical "
                    "release name"
                );
            }
            return release;
        }
    }

    auto reconcileBundleDirectories(
        std::filesystem::path const& sourceDirectory
    ) -> Result<std::string>
    {
        auto const bundleRoot = sourceDirectory / k_bundleDirectory;
        UF_TRY_VALUE(live, isDirectory(bundleRoot));
        UF_TRY_VALUE(superseded, supersededBundles(sourceDirectory));
        if (superseded.empty())
        {
            return std::string{};
        }

        if (!live)
        {
            if (superseded.size() != 1U)
            {
                auto names = std::string{};
                for (auto const& path : superseded)
                {
                    names += "\n  " + path.filename().string();
                }
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" holds no installed bundle and more than one "
                        "superseded one, so nothing here can choose between "
                        "them:{}",
                        sourceDirectory.string(),
                        names
                    )
                );
            }
            auto error = std::error_code{};
            std::filesystem::rename(superseded.front(), bundleRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot finish the interrupted upgrade that left "
                        "\"{}\" with no installed bundle: {}",
                        sourceDirectory.string(),
                        error.message()
                    )
                );
            }
            return std::format(
                "project: an upgrade was interrupted between its two renames; "
                "\"{}\" has been put back as \"{}\". Run project upgrade "
                "again.\n",
                superseded.front().filename().string(),
                k_bundleDirectory
            );
        }

        // A superseded bundle that will not delete is normally held by this
        // process's own parent -- the binary that performed the upgrade and is
        // still running out of it. Refusing the command would make every verb
        // fail until that process exits, at the one moment the operator needs
        // them; the leftover is named instead, and the next invocation removes
        // it.
        auto notes = std::string{};
        for (auto const& path : superseded)
        {
            auto error = std::error_code{};
            static_cast<void>(std::filesystem::remove_all(path, error));
            if (error)
            {
                notes += std::format(
                    "project: superseded bundle \"{}\" is still in use and was "
                    "left in place; the next project command removes it.\n",
                    path.filename().string()
                );
            }
        }
        return notes;
    }

    auto upgradeReleaseBundle(ProjectUpgradeSpec const& spec)
        -> Result<InstalledRelease>
    {
        UF_TRY_VALUE(config, parseKitConfig(spec.sourceDirectory));
        auto const selector = spec.releaseOverride.empty()
            ? std::string_view{config.release}
            : std::string_view{spec.releaseOverride};
        if (!isUrlSegment(selector))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "project upgrade --release must be \"{}\" or one canonical "
                    "release name",
                    k_latestRelease
                )
            );
        }

        UF_TRY_VALUE(
            sourceRoot,
            resolvedPath(spec.sourceDirectory, "project source")
        );
        auto const bundleRoot  = spec.sourceDirectory / k_bundleDirectory;
        auto const stagingRoot = (
            spec.sourceDirectory / "work" / k_stagingDirectory
        );
        UF_TRY(confinedChild(sourceRoot, bundleRoot, "release bundle"));
        UF_TRY(confinedChild(sourceRoot, stagingRoot, "release staging"));

        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove_all(stagingRoot, error));
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "cannot clear the release staging path \"{}\": {}",
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

        auto const manifestPath = stagingRoot / config.manifest;
        UF_TRY(downloadFile(
            manifestUrl(config, selector),
            manifestPath,
            k_maximumManifestBytes
        ));
        UF_TRY_VALUE(
            manifestBytes,
            readFile(manifestPath, k_maximumManifestBytes, "release manifest")
        );
        UF_TRY_VALUE(manifest, parseReleaseManifest(manifestBytes));

        // A pinned selector is a claim about which bytes are being installed,
        // so the manifest that arrived has to agree with it. Without this a
        // misconfigured host answers every pin with the same release and the
        // pin silently stops pinning.
        if (selector != k_latestRelease && manifest.release != selector)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "release {} was requested and the host served the manifest "
                    "of release {}",
                    selector,
                    manifest.release
                )
            );
        }

        UF_TRY_VALUE(artifacts, selectedArtifacts(manifest));
        auto bundleBytes     = static_cast<std::uintmax_t>(manifestBytes.size());
        auto projectArtifact = std::filesystem::path{};
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
            UF_TRY_VALUE(artifactBytes, verifyArtifact(stagingRoot, artifact));
            UF_TRY_VALUE(total, addedBundleBytes(bundleBytes, artifactBytes));
            bundleBytes = total;
            if (artifact.name == k_projectArtifactName)
            {
                projectArtifact = artifact.path;
            }
        }

        // The swap. Two renames and no delete, because a directory holding a
        // running executable can be renamed on this platform and cannot be
        // removed -- and the process running this code is normally the one
        // inside the bundle being replaced. A crash between them leaves no live
        // bundle and exactly one set aside, which reconcileBundleDirectories
        // recognises by that shape and finishes.
        UF_TRY_VALUE(live, isDirectory(bundleRoot));
        if (live)
        {
            UF_TRY_VALUE(previous, installedRelease(bundleRoot, config));
            auto const asideRoot = (
                spec.sourceDirectory
                / (std::string{k_bundleDirectory} + "." + previous)
            );
            error = std::error_code{};
            static_cast<void>(std::filesystem::remove_all(asideRoot, error));
            error = std::error_code{};
            std::filesystem::rename(bundleRoot, asideRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot set the installed release {} aside as \"{}\": "
                        "{}",
                        previous,
                        asideRoot.string(),
                        error.message()
                    )
                );
            }
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

        return InstalledRelease{
            .release           = manifest.release,
            .bundleDirectory   = bundleRoot,
            .projectExecutable = bundleRoot / projectArtifact,
        };
    }
}
