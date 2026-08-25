#include "runtime-installation.hpp"

#include <task/platform/confined-file.hpp>

#include <domain/error.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace uf::operator_runtime::detail
{
    namespace
    {
        // The staging leaf the genesis artifact lands through. It is a fixed
        // short name rather than a CSPRNG token because only one Coordinator
        // may hold a root at a time, so there is no second publisher to
        // collide with -- and because a name shorter than the 64-character
        // destination cannot be what pushes a path past a platform limit the
        // destination itself fits under.
        constexpr auto k_genesisStagingName = std::string_view{"genesis"};

        constexpr auto k_publishRetryDelays = std::array{
            std::chrono::milliseconds{5},
            std::chrono::milliseconds{10},
            std::chrono::milliseconds{20},
            std::chrono::milliseconds{40},
            std::chrono::milliseconds{80},
            std::chrono::milliseconds{160},
            std::chrono::milliseconds{320},
        };

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

        // Whether the content-addressed destination already holds a directory.
        // A path that exists and is not a plain one is refused rather than
        // reported absent: a link there would redirect every later read out of
        // the production root.
        [[nodiscard]]
        auto alreadyPublished(std::filesystem::path const& destination)
            -> Result<bool>
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(destination, error);
            if (!error && std::filesystem::exists(status))
            {
                if (
                    !std::filesystem::is_directory(status)
                    || std::filesystem::is_symlink(status)
                )
                {
                    return refuse("production RuntimeArtifact object path is not a directory");
                }
                return true;
            }
            if (error && error != std::errc::no_such_file_or_directory)
            {
                return ioFailure("inspect", destination, error);
            }
            return false;
        }

        // Publication itself: one rename, and true when it was OUR staging
        // directory that landed. False means the destination was already
        // there -- a second publisher of identical content won the race -- and
        // the caller must let its staging directory be removed rather than
        // released.
        [[nodiscard]]
        auto renameIntoPlace(
            std::filesystem::path const& staging,
            std::filesystem::path const& destination
        ) -> Result<bool>
        {
            auto error = std::error_code{};
            for (auto attempt = std::size_t{}; ; ++attempt)
            {
                error.clear();
                std::filesystem::rename(staging, destination, error);
                if (!error)
                {
                    return true;
                }

                // A second publisher of identical content may have won the
                // destination between the first inspection and this rename.
                // Keep the rename error separate: alreadyPublished writes its
                // own error_code and must not erase the failure we report.
                auto const publishError = error;
                UF_TRY_VALUE(published, alreadyPublished(destination));
                if (published)
                {
                    return false;
                }
                if (attempt == k_publishRetryDelays.size())
                {
                    return ioFailure("publish", destination, publishError);
                }

                // Windows virus scanners and indexing filters can retain a
                // just-closed directory handle briefly. Publication remains
                // one atomic rename; only that boundary is retried, for a
                // fixed total well below one second.
                std::this_thread::sleep_for(k_publishRetryDelays[attempt]);
            }
        }

        [[nodiscard]]
        auto materialize(
            std::filesystem::path const& productionRoot,
            task::RuntimeArtifactHandle const& source,
            std::string_view stagingToken
        ) -> Result<std::filesystem::path>
        {
            auto destination = productionRoot / source.rootHash().hex();
            UF_TRY_VALUE(published, alreadyPublished(destination));
            if (published)
            {
                return destination;
            }
            auto error = std::error_code{};

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
            UF_TRY_VALUE(renamed, renameIntoPlace(staging, destination));
            if (renamed)
            {
                cleanup.release();
            }
            return destination;
        }
    }

    auto readRuntimeArtifactSource(
        std::filesystem::path const& productionRoot,
        std::filesystem::path const& artifactDirectory,
        ContentHash const& artifactRootHash
    ) -> Result<task::RuntimeArtifactHandle>
    {
        UF_TRY(requirePlainDirectory(productionRoot));
        UF_TRY(requirePlainDirectory(artifactDirectory));
        auto error = std::error_code{};
        auto const canonicalProductionRoot = std::filesystem::canonical(
            productionRoot,
            error
        );
        if (error)
        {
            return ioFailure("canonicalize", productionRoot, error);
        }
        auto const canonicalArtifactDirectory = std::filesystem::canonical(
            artifactDirectory,
            error
        );
        if (error)
        {
            return ioFailure("canonicalize", artifactDirectory, error);
        }

        // The production root is content-addressed storage this installer
        // owns; a source directory nested in it, or a production root nested
        // in the source, would make the copy read and write one tree.
        if (
            isWithin(canonicalArtifactDirectory, canonicalProductionRoot)
            || isWithin(canonicalProductionRoot, canonicalArtifactDirectory)
        )
        {
            return refuse(
                "source RuntimeArtifact and production RuntimeArtifact roots must be disjoint"
            );
        }
        return task::loadRuntimeArtifact(artifactDirectory, artifactRootHash);
    }

    auto publishRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        task::RuntimeArtifactHandle const& source,
        std::string_view stagingToken
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>
    {
        UF_TRY_VALUE(
            productionPath,
            materialize(productionRoot, source, stagingToken)
        );
        UF_TRY_VALUE(
            installed,
            task::loadRuntimeArtifact(productionPath, source.rootHash())
        );
        return std::make_shared<task::RuntimeArtifactHandle const>(
            std::move(installed)
        );
    }

    auto ensureGenesisRuntimeArtifact(
        std::filesystem::path const& productionRoot
    ) -> Result<ContentHash>
    {
        UF_TRY(requirePlainDirectory(productionRoot));
        UF_TRY_VALUE(rootHash, task::genesisArtifactRootHash());
        auto const destination = productionRoot / rootHash.hex();
        UF_TRY_VALUE(published, alreadyPublished(destination));
        if (!published)
        {
            UF_TRY_VALUE(manifest, task::genesisRuntimeArtifactManifestJcs());
            auto const stagingRoot = productionRoot / k_stagingDirectoryName;
            UF_TRY(requirePlainDirectory(stagingRoot));
            auto const staging = stagingRoot / std::string{k_genesisStagingName};

            // An attempt that died between the create and the rename left this
            // directory behind. Reclamation already sweeps every child of
            // .staging wholesale, so removing this one by name takes nothing a
            // later sweep would have kept.
            auto error = std::error_code{};
            std::filesystem::remove_all(staging, error);
            if (error)
            {
                return ioFailure("clear", staging, error);
            }
            error.clear();
            if (!std::filesystem::create_directory(staging, error) || error)
            {
                return ioFailure("create", staging, error);
            }
            auto cleanup = StagingDirectory{staging};

            // The same confinement the installer stages through, for the same
            // reason: the directories above the leaf are ours to make, and a
            // link planted at one of them between the create and the write
            // would redirect the write out of the production root.
            {
                UF_TRY_VALUE(
                    confinedStaging,
                    task_platform::ConfinedRoot::open(staging)
                );
                UF_TRY(confinedStaging.writeNewFile(
                    task::k_runtimeArtifactManifestFileName,
                    std::as_bytes(std::span{manifest})
                ));
                UF_TRY(confinedStaging.writeNewFile(
                    task::k_runtimeModelFileName,
                    std::as_bytes(std::span{task::k_genesisRuntimeModelToml})
                ));
            }
            UF_TRY(task::loadRuntimeArtifact(staging, rootHash));
            UF_TRY_VALUE(renamed, renameIntoPlace(staging, destination));
            if (renamed)
            {
                cleanup.release();
            }
        }

        // Verified on every open, whether it was written just now or has been
        // there since the root was created. H_genesis is a compile-time
        // constant, so exactly one byte sequence is legal at this address and
        // anything else is named rather than accepted.
        UF_TRY(task::loadRuntimeArtifact(destination, rootHash));
        return rootHash;
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
